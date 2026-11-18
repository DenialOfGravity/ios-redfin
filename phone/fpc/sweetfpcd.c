/*
 * sweetfpcd (ws-fpcd-a): root daemon that owns the stock FPC fingerprint TrustZone app and serves Touch ID requests
 * that the kernel persona SweetMesa forwards over the SweetMesaBridge user client (fpcd_bridge.h).
 * TA/QSEE code: phone/qsee/qsee_lib.c (qseeprobe's, proven on the phone in fingerprint-2026-11-16.md P3/P4/P5).
 *
 *   sweetfpcd                 daemon: TA up, then the bridge loop (WAIT -> dispatch -> REPLY) + the capture thread
 *   sweetfpcd --selftest [S]  no bridge: TA up, sensor init, ids, db id, then one capture if a finger touches within
 *                             S seconds (default 15; else "no finger, skipped"); identify if a finger is enrolled.
 *                             Unloads the TA at the end if this run loaded it. Exit 0 = pass.
 *   options: --fresh (unload a resident TA and load it again), --keep-ta (never unload)
 *
 * Startup: the QSEE lock (qsee_lock, /var/local/fpc/qsee.lock: the only SMC user); kern.sweet_fpc must have bits
 * 0 (reset) and 1 (irq) set, i.e. value 3 (xnu 1190); lookup "fingerprint": resident -> reuse its id (templates kept),
 * else load DIR/fpctzappfingerprint.mdt+.bNN and load_empty_db; then sensor init (g10/0, g10/5), set_gid 0 (on a
 * resident TA a failing set_gid falls back to load_empty_db + set_gid), get_finger_ids.
 * SIGTERM/SIGINT: cancel the operation, stop the capture thread, unload the TA (FPCD_KEEP_TA=1 / --keep-ta: keep).
 * SIGUSR1: wipe all (delete every finger id, debug).  SIGUSR2: one status line.
 *
 * Operations (one at a time; a new Enroll/Match/Detect cancels the running one) run in the capture thread, driven by
 * gpio112 (kern.sweet_fpc_wait in FPCD_WAIT_SLICE_MS slices so Cancel/SIGTERM are seen): the HAL capture sequence
 * g10/2 arm, wait, g10/1, then g10/3 / wait / g10/4 until not 3; g10/5 on a bad image or a cancel.
 *   enrol : good image -> g11/1 enrol; code 0 -> g11/2 end_enrol (fails without a HAT: then the new id comes from
 *           get_finger_ids, flag FPCD_EF_END_UNAUTH); events ENROL_PROGRESS / ENROL_DONE / TEMPLATES
 *   match : good image -> g11/3 identify; status 0 -> match_deliver() (the only producer of a match); else NOMATCH and
 *           the next capture, until a match, Cancel or the op timeout
 *   detect: captures only, for FINGER_DOWN / FINGER_UP events
 * Templates live in TA memory only (no store_template_db); the Apple catacomb commands answer UNSUPPORTED.
 *
 * SAFETY: a match is reported only by match_deliver(), only for a TA identify status 0 with a finger id the TA lists,
 * and only while /var/local/fpc/allow-match exists (absent by default). The kernel gates again (kern.sweet_mesa_match).
 * fpcd_verify_hat() is the step-5 hook for keymaster/HAT verification (not implemented: matches carry no
 * FPCD_MF_HAT_VERIFIED flag; FPCD_REQUIRE_HAT=1 withholds every match until it is).
 *
 * Environment: FPCD_TA_DIR (/var/local/fpc/ta), FPCD_TA_FILE (fpctzappfingerprint), FPCD_TA_NAME (fingerprint),
 * FPCD_OP_TIMEOUT_MS (120000, 0 = none), FPCD_CONSOLE=1 (mirror log lines to /dev/console), FPCD_KEEP_TA=1,
 * FPCD_REQUIRE_HAT=1, FPCD_VERBOSE=1 (every SMC line of qsee_lib), QSEE_NOLOCK / QSEE_LOCK as qsee_lib.
 * Log: stderr (launchd: /var/local/fpc/sweetfpcd.log). Exit codes: 0 ok, 1 usage/setup, 2 TA/sensor, 9 lock held.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include "../video/pmem.h"
#include "../qsee/qsee_lib.h"
#include "fpcd_bridge.h"

#ifndef FPCD_VERSION
#define FPCD_VERSION "dev"
#endif
#define ALLOW_MATCH_PATH	"/var/local/fpc/allow-match"
#define FPCD_WAIT_SLICE_MS	250	/* gpio112 wait slice: Cancel / SIGTERM latency */
#define FPCD_UP_MS		150	/* armed and quiet this long after a touch = finger up */
#define FPCD_CAPTURE_WAIT_MS	3000	/* wait after g10/3 (arm capture) */
#define FPCD_WAIT_REQ_MS	500	/* bridge WAIT timeout: signal latency of the main loop */

enum { OP_NONE, OP_ENROL, OP_MATCH, OP_DETECT };
static const char *op_name[] = { "none", "enrol", "match", "detect" };
enum { CAP_OK, CAP_CANCEL, CAP_TIMEOUT, CAP_ERR };

static const char *ta_dir = "/var/local/fpc/ta", *ta_file = "fpctzappfingerprint", *ta_name = "fingerprint";
static uint32_t appid;
static int ta_resident, sensor_ok, keep_ta, require_hat, verbose;
static long op_timeout_ms = 120000;
static volatile sig_atomic_t g_stop, g_wipe, g_dump;

/* ta_mx serialises every qsee_lib call (one SMC user, and say_quiet etc. are globals). st_mx guards S, the id cache
 * and the counters. Order: ta_mx before st_mx, never the reverse. */
static pthread_mutex_t ta_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t st_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t st_cv = PTHREAD_COND_INITIALIZER;
static struct {
	int op, busy, cancel, finger_on;
	uint32_t gen;
	long started;
	uint32_t gid;
	uint32_t pre_ids[5], pre_n;	/* id list before begin_enrol: finds the new id */
	uint32_t ids[5], nids;
	int ids_valid;
	uint32_t captures, good, matches, nomatches, withheld, evseq;
	struct fpcd_event last_match;
	int have_last_match;
} S;

/* ---------------------------------------------------------------- log */
static struct timespec t_start;
static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static int log_con = -1;

static long now_ms(void)
{
	return fpc_ms_since(&t_start);
}

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
	char line[600];
	long ms = now_ms();
	int h = snprintf(line, sizeof(line), "sweetfpcd %ld.%03ld: ", ms / 1000, ms % 1000);
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line + h, sizeof(line) - (size_t)h - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	n += h;
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	pthread_mutex_lock(&log_mx);
	if (write(2, line, (size_t)n) < 0 || (log_con >= 0 && write(log_con, line, (size_t)n) < 0)) {
		/* nowhere else to report */
	}
	pthread_mutex_unlock(&log_mx);
}

/* ---------------------------------------------------------------- Apple command names (p3 doc 2.2) */
static const char *fpcd_cmd_name(uint32_t c)
{
	switch (c) {
	case 1: return "GetCmdProtocolVersion";	case 2: return "ResetSensor";	case 3: return "EnrollMode";
	case 4: return "MatchMode";		case 5: return "ReadCatacombSU";	case 8: return "AlignmentData";
	case 9: return "RequestCaptureBuffer";	case 10: return "PreImage";	case 11: return "PostImage";
	case 12: return "Cancel";		case 13: return "RemoveTemplate";	case 14: return "EnrollContinue";
	case 15: return "RequestMaxIdentityCount"; case 16: return "GetProvisioningState";
	case 17: return "RequestTopologyOfTemplate"; case 18: return "ProvisionSensor"; case 20: return "DisplayStatusChanged";
	case 24: return "ThermalLevelChanged";	case 27: return "GetSerialisedTemplates";
	case 30: return "TemplatesExistsAtBoot";	case 31: return "PrepareSerialisedTemplates";
	case 34: return "GetModuleSerialNumber";	case 36: return "CacheCustomPatch";
	case 38: return "FingerDetectMode";	case 39: return "GetSKSLockState";	case 40: return "GetBiometrickitdInfo";
	case 41: return "Diagnostics";		case 42: return "GetLoggingType";	case 43: return "RequestMessageData";
	case 44: return "BioLogEnabled";	case 46: return "GetProtectedConfig";	case 47: return "SetProtectedConfig";
	case 48: return "GetEnabledForUnlock";	case 49: return "NoCatacomb";	case 50: return "GetTemplatesValidity";
	case 51: return "GetTimestampCollection"; case 53: return "GetSensorInfo";	case 54: return "IsDisableTheDevGidSet";
	case 55: return "UnprovisionSensor";	case 56: return "GetCatacombId";	case 57: return "DropUnlockToken";
	case 58: return "GetCatacombHash";	case 59: return "GetSensorSerialNumber"; case 60: return "GetCatacombState";
	case 61: return "PrepareSaveCatacomb";	case 62: return "CompleteSaveCatacomb"; case 63: return "ConfirmSaveCatacomb";
	case 64: return "LoadCatacomb";		case 65: return "GetFreeIdentityCount"; case 66: return "GetIdentitiesList";
	case 67: return "GetSystemProtectedConfig"; case 68: return "SetSystemProtectedConfig";
	case 69: return "EnableBackgroundFdet";	case 70: return "TouchIDButtonPressed"; case 72: return "RemoveUserData";
	case 73: return "ForceBioLockout";	case 77: return "SeaCookieHandleMessage"; case 78: return "GetLastMatchEvent";
	case 83: return "IsSensorReady";	case 87: return "SystemSleepStateChanged";
	case FPCD_CMD_STATUS: return "fpcd:Status";	case FPCD_CMD_WIPE_ALL: return "fpcd:WipeAll";
	case FPCD_CMD_PING: return "fpcd:Ping";
	case 25: case 26: case 29: case 32: case 33: case 37: return "Calibration*";
	default: return "?";
	}
}

/* ---------------------------------------------------------------- bridge */
static io_connect_t conn;
static pthread_mutex_t ev_mx = PTHREAD_MUTEX_INITIALIZER;

static int bridge_open(void)
{
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SMB_SERVICE));
	if (!s)
		s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(SMB_SERVICE));
	if (!s)
		return -1;
	io_connect_t c = 0;
	kern_return_t kr = IOServiceOpen(s, mach_task_self(), SMB_UC_TYPE, &c);
	IOObjectRelease(s);
	if (kr != KERN_SUCCESS) {
		lg("bridge: IOServiceOpen(%s, type %d): %#x", SMB_SERVICE, SMB_UC_TYPE, kr);
		return -2;
	}
	struct smb_state st = { 0 };
	size_t sz = sizeof(st);
	kr = IOConnectCallStructMethod(c, SMB_SEL_STATE, NULL, 0, &st, &sz);
	lg("bridge: %s open; STATE %s: version %u flags %#x pending %u clients %u", SMB_SERVICE,
	   kr ? "failed" : "ok", st.version, st.flags, st.pending, st.clients);
	pthread_mutex_lock(&ev_mx);
	conn = c;
	pthread_mutex_unlock(&ev_mx);
	return 0;
}

static void bridge_close(void)
{
	pthread_mutex_lock(&ev_mx);
	if (conn)
		IOServiceClose(conn);
	conn = 0;
	pthread_mutex_unlock(&ev_mx);
}

static void ev_send(unsigned code, int32_t status, uint32_t fid, int32_t score, uint32_t remaining, uint32_t samples,
		    uint32_t flags, int with_ids)
{
	static struct smb_event e;	/* under ev_mx */
	struct fpcd_event ev = { .magic = FPCD_EV_MAGIC, .version = FPCD_ABI_VERSION, .code = (uint16_t)code,
				 .status = status, .fid = fid, .score = score, .remaining = remaining,
				 .samples = samples, .flags = flags };
	pthread_mutex_lock(&st_mx);
	ev.evseq = ++S.evseq;
	ev.op_gen = S.gen;
	if (with_ids) {
		ev.n = S.nids;
		memcpy(ev.ids, S.ids, sizeof(ev.ids));
	}
	if (code == FPCD_EV_MATCH) {
		S.last_match = ev;
		S.have_last_match = 1;
	}
	pthread_mutex_unlock(&st_mx);
	pthread_mutex_lock(&ev_mx);
	kern_return_t kr = -1;
	if (conn) {
		memset(&e, 0, sizeof(e));
		e.subtype = fpcd_event_subtype(code);
		e.len = sizeof(ev);
		memcpy(e.data, &ev, sizeof(ev));
		kr = IOConnectCallStructMethod(conn, SMB_SEL_EVENT, &e, sizeof(e), NULL, NULL);
	}
	pthread_mutex_unlock(&ev_mx);
	lg("event #%u code %u sub %#x: status %d fid %#x score %d remaining %u samples %u flags %#x n %u -> %s", ev.evseq,
	   code, SMB_MSG_BASE + fpcd_event_subtype(code), status, fid, score, remaining, samples, flags, ev.n,
	   !conn ? "no bridge (logged only)" : kr ? "EVENT failed" : "sent");
}

/* ---------------------------------------------------------------- TA helpers (caller holds ta_mx) */
static int ta_bio(uint32_t cmd, uint8_t *d, int32_t *st)
{
	int r = fpc_bio(appid, d, cmd, st);
	if (r)
		lg("ta: g11/%u transport rc %d", cmd, r);
	return r;
}

static int ta_set_gid(uint32_t gid, int32_t *st)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	put_u32(d + 0xc, gid);
	return ta_bio(9, d, st);
}

static int ta_load_empty_db(int32_t *st)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	return ta_bio(6, d, st);
}

/* refresh the id cache from the TA; 0 ok */
static int ids_refresh(void)
{
	uint32_t l[5] = { 0 }, n = 0;
	int32_t st = 0;
	if (fpc_get_ids(appid, l, &n, &st) || st) {
		lg("ta: get_finger_ids failed (status %d)", st);
		return -1;
	}
	pthread_mutex_lock(&st_mx);
	memcpy(S.ids, l, sizeof(S.ids));
	S.nids = n;
	S.ids_valid = 1;
	pthread_mutex_unlock(&st_mx);
	char s[80] = "";
	for (uint32_t i = 0; i < n; i++)
		snprintf(s + strlen(s), sizeof(s) - strlen(s), " %#x", l[i]);
	lg("ta: ids (%u):%s", n, n ? s : " none");
	return 0;
}

static int fid_listed(uint32_t fid)
{
	int ok = 0;
	pthread_mutex_lock(&st_mx);
	for (uint32_t i = 0; i < S.nids && S.ids_valid; i++)
		ok |= S.ids[i] == fid;
	pthread_mutex_unlock(&st_mx);
	return ok;
}

static int ta_sensor_init(void)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	int r = fd_cmd(appid, d, 0, &st);
	lg("ta: sensor init g10/0: rc %d status %d", r, st);
	if (!r) {
		memset(d, 0, sizeof(d));
		r = fd_cmd(appid, d, 5, &st);
		lg("ta: sensor init g10/5: rc %d status %d", r, st);
	}
	sensor_ok = !r;
	return r;
}

/* ---------------------------------------------------------------- operations */
static int cancelled(uint32_t gen)
{
	pthread_mutex_lock(&st_mx);
	int c = g_stop || S.cancel || S.gen != gen;
	pthread_mutex_unlock(&st_mx);
	return c;
}

static void finger_set(int on)
{
	pthread_mutex_lock(&st_mx);
	int was = S.finger_on;
	S.finger_on = on;
	pthread_mutex_unlock(&st_mx);
	if (was != on)
		ev_send(on ? FPCD_EV_FINGER_DOWN : FPCD_EV_FINGER_UP, 0, 0, 0, 0, 0, 0, 0);
}

static void sensor_release(void)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st;
	pthread_mutex_lock(&ta_mx);
	fd_cmd(appid, d, 5, &st);
	pthread_mutex_unlock(&ta_mx);
}

/*
 * One capture as the HAL's capture_image(wait_for_finger) (qsee_lib fpc_capture), cancellable: CAP_OK with *st4 =
 * the g10/4 status (0 = good image), CAP_CANCEL, CAP_TIMEOUT (deadline, ms since start; 0 = none), CAP_ERR.
 */
static int capture_once(uint32_t gen, int32_t *st4, long deadline)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	pthread_mutex_lock(&ta_mx);
	d[0xc] = 1;
	int r = fd_cmd(appid, d, 2, &st);
	pthread_mutex_unlock(&ta_mx);
	if (r) {
		lg("capture: arm (g10/2) rc %d status %d", r, st);
		return CAP_ERR;
	}
	long armed = now_ms();
	for (;;) {
		if (cancelled(gen)) {
			sensor_release();
			return CAP_CANCEL;
		}
		if (deadline && now_ms() >= deadline) {
			sensor_release();
			return CAP_TIMEOUT;
		}
		int hit = fpc_wait_high(FPCD_WAIT_SLICE_MS);
		if (hit < 0) {
			sensor_release();
			return CAP_ERR;
		}
		if (hit)
			break;
		if (now_ms() - armed >= FPCD_UP_MS)
			finger_set(0);
	}
	finger_set(1);
	pthread_mutex_lock(&ta_mx);
	r = fd_cmd(appid, d, 1, &st);	/* the HAL ignores this status */
	int rc = CAP_OK;
	*st4 = 0;
	if (r && r != 5) {
		rc = CAP_ERR;
		goto out;
	}
	for (int tries = 1; tries <= 4; tries++) {
		r = fd_cmd(appid, d, 3, &st);
		if (r && r != 5) {
			rc = CAP_ERR;
			goto out;
		}
		int hit = fpc_wait_high(FPCD_CAPTURE_WAIT_MS);
		if (hit <= 0) {
			lg("capture: no irq %d ms after g10/3", FPCD_CAPTURE_WAIT_MS);
			*st4 = 3;	/* treat as insufficient: start over */
			break;
		}
		fd_cmd(appid, d, 4, st4);
		if (*st4 != 3)
			break;
	}
	if (*st4)
		fd_cmd(appid, d, 5, &st);	/* capture_image's error path */
out:
	pthread_mutex_unlock(&ta_mx);
	pthread_mutex_lock(&st_mx);
	S.captures++;
	S.good += rc == CAP_OK && *st4 == 0;
	pthread_mutex_unlock(&st_mx);
	if (rc == CAP_ERR)
		lg("capture: TZ/TA error (rc %d status %d)", r, st);
	return rc;
}

/* Step 5 hook: keymaster verification of the FPC HAT for this identify (KM begin -> challenge -> g3/4 get_auth_result
 * -> KM update/finish). Not implemented: returns 0 = not verified (no flag). 1 = verified, -1 = refused. */
static int fpcd_verify_hat(uint32_t fid)
{
	(void)fid;
	return 0;
}

/*
 * The ONLY place that produces FPCD_EV_MATCH. Delivered iff: the TA's identify status is 0, the finger id is one the
 * TA lists, /var/local/fpc/allow-match exists, the HAT hook did not refuse (and verified it, with FPCD_REQUIRE_HAT=1).
 * Anything else -> MATCH_WITHHELD (never a match). Returns 1 iff a match was sent.
 */
static int match_deliver(int32_t ta_status, uint32_t fid, int32_t score, uint32_t attempts)
{
	struct stat sb;
	uint32_t why = 0;
	int hat = 0;
	if (ta_status != 0)
		return 0;
	if (!fid_listed(fid))
		why = FPCD_MW_UNKNOWN_FID;
	else if (stat(ALLOW_MATCH_PATH, &sb))
		why = FPCD_MW_NO_ALLOW_FILE;
	else if ((hat = fpcd_verify_hat(fid)) < 0 || (require_hat && hat != 1))
		why = FPCD_MW_HAT_REFUSED;
	if (why) {
		pthread_mutex_lock(&st_mx);
		S.withheld++;
		pthread_mutex_unlock(&st_mx);
		lg("match: identify status 0 fid %#x score %d WITHHELD (%s)", fid, score,
		   why == FPCD_MW_UNKNOWN_FID ? "fid not in the TA list" :
		   why == FPCD_MW_NO_ALLOW_FILE ? ALLOW_MATCH_PATH " absent" : "HAT not verified");
		ev_send(FPCD_EV_MATCH_WITHHELD, ta_status, fid, score, 0, attempts, why, 0);
		return 0;
	}
	pthread_mutex_lock(&st_mx);
	S.matches++;
	pthread_mutex_unlock(&st_mx);
	lg("match: fid %#x score %d attempts %u, HAT %s", fid, score, attempts, hat == 1 ? "verified" : "NOT verified (step 5)");
	ev_send(FPCD_EV_MATCH, 0, fid, score, 0, attempts, hat == 1 ? FPCD_MF_HAT_VERIFIED : 0, 0);
	return 1;
}

static long op_deadline(void)
{
	pthread_mutex_lock(&st_mx);
	long d = op_timeout_ms > 0 ? S.started + op_timeout_ms : 0;
	pthread_mutex_unlock(&st_mx);
	return d;
}

/* returns 1 when the op should continue with the next capture, 0 when it ends; *st4 valid on 1 + good image */
static int op_capture(uint32_t gen, int32_t *st4)
{
	int r = capture_once(gen, st4, op_deadline());
	if (r == CAP_CANCEL)
		return 0;
	if (r == CAP_TIMEOUT) {
		lg("op: timeout after %ld ms", op_timeout_ms);
		ev_send(FPCD_EV_OP_TIMEOUT, 0, 0, 0, 0, 0, 0, 0);
		return 0;
	}
	if (r == CAP_ERR) {
		ev_send(FPCD_EV_ERROR, -5, 0, 0, 0, 0, 0, 0);
		return 0;
	}
	if (*st4 < 0) {
		lg("op: g10/4 FPC error %d", *st4);
		ev_send(FPCD_EV_ERROR, *st4, 0, 0, 0, 0, 0, 0);
		return 0;
	}
	if (*st4 == 2 || *st4 == 3)
		ev_send(FPCD_EV_IMAGE, *st4, 0, 0, 0, 0, 0, 0);
	return 1;
}

static void run_enrol(uint32_t gen)
{
	int samples = 0;
	while (!cancelled(gen)) {
		int32_t st4 = 0, st = 0;
		if (!op_capture(gen, &st4))
			return;
		if (st4)
			continue;	/* 1/4 too fast / no finger (idle loop), 2/3 reported */
		uint8_t d[FPC_SENSOR_LEN] = { 0 };
		pthread_mutex_lock(&ta_mx);
		int r = ta_bio(1, d, &st);
		pthread_mutex_unlock(&ta_mx);
		if (r) {
			ev_send(FPCD_EV_ERROR, -5, 0, 0, 0, 0, 0, 0);
			return;
		}
		uint32_t rem = rd_u32(d + 0xc);
		if (st == 0 || st == 1 || st == 5)
			samples++;
		lg("enrol: code %d (%s), remaining %u, accepted %d", st, enrol_code(st), rem, samples);
		ev_send(FPCD_EV_ENROL_PROGRESS, st, 0, 0, rem, (uint32_t)samples, 0, 0);
		if (st == 2 || st == 3 || st < 0) {
			ev_send(FPCD_EV_ENROL_FAILED, st, 0, 0, rem, (uint32_t)samples, 0, 0);
			return;
		}
		if (st != 0)
			continue;
		/* done: end_enrol; without a HAT it gives -1 but the TA may still list the finger (P5) */
		int32_t st2 = 0;
		uint32_t fid = 0, flags = 0;
		memset(d, 0, sizeof(d));
		pthread_mutex_lock(&ta_mx);
		r = ta_bio(2, d, &st2);
		fid = rd_u32(d + 0xc);
		ids_refresh();
		pthread_mutex_unlock(&ta_mx);
		if (r || st2) {
			fid = 0;
			pthread_mutex_lock(&st_mx);
			for (uint32_t i = 0; i < S.nids && !fid; i++) {
				int old = 0;
				for (uint32_t j = 0; j < S.pre_n; j++)
					old |= S.pre_ids[j] == S.ids[i];
				if (!old)
					fid = S.ids[i];
			}
			pthread_mutex_unlock(&st_mx);
			flags |= FPCD_EF_END_UNAUTH;
			lg("enrol: end_enrol rc %d status %d; new id from get_finger_ids: %#x%s", r, st2, fid,
			   fid ? "" : " (none: enrol lost)");
		} else {
			lg("enrol: end_enrol ok, finger id %#x", fid);
		}
		ev_send(fid ? FPCD_EV_ENROL_DONE : FPCD_EV_ENROL_FAILED, st2, fid, 0, 0, (uint32_t)samples, flags, 1);
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
		return;
	}
}

static void run_match(uint32_t gen)
{
	uint32_t attempts = 0;
	while (!cancelled(gen)) {
		int32_t st4 = 0, st = 0;
		if (!op_capture(gen, &st4))
			return;
		if (st4)
			continue;
		uint8_t d[FPC_SENSOR_LEN] = { 0 };
		pthread_mutex_lock(&ta_mx);
		int r = ta_bio(3, d, &st);
		pthread_mutex_unlock(&ta_mx);
		if (r) {
			ev_send(FPCD_EV_ERROR, -5, 0, 0, 0, 0, 0, 0);
			return;
		}
		attempts++;
		uint32_t fid = rd_u32(d + 0xc);
		int32_t score = (int32_t)rd_u32(d + 0x20);
		lg("identify %u: status %d fid %#x score %d coverage %d quality %d result %d", attempts, st, fid, score,
		   (int)rd_u32(d + 0x10), (int)rd_u32(d + 0x14), (int)rd_u32(d + 0x1c));
		if (st == 0 && !cancelled(gen)) {
			match_deliver(st, fid, score, attempts);
			return;	/* one result per MatchMode: delivered or withheld */
		}
		pthread_mutex_lock(&st_mx);
		S.nomatches++;
		pthread_mutex_unlock(&st_mx);
		ev_send(FPCD_EV_NOMATCH, st, 0, score, 0, attempts, 0, 0);
	}
}

static void run_detect(uint32_t gen)
{
	while (!cancelled(gen)) {
		int32_t st4 = 0;
		if (!op_capture(gen, &st4))
			return;
	}
}

static void *capture_thread(void *arg)
{
	(void)arg;
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);	/* signals go to the main thread */
	for (;;) {
		pthread_mutex_lock(&st_mx);
		while (!g_stop && (S.op == OP_NONE || S.busy))
			pthread_cond_wait(&st_cv, &st_mx);
		if (g_stop) {
			pthread_mutex_unlock(&st_mx);
			return NULL;
		}
		int op = S.op;
		uint32_t gen = S.gen;
		S.busy = 1;
		pthread_mutex_unlock(&st_mx);
		long t0 = now_ms();
		lg("op %s #%u: start", op_name[op], gen);
		if (op == OP_ENROL)
			run_enrol(gen);
		else if (op == OP_MATCH)
			run_match(gen);
		else
			run_detect(gen);
		pthread_mutex_lock(&st_mx);
		if (S.gen == gen)
			S.op = OP_NONE;
		S.busy = 0;
		int c = S.cancel;
		pthread_cond_broadcast(&st_cv);
		pthread_mutex_unlock(&st_mx);
		lg("op %s #%u: end after %ld ms%s", op_name[op], gen, now_ms() - t0, c ? " (cancelled)" : "");
	}
}

/* cancel the running op and wait (max 5 s) until the capture thread is idle; 0 ok */
static int op_cancel_wait(void)
{
	struct timespec dl;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += 5;
	pthread_mutex_lock(&st_mx);
	S.cancel = 1;
	pthread_cond_broadcast(&st_cv);
	int rc = 0;
	while (S.busy && !rc)
		rc = pthread_cond_timedwait(&st_cv, &st_mx, &dl);
	if (!rc) {
		S.op = OP_NONE;
		S.cancel = 0;
	}
	pthread_mutex_unlock(&st_mx);
	if (rc)
		lg("op: cancel: capture thread still busy after 5 s");
	return rc ? -1 : 0;
}

static int op_start(int op)
{
	if (op_cancel_wait())
		return FPCD_E_BUSY;
	pthread_mutex_lock(&st_mx);
	S.op = op;
	S.gen++;
	S.cancel = 0;
	S.started = now_ms();
	pthread_cond_broadcast(&st_cv);
	pthread_mutex_unlock(&st_mx);
	return FPCD_OK;
}

/* debug "wipe all": cancel, delete every id the TA lists; 0 iff the list ends empty */
static int wipe_all(const char *why)
{
	op_cancel_wait();
	pthread_mutex_lock(&ta_mx);
	int r = fpc_cleanup_all(appid, "wipe");
	ids_refresh();
	pthread_mutex_unlock(&ta_mx);
	lg("wipe all (%s): %s", why, r ? "FAILED, ids left" : "OK, no finger left");
	ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
	return r ? FPCD_E_IO : FPCD_OK;
}

/* ---------------------------------------------------------------- command handlers */
static uint32_t arg_u32(const struct smb_req *q, uint32_t def)
{
	return q->len >= 4 ? rd_u32(q->data) : def;
}

static int rep_u32(struct smb_reply *p, uint32_t v)
{
	put_u32(p->data, v);
	p->len = 4;
	return FPCD_OK;
}

static int rep_ta_status(struct smb_reply *p, int32_t st)
{
	put_u32(p->data, (uint32_t)st);
	p->len = 4;
	return FPCD_E_IO;
}

static int h_proto(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, 2); }
static int h_ready(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, appid && sensor_ok); }
static int h_maxids(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, FPC_MAX_FINGERS); }
static int h_unsupported(const struct smb_req *q, struct smb_reply *p) { return FPCD_E_UNSUPPORTED; }

static int h_reset(const struct smb_req *q, struct smb_reply *p)
{
	if (op_cancel_wait())
		return FPCD_E_BUSY;
	pthread_mutex_lock(&ta_mx);
	int r = ta_sensor_init();
	pthread_mutex_unlock(&ta_mx);
	return r ? FPCD_E_IO : FPCD_OK;
}

static int h_ids(const struct smb_req *q, struct smb_reply *p)
{
	if (!pthread_mutex_trylock(&ta_mx)) {	/* fresh list when the TA is free, else the cache */
		ids_refresh();
		pthread_mutex_unlock(&ta_mx);
	}
	struct fpcd_ids ids = { 0 };
	pthread_mutex_lock(&st_mx);
	int ok = S.ids_valid;
	ids.count = S.nids;
	memcpy(ids.ids, S.ids, sizeof(ids.ids));
	pthread_mutex_unlock(&st_mx);
	if (!ok)
		return FPCD_E_IO;
	memcpy(p->data, &ids, sizeof(ids));
	p->len = sizeof(ids);
	return FPCD_OK;
}

static int h_free(const struct smb_req *q, struct smb_reply *p)
{
	pthread_mutex_lock(&st_mx);
	uint32_t n = S.nids, ok = S.ids_valid;
	pthread_mutex_unlock(&st_mx);
	return ok ? rep_u32(p, n < FPC_MAX_FINGERS ? FPC_MAX_FINGERS - n : 0) : FPCD_E_IO;
}

static int h_catacomb_id(const struct smb_req *q, struct smb_reply *p)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	pthread_mutex_lock(&ta_mx);
	int r = ta_bio(10, d, &st);
	pthread_mutex_unlock(&ta_mx);
	if (r || st)
		return rep_ta_status(p, st);
	memcpy(p->data, d + 0x10, 8);
	p->len = 8;
	return FPCD_OK;
}

static int h_enrol(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t gid = arg_u32(q, 0);
	if (op_cancel_wait())
		return FPCD_E_BUSY;
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	pthread_mutex_lock(&ta_mx);
	ids_refresh();
	int r = ta_set_gid(gid, &st);
	if (!r && !st) {
		memset(d, 0, sizeof(d));
		r = ta_bio(0, d, &st);	/* begin_enrol; the HAL sends authorize_enrol (g3, HAT) first: not yet */
	}
	pthread_mutex_unlock(&ta_mx);
	lg("enrol: gid %u, set_gid/begin_enrol rc %d status %d", gid, r, st);
	if (r || st)
		return rep_ta_status(p, st);
	pthread_mutex_lock(&st_mx);
	S.gid = gid;
	memcpy(S.pre_ids, S.ids, sizeof(S.pre_ids));
	S.pre_n = S.ids_valid ? S.nids : 0;
	pthread_mutex_unlock(&st_mx);
	return op_start(OP_ENROL);
}

static int h_enrol_continue(const struct smb_req *q, struct smb_reply *p)
{
	pthread_mutex_lock(&st_mx);
	int op = S.op;
	pthread_mutex_unlock(&st_mx);
	return op == OP_ENROL ? FPCD_OK : FPCD_E_INVAL;	/* the capture thread continues by itself */
}

static int h_match(const struct smb_req *q, struct smb_reply *p)
{
	pthread_mutex_lock(&st_mx);
	uint32_t n = S.nids;
	pthread_mutex_unlock(&st_mx);
	if (!n)
		lg("match: no finger enrolled in the TA: every identify will fail");
	return op_start(OP_MATCH);
}

static int h_cancel(const struct smb_req *q, struct smb_reply *p)
{
	return op_cancel_wait() ? FPCD_E_BUSY : FPCD_OK;
}

static int h_fdet(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t en = arg_u32(q, 1);
	pthread_mutex_lock(&st_mx);
	int op = S.op;
	pthread_mutex_unlock(&st_mx);
	if (!en)
		return op == OP_DETECT ? h_cancel(q, p) : FPCD_OK;
	if (op == OP_ENROL || op == OP_MATCH)
		return FPCD_OK;	/* those report finger down/up already */
	return op == OP_DETECT ? FPCD_OK : op_start(OP_DETECT);
}

static int h_remove(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t fid = arg_u32(q, FPCD_FID_ALL);
	if (fid == FPCD_FID_ALL)
		return wipe_all("RemoveTemplate all");
	if (q->len < 4)
		return FPCD_E_INVAL;
	if (op_cancel_wait())
		return FPCD_E_BUSY;
	pthread_mutex_lock(&ta_mx);
	ids_refresh();
	int listed = fid_listed(fid);
	int r = listed ? fpc_delete(appid, fid) : 0;
	ids_refresh();
	int still = fid_listed(fid);
	pthread_mutex_unlock(&ta_mx);
	lg("remove %#x: %s, delete rc %d, %s", fid, listed ? "listed" : "NOT listed", r, still ? "STILL LISTED" : "gone");
	if (listed)
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
	return !listed ? FPCD_E_NOENT : still ? FPCD_E_IO : FPCD_OK;
}

static int h_remove_all(const struct smb_req *q, struct smb_reply *p)
{
	return wipe_all(fpcd_cmd_name(q->cmd));
}

static int h_nocatacomb(const struct smb_req *q, struct smb_reply *p)
{
	/* no catacomb on disk: an empty template db (templates are TA-memory only for now) */
	if (op_cancel_wait())
		return FPCD_E_BUSY;
	int32_t st = 0, st2 = 0;
	pthread_mutex_lock(&ta_mx);
	int r = ta_load_empty_db(&st);
	if (!r && !st)
		r = ta_set_gid(0, &st2);
	ids_refresh();
	pthread_mutex_unlock(&ta_mx);
	lg("NoCatacomb: load_empty_db status %d, set_gid 0 status %d", st, st2);
	ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
	return r || st || st2 ? rep_ta_status(p, st ? st : st2) : FPCD_OK;
}

static int h_last_match(const struct smb_req *q, struct smb_reply *p)
{
	pthread_mutex_lock(&st_mx);
	if (S.have_last_match) {	/* only ever set by match_deliver()'s FPCD_EV_MATCH */
		memcpy(p->data, &S.last_match, sizeof(S.last_match));
		p->len = sizeof(S.last_match);
	}
	pthread_mutex_unlock(&st_mx);
	return FPCD_OK;
}

static int h_status(const struct smb_req *q, struct smb_reply *p)
{
	struct stat sb;
	struct fpcd_status s = { .abi = FPCD_ABI_VERSION, .appid = appid, .allow_match = !stat(ALLOW_MATCH_PATH, &sb) };
	pthread_mutex_lock(&st_mx);
	s.op = (uint32_t)S.op, s.op_gen = S.gen, s.finger_on = (uint32_t)S.finger_on;
	s.captures = S.captures, s.good_images = S.good, s.matches = S.matches, s.nomatches = S.nomatches;
	s.withheld = S.withheld, s.ids.count = S.nids;
	memcpy(s.ids.ids, S.ids, sizeof(s.ids.ids));
	pthread_mutex_unlock(&st_mx);
	memcpy(p->data, &s, sizeof(s));
	p->len = sizeof(s);
	return FPCD_OK;
}

static int h_wipe(const struct smb_req *q, struct smb_reply *p) { return wipe_all("bridge WipeAll"); }

static int h_ping(const struct smb_req *q, struct smb_reply *p)
{
	p->len = (uint32_t)snprintf((char *)p->data, 64, "sweetfpcd %s", FPCD_VERSION);
	return FPCD_OK;
}

/* Apple command id -> action (unlisted ids: FPCD_E_UNSUPPORTED, logged as unmapped) */
static const struct {
	uint32_t cmd;
	int (*fn)(const struct smb_req *, struct smb_reply *);
} cmd_map[] = {
	{ 1, h_proto },		/* GetCmdProtocolVersion: 2 */
	{ 2, h_reset },		/* ResetSensor: cancel, g10/0 + g10/5 */
	{ 3, h_enrol },		/* EnrollMode: set_gid, begin_enrol, enrol op */
	{ 14, h_enrol_continue },	/* EnrollContinue: ack (the op runs on its own) */
	{ 4, h_match },		/* MatchMode: identify op */
	{ 12, h_cancel },	/* Cancel */
	{ 13, h_remove },	/* RemoveTemplate: delete_template (FPCD_FID_ALL: all) */
	{ 72, h_remove_all },	/* RemoveUserData: delete every id */
	{ 66, h_ids },		/* GetIdentitiesList: get_finger_ids */
	{ 65, h_free },		/* GetFreeIdentityCount: 5 - count */
	{ 15, h_maxids },	/* RequestMaxIdentityCount: 5 */
	{ 56, h_catacomb_id },	/* GetCatacombId: get_template_db_id */
	{ 38, h_fdet },		/* FingerDetectMode */
	{ 69, h_fdet },		/* EnableBackgroundFdet */
	{ 83, h_ready },	/* IsSensorReady */
	{ 49, h_nocatacomb },	/* NoCatacomb: load_empty_db + set_gid 0 */
	{ 78, h_last_match },	/* GetLastMatchEvent */
	{ 5, h_unsupported },	/* ReadCatacombSU     } catacomb store/load: templates are TA-memory only (no */
	{ 61, h_unsupported },	/* PrepareSaveCatacomb } store_template_db / load_template_db yet) */
	{ 62, h_unsupported },	/* CompleteSaveCatacomb */
	{ 63, h_unsupported },	/* ConfirmSaveCatacomb */
	{ 64, h_unsupported },	/* LoadCatacomb */
	{ FPCD_CMD_STATUS, h_status },
	{ FPCD_CMD_WIPE_ALL, h_wipe },
	{ FPCD_CMD_PING, h_ping },
};

static struct smb_req rq;
static struct smb_reply rp;

static void dispatch(void)
{
	long t0 = now_ms();
	memset(&rp, 0, offsetof(struct smb_reply, data) + 64);
	rp.seq = rq.seq;
	if (rq.len > SMB_DATA_MAX)
		rq.len = SMB_DATA_MAX;
	int mapped = 0;
	for (unsigned i = 0; i < sizeof(cmd_map) / sizeof(cmd_map[0]); i++)
		if (cmd_map[i].cmd == rq.cmd) {
			rp.status = cmd_map[i].fn(&rq, &rp);
			mapped = 1;
			break;
		}
	if (!mapped)
		rp.status = FPCD_E_UNSUPPORTED;
	kern_return_t kr = IOConnectCallStructMethod(conn, SMB_SEL_REPLY, &rp, sizeof(rp), NULL, NULL);
	lg("cmd seq %u id %u %s len %u -> status %d len %u%s (%ld ms)%s", rq.seq, rq.cmd, fpcd_cmd_name(rq.cmd), rq.len,
	   rp.status, rp.len, mapped ? "" : " UNMAPPED", now_ms() - t0, kr ? " REPLY FAILED" : "");
	if (kr)
		lg("bridge: REPLY seq %u: %#x", rq.seq, kr);
}

/* ---------------------------------------------------------------- startup / shutdown */
static void on_sig(int s)
{
	if (s == SIGUSR1)
		g_wipe = 1;
	else if (s == SIGUSR2)
		g_dump = 1;
	else
		g_stop = 1;
}

static void dump_status(void)
{
	struct stat sb;
	pthread_mutex_lock(&st_mx);
	lg("status: app %u op %s #%u finger %s ids %u | captures %u good %u matches %u nomatch %u withheld %u | "
	   "allow-match %s | bridge %s", appid, op_name[S.op], S.gen, S.finger_on ? "on" : "off", S.nids, S.captures,
	   S.good, S.matches, S.nomatches, S.withheld, stat(ALLOW_MATCH_PATH, &sb) ? "absent" : "PRESENT",
	   conn ? "open" : "closed");
	pthread_mutex_unlock(&st_mx);
}

/* kern.sweet_fpc needs value 3 (bit 0 reset, bit 1 irq; xnu 1190). wait: poll every 10 s until it is. */
static int fpc_kernel_ready(int wait)
{
	int said = 0;
	for (;;) {
		uint32_t v = fpc_sysctl_u32("kern.sweet_fpc");
		if (v != 0xffffffffu && (v & 3) == 3)
			return 0;
		if (!said++)
			lg("kern.sweet_fpc = %s: need 3 (reset + irq, xnu 1190)%s",
			   v == 0xffffffffu ? "missing" : v ? "partial" : "0", wait ? "; waiting" : "");
		if (!wait || g_stop)
			return -1;
		sleep(10);
	}
}

/* lookup / load, sensor init, gid, ids. 0 ok */
static int ta_up(int fresh)
{
	uint32_t id = 0;
	pthread_mutex_lock(&ta_mx);
	int lr = lookup_name(ta_name, &id);
	if (lr >= 2) {
		lg("ta: lookup %s failed (%d)", ta_name, lr);
		goto fail;
	}
	if (lr == 0 && fresh) {
		lg("ta: --fresh: unloading resident %s (app %u)", ta_name, id);
		setenv("QSEE_APP", ta_name, 1);
		if (check_appid(id) || qsee_unload(id))
			goto fail;
		lr = 1;
	}
	if (lr == 0) {
		ta_resident = 1;
		lg("ta: %s resident as app %u: reusing it (templates in TA memory kept)", ta_name, id);
	} else {
		lg("ta: loading %s/%s.mdt", ta_dir, ta_file);
		int r = cmd_load(ta_file, ta_dir);
		if (r || lookup_name(ta_name, &id)) {
			lg("ta: load failed (rc %d)", r);
			goto fail;
		}
		lg("ta: loaded as app %u", id);
	}
	setenv("QSEE_APP", ta_name, 1);
	if (check_appid(id)) {	/* never keymaster64, must be the fingerprint TA */
		lg("ta: app id %u refused by the guard", id);
		goto fail;
	}
	appid = id;
	if (ta_sensor_init())
		goto fail;
	int32_t st = 0;
	if (!ta_resident) {
		ta_load_empty_db(&st);
		lg("ta: load_empty_db status %d", st);
	}
	int r = ta_set_gid(0, &st);
	lg("ta: set_gid 0: rc %d status %d", r, st);
	if ((r || st) && ta_resident) {
		ta_load_empty_db(&st);
		r = ta_set_gid(0, &st);
		lg("ta: retry after load_empty_db: set_gid rc %d status %d", r, st);
	}
	if (r || st || ids_refresh())
		goto fail;
	pthread_mutex_unlock(&ta_mx);
	return 0;
fail:
	pthread_mutex_unlock(&ta_mx);
	return -1;
}

static void ta_down(void)
{
	if (!appid)
		return;
	if (keep_ta) {
		lg("ta: kept loaded as app %u (--keep-ta)", appid);
		return;
	}
	pthread_mutex_lock(&ta_mx);
	int r = check_appid(appid) ? -1 : qsee_unload(appid);
	pthread_mutex_unlock(&ta_mx);
	lg("ta: unload app %u: %s", appid, r ? "FAILED" : "ok");
	appid = 0;
}

static int selftest(int touch_s)
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	pthread_mutex_lock(&ta_mx);
	int r = ta_bio(10, d, &st);
	pthread_mutex_unlock(&ta_mx);
	lg("selftest: start OK: app %u (%s), sensor init ok, %u finger(s); db id rc %d status %d %#llx", appid,
	   ta_resident ? "resident" : "loaded now", S.nids, r, st,
	   (unsigned long long)rd_u32(d + 0x10) | (unsigned long long)rd_u32(d + 0x14) << 32);
	lg("selftest: touch the sensor within %d s (one capture)", touch_s);
	long deadline = now_ms() + touch_s * 1000L;
	uint32_t gen = S.gen;
	int touched = 0, rc = 0;
	for (;;) {
		int32_t st4 = 0;
		int c = capture_once(gen, &st4, deadline);
		pthread_mutex_lock(&st_mx);
		touched |= S.finger_on;
		pthread_mutex_unlock(&st_mx);
		if (c == CAP_TIMEOUT || c == CAP_CANCEL) {
			lg("selftest: %s", touched ? "touched, but no good image before the timeout" : "no finger, skipped");
			rc = touched ? 6 : 0;
			break;
		}
		if (c == CAP_ERR) {
			lg("selftest: capture ERROR");
			rc = 2;
			break;
		}
		touched = 1;
		if (st4 == 1 || st4 == 4)
			continue;
		if (st4) {
			lg("selftest: image status %d (%s): touch again", st4, st4 == 3 ? "insufficient" : "error");
			if (st4 < 0) {
				rc = 2;
				break;
			}
			continue;
		}
		lg("selftest: GOOD IMAGE (g10/4 status 0)");
		if (!S.nids) {
			lg("selftest: no finger enrolled: identify skipped");
			break;
		}
		memset(d, 0, sizeof(d));
		pthread_mutex_lock(&ta_mx);
		r = ta_bio(3, d, &st);
		pthread_mutex_unlock(&ta_mx);
		lg("selftest: identify rc %d status %d fid %#x score %d (log only: selftest never reports a match)", r, st,
		   rd_u32(d + 0xc), (int)rd_u32(d + 0x20));
		break;
	}
	lg("selftest: %s (rc %d)", rc ? "FAILED" : "PASS", rc);
	return rc;
}

int main(int argc, char **argv)
{
	clock_gettime(CLOCK_MONOTONIC, &t_start);
	int self = 0, fresh = 0, touch_s = 15;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest")) {
			self = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				touch_s = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--fresh")) {
			fresh = 1;
		} else if (!strcmp(argv[i], "--keep-ta")) {
			keep_ta = 1;
		} else {
			fprintf(stderr, "usage: sweetfpcd [--selftest [SECONDS]] [--fresh] [--keep-ta]\n");
			return 1;
		}
	}
	const char *e;
	if ((e = getenv("FPCD_TA_DIR")) && *e)
		ta_dir = e;
	if ((e = getenv("FPCD_TA_FILE")) && *e)
		ta_file = e;
	if ((e = getenv("FPCD_TA_NAME")) && *e)
		ta_name = e;
	if ((e = getenv("FPCD_OP_TIMEOUT_MS")))
		op_timeout_ms = atol(e);
	keep_ta |= (e = getenv("FPCD_KEEP_TA")) && atoi(e);
	require_hat = (e = getenv("FPCD_REQUIRE_HAT")) && atoi(e);
	verbose = (e = getenv("FPCD_VERBOSE")) && atoi(e);
	if ((e = getenv("FPCD_CONSOLE")) && atoi(e))
		log_con = open("/dev/console", O_WRONLY | O_NOCTTY);
	/* qsee_lib: its say() lines go to stdout -> our log; quiet unless FPCD_VERBOSE (fd_cmd/fpc_bio are quiet anyway) */
	dup2(2, 1);
	say_tag = "sweetfpcd(qsee): ";
	con = log_con >= 0 && verbose ? log_con : -1;
	say_quiet = 0;
	dryrun = 0, shmbridge = 0;

	struct sigaction sa = { 0 };
	sa.sa_handler = on_sig;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	lg("start %s: %s, TA %s/%s (name %s), op timeout %ld ms, allow-match file %s", FPCD_VERSION,
	   self ? "selftest" : "daemon", ta_dir, ta_file, ta_name, op_timeout_ms, ALLOW_MATCH_PATH);
	pm_init();
	if (qsee_lock("sweetfpcd"))
		return 9;
	if (fpc_kernel_ready(!self))
		return g_stop ? 0 : 2;
	if (g_stop)
		return 0;
	if (ta_up(fresh)) {
		lg("TA/sensor setup FAILED");
		if (ta_resident)
			keep_ta = 1;	/* never unload a TA this run did not load, on a failed start */
		ta_down();
		return 2;
	}
	if (self) {
		int rc = selftest(touch_s);
		if (ta_resident)
			keep_ta = 1;	/* a selftest leaves a TA it found loaded */
		ta_down();
		return rc;
	}

	pthread_t th;
	if (pthread_create(&th, NULL, capture_thread, NULL)) {
		lg("pthread_create failed");
		ta_down();
		return 1;
	}
	int said = 0;
	while (!g_stop) {
		if (g_wipe) {
			g_wipe = 0;
			wipe_all("SIGUSR1");
		}
		if (g_dump) {
			g_dump = 0;
			dump_status();
		}
		if (!conn) {
			int r = bridge_open();
			if (r) {
				if (!said++)
					lg("bridge: %s not available (%s); retrying every 5 s", SMB_SERVICE,
					   r == -1 ? "no such service: SweetMesa not booted / kern.sweet_mesa off" : "open failed");
				for (int i = 0; i < 10 && !g_stop && !g_wipe && !g_dump; i++)
					usleep(500000);
				continue;
			}
			said = 0;
		}
		uint64_t in = FPCD_WAIT_REQ_MS;
		size_t osz = sizeof(rq);
		kern_return_t kr = IOConnectCallMethod(conn, SMB_SEL_WAIT, &in, 1, NULL, 0, NULL, NULL, &rq, &osz);
		if (kr == kIOReturnTimeout)
			continue;
		if (kr != KERN_SUCCESS || osz < offsetof(struct smb_req, data)) {
			lg("bridge: WAIT %#x (out %zu bytes): reopening in 1 s", kr, osz);
			bridge_close();
			sleep(1);
			continue;
		}
		dispatch();
	}
	lg("stopping");
	op_cancel_wait();
	pthread_mutex_lock(&st_mx);
	pthread_cond_broadcast(&st_cv);
	pthread_mutex_unlock(&st_mx);
	pthread_join(th, NULL);
	bridge_close();
	ta_down();
	lg("stopped");
	return 0;
}
