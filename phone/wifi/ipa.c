/*
 * ipa.c: AP-side IPA v4.7 setup for the cellular data path (redfin / lito), polled, from userland over /dev/pmem.
 * Step plan: project doc claude/redfin-cell-data-handoff-2026-11-05.md (steps 1-3). Already proven before this:
 *   wland bcm IP0 0 0x3fff                                  IPA core clock (RPMh BCM IP0)
 *   wland pas-load 15 /var/local/fw/ipa ipa_fws 0x8bc00000 0x10000   GSI/IPA firmware via TrustZone
 *
 *   wland ipahw regs                  read-only dump of the registers below (and the version gate)
 *   wland ipahw smmu                  IPA DMA streams 0x520 (AP) and 0x522 (uC) on translation-off context banks
 *                                   (radio.c smmu_stream_identity, the same setup the WLAN stream uses)
 *   wland ipahw hwinit [--dry-run] [--route]   the registers downstream ipa3_post_init / ipa3_init_hw write on v4.7
 *   wland ipahw meminit [--dry-run]   local memory: zero above the uC area, canaries of downstream ipa_4_7_mem_part,
 *                                   LOCAL_PKT_PROC_CNTXT base
 *
 * Gate: nothing is written (or read beyond the gate) unless IPA_COMP_HW_VERSION reads 0x40070000, which also shows
 * the IP0 clock vote is in place (an unclocked IPA would not answer). No GSI channels, SMP2P, QMI or SMEM here.
 * Register offsets are from the IPA register base 0x1e40000 (ipa-base 0x1e00000 + 0x40000, downstream
 * ipahal_reg.c); values from downstream redbull (the code that ran on this phone under Android): ipa_utils.c
 * (QSB {reads 13, writes 12}, resource groups for IPA_4_7), ipa.c ipa3_init_hw / ipa3_post_init, ipa_utils.c:3163
 * ipa_4_7_mem_part. Every write is logged with the value it replaced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmem.h"
#include "radio.h"
#include "ipa.h"

#define IPA_REG		0x01e40000ull	/* ipa-reg */
#define IPA_SHARED	0x01e50000ull	/* ipa-shared (local memory window, 0x4b20 bytes on v4.7) */
#define GSI_REG		0x01e04000ull

#define COMP_HW_VERSION		0x030
#define VERSION			0x034
#define ENABLED_PIPES		0x038
#define COMP_CFG		0x03c
#define ROUTE			0x048
#define SHARED_MEM_SIZE		0x054
#define QSB_MAX_WRITES		0x074
#define QSB_MAX_READS		0x078
#define LOCAL_PKT_PROC_CNTXT	0x1e8
#define IDLE_INDICATION_CFG	0x240
#define QTIME_TIMESTAMP_CFG	0x24c
#define TIMERS_XO_CLK_DIV_CFG	0x250
#define TIMERS_PULSE_GRAN_CFG	0x254
#define SRC_RSRC_GRP_01(n)	(0x400 + 0x20 * (n))
#define DST_RSRC_GRP_01(n)	(0x500 + 0x20 * (n))

#define IPA_SID_AP	0x520
#define IPA_SID_UC	0x522

static int dry;

static uint32_t rdr(uint32_t off)
{
	return rd32(IPA_REG + off);
}

static void wrr(const char *name, uint32_t off, uint32_t val)
{
	uint32_t was = rdr(off);
	printf("ipa: %s %s(%#x) = %#010x (was %#010x)\n", dry ? "would write" : "wr", name, off, val, was);
	if (!dry)
		wr32(IPA_REG + off, val);
}

static int gate(void)
{
	uint32_t v = rdr(COMP_HW_VERSION);
	if (v != 0x40070000) {
		printf("ipa: COMP_HW_VERSION %#010x, not 0x40070000 (IP0 not voted?): nothing done\n", v);
		return -1;
	}
	return 0;
}

/* SHARED_MEM_SIZE: size [15:0] and base address [31:16], both in 8-byte units (mainline ipa_mem_config) */
static void shared_mem(uint32_t *base, uint32_t *size)
{
	uint32_t v = rdr(SHARED_MEM_SIZE);
	*size = 8 * (v & 0xffff);
	*base = 8 * (v >> 16);
}

static int regs(void)
{
	if (gate())
		return 1;
	static const struct { const char *n; uint32_t o; } r[] = {
		{ "COMP_HW_VERSION", COMP_HW_VERSION }, { "VERSION", VERSION }, { "ENABLED_PIPES", ENABLED_PIPES },
		{ "COMP_CFG", COMP_CFG }, { "ROUTE", ROUTE }, { "SHARED_MEM_SIZE", SHARED_MEM_SIZE },
		{ "QSB_MAX_WRITES", QSB_MAX_WRITES }, { "QSB_MAX_READS", QSB_MAX_READS },
		{ "LOCAL_PKT_PROC_CNTXT", LOCAL_PKT_PROC_CNTXT }, { "IDLE_INDICATION_CFG", IDLE_INDICATION_CFG },
		{ "QTIME_TIMESTAMP_CFG", QTIME_TIMESTAMP_CFG }, { "TIMERS_XO_CLK_DIV_CFG", TIMERS_XO_CLK_DIV_CFG },
		{ "TIMERS_PULSE_GRAN_CFG", TIMERS_PULSE_GRAN_CFG },
	};
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		printf("ipa: %-22s %#05x = %#010x\n", r[i].n, r[i].o, rdr(r[i].o));
	for (unsigned n = 0; n < 5; n++)
		printf("ipa: SRC_RSRC_GRP_01(%u)     %#05x = %#010x\n", n, SRC_RSRC_GRP_01(n), rdr(SRC_RSRC_GRP_01(n)));
	for (unsigned n = 0; n < 2; n++)
		printf("ipa: DST_RSRC_GRP_01(%u)     %#05x = %#010x\n", n, DST_RSRC_GRP_01(n), rdr(DST_RSRC_GRP_01(n)));
	uint32_t base, size;
	shared_mem(&base, &size);
	printf("ipa: local memory: base %#x size %#x (window %#llx)\n", base, size, (unsigned long long)IPA_SHARED);
	printf("gsi: GSI_CFG %#010x MCS_CFG %#010x\n", rd32(GSI_REG), rd32(GSI_REG + 0xb000));
	return 0;
}

static int smmu(void)
{
	if (gate())
		return 1;
	int a = smmu_stream_identity(IPA_SID_AP, 0, "IPA-AP");
	int b = smmu_stream_identity(IPA_SID_UC, 0, "IPA-uC");
	return a || b ? 1 : 0;
}

static int hwinit(int route)
{
	if (gate())
		return 1;
	wrr("QSB_MAX_WRITES", QSB_MAX_WRITES, 0x0000000c);	/* ipa_utils.c ipa3_qmb_outstanding IPA_4_7 {13, 12} */
	wrr("QSB_MAX_READS", QSB_MAX_READS, 0x0000000d);
	wrr("QTIME_TIMESTAMP_CFG", QTIME_TIMESTAMP_CFG, 0x00180e8e);	/* ipa3_init_hw: qtime config */
	wrr("TIMERS_PULSE_GRAN_CFG", TIMERS_PULSE_GRAN_CFG, 0x00000123);
	uint32_t xo = rdr(TIMERS_XO_CLK_DIV_CFG);	/* keep downstream's divider, toggle enable (bit 31) off then on */
	wrr("TIMERS_XO_CLK_DIV_CFG", TIMERS_XO_CLK_DIV_CFG, xo & ~(1u << 31));
	wrr("TIMERS_XO_CLK_DIV_CFG", TIMERS_XO_CLK_DIV_CFG, xo | (1u << 31));
	wrr("COMP_CFG", COMP_CFG, rdr(COMP_CFG) | 0x180);	/* ipa_comp_cfg v4.5+: GSI/IPA clock gating bits */
	wrr("IDLE_INDICATION_CFG", IDLE_INDICATION_CFG, 0x00000100);
	static const uint32_t src[5] = { 0x0808, 0x0808, 0x1212, 0x0202, 0x0f0f };	/* ipa3_rsrc_src_grp_config 4.7 */
	static const uint32_t dst[2] = { 0x0707, 0x0202 };
	char n[32];
	for (unsigned i = 0; i < 5; i++) {
		snprintf(n, sizeof(n), "SRC_RSRC_GRP_01(%u)", i);
		wrr(n, SRC_RSRC_GRP_01(i), src[i]);
	}
	for (unsigned i = 0; i < 2; i++) {
		snprintf(n, sizeof(n), "DST_RSRC_GRP_01(%u)", i);
		wrr(n, DST_RSRC_GRP_01(i), dst[i]);
	}
	if (route)	/* default and fragment route to the AP LAN RX endpoint 9, header table, retain header */
		wrr("ROUTE", ROUTE, 0x01120052);
	else
		printf("ipa: ROUTE left as %#010x (pass --route to write 0x01120052 once LAN RX ep 9 exists)\n", rdr(ROUTE));
	return 0;
}

static int meminit(void)
{
	if (gate())
		return 1;
	uint32_t base, size;
	shared_mem(&base, &size);
	printf("ipa: local memory base %#x size %#x\n", base, size);
	if (size != 0x3000 || base + 0x3000 > 0x4b20) {
		printf("ipa: unexpected size/base (downstream ipa_4_7_mem_part ends at 0x3000): nothing done\n");
		return 1;
	}
	uint64_t m = IPA_SHARED + base;
	/* +0x0..+0x280 is the uC's area: left alone. The rest is zeroed (downstream ipa3_init_sram path). */
	printf("ipa: %s %#llx..%#llx\n", dry ? "would zero" : "zero", (unsigned long long)(m + 0x280),
	       (unsigned long long)(m + 0x3000));
	if (!dry)
		for (uint32_t o = 0x280; o < 0x3000; o += 4)
			wr32(m + o, 0);
	/* canaries: ipa3_sram_set_canary(X) writes 0xdeadbeef at X - 4, pairs before each table (ipa_4_7_mem_part) */
	static const uint32_t can[] = { 0x280, 0x284, 0x300, 0x304, 0x380, 0x384, 0x400, 0x404, 0x480, 0x484, 0x500,
					0x504, 0x580, 0x584, 0x600, 0x604, 0x680, 0x684, 0xac8, 0xacc, 0x1790, 0x24f0,
					0x27d0, 0x27d4, 0x2ffc };
	for (unsigned i = 0; i < sizeof(can) / sizeof(can[0]); i++) {
		if (!dry)
			wr32(m + can[i], 0xdeadbeef);
	}
	printf("ipa: %s %u canaries (0xdeadbeef)\n", dry ? "would write" : "wrote", (unsigned)(sizeof(can) / sizeof(can[0])));
	wrr("LOCAL_PKT_PROC_CNTXT", LOCAL_PKT_PROC_CNTXT, base + 0xad0);	/* modem_hdr_proc_ctx_ofst 0xad0 */
	return 0;
}

int ipa_main(int argc, char **argv)
{
	/* argv[0] = "ipahw", argv[1] = subcommand */
	if (argc < 2) {
		fprintf(stderr, "usage: wland ipahw regs|smmu|hwinit [--dry-run] [--route]|meminit [--dry-run]\n");
		return 2;
	}
	int route = 0;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--route"))
			route = 1;
	}
	if (!strcmp(argv[1], "regs"))
		return regs();
	if (!strcmp(argv[1], "smmu"))
		return smmu();
	if (!strcmp(argv[1], "hwinit"))
		return hwinit(route);
	if (!strcmp(argv[1], "meminit"))
		return meminit();
	fprintf(stderr, "wland ipa: unknown subcommand %s\n", argv[1]);
	return 2;
}
