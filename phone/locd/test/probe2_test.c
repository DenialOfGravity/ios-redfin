/* Host test for sweetlocd probe2=1 (ws-gnss-i): with every other word set too and a crash-left locd.lock-orig present,
 * only the whitelisted read-only IDs (INFORM_CLIENT_REVISION + the zero-TLV GETs) may reach the mock modem, each GET once
 * with no TLVs, an error answer does not stop the rest, and loc_probe2_done() is reached. test/run.sh builds and runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../link.h"
#include "qmi.h"
#include "../englock.h"
#include "../pdsemu.h"
#include "../loc.h"

static uint64_t t;
uint64_t link_ms(void) { return t; }
static int nlog, nfail;
static uint16_t log_id[256];
static uint16_t log_tn[256];
static uint8_t outq[32][128];
static uint32_t outn[32];
static int nout;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); nfail++; } } while (0)

static uint32_t tlv(uint8_t *o, uint8_t ty, const void *v, uint16_t l)
{
	o[0] = ty; o[1] = (uint8_t)l; o[2] = (uint8_t)(l >> 8);
	memcpy(o + 3, v, l);
	return 3u + l;
}

static void emit(uint8_t type, uint16_t txn, uint16_t id, const uint8_t *tl, uint32_t tn)
{
	uint8_t *b = outq[nout];
	b[0] = type; b[1] = (uint8_t)txn; b[2] = (uint8_t)(txn >> 8); b[3] = (uint8_t)id; b[4] = (uint8_t)(id >> 8);
	b[5] = (uint8_t)tn; b[6] = (uint8_t)(tn >> 8);
	memcpy(b + 7, tl, tn);
	outn[nout++] = 7 + tn;
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	struct qmi_msg m;
	uint8_t r[96];
	uint32_t n, st = 0;
	if (dst_port == PDSR_KPORT)
		return 0;
	if (qmi_parse(p, len, &m))
		return 0;
	log_tn[nlog % 256] = (uint16_t)m.len;
	log_id[nlog++ % 256] = m.id;
	int bad = m.id == 0x00b7;	/* one GET answered with a QMI error: the rest still go */
	uint8_t v[4] = { bad, 0, bad ? 0x1e : 0, 0 };
	n = tlv(r, 0x02, v, 4);
	emit(QMI_RESPONSE, m.txn, m.id, r, n);
	if (bad || m.id == LOC_INFORM_CLIENT_REV)
		return 0;
	n = tlv(r, 0x01, &st, 4);
	if (m.id == 0x00bf) {
		uint32_t cs[6] = { 0, 103, 103, 101, 103, 100 };
		for (int i = 0; i < 6; i++)
			n += tlv(r + n, (uint8_t)(0x10 + i), &cs[i], 4);
	} else if (m.id != 0x001e) {	/* GET_SUPPORTED_MSGS: response only */
		uint32_t x = 0x11223344;
		n += tlv(r + n, 0x10, &x, 4);
	}
	if (m.id != 0x001e)
		emit(QMI_INDICATION, 1, m.id, r, n);
	return 0;
}

static void pump(int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		t += 10;
		loc_poll();
		int k = nout;
		nout = 0;
		for (int j = 0; j < k; j++) {
			struct qrtr_hdr h = { 0 };
			loc_handle(&h, outq[j], outn[j]);
		}
	}
}

int main(void)
{
	static const uint16_t gets[] = { 0x00bf, 0x00a9, 0x001e, 0x00b7, 0x00e1, 0x003d, 0x003f, 0x004b, 0x00d5, 0x0052 };
	loc_fake_kernel_router = 1;
	/* a crash-left original lock: outside probe2 this would start a repair (SET_ENGINE_LOCK) */
	CHECK(lk_save(LOCK_ORIG, 0x3, 0) == 0);
	FILE *f = fopen(LOC_ON, "w");
	fputs("probe2=1 probe=1 pds=1 session=30 unlock=mi time=1 opmode=standalone lockclient=1\n", f);
	fclose(f);
	t = 10001;
	pump(10);
	loc_server(0x10, 2, 0, 0x57);
	for (int i = 0; i < 60 && !loc_probe2_done(); i++)
		pump(1000);
	CHECK(loc_probe2_done());
	CHECK(!loc_lock_busy());
	for (int i = 0; i < nlog; i++) {
		CHECK(loc_probe2_allowed(log_id[i]));
		if (!loc_probe2_allowed(log_id[i]))
			printf("  non-whitelisted 0x%04x sent\n", log_id[i]);
		CHECK(log_id[i] == LOC_INFORM_CLIENT_REV || log_tn[i] == 0);
	}
	CHECK(!loc_probe2_allowed(LOC_SET_ENGINE_LOCK) && !loc_probe2_allowed(LOC_GET_ENGINE_LOCK) &&
	      !loc_probe2_allowed(LOC_START) && !loc_probe2_allowed(LOC_STOP) && !loc_probe2_allowed(LOC_REG_EVENTS) &&
	      !loc_probe2_allowed(LOC_INJECT_UTC_TIME) && !loc_probe2_allowed(LOC_SET_OPERATION_MODE) &&
	      !loc_probe2_allowed(0x00b8));
	CHECK(nlog == 11 && log_id[0] == LOC_INFORM_CLIENT_REV);
	for (unsigned g = 0; g < sizeof(gets) / sizeof(gets[0]); g++) {
		int c = 0;
		for (int i = 0; i < nlog; i++)
			c += log_id[i] == gets[g];
		CHECK(c == 1);
	}
	loc_shutdown("probe2 done");
	pump(500);
	CHECK(nlog == 11);	/* nothing at shutdown either */
	CHECK(access(LOCK_ORIG, F_OK) == 0);	/* left for a normal run's repair */
	unlink(LOC_ON); unlink(LOC_STATUS); unlink(LOCK_ORIG);
	printf("probe2_test: %s (%d messages sent, %d checks failed)\n", nfail ? "FAILED" : "all ok", nlog, nfail);
	return nfail != 0;
}
