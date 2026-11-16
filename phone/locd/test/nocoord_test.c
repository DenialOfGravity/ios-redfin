/* Host test for ws-gnss-k "no coordinates anywhere": a POSITION_REPORT with distinctive latitude/longitude/altitude, and
 * an unknown indication carrying the same bytes, go through loc.c during a session; neither the log (stdout) nor the
 * status file may contain the values (decimal, any rounding) or their raw bytes in hex, nor the words lat/lon/alt.
 * The fix line must carry status, hunc, vunc, SVs used, TTFF and time source. test/run.sh also greps the sources. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "qmi.h"
#include "../pdsemu.h"
#include "../loc.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
static uint16_t sid, stxn;
static int nsent, answered, nfail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	if (dst_port == PDSR_KPORT)
		return 0;
	stxn = (uint16_t)(b[1] | b[2] << 8);
	sid = (uint16_t)(b[3] | b[4] << 8);
	nsent++;
	answered = 0;
	return 0;
}
static void msg_in(uint8_t type, uint16_t txn, uint16_t id, const uint8_t *tl, uint32_t tn)
{
	uint8_t b[512] = { type, (uint8_t)txn, (uint8_t)(txn >> 8), (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)tn, (uint8_t)(tn >> 8) };
	memcpy(b + 7, tl, tn);
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 7 + tn);
}
static void modem(void)
{
	if (answered || !nsent)
		return;
	answered = 1;
	uint8_t r[7] = { 2, 4, 0, 0, 0, 0, 0 };
	msg_in(2, stxn, sid, r, 7);
}
static void run(int ms) { for (int i = 0; i < ms / 10; i++) { t += 10; loc_poll(); modem(); } }
static uint32_t tlv(uint8_t *o, uint8_t ty, const void *v, uint16_t l)
{
	o[0] = ty; o[1] = (uint8_t)l; o[2] = (uint8_t)(l >> 8);
	memcpy(o + 3, v, l);
	return 3u + l;
}

int main(void)
{
	char logp[300];
	snprintf(logp, sizeof(logp), "%s.log", LOC_STATUS);
	fflush(stdout);
	if (!freopen(logp, "w", stdout))
		return 2;
	FILE *f = fopen(LOC_ON, "w");
	fputs("session=60 client=afw\n", f);
	fclose(f);
	loc_server(0x10, 2, 0, 0x57);
	t += 10001; loc_poll(); modem();
	run(2000);
	double lat = 12.3456789, lon = 98.7654321;
	float unc = 7.0f, vunc = 11.0f, alt = 4321.5f;
	uint32_t st = 0, tsrc = 6, tech = 1;
	uint8_t svu[1 + 2 * 5] = { 5 };
	uint8_t tl[400];
	uint32_t o = 0;
	o += tlv(tl + o, 0x01, &st, 4);
	o += tlv(tl + o, 0x10, &lat, 8);
	o += tlv(tl + o, 0x11, &lon, 8);
	o += tlv(tl + o, 0x12, &unc, 4);
	o += tlv(tl + o, 0x1a, &alt, 4);
	o += tlv(tl + o, 0x1b, &alt, 4);
	o += tlv(tl + o, 0x1c, &vunc, 4);
	o += tlv(tl + o, 0x23, &tech, 4);
	o += tlv(tl + o, 0x29, &tsrc, 4);
	o += tlv(tl + o, 0x2c, svu, sizeof(svu));
	t += 30000;
	for (int i = 0; i < 3; i++)
		msg_in(4, 9, LOC_IND_POSITION, tl, o);
	msg_in(4, 10, 0x0061, tl + 7, o - 7);		/* an indication we do not know, carrying the same bytes */
	msg_in(4, 11, LOC_IND_ENGINE_STATE, (const uint8_t *)"\x01\x04\x00\x01\x00\x00\x00", 7);
	run(3000);
	loc_shutdown("test");
	run(2000);
	fflush(stdout);
	char *buf = calloc(1, 1 << 20);
	size_t n = 0;
	const char *files[2] = { logp, LOC_STATUS };
	FILE *g;
	if ((g = fopen(logp, "r"))) { n = fread(buf, 1, (1 << 19) - 1, g); fclose(g); }
	CHECK(n > 0);
	unlink(LOC_ON);	/* the status file is still there from the last write (loc.on is read again only every 10 s) */
	if ((g = fopen(LOC_STATUS, "r"))) { n += fread(buf + n, 1, (1 << 19) - 1, g); fclose(g); }
	(void)files;
	buf[n] = 0;
	static const char *bad[] = { "12.3", "12.35", "98.7", "98.77", "4321", "4322", " lat", " lon", " alt", "latitude",
				     "longitude" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(strstr(buf, bad[i]) == NULL);
	char hx[64];
	const uint8_t *lb = (const uint8_t *)&lat, *ob = (const uint8_t *)&lon;
	snprintf(hx, sizeof(hx), "%02x %02x %02x %02x", lb[4], lb[5], lb[6], lb[7]);
	CHECK(strstr(buf, hx) == NULL);
	snprintf(hx, sizeof(hx), "%02x %02x %02x %02x", ob[4], ob[5], ob[6], ob[7]);
	CHECK(strstr(buf, hx) == NULL);
	CHECK(strstr(buf, "loc: fix #1 status 0 hunc 7 m vunc 11 m svs used 5 ttff ") != NULL);
	CHECK(strstr(buf, "timesrc 6") != NULL);
	CHECK(strstr(buf, "qmi: type 4 txn 10 msg 0x61 len") != NULL && strstr(buf, "(ids only)") != NULL);
	CHECK(strstr(buf, "loc: last fix status 0") != NULL);
	CHECK(loc_ind_hex_ok(LOC_IND_ENGINE_STATE) && !loc_ind_hex_ok(LOC_IND_POSITION) && !loc_ind_hex_ok(0x0061));
	unlink(logp); unlink(LOC_STATUS);
	free(buf);
	fprintf(stderr, "nocoord_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
