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
 * Startup: the QSEE lock (qsee_lib_lock, /var/local/tz/qsee.lock, shared with qseeprobe: the only SMC user); kern.sweet_fpc must have bits
 * 0 (reset) and 1 (irq) set, i.e. value 3 (xnu 1190); lookup "fingerprint": resident -> reuse its id (templates kept),
 * else load DIR/fpctzappfingerprint.mdt+.bNN and load_empty_db; then sensor init (g10/0, g10/5), set_gid 0 (on a
 * resident TA a failing set_gid falls back to load_empty_db + set_gid), get_finger_ids.
 * SIGTERM/SIGINT: cancel the operation, stop the capture thread, unload the TA (FPCD_KEEP_TA=1 / --keep-ta: keep).
 * SIGUSR1: wipe all (delete every finger id, debug).  SIGUSR2: one status line.
 *
 * Operations (one at a time; a new Enroll/Match/Detect cancels the running one) run in the capture thread, driven by
 * gpio112 (kern.sweet_fpc_wait in FPCD_WAIT_SLICE_MS slices so Cancel/SIGTERM are seen): the HAL capture sequence
 * g10/2 arm, wait, g10/1, then g10/3 / wait / g10/4 until not 3; g10/5 on a bad image or a cancel.
 *   enrol : good image -> g11/1 enrol (4 partial / 5 too similar: non-fatal); code 0 -> g11/2 end_enrol. Without the
 *           hw-auth chain (fpcd_enrol_authorize hook) end_enrol gives -1: ENROL_FAILED status FPCD_E_NOTAUTH, flag
 *           FPCD_EF_END_UNAUTH, any stray new id deleted. Events ENROL_PROGRESS / ENROL_DONE / ENROL_FAILED / TEMPLATES
 *   match : good image -> g11/3 identify; status 0 -> match_deliver() (the only producer of a match); else NOMATCH and
 *           the next capture, until a match, Cancel or the op timeout
 *   detect: captures only, for FINGER_DOWN / FINGER_UP events
 * Templates live in TA memory; with xnu 1206 (kern.sweet_mesa_catacomb=1) they persist the iOS way: 61 stores the TA's
 * encrypted template db (FPC db_command (2,12) through an in-memory file listener, qsee_fpc_db_cmd) into a "SWCT"
 * blob that biometrickitd writes into its catacomb file, 64 loads it back ((2,11)). sweetfpcd writes no file.
 *
 * SAFETY: a match is reported only by match_deliver(), only for a TA identify status 0 with a finger id the TA lists,
 * and only while /var/local/fpc/allow-match exists (absent by default) or kern.sweet_mesa_match is 2 (owner's
 * unverified-match mode, xnu 1198). The kernel gates again (kern.sweet_mesa_match)
 * and (xnu 1192d) builds an identity match only when the event carries FPCD_MF_HAT_VERIFIED.
 *
 * Bridge v2 (ABI 2, xnu 1192d): events carry op / percent / hat_status; the kernel turns them into Apple messages.
 * Enrol authorisation (ENROL): before begin_enrol, the FPC hw-auth chain qsee_fpc_authorize_enrol() (phone/qsee/
 * qsee_gk.c, moved from qseeprobe ws-qsee-b e1bf971): KM 0x205 -> (3,5) -> (3,2) challenge -> a credential source
 * gives a gatekeeper HAT for it -> (3,3) authorize_enrol. Credential sources (env FPCD_CRED, pluggable table cred_src[]):
 *   gk-test-file (default, STOP-GAP until iOS passcode support): gatekeeper VERIFY of the private test uid 0x7e000001
 *     (handle from qseeprobe gk-enroll) with the password in /var/local/fpc/gk-test.pw (root-owned, mode 0600, never
 *     logged); kern.sweet_rpmb is set to 2 only around that verify and back to 1, both logged.
 *   none: never authorised (end_enrol refuses; the enrol fails, nothing is kept).
 * Match HAT check (MATCH): (3,1) set_auth_challenge(random) before each identify, (3,4) get_auth_result after status 0,
 * field check (version 0, challenge echoed, type fingerprint, authenticator id = template db id), then a HAT verifier
 * (env FPCD_HAT_VERIFIER, table hat_vfy[]). Only "none" exists: no keymaster begin/finish client is built (the QTI
 * keymaster64 operation protocol is not reversed and its commands are outside qsee_lsnr's whitelist), so
 * FPCD_MF_HAT_VERIFIED is never set and no match ever reaches biometrickitd.
 *
 * Environment: FPCD_TA_DIR (/var/local/fpc/ta), FPCD_TA_FILE (fpctzappfingerprint), FPCD_TA_NAME (fingerprint),
 * FPCD_OP_TIMEOUT_MS (120000, 0 = none), FPCD_CONSOLE=1 (mirror log lines to /dev/console), FPCD_KEEP_TA=1,
 * FPCD_REQUIRE_HAT=1, FPCD_VERBOSE=1 (every SMC line of qsee_lib), QSEE_NOLOCK / QSEE_LOCK as qsee_lib,
 * FPCD_CRED (gk-test-file | none), FPCD_HAT_VERIFIER (none).
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
#include "../qsee/qsee_gk.h"
#include "fpcd_bridge.h"

#ifndef FPCD_VERSION
#define FPCD_VERSION "dev"
#endif
#define ALLOW_MATCH_PATH	"/var/local/fpc/allow-match"
#define FPCD_WAIT_SLICE_MS	250	/* gpio112 wait slice: Cancel / SIGTERM latency */
#define FPCD_UP_MS		150	/* armed and quiet this long after a touch = finger up */
#define FPCD_CAPTURE_WAIT_MS	0	/* max wait after g10/3; 0 = until the op deadline (as qseeprobe / the HAL) */
#define FPCD_WAIT_REQ_MS	500	/* bridge WAIT timeout: signal latency of the main loop */

enum { OP_NONE, OP_ENROL, OP_MATCH, OP_DETECT };
static const char *op_name[] = { "none", "enrol", "match", "detect" };
enum { CAP_OK, CAP_CANCEL, CAP_TIMEOUT, CAP_ERR };

static const char *ta_dir = "/var/local/fpc/ta", *ta_file = "fpctzappfingerprint", *ta_name = "fingerprint";
static uint32_t appid;
static int ta_resident, sensor_ok, keep_ta, require_hat, verbose;
static long op_timeout_ms = 120000;
/* FPCD_BRIDGE_SMALL (default 1): WAIT with a 4096-byte out capacity, REPLY/EVENT with header + used bytes only, so
 * IOKit passes every struct inline (<= 4096) instead of through a memory descriptor (1192's large-struct path). */
static int bridge_small = 1;
#define SMB_SMALL_MAX		4096
#define SMB_REQ_HDR		12
#define SMB_EV_HDR_SZ		8
static volatile sig_atomic_t g_stop, g_wipe, g_dump;

/* ta_mx serialises every qsee_lib call (one SMC user, and say_quiet etc. are globals). st_mx guards S, the id cache
 * and the counters. Order: ta_mx before st_mx, never the reverse. */
static pthread_mutex_t ta_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t st_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t st_cv = PTHREAD_COND_INITIALIZER;
static struct {
	int op, busy, cancel, finger_on;
	int extend;		/* the MatchMode is an extend-enrolment (xnu 1198: request u32 flags bit 0) */
	uint32_t up_seq;	/* finger-up count: an extend match needs a lift between touches */
	uint32_t gen;
	long started;
	uint32_t gid;
	uint32_t pre_ids[5], pre_n;	/* id list before begin_enrol: finds the new id */
	uint32_t ids[5], nids;
	int ids_valid;
	uint32_t captures, good, matches, nomatches, withheld, evseq;
	struct fpcd_event last_match;
	int have_last_match;
	uint64_t dbid;
	int dbid_valid;
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
	case FPCD_CMD_CAT_PUT: return "fpcd:CatPut";	case FPCD_CMD_CAT_GET: return "fpcd:CatGet";
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

static void ev_send2(unsigned code, int32_t status, uint32_t fid, int32_t score, uint32_t remaining, uint32_t samples,
		     uint32_t flags, int with_ids, uint32_t percent, int32_t hat_status)
{
	static struct smb_event e;	/* under ev_mx */
	struct fpcd_event ev = { .magic = FPCD_EV_MAGIC, .version = FPCD_ABI_VERSION, .code = (uint16_t)code,
				 .status = status, .fid = fid, .score = score, .remaining = remaining,
				 .samples = samples, .flags = flags, .percent = percent, .hat_status = hat_status };
	pthread_mutex_lock(&st_mx);
	ev.evseq = ++S.evseq;
	ev.op_gen = S.gen;
	ev.op = (uint32_t)S.op;
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
		kr = IOConnectCallStructMethod(conn, SMB_SEL_EVENT, &e, bridge_small ? SMB_EV_HDR_SZ + e.len : sizeof(e), NULL,
					       NULL);
	}
	pthread_mutex_unlock(&ev_mx);
	lg("event #%u code %u sub %#x: status %d fid %#x score %d remaining %u samples %u flags %#x n %u op %u pct %u hat %d"
	   " -> %s", ev.evseq, code, SMB_MSG_BASE + fpcd_event_subtype(code), status, fid, score, remaining, samples, flags,
	   ev.n, ev.op, percent, hat_status, !conn ? "no bridge (logged only)" : kr ? "EVENT failed" : "sent");
}

static void ev_send(unsigned code, int32_t status, uint32_t fid, int32_t score, uint32_t remaining, uint32_t samples,
		    uint32_t flags, int with_ids)
{
	ev_send2(code, status, fid, score, remaining, samples, flags, with_ids, 0, 0);
}

/* ---------------------------------------------------------------- TA helpers (caller holds ta_mx) */
static void dbid_refresh(void);	/* caller holds ta_mx */

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
	if (st == -12)
		lg("ta: sensor init status -12: kern.sweet_fpc reset not done since boot? (set kern.sweet_fpc=3 first)");
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
	if (was && !on)
		S.up_seq++;
	pthread_mutex_unlock(&st_mx);
	if (was != on)
		ev_send(on ? FPCD_EV_FINGER_DOWN : FPCD_EV_FINGER_UP, 0, 0, 0, 0, 0, 0, 0);
}

/*
 * One good image through qsee_lib's fpc_capture_hal (the HAL loop of qseeprobe finger-detect: cmd 2 arm, wait,
 * cmd 1, up to 4x {cmd 3, wait, cmd 4} while 3; any other non-zero cmd 4 or no irq after cmd 3 -> cmd 5 + re-arm).
 * Finger-down only for a good image or a wake later than FPC_EARLY_WAKE_MS after arming (the 20-33 ms wake right
 * after cmd 2 is not a finger); finger-up after FPCD_UP_MS armed and quiet. Rejected images (2/3) give IMAGE events.
 * Returns CAP_OK (*st4 = 0 good image, or < 0 FPC error), CAP_CANCEL, CAP_TIMEOUT (absolute deadline, 0 = none),
 * CAP_ERR.
 */
struct capctx { uint32_t gen; };
static long cap3_ms = FPCD_CAPTURE_WAIT_MS;	/* env FPCD_CAP3_MS */
static int cap_stop(void *c) { return cancelled(((struct capctx *)c)->gen); }
static void cap_lock(void *c) { pthread_mutex_lock(&ta_mx); }
static void cap_unlock(void *c) { pthread_mutex_unlock(&ta_mx); }
static void cap_wake(void *c, long ms)
{
	if (ms > FPC_EARLY_WAKE_MS)
		finger_set(1);
	else if (verbose)
		lg("capture: early wake %ld ms after arm (not a finger)", ms);
}
static void cap_idle(void *c, long ms)
{
	if (ms >= FPCD_UP_MS)
		finger_set(0);
}
static void cap_retry(void *c, int32_t st4)
{
	pthread_mutex_lock(&st_mx);
	S.captures++;
	int on = S.finger_on;
	pthread_mutex_unlock(&st_mx);
	if (verbose || (st4 != 1 && st4 != 4))
		lg("capture: no good image (cmd 4 status %d%s): cmd 5, re-arm", st4,
		   st4 == 3 ? " insufficient / no irq after cmd 3" : st4 == 2 ? " capture error" : "");
	if (st4 == 2 || (st4 == 3 && on))
		ev_send(FPCD_EV_IMAGE, st4, 0, 0, 0, 0, 0, 0);
}

static int capture_once(uint32_t gen, int32_t *st4, long deadline)
{
	struct capctx cx = { gen };
	struct fpc_hal_cb cb = { .ctx = &cx, .slice_ms = FPCD_WAIT_SLICE_MS, .cap3_ms = cap3_ms,
				 .stop = cap_stop, .lock = cap_lock, .unlock = cap_unlock, .wake = cap_wake,
				 .idle = cap_idle, .retry = cap_retry };
	long rel = 0;
	if (deadline) {
		rel = deadline - now_ms();
		if (rel <= 0)
			return CAP_TIMEOUT;
	}
	int rounds = 0;
	*st4 = 0;
	int r = fpc_capture_hal(appid, rel, &cb, st4, &rounds);
	pthread_mutex_lock(&st_mx);
	if (r == FPC_HAL_GOOD) {
		S.captures++;
		S.good++;
	}
	pthread_mutex_unlock(&st_mx);
	switch (r) {
	case FPC_HAL_GOOD:
		finger_set(1);
		lg("capture: GOOD IMAGE (cmd 4 status 0) after %d round(s)", rounds);
		return CAP_OK;
	case FPC_HAL_FPCERR:
		return CAP_OK;	/* *st4 < 0: op_capture reports it */
	case FPC_HAL_CANCEL:
		return CAP_CANCEL;
	case FPC_HAL_TIMEOUT:
		finger_set(0);
		return CAP_TIMEOUT;
	case FPC_HAL_STORM: {
		/* gpio112 interrupts > FPC_STORM_IRQS/s: the sensor was released (cmd 5); back off before any re-arm.
		 * The capture thread sleeps here, so no op (detect from 69 included) can re-arm meanwhile. */
		static int backoff_s = 0;
		backoff_s = backoff_s ? (backoff_s * 2 > 60 ? 60 : backoff_s * 2) : 5;
		lg("IRQ storm: detection cancelled (cmd 5), backing off %d s", backoff_s);
		ev_send(FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, 0, 0, 0);
		for (int i = 0; i < backoff_s * 4 && !g_stop; i++)
			usleep(250000);
		return CAP_ERR;
	}
	default:
		lg("capture: TZ/TA error (status %d) in round %d", *st4, rounds);
		return CAP_ERR;
	}
}

/*
 * HAT verifiers (env FPCD_HAT_VERIFIER, default "none"). A verifier gets the identify HAT (already field-checked) and
 * returns FPCD_HAT_VERIFIED only if keymaster64 proved the HMAC (e.g. KM begin on a key bound to the FPC authenticator
 * id with per-operation auth, the op handle as the identify challenge, then update/finish with this HAT). No such
 * client exists yet (QTI keymaster64 operation protocol not reversed; its commands are outside qsee_lsnr's whitelist),
 * so the only verifier is "none": FPCD_HAT_NO_VERIFIER, never verified.
 */
typedef int32_t (*hat_verify_fn)(const uint8_t hat[QSEE_HAT_LEN], uint64_t challenge);
static int32_t hat_vfy_none(const uint8_t hat[QSEE_HAT_LEN], uint64_t challenge)
{
	(void)hat, (void)challenge;
	return FPCD_HAT_NO_VERIFIER;
}
static const struct { const char *name; hat_verify_fn fn; } hat_vfy[] = {
	{ "none", hat_vfy_none },
};
static hat_verify_fn hat_verifier = hat_vfy_none;
static const char *hat_verifier_name = "none";

/* the challenge handed to the TA for the running identify (set_auth_challenge), 0 = none */
static uint64_t auth_challenge;

/* (3,1) before an identify: a fresh random challenge (a keymaster verifier would use its op handle). caller: ta_mx */
static void auth_challenge_set(void)
{
	uint64_t ch = 0;
	int32_t st = 0;
	while (!ch)
		arc4random_buf(&ch, sizeof(ch));
	int r = qsee_fpc_set_auth_challenge(appid, ch, &st);
	auth_challenge = !r && !st ? ch : 0;
	if (r || st)
		lg("match: set_auth_challenge (3,1) rc %d status %d: no HAT check possible for this identify", r, st);
}

/*
 * After identify status 0 (caller: ta_mx): (3,4) get_auth_result, field check, verifier. Returns FPCD_HAT_*; the HMAC
 * is never logged.
 */
static int32_t fpcd_verify_hat(uint32_t fid)
{
	uint8_t hat[QSEE_HAT_LEN] = { 0 };
	int32_t st = 0, v;
	struct qsee_hat_info hi;
	if (!auth_challenge)
		return FPCD_HAT_NONE;
	int r = qsee_fpc_get_auth_result(appid, hat, &st);
	if (r || st) {
		lg("match: get_auth_result (3,4) rc %d status %d: no HAT", r, st);
		return FPCD_HAT_NONE;
	}
	qsee_hat_parse(hat, &hi);
	pthread_mutex_lock(&st_mx);
	uint64_t dbid = S.dbid;
	int dbid_ok = S.dbid_valid;
	pthread_mutex_unlock(&st_mx);
	int ok = hi.version == 0 && hi.challenge == auth_challenge && hi.authenticator_type == 2 &&
		 (!dbid_ok || hi.authenticator_id == dbid);
	lg("match: HAT fid %#x version %u challenge %s type %u authenticator id %#llx (db id %#llx%s) SID %#llx -> fields %s",
	   fid, hi.version, hi.challenge == auth_challenge ? "echoed" : "NOT echoed", hi.authenticator_type,
	   (unsigned long long)hi.authenticator_id, (unsigned long long)dbid, dbid_ok ? "" : ", unknown",
	   (unsigned long long)hi.user_id, ok ? "ok" : "WRONG");
	if (!ok)
		v = FPCD_HAT_BAD;
	else
		v = hat_verifier(hat, auth_challenge);
	memset(hat, 0, sizeof(hat));
	auth_challenge = 0;
	lg("match: HAT verifier \"%s\": %s", hat_verifier_name, v == FPCD_HAT_VERIFIED ? "VERIFIED" :
	   v == FPCD_HAT_NO_VERIFIER ? "no keymaster verifier: NOT verified" : "NOT verified");
	return v;
}

/*
 * The ONLY place that produces FPCD_EV_MATCH. Delivered iff: the TA's identify status is 0, the finger id is one the
 * TA lists, /var/local/fpc/allow-match exists, the HAT hook did not refuse (and verified it, with FPCD_REQUIRE_HAT=1).
 * Anything else -> MATCH_WITHHELD (never a match). Returns 1 iff a match was sent.
 */
static int match_deliver(int32_t ta_status, uint32_t fid, int32_t score, uint32_t attempts, int32_t hat)
{
	struct stat sb;
	uint32_t why = 0;
	if (ta_status != 0)
		return 0;
	if (!fid_listed(fid))
		why = FPCD_MW_UNKNOWN_FID;
	else if (stat(ALLOW_MATCH_PATH, &sb) && fpc_sysctl_u32("kern.sweet_mesa_match") != 2)
		why = FPCD_MW_NO_ALLOW_FILE;
	else if (fpc_sysctl_u32("kern.sweet_mesa_match") == 2 && !require_hat)
		why = 0;	/* owner's unverified-match mode: the HAT field check is informational only */
	else if (hat == FPCD_HAT_BAD || hat == FPCD_HAT_REFUSED || (require_hat && hat != FPCD_HAT_VERIFIED))
		why = FPCD_MW_HAT_REFUSED;
	if (why) {
		pthread_mutex_lock(&st_mx);
		S.withheld++;
		pthread_mutex_unlock(&st_mx);
		lg("match: identify status 0 fid %#x score %d WITHHELD (%s)", fid, score,
		   why == FPCD_MW_UNKNOWN_FID ? "fid not in the TA list" :
		   why == FPCD_MW_NO_ALLOW_FILE ? ALLOW_MATCH_PATH " absent" : "HAT not verified");
		ev_send2(FPCD_EV_MATCH_WITHHELD, ta_status, fid, score, 0, attempts, why, 0, 0, hat);
		return 0;
	}
	pthread_mutex_lock(&st_mx);
	S.matches++;
	pthread_mutex_unlock(&st_mx);
	lg("match: fid %#x score %d attempts %u, HAT %s", fid, score, attempts,
	   hat == FPCD_HAT_VERIFIED ? "verified" : "NOT verified (the kernel will not deliver it as a match)");
	ev_send2(FPCD_EV_MATCH, 0, fid, score, 0, attempts, hat == FPCD_HAT_VERIFIED ? FPCD_MF_HAT_VERIFIED : 0, 0, 0, hat);
	return 1;
}

static long op_deadline(void)
{
	pthread_mutex_lock(&st_mx);
	/* background finger detect (38/69) runs until switched off: no op timeout (it ended detection after 120 s) */
	long d = op_timeout_ms > 0 && S.op != OP_DETECT ? S.started + op_timeout_ms : 0;
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
		if (st == 0 || st == 1)
			samples++;	/* 4 partial, 5 too similar, 6 dirty: non-fatal, sample not taken; touch again */
		uint32_t pct = st == 0 ? 100 : (samples + rem) ? (uint32_t)samples * 100 / ((uint32_t)samples + rem) : 0;
		if (pct > 100)
			pct = 100;
		lg("enrol: code %d (%s), remaining %u, accepted %d, %u%%", st, enrol_code(st), rem, samples, pct);
		ev_send2(FPCD_EV_ENROL_PROGRESS, st, 0, 0, rem, (uint32_t)samples, 0, 0, pct, 0);
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
		dbid_refresh();	/* ws-fpcd-b: the db id changes with the db */
		pthread_mutex_unlock(&ta_mx);
		if (r || st2) {
			/* no hw-auth chain yet (fpcd_enrol_authorize): end_enrol refuses with -1 = NOT AUTHORIZED. Any id
			 * the TA nevertheless lists that was not there before this enrol is deleted again, so no
			 * unauthorised template stays behind (and none can ever match). */
			uint32_t stray[5], ns = 0;
			pthread_mutex_lock(&st_mx);
			for (uint32_t i = 0; i < S.nids && i < 5; i++) {
				int old = 0;
				for (uint32_t j = 0; j < S.pre_n; j++)
					old |= S.pre_ids[j] == S.ids[i];
				if (!old)
					stray[ns++] = S.ids[i];
			}
			pthread_mutex_unlock(&st_mx);
			pthread_mutex_lock(&ta_mx);
			for (uint32_t i = 0; i < ns; i++)
				lg("enrol: deleting unauthorised id %#x: delete rc %d", stray[i], fpc_delete(appid, stray[i]));
			ids_refresh();
			pthread_mutex_unlock(&ta_mx);
			lg("enrol: FAILED: NOT AUTHORIZED (end_enrol rc %d status %d; the hw-auth chain did not authorise this "
			   "enrol: see the \"enrol: authorisation\" line)", r, st2);
			ev_send(FPCD_EV_ENROL_FAILED, FPCD_E_NOTAUTH, 0, 0, 0, (uint32_t)samples, FPCD_EF_END_UNAUTH, 1);
			ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
			return;
		}
		lg("enrol: end_enrol ok, finger id %#x", fid);
		ev_send(fid ? FPCD_EV_ENROL_DONE : FPCD_EV_ENROL_FAILED, st2, fid, 0, 0, (uint32_t)samples, flags, 1);
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
		return;
	}
}

static void run_match(uint32_t gen)
{
	uint32_t attempts = 0, last_up = 0;
	int have_last = 0;
	while (!cancelled(gen)) {
		int32_t st4 = 0, st = 0;
		if (!op_capture(gen, &st4))
			return;
		if (st4)
			continue;
		pthread_mutex_lock(&st_mx);
		int ext = S.extend, same_touch = ext && have_last && S.up_seq == last_up;
		pthread_mutex_unlock(&st_mx);
		if (same_touch)
			continue;	/* extend-enrolment: one match per touch; lift first */
		uint8_t d[FPC_SENSOR_LEN] = { 0 };
		int32_t hat = FPCD_HAT_NONE;
		pthread_mutex_lock(&ta_mx);
		auth_challenge_set();
		int r = ta_bio(3, d, &st);
		if (!r && st == 0)
			hat = fpcd_verify_hat(rd_u32(d + 0xc));
		auth_challenge = 0;
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
			int sent = match_deliver(st, fid, score, attempts, hat);
			if (!ext)
				return;	/* one result per MatchMode: delivered or withheld */
			/* extend-enrolment (Settings phase 2): add this touch to the template (g11/4 update_template, as
			 * the HAL does after an authenticate), then keep matching, one result per touch, until Cancel */
			uint8_t u[FPC_SENSOR_LEN] = { 0 };
			int32_t ust = 0;
			pthread_mutex_lock(&ta_mx);
			int ur = ta_bio(4, u, &ust);
			if (!ur && !ust)
				dbid_refresh();
			pthread_mutex_unlock(&ta_mx);
			if (!ur && !ust)	/* ws-fpcd-b: the template changed: the kernel marks the catacomb dirty */
				ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0, 1);
			pthread_mutex_lock(&st_mx);
			last_up = S.up_seq;
			pthread_mutex_unlock(&st_mx);
			have_last = 1;
			lg("extend-enrolment: match %s, update_template rc %d status %d; next touch after a lift",
			   sent ? "sent" : "withheld", ur, ust);
			continue;
		}
		if (ext && !cancelled(gen)) {
			/*
			 * extend-enrolment (Settings phase 2, owner-approved "enable matching"): an edge capture of the finger
			 * just enrolled need not identify against the centre template. Every good image counts as a touch of
			 * the identity being extended (the only one the TA holds), as the SEP's extend match does.
			 */
			uint32_t efid = 0, n;
			pthread_mutex_lock(&st_mx);
			n = S.ids_valid ? S.nids : 0;
			if (n == 1)
				efid = S.ids[0];
			last_up = S.up_seq;
			pthread_mutex_unlock(&st_mx);
			have_last = 1;
			if (efid) {
				int sent = match_deliver(0, efid, score, attempts, hat);
				lg("extend-enrolment touch %u: identify status %d (%s) -> reported as fid %#x: %s", attempts, st,
				   st == -8 ? "NOENTITY" : st == -6 ? "IO" : "no match", efid, sent ? "sent" : "withheld");
				continue;
			}
			lg("extend-enrolment touch %u: identify status %d and the TA holds %u ids: no match", attempts, st, n);
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
		finger_set(0);	/* an op never ends with a finger "on" that no later wake would clear */
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

/*
 * The bridge thread never waits: no TA call (ta_mx), no capture, no op_cancel_wait. Commands that need the TA are
 * queued here and run by ta_worker; ops (enrol/match/detect) are switched by changing S.op/S.gen, which the capture
 * thread sees within one wait slice. Results come back as EVENTs.
 */
static void op_set_nowait(int op)	/* op NONE = cancel; the capture thread notices the new gen */
{
	pthread_mutex_lock(&st_mx);
	S.op = op;
	S.gen++;
	S.started = now_ms();
	pthread_cond_broadcast(&st_cv);
	pthread_mutex_unlock(&st_mx);
}

enum { JOB_RESET = 1, JOB_ENROL, JOB_DELETE, JOB_WIPE, JOB_NOCATACOMB };
static const char *job_name[] = { "?", "reset", "enrol-begin", "delete", "wipe-all", "no-catacomb" };
struct job { int kind; uint32_t arg, gen; char why[40]; };
#define JQ_N 16
static struct job jq[JQ_N];
static unsigned jq_head, jq_tail;	/* under st_mx */
static pthread_cond_t jq_cv = PTHREAD_COND_INITIALIZER;

static int job_post(int kind, uint32_t arg, const char *why)
{
	pthread_mutex_lock(&st_mx);
	if (jq_tail - jq_head >= JQ_N) {
		pthread_mutex_unlock(&st_mx);
		lg("job %s (%s): queue full: refused", job_name[kind], why);
		return FPCD_E_BUSY;
	}
	struct job *j = &jq[jq_tail++ % JQ_N];
	j->kind = kind, j->arg = arg, j->gen = S.gen;
	snprintf(j->why, sizeof(j->why), "%s", why);
	pthread_cond_broadcast(&jq_cv);
	pthread_mutex_unlock(&st_mx);
	return FPCD_OK;
}

static void dbid_refresh(void)	/* caller holds ta_mx */
{
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	int32_t st = 0;
	int r = ta_bio(10, d, &st);
	pthread_mutex_lock(&st_mx);
	S.dbid_valid = !r && !st;
	S.dbid = (uint64_t)rd_u32(d + 0x10) | (uint64_t)rd_u32(d + 0x14) << 32;
	pthread_mutex_unlock(&st_mx);
}

/* debug "wipe all": delete every id the TA lists; 0 iff the list ends empty (worker / selftest / shutdown only) */
static int wipe_all(const char *why)
{
	pthread_mutex_lock(&ta_mx);
	int r = fpc_cleanup_all(appid, "wipe");
	ids_refresh();
	dbid_refresh();
	pthread_mutex_unlock(&ta_mx);
	lg("wipe all (%s): %s", why, r ? "FAILED, ids left" : "OK, no finger left");
	ev_send(FPCD_EV_TEMPLATES, r ? FPCD_E_IO : 0, 0, 0, 0, 0, 0, 1);
	return r ? FPCD_E_IO : FPCD_OK;
}

static int fpcd_enrol_authorize(uint32_t gid);

static void job_run(const struct job *j)
{
	long t0 = now_ms();
	int32_t st = 0, st2 = 0;
	int r = 0;
	uint8_t d[FPC_SENSOR_LEN] = { 0 };
	switch (j->kind) {
	case JOB_RESET:
		pthread_mutex_lock(&ta_mx);
		r = ta_sensor_init();
		pthread_mutex_unlock(&ta_mx);
		if (r)
			ev_send(FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, 0, 0, 0);
		break;
	case JOB_ENROL: {
		uint32_t gid = j->arg;
		pthread_mutex_lock(&ta_mx);
		ids_refresh();
		r = ta_set_gid(gid, &st);
		if (!r && !st) {
			fpcd_enrol_authorize(gid);	/* 1 = not authorised: see the hook */
			r = ta_bio(0, d, &st);	/* begin_enrol */
		}
		pthread_mutex_unlock(&ta_mx);
		lg("enrol: gid %u, set_gid/begin_enrol rc %d status %d", gid, r, st);
		if (r || st) {
			ev_send(FPCD_EV_ENROL_FAILED, r ? FPCD_E_IO : st, 0, 0, 0, 0, 0, 0);
			break;
		}
		pthread_mutex_lock(&st_mx);
		if (S.gen != j->gen) {	/* Cancel or another mode arrived meanwhile */
			pthread_mutex_unlock(&st_mx);
			lg("enrol: superseded (gen %u -> %u): not started", j->gen, S.gen);
			break;
		}
		S.gid = gid;
		memcpy(S.pre_ids, S.ids, sizeof(S.pre_ids));
		S.pre_n = S.ids_valid ? S.nids : 0;
		S.op = OP_ENROL;
		S.gen++;
		S.started = now_ms();
		pthread_cond_broadcast(&st_cv);
		pthread_mutex_unlock(&st_mx);
		break;
	}
	case JOB_DELETE: {
		uint32_t fid = j->arg;
		pthread_mutex_lock(&ta_mx);
		ids_refresh();
		int listed = fid_listed(fid);
		r = listed ? fpc_delete(appid, fid) : 0;
		ids_refresh();
		dbid_refresh();
		int still = fid_listed(fid);
		pthread_mutex_unlock(&ta_mx);
		lg("remove %#x: %s, delete rc %d, %s", fid, listed ? "listed" : "NOT listed", r, still ? "STILL LISTED" : "gone");
		ev_send(FPCD_EV_TEMPLATES, !listed ? FPCD_E_NOENT : still ? FPCD_E_IO : 0, fid, 0, 0, 0, 0, 1);
		break;
	}
	case JOB_WIPE:
		wipe_all(j->why);
		break;
	case JOB_NOCATACOMB:
		pthread_mutex_lock(&ta_mx);
		r = ta_load_empty_db(&st);
		if (!r && !st)
			r = ta_set_gid(0, &st2);
		ids_refresh();
		dbid_refresh();
		pthread_mutex_unlock(&ta_mx);
		lg("NoCatacomb: load_empty_db status %d, set_gid 0 status %d", st, st2);
		ev_send(FPCD_EV_TEMPLATES, r || st || st2 ? FPCD_E_IO : 0, 0, 0, 0, 0, 0, 1);
		break;
	}
	lg("job %s (%s): done in %ld ms", job_name[j->kind], j->why, now_ms() - t0);
}

static void *ta_worker(void *arg)
{
	(void)arg;
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	for (;;) {
		pthread_mutex_lock(&st_mx);
		while (!g_stop && jq_head == jq_tail)
			pthread_cond_wait(&jq_cv, &st_mx);
		if (g_stop) {
			pthread_mutex_unlock(&st_mx);
			return NULL;
		}
		struct job j = jq[jq_head++ % JQ_N];
		pthread_mutex_unlock(&st_mx);
		job_run(&j);
	}
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

static int h_proto(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, 2); }
static int h_ready(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, appid && sensor_ok); }
static int h_maxids(const struct smb_req *q, struct smb_reply *p) { return rep_u32(p, FPC_MAX_FINGERS); }
static int h_unsupported(const struct smb_req *q, struct smb_reply *p) { return FPCD_E_UNSUPPORTED; }

static int h_reset(const struct smb_req *q, struct smb_reply *p)
{
	op_set_nowait(OP_NONE);
	return job_post(JOB_RESET, 0, "ResetSensor");
}

static int h_ids(const struct smb_req *q, struct smb_reply *p)
{
	struct fpcd_ids ids = { 0 };	/* the cache: refreshed by every TA change (worker / capture thread) */
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
	pthread_mutex_lock(&st_mx);
	int ok = S.dbid_valid;
	uint64_t id = S.dbid;
	pthread_mutex_unlock(&st_mx);
	if (!ok)
		return FPCD_E_NOTREADY;
	memcpy(p->data, &id, 8);
	p->len = 8;
	return FPCD_OK;
}

/*
 * Credential sources for the enrol authorisation (env FPCD_CRED). Each gets the keymaster64 app id and the FPC enrol
 * challenge inside qsee_fpc_authorize_enrol()'s listener window and must return 0 with a gatekeeper HAT for exactly
 * that challenge. Add the iOS passcode source (SweetKeyStore) here when it exists; gk-test-file is a stop-gap.
 */
#define GK_TEST_PW_PATH		"/var/local/fpc/gk-test.pw"
#define RPMB_SYSCTL		"kern.sweet_rpmb"

static int rpmb_mode_set(int v, const char *why)
{
	int old = -1;
	size_t sz = sizeof(old);
	sysctlbyname(RPMB_SYSCTL, &old, &sz, NULL, 0);
	int r = sysctlbyname(RPMB_SYSCTL, NULL, NULL, &v, sizeof(v));
	lg("%s %d -> %d (%s): %s", RPMB_SYSCTL, old, v, why, r ? strerror(errno) : "ok");
	return r ? -1 : 0;
}

/* the test password: a root-owned regular file, mode exactly 0600; 8..63 chars; the content is never logged */
static int gk_test_pw_read(char *pw, size_t cap)
{
	struct stat sb;
	if (lstat(GK_TEST_PW_PATH, &sb)) {
		lg("cred gk-test-file: %s: %s", GK_TEST_PW_PATH, strerror(errno));
		return -1;
	}
	if (!S_ISREG(sb.st_mode) || sb.st_uid != 0 || (sb.st_mode & 07777) != 0600) {
		lg("cred gk-test-file: %s refused (must be a root-owned regular file, mode 0600; is uid %u mode %o)",
		   GK_TEST_PW_PATH, (unsigned)sb.st_uid, (unsigned)(sb.st_mode & 07777));
		return -1;
	}
	int fd = open(GK_TEST_PW_PATH, O_RDONLY | O_NOFOLLOW);
	ssize_t n = fd >= 0 ? read(fd, pw, cap - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0) {
		lg("cred gk-test-file: %s unreadable", GK_TEST_PW_PATH);
		return -1;
	}
	pw[n] = 0;
	pw[strcspn(pw, "\r\n")] = 0;
	size_t len = strlen(pw);
	if (len < 8 || len > 63) {
		memset(pw, 0, cap);
		lg("cred gk-test-file: password length out of range (8..63)");
		return -1;
	}
	return 0;
}

static int cred_gk_test_file(void *ctx, uint32_t km, uint64_t challenge, uint8_t hat[QSEE_HAT_LEN])
{
	(void)ctx;
	char pw[80];
	uint32_t hl = 0;
	if (gk_test_pw_read(pw, sizeof(pw)))
		return -1;
	if (rpmb_mode_set(2, "gatekeeper verify of the test uid 0x7e000001 only")) {
		memset(pw, 0, sizeof(pw));
		rpmb_mode_set(1, "restore after a failed switch");
		return -1;
	}
	int r = qsee_gk_verify(km, challenge, pw, hat, &hl);
	memset(pw, 0, sizeof(pw));
	rpmb_mode_set(1, "gatekeeper verify done");
	lg("cred gk-test-file: gatekeeper verify uid %#x challenge %#llx: %s (HAT %u bytes, not shown)", QSEE_GK_UID,
	   (unsigned long long)challenge, !r && hl == QSEE_HAT_LEN ? "ok" : "FAILED", hl);
	return !r && hl == QSEE_HAT_LEN ? 0 : -1;
}

static int cred_none(void *ctx, uint32_t km, uint64_t challenge, uint8_t hat[QSEE_HAT_LEN])
{
	(void)ctx, (void)km, (void)challenge, (void)hat;
	lg("cred none: no credential source configured: enrol not authorised");
	return -1;
}

static const struct { const char *name; qsee_hat_source_fn fn; } cred_src[] = {
	{ "gk-test-file", cred_gk_test_file },	/* stop-gap: test uid 0x7e000001 + GK_TEST_PW_PATH */
	{ "none", cred_none },
	/* { "passcode", cred_passcode }: the iOS passcode via SweetKeyStore (not written yet) */
};
static qsee_hat_source_fn cred_fn = cred_gk_test_file;
static const char *cred_name = "gk-test-file";

/*
 * Enrol authorisation (caller holds ta_mx; the FPC TA is loaded): the hw-auth chain with the configured credential
 * source. 0 = authorised (end_enrol will commit the template); else the enrol still runs (samples, progress) but
 * end_enrol refuses with -1 and the enrol ends with ENROL_FAILED FPCD_E_NOTAUTH; nothing is kept.
 */
static int fpcd_enrol_authorize(uint32_t gid)
{
	int32_t st = 0;
	long t0 = now_ms();
	int r = qsee_fpc_authorize_enrol(appid, cred_fn, NULL, &st);
	lg("enrol: authorisation gid %u via \"%s\": %s (step %d, status %d, %ld ms)", gid, cred_name,
	   r ? "NOT authorised (end_enrol will refuse)" : "authorised", r, st, now_ms() - t0);
	return r ? 1 : 0;
}

static int h_enrol(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t gid = arg_u32(q, 0);
	op_set_nowait(OP_NONE);	/* stop what runs; the worker begins the enrol and starts the op */
	return job_post(JOB_ENROL, gid, "EnrollMode");	/* failures: ENROL_FAILED event */
}

static int h_enrol_continue(const struct smb_req *q, struct smb_reply *p)
{
	pthread_mutex_lock(&st_mx);
	int op = S.op;
	pthread_mutex_unlock(&st_mx);
	if (op != OP_ENROL)
		lg("EnrollContinue with no enrol running (late after the last sample): acknowledged");
	return FPCD_OK;	/* libBKDM1 sends one after every progress status; the capture thread continues by itself */
}

static int h_match(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t fl = arg_u32(q, 0);	/* xnu 1198: bit 0 = extend-enrolment */
	pthread_mutex_lock(&st_mx);
	S.extend = fl & 1;
	pthread_mutex_unlock(&st_mx);
	lg("MatchMode: %s", fl & 1 ? "extend-enrolment (one match per touch until Cancel)" : "normal");
	pthread_mutex_lock(&st_mx);
	uint32_t n = S.nids;
	pthread_mutex_unlock(&st_mx);
	if (!n)
		lg("match: no finger enrolled in the TA: every identify will fail");
	return (op_set_nowait(OP_MATCH), FPCD_OK);
}

static int h_cancel(const struct smb_req *q, struct smb_reply *p)
{
	op_set_nowait(OP_NONE);
	return FPCD_OK;
}

static int h_fdet(const struct smb_req *q, struct smb_reply *p)
{
	/* 38 (enable = 1) / 69 (enable = bm_cmd value): set the mode and reply at once; detection runs on the capture
	 * thread and reports by EVENT */
	uint32_t en = arg_u32(q, 1);
	pthread_mutex_lock(&st_mx);
	int op = S.op;
	pthread_mutex_unlock(&st_mx);
	lg("finger detect %s (op %s)", en ? "on" : "off", op_name[op]);
	if (!en) {
		if (op == OP_DETECT)
			op_set_nowait(OP_NONE);
	} else if (op == OP_NONE) {
		op_set_nowait(OP_DETECT);
	}
	return FPCD_OK;	/* enrol / match running: they report finger down/up already */
}

static int h_remove(const struct smb_req *q, struct smb_reply *p)
{
	/*
	 * Kernel-forwarded form: Apple's raw 20 bytes {u32 user, 16-byte identity uuid}; a uuid the kernel reported
	 * is SMB_UUID(fid) = {le32 'SMFP', le32 fid, 0, 0}, so fid = le32 at data[8]. A 20-byte request is never "all".
	 * Private form (len 4): u32 fid, FPCD_FID_ALL = wipe all. Any other length: refused (no implicit "all").
	 */
	uint32_t fid;
	if (q->len == 20) {
		uint32_t user = rd_u32(q->data), magic = rd_u32(q->data + 4);
		fid = rd_u32(q->data + 8);
		if (magic != SMB_UUID_MAGIC || rd_u32(q->data + 12) || rd_u32(q->data + 16)) {
			lg("remove: user %u uuid %08x-%08x-%08x-%08x is not an SMFP uuid: refused", user, magic, fid,
			   rd_u32(q->data + 12), rd_u32(q->data + 16));
			return FPCD_E_NOENT;
		}
		lg("remove: user %u uuid SMFP fid %#x", user, fid);
	} else if (q->len == 4) {
		fid = rd_u32(q->data);
		if (fid == FPCD_FID_ALL)
			return job_post(JOB_WIPE, 0, "RemoveTemplate all (u32)");
	} else {
		lg("remove: payload len %u (want 20, or 4 private): refused", q->len);
		return FPCD_E_INVAL;
	}
	if (!fid_listed(fid)) {
		lg("remove %#x: not in the id cache", fid);
		return FPCD_E_NOENT;
	}
	return job_post(JOB_DELETE, fid, "RemoveTemplate");	/* result: TEMPLATES event (status, fid) */
}

static int h_remove_all(const struct smb_req *q, struct smb_reply *p)
{
	return job_post(JOB_WIPE, 0, "RemoveUserData");
}

static int h_nocatacomb(const struct smb_req *q, struct smb_reply *p)
{
	/*
	 * 2026-11-18 (r5tid3): biometrickitd sends NoCatacomb right after every enrol (it cannot save a catacomb:
	 * 61-63 unsupported), and load_empty_db here WIPED the template just enrolled, so phase 2 identified against
	 * an empty db (-8 FPC_ERROR_NOENTITY / -6 FPC_ERROR_IO alternating). Templates live in TA memory for the life
	 * of the TA load (empty db loaded at TA start), so NoCatacomb is only acknowledged when the TA holds any id.
	 */
	if (q->len >= 4 && rd_u32(q->data) == 0xffffffffu) {
		/* ws-fpcd-b (kernel catacomb mode passes the user): the master component holds no templates here */
		lg("NoCatacomb: master component: acknowledged, TA untouched");
		return FPCD_OK;
	}
	pthread_mutex_lock(&st_mx);
	uint32_t n = S.ids_valid ? S.nids : 0;
	pthread_mutex_unlock(&st_mx);
	if (n) {
		lg("NoCatacomb: %u template(s) in TA memory kept (no db reload)", n);
		return FPCD_OK;
	}
	return job_post(JOB_NOCATACOMB, 0, "NoCatacomb");
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

static int h_wipe(const struct smb_req *q, struct smb_reply *p) { return job_post(JOB_WIPE, 0, "bridge WipeAll"); }

static int h_ping(const struct smb_req *q, struct smb_reply *p)
{
	p->len = (uint32_t)snprintf((char *)p->data, 64, "sweetfpcd %s", FPCD_VERSION);
	return FPCD_OK;
}

/* ---------------------------------------------------------------- catacomb (xnu 1206) */
#define CAT_DIR		"/data/vendor_de/0/fpdata"	/* the stock HAL's path; served from memory, never a file */
#define CAT_DB		"user.db"
static struct qsee_vfs cat_vfs;		/* the TA's fpdata directory, kept in memory between store / load */
static int cat_vfs_ready;
static uint8_t *cat_out;		/* blob staged by 61 for the kernel's CAT_GET pulls */
static uint32_t cat_out_len;
static uint8_t *cat_in;			/* blob being pushed by CAT_PUT for 64 */
static uint32_t cat_in_total, cat_in_have;

static uint32_t crc32_le(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

static void cat_free(uint8_t **b, uint32_t len)
{
	if (*b)
		memset(*b, 0, len), free(*b);
	*b = NULL;
}

static void cat_vfs_setup(void)
{
	if (!cat_vfs_ready) {
		qsee_vfs_init(&cat_vfs, CAT_DIR, FPCD_CAT_MAX - 4096);
		cat_vfs_ready = 1;
	}
}

/* wait (<= 3 s) until the TA worker has run every queued job (a queued NoCatacomb must not run after a load) */
static void jobs_drain(void)
{
	for (int i = 0; i < 300; i++) {
		pthread_mutex_lock(&st_mx);
		int empty = jq_head == jq_tail;
		pthread_mutex_unlock(&st_mx);
		if (empty)
			break;
		usleep(10000);
	}
	pthread_mutex_lock(&ta_mx);	/* the job taken last has finished when the TA mutex is free */
	pthread_mutex_unlock(&ta_mx);
}

static int h_cat_prepare(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t user = q->len >= 4 ? rd_u32(q->data) : 0;
	int32_t st = 0;
	long t0 = now_ms();
	if (!appid)
		return FPCD_E_NOTREADY;
	cat_vfs_setup();
	cat_free(&cat_out, cat_out_len);
	cat_out_len = 0;
	pthread_mutex_lock(&ta_mx);
	int r = qsee_fpc_db_cmd(appid, QSEE_FPC_DB_STORE, CAT_DIR "/" CAT_DB ".bak", &cat_vfs, &st);
	if (!r && !st && qsee_vfs_get(&cat_vfs, CAT_DB ".bak"))
		qsee_vfs_rename(&cat_vfs, CAT_DB ".bak", CAT_DB);	/* the HAL renames <db>.bak to <db> */
	ids_refresh();
	dbid_refresh();
	pthread_mutex_unlock(&ta_mx);
	if (r || st) {
		lg("catacomb: store for user %u FAILED (rc %d TA status %d)", user, r, st);
		put_u32(p->data, (uint32_t)st), p->len = 4;
		return FPCD_E_IO;
	}
	struct swct_hdr h;
	memset(&h, 0, sizeof(h));
	h.magic = SWCT_MAGIC, h.version = SWCT_VERSION, h.hdr_size = sizeof(h), h.user = user;
	snprintf(h.ta, sizeof(h.ta), "%s", ta_name);
	pthread_mutex_lock(&st_mx);
	h.gid = S.gid, h.dbid = S.dbid_valid ? S.dbid : 0, h.nfids = S.ids_valid ? S.nids : 0;
	memcpy(h.fids, S.ids, sizeof(h.fids));
	pthread_mutex_unlock(&st_mx);
	uint64_t total = sizeof(h);
	for (int i = 0; i < QSEE_VFS_FILES; i++)
		if (cat_vfs.f[i].name[0]) {
			h.nfiles++;
			total += 8 + strlen(cat_vfs.f[i].name) + cat_vfs.f[i].len;
		}
	if (!qsee_vfs_get(&cat_vfs, CAT_DB) || total > FPCD_CAT_MAX) {
		lg("catacomb: store gave %s (%llu bytes): refused", qsee_vfs_get(&cat_vfs, CAT_DB) ? "too much" : "no " CAT_DB,
		   (unsigned long long)total);
		return FPCD_E_IO;
	}
	uint8_t *b = calloc(1, (size_t)total);
	if (!b)
		return FPCD_E_IO;
	size_t o = sizeof(h);
	for (int i = 0; i < QSEE_VFS_FILES; i++) {
		struct qsee_vfile *f = &cat_vfs.f[i];
		if (!f->name[0])
			continue;
		uint32_t nl = (uint32_t)strlen(f->name);
		put_u32(b + o, nl), put_u32(b + o + 4, f->len);
		memcpy(b + o + 8, f->name, nl);
		if (f->len)
			memcpy(b + o + 8 + nl, f->data, f->len);
		o += 8 + nl + f->len;
	}
	h.total = (uint32_t)total;
	memcpy(b, &h, sizeof(h));
	h.crc = crc32_le(b + 16, (size_t)total - 16);
	memcpy(b, &h, sizeof(h));
	cat_out = b, cat_out_len = (uint32_t)total;
	char fl[64] = "";
	for (uint32_t i = 0; i < h.nfids && i < 5; i++)
		snprintf(fl + strlen(fl), sizeof(fl) - strlen(fl), " %#x", h.fids[i]);
	lg("catacomb: stored for user %u: %u file(s), db %u bytes, blob %u bytes, crc %08x, db id %#llx, fids:%s (%ld ms)",
	   user, h.nfiles, qsee_vfs_get(&cat_vfs, CAT_DB)->len, cat_out_len, h.crc, (unsigned long long)h.dbid,
	   h.nfids ? fl : " none", now_ms() - t0);
	return rep_u32(p, cat_out_len);
}

static int h_cat_get(const struct smb_req *q, struct smb_reply *p)
{
	if (q->len < 8 || !cat_out)
		return FPCD_E_INVAL;
	uint32_t off = rd_u32(q->data), max = rd_u32(q->data + 4);
	if (off >= cat_out_len)
		return FPCD_E_INVAL;
	uint32_t n = cat_out_len - off;
	if (n > max)
		n = max;
	if (n > SMB_SMALL_MAX - SMB_REQ_HDR)
		n = SMB_SMALL_MAX - SMB_REQ_HDR;
	memcpy(p->data, cat_out + off, n);
	p->len = n;
	return FPCD_OK;
}

static int h_cat_confirm(const struct smb_req *q, struct smb_reply *p)
{
	lg("catacomb: confirmed (biometrickitd wrote %u bytes into its catacomb file)", cat_out_len);
	cat_free(&cat_out, cat_out_len);
	cat_out_len = 0;
	return FPCD_OK;
}

static int h_cat_put(const struct smb_req *q, struct smb_reply *p)
{
	if (q->len < 8)
		return FPCD_E_INVAL;
	uint32_t total = rd_u32(q->data), off = rd_u32(q->data + 4), n = q->len - 8;
	if (!total || total > FPCD_CAT_MAX || off > total || n > total - off)
		return FPCD_E_INVAL;
	if (off == 0) {
		cat_free(&cat_in, cat_in_total);
		cat_in = calloc(1, total);
		if (!cat_in)
			return FPCD_E_IO;
		cat_in_total = total, cat_in_have = 0;
	}
	if (!cat_in || total != cat_in_total || off != cat_in_have)
		return FPCD_E_INVAL;
	memcpy(cat_in + off, q->data + 8, n);
	cat_in_have += n;
	return FPCD_OK;
}

static int h_cat_load(const struct smb_req *q, struct smb_reply *p)
{
	long t0 = now_ms();
	if (q->len < 12 || rd_u32(q->data) != FPCD_CAT_LOAD_MAGIC) {
		lg("catacomb: LoadCatacomb without the kernel's staged form (len %u): unsupported", q->len);
		return FPCD_E_UNSUPPORTED;
	}
	uint32_t total = rd_u32(q->data + 4), user = rd_u32(q->data + 8);
	if (!appid)
		return FPCD_E_NOTREADY;
	if (!cat_in || cat_in_total != total || cat_in_have != total || total < sizeof(struct swct_hdr)) {
		lg("catacomb: load: staged %u of %u bytes (want %u): refused", cat_in_have, cat_in_total, total);
		return FPCD_E_INVAL;
	}
	struct swct_hdr h;
	memcpy(&h, cat_in, sizeof(h));
	int bad = h.magic != SWCT_MAGIC ? 1 : h.version != SWCT_VERSION ? 2 : h.hdr_size != sizeof(h) ? 3 :
		  h.total != total ? 4 : crc32_le(cat_in + 16, total - 16) != h.crc ? 5 : h.nfids > 5 ? 6 :
		  h.nfiles == 0 || h.nfiles > QSEE_VFS_FILES ? 7 : 0;
	if (bad) {
		lg("catacomb: load: blob of %u bytes is not a valid SWCT v1 image (check %d): refused, TA untouched", total, bad);
		cat_free(&cat_in, cat_in_total);
		return FPCD_E_INVAL;
	}
	if (h.user != user)
		lg("catacomb: load: blob user %u, kernel user %u (loading anyway: one user)", h.user, user);
	cat_vfs_setup();
	qsee_vfs_clear(&cat_vfs);
	size_t o = sizeof(h);
	for (uint32_t i = 0; i < h.nfiles; i++) {
		char nm[QSEE_VFS_NAME];
		if (o + 8 > total)
			bad = 8;
		uint32_t nl = bad ? 0 : rd_u32(cat_in + o), dl = bad ? 0 : rd_u32(cat_in + o + 4);
		if (!bad && (nl == 0 || nl >= sizeof(nm) || (uint64_t)o + 8 + nl + dl > total))
			bad = 9;
		if (bad)
			break;
		memcpy(nm, cat_in + o + 8, nl), nm[nl] = 0;
		if (qsee_vfs_put(&cat_vfs, nm, cat_in + o + 8 + nl, dl))
			bad = 10;
		o += 8 + nl + dl;
	}
	cat_free(&cat_in, cat_in_total);
	struct qsee_vfile *db = bad ? NULL : qsee_vfs_get(&cat_vfs, CAT_DB);
	if (!db || !db->len) {
		lg("catacomb: load: blob files unusable (check %d, %s): refused, TA untouched", bad, db ? "empty db" : "no db");
		qsee_vfs_clear(&cat_vfs);
		return FPCD_E_INVAL;
	}
	jobs_drain();
	int32_t st = 0, st2 = 0;
	pthread_mutex_lock(&ta_mx);
	int r = qsee_fpc_db_cmd(appid, QSEE_FPC_DB_LOAD, CAT_DIR "/" CAT_DB, &cat_vfs, &st);
	int r2 = (!r && !st) ? ta_set_gid(h.gid, &st2) : 0;	/* HAL fpc_set_active_group: load, set_gid, db id */
	ids_refresh();
	dbid_refresh();
	pthread_mutex_unlock(&ta_mx);
	if (r || st || r2 || st2) {
		lg("catacomb: load of %u-byte db FAILED: rc %d TA status %d, set_gid rc %d status %d (biometrickitd keeps "
		   "its file; the TA db is whatever the failed load left)", db->len, r, st, r2, st2);
		put_u32(p->data, (uint32_t)(st ? st : st2)), p->len = 4;
		return FPCD_E_IO;
	}
	pthread_mutex_lock(&st_mx);
	uint32_t n = S.ids_valid ? S.nids : 0, ids[5];
	memcpy(ids, S.ids, sizeof(ids));
	uint64_t dbid = S.dbid;
	pthread_mutex_unlock(&st_mx);
	int same = n == h.nfids;
	for (uint32_t i = 0; same && i < n; i++) {
		int f = 0;
		for (uint32_t k = 0; k < h.nfids; k++)
			f |= ids[i] == h.fids[k];
		same = f;
	}
	lg("catacomb: LOADED user %u: db %u bytes, %u finger id(s) %s the blob's (%u), db id %#llx (blob %#llx) (%ld ms)",
	   user, db->len, n, same ? "match" : "DO NOT MATCH", h.nfids, (unsigned long long)dbid, (unsigned long long)h.dbid,
	   now_ms() - t0);
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
	{ 13, h_remove },	/* RemoveTemplate: 20 B {user, SMFP uuid}: fid at data[8] */
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
	{ 61, h_cat_prepare },	/* PrepareSaveCatacomb: store_template_db -> staged SWCT blob, u32 size (xnu 1206) */
	{ 62, h_unsupported },	/* CompleteSaveCatacomb: the kernel pulls the blob with FPCD_CMD_CAT_GET */
	{ 63, h_cat_confirm },	/* ConfirmSaveCatacomb: free the staged blob */
	{ 64, h_cat_load },	/* LoadCatacomb: the blob pushed with FPCD_CMD_CAT_PUT -> load_template_db + set_gid */
	{ FPCD_CMD_CAT_PUT, h_cat_put },
	{ FPCD_CMD_CAT_GET, h_cat_get },
	{ FPCD_CMD_STATUS, h_status },
	{ FPCD_CMD_WIPE_ALL, h_wipe },
	{ FPCD_CMD_PING, h_ping },
};

static struct smb_req rq;
static struct smb_reply rp;

/* reply watchdog: the bridge thread must REPLY within a few ms (the kernel blocks the Apple caller meanwhile) */
#define FPCD_REPLY_WARN_MS	100
static volatile long disp_t0_us;	/* != 0 while a request is being handled */
static volatile uint32_t disp_seq, disp_cmd;
static long now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t_start.tv_sec) * 1000000L + (t.tv_nsec - t_start.tv_nsec) / 1000;
}

static void *watchdog_thread(void *arg)
{
	(void)arg;
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);
	long warned = 0;
	while (!g_stop) {
		usleep(20000);
		long t0 = disp_t0_us;
		if (t0 && t0 != warned && now_us() - t0 > FPCD_REPLY_WARN_MS * 1000L) {
			warned = t0;
			lg("WATCHDOG: reply to seq %u id %u %s still pending after %ld ms", disp_seq, disp_cmd,
			   fpcd_cmd_name(disp_cmd), (now_us() - t0) / 1000);
		}
	}
	return NULL;
}

static void dispatch(void)
{
	long t0u = now_us();
	disp_seq = rq.seq, disp_cmd = rq.cmd;
	disp_t0_us = t0u ? t0u : 1;
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
	long th = now_us() - t0u;
	if (rp.len > SMB_DATA_MAX)
		rp.len = SMB_DATA_MAX;
	size_t rsz = bridge_small ? SMB_REQ_HDR + rp.len : sizeof(rp);	/* > 4096 only for replies we never build */
	kern_return_t kr = IOConnectCallStructMethod(conn, SMB_SEL_REPLY, &rp, rsz, NULL, NULL);
	long tr = now_us() - t0u;
	disp_t0_us = 0;
	lg("cmd seq %u id %u %s len %u -> status %d len %u%s | handler %ld us, replied %ld us%s", rq.seq, rq.cmd,
	   fpcd_cmd_name(rq.cmd), rq.len, rp.status, rp.len, mapped ? "" : " UNMAPPED", th, tr, kr ? " REPLY FAILED" : "");
	if (tr > FPCD_REPLY_WARN_MS * 1000L)
		lg("WARN: reply to seq %u id %u %s took %ld ms (> %d ms)", rq.seq, rq.cmd, fpcd_cmd_name(rq.cmd), tr / 1000,
		   FPCD_REPLY_WARN_MS);
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
	if ((e = getenv("FPCD_CRED")) && *e) {
		cred_fn = NULL;
		for (unsigned i = 0; i < sizeof(cred_src) / sizeof(cred_src[0]); i++)
			if (!strcmp(e, cred_src[i].name))
				cred_fn = cred_src[i].fn, cred_name = cred_src[i].name;
		if (!cred_fn) {
			fprintf(stderr, "sweetfpcd: FPCD_CRED=%s unknown (gk-test-file | none)\n", e);
			return 1;
		}
	}
	if ((e = getenv("FPCD_HAT_VERIFIER")) && *e) {
		hat_verifier = NULL;
		for (unsigned i = 0; i < sizeof(hat_vfy) / sizeof(hat_vfy[0]); i++)
			if (!strcmp(e, hat_vfy[i].name))
				hat_verifier = hat_vfy[i].fn, hat_verifier_name = hat_vfy[i].name;
		if (!hat_verifier) {
			fprintf(stderr, "sweetfpcd: FPCD_HAT_VERIFIER=%s unknown (none)\n", e);
			return 1;
		}
	}
	verbose = (e = getenv("FPCD_VERBOSE")) && atoi(e);
	if ((e = getenv("FPCD_BRIDGE_SMALL")))
		bridge_small = atoi(e) != 0;
	if ((e = getenv("FPCD_CAP3_MS")))
		cap3_ms = atol(e);
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

	lg("start %s: %s, TA %s/%s (name %s), op timeout %ld ms, allow-match file %s, bridge structs %s", FPCD_VERSION,
	   self ? "selftest" : "daemon", ta_dir, ta_file, ta_name, op_timeout_ms, ALLOW_MATCH_PATH,
	   bridge_small ? "small (inline, FPCD_BRIDGE_SMALL=1)" : "full 16388/16384 (FPCD_BRIDGE_SMALL=0)");
	pm_init();
	if (qsee_lib_lock("sweetfpcd"))
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

	pthread_mutex_lock(&ta_mx);
	dbid_refresh();
	pthread_mutex_unlock(&ta_mx);
	pthread_t th, thw, thd;
	if (pthread_create(&thw, NULL, ta_worker, NULL) || pthread_create(&thd, NULL, watchdog_thread, NULL) ||
	    pthread_create(&th, NULL, capture_thread, NULL)) {
		lg("pthread_create failed");
		ta_down();
		return 1;
	}
	int said = 0;
	while (!g_stop) {
		if (g_wipe) {
			g_wipe = 0;
			job_post(JOB_WIPE, 0, "SIGUSR1");
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
			static int fresh_sent;
			if (!ta_resident && !fresh_sent) {	/* ws-fpcd-b: the kernel's catacomb state: not loaded */
				fresh_sent = 1;
				ev_send(FPCD_EV_TA_FRESH, 0, 0, 0, 0, 0, 0, 1);
			}
		}
		uint64_t in = FPCD_WAIT_REQ_MS;
		size_t osz = bridge_small ? SMB_SMALL_MAX : sizeof(rq);
		kern_return_t kr = IOConnectCallMethod(conn, SMB_SEL_WAIT, &in, 1, NULL, 0, NULL, NULL, &rq, &osz);
		if (kr == kIOReturnTimeout)
			continue;
		if (kr == kIOReturnNoSpace && bridge_small) {
			/* the request needs more than 4084 data bytes (1192 keeps it queued): fetch it once at full size */
			lg("bridge: WAIT: request larger than %d bytes: fetching it with the full-size buffer", SMB_SMALL_MAX);
			in = 0;
			osz = sizeof(rq);
			kr = IOConnectCallMethod(conn, SMB_SEL_WAIT, &in, 1, NULL, 0, NULL, NULL, &rq, &osz);
			if (kr == kIOReturnTimeout)
				continue;
		}
		if (kr != KERN_SUCCESS || osz < offsetof(struct smb_req, data)) {
			lg("bridge: WAIT %#x (out %zu bytes): reopening in 1 s", kr, osz);
			bridge_close();
			sleep(1);
			continue;
		}
		if (rq.len > osz - offsetof(struct smb_req, data))
			rq.len = (uint32_t)(osz - offsetof(struct smb_req, data));	/* never read past what WAIT wrote */
		dispatch();
	}
	lg("stopping");
	op_cancel_wait();
	pthread_mutex_lock(&st_mx);
	pthread_cond_broadcast(&st_cv);
	pthread_cond_broadcast(&jq_cv);
	pthread_mutex_unlock(&st_mx);
	pthread_join(th, NULL);
	pthread_join(thw, NULL);
	pthread_join(thd, NULL);
	bridge_close();
	ta_down();
	lg("stopped");
	return 0;
}
