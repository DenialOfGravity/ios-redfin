/* Host test for ws-gnss-k: SV_INFO expanded list + per-system listed counts, SNTP packet parse, and the loc.c init order
 * with master=1 measconfig=1 time=1 xtra=FILE: REGISTER_MASTER_CLIENT, REG_EVENTS (MHAL, meas bit), constellation report
 * config, INJECT_UTC_TIME, XTRA parts (reassembled == file), validity GET, START; 0x00A7 -> 0x0035 fallback; a refused part
 * is logged, START still goes, no LOC failure; the same file is not injected twice; only allowed message IDs leave.
 * test/run.sh builds and runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "qmi.h"
#include "../pdsemu.h"
#include "../loc.h"
#include "../ntp.h"
#include "../rfm.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
#define NS 512
static uint16_t sent_id[NS], sent_txn[NS];
static uint8_t last_msg[2048];
static uint32_t last_len;
static int nsent, nfail, answered;
static uint8_t got[200000];
static uint32_t got_n;
static int part_fail_at, a7_unsupported;
static uint64_t reg_mask;
static char reg_strid[16];
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

static const uint8_t *tget(const uint8_t *p, uint32_t n, uint8_t ty, uint16_t *len)
{
	for (uint32_t o = 0; o + 3 <= n;) {
		uint16_t l = (uint16_t)(p[o + 1] | p[o + 2] << 8);
		if (p[o] == ty) { *len = l; return p + o + 3; }
		o += 3u + l;
	}
	return NULL;
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	if (dst_port == PDSR_KPORT)
		return 0;
	CHECK(nsent < NS);
	sent_txn[nsent] = (uint16_t)(b[1] | b[2] << 8);
	sent_id[nsent] = (uint16_t)(b[3] | b[4] << 8);
	memcpy(last_msg, b, len < sizeof(last_msg) ? len : sizeof(last_msg));
	last_len = len;
	nsent++;
	answered = 0;
	const uint8_t *tl = b + 7;
	uint32_t tn = len - 7;
	uint16_t l;
	const uint8_t *v;
	if (sent_id[nsent - 1] == LOC_REG_EVENTS) {
		if ((v = tget(tl, tn, 0x01, &l)) && l == 8) memcpy(&reg_mask, v, 8);
		reg_strid[0] = 0;
		if ((v = tget(tl, tn, 0x10, &l)) && l < sizeof(reg_strid)) { memcpy(reg_strid, v, l); reg_strid[l] = 0; }
	}
	if (sent_id[nsent - 1] == LOC_INJECT_XTRA_DATA || sent_id[nsent - 1] == LOC_INJECT_PRED_ORBITS) {
		uint16_t pn = 0;
		if ((v = tget(tl, tn, 0x03, &l)) && l == 2) memcpy(&pn, v, 2);
		if ((v = tget(tl, tn, 0x04, &l)) && l >= 2) {
			uint16_t dl = (uint16_t)(v[0] | v[1] << 8);
			CHECK(dl + 2u == l && dl <= 1024);
			if (pn == 1) got_n = 0;
			if (!(sent_id[nsent - 1] == LOC_INJECT_XTRA_DATA && a7_unsupported)) {
				memcpy(got + got_n, v + 2, dl);
				got_n += dl;
			}
		}
		CHECK(tget(tl, tn, 0x10, &l) && l == 4);	/* formatType XTRA */
	}
	return 0;
}

static void msg_in(uint8_t type, uint16_t txn, uint16_t id, const uint8_t *tl, uint32_t tn)
{
	uint8_t b[512] = { type, (uint8_t)txn, (uint8_t)(txn >> 8), (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)tn, (uint8_t)(tn >> 8) };
	memcpy(b + 7, tl, tn);
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 7 + tn);
}

/* the mock modem answers the last request: response (error for 0xA7 when a7_unsupported), and an indication for the
 * messages that have one (status, and partNum for XTRA parts; part part_fail_at gets status 1) */
static void modem(void)
{
	if (answered || !nsent)
		return;
	answered = 1;
	uint16_t id = sent_id[nsent - 1], txn = sent_txn[nsent - 1];
	int err = id == LOC_INJECT_XTRA_DATA && a7_unsupported;
	uint8_t r[7] = { 2, 4, 0, err ? 1 : 0, 0, err ? 0x29 : 0, 0 };
	msg_in(2, txn, id, r, 7);
	if (err)
		return;
	if (id == LOC_INJECT_XTRA_DATA || id == LOC_INJECT_PRED_ORBITS) {
		uint16_t l, pn = 0;
		const uint8_t *v = tget(last_msg + 7, last_len - 7, 0x03, &l);
		if (v) memcpy(&pn, v, 2);
		uint32_t st = pn == part_fail_at ? 1 : 0;
		uint8_t ind[3 + 4 + 3 + 2 + 3 + 8] = { 0x01, 4, 0 };
		memcpy(ind + 3, &st, 4);
		ind[7] = 0x10; ind[8] = 2; ind[9] = 0; memcpy(ind + 10, &pn, 2);
		uint64_t cm = 0x0f;
		ind[12] = 0x11; ind[13] = 8; ind[14] = 0; memcpy(ind + 15, &cm, 8);
		msg_in(4, txn, id, ind, sizeof(ind));
	} else if (id == LOC_REG_MASTER || id == LOC_SET_CONSTELL_REPORT || id == LOC_INJECT_UTC_TIME || id == LOC_GET_XTRA_VALIDITY) {
		uint8_t ind[3 + 4 + 3 + 10] = { 0x01, 4, 0, 0, 0, 0, 0, 0x10, 10, 0 };
		uint64_t st0 = 1794800000ull;
		uint16_t dh = 168;
		memcpy(ind + 10, &st0, 8); memcpy(ind + 18, &dh, 2);
		msg_in(4, txn, id, ind, id == LOC_GET_XTRA_VALIDITY ? sizeof(ind) : 7);
	}
}
static void run(int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; loc_poll(); modem(); } }
static void wordfile(const char *s)
{
	if (s) { FILE *f = fopen(LOC_ON, "w"); fputs(s, f); fclose(f); } else unlink(LOC_ON);
	t += 10001; loc_poll(); modem();
}
static int idx(uint16_t id, int from) { for (int i = from; i < nsent; i++) if (sent_id[i] == id) return i; return -1; }
static int count(uint16_t id, int from) { int c = 0; for (int i = from; i < nsent; i++) c += sent_id[i] == id; return c; }

int main(void)
{
	/* 1. SV_INFO: expanded list (29-byte entries) wins over svList; listed SVs counted per system with or without CN0 */
	{
		uint8_t xl[1 + 29 * 3], tl[300];
		struct { uint32_t vm, sys; float snr; uint32_t st; } e[3] = { { 0x8b, 1, 31.0f, 3 }, { 0x0b, 5, 0.0f, 2 }, { 0x0b, 2, 0.0f, 2 } };
		memset(xl, 0, sizeof(xl));
		xl[0] = 3;
		for (int i = 0; i < 3; i++) {
			uint8_t *p8 = xl + 1 + 29 * i;
			memcpy(p8, &e[i].vm, 4); memcpy(p8 + 4, &e[i].sys, 4); memcpy(p8 + 11, &e[i].st, 4); memcpy(p8 + 24, &e[i].snr, 4);
			p8[28] = 8;
		}
		uint32_t o = 0;
		uint8_t one[1 + 28] = { 1 };	/* a legacy list that disagrees: must be ignored */
		tl[o++] = 0x10; tl[o++] = sizeof(one); tl[o++] = 0; memcpy(tl + o, one, sizeof(one)); o += sizeof(one);
		tl[o++] = 0x11; tl[o++] = sizeof(xl); tl[o++] = 0; memcpy(tl + o, xl, sizeof(xl)); o += sizeof(xl);
		struct loc_sv_sum ss;
		char sb[320];
		CHECK(loc_sv_summary(tl, o, &ss) == 0 && ss.expanded == 1 && ss.listed == 3 && ss.searching == 2 && ss.tracked == 1);
		CHECK(ss.lsys[1] == 1 && ss.lsys[5] == 1 && ss.lsys[2] == 1 && ss.sys[1] == 1 && ss.sys[5] == 0);
		loc_sv_summary_str(&ss, sb, sizeof(sb));
		printf("gnssk_test: %s\n", sb);
		CHECK(strstr(sb, "; GPS 1; listed GPS 1 GAL 1 GLO 1 (searching 2, expanded list)") != NULL);
		uint32_t bad = 0;
		tl[bad++] = 0x11; tl[bad++] = 1 + 28; tl[bad++] = 0; tl[bad] = 1; bad += 29;
		CHECK(loc_sv_summary(tl, bad, &ss) == -2);
		uint32_t s8 = 8, vm = 0x01;	/* NavIC (8) has its own name now */
		memcpy(xl + 1, &vm, 4); memcpy(xl + 5, &s8, 4); xl[0] = 1;
		o = 0; tl[o++] = 0x11; tl[o++] = 30; tl[o++] = 0; memcpy(tl + o, xl, 30); o += 30;
		CHECK(loc_sv_summary(tl, o, &ss) == 0 && ss.lsys[8] == 1);
		loc_sv_summary_str(&ss, sb, sizeof(sb));
		CHECK(strstr(sb, "listed NavIC 1") != NULL);
	}
	/* 2. SNTP: offset +1500 ms, 40 ms round trip (server holds it 10 ms) */
	{
		uint8_t q[NTP_PKT], r[NTP_PKT];
		uint64_t t1 = 1794838952000ull;
		ntp_build(q, t1);
		CHECK(q[0] == 0x23);
		memset(r, 0, sizeof(r));
		r[0] = 0x24; r[1] = 2;
		memcpy(r + 24, q + 40, 8);
		uint8_t tmp[NTP_PKT];
		ntp_build(tmp, t1 + 1500 + 15); memcpy(r + 32, tmp + 40, 8);	/* T2 */
		ntp_build(tmp, t1 + 1500 + 25); memcpy(r + 40, tmp + 40, 8);	/* T3 */
		int64_t off = 0;
		uint32_t rtt = 0;
		CHECK(ntp_parse(r, sizeof(r), t1, t1 + 40, &off, &rtt) == 0);
		CHECK(off >= 1499 && off <= 1501 && rtt >= 29 && rtt <= 31);
		printf("gnssk_test: ntp offset %lld rtt %u\n", (long long)off, rtt);
		CHECK(ntp_parse(r, sizeof(r), t1 + 1, t1 + 40, &off, &rtt) == -2);
		r[1] = 0;
		CHECK(ntp_parse(r, sizeof(r), t1, t1 + 40, &off, &rtt) == -3);
		CHECK(ntp_parse(r, 20, t1, t1 + 40, &off, &rtt) == -1);
	}
	/* 3. the init order and the XTRA transfer */
	char xf[256];
	snprintf(xf, sizeof(xf), "%s.xtra", LOC_STATUS);
	uint8_t *file = malloc(5000);
	for (int i = 0; i < 5000; i++) file[i] = (uint8_t)(i * 7 + 3);
	{ FILE *f = fopen(xf, "wb"); fwrite(file, 1, 5000, f); fclose(f); }
	char words[512];
	snprintf(words, sizeof(words), "session=60 client=afw time=1 master=1 measconfig=1 xtra=%s\n", xf);
	loc_server(0x10, 2, 0, 0x57);
	wordfile(words);
	run(3000);
	int iv = idx(LOC_INFORM_CLIENT_REV, 0), im = idx(LOC_REG_MASTER, 0), ir = idx(LOC_REG_EVENTS, 0), ic = idx(LOC_SET_CONSTELL_REPORT, 0),
	    it = idx(LOC_INJECT_UTC_TIME, 0), ix = idx(LOC_INJECT_XTRA_DATA, 0), iy = idx(LOC_GET_XTRA_VALIDITY, 0), is = idx(LOC_START, 0);
	printf("gnssk_test: order rev %d master %d reg %d cfg %d time %d xtra %d validity %d start %d (%d sent)\n", iv, im, ir, ic, it, ix, iy, is, nsent);
	CHECK(iv == 0 && im == 1 && ir == 2 && ic == 3 && it == 4 && ix == 5 && iy > ix && is == iy + 1);
	CHECK(count(LOC_INJECT_XTRA_DATA, 0) == 5 && got_n == 5000 && !memcmp(got, file, 5000));
	CHECK(reg_mask == (LOC_EVMASK_BRIDGE | LOC_EVMASK_ASSIST | LOC_EVMASK_GNSS_MEAS) && !strcmp(reg_strid, "MHAL"));
	for (int i = 0; i < nsent; i++)
		CHECK(sent_id[i] == LOC_INFORM_CLIENT_REV || sent_id[i] == LOC_REG_MASTER || sent_id[i] == LOC_REG_EVENTS ||
		      sent_id[i] == LOC_SET_CONSTELL_REPORT || sent_id[i] == LOC_INJECT_UTC_TIME || sent_id[i] == LOC_INJECT_XTRA_DATA ||
		      sent_id[i] == LOC_GET_XTRA_VALIDITY || sent_id[i] == LOC_START);
	/* the modem asks for XTRA again: same file -> nothing sent */
	{
		int n0 = nsent;
		uint8_t ind[3 + 8] = { 0x01, 8, 0, 0x00, 0x08, 0x02, 0, 0x00, 0x28, 0, 0 };
		msg_in(4, 9, LOC_IND_XTRA_REQ, ind, sizeof(ind));
		run(1000);
		CHECK(count(LOC_INJECT_XTRA_DATA, n0) == 0);
		/* a new file (another size): injected again, at the front */
		FILE *f = fopen(xf, "wb"); fwrite(file, 1, 3000, f); fclose(f);
		t += 61000;
		msg_in(4, 10, LOC_IND_XTRA_REQ, ind, sizeof(ind));
		run(1000);
		CHECK(count(LOC_INJECT_XTRA_DATA, n0) == 3 && got_n == 3000);
	}
	wordfile(NULL);
	run(2000);
	/* 4. 0x00A7 not supported -> 0x0035 from part 1; 5. a refused part ends XTRA but START still goes, no failure */
	{ FILE *f = fopen(xf, "wb"); fwrite(file, 1, 2500, f); fclose(f); }
	a7_unsupported = 1;
	snprintf(words, sizeof(words), "session=60 xtra=%s\n", xf);
	int n1 = nsent;
	wordfile(words);
	run(3000);
	CHECK(count(LOC_INJECT_XTRA_DATA, n1) == 1 && count(LOC_INJECT_PRED_ORBITS, n1) == 3 && got_n == 2500);
	CHECK(idx(LOC_START, n1) > idx(LOC_INJECT_PRED_ORBITS, n1) && idx(LOC_REG_MASTER, n1) < 0 && idx(LOC_SET_CONSTELL_REPORT, n1) < 0);
	wordfile(NULL);
	run(2000);
	a7_unsupported = 0;
	part_fail_at = 2;
	{ FILE *f = fopen(xf, "wb"); fwrite(file, 1, 4000, f); fclose(f); }
	snprintf(words, sizeof(words), "session=60 xtra=%s\n", xf);
	int n2 = nsent;
	wordfile(words);
	run(3000);
	CHECK(count(LOC_INJECT_XTRA_DATA, n2) == 2 && idx(LOC_START, n2) > 0 && idx(LOC_GET_XTRA_VALIDITY, n2) < 0);
	{
		FILE *f = fopen(LOC_STATUS, "r");
		char b[2048] = "";
		if (f) { size_t r = fread(b, 1, sizeof(b) - 1, f); b[r] = 0; fclose(f); }
		CHECK(strstr(b, "state failed") != NULL && strstr(b, "fails 0") != NULL);
	}
	/* 6. no word: none of the ws-gnss-k messages */
	wordfile(NULL);
	run(2000);
	int n3 = nsent;
	wordfile("session=60\n");
	run(2000);
	CHECK(idx(LOC_REG_MASTER, n3) < 0 && idx(LOC_SET_CONSTELL_REPORT, n3) < 0 && idx(LOC_INJECT_XTRA_DATA, n3) < 0 &&
	      idx(LOC_INJECT_PRED_ORBITS, n3) < 0 && idx(LOC_INJECT_UTC_TIME, n3) < 0 && idx(LOC_START, n3) >= 0);
	loc_shutdown("test");
	run(2000);
	unlink(xf); unlink(LOC_ON); unlink(LOC_STATUS);
	free(file);
	printf("gnssk_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
