/* Host test for ws-gnss-pdsfix (test/run.sh builds and runs it): locationd's "wrong state track" check and the START
 * refusal loop of 2026-11-19 (36,000 STARTs).
 *
 * pds=1 (default AFW/HAL form) + kernel relay; locationd's init: RESET (kernel), SET_GPS_SERVICE_STATE 1,
 * SET_AUTO_TRACKING_STATE 1, SET_GPS_SERVICE_STATE 0 ("keeping service state OFF"), then GET_GPS_SERVICE_STATE every 4 s.
 * The modem refuses every START (error 0x2e, engine lock 0x3). Checks:
 *  - every GET answer says tracking 2 while auto tracking is on, and the last GPS_SERVICE_STATE_IND said tracking 2
 *    (locationd's fQueriedTrackingState == fReportedTrackingState == 2: no "wrong state track", no baseband reset);
 *  - START attempts back off (30 s, 60 s, 120 s, ...), at most 8 per locationd start, none closer than 30 s;
 *  - one read-only GET_ENGINE_LOCK, then one EVENT_REPORT session status 7 per refusal; never SET_ENGINE_LOCK;
 *  - the relay stays (no BYE), the log stays small. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "qmi.h"
#include "../pdsemu.h"
#include "../loc.h"

struct pdsr {
	uint32_t magic;
	uint8_t op, cid;
	uint16_t flags;
	uint32_t pipe, sess, tag;
	uint16_t code, len;
};

static uint64_t t;
uint64_t link_ms(void) { return t; }
static uint16_t last_id, last_txn;
static int nsent, nstart, nlockget, nlockset, pending;
static uint64_t start_at[64];
static int nstart_at;
static int nbye, nind_state, nind_ev, last_trk = -1, last_ev = -1, resp_trk = -1;
static int nfail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	(void)type; (void)src_port; (void)dst_node;
	if (dst_port == PDSR_KPORT) {
		struct pdsr r;
		memcpy(&r, b, sizeof(r));
		const uint8_t *d = b + sizeof(r);
		if (r.op == 4)
			nbye++;
		if (r.op == 3 && r.code == 0x0020 && r.len == 5) {
			nind_state++;
			last_trk = d[4];
		}
		if (r.op == 3 && r.code == 0x0001) {
			nind_ev++;
			CHECK(r.len == 4 && d[0] == 0x12);	/* status only, never a position */
			last_ev = d[3];
		}
		if (r.op == 2 && r.tag == 0x20 && r.len == 5)
			resp_trk = d[4];
		return 0;
	}
	last_txn = (uint16_t)(b[1] | b[2] << 8);
	last_id = (uint16_t)(b[3] | b[4] << 8);
	nsent++;
	pending = 1;
	if (last_id == LOC_START) {
		nstart++;
		if (nstart_at < 64)
			start_at[nstart_at++] = t;
	}
	if (last_id == LOC_GET_ENGINE_LOCK)
		nlockget++;
	if (last_id == 0x003a)
		nlockset++;
	return 0;
}

static void answer(void)	/* the modem: START error 0x2e, everything else ok; GET_ENGINE_LOCK ind lockClient 3 */
{
	if (!pending)
		return;
	pending = 0;
	int bad = last_id == LOC_START;
	uint8_t b[32] = { 2, (uint8_t)last_txn, (uint8_t)(last_txn >> 8), (uint8_t)last_id, (uint8_t)(last_id >> 8), 7, 0, 2, 4, 0,
			  bad ? 1 : 0, 0, bad ? 0x2e : 0, 0 };
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 14);
	if (last_id == LOC_GET_ENGINE_LOCK || last_id == LOC_SET_OPERATION_MODE) {
		uint8_t ind[40] = { 4, (uint8_t)last_txn, (uint8_t)(last_txn >> 8), (uint8_t)last_id, (uint8_t)(last_id >> 8), 0, 0 };
		uint32_t o = 7, st = 0;
		ind[o++] = 1; ind[o++] = 4; ind[o++] = 0; memcpy(ind + o, &st, 4); o += 4;
		if (last_id == LOC_GET_ENGINE_LOCK) {
			uint64_t lc = 3;
			ind[o++] = 0x12; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lc, 8); o += 8;
		}
		ind[5] = (uint8_t)(o - 7);
		loc_handle(&h, ind, o);
	}
}
static void tick(int ms) { t += (uint64_t)ms; loc_poll(); answer(); loc_poll(); }
static void kin(uint8_t op, unsigned cid, unsigned flags, uint32_t tag, const uint8_t *d, uint16_t len)
{
	uint8_t b[256];
	struct pdsr r = { 0x52534450u, op, (uint8_t)cid, (uint16_t)flags, 1, 9, tag, 0, len };
	memcpy(b, &r, sizeof(r));
	if (len)
		memcpy(b + sizeof(r), d, len);
	struct qrtr_hdr h = { 1, QRTR_TYPE_DATA, 1, PDSR_KPORT, 0, (uint32_t)sizeof(r) + len, 1, PDSR_WPORT };
	loc_pdsr_handle(&h, b, (uint32_t)sizeof(r) + len);
}
static void req(uint16_t msg, int v)
{
	uint8_t s[16] = { 0, 1, 0, (uint8_t)msg, (uint8_t)(msg >> 8), 0, 0 }, tn = 0;
	if (v >= 0) { s[7] = 1; s[8] = 1; s[9] = 0; s[10] = (uint8_t)v; tn = 4; }
	s[5] = tn;
	kin(0x81, 4, 0, msg, s, (uint16_t)(7 + tn));
}

int main(void)
{
	FILE *f;
	unlink(LOC_ON);
	loc_fake_kernel_router = 1;
	loc_server(0x10, 2, 0, 0x57);
	f = fopen(LOC_ON, "w"); fputs("pds=1\n", f); fclose(f);
	t += 10001; loc_poll();
	kin(0x84, 0, 1, 0, NULL, 0);
	/* locationd init */
	req(0x0000, -1);
	req(0x0021, 1);
	req(0x0031, 1);
	CHECK(last_trk == 2);			/* the indication right after the answer */
	req(0x0021, 0);
	CHECK(last_trk == 2);			/* service OFF: tracking stays */
	int ind0 = nind_state;
	/* 2 hours: GET every 4 s, the modem answers at once */
	for (int s = 0; s < 7200; s++) {
		tick(1000);
		if (s % 4 == 0) {
			resp_trk = -1;
			req(0x0020, -1);
			if (resp_trk != 2 || last_trk != 2) {
				CHECK(resp_trk == 2 && last_trk == 2);
				break;
			}
		}
	}
	CHECK(nind_state == ind0);		/* no indication flood: nothing changed */
	CHECK(nbye == 0);
	CHECK(nstart >= 2 && nstart <= 8);
	for (int i = 1; i < nstart_at; i++) {
		CHECK(start_at[i] - start_at[i - 1] >= 30000);
		if (i >= 2)
			CHECK(start_at[i] - start_at[i - 1] >= start_at[i - 1] - start_at[i - 2]);	/* backoff grows */
	}
	CHECK(nlockget == 1 && nlockset == 0);
	CHECK(nind_ev == nstart && last_ev == 7);	/* one honest status per refusal: engine locked */
	CHECK(nsent < 60);
	fprintf(stderr, "pdsfix_test: %d STARTs in 2 h (gaps", nstart);
	for (int i = 1; i < nstart_at; i++)
		fprintf(stderr, " %llu", (unsigned long long)((start_at[i] - start_at[i - 1]) / 1000));
	fprintf(stderr, " s), %d LOC requests, %d state inds, %d status reports\n", nsent, nind_state, nind_ev);
	/* locationd stops and starts again: tracking 1 then 2, a new START budget, still >= 30 s after the last */
	int s0 = nstart;
	req(0x0031, 0);
	CHECK(last_trk == 1);
	tick(100);
	req(0x0031, 1);
	CHECK(last_trk == 2);
	for (int s = 0; s < 25; s++)
		tick(1000);
	CHECK(nstart == s0 + 1);		/* at once (the last attempt was long ago), then 30 s backoff again */
	/* RESET: the kernel answers it; GET then says inactive (locationd cleared its states too) */
	req(0x0000, -1);
	resp_trk = -1;
	req(0x0020, -1);
	CHECK(resp_trk == 1);
	unlink(LOC_ON); unlink(LOC_STATUS);
	fprintf(stderr, "pdsfix_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
