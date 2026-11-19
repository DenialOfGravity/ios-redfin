/* Host test for sweetlocd's loc.c session machine with stubbed QRTR (test/run.sh builds and runs it).
 *
 * locationd-style PDS requests in (as the kernel relay hands them) -> LOC requests out (ids, order, one in flight), LOC
 * indications in -> PDS EVENT_REPORT relay records out, word removal -> STOP + REG_EVENTS 0, 3 timeouts -> DROPPED with
 * one last STOP, loc_shutdown (SIGTERM) -> STOP + REG_EVENTS 0 + relay BYE, then loc_quiet. All LOC traffic from port
 * LOC_PORT (0x4700, the kernel's aux range), relay traffic from PDSR_WPORT to node 1 port PDSR_KPORT. */
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
static uint16_t sent_id[128], sent_txn[128];
static int nsent;
static struct pdsr rel[64];
static uint8_t rel_data[64][256];
static int nrel;
static int nfail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	if (dst_port == PDSR_KPORT) {
		CHECK(src_port == PDSR_WPORT && dst_node == QRTR_NODE_APPS && type == QRTR_TYPE_DATA && len >= sizeof(struct pdsr));
		memcpy(&rel[nrel % 64], b, sizeof(struct pdsr));
		memcpy(rel_data[nrel % 64], b + sizeof(struct pdsr), len - sizeof(struct pdsr) < 256 ? len - sizeof(struct pdsr) : 256);
		nrel++;
		return 0;
	}
	CHECK(src_port == LOC_PORT && dst_node == 0 && dst_port == 0x57 && type == QRTR_TYPE_DATA);
	sent_txn[nsent] = (uint16_t)(b[1] | b[2] << 8);
	sent_id[nsent++] = (uint16_t)(b[3] | b[4] << 8);
	return 0;
}

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
static void kin(uint8_t op, unsigned flags)
{
	struct pdsr r = { 0x52534450u, op, 0, (uint16_t)flags, 0, 0, 0, 0, 0 };
	struct qrtr_hdr h = { 1, QRTR_TYPE_DATA, 1, PDSR_KPORT, 0, (uint32_t)sizeof(r), 1, PDSR_WPORT };
	loc_pdsr_handle(&h, (const uint8_t *)&r, (uint32_t)sizeof(r));
}
static void wordfile(const char *s)
{
	if (s) { FILE *f = fopen(LOC_ON, "w"); fputs(s, f); fclose(f); } else unlink(LOC_ON);
	t += 10001; loc_poll();
}
static void fix(void)
{
	uint8_t ind[128] = { 4, 1, 0, 0x24, 0, 0, 0 };
	uint32_t o = 7, st = 0; double lat = 1.25, lon = 2.5;
	ind[o++] = 1; ind[o++] = 4; ind[o++] = 0; memcpy(ind + o, &st, 4); o += 4;
	ind[o++] = 0x10; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lat, 8); o += 8;
	ind[o++] = 0x11; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lon, 8); o += 8;
	ind[5] = (uint8_t)(o - 7);
	struct qrtr_hdr h = { 0 };
	loc_handle(&h, ind, o);
}

int main(void)
{
	loc_fake_kernel_router = 1;
	wordfile(NULL);
	loc_server(0x10, 2, 0, 0x57);
	tick(100);
	CHECK(nsent == 0 && nrel == 0);		/* no loc.on: nothing, not even the relay */
	CHECK(!loc_pds_on() && !loc_on_present());
	wordfile("pds=1 pdsform=raw\n");
	CHECK(loc_pds_on() && nsent == 0);	/* no session wanted yet */
	CHECK(nrel == 1 && rel[0].op == 1);	/* HELLO to the relay */
	kin(0x84, 1);				/* kern.sweet_qmux_pds 1 */
	pds(0x0031, 1, 1);			/* locationd: auto tracking on */
	tick(10);
	CHECK(nsent == 1 && sent_id[0] == LOC_INFORM_CLIENT_REV);
	tick(10); CHECK(nsent == 1);		/* one in flight */
	respond(1); tick(10); CHECK(nsent == 2 && sent_id[1] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(nsent == 3 && sent_id[2] == LOC_START);
	respond(1); tick(10);
	/* a fix -> EVENT_REPORT relay record (0x12 status + 0x13 position) */
	int r0 = nrel;
	fix();
	CHECK(nrel == r0 + 1 && rel[r0].op == 3 && rel[r0].code == 0x0001 && rel[r0].len == 4 + 3 + PDS_FIX_TLV_LEN &&
	      rel_data[r0][4] == 0x13);
	/* locationd stops */
	pds(0x0031, 0, 1); tick(10);
	CHECK(nsent == 4 && sent_id[3] == LOC_STOP);
	respond(1); tick(10); CHECK(nsent == 5 && sent_id[4] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(nsent == 5);
	/* start again, then the word goes away mid-session: STOP + REG_EVENTS 0 + BYE */
	pds(0x0031, 1, 1); tick(10);
	CHECK(nsent == 6 && sent_id[5] == LOC_REG_EVENTS);	/* client revision only once per server */
	respond(1); tick(10); respond(1); tick(10); CHECK(sent_id[6] == LOC_START);
	r0 = nrel;
	wordfile("");
	CHECK(!loc_pds_on());
	CHECK(sent_id[nsent - 1] == LOC_STOP);
	CHECK(nrel == r0 + 1 && rel[r0].op == 4);
	respond(1); tick(10); CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	respond(1); tick(10);
	CHECK(loc_quiet());
	int before = nsent;
	/* probe=1: 5 GETs (client revision already sent for this server; ws-gnss-f adds GET_ENGINE_LOCK subType DV) */
	wordfile("probe=1\n");
	for (int i = 0; i < 5; i++) {
		CHECK(nsent == before + i + 1);
		respond(1);
		tick(3100);	/* no indication: given up after 3 s, next one goes */
	}
	CHECK(sent_id[before] == LOC_GET_SERVICE_REV && sent_id[before + 1] == LOC_GET_ENGINE_LOCK &&
	      sent_id[before + 2] == LOC_GET_ENGINE_LOCK && sent_id[before + 3] == LOC_GET_FIX_CRITERIA &&
	      sent_id[before + 4] == LOC_GET_XTRA_SOURCE);
	/* failures: pds session, START times out 3 times -> dropped, one last STOP (+ REG_EVENTS 0) */
	wordfile("pds=1 pdsform=raw\n");
	pds(0x0031, 1, 1);
	tick(10);
	for (int i = 0; i < 3 && nfail < 50; i++) {
		tick(5100);	/* timeout */
		tick(10);
	}
	tick(10);
	CHECK(loc_pds_on());	/* LOC dropped: the PDS emulation stays (ws-gnss-pdsfix) */
	CHECK(sent_id[nsent - 1] == LOC_STOP);
	respond(1); tick(10); CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(loc_quiet());
	/* session=N: standalone test session (the word change allows LOC again) */
	wordfile("session=60\n");
	tick(10); CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	respond(1); tick(10); CHECK(sent_id[nsent - 1] == LOC_START);
	respond(1); tick(10);
	/* SIGTERM mid-session: STOP + REG_EVENTS 0, nothing new starts, quiet after the answers */
	loc_shutdown("SIGTERM");
	tick(10);
	CHECK(sent_id[nsent - 1] == LOC_STOP && !loc_quiet());
	respond(1); tick(10); CHECK(sent_id[nsent - 1] == LOC_REG_EVENTS);
	respond(1); tick(10);
	int k = nsent;
	tick(20000); tick(10);
	CHECK(nsent == k && loc_quiet());	/* no new session after shutdown */
	unlink(LOC_ON); unlink(LOC_STATUS);
	printf("loc_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
