/* Host test for loc.c's session machine with stubbed QRTR/abpbridge (test/run.sh builds and runs it).
 *
 * locationd-style PDS requests in -> LOC requests out (ids, order, one in flight), LOC indications in -> PDS
 * EVENT_REPORT out, word removal -> STOP + REG_EVENTS 0, 3 timeouts -> DROPPED with one last STOP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "../qmi.h"
#include "../pdsemu.h"
#include "../loc.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
static uint16_t sent_id[64], sent_txn[64];
static int nsent;
int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	sent_txn[nsent] = (uint16_t)(b[1] | b[2] << 8);
	sent_id[nsent++] = (uint16_t)(b[3] | b[4] << 8);
	return 0;
}
static int nind, ind_len;
static uint8_t ind_buf[512];
int abpbridge_send_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn)
{
	nind++; ind_len = (int)tn; memcpy(ind_buf, tl, tn);
	return 0;
}
static int nfail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

static void respond(int ok)	/* answer the last request */
{
	uint8_t b[32] = { 2, (uint8_t)sent_txn[nsent - 1], (uint8_t)(sent_txn[nsent - 1] >> 8), (uint8_t)sent_id[nsent - 1],
			  (uint8_t)(sent_id[nsent - 1] >> 8), 7, 0, 2, 4, 0, ok ? 0 : 1, 0, ok ? 0 : 3, 0 };
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, b, 14);
}
static void tick(int ms) { t += (uint64_t)ms; loc_poll(); }
static void pds(uint16_t msg, uint8_t v, int with)
{
	uint8_t s[16] = { 0, 1, 0, (uint8_t)msg, (uint8_t)(msg >> 8), 0, 0 }, tn = 0;
	if (with) { s[7] = 1; s[8] = 1; s[9] = 0; s[10] = v; tn = 4; }
	s[5] = tn;
	int f, c; const uint8_t *tl; uint16_t n; const char *w;
	CHECK(loc_pds_request(1, 9, 4, s, 7u + tn, &f, &c, &tl, &n, &w) == 1);
}
static void wordfile(const char *s)
{
	if (s) { FILE *f = fopen(LOC_ON, "w"); fputs(s, f); fclose(f); } else unlink(LOC_ON);
	t += 10001; loc_poll();
}

int main(void)
{
	wordfile(NULL);
	loc_server(0x10, 2, 0, 0x57);
	tick(100);
	CHECK(nsent == 0);			/* no loc.on: nothing */
	CHECK(!loc_pds_on());
	wordfile("pds=1\n");
	CHECK(loc_pds_on() && nsent == 0);	/* no session wanted yet */
	pds(0x0031, 1, 1);			/* locationd: auto tracking on */
	tick(10);
	CHECK(nsent == 1 && sent_id[0] == LOC_INFORM_CLIENT_REV);
	tick(10); CHECK(nsent == 1);		/* one in flight */
	respond(1); tick(10); CHECK(nsent == 2 && sent_id[1] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(nsent == 3 && sent_id[2] == LOC_START);
	respond(1); tick(10);
	/* a fix */
	uint8_t ind[128] = { 4, 1, 0, 0x24, 0, 0, 0 };
	uint32_t o = 7, st = 0; double lat = 1.25, lon = 2.5;
	ind[o++] = 1; ind[o++] = 4; ind[o++] = 0; memcpy(ind + o, &st, 4); o += 4;
	ind[o++] = 0x10; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lat, 8); o += 8;
	ind[o++] = 0x11; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lon, 8); o += 8;
	ind[5] = (uint8_t)(o - 7);
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, ind, o);
	CHECK(nind == 1 && ind_len == 4 + 3 + PDS_FIX_TLV_LEN && ind_buf[4] == 0x13);
	/* locationd stops */
	pds(0x0031, 0, 1); tick(10);
	CHECK(nsent == 4 && sent_id[3] == LOC_STOP);
	respond(1); tick(10); CHECK(nsent == 5 && sent_id[4] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(nsent == 5);
	/* start again, then the word goes away mid-session */
	pds(0x0031, 1, 1); tick(10);
	CHECK(nsent == 6 && sent_id[5] == LOC_REG_EVENTS);	/* client revision only once per server */
	respond(1); tick(10); respond(1); tick(10); CHECK(sent_id[6] == LOC_START);
	wordfile("");
	CHECK(!loc_pds_on());
	CHECK(sent_id[nsent - 1] == LOC_STOP);
	respond(1); tick(10); CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	respond(1); tick(10);
	int before = nsent;
	/* probe=1: 1 + 4 GETs (client revision already sent for this server) */
	wordfile("probe=1\n");
	for (int i = 0; i < 4; i++) {
		CHECK(nsent == before + i + 1);
		respond(1);
		tick(3100);	/* no indication: given up after 3 s, next one goes */
	}
	CHECK(sent_id[before] == LOC_GET_SERVICE_REV && sent_id[before + 1] == LOC_GET_ENGINE_LOCK &&
	      sent_id[before + 2] == LOC_GET_FIX_CRITERIA && sent_id[before + 3] == LOC_GET_XTRA_SOURCE);
	/* failures: pds session, START times out 3 times -> dropped, one last STOP */
	wordfile("pds=1\n");
	pds(0x0031, 1, 1);
	tick(10);
	int k = nsent;
	for (int i = 0; i < 3 && nfail < 50; i++) {
		tick(5100);	/* timeout */
		tick(10);
	}
	tick(10);
	CHECK(!loc_pds_on());	/* dropped */
	CHECK(sent_id[nsent - 1] == LOC_STOP || sent_id[nsent - 2] == LOC_STOP);
	printf("sent:"); for (int i = k - 1; i < nsent; i++) printf(" %04x", sent_id[i]); printf("\n");
	unlink(LOC_ON); unlink(LOC_STATUS);
	printf("loc_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
