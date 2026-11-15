/* Host test for loc.c's side of the kernel router's PDS relay (xnu 1122; test/run.sh builds and runs it).
 *
 * pds=1 + kernel router -> HELLO to 1:PDSR_KPORT; kernel ack (flags 1) -> REQ answered (RESP flags 1, same tag) or
 * "not ours" (flags 0); a LOC fix -> IND record (code = PDS EVENT_REPORT) instead of abpbridge; GONE -> LOC STOP;
 * word gone -> BYE. Without the kernel router nothing goes to PDSR_KPORT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "../qmi.h"
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
static uint16_t sent_id[64], sent_txn[64];
static int nsent;
static struct pdsr rel[64];
static int nrel;
int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	const uint8_t *b = p;
	if (dst_port == PDSR_KPORT) {
		if (src_port != PDSR_WPORT || dst_node != QRTR_NODE_APPS || type != QRTR_TYPE_DATA || len < sizeof(struct pdsr))
			printf("FAIL bad relay packet\n"), exit(1);
		memcpy(&rel[nrel++ % 64], b, sizeof(struct pdsr));
		return 0;
	}
	sent_txn[nsent] = (uint16_t)(b[1] | b[2] << 8);
	sent_id[nsent++] = (uint16_t)(b[3] | b[4] << 8);
	return 0;
}
static int nabp;
int abpbridge_send_ind(uint32_t pipe, uint32_t sess, unsigned svc, unsigned cid, unsigned msg, const uint8_t *tl, uint32_t tn)
{
	nabp++;
	return -1;
}
static int nfail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

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
static void req(uint16_t msg, int v, uint32_t tag)
{
	uint8_t s[16] = { 0, 1, 0, (uint8_t)msg, (uint8_t)(msg >> 8), 0, 0 }, tn = 0;
	if (v >= 0) { s[7] = 1; s[8] = 1; s[9] = 0; s[10] = (uint8_t)v; tn = 4; }
	s[5] = tn;
	kin(0x81, 4, 0, tag, s, (uint16_t)(7 + tn));
}

int main(void)
{
	unlink(LOC_ON);
	loc_server(0x10, 2, 0, 0x57);
	/* no kernel router: pds=1 sends nothing to the relay */
	loc_fake_kernel_router = 0;
	wordfile("pds=1\n");
	tick(10);
	CHECK(nrel == 0);
	/* kernel router: HELLO */
	loc_fake_kernel_router = 1;
	tick(10);
	CHECK(nrel == 1 && rel[0].op == 1);
	tick(10); CHECK(nrel == 1);		/* every 10 s, not every poll */
	kin(0x84, 0, 1, 0, NULL, 0);		/* kern.sweet_qmux_pds 1 */
	/* a request pdsemu answers, and one it does not */
	req(0x0031, 1, 7);
	CHECK(nrel == 2 && rel[1].op == 2 && rel[1].tag == 7 && rel[1].flags == 1 && rel[1].cid == 4);
	req(0x5557, -1, 8);
	CHECK(nrel == 3 && rel[2].op == 2 && rel[2].tag == 8 && (rel[2].flags & 1) == 0);
	/* the LOC session starts (client revision, REG_EVENTS, START) */
	tick(10); respond(); tick(10); respond(); tick(10); CHECK(sent_id[nsent - 1] == LOC_START);
	respond(); tick(10);
	/* a fix -> IND record, not abpbridge */
	uint8_t ind[128] = { 4, 1, 0, 0x24, 0, 0, 0 };
	uint32_t o = 7, st = 0; double lat = 1.25, lon = 2.5;
	ind[o++] = 1; ind[o++] = 4; ind[o++] = 0; memcpy(ind + o, &st, 4); o += 4;
	ind[o++] = 0x10; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lat, 8); o += 8;
	ind[o++] = 0x11; ind[o++] = 8; ind[o++] = 0; memcpy(ind + o, &lon, 8); o += 8;
	ind[5] = (uint8_t)(o - 7);
	struct qrtr_hdr h = { 0 };
	int r0 = nrel;
	loc_handle(&h, ind, o);
	CHECK(nrel == r0 + 1 && rel[r0].op == 3 && rel[r0].code == 0x0001 && rel[r0].cid == 4 && rel[r0].pipe == 1 &&
	      rel[r0].sess == 9 && rel[r0].len == 4 + 3 + PDS_FIX_TLV_LEN);
	CHECK(nabp == 0);
	/* the session closes in the kernel: GONE -> LOC STOP */
	kin(0x82, 0, 0, 0, (const uint8_t *)"session closed", 14);
	tick(10);
	CHECK(sent_id[nsent - 1] == LOC_STOP);
	respond(); tick(10); respond(); tick(10);
	/* word gone: BYE */
	r0 = nrel;
	wordfile("");
	CHECK(nrel == r0 + 1 && rel[r0].op == 4);
	tick(20000); CHECK(nrel == r0 + 1);
	unlink(LOC_ON); unlink(LOC_STATUS);
	printf("pdsr_test: %s (%d failed)\n", nfail ? "FAILED" : "all ok", nfail);
	return nfail != 0;
}
