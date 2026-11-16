/* Host test for ws-gnss-j rfmetrics=1: REG_EVENTS mask (with and without the word), only REG_EVENTS/START-type requests
 * (no new SET), rfm.c decoders (band metrics 0x00CE, meas report 0x0086, SV_INFO rfLoss) and the 10 s rate limit.
 * test/run.sh builds and runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "qmi.h"
#include "../pdsemu.h"
#include "../loc.h"
#include "../rfm.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
static uint16_t sent_id[128], sent_txn[128];
static uint64_t last_mask;
static int nsent, nfail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	if (dst_port == PDSR_KPORT)
		return 0;
	sent_txn[nsent] = (uint16_t)(b[1] | b[2] << 8);
	sent_id[nsent] = (uint16_t)(b[3] | b[4] << 8);
	if (sent_id[nsent] == LOC_REG_EVENTS && len >= 7 + 11 && b[7] == 0x01)
		memcpy(&last_mask, b + 10, 8);
	nsent++;
	return 0;
}
static void respond(void)
{
	uint8_t b[32] = { 2, (uint8_t)sent_txn[nsent - 1], (uint8_t)(sent_txn[nsent - 1] >> 8), (uint8_t)sent_id[nsent - 1],
			  (uint8_t)(sent_id[nsent - 1] >> 8), 7, 0, 2, 4, 0, 0, 0, 0, 0 };
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 14);
}
static void tick(int ms) { t += (uint64_t)ms; loc_poll(); }
static void wordfile(const char *s)
{
	if (s) { FILE *f = fopen(LOC_ON, "w"); fputs(s, f); fclose(f); } else unlink(LOC_ON);
	t += 10001; loc_poll();
}
static uint32_t tlv(uint8_t *o, uint8_t ty, const void *v, uint16_t l)
{
	o[0] = ty; o[1] = (uint8_t)l; o[2] = (uint8_t)(l >> 8);
	memcpy(o + 3, v, l);
	return 3u + l;
}

static uint32_t band_msg(uint8_t *b)
{
	uint32_t o = 0;
	uint64_t tick0 = 123;
	int32_t pga = 3, pga5 = -2;
	uint8_t rf[8];
	uint16_t bi = 180, bq = 175;
	int32_t jp = 42;
	memcpy(rf, &bi, 2); memcpy(rf + 2, &bq, 2); memcpy(rf + 4, &jp, 4);
	o += tlv(b + o, 0x01, &tick0, 8);
	o += tlv(b + o, 0x02, &pga, 4);
	o += tlv(b + o, 0x10, rf, 8);
	o += tlv(b + o, 0x11, rf, 8);
	o += tlv(b + o, 0x17, &pga5, 4);
	o += tlv(b + o, 0x18, rf, 8);
	return o;
}

static uint32_t meas_msg(uint8_t *b, uint64_t sig)
{
	uint32_t o = 0, sys = 1;
	uint8_t seq = 1, mx = 2, sv[1 + 2 * 86];
	int32_t jam[2] = { 1234, -50 };
	memset(sv, 0x7f, sizeof(sv));	/* garbage everywhere else, incl. az/el */
	sv[0] = 2;
	for (int i = 0; i < 2; i++) {
		uint8_t *e = sv + 1 + 86 * i;
		uint16_t id = (uint16_t)(5 + i), cn = (uint16_t)(345 + 10 * i), rl = 0;
		memcpy(e, &id, 2); memcpy(e + 27, &cn, 2); memcpy(e + 29, &rl, 2);
	}
	o += tlv(b + o, 0x01, &seq, 1);
	o += tlv(b + o, 0x02, &mx, 1);
	o += tlv(b + o, 0x03, &sys, 4);
	o += tlv(b + o, 0x1b, sv, sizeof(sv));
	o += tlv(b + o, 0x1e, &sig, 8);
	o += tlv(b + o, 0x1f, jam, 8);
	return o;
}

static void ind(uint16_t id, const uint8_t *tl, uint32_t tn)
{
	uint8_t b[2048] = { 4, 1, 0, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)tn, (uint8_t)(tn >> 8) };
	memcpy(b + 7, tl, tn);
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 7 + tn);
}

int main(void)
{
	struct rfm r;
	char lines[10][RFM_LINE], ln[RFM_LINE];
	uint8_t b[1024];
	uint32_t n;

	/* decoders */
	rfm_reset(&r);
	n = band_msg(b);
	CHECK(rfm_band(&r, b, n, 1000, lines, 10) == 3);
	CHECK(strstr(lines[0], "GPS L1CA: jammerPwr 42 dB, bpAmp I 180 Q 175, PGA1 3 dB") != NULL);
	CHECK(strstr(lines[1], "GLO G1") != NULL);
	CHECK(strstr(lines[2], "GPS L5") && strstr(lines[2], "PGA5 -2 dB"));
	CHECK(rfm_band(&r, b, n, 5000, lines, 10) == 0);	/* rate-limited */
	CHECK(rfm_band(&r, b, n, 11000, lines, 10) == 3);
	n = meas_msg(b, 0x1);
	CHECK(rfm_meas(&r, b, n, 1000, ln, sizeof(ln)) == 1);
	printf("%s\n", ln);
	CHECK(strstr(ln, "GPS L1CA") && strstr(ln, "AGC 12.34 dB, jammer(BP) -0.50 dB") && strstr(ln, "2 SVs") &&
	      strstr(ln, " 5:34.5 6:35.5"));
	CHECK(rfm_meas(&r, b, n, 9000, ln, sizeof(ln)) == 0);
	n = meas_msg(b, 0x10);	/* another signal has its own window */
	CHECK(rfm_meas(&r, b, n, 9000, ln, sizeof(ln)) == 1 && strstr(ln, "GLO G1"));
	{
		uint8_t bad[16];
		uint32_t m = tlv(bad, 0x1b, "\x02\x00\x00", 3);
		CHECK(rfm_meas(&r, bad, m, 50000, ln, sizeof(ln)) == -1);
	}
	{
		uint8_t lt[1 + 3 * 4];
		float f[3] = { 2.5f, 3.0f, 4.5f };
		lt[0] = 3; memcpy(lt + 1, f, 12);
		n = tlv(b, 0x13, lt, sizeof(lt));
		CHECK(rfm_rfloss(&r, b, n, 1000, ln, sizeof(ln)) == 1);
		CHECK(strstr(ln, "rfLoss 3 SVs: min 2.5 avg 3.3 max 4.5 dB; 2.5 3.0 4.5") != NULL);
		CHECK(rfm_rfloss(&r, b, n, 2000, ln, sizeof(ln)) == 0);
	}

	/* loc.c: the mask */
	loc_server(0x10, 2, 0, 0x57);
	wordfile("session=60\n");
	tick(10);
	CHECK(sent_id[nsent - 1] == LOC_INFORM_CLIENT_REV);
	respond(); tick(10);
	CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	CHECK(last_mask == LOC_EVMASK_BRIDGE);
	respond(); tick(10); respond(); tick(10);
	wordfile("session=60 rfmetrics=1\n");
	for (int i = 0; i < 6; i++) { respond(); tick(10); }
	int got = 0;
	for (int i = 0; i < nsent; i++)
		CHECK(sent_id[i] == LOC_REG_EVENTS || sent_id[i] == LOC_START || sent_id[i] == LOC_STOP ||
		      sent_id[i] == LOC_INFORM_CLIENT_REV);	/* no SET of any kind */
	CHECK(last_mask == (LOC_EVMASK_BRIDGE | LOC_EVMASK_GNSS_MEAS | LOC_EVMASK_BAND_METRICS));
	got = last_mask == 0x40001000183ull;
	CHECK(got);
	n = band_msg(b); ind(LOC_IND_BAND_METRICS, b, n);
	n = meas_msg(b, 0x1); ind(LOC_IND_GNSS_MEAS, b, n);
	loc_shutdown("test");
	for (int i = 0; i < 4; i++) { respond(); tick(10); }
	unlink(LOC_ON); unlink(LOC_STATUS);
	printf("rfm_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
