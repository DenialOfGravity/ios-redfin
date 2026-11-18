/* Host test of qseeprobe's SMC whitelist: cc -o /tmp/test_wl test_wl.c qsee_wl.c && /tmp/test_wl */
#include <stdio.h>
#include "qsee_wl.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	/* the IDs are the literal values Linux 4.19 issues (scm.c ORs in SMC64) */
	CHECK(QSEE_SMC_FEAT_VERSION == 0x42000603ull);
	CHECK(QSEE_SMC_APP_LOOKUP == 0x72000103ull);
	CHECK(QSEE_SMC_SHMB_CREATE == 0x42000c1eull);
	CHECK(QSEE_SMC_SHMB_DELETE == 0x42000c1dull);
	CHECK(QSEE_ARGS_FEAT_VERSION == 0x01 && QSEE_ARGS_APP_LOOKUP == 0x22);
	CHECK(QSEE_ARGS_SHMB_CREATE == 0x04 && QSEE_ARGS_SHMB_DELETE == 0x01);
	CHECK(QSEE_SMC_APP_START == 0x72000101ull && QSEE_SMC_APP_SHUTDOWN == 0x72000102ull);
	CHECK(QSEE_SMC_LSTNR_RESP == 0x72000203ull && QSEE_SMC_CONT_BLOCKED == 0x72000204ull);
	CHECK(QSEE_SMC_CONT_BLOCKED_SI == 0x72000207ull);
	CHECK(QSEE_SMC_SEND_DATA == 0x70000001ull && QSEE_SMC_SEND_DATA_WL == 0x70000006ull);
	CHECK(QSEE_ARGS_APP_START == 0x03 && QSEE_ARGS_APP_SHUTDOWN == 0x01 && QSEE_ARGS_LSTNR_RESP == 0x02);
	CHECK(QSEE_ARGS_SEND_DATA == 0x885 && QSEE_ARGS_SEND_DATA_WL == 0x8887);
	CHECK(qsee_whitelist_n == 15);
	CHECK(QSEE_SMC_LSTNR_REG == 0x72000201ull && QSEE_SMC_LSTNR_DEREG == 0x72000202ull);
	CHECK(QSEE_SMC_LSTNR_RESP_WL == 0x72000205ull && QSEE_SMC_LSTNR_REG_SI == 0x72000206ull);
	CHECK(QSEE_ARGS_LSTNR_REG == 0x83 && QSEE_ARGS_LSTNR_DEREG == 0x01 && QSEE_ARGS_LSTNR_RESP_WL == 0x204);
	CHECK(!qsee_allowed(QSEE_SMC_LSTNR_REG_SI, 0x03));
	for (unsigned i = 0; i < qsee_whitelist_n; i++)
		CHECK(qsee_allowed(qsee_whitelist[i].x0, qsee_whitelist[i].arginfo) == &qsee_whitelist[i]);
	/* right ID, wrong arginfo: refused */
	CHECK(!qsee_allowed(QSEE_SMC_APP_LOOKUP, 0x02));
	CHECK(!qsee_allowed(QSEE_SMC_FEAT_VERSION, 0x00));
	CHECK(!qsee_allowed(QSEE_SMC_SEND_DATA, QSEE_ARGS_SEND_DATA_WL));
	CHECK(!qsee_allowed(QSEE_SMC_SEND_DATA_WL, QSEE_ARGS_SEND_DATA));
	CHECK(!qsee_allowed(QSEE_SMC_APP_START, 0x22));
	/* everything that changes TZ state or talks to a TA: refused */
	static const uint64_t bad[] = {	/* every other qseecomi.h ID (| SMC64), as computed from its macros */
		0x72000104, 0x72000105,				/* GET_STATE, REGION_NOTIF */
		0x72000106, 0x72000107, 0x72000108,		/* REGISTER_LOG_BUFFER, LOAD/UNLOAD_SERVICES_IMAGE */
		0x72000301, 0x72000302,				/* external image load/unload */
		0x72000401, 0x72000402, 0x72000403,		/* RPMB provision/erase/check */
		0x72000501, 0x72000502, 0x72000503, 0x72000504, 0x72000505,	/* keystore */
		0x70000002, 0x70000003, 0x70000004, 0x70000005,	/* GP sessions */
		0x70000007, 0x70000009,				/* GP with whitelist */
		0x42001001, 0x42001201,				/* ES save partition hash, MDTP */
		0x42000601,	/* TZ_INFO_IS_SVC_AVAILABLE (harmless, but not on the list) */
		0x42000c1c,	/* TZ_SHM_BRIDGE_ENABLE: one-way, not issued */
		0x42000c16,	/* MP_ASSIGN (hyp_assign) */
		0x42000201, 0x42000202, 0x42000205, 0x42000206,	/* PAS */
		0x02000603, 0x32000103, 0x02000c1e, 0x02000c1d,	/* SMC32 forms */
		0x32000101, 0x32000102, 0x30000001, 0x30000006, 0xf2000101, 0xf0000001,
		0xc2000603, 0xf2000103, 0xc2000c1e, 0xc2000c1d,	/* atomic (fast) forms */
		0x84000009,	/* PSCI SYSTEM_RESET */
		0, 1, ~0ull,
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		for (uint64_t ai = 0; ai < 0x10000; ai++)	/* send-data arginfos go past 0xff */
			if (qsee_allowed(bad[i], ai)) {
				printf("FAIL: %#llx (arginfo %#llx) allowed\n", (unsigned long long)bad[i],
				       (unsigned long long)ai);
				fails++;
			}
	/* exhaustive over the low 16 bits of each owner/service page: only the 11 IDs pass */
	static const uint64_t pages[] = { 0x42000000, 0x72000000, 0x70000000, 0x32000000, 0x02000000 };
	unsigned n = 0;
	for (unsigned p = 0; p < 5; p++)
		for (uint64_t f = 0; f < 0x10000; f++) {	/* count IDs, not (ID, arginfo) pairs */
			int ok = 0;
			for (uint64_t ai = 0; ai < 0x100; ai++)
				ok |= !!qsee_allowed(pages[p] | f, ai);
			ok |= !!qsee_allowed(pages[p] | f, QSEE_ARGS_SEND_DATA) | !!qsee_allowed(pages[p] | f, QSEE_ARGS_SEND_DATA_WL) |
			      !!qsee_allowed(pages[p] | f, QSEE_ARGS_LSTNR_RESP_WL);
			n += ok;
		}
	CHECK(n == 15);
	printf("%s: qsee whitelist test (%u entries)\n", fails ? "FAILED" : "ok", qsee_whitelist_n);
	return fails ? 1 : 0;
}
