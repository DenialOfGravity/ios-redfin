/*
 * loc: sweetlocd's QMI LOC client (the modem's GNSS engine, service 0x10) on QRTR port LOC_PORT, and the PDS bridge for
 * Apple locationd (Claude 2026-11-15, logs/research/gnss-plan-2026-11-15.md option C). Moved out of wland (ws-gnss-c
 * phone/wifi/loc.c) into its own daemon: the QRTR link is the kernel's /dev/sweet-mqrtr-aux (xnu 1124,
 * kern.sweet_mqrtr_aux & 1; sweetlocd.c), not wland's, so starting, stopping or a crash of sweetlocd never touches the
 * modem link that carries calls/SMS; at close the kernel sends DEL_CLIENT for our ports (the modem drops our LOC client).
 *
 * Switch file /var/local/loc.on (absent = nothing here runs, no LOC traffic; re-read every 10 s; words):
 *   probe=1      P0a: once per loc.on change (and per LOC server appearance): INFORM_CLIENT_REVISION 2, then the
 *                read-only GET_SERVICE_REVISION, GET_ENGINE_LOCK, GET_FIX_CRITERIA, GET_PREDICTED_ORBITS_DATA_SOURCE;
 *                each response and its indication are logged with every TLV (qmi_dump). Changes nothing in the modem.
 *   pds=1        the bridge: the kernel router (xnu 1122/1124 relay) hands locationd's PDS requests to pdsemu.c; while
 *                locationd's SET_AUTO_TRACKING_STATE is on, a periodic LOC session runs (REG_EVENTS 0x183, START at
 *                locationd's interval) and every LOC POSITION_REPORT becomes a PDS EVENT_REPORT indication (TLV 0x12
 *                status, TLV 0x13 parsed position) to the client that sent the requests. Stopped (STOP, REG_EVENTS 0)
 *                when locationd stops/cancels, its QMUX session closes, or the word goes away.
 *   session=N    P0b: a standalone test session of N seconds (max 600) without locationd, stopped after N s or 5
 *                good fixes; once per loc.on change. pds=1 wins if both want the engine.
 *   unlock=mi    (ws-gnss-e, owner-approved 2026-11-16) the modem answers our START with ENGINE_LOCKED while its engine
 *                lock has MI locked (redfin: GET_ENGINE_LOCK lockClient 0x3 = MI + NI locked). With this word, before any
 *                session: GET_ENGINE_LOCK (logged as the original), the original goes to /var/local/locd.lock-orig
 *                (fsync'd) BEFORE any change, then SET_ENGINE_LOCK to the original with only the MI bit cleared
 *                (lockType MT = "lock NI only" when the original was ALL, NONE when it was MI) + subType ALL_SUB,
 *                exactly as the QTI HAL's setGpsLockSync encodes it; GET again to confirm (a second try adds the
 *                lockClient TLV). NI/E911 stays as it was. Restored (SET original, GET to confirm, then the file is
 *                removed) when the word goes or changes, loc.on goes, or on SIGTERM/INT/HUP, after the session is stopped.
 *   unlock=none  the same, but the target is "nothing locked": this ALSO unlocks network-initiated (NI/E911/SUPL-MT)
 *                positioning while set. Only if unlock=mi is not enough.
 *                Any other unlock= value is ignored (logged). Every engine lock change is logged with old/new values
 *                ("ENGINE LOCK"). Crash repair: when locd.lock-orig exists and no unlock word is set (also with loc.on
 *                absent: launchd starts us for that file), its value is restored (GET, SET, GET), at most 5 runs.
 * ws-gnss-f words (all default off; Claude 2026-11-16, sources: CLO LA.UM.9.12 location_service_v02.h/.c, loc_api_v02_client.cpp,
 *   LocApiV02.cpp, GnssAdapter.cpp):
 *   client=afw   REG_EVENTS carries TLV 0x11 clientType 1 (AFW) + 0x12 enablePosRequestNotification 0, as the QTI HAL's
 *                locClientRegisterEventMask always does. Without 0x11 the modem treats a client as NFW
 *                ("If not specified, defaults to NFW client"), so the NFW bit of the engine lock (0x2) refuses our START
 *                with ENGINE_LOCKED even when the AFW bit is open. client=nfw sends 0x11 = 2 (for comparison only).
 *   clientid=1   with client=: also TLV 0x10 clientStrId "HAL" (the HAL's non-master id; top-level QMI string, no prefix).
 *   opmode=standalone  SET_OPERATION_MODE (0x004A) 0x01 = 4 STANDALONE before each START, as startFix does. No other mode.
 *   start=android      START with the HAL's TLVs (0x01 sess 1, 0x10 periodic, 0x11 accuracy HIGH, 0x13 interval,
 *                0x15 altitude-assumed off, 0x18 sharePosition 1; no 0x12) instead of ours (0x01, 0x10, 0x12 off, 0x13).
 *   lockclient=1 the FIRST unlock SET_ENGINE_LOCK also carries TLV 0x11 lockClient (otherwise only on try 2).
 *   lockev=1     REG_EVENTS also asks for ENGINE_LOCK_STATE indications (mask bit 0x2000000000, ind 0x00BB; logged in hex).
 * Restore (ws-gnss-f): SET_ENGINE_LOCK in the same TLV kind the original GET answered with (redfin: lockClient TLV 0x12, so
 *   lockType + lockClient = original mask + subType ALL_SUB), then GET to confirm. The modem keeps its EFS (where the lock
 *   lives) in RAM and writes it back to modemst1/modemst2 through wland's rmtfs only now and then; a GET reads the RAM
 *   value. So locd.lock-orig is kept (efs=1) after a confirmed restore until modemst1.img or modemst2.img in EFS_DIR
 *   changes ("ENGINE LOCK restore reached EFS"); a hard reboot before that is repaired at the next start. If neither file
 *   exists, the old behaviour (file removed at once).
 * Hex: every request's TLV bytes are logged ("loc: > ... hex"), every response and indication with qmi_dump, except
 *   POSITION_REPORT / SV_INFO (TLV ids and lengths only, plus the session status: no position bytes in the log).
 * Kernel router (xnu 1122/1124, kern.sweet_bb & 2; the only PDS path for sweetlocd: wland's abpbridge router mode is not
 *   served here): with pds=1 sweetlocd says HELLO to the kernel's relay (QRTR DATA from port PDSR_WPORT to node 1 port PDSR_KPORT, every 10 s; BYE when
 *   the word goes); with kern.sweet_qmux_pds 1 the kernel then sends every PDS request (REQ: pipe, sess, cid, SDU) here,
 *   answered at once by pdsemu (RESP flags 1) or "not mine" (flags 0: the kernel's policy answers); EVENT_REPORTs go back
 *   as IND records; GONE (session closed / client gone) and CID release end locationd's client as abpbridge's session end.
 *   Record: struct pdsr below (24 bytes, LE) + data, the same layout as bsd/dev/sweet_qmux.c.
 * Never sent: SET_ENGINE_LOCK (except unlock=/repair above: only the engine lock value, nothing else),
 * SET_OPERATION_MODE other than STANDALONE (and only with opmode=standalone), SUPL/NI settings, any NV/EFS write, XTRA or
 * position injects; INJECT_UTC_TIME only with time=1 (below).
 * ws-gnss-g words (default off; Claude 2026-11-16):
 *   time=1       REG_EVENTS adds INJECT_TIME_REQ 0x10 | INJECT_PREDICTED_ORBITS_REQ 0x20 | INJECT_POSITION_REQ 0x40 (mask 0x1f3)
 *                and INJECT_UTC_TIME (0x0038: 0x01 timeUtc u64 ms since 1970, 0x02 timeUnc u32 ms) is sent from the AP's
 *                CLOCK_REALTIME once before each START and again on every modem INJECT_TIME_REQ (0x0028; at most one per
 *                10 s, only while a session runs). The time is read when the request is sent, not when queued. Never
 *                injected when the clock is before 2025 (no sweetrtcoff). A refused inject is logged, not a LOC failure.
 *   timeunc=MS   the uncertainty sent (20..60000, default 5000): the port's clock is the PM8150 RTC + boot-arg sweetrtcoff
 *                (kernel 0099, calibrated once against the PC, then free-running; no NTP/NITZ correction is known), so
 *                5 s is the honest default; lower it only after checking the phone's `date` against a reference.
 *   INJECT_PREDICTED_ORBITS_REQ (0x0029: allowed sizes, server list) and INJECT_POSITION_REQ (0x002A) are logged only.
 *   Every GNSS_SV_INFO report gets one "loc: sv #N" line: satellites listed / tracked / with CN0, how many >= 30 and
 *   >= 20 dB-Hz, best CN0 and the average of the best 4, per constellation counts (no elevation, azimuth or position).
 * One request in flight, 5 s response timeout; 3 failures in a row (timeout or QMI error) drop LOC until loc.on
 * changes ("loc: DROPPED"). Status: /var/local/loc.status (`sweetlocd status`). Coordinates are logged rounded to 0.01 deg.
 * loc_shutdown() (SIGTERM, loc.on gone): STOP + REG_EVENTS 0 for a running session and BYE to the relay, then the
 * engine lock restore if we changed it (loc_quiet waits for it).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "englock.h"
#include "link.h"
#include "loc.h"
#include "pdsemu.h"
#include "qmi.h"

#define MAX_FAILS	3
#define RESP_MS		5000
#define IND_MS		3000
#define NQ		12
#ifndef EFS_DIR				/* wland's rmtfs files (phone/wifi/rmtfs.c RMTFS_DIR); the host test points it elsewhere */
#define EFS_DIR		"/var/local/fw/rmtfs"
#endif
#define EFS_WAIT_MS	900000		/* how long a confirmed restore waits for the modem's EFS write-back */

struct req {
	uint16_t id;
	uint8_t tl[64];
	uint16_t tn;
	uint8_t want_ind;
	const char *why;
};

static int srv_up;
static uint32_t srv_node, srv_port, srv_inst;
static int on_present, w_probe, w_pds, w_session, w_unlock;	/* w_unlock: 1 mi, 2 none */
static int w_client, w_clientid, w_opmode, w_start_android, w_lockclient, w_lockev;	/* ws-gnss-f; w_client 1 afw, 2 nfw */
static int w_time;			/* ws-gnss-g time=1 */
static uint32_t w_timeunc = 5000;	/* ws-gnss-g timeunc=MS (default 5 s: the AP clock is not known to be network-synced) */
static uint64_t time_last_inject;	/* link_ms of the last INJECT_UTC_TIME sent (0 = none) */
static unsigned n_time_req, n_time_inj, n_time_ok, n_xtra_req, n_pos_req, n_sv;
static struct loc_sv_sum sv_last;
static int sv_have;
static char on_text[256];
static uint64_t next_check, probe_gen_done = ~0ull, on_gen, sess_gen_done = ~0ull;
static int fails, dropped, rev_sent, after_drop;
static void q_stop(void);
static int time_ok(uint64_t *ms);
static void efs_log_now(const char *why);
static void lk_event(int ok, const struct qmi_msg *m);
static struct req q[NQ];
static int qn;
static struct {
	int busy, got_resp;
	uint16_t id, txn;
	uint8_t want_ind;
	uint64_t sent, resp_at;
	const char *why;
	int lk;			/* an engine lock request (lkq): answers go to lk_event, never to fail() */
} cur;
static uint16_t txn_next = 1;
/* session: 0 idle, 1 starting (queued), 2 running, 3 stopping */
static int sess, sess_owner;	/* owner 1 = pds, 2 = test */
static uint32_t sess_interval;
static uint64_t sess_t0, test_until, last_fail_ind, status_at;
static unsigned nfix_good, nfix_all, npos_sent, last_nsv, engine;
static struct loc_fix last;
static int have_last, status_dirty = 1;
static struct pds_state pds;
static int pds_inited;
/* engine lock (unlock=mi / unlock=none / repair; englock.h): its own one-request slot, sent before the queue */
enum { LK_IDLE, LK_RD, LK_RD_DV, LK_SET, LK_CHK, LK_UNLOCKED, LK_RS_SET, LK_RS_CHK, LK_RP_RD, LK_NOOP, LK_FAILED, LK_RS_FAILED,
       LK_EFS };
static const char *const lk_names[] = { "idle", "reading", "reading DV", "setting", "checking", "UNLOCKED", "restoring",
					"checking restore", "repair read", "no change needed", "unlock failed", "RESTORE FAILED",
					"restored, waiting for the EFS write" };
static unsigned lk_orig_kind;			/* ws-gnss-f: LK_KIND_* of the original GET */
static int lk_orig_dv_known;
static uint64_t lk_orig_dv;
/* ws-gnss-f: modem EFS write-back watch (modemst1/2 mtimes in ns, 0 = file missing) */
static long long efs_m[2];
static int efs_watch;				/* 1 after an unlock (log only), 2 after a restore (LK_EFS) */
static uint64_t efs_t0, efs_next;
static const char *efs_why;
static int lk, lk_try, lk_mode, lk_rs_tries, lk_verify, lk_now_known;	/* lk_mode: the word we unlocked for */
static uint64_t lk_orig, lk_target, lk_now_mask;
static uint64_t lk_gen_done = ~0ull, lk_fail_gen = ~0ull, lk_repair_gen = ~0ull;
static struct {
	int pending;
	uint16_t id, tn;
	uint8_t tl[32];
	const char *why;
} lkq;

static unsigned long long now_ms(void) { return (unsigned long long)link_ms(); }

/* ws-gnss-f: a request's TLV bytes in hex (all our requests are short: no position data is ever sent) */
static void hex_req(uint16_t id, const uint8_t *tl, uint32_t tn)
{
	printf("[%6llu] loc: > 0x%04x hex:", now_ms(), id);
	for (uint32_t i = 0; i < tn; i++)
		printf(" %02x", tl[i]);
	printf("%s\n", tn ? "" : " (no TLVs)");
}

/* ws-gnss-f: TLV ids/lengths only, plus 0x01 (session status / state) and 0x02 (session id) in hex: for indications that can
 * carry a position (POSITION_REPORT, SV_INFO), so no coordinates reach the log */
static void dump_ids(const struct qmi_msg *m)
{
	printf("qmi: type %u txn %u msg %#x len %u (ids only):", m->type, m->txn, m->id, m->len);
	for (uint32_t o = 0; o + 3 <= m->len;) {
		uint16_t l = (uint16_t)(m->tlv[o + 1] | m->tlv[o + 2] << 8);
		printf(" [%#x/%u", m->tlv[o], l);
		if ((m->tlv[o] == 0x01 || m->tlv[o] == 0x02) && l <= 4)
			for (uint16_t k = 0; k < l && o + 3 + k < m->len; k++)
				printf(" %02x", m->tlv[o + 3 + k]);
		printf("]");
		o += 3u + l;
	}
	printf("\n");
}

/* ---- xnu 1122: the kernel router's PDS relay ---- */
#define PDSR_MAGIC	0x52534450u
struct pdsr {
	uint32_t magic;
	uint8_t op, cid;
	uint16_t flags;
	uint32_t pipe, sess, tag;
	uint16_t code, len;
};
static int kr_hello_sent, kr_ack = -1;
static uint64_t kr_next_hello;
static unsigned kr_reqs, kr_mine, kr_inds;
int loc_fake_kernel_router;

int loc_kernel_router(void)
{
#ifdef __APPLE__
	static int v = -1;
	if (v < 0) {
		int b = 0;
		size_t n = sizeof(b);
		v = !sysctlbyname("kern.sweet_bb", &b, &n, NULL, 0) && (b & 2);
	}
	return v;
#else
	return loc_fake_kernel_router;
#endif
}

static int kr_send(uint8_t op, unsigned cid, unsigned flags, uint32_t pipe, uint32_t sess, uint32_t tag, unsigned code,
		   const uint8_t *d, uint32_t len)
{
	uint8_t b[sizeof(struct pdsr) + 1024];
	struct pdsr h = { PDSR_MAGIC, op, (uint8_t)cid, (uint16_t)flags, pipe, sess, tag, (uint16_t)code, (uint16_t)len };
	if (len > 1024)
		return -1;
	memcpy(b, &h, sizeof(h));
	if (len)
		memcpy(b + sizeof(h), d, len);
	return qrtr_send(QRTR_TYPE_DATA, PDSR_WPORT, QRTR_NODE_APPS, PDSR_KPORT, b, (uint32_t)sizeof(h) + len);
}

/* an indication for locationd's PDS client: only through the kernel's relay (sweetlocd has no abpbridge) */
static int pds_send_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn)
{
	(void)svc;
	if (!kr_hello_sent || kr_ack != 1)
		return -1;
	if (kr_send(3, cid, 0, pipe, sess, 0, msg, tl, tn) != 0)
		return -1;
	kr_inds++;
	return 0;
}

static int shutting;

static void kr_poll(uint64_t now)
{
	int want = loc_pds_on() && loc_kernel_router() && !shutting;
	if (want && now >= kr_next_hello) {
		kr_next_hello = now + 10000;
		if (!kr_hello_sent)
			printf("[%6llu] loc: PDS relay: HELLO to the kernel router (port %#x -> 1:%#x)\n", now_ms(), PDSR_WPORT, PDSR_KPORT);
		kr_send(1, 0, 0, 0, 0, 0, 0, NULL, 0);
		kr_hello_sent = 1;
	} else if (!want && kr_hello_sent) {
		kr_send(4, 0, 0, 0, 0, 0, 0, NULL, 0);
		printf("[%6llu] loc: PDS relay: BYE (%u requests, %u answered here, %u indications)\n", now_ms(), kr_reqs, kr_mine,
		       kr_inds);
		kr_hello_sent = 0;
		kr_ack = -1;
		kr_next_hello = 0;
	}
}

int loc_pds_on(void)
{
	return on_present && w_pds && !dropped;
}

static void enqueue(uint16_t id, const uint8_t *tl, uint16_t tn, int want_ind, const char *why)
{
	if (qn >= NQ) {
		printf("[%6llu] loc: queue full, %s (0x%04x) dropped\n", now_ms(), why, id);
		return;
	}
	q[qn].id = id;
	if (tn > sizeof(q[qn].tl))
		tn = sizeof(q[qn].tl);
	if (tn)
		memcpy(q[qn].tl, tl, tn);
	q[qn].tn = tn;
	q[qn].want_ind = (uint8_t)want_ind;
	q[qn].why = why;
	qn++;
}

static void fail(const char *what)
{
	fails++;
	printf("[%6llu] loc: %s (failure %d of %d)\n", now_ms(), what, fails, MAX_FAILS);
	if (fails >= MAX_FAILS && !dropped) {
		dropped = 1;
		qn = 0;
		if (sess == 1 || sess == 2) {	/* one last STOP + REG_EVENTS 0 so the engine does not stay on */
			q_stop();
			after_drop = qn;
		}
		sess = 0;
		sess_owner = 0;
		printf("[%6llu] loc: DROPPED after %d failures in a row: no LOC requests until %s changes\n", now_ms(), fails, LOC_ON);
	}
	status_dirty = 1;
}

static void send_next(void)
{
	if (!cur.busy && lkq.pending && srv_up) {
		struct qmi_buf lb;
		uint16_t ltxn = txn_next++;
		if (!txn_next)
			txn_next = 1;
		lkq.pending = 0;
		qmi_start(&lb, QMI_REQUEST, ltxn, lkq.id);
		memcpy(lb.b + lb.n, lkq.tl, lkq.tn);
		lb.n += lkq.tn;
		size_t ll = qmi_finish(&lb);
		memset(&cur, 0, sizeof(cur));
		cur.lk = 1;
		cur.id = lkq.id;
		cur.txn = ltxn;
		cur.want_ind = 1;
		cur.why = lkq.why;
		if (!ll || qrtr_send(QRTR_TYPE_DATA, LOC_PORT, srv_node, srv_port, lb.b, (uint32_t)ll)) {
			printf("[%6llu] loc: engine lock: send of 0x%04x failed\n", now_ms(), lkq.id);
			lk_event(0, NULL);
			return;
		}
		cur.busy = 1;
		cur.sent = link_ms();
		printf("[%6llu] loc: > 0x%04x %s (txn %u, %u TLV bytes)\n", now_ms(), lkq.id, lkq.why, ltxn, lkq.tn);
		hex_req(lkq.id, lkq.tl, lkq.tn);
		return;
	}
	if (cur.busy || !qn || !srv_up || (dropped && !after_drop))
		return;
	if (dropped)
		after_drop--;
	struct req r = q[0];
	memmove(q, q + 1, (size_t)(qn - 1) * sizeof(q[0]));
	qn--;
	if (r.id == LOC_INJECT_UTC_TIME) {	/* ws-gnss-g: the time as it is now, not when it was queued */
		uint64_t ms;
		if (!time_ok(&ms))
			return;
		r.tn = (uint16_t)loc_tlv_inject_utc(r.tl, ms, w_timeunc);
		time_last_inject = link_ms();
		n_time_inj++;
		printf("[%6llu] loc: INJECT_UTC_TIME (%s): AP UTC %llu ms, uncertainty %u ms (clock: PM8150 RTC + sweetrtcoff, "
		       "not known to be network-synced)\n", now_ms(), r.why, (unsigned long long)ms, w_timeunc);
	}
	struct qmi_buf b;
	uint16_t txn = txn_next++;
	if (!txn_next)
		txn_next = 1;
	qmi_start(&b, QMI_REQUEST, txn, r.id);
	if (r.tn) {
		memcpy(b.b + b.n, r.tl, r.tn);
		b.n += r.tn;
	}
	size_t len = qmi_finish(&b);
	if (!len || qrtr_send(QRTR_TYPE_DATA, LOC_PORT, srv_node, srv_port, b.b, (uint32_t)len)) {
		fail("send failed");
		return;
	}
	memset(&cur, 0, sizeof(cur));
	cur.busy = 1;
	cur.id = r.id;
	cur.txn = txn;
	cur.want_ind = r.want_ind;
	cur.sent = link_ms();
	cur.why = r.why;
	printf("[%6llu] loc: > 0x%04x %s (txn %u, %u TLV bytes)\n", now_ms(), r.id, r.why, txn, r.tn);
	hex_req(r.id, r.tl, r.tn);
}

/* ws-gnss-g time=1: INJECT_UTC_TIME from the AP clock. The port's wall clock is the PM8150 RTC + the boot-arg
 * sweetrtcoff (kernel 0099, calibrated once against the PC; no NTP/NITZ correction known), so the uncertainty is
 * timeunc= (default 5000 ms), never smaller than 20 ms. A clock before 2025 (no sweetrtcoff) is never injected. */
#define TIME_SANE_S	1735689600LL	/* 2025-01-01 */
static int time_ok(uint64_t *ms)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) || (long long)ts.tv_sec < TIME_SANE_S)
		return 0;
	*ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
	return 1;
}

static void time_queue(const char *why)
{
	uint64_t ms;
	uint8_t t[32];
	if (!time_ok(&ms)) {
		printf("[%6llu] loc: time=1: AP clock before 2025 (no sweetrtcoff?): no INJECT_UTC_TIME (%s)\n", now_ms(), why);
		return;
	}
	for (int i = 0; i < qn; i++)
		if (q[i].id == LOC_INJECT_UTC_TIME)
			return;		/* one queued is enough */
	enqueue(LOC_INJECT_UTC_TIME, t, (uint16_t)loc_tlv_inject_utc(t, ms, w_timeunc), 1, why);
}

static void q_start(uint32_t interval)
{
	uint8_t t[64];
	uint32_t n;
	if (!rev_sent) {
		n = loc_tlv_client_rev(t, 2);
		enqueue(LOC_INFORM_CLIENT_REV, t, (uint16_t)n, 0, "INFORM_CLIENT_REVISION 2");
		rev_sent = 1;
	}
	n = loc_tlv_reg_events_ex(t, LOC_EVMASK_BRIDGE | (w_lockev ? LOC_EVMASK_ENGINE_LOCK_STATE : 0) |
				  (w_time ? LOC_EVMASK_ASSIST : 0), (uint32_t)w_client, w_client && w_clientid ? "HAL" : NULL);
	enqueue(LOC_REG_EVENTS, t, (uint16_t)n, 0, w_time ? (w_client == LOC_CLIENT_AFW ? "REG_EVENTS 0x1f3 clientType AFW" :
		w_client == LOC_CLIENT_NFW ? "REG_EVENTS 0x1f3 clientType NFW" : "REG_EVENTS 0x1f3") :
		w_client == LOC_CLIENT_AFW ? "REG_EVENTS 0x183 clientType AFW" :
		w_client == LOC_CLIENT_NFW ? "REG_EVENTS 0x183 clientType NFW" : "REG_EVENTS 0x183");
	if (w_time)		/* ws-gnss-g: the HAL injects network time when it has it; the TLV is filled at send time */
		time_queue("session start");
	if (w_opmode) {
		n = loc_tlv_opmode_standalone(t);
		enqueue(LOC_SET_OPERATION_MODE, t, (uint16_t)n, 1, "SET_OPERATION_MODE STANDALONE");
	}
	n = w_start_android ? loc_tlv_start_android(t, interval) : loc_tlv_start(t, interval);
	enqueue(LOC_START, t, (uint16_t)n, 0, w_start_android ? "START session 1 periodic (HAL TLVs)" : "START session 1 periodic");
}

static void q_stop(void)
{
	uint8_t t[64];
	uint32_t n = loc_tlv_stop(t);
	enqueue(LOC_STOP, t, (uint16_t)n, 0, "STOP session 1");
	n = loc_tlv_reg_events_ex(t, 0, (uint32_t)w_client, w_client && w_clientid ? "HAL" : NULL);
	enqueue(LOC_REG_EVENTS, t, (uint16_t)n, 0, "REG_EVENTS 0");
}

static void session_stop(const char *why)
{
	if (sess == 1 || sess == 2) {
		printf("[%6llu] loc: session stop (%s), %u fixes (%u good) in %llu s\n", now_ms(), why, nfix_all, nfix_good,
		       (now_ms() - sess_t0) / 1000);
		/* a START still queued is simply taken back */
		for (int i = 0; i < qn;)
			if (q[i].id == LOC_START || (q[i].id == LOC_REG_EVENTS && q[i].tl[3])) {
				memmove(q + i, q + i + 1, (size_t)(qn - i - 1) * sizeof(q[0]));
				qn--;
			} else {
				i++;
			}
		q_stop();
		sess = 3;
		status_dirty = 1;
	}
	sess_owner = 0;
}

static void session_start(int owner, uint32_t interval)
{
	sess_owner = owner;
	sess_interval = interval;
	sess_t0 = link_ms();
	nfix_all = nfix_good = 0;
	q_start(interval);
	sess = 1;
	status_dirty = 1;
	printf("[%6llu] loc: session start for %s, interval %u ms\n", now_ms(), owner == 1 ? "locationd (pds=1)" : "test (session=)",
	       interval);
}

static void probe_queue(void)
{
	uint8_t t[16];
	uint32_t n = loc_tlv_client_rev(t, 2);
	if (!rev_sent) {
		enqueue(LOC_INFORM_CLIENT_REV, t, (uint16_t)n, 0, "INFORM_CLIENT_REVISION 2 (probe)");
		rev_sent = 1;
	}
	enqueue(LOC_GET_SERVICE_REV, NULL, 0, 1, "GET_SERVICE_REVISION (probe)");
	enqueue(LOC_GET_ENGINE_LOCK, NULL, 0, 1, "GET_ENGINE_LOCK (probe, read-only)");
	n = lk_tlv_get(t, LK_SUB_DV);
	enqueue(LOC_GET_ENGINE_LOCK, t, (uint16_t)n, 1, "GET_ENGINE_LOCK subType DV (probe, read-only)");
	efs_log_now("probe");
	enqueue(LOC_GET_FIX_CRITERIA, NULL, 0, 1, "GET_FIX_CRITERIA (probe)");
	enqueue(LOC_GET_XTRA_SOURCE, NULL, 0, 1, "GET_PREDICTED_ORBITS_DATA_SOURCE (probe)");
}

/* ---- ws-gnss-f: the modem's EFS write-back (wland's rmtfs writes modemst1.img / modemst2.img) ---- */
static int efs_snap(long long m[2])
{
	static const char *const f[2] = { EFS_DIR "/modemst1.img", EFS_DIR "/modemst2.img" };
	int found = 0;
	for (int i = 0; i < 2; i++) {
		struct stat st;
		m[i] = 0;
		if (stat(f[i], &st))
			continue;
		found++;
#ifdef __APPLE__
		m[i] = (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
		m[i] = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
		if (!m[i])
			m[i] = 1;
	}
	return found;
}

static void efs_log_now(const char *why)
{
	long long m[2];
	time_t now = time(NULL);
	if (!efs_snap(m)) {
		printf("[%6llu] loc: EFS (%s): no modemst1.img/modemst2.img in %s\n", now_ms(), why, EFS_DIR);
		return;
	}
	printf("[%6llu] loc: EFS (%s): modemst1.img written %lld s ago, modemst2.img %lld s ago\n", now_ms(), why,
	       m[0] ? (long long)now - m[0] / 1000000000LL : -1, m[1] ? (long long)now - m[1] / 1000000000LL : -1);
}

/* start watching for the next EFS write; 0 if the files are not there (nothing to wait for) */
static int efs_watch_start(int kind, const char *why)
{
	if (!efs_snap(efs_m))
		return 0;
	efs_watch = kind;
	efs_t0 = link_ms();
	efs_next = efs_t0 + 2000;
	efs_why = why;
	return 1;
}

/* 1 = one of the files changed since the snapshot */
static int efs_changed(const long long ref[2], int *which)
{
	long long m[2];
	efs_snap(m);
	for (int i = 0; i < 2; i++)
		if (m[i] != ref[i] && m[i]) {
			if (which)
				*which = i + 1;
			return 1;
		}
	return 0;
}

static int lk_save_kind(uint64_t mask, unsigned tries, int efs)
{
	struct lk_file lf = { .mask = mask, .tries = tries, .kind = lk_orig_kind, .efs = (unsigned)efs };
	if (efs) {
		lf.m1 = efs_m[0];
		lf.m2 = efs_m[1];
	}
	return lk_save_ex(LOCK_ORIG, &lf);
}

/* ---- engine lock: unlock=mi / unlock=none, restore, crash repair (header; englock.h) ---- */
static int lk_active(void)
{
	return lk == LK_RD || lk == LK_RD_DV || lk == LK_SET || lk == LK_CHK || lk == LK_RS_SET || lk == LK_RS_CHK || lk == LK_RP_RD;
}

static int lk_restore_wanted(void)
{
	return lk == LK_UNLOCKED && (shutting || !on_present || w_unlock != lk_mode);
}

static int lk_repair_wanted(void)
{
	return lk == LK_IDLE && (!w_unlock || !on_present || shutting) && lk_repair_gen != on_gen && !access(LOCK_ORIG, F_OK);
}

static void lk_req(uint16_t id, const uint8_t *tl, uint32_t tn, const char *why)
{
	lkq.pending = 1;
	lkq.id = id;
	lkq.tn = (uint16_t)tn;
	if (tn)
		memcpy(lkq.tl, tl, tn);
	lkq.why = why;
	status_dirty = 1;
}

static void lk_get(unsigned sub, const char *why)
{
	uint8_t t[16];
	lk_req(LOC_GET_ENGINE_LOCK, t, lk_tlv_get(t, sub), why);
}

static void lk_set(uint64_t mask, int with_client, const char *why)
{
	uint8_t t[32];
	char a[96], b[96], c[96];
	printf("[%6llu] loc: ENGINE LOCK CHANGE (%s): SET_ENGINE_LOCK lockType %u%s%llx subType ALL_SUB = %s; modem was %s, "
	       "original %s\n", now_ms(), why, lk_type_of_mask(mask), with_client ? " + lockClient 0x" : " (no lockClient) target 0x",
	       (unsigned long long)mask, lk_str(mask, a, sizeof(a)), lk_now_known ? lk_str(lk_now_mask, b, sizeof(b)) : "?",
	       lk_str(lk_orig, c, sizeof(c)));
	lk_req(LOC_SET_ENGINE_LOCK, t, lk_tlv_set(t, lk_type_of_mask(mask), with_client, mask), "SET_ENGINE_LOCK");
}

/* ws-gnss-f: the restore uses the TLV kind the original GET answered with (lockClient when it reported lockClient) */
static void lk_restore_start(const char *why)
{
	lk_rs_tries++;
	lk_try = ((lk_orig & ~3ull) || (lk_orig_kind & LK_KIND_CLIENT)) ? 2 : 1;
	printf("[%6llu] loc: engine lock: restore in the original's TLV kind: %s\n", now_ms(),
	       lk_try == 2 ? "lockType + lockClient (the GET reported lockClient)" : "lockType only (the GET reported lockType)");
	lk_set(lk_orig, lk_try == 2, why);
	lk = LK_RS_SET;
}

static void lk_fail(int to, const char *msg)
{
	printf("[%6llu] loc: engine lock: %s\n", now_ms(), msg);
	lk = to;
	lk_fail_gen = on_gen;
}

static void lk_event(int ok, const struct qmi_msg *m)
{
	struct lk_val v;
	int64_t mask = -1;
	char a[96], b[96];
	memset(&v, 0, sizeof(v));
	if (ok && m) {
		lk_parse(m, &v);
		if (v.has_status && v.status != 0)
			ok = 0;
	}
	if (ok)
		mask = lk_mask(&v);
	if (ok && mask >= 0 && lk != LK_RD_DV) {
		lk_now_mask = (uint64_t)mask;
		lk_now_known = 1;
	}
	if (!ok && (lk == LK_RD || lk == LK_RP_RD || lk == LK_CHK || lk == LK_RS_CHK || lk == LK_RD_DV))
		printf("[%6llu] loc: engine lock: GET_ENGINE_LOCK %s\n", now_ms(), m ? "indication status not success" :
		       "failed (error/timeout)");
	switch (lk) {
	case LK_RD:
	case LK_RP_RD: {
		if (!ok || mask < 0) {
			lk_fail(lk == LK_RD ? LK_FAILED : LK_RS_FAILED, ok ? "GET_ENGINE_LOCK gave no lock value: nothing changed" :
				"no lock value: nothing changed");
			break;
		}
		printf("[%6llu] loc: engine lock: modem %s%s%s\n", now_ms(), lk_str((uint64_t)mask, a, sizeof(a)),
		       v.has_type ? " (lockType TLV)" : "", v.has_client ? " (lockClient TLV)" : "");
		if (lk == LK_RP_RD) {
			struct lk_file rf;
			int rr = lk_load_ex(LOCK_ORIG, &rf);
			if (!lk_orig_kind)
				lk_orig_kind = (v.has_type ? LK_KIND_TYPE : 0) | (v.has_client ? LK_KIND_CLIENT : 0);
			if ((uint64_t)mask == lk_orig && rr == 0 && rf.efs) {
				long long ref[2] = { rf.m1, rf.m2 };
				int w = 0;
				if (efs_changed(ref, &w)) {
					printf("[%6llu] loc: engine lock: repair: modem at the original %s and modemst%d.img was written since "
					       "the restore: %s removed\n", now_ms(), lk_str(lk_orig, b, sizeof(b)), w, LOCK_ORIG);
					unlink(LOCK_ORIG);
					lk = LK_IDLE;
				} else {
					printf("[%6llu] loc: engine lock: repair: modem at the original %s but no EFS write seen since the "
					       "restore yet: waiting for it (%s kept)\n", now_ms(), lk_str(lk_orig, b, sizeof(b)), LOCK_ORIG);
					efs_m[0] = rf.m1;
					efs_m[1] = rf.m2;
					efs_watch = 2;
					efs_t0 = link_ms();
					efs_next = efs_t0 + 2000;
					efs_why = "restore (repair run)";
					lk = LK_EFS;
				}
			} else if ((uint64_t)mask == lk_orig) {
				printf("[%6llu] loc: engine lock: repair: modem already at the original %s, %s removed\n", now_ms(),
				       lk_str(lk_orig, b, sizeof(b)), LOCK_ORIG);
				unlink(LOCK_ORIG);
				lk = LK_IDLE;
			} else {
				lk_restore_start("repair after an unclean exit");
			}
			break;
		}
		struct lk_file lf;
		int fr = lk_load_ex(LOCK_ORIG, &lf);
		if (fr == -2) {
			lk_fail(LK_FAILED, LOCK_ORIG " exists but is unreadable: not touching the lock");
			break;
		}
		lk_orig_kind = (v.has_type ? LK_KIND_TYPE : 0) | (v.has_client ? LK_KIND_CLIENT : 0);
		if (fr == 0) {
			lk_orig = lf.mask;
			if (lf.kind)
				lk_orig_kind = lf.kind;
			printf("[%6llu] loc: engine lock: original from %s (an earlier run did not restore): %s\n", now_ms(), LOCK_ORIG,
			       lk_str(lk_orig, a, sizeof(a)));
		} else {
			lk_orig = (uint64_t)mask;
			printf("[%6llu] loc: engine lock: ORIGINAL %s\n", now_ms(), lk_str(lk_orig, a, sizeof(a)));
		}
		lk_get(LK_SUB_DV, "GET_ENGINE_LOCK subType DV (read-only, for the log)");
		lk = LK_RD_DV;
		break;
	}
	case LK_RD_DV:
		if (shutting || !on_present || w_unlock != lk_mode) {
			printf("[%6llu] loc: engine lock: unlock no longer wanted: nothing set\n", now_ms());
			lk = LK_IDLE;
			break;
		}
		if (ok && mask >= 0) {
			printf("[%6llu] loc: engine lock: DV subscription %s%s\n", now_ms(), lk_str((uint64_t)mask, a, sizeof(a)),
			       (uint64_t)mask == lk_orig ? "" : " (DIFFERS from DD: the ALL_SUB restore will set the DD value on both)");
			lk_orig_dv = (uint64_t)mask;
			lk_orig_dv_known = 1;
		} else
			printf("[%6llu] loc: engine lock: DV subscription value not available\n", now_ms());
		lk_target = lk_mode == 2 ? 0 : (lk_orig & ~LK_AFW);
		if (lk_now_mask == lk_target) {
			if (lk_target == lk_orig) {
				printf("[%6llu] loc: engine lock: %s already allows our sessions: nothing to set\n", now_ms(),
				       lk_str(lk_orig, a, sizeof(a)));
				unlink(LOCK_ORIG);
				lk = LK_NOOP;
			} else {
				if (access(LOCK_ORIG, F_OK) && lk_save_kind(lk_orig, 0, 0)) {
					lk_fail(LK_FAILED, "already at the target but " LOCK_ORIG " cannot be written");
					break;
				}
				printf("[%6llu] loc: engine lock: modem already at the target %s (original %s kept in %s)\n", now_ms(),
				       lk_str(lk_target, a, sizeof(a)), lk_str(lk_orig, b, sizeof(b)), LOCK_ORIG);
				lk = LK_UNLOCKED;
			}
			break;
		}
		if (access(LOCK_ORIG, F_OK) && lk_save_kind(lk_orig, 0, 0)) {
			printf("[%6llu] loc: engine lock: cannot write %s (%s)\n", now_ms(), LOCK_ORIG, strerror(errno));
			lk_fail(LK_FAILED, "original not saved: not unlocking");
			break;
		}
		printf("[%6llu] loc: engine lock: original saved in %s\n", now_ms(), LOCK_ORIG);
		lk_try = (((lk_target | lk_orig) & ~3ull) || w_lockclient) ? 2 : 1;	/* lockclient=1: lockClient on try 1 */
		lk_set(lk_target, lk_try == 2, lk_mode == 2 ? "unlock=none" : "unlock=mi");
		lk = LK_SET;
		break;
	case LK_SET:
	case LK_RS_SET:
		if (!ok)
			printf("[%6llu] loc: engine lock: SET_ENGINE_LOCK not confirmed (%s): reading back\n", now_ms(),
			       m ? "indication status not success" : "error/timeout");
		lk_get(0, lk == LK_SET ? "GET_ENGINE_LOCK (check after unlock)" : "GET_ENGINE_LOCK (check after restore)");
		lk = lk == LK_SET ? LK_CHK : LK_RS_CHK;
		break;
	case LK_CHK:
		if (ok && mask >= 0 && (uint64_t)mask == lk_target) {
			printf("[%6llu] loc: ENGINE LOCK now %s (original %s; restored when the word/loc.on goes or on exit)\n", now_ms(),
			       lk_str((uint64_t)mask, a, sizeof(a)), lk_str(lk_orig, b, sizeof(b)));
			lk = LK_UNLOCKED;
			lk_verify = 0;
			efs_watch_start(1, "unlock");
			break;
		}
		printf("[%6llu] loc: engine lock: unlock not confirmed: modem %s, wanted %s (try %d)\n", now_ms(),
		       ok && mask >= 0 ? lk_str((uint64_t)mask, a, sizeof(a)) : "?", lk_str(lk_target, b, sizeof(b)), lk_try);
		if (lk_try == 1) {
			lk_try = 2;
			lk_set(lk_target, 1, "unlock, try 2 with lockClient");
			lk = LK_SET;
			break;
		}
		lk_verify = 0;
		if (ok && mask >= 0 && (uint64_t)mask == lk_orig) {	/* nothing changed: nothing to put back */
			unlink(LOCK_ORIG);
			lk_fail(LK_FAILED, "unlock refused, modem unchanged at the original: " LOCK_ORIG " removed");
			break;
		}
		lk_restore_start("unlock not confirmed: putting the original back");
		break;
	case LK_RS_CHK:
		if (ok && mask >= 0 && (uint64_t)mask == lk_orig) {
			lk_mode = 0;
			lk_rs_tries = 0;
			if (efs_watch_start(2, "restore")) {
				lk_repair_gen = on_gen;		/* no repair run in this loc.on generation right after the wait */
				if (lk_save_kind(lk_orig, 0, 1))
					printf("[%6llu] loc: engine lock: cannot update %s (%s)\n", now_ms(), LOCK_ORIG, strerror(errno));
				printf("[%6llu] loc: ENGINE LOCK restored to the original %s (GET); %s kept until the modem writes its EFS "
				       "(modemst1/2), at most %d s\n", now_ms(), lk_str(lk_orig, a, sizeof(a)), LOCK_ORIG, EFS_WAIT_MS / 1000);
				lk = LK_EFS;
				break;
			}
			printf("[%6llu] loc: ENGINE LOCK restored to the original %s; %s removed (no EFS files in %s to watch)\n",
			       now_ms(), lk_str(lk_orig, a, sizeof(a)), LOCK_ORIG, EFS_DIR);
			unlink(LOCK_ORIG);
			lk = LK_IDLE;
			break;
		}
		if (lk_try == 1) {
			lk_try = 2;
			lk_set(lk_orig, 1, "restore, try 2 with lockClient");
			lk = LK_RS_SET;
			break;
		}
		printf("[%6llu] loc: engine lock: RESTORE FAILED: modem %s, original %s; %s kept (tried again when %s changes, "
		       "and at the next start)\n", now_ms(), ok && mask >= 0 ? lk_str((uint64_t)mask, a, sizeof(a)) : "?",
		       lk_str(lk_orig, b, sizeof(b)), LOCK_ORIG, LOC_ON);
		lk = LK_RS_FAILED;
		lk_fail_gen = on_gen;
		break;
	default:
		break;
	}
	status_dirty = 1;
}

/* 1 = sessions must not start yet (an unlock, restore or repair is due or running) */
static int lk_hold(void)
{
	return lk_active() || lkq.pending || lk_restore_wanted() || lk_repair_wanted() ||
	       (on_present && w_unlock && lk == LK_IDLE && lk_gen_done != on_gen && !shutting && !dropped);
}

/* ws-gnss-f: the modem's EFS write-back. ws-gnss-g: its own poll, run every loop even with the LOC server down or a
 * request in flight, so the 900 s limit always ends the wait (it only stats two files) */
static void efs_poll(void)
{
	if (efs_watch && link_ms() >= efs_next) {
		int w = 0;
		efs_next = link_ms() + 2000;
		if (efs_changed(efs_m, &w)) {
			printf("[%6llu] loc: ENGINE LOCK %s reached EFS: modemst%d.img written %llu s after the SET was confirmed\n",
			       now_ms(), efs_why, w, (unsigned long long)(link_ms() - efs_t0) / 1000);
			if (efs_watch == 2 && lk == LK_EFS) {
				unlink(LOCK_ORIG);
				printf("[%6llu] loc: engine lock: %s removed\n", now_ms(), LOCK_ORIG);
				lk = LK_IDLE;
			}
			efs_watch = 0;
			status_dirty = 1;
		} else if (link_ms() - efs_t0 >= EFS_WAIT_MS) {
			printf("[%6llu] loc: engine lock: no EFS write %d s after the %s: the modem holds it in RAM only; do not hard-"
			       "reboot yet%s\n", now_ms(), EFS_WAIT_MS / 1000, efs_why,
			       efs_watch == 2 ? " (" LOCK_ORIG " kept: the next start checks and repairs)" : "");
			if (lk == LK_EFS)
				lk = LK_IDLE;
			efs_watch = 0;
			status_dirty = 1;
		}
	}
}

static void lk_poll(void)
{
	if (!srv_up || cur.busy || lkq.pending || lk_active())
		return;
	int idle = qn == 0 && sess == 0;
	char a[96];
	if (lk == LK_EFS && on_present && w_unlock && !shutting && !dropped && lk_gen_done != on_gen) {
		printf("[%6llu] loc: engine lock: a new unlock word while waiting for the EFS write: %s kept as the original\n",
		       now_ms(), LOCK_ORIG);
		lk = LK_IDLE;
		efs_watch = 0;
	}
	if ((lk == LK_FAILED || lk == LK_NOOP) && lk_gen_done != on_gen)
		lk = LK_IDLE;
	if (lk == LK_RS_FAILED && lk_fail_gen != on_gen && !shutting)
		lk = lk_mode ? LK_UNLOCKED : LK_IDLE;
	if (lk == LK_UNLOCKED && lk_verify && idle) {	/* the server went away mid-change: read back first */
		lk_try = 2;
		lk_get(0, "GET_ENGINE_LOCK (check after the LOC server came back)");
		lk = LK_CHK;
		return;
	}
	if (lk_restore_wanted()) {
		if (!idle)
			return;		/* the session's STOP + REG_EVENTS 0 go first */
		if (lk_rs_tries >= 3) {
			lk_fail(LK_RS_FAILED, "RESTORE FAILED 3 times: " LOCK_ORIG " kept");
			return;
		}
		lk_restore_start(shutting ? "restore: shutdown" : !on_present ? "restore: loc.on gone" : "restore: unlock word changed");
		return;
	}
	if (lk != LK_IDLE || !idle)
		return;
	if (on_present && w_unlock && !shutting && !dropped && lk_gen_done != on_gen) {
		lk_gen_done = on_gen;
		lk_mode = w_unlock;
		lk_rs_tries = 0;
		lk_verify = 0;
		printf("[%6llu] loc: engine lock: unlock=%s requested: reading the lock first\n", now_ms(), w_unlock == 2 ? "none" : "mi");
		lk_get(0, "GET_ENGINE_LOCK (before unlock)");
		lk = LK_RD;
		return;
	}
	if (lk_repair_wanted()) {
		uint64_t m = 0;
		unsigned tr = 0;
		struct lk_file rf;
		lk_repair_gen = on_gen;
		int r = lk_load_ex(LOCK_ORIG, &rf);
		if (!r) {
			m = rf.mask;
			tr = rf.tries;
		}
		if (r) {
			lk_fail(LK_RS_FAILED, LOCK_ORIG " unreadable: no repair (check GET_ENGINE_LOCK, then remove it by hand)");
			return;
		}
		if (tr >= 5) {
			lk_fail(LK_RS_FAILED, "repair given up after 5 runs (" LOCK_ORIG " kept; check by hand)");
			return;
		}
		rf.tries = tr + 1;
		lk_save_ex(LOCK_ORIG, &rf);
		lk_orig = m;
		lk_orig_kind = rf.kind;
		lk_mode = 0;
		lk_rs_tries = 0;
		printf("[%6llu] loc: engine lock: repair run %u: %s says the original was %s\n", now_ms(), tr + 1, LOCK_ORIG,
		       lk_str(m, a, sizeof(a)));
		lk_get(0, "GET_ENGINE_LOCK (repair)");
		lk = LK_RP_RD;
	}
}

/* sweetlocd.c: 1 while an engine lock change/restore/repair is running or due (shutdown waits longer for it) */
int loc_lock_busy(void)
{
	return (srv_up && (lk_active() || lkq.pending || lk_restore_wanted() || lk_repair_wanted() || (cur.busy && cur.lk))) ||
	       (lk == LK_EFS && !shutting);	/* ws-gnss-f: stay up for the EFS write-back (not at SIGTERM) */
}

static void read_on(void)
{
	char b[256] = "";
	FILE *f = fopen(LOC_ON, "r");
	int present = f != NULL;
	if (f) {
		size_t r = fread(b, 1, sizeof(b) - 1, f);
		b[r] = 0;
		fclose(f);
	}
	if (present == on_present && !strcmp(b, on_text))
		return;
	on_present = present;
	snprintf(on_text, sizeof(on_text), "%s", b);
	on_gen++;
	const char *w;
	w_probe = strstr(b, "probe=1") != NULL;
	w_pds = strstr(b, "pds=1") != NULL;
	w_session = (w = strstr(b, "session=")) ? atoi(w + 8) : 0;
	w_unlock = 0;
	if ((w = strstr(b, "unlock="))) {
		size_t k = strcspn(w + 7, " \t\r\n");
		if (k == 2 && !strncmp(w + 7, "mi", 2))
			w_unlock = 1;
		else if (k == 4 && !strncmp(w + 7, "none", 4))
			w_unlock = 2;
		else
			printf("[%6llu] loc: %s: unknown %.*s ignored (only unlock=mi or unlock=none)\n", now_ms(), LOC_ON, (int)(k + 7), w);
	}
	w_client = 0;
	if ((w = strstr(b, "client="))) {
		size_t k = strcspn(w + 7, " \t\r\n");
		if (k == 3 && !strncmp(w + 7, "afw", 3))
			w_client = LOC_CLIENT_AFW;
		else if (k == 3 && !strncmp(w + 7, "nfw", 3))
			w_client = LOC_CLIENT_NFW;
		else
			printf("[%6llu] loc: %s: unknown %.*s ignored (only client=afw or client=nfw)\n", now_ms(), LOC_ON, (int)(k + 7), w);
	}
	w_clientid = strstr(b, "clientid=1") != NULL;
	w_opmode = 0;
	if ((w = strstr(b, "opmode="))) {
		size_t k = strcspn(w + 7, " \t\r\n");
		if (k == 10 && !strncmp(w + 7, "standalone", 10))
			w_opmode = 1;
		else
			printf("[%6llu] loc: %s: %.*s ignored (only opmode=standalone is ever sent)\n", now_ms(), LOC_ON, (int)(k + 7), w);
	}
	w_start_android = strstr(b, "start=android") != NULL;
	w_lockclient = strstr(b, "lockclient=1") != NULL;
	w_lockev = strstr(b, "lockev=1") != NULL;
	w_time = strstr(b, "time=1") != NULL;
	w_timeunc = 5000;
	if ((w = strstr(b, "timeunc=")))
		w_timeunc = (uint32_t)strtoul(w + 8, NULL, 10);
	if (w_timeunc < 20)
		w_timeunc = 20;
	if (w_timeunc > 60000)
		w_timeunc = 60000;
	if (w_session > 600)
		w_session = 600;
	if (dropped)
		printf("[%6llu] loc: %s changed: LOC allowed again\n", now_ms(), LOC_ON);
	dropped = 0;
	after_drop = 0;
	fails = 0;
	printf("[%6llu] loc: %s %s:%s%s%s%s session=%d\n", now_ms(), LOC_ON, present ? "present" : "absent",
	       w_probe ? " probe=1" : "", w_pds ? " pds=1" : "", w_unlock == 1 ? " unlock=mi" : w_unlock == 2 ? " unlock=none" : "",
	       (!w_probe && !w_pds && !w_session && !w_unlock) ? " (no words)" : "", w_session);
	if (w_time)
		printf("[%6llu] loc: ws-gnss-g words: time=1 timeunc=%u ms (INJECT_UTC_TIME at session start and on INJECT_TIME_REQ)\n",
		       now_ms(), w_timeunc);
	if (w_client || w_clientid || w_opmode || w_start_android || w_lockclient || w_lockev)
		printf("[%6llu] loc: ws-gnss-f words:%s%s%s%s%s%s\n", now_ms(), w_client == LOC_CLIENT_AFW ? " client=afw" :
		       w_client == LOC_CLIENT_NFW ? " client=nfw" : "", w_clientid ? " clientid=1" : "", w_opmode ? " opmode=standalone" : "",
		       w_start_android ? " start=android" : "", w_lockclient ? " lockclient=1" : "", w_lockev ? " lockev=1" : "");
	status_dirty = 1;
}

static void write_status(void)
{
	if (!status_dirty || link_ms() < status_at)
		return;
	status_dirty = 0;
	status_at = link_ms() + 2000;
	if (!on_present) {
		unlink(LOC_STATUS);
		return;
	}
	FILE *f = fopen(LOC_STATUS ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "loc: t+%llus server %s%s, words%s%s session=%d, %s, session %s%s, interval %u ms, fixes %u (good %u), "
		   "pds ind %u, engine %s, svs %u, fails %d\n",
		now_ms() / 1000, srv_up ? "up" : "down", srv_up ? "" : " (no LOC server seen yet)", w_probe ? " probe=1" : "",
		w_pds ? " pds=1" : "", w_session, dropped ? "DROPPED" : "ok",
		sess == 0 ? "idle" : sess == 1 ? "starting" : sess == 2 ? "running" : "stopping",
		sess_owner == 1 ? " (locationd)" : sess_owner == 2 ? " (test)" : "", sess_interval, nfix_all, nfix_good, npos_sent,
		engine == 1 ? "on" : engine == 2 ? "off" : "?", last_nsv, fails);
	{
		char a[96], b[96];
		fprintf(f, "loc: engine lock %s (word %s), modem %s, original %s%s\n", lk_names[lk],
			w_unlock == 1 ? "unlock=mi" : w_unlock == 2 ? "unlock=none" : "none",
			lk_now_known ? lk_str(lk_now_mask, a, sizeof(a)) : "not read", lk_orig || lk_now_known ? lk_str(lk_orig, b, sizeof(b)) : "-",
			access(LOCK_ORIG, F_OK) ? "" : ", " LOCK_ORIG " present");
		fprintf(f, "loc: ws-gnss-f client %s%s, opmode %s, start %s, lockclient %d, lockev %d, original kind %s, EFS watch %s\n",
			w_client == LOC_CLIENT_AFW ? "afw" : w_client == LOC_CLIENT_NFW ? "nfw" : "none (modem default NFW)",
			w_clientid ? " +id HAL" : "", w_opmode ? "standalone" : "not sent", w_start_android ? "android" : "ours", w_lockclient,
			w_lockev, lk_orig_kind & LK_KIND_CLIENT ? "lockClient" : lk_orig_kind & LK_KIND_TYPE ? "lockType" : "-",
			efs_watch == 2 ? "restore pending" : efs_watch == 1 ? "unlock pending" : "idle");
	}
	fprintf(f, "loc: ws-gnss-g time %s (unc %u ms): modem asked %u, injected %u, accepted %u; XTRA requests %u, position "
		   "requests %u\n", w_time ? "on" : "off", w_timeunc, n_time_req, n_time_inj, n_time_ok, n_xtra_req, n_pos_req);
	if (sv_have) {
		char sb[200];
		loc_sv_summary_str(&sv_last, sb, sizeof(sb));
		fprintf(f, "loc: sv reports %u, last: %s\n", n_sv, sb);
	}
	if (have_last)
		fprintf(f, "loc: last fix status %u lat %.2f lon %.2f unc %.0f m alt %.0f m tech 0x%x\n", last.status,
			last.has_lat ? last.lat : 0.0, last.has_lon ? last.lon : 0.0, last.has_unc ? last.unc : -1.0f,
			last.has_alt_ell ? last.alt_ell : 0.0f, last.tech);
	if (pds_inited)
		fprintf(f, "loc: pds client %s pipe %u sess %u cid %u, svc %d auto %d cancelled %d, def interval %u accuracy %u, "
			   "requests %u\n", pds.client.valid ? "known" : "none", pds.client.pipe, pds.client.sess, pds.client.cid,
			pds.svc_on, pds.auto_on, pds.cancelled, pds.def_interval, pds.def_accuracy, pds.nreq);
	fclose(f);
	rename(LOC_STATUS ".tmp", LOC_STATUS);
}

int loc_print(void)
{
	char line[600];
	FILE *f = fopen(LOC_STATUS, "r");
	if (!f) {
		printf("loc: no %s (%s %s; sweetlocd running?)\n", LOC_STATUS, LOC_ON, access(LOC_ON, F_OK) ? "absent" : "present");
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		fputs(line, stdout);
	fclose(f);
	return 0;
}

void loc_poll(void)
{
	uint64_t now = link_ms();
	if (!pds_inited) {
		pdsemu_init(&pds);
		pds_inited = 1;
	}
	if (now >= next_check) {
		next_check = now + 10000;
		read_on();
	}
	/* timeouts */
	if (cur.busy) {
		if (!cur.got_resp && now - cur.sent > RESP_MS) {
			printf("[%6llu] loc: no response to 0x%04x %s in %d ms\n", now_ms(), cur.id, cur.why, RESP_MS);
			cur.busy = 0;
			if (cur.lk) {
				lk_event(0, NULL);
			} else {
				if (cur.id == LOC_START && sess == 1)
					sess = 0;
				fail("request timeout");
			}
		} else if (cur.got_resp && cur.want_ind && now - cur.resp_at > IND_MS) {
			printf("[%6llu] loc: no indication for 0x%04x in %d ms (response only)\n", now_ms(), cur.id, IND_MS);
			cur.busy = 0;
			if (cur.lk)
				lk_event(0, NULL);
		}
	}
	int want_pds = 0, want_test = 0, hold = lk_hold();
	uint32_t ivl = 1000;
	if (on_present && !dropped && srv_up && !shutting) {
		want_pds = w_pds && pdsemu_want_session(&pds, &ivl);
		if (w_session && sess_gen_done != on_gen && sess == 0 && !want_pds && !hold) {
			sess_gen_done = on_gen;
			want_test = 1;
		}
		if (w_probe && probe_gen_done != on_gen) {
			probe_gen_done = on_gen;
			probe_queue();
		}
	}
	/* session control */
	if (sess == 0 || sess == 3) {
		if (sess == 3 && !cur.busy && !qn)
			sess = 0;
		if (sess == 0 && want_pds && !hold)
			session_start(1, ivl);
		else if (sess == 0 && want_test) {
			session_start(2, 1000);
			test_until = now + (uint64_t)w_session * 1000;
		}
	} else if (lk_restore_wanted()) {
		session_stop("engine lock restore");	/* restore after the STOP; a new session waits for it */
	} else if (sess_owner == 0) {
		/* a session whose owner went (dropped/server gone resets sess itself): stop it */
		session_stop("no owner");
	} else if (sess_owner == 1 && !want_pds) {
		session_stop(!on_present || !w_pds ? "pds word gone" : "locationd stopped");
	} else if (sess_owner == 2 && (now >= test_until || nfix_good >= 5 || !on_present || want_pds)) {
		session_stop(nfix_good >= 5 ? "5 good fixes" : now >= test_until ? "time is up" : want_pds ? "locationd wants it" : "loc.on gone");
	} else if (sess_owner == 1 && ivl != sess_interval && sess == 2) {
		session_stop("interval changed");	/* started again at the new interval next round */
	}
	if (!on_present && sess == 0)
		qn = 0;		/* probe requests still queued: not sent once the file is gone */
	efs_poll();
	lk_poll();
	send_next();
	kr_poll(now);		/* xnu 1122 */
	write_status();
}

void loc_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	if (service != LOC_SVC || srv_up)
		return;
	srv_up = 1;
	srv_node = node;
	srv_port = port;
	srv_inst = instance;
	rev_sent = 0;
	probe_gen_done = ~0ull;	/* a new server: probe again */
	if (on_present)
		printf("[%6llu] loc: LOC server (instance %#x) at %u:%x\n", now_ms(), instance, node, port);
	status_dirty = 1;
}

void loc_server_gone(uint32_t node, uint32_t port)
{
	if (!srv_up || srv_node != node || (port != 0xffffffffu && srv_port != port))
		return;
	srv_up = 0;
	memset(&cur, 0, sizeof(cur));
	qn = 0;
	sess = 0;
	sess_owner = 0;
	lkq.pending = 0;
	if (lk == LK_RD || lk == LK_RD_DV) {		/* nothing changed yet: read again when it is back */
		lk = LK_IDLE;
		lk_gen_done = ~0ull;
	} else if (lk == LK_RP_RD) {
		lk = LK_IDLE;
		lk_repair_gen = ~0ull;
	} else if (lk == LK_SET || lk == LK_CHK) {	/* unknown: read back when it is back, restore if not as wanted */
		lk = LK_UNLOCKED;
		lk_verify = 1;
	} else if (lk == LK_RS_SET || lk == LK_RS_CHK) {	/* restore again when it is back */
		lk = LK_UNLOCKED;
	}
	if (lk != LK_IDLE)
		printf("[%6llu] loc: LOC server gone during engine lock work: state now %s\n", now_ms(), lk_names[lk]);
	if (on_present)
		printf("[%6llu] loc: LOC server gone (session state dropped)\n", now_ms());
	status_dirty = 1;
}

static void on_position(const uint8_t *tl, uint32_t tn)
{
	struct loc_fix f;
	if (loc_parse_position(tl, tn, &f)) {
		printf("[%6llu] loc: POSITION_REPORT without a session status TLV, ignored\n", now_ms());
		return;
	}
	nfix_all++;
	if (f.status == 0 && f.has_lat && f.has_lon)
		nfix_good++;
	last = f;
	have_last = 1;
	status_dirty = 1;
	if (nfix_all <= 3 || nfix_all % 30 == 0 || f.status > 1)
		printf("[%6llu] loc: fix #%u status %u lat %.2f lon %.2f unc %.0f m tech 0x%x utc %s\n", now_ms(), nfix_all, f.status,
		       f.has_lat ? f.lat : 0.0, f.has_lon ? f.lon : 0.0, f.has_unc ? f.unc : -1.0f, f.tech, f.has_utc ? "yes" : "no");
	if (sess_owner != 1 || !pds.client.valid)
		return;
	if (f.status > 1) {	/* failure reports: at most one per 10 s */
		if (link_ms() - last_fail_ind < 10000)
			return;
		last_fail_ind = link_ms();
	}
	uint8_t b[256];
	uint32_t n = pdsemu_event_report(&f, b, sizeof(b));
	if (!n)
		return;
	if (pds_send_ind(pds.client.pipe, pds.client.sess, PDS_SVC, pds.client.cid, PDS_EVENT_REPORT, b, n) == 0) {
		npos_sent++;
		pds.nind++;
		if (npos_sent <= 3)
			printf("[%6llu] loc: EVENT_REPORT to locationd (pipe %u sess %u cid %u): %u TLV bytes, status %u\n", now_ms(),
			       pds.client.pipe, pds.client.sess, pds.client.cid, n, f.status);
	}
}

/* ws-gnss-g: a server list TLV (time servers / XTRA servers) as text: after `skip` bytes (delayThreshold), a count byte,
 * then URL strings; printed as their printable characters with '|' between strings, so the string encoding needs no guess */
static void assist_log(const struct qmi_msg *m, const char *what, unsigned id, unsigned skip)
{
	uint16_t l = 0;
	const uint8_t *v = qmi_get(m, (uint8_t)id, &l);
	char b[800];
	size_t o = 0;
	if (!v || l <= skip) {
		printf("[%6llu] loc: %s: no server list (TLV %#x)\n", now_ms(), what, id);
		return;
	}
	if (skip >= 4)
		printf("[%6llu] loc: %s: delayThreshold %u ms\n", now_ms(), what,
		       (unsigned)(v[0] | v[1] << 8 | v[2] << 16 | (unsigned)v[3] << 24));
	int sep = 1;
	for (uint16_t i = (uint16_t)(skip + 1); i < l && o + 2 < sizeof(b); i++) {
		if (v[i] >= 0x20 && v[i] < 0x7f) {
			b[o++] = (char)v[i];
			sep = 0;
		} else if (!sep) {
			b[o++] = '|';
			sep = 1;
		}
	}
	b[o] = 0;
	printf("[%6llu] loc: %s: %u server(s): %s\n", now_ms(), what, v[skip], b);
}

void loc_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type == QMI_RESPONSE) {
		if (!cur.busy || m.txn != cur.txn) {
			printf("[%6llu] loc: late/unknown response 0x%04x txn %u\n", now_ms(), m.id, m.txn);
			return;
		}
		uint16_t l = 0;
		const uint8_t *r = qmi_get(&m, 0x02, &l);
		unsigned res = r && l >= 4 ? (unsigned)(r[0] | r[1] << 8) : 1, err = r && l >= 4 ? (unsigned)(r[2] | r[3] << 8) : 0xffff;
		printf("[%6llu] loc: < 0x%04x response %s (error 0x%x)\n", now_ms(), m.id, res ? "FAILED" : "ok", err);
		qmi_dump(&m);		/* ws-gnss-f: every response in hex */
		if (cur.lk) {
			if (res) {
				cur.busy = 0;
				lk_event(0, NULL);
			} else {
				cur.got_resp = 1;
				cur.resp_at = link_ms();
			}
			return;
		}
		if (res && m.id == LOC_INJECT_UTC_TIME) {	/* ws-gnss-g: a refused inject is logged, never a LOC failure */
			cur.busy = 0;
			printf("[%6llu] loc: INJECT_UTC_TIME refused by the modem (error 0x%x)\n", now_ms(), err);
			return;
		}
		if (res) {
			cur.busy = 0;
			if (m.id == LOC_START && sess == 1) {
				sess = 0;
				sess_owner = 0;
			}
			fail("QMI error");
			return;
		}
		fails = 0;
		if (m.id == LOC_START && sess == 1) {
			sess = 2;
			status_dirty = 1;
		}
		cur.got_resp = 1;
		cur.resp_at = link_ms();
		if (!cur.want_ind)
			cur.busy = 0;
		return;
	}
	if (m.type != QMI_INDICATION)
		return;
	uint16_t l0 = 0;
	if (cur.busy && cur.got_resp && cur.want_ind && m.id == cur.id) {
		printf("[%6llu] loc: < 0x%04x indication (%s)\n", now_ms(), m.id, cur.why);
		qmi_dump(&m);
		if (m.id == LOC_GET_ENGINE_LOCK && !cur.lk) {	/* probe reads: the value in words too */
			struct lk_val pv;
			char pb[96];
			lk_parse(&m, &pv);
			int64_t pm = lk_mask(&pv);
			printf("[%6llu] loc: engine lock (%s): %s%s%s%s\n", now_ms(), cur.why, pm >= 0 ? lk_str((uint64_t)pm, pb, sizeof(pb)) : "no value",
			       pv.has_type ? " (lockType TLV)" : "", pv.has_sub ? " (subType TLV)" : "", pv.has_client ? " (lockClient TLV)" : "");
		}
		if (m.id == LOC_INJECT_UTC_TIME) {
			const uint8_t *sv = qmi_get(&m, 0x01, &l0);
			unsigned stv = sv && l0 >= 4 ? (unsigned)(sv[0] | sv[1] << 8) : 0xffff;
			if (stv == 0)
				n_time_ok++;
			printf("[%6llu] loc: INJECT_UTC_TIME indication status %u%s\n", now_ms(), stv, stv == 0 ? " (success)" : " (NOT accepted)");
		}
		cur.busy = 0;
		if (cur.lk)
			lk_event(1, &m);
		return;
	}
	uint16_t l = 0;
	const uint8_t *v;
	static unsigned n_pos_dump, n_sv_dump;
	if (m.id == LOC_IND_POSITION || m.id == LOC_IND_SV_INFO) {
		unsigned *c = m.id == LOC_IND_POSITION ? &n_pos_dump : &n_sv_dump;
		if (++*c <= 5 || *c % 60 == 0)
			dump_ids(&m);
	} else {
		qmi_dump(&m);	/* ws-gnss-f: every other indication in hex (engine state, fix session, engine lock state, ...) */
	}
	switch (m.id) {
	case LOC_IND_POSITION:
		on_position(m.tlv, m.len);
		break;
	case LOC_IND_SV_INFO: {
		if ((v = qmi_get(&m, 0x10, &l)) && l >= 1 && last_nsv != v[0]) {
			last_nsv = v[0];
			status_dirty = 1;
		}
		struct loc_sv_sum ss;	/* ws-gnss-g: one summary line per report (no elevation/azimuth, no positions) */
		char sb[200];
		int sr = loc_sv_summary(m.tlv, m.len, &ss);
		n_sv++;
		if (sr == 0) {
			loc_sv_summary_str(&ss, sb, sizeof(sb));
			printf("[%6llu] loc: sv #%u: %s\n", now_ms(), n_sv, sb);
			sv_last = ss;
			sv_have = 1;
			status_dirty = 1;
		} else if (n_sv <= 5 || n_sv % 60 == 0) {
			printf("[%6llu] loc: sv #%u: %s (entry size %u)\n", now_ms(), n_sv, sr == -1 ? "no svList TLV" :
			       "svList entries not 28 bytes: not parsed", ss.entry_size);
		}
		break;
	}
	case LOC_IND_INJECT_TIME_REQ:	/* ws-gnss-g: the modem asks for time */
		n_time_req++;
		assist_log(&m, "INJECT_TIME_REQ", 0x10, 4);
		if (w_time && (sess == 1 || sess == 2) && (!time_last_inject || link_ms() - time_last_inject >= 10000))
			time_queue("modem INJECT_TIME_REQ");
		else if (!w_time)
			printf("[%6llu] loc: INJECT_TIME_REQ: time=1 not set, not answered\n", now_ms());
		status_dirty = 1;
		break;
	case LOC_IND_XTRA_REQ:		/* ws-gnss-g: logged only (no XTRA download in this step) */
		n_xtra_req++;
		if ((v = qmi_get(&m, 0x01, &l)) && l >= 8)
			printf("[%6llu] loc: INJECT_PREDICTED_ORBITS_REQ: max file %u bytes, max part %u bytes\n", now_ms(),
			       (unsigned)(v[0] | v[1] << 8 | v[2] << 16 | (unsigned)v[3] << 24),
			       (unsigned)(v[4] | v[5] << 8 | v[6] << 16 | (unsigned)v[7] << 24));
		assist_log(&m, "INJECT_PREDICTED_ORBITS_REQ", 0x10, 0);
		status_dirty = 1;
		break;
	case LOC_IND_INJECT_POS_REQ:	/* ws-gnss-g: logged only (never injected) */
		n_pos_req++;
		printf("[%6llu] loc: INJECT_POSITION_REQ from the modem (not answered)\n", now_ms());
		status_dirty = 1;
		break;
	case LOC_IND_ENGINE_STATE:
		if ((v = qmi_get(&m, 0x01, &l)) && l == 4) {
			unsigned e = v[0] | v[1] << 8;
			if (e != engine)
				printf("[%6llu] loc: engine %s\n", now_ms(), e == 1 ? "on" : e == 2 ? "off" : "?");
			engine = e;
			status_dirty = 1;
		}
		break;
	case LOC_IND_FIX_SESSION:
		break;
	default:
		printf("[%6llu] loc: < indication 0x%04x (%u bytes)%s\n", now_ms(), m.id, m.len,
		       m.id == LOC_IND_ENGINE_LOCK_STATE ? " ENGINE_LOCK_STATE" : "");
		break;
	}
	(void)h;
}

int loc_pds_request(uint32_t pipe, uint32_t sess_id, unsigned cid, const uint8_t *sdu, uint32_t sn, int *fl, int *code,
		    const uint8_t **tl, uint16_t *tn, const char **what)
{
	static struct pds_answer a;
	if (!loc_pds_on())
		return 0;
	if (!pds_inited) {
		pdsemu_init(&pds);
		pds_inited = 1;
	}
	struct pds_client c = { pipe, sess_id, cid, 1 };
	if (!pdsemu_request(&pds, &c, sdu, sn, &a))
		return 0;
	*fl = a.fail;
	*code = a.code;
	*tl = a.tn ? a.tl : NULL;
	*tn = a.tn;
	*what = a.what;
	status_dirty = 1;
	return 1;
}

void loc_pdsr_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct pdsr r;
	(void)h;
	if (n < sizeof(r))
		return;
	memcpy(&r, d, sizeof(r));
	if (r.magic != PDSR_MAGIC || r.len > n - sizeof(r))
		return;
	const uint8_t *data = d + sizeof(r);
	switch (r.op) {
	case 0x84:
		if (kr_ack != (int)r.flags)
			printf("[%6llu] loc: PDS relay: kernel says kern.sweet_qmux_pds %u%s (%s)\n", now_ms(), r.flags & 0xff,
			       (r.flags & 0x100) ? ", busy" : "", (r.flags & 0x100) ? "another client (wland?) has the relay" :
			       r.flags == 1 ? "locationd's PDS comes here" : "the kernel answers PDS itself");
		kr_ack = r.flags;
		break;
	case 0x81: {
		int fl = 0, code = 0;
		const uint8_t *tl = NULL;
		uint16_t tn = 0;
		const char *w = "";
		kr_reqs++;
		int mine = loc_pds_request(r.pipe, r.sess, r.cid, data, r.len, &fl, &code, &tl, &tn, &w);
		unsigned msg = r.len >= 5 ? (unsigned)(data[3] | data[4] << 8) : 0;
		if (mine)
			kr_mine++;
		if (kr_reqs <= 40)
			printf("[%6llu] loc: PDS relay: 0x%04x from pipe %u sess %u cid %u -> %s%s\n", now_ms(), msg, r.pipe, r.sess,
			       r.cid, mine ? (fl ? "error " : "answered ") : "not ours (kernel policy)", mine ? w : "");
		kr_send(2, r.cid, (mine ? 1u : 0u) | (mine && fl ? 2u : 0u), r.pipe, r.sess, r.tag, (unsigned)code, tl, tl ? tn : 0);
		break;
	}
	case 0x82:
		printf("[%6llu] loc: PDS relay: pipe %u sess %u gone (%.*s)\n", now_ms(), r.pipe, r.sess, r.len < 40 ? r.len : 40,
		       (const char *)data);
		loc_pds_session_gone(r.pipe, r.sess);
		break;
	case 0x83:
		printf("[%6llu] loc: PDS relay: pipe %u sess %u PDS client id %u %s\n", now_ms(), r.pipe, r.sess, r.cid,
		       r.flags ? "allocated" : "released");
		if (!r.flags && pds.client.valid && pds.client.pipe == r.pipe && pds.client.sess == r.sess && pds.client.cid == r.cid)
			loc_pds_session_gone(r.pipe, r.sess);
		break;
	default:
		break;
	}
}

void loc_pds_session_gone(uint32_t pipe, uint32_t sess_id)
{
	if (pds_inited)
		pdsemu_client_gone(&pds, pipe, sess_id);
}

/* sweetlocd.c */
int loc_on_present(void)
{
	return on_present;
}

/* SIGTERM / exit: stop a running session (STOP + REG_EVENTS 0 queued, sent by loc_poll) and say BYE to the relay */
void loc_shutdown(const char *why)
{
	if (shutting)
		return;
	shutting = 1;
	printf("[%6llu] loc: shutdown (%s)\n", now_ms(), why);
	if (sess == 1 || sess == 2)
		session_stop(why);
	else
		sess_owner = 0;
	for (int i = 0; i < qn;)	/* probe GETs still queued are not sent any more */
		if (q[i].id != LOC_STOP && q[i].id != LOC_REG_EVENTS) {
			memmove(q + i, q + i + 1, (size_t)(qn - i - 1) * sizeof(q[0]));
			qn--;
		} else {
			i++;
		}
	kr_poll(link_ms());
	status_dirty = 1;
}

/* ws-gnss-g: 1 while the only engine lock work left is waiting for the modem's EFS write after a confirmed restore
 * (nothing is sent meanwhile; efs_poll ends it at the write or after EFS_WAIT_MS) */
int loc_efs_waiting(void)
{
	return lk == LK_EFS && !shutting && !(srv_up && (lk_active() || lkq.pending || (cur.busy && cur.lk)));
}

/* 1 = nothing in flight or queued for the modem (safe to close the device) */
int loc_quiet(void)
{
	return !cur.busy && qn == 0 && !loc_lock_busy();
}
