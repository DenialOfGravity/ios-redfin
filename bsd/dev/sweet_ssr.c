/*
 * redfin 1070 (P4 phase 2a, research/cc/P4-phase2a-notes.md): the modem's crash reason, and the subsystem up/down
 * notices between the modem (MPSS) and the audio DSP in Android's order. One kernel thread "sweetssr", started only when
 * one of the switches below is on. Log lines "SWEET MDM:" (crash reader) and "SWEET SSR:" (notices).
 *
 * Crash reader (kern.sweet_mdm_crash, boot-arg sweetmdmcrash; default 0 = nothing runs). Read-only: nothing is written to
 * SMEM, SMP2P or the modem (Linux qcom_q6v5 clears the first byte of the reason after reading it; we do not).
 *   bit 1  watch: every 100 ms the modem's SMP2P item (435, entry "master-kernel": bit 0 err-fatal, 1 ready, 2 handover,
 *          3 stop-ack, 7 shutdown-ack; Linux q6v5 "fatal"/"ready"/...) is read; every change is logged, and on err-fatal,
 *          on "ready" going away, or when the modem's SSCTL server goes (kernel link), SMEM item 421 (the modem's crash
 *          reason, qcom_q6v5_pas crash_reason_smem 421, QCOM_SMEM_HOST_ANY) is read and logged:
 *          "SWEET MDM: crash reason: ...". Item 421 is also checked every 1 s and logged whenever its text changes
 *          (a watchdog bite has no SMP2P bit).
 *   write 2  read item 421 now, once (any time, watch on or off).
 *
 * Notices (kern.sweet_ssr_notify, boot-arg sweetssr; default 0). Linux qcom_sysmon: when a remote processor starts, the
 * processors already running are told "<it> after-powerup" (sysmon_notify), then it is told "<each running one>
 * after-powerup" (sysmon_start); when one stops, the others are told "<it> before-shutdown" and "<it> after-shutdown".
 * Transport: SSCTL v2 (QMI service 0x2b; modem instance 0x12, DSP instance 0x14), SUBSYS_EVENT_REQ 0x23 with
 * TLV 1 name, TLV 2 event (0 before-powerup, 1 after-powerup, 2 before-shutdown, 3 after-shutdown), TLV 0x10 driven 0
 * (forced), answered by a response with TLV 2 result; 5 s timeout (Linux SSCTL_TIMEOUT... as qcom_sysmon). Sent from the
 * kernel only: the modem's on the phase-1 kernel QRTR (sweetbb & 1; client port 0x4500), the DSP's on sweet_adsp.c's QRTR
 * (client port 0x4002). The modem counts as up when its SMP2P "ready" bit is set and err-fatal is not; the DSP when its
 * SSCTL server is known.
 *   bit 1  tell the DSP about the modem ("modem after-powerup", on a modem stop "modem before-/after-shutdown")
 *   bit 2  tell the modem about the DSP ("adsp after-powerup", ...)
 *   bit 4  timing: wait until both are up and send in Linux's order (the processor that came up last is announced to the
 *          other first, then told about the other); again after every restart of either. Without 4, a write of 1/2 sends
 *          the after-powerup notice(s) once, right away (if the target's SSCTL server is there).
 *   bit 8  answer SSCTL requests sent to our client ports with QMI_RESULT_SUCCESS (Linux answers none; default: log only)
 *   Safety interlock: refused (EPERM, and every send re-checked) unless kern.sweet_ufs_recover = 3 (1069: log + retry + host
 *   reset) and kern.sweet_mdm_crash & 1 (the crash watch). Everything the modem or the DSP sends to our ports
 *   (responses, indications such as SHUTDOWN_READY_IND 0x21, requests) is logged.
 * kern.sweet_ssr_state: both processors' state, servers, counters, the last crash reason. Nothing here returns to fastboot.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include "sweet_bb.h"
#include "sweet_ssr.h"

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

#define MDM(...)                printf("SWEET MDM: " __VA_ARGS__)
#define SSR(...)                printf("SWEET SSR: " __VA_ARGS__)

#define ITEM_MDM_SMP2P_IN       435u    /* lito-smp2p.dtsi qcom,smp2p-mpss qcom,smem = <435>, <428>: the modem's */
#define ITEM_MDM_CRASH_REASON   421u
#define SMP2P_MAGIC             0x504d5324u
#define MK_FATAL                (1u << 0)
#define MK_READY                (1u << 1)

#define SSCTL_SERVICE           0x2bu
#define SSCTL_INST_MODEM        0x12u
#define SSCTL_INST_ADSP         0x14u
#define SSCTL_SHUTDOWN_READY_IND 0x21u
#define SSCTL_SUBSYS_EVENT_REQ  0x23u
#define QMI_REQ                 0u
#define QMI_RESP                2u
#define QMI_IND                 4u
#define QRTR_DATA               1u
#define QRTR_RESUME_TX          7u
#define NODE_APPS               1u

enum { EV_BEFORE_POWERUP, EV_AFTER_POWERUP, EV_BEFORE_SHUTDOWN, EV_AFTER_SHUTDOWN };
static const char *const ev_name[] = { "before-powerup", "after-powerup", "before-shutdown", "after-shutdown" };
static const char *const pname[] = { "modem", "adsp" };

static int sweet_mdm_crash;
static int sweet_ssr_notify;

static LCK_GRP_DECLARE(ssr_grp, "sweet-ssr");
static LCK_MTX_DECLARE(ssr_lock, &ssr_grp);     /* the receive queue and the thread's start; never held across sbb_/redwood_ */

/* ---- packets to our ports (queued by the links' threads, handled here) ---- */

#define RXQ     16
#define RXMAX   512
static struct rxp {
	unsigned from;
	uint32_t type, node, port, confirm, n;
	uint8_t d[RXMAX];
} rxq[RXQ];
static unsigned rx_head, rx_count;
static uint32_t rx_dropped;

static thread_t ssr_thread_ref;
static bool ssr_started;

static struct {
	/* crash reader */
	bool smp2p_seen;
	uint32_t mk, mk_changes, crash_reads;
	uint32_t reason_sum;
	char reason[200];
	/* notices */
	bool up[2], sent_up[2];         /* sent_up[p]: "p is up" already told to the other one for this epoch */
	uint32_t ups[2], downs[2];
	uint64_t up_since[2];
	uint32_t sent, ok, failed, refused, no_answer, inds, reqs, other;
	uint16_t txn;
	bool srv_mdm;                   /* the modem's SSCTL server, seen on the kernel link */
	bool now_pending;               /* a write without bit 4: send once */
} st;

void
sweet_ssr_qrtr_in(unsigned from, uint32_t type, uint32_t node, uint32_t port, uint32_t confirm, const uint8_t *d, uint32_t n)
{
	lck_mtx_lock(&ssr_lock);
	if (rx_count < RXQ) {
		struct rxp *p = &rxq[(rx_head + rx_count) % RXQ];
		p->from = from;
		p->type = type;
		p->node = node;
		p->port = port;
		p->confirm = confirm;
		p->n = n < RXMAX ? n : RXMAX;
		memcpy(p->d, d, p->n);
		rx_count++;
		wakeup(&rx_count);
	} else {
		rx_dropped++;
	}
	lck_mtx_unlock(&ssr_lock);
}

static bool
rx_pop(struct rxp *out)
{
	bool got = false;
	lck_mtx_lock(&ssr_lock);
	if (rx_count) {
		*out = rxq[rx_head];
		rx_head = (rx_head + 1) % RXQ;
		rx_count--;
		got = true;
	}
	lck_mtx_unlock(&ssr_lock);
	return got;
}

static uint64_t
now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
nap_ms(uint32_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
	lck_mtx_lock(&ssr_lock);
	if (rx_count == 0) {
		(void)msleep(&rx_count, &ssr_lock, PZERO, "sweetssr", &ts);
	}
	lck_mtx_unlock(&ssr_lock);
}

/* ---- crash reader ---- */

/* the modem's SMP2P "master-kernel" value; false if the item or the entry is not there (modem never booted) */
static bool
mdm_master_kernel(uint32_t *v)
{
	uint8_t b[20 + 20 * 24];
	uint32_t size = 0;
	if (sbb_smem_copy(ITEM_MDM_SMP2P_IN, b, sizeof(b), &size) != 0 || size < 20) {
		return false;
	}
	uint32_t magic, valid;
	memcpy(&magic, b, 4);
	valid = (uint32_t)b[14] | (uint32_t)b[15] << 8;
	if (magic != SMP2P_MAGIC) {
		return false;
	}
	for (uint32_t i = 0; i < valid && i < 24 && 36 + 20 * i + 4 <= MIN(size, sizeof(b)); i++) {
		if (strncmp((const char *)b + 20 + 20 * i, "master-kernel", 16) == 0) {
			memcpy(v, b + 36 + 20 * i, 4);
			return true;
		}
	}
	return false;
}

/* item 421 -> printable text (non-printables as '.'), returns its length; -1 absent */
static int
crash_text(char *out, uint32_t cap, uint32_t *item_size)
{
	uint8_t raw[512];
	uint32_t size = 0;
	if (sbb_smem_copy(ITEM_MDM_CRASH_REASON, raw, sizeof(raw), &size) != 0) {
		*item_size = 0;
		return -1;
	}
	*item_size = size;
	uint32_t n = MIN(size, (uint32_t)sizeof(raw)), k = 0;
	for (uint32_t i = 0; i < n && raw[i] != 0 && k + 1 < cap; i++) {
		out[k++] = (raw[i] >= 0x20 && raw[i] < 0x7f) ? (char)raw[i] : '.';
	}
	out[k] = '\0';
	return (int)k;
}

static void
crash_read(const char *why, bool only_if_changed)
{
	char text[520];
	uint32_t size = 0;
	int len = crash_text(text, sizeof(text), &size);
	uint32_t sum = 5381;
	for (int i = 0; i < len; i++) {
		sum = sum * 33 + (uint8_t)text[i];
	}
	if (only_if_changed && (len <= 0 || sum == st.reason_sum)) {
		return;
	}
	st.crash_reads++;
	st.reason_sum = len > 0 ? sum : st.reason_sum;
	if (len < 0) {
		MDM("crash reason (%s): SMEM item %u not there\n", why, ITEM_MDM_CRASH_REASON);
		return;
	}
	if (len == 0) {
		MDM("crash reason (%s): empty (item %u, %u bytes)\n", why, ITEM_MDM_CRASH_REASON, size);
		return;
	}
	strlcpy(st.reason, text, sizeof(st.reason));
	MDM("crash reason: %s (%s; item %u, %u bytes)\n", text, why, ITEM_MDM_CRASH_REASON, size);
}

static void
crash_watch(uint64_t t, uint64_t *next_reason)
{
	uint32_t mk = 0;
	bool have = mdm_master_kernel(&mk);
	if (have && (!st.smp2p_seen || mk != st.mk)) {
		uint32_t old = st.mk;
		st.mk_changes++;
		MDM("smp2p master-kernel 0x%x -> 0x%x (fatal %u ready %u handover %u stop-ack %u shutdown-ack %u)%s\n", old, mk,
		    mk & 1, (mk >> 1) & 1, (mk >> 2) & 1, (mk >> 3) & 1, (mk >> 7) & 1, st.smp2p_seen ? "" : " (first look)");
		if ((mk & MK_FATAL) && (!st.smp2p_seen || !(old & MK_FATAL))) {
			crash_read("err-fatal", false);
		} else if (st.smp2p_seen && (old & MK_READY) && !(mk & MK_READY)) {
			crash_read("ready gone", false);
		}
		st.mk = mk;
		st.smp2p_seen = true;
	}
	if (t >= *next_reason) {
		*next_reason = t + 1000;
		crash_read("item 421 changed", true);
	}
}

/* ---- SSCTL ---- */

static bool
interlock(const char **why)
{
	if (sweet_ufs_recover < 3) {
		*why = "kern.sweet_ufs_recover is not 3";
		return false;
	}
	if (!(sweet_mdm_crash & 1)) {
		*why = "kern.sweet_mdm_crash watch (bit 1) is off";
		return false;
	}
	return true;
}

static bool
target_server(unsigned to, uint32_t *node, uint32_t *port, uint32_t *version)
{
	if (to == SWEET_SSR_MODEM) {
		return (sweet_bb & SBB_LINK) && sbb_server(SSCTL_SERVICE, SSCTL_INST_MODEM, node, port, version);
	}
	return redwood_qrtr_server(SSCTL_SERVICE, SSCTL_INST_ADSP, node, port, version);
}

static int
qrtr_send(unsigned to, uint32_t type, uint32_t node, uint32_t port, const void *d, uint32_t n)
{
	if (to == SWEET_SSR_MODEM) {
		return sbb_qrtr_send(type, SWEET_SSR_MDM_PORT, node, port, d, n);
	}
	return redwood_qrtr_send(type, SWEET_SSR_ADSP_PORT, node, port, d, n);
}

static void
hexline(char *out, size_t cap, const uint8_t *d, uint32_t n)
{
	size_t at = 0;
	out[0] = '\0';
	for (uint32_t i = 0; i < n && i < 40 && at + 4 < cap; i++) {
		at += (size_t)snprintf(out + at, cap - at, " %02x", d[i]);
	}
}

/* something the modem or the DSP sent to our port; a response with *want_txn is returned in *result (true) */
static bool
handle_rx(const struct rxp *p, int want_txn, uint16_t *res, uint16_t *err)
{
	char hex[3 * 40 + 8];
	hexline(hex, sizeof(hex), p->d, p->n);
	if (p->confirm) {
		uint32_t rt[5] = { QRTR_RESUME_TX, NODE_APPS, p->from == SWEET_SSR_MODEM ? SWEET_SSR_MDM_PORT : SWEET_SSR_ADSP_PORT, 0, 0 };
		qrtr_send(p->from, QRTR_RESUME_TX, p->node, p->port, rt, sizeof(rt));
	}
	if (p->type != QRTR_DATA || p->n < 7) {
		st.other++;
		SSR("<- %s %u:%#x: QRTR type %u, %u bytes:%s\n", pname[p->from], p->node, p->port, p->type, p->n, hex);
		return false;
	}
	uint8_t qt = p->d[0];
	uint16_t txn = (uint16_t)(p->d[1] | p->d[2] << 8), msg = (uint16_t)(p->d[3] | p->d[4] << 8);
	uint16_t len = (uint16_t)(p->d[5] | p->d[6] << 8);
	if (qt == QMI_RESP && want_txn >= 0 && txn == (uint16_t)want_txn) {
		*res = 0xffff;
		*err = 0xffff;
		for (uint32_t at = 7; at + 3 <= p->n && at + 3 <= 7u + len;) {
			uint8_t t = p->d[at];
			uint16_t l = (uint16_t)(p->d[at + 1] | p->d[at + 2] << 8);
			if (t == 2 && l >= 4 && at + 3 + 4 <= p->n) {
				*res = (uint16_t)(p->d[at + 3] | p->d[at + 4] << 8);
				*err = (uint16_t)(p->d[at + 5] | p->d[at + 6] << 8);
			}
			at += 3u + l;
		}
		return true;
	}
	if (qt == QMI_IND) {
		st.inds++;
		SSR("<- %s %u:%#x: indication %#x%s (txn %u, %u bytes):%s\n", pname[p->from], p->node, p->port, msg,
		    msg == SSCTL_SHUTDOWN_READY_IND ? " SHUTDOWN_READY" : "", txn, p->n, hex);
		return false;
	}
	if (qt == QMI_REQ) {
		st.reqs++;
		SSR("<- %s %u:%#x: REQUEST %#x (txn %u, %u bytes):%s -> %s\n", pname[p->from], p->node, p->port, msg, txn, p->n, hex,
		    (sweet_ssr_notify & 8) ? "answered success" : "not answered (as Linux)");
		if (sweet_ssr_notify & 8) {
			uint8_t r[14] = { QMI_RESP, (uint8_t)txn, (uint8_t)(txn >> 8), (uint8_t)msg, (uint8_t)(msg >> 8), 7, 0,
				              2, 4, 0, 0, 0, 0, 0 };
			int e = qrtr_send(p->from, QRTR_DATA, p->node, p->port, r, sizeof(r));
			if (e) {
				SSR("   answer not sent (%d)\n", e);
			}
		}
		return false;
	}
	st.other++;
	SSR("<- %s %u:%#x: QMI type %u msg %#x txn %u (late or unexpected), %u bytes:%s\n", pname[p->from], p->node, p->port, qt, msg,
	    txn, p->n, hex);
	return false;
}

/* tell processor `to` that `about` had event ev (Linux ssctl_send_event) */
static void
ssctl_event(unsigned to, unsigned about, unsigned ev)
{
	const char *why = NULL;
	uint32_t node = 0, port = 0, ver = 0;
	if (!interlock(&why)) {
		st.refused++;
		SSR("-> %s \"%s %s\": REFUSED (%s)\n", pname[to], pname[about], ev_name[ev], why);
		return;
	}
	if (!target_server(to, &node, &port, &ver)) {
		st.refused++;
		SSR("-> %s \"%s %s\": no SSCTL server of the %s known%s\n", pname[to], pname[about], ev_name[ev], pname[to],
		    to == SWEET_SSR_MODEM && !(sweet_bb & SBB_LINK) ? " (needs the kernel modem link, sweetbb & 1)" : "");
		return;
	}
	if (ver != 2) {
		st.refused++;
		SSR("-> %s \"%s %s\": its SSCTL server is version %u, not 2: not sent (as Linux)\n", pname[to], pname[about],
		    ev_name[ev], ver);
		return;
	}
	const char *name = pname[about];
	uint8_t nl = (uint8_t)strlen(name);
	uint8_t m[64];
	uint16_t txn = ++st.txn;
	if (txn == 0) {
		txn = st.txn = 1;
	}
	uint32_t at = 7;
	m[at++] = 1;                            /* TLV 1: subsys_name_len + name */
	m[at++] = (uint8_t)(1 + nl);
	m[at++] = 0;
	m[at++] = nl;
	memcpy(m + at, name, nl);
	at += nl;
	m[at++] = 2;                            /* TLV 2: event */
	m[at++] = 4;
	m[at++] = 0;
	m[at++] = (uint8_t)ev;
	m[at++] = 0;
	m[at++] = 0;
	m[at++] = 0;
	m[at++] = 0x10;                         /* TLV 0x10: evt_driven = SSCTL_SSR_EVENT_FORCED (Linux) */
	m[at++] = 4;
	m[at++] = 0;
	m[at++] = 0;
	m[at++] = 0;
	m[at++] = 0;
	m[at++] = 0;
	m[0] = QMI_REQ;
	m[1] = (uint8_t)txn;
	m[2] = (uint8_t)(txn >> 8);
	m[3] = SSCTL_SUBSYS_EVENT_REQ;
	m[4] = 0;
	m[5] = (uint8_t)(at - 7);
	m[6] = 0;
	st.sent++;
	uint64_t t0 = now_ms();
	int e = qrtr_send(to, QRTR_DATA, node, port, m, at);
	if (e) {
		st.failed++;
		SSR("-> %s %u:%#x \"%s %s\": not sent (error %d)\n", pname[to], node, port, name, ev_name[ev], e);
		return;
	}
	SSR("-> %s %u:%#x \"%s %s\" (txn %u) sent\n", pname[to], node, port, name, ev_name[ev], txn);
	uint64_t mk_next = 0;
	while (now_ms() - t0 < 5000) {
		struct rxp p;
		uint16_t res = 0, err = 0;
		while (rx_pop(&p)) {
			if (p.from == to && handle_rx(&p, txn, &res, &err)) {
				if (res == 0) {
					st.ok++;
				} else {
					st.failed++;
				}
				SSR("<- %s: \"%s %s\": result %u error %#x (%llu ms)\n", pname[to], name, ev_name[ev], res, err, now_ms() - t0);
				return;
			} else if (p.from != to) {
				handle_rx(&p, -1, &res, &err);
			}
		}
		if (sweet_mdm_crash & 1) {
			crash_watch(now_ms(), &mk_next);        /* (keep watching the modem while waiting) */
		}
		nap_ms(20);
	}
	st.no_answer++;
	SSR("<- %s: \"%s %s\": NO ANSWER in 5 s\n", pname[to], name, ev_name[ev]);
}

/* Linux order: the one that came up (`up`) is announced to the other first, then told about the other */
static void
announce_up(unsigned upn)
{
	unsigned other = upn ^ 1;
	unsigned bit_to_other = other == SWEET_SSR_ADSP ? 1 : 2;     /* bit 1: tell the DSP; bit 2: tell the modem */
	unsigned bit_to_up = upn == SWEET_SSR_ADSP ? 1 : 2;
	SSR("%s up with the %s up: notices in Linux order\n", pname[upn], pname[other]);
	if (sweet_ssr_notify & bit_to_other) {
		ssctl_event(other, upn, EV_AFTER_POWERUP);
	}
	if (sweet_ssr_notify & bit_to_up) {
		ssctl_event(upn, other, EV_AFTER_POWERUP);
	}
}

static void
announce_down(unsigned downn)
{
	unsigned other = downn ^ 1;
	unsigned bit_to_other = other == SWEET_SSR_ADSP ? 1 : 2;
	if (st.up[other] && (sweet_ssr_notify & bit_to_other)) {
		ssctl_event(other, downn, EV_BEFORE_SHUTDOWN);
		ssctl_event(other, downn, EV_AFTER_SHUTDOWN);
	}
}

static void
track(uint64_t t)
{
	uint32_t node, port, ver, mk = 0;
	bool srv_mdm = (sweet_bb & SBB_LINK) && sbb_server(SSCTL_SERVICE, SSCTL_INST_MODEM, &node, &port, &ver);
	if (srv_mdm != st.srv_mdm) {
		st.srv_mdm = srv_mdm;
		SSR("modem SSCTL server %s\n", srv_mdm ? "there" : "gone");
		if (srv_mdm) {
			SSR("   at %u:%#x, version %u\n", node, port, ver);
		} else if (sweet_mdm_crash & 1) {
			crash_read("modem SSCTL server gone", false);
		}
	}
	bool mdm = mdm_master_kernel(&mk) && (mk & MK_READY) && !(mk & MK_FATAL);
	bool adsp = redwood_qrtr_server(SSCTL_SERVICE, SSCTL_INST_ADSP, &node, &port, &ver);
	bool now[2] = { mdm, adsp };
	for (unsigned p = 0; p < 2; p++) {
		if (now[p] && !st.up[p]) {
			st.up[p] = true;
			st.ups[p]++;
			st.up_since[p] = t;
			st.sent_up[p] = false;
			SSR("%s up (#%u)%s\n", pname[p], st.ups[p], p == SWEET_SSR_ADSP ? "" : " (SMP2P ready)");
		} else if (!now[p] && st.up[p]) {
			st.up[p] = false;
			st.downs[p]++;
			SSR("%s down (#%u)\n", pname[p], st.downs[p]);
			if (sweet_ssr_notify & 4) {
				announce_down(p);
			}
		}
	}
	if (!(sweet_ssr_notify & 4) || !st.up[0] || !st.up[1]) {
		return;
	}
	/* both up: the one that came up last is announced (500 ms settle; the modem's SSCTL server too if bit 2 is on) */
	for (unsigned k = 0; k < 2; k++) {
		unsigned p = st.up_since[0] >= st.up_since[1] ? (k == 0 ? 0u : 1u) : (k == 0 ? 1u : 0u);
		if (st.sent_up[p] || t - st.up_since[p] < 500) {
			continue;
		}
		if ((sweet_ssr_notify & 2) && (sweet_bb & SBB_LINK) && !st.srv_mdm) {
			continue;               /* (wait for the modem's SSCTL server) */
		}
		st.sent_up[p] = true;
		st.sent_up[p ^ 1] = true;       /* the pair is done for this epoch */
		announce_up(p);
	}
}

static void
ssr_thread(__unused void *param, __unused wait_result_t wr)
{
	uint64_t next_reason = 0;
	SSR("thread up (mdm_crash %d, ssr_notify %#x)\n", sweet_mdm_crash, sweet_ssr_notify);
	for (;;) {
		uint64_t t = now_ms();
		struct rxp p;
		uint16_t res, err;
		while (rx_pop(&p)) {
			handle_rx(&p, -1, &res, &err);
		}
		if (sweet_mdm_crash & 1) {
			crash_watch(t, &next_reason);
		}
		if (sweet_ssr_notify) {
			track(t);
			if (st.now_pending) {
				st.now_pending = false;
				if (sweet_ssr_notify & 1) {
					ssctl_event(SWEET_SSR_ADSP, SWEET_SSR_MODEM, EV_AFTER_POWERUP);
				}
				if (sweet_ssr_notify & 2) {
					ssctl_event(SWEET_SSR_MODEM, SWEET_SSR_ADSP, EV_AFTER_POWERUP);
				}
			}
		}
		nap_ms(100);
	}
}

static void
ssr_start(void)
{
	bool go = false;
	lck_mtx_lock(&ssr_lock);
	if (!ssr_started) {
		ssr_started = go = true;
	}
	lck_mtx_unlock(&ssr_lock);
	if (go) {
		if (kernel_thread_start(ssr_thread, NULL, &ssr_thread_ref) == KERN_SUCCESS) {
			thread_deallocate(ssr_thread_ref);
		} else {
			SSR("no thread\n");
		}
	}
}

/* ---- sysctls ---- */

static int
sysctl_mdm_crash SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = sweet_mdm_crash, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v & 2) {
		crash_read("on demand", false);
	}
	v &= 1;
	if (!v && (sweet_ssr_notify & ~8)) {
		SSR("crash watch off: notices off too (interlock)\n");
		sweet_ssr_notify = 0;
	}
	sweet_mdm_crash = v;
	if (v) {
		ssr_start();
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mdm_crash, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_mdm_crash, "I",
    "redfin 1070: modem crash reason (SMEM 421), read-only: 1 watch SMP2P err-fatal/ready + item 421; write 2 (|1) = read now");

static int
sysctl_ssr_notify SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = sweet_ssr_notify, changed = 0;
	int e = sysctl_io_number(req, v, sizeof(v), &v, &changed);
	if (e || !changed) {
		return e;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (v & ~15) {
		return EINVAL;
	}
	const char *why = NULL;
	if ((v & 3) && !interlock(&why)) {
		SSR("kern.sweet_ssr_notify=%#x refused: %s\n", v, why);
		return EPERM;
	}
	sweet_ssr_notify = v;
	SSR("notify %#x: tell adsp %s, tell modem %s, %s%s\n", v, (v & 1) ? "on" : "off", (v & 2) ? "on" : "off",
	    (v & 4) ? "when both are up (Linux order, again after restarts)" : "once now", (v & 8) ? ", answer requests" : "");
	if (v & 3) {
		if (!(v & 4)) {
			st.now_pending = true;
		} else {
			st.sent_up[0] = st.sent_up[1] = false;
		}
		ssr_start();
		lck_mtx_lock(&ssr_lock);
		wakeup(&rx_count);
		lck_mtx_unlock(&ssr_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ssr_notify, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_ssr_notify, "I",
    "redfin 1070: SSCTL notices: 1 tell adsp 'modem up/down', 2 tell modem 'adsp up/down', 4 when both up (Linux order), "
    "8 answer requests; needs sweet_ufs_recover=3 and sweet_mdm_crash&1");

static int
sysctl_ssr_state SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char s[640];
	int n = snprintf(s, sizeof(s), "mdm_crash %d notify %#x ufs_recover %d | modem up %d (ups %u downs %u) mk 0x%x (seen %d, "
	    "changes %u) ssctl srv %d | adsp up %d (ups %u downs %u) | sent %u ok %u failed %u no answer %u refused %u | in: inds %u "
	    "reqs %u other %u dropped %u | crash reads %u, last: %s", sweet_mdm_crash, sweet_ssr_notify, sweet_ufs_recover, st.up[0],
	    st.ups[0], st.downs[0], st.mk, st.smp2p_seen, st.mk_changes, st.srv_mdm, st.up[1], st.ups[1], st.downs[1], st.sent, st.ok,
	    st.failed, st.no_answer, st.refused, st.inds, st.reqs, st.other, rx_dropped, st.crash_reads,
	    st.reason[0] ? st.reason : "-");
	return SYSCTL_OUT(req, s, (size_t)MIN(n, (int)sizeof(s) - 1) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ssr_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_ssr_state, "A",
    "redfin 1070: modem/adsp up state, SSCTL notice counters, last modem crash reason");

void
sweet_ssr_init(void)
{
	uint32_t c = 0, n = 0;
	PE_parse_boot_argn("sweetmdmcrash", &c, sizeof(c));
	PE_parse_boot_argn("sweetssr", &n, sizeof(n));
	sweet_mdm_crash = (int)(c & 1);
	if (n & ~15u) {
		n &= 15u;
	}
	if (n & 3) {
		const char *why = NULL;
		if (!interlock(&why)) {
			SSR("boot-arg sweetssr=%#x refused: %s\n", n, why);
			n = 0;
		}
	}
	if ((n & 3) && !(n & 4)) {
		SSR("boot-arg sweetssr=%#x without 4: nothing is sent at boot (write kern.sweet_ssr_notify to send once)\n", n);
	}
	sweet_ssr_notify = (int)n;
	if (sweet_mdm_crash || sweet_ssr_notify) {
		SSR("boot: mdm_crash %d, notify %#x\n", sweet_mdm_crash, sweet_ssr_notify);
		ssr_start();
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
