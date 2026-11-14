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
 *   wland ipahw memdump [OFST LEN]    read-only dump of the local memory (default: flt/rt tables, drop stats)
 *   wland ipahw meminit [--dry-run]   local memory: zero above the uC area, canaries of downstream ipa_4_7_mem_part,
 *                                   LOCAL_PKT_PROC_CNTXT base
 *
 * ipa_smmu / ipa_meminit are also called in-process by wland's ipa=1 bring-up (wland.c, "IPA auto"). hwinit is not
 * part of it: the reset values already match what it would write (2026-11-06).
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

int ipa_smmu(void)
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

/* ipatbl=1 in /var/local/cell.on (Claude 2026-11-08, opt-in): the filter/route table init mainline's
 * ipa_table_setup() and downstream's _ipa_init_flt4_v3/_ipa_init_rt4_v3 do before INIT_DRIVER.  Without it the
 * tables are all zeros: filter bitmap 0 (the modem then put its DL filter for ep5 in slot 0) and every slot
 * "address 0".  With it:
 *   filter tables v4/v6 hash/nhash (0x288/0x308/0x388/0x408, 15 slots): slot 0 = endpoint bitmap, IPA < v5.0
 *     format (endpoint X -> bit X+1): ep2 AP_MODEM_TX, ep5 MODEM_AP_TX, ep8 MODEM_DL_NLO_TX (mainline
 *     ipa_data-v4.7.c filter_support) = 0x248; slots 1..14 = the zero rule.
 *   route tables v4/v6 hash/nhash (0x488/0x508/0x588/0x608, 15 entries; modem 0-7, AP 8-14) = the zero rule.
 * The zero rule: 8 zero bytes in DDR ("no filtering / no routing"), allocated once and kept for the life of
 * wland (the IPA reads it by physical address; the IPA AP stream is translation-off, ipa_smmu). */
#define FLT_EP_BITMAP	((1u << 2 | 1u << 5 | 1u << 8) << 1)
static uint64_t zero_rule_pa;

static int cell_flag(const char *w)
{
	char b[512] = "";
	FILE *f = fopen("/var/local/cell.on", "r");
	if (f) {
		size_t k = fread(b, 1, sizeof(b) - 1, f);
		b[k] = 0;
		fclose(f);
	}
	return strstr(b, w) != NULL;
}

static void slot_wr(uint64_t a, uint64_t v)
{
	wr32(a, (uint32_t)v);
	wr32(a + 4, (uint32_t)(v >> 32));
}

static void tables_init(uint64_t m)
{
	static const uint32_t flt[] = { 0x288, 0x308, 0x388, 0x408 }, rt[] = { 0x488, 0x508, 0x588, 0x608 };
	static const uint8_t zero[64];
	if (!zero_rule_pa) {
		zero_rule_pa = pm_alloc(4096, 4096);
		if (!zero_rule_pa || pm_wr(zero_rule_pa, zero, sizeof(zero))) {
			printf("ipa: ipatbl=1: zero rule alloc/write failed: tables left as zeros\n");
			zero_rule_pa = 0;
			return;
		}
	}
	for (unsigned t = 0; t < 4; t++) {
		slot_wr(m + flt[t], FLT_EP_BITMAP);
		for (unsigned i = 1; i < 15; i++)
			slot_wr(m + flt[t] + 8u * i, zero_rule_pa);
		for (unsigned i = 0; i < 15; i++)
			slot_wr(m + rt[t] + 8u * i, zero_rule_pa);
	}
	printf("ipa: ipatbl=1: filter bitmap %#x (ep2/5/8), all filter and route slots -> zero rule %#llx\n",
	       FLT_EP_BITMAP, (unsigned long long)zero_rule_pa);
}

int ipa_meminit(void)
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
	if (!dry && cell_flag("ipatbl=1"))
		tables_init(m);
	return 0;
}

/* memdump [OFST LEN] (read only, Claude 2026-11-08): the local memory as 32-bit words, 16 bytes a line, runs of
 * identical lines folded to "*" (od style).  Default: the filter and route tables (0x280..0x600 of downstream
 * ipa_4_7_mem_part: v4/v6 flt hash/nhash, v4/v6 rt hash/nhash, with their canaries) and the drop stats (0x27b0+0x20).
 * Tells whether the table entries are still the zeros meminit wrote or point at rules (modem or anyone else). */
static void memdump_range(uint64_t m, uint32_t o, uint32_t n)
{
	uint32_t prev[4] = { 0 }, cur[4];
	int have = 0, folded = 0;
	for (uint32_t a = o; a < o + n; a += 16) {
		for (int i = 0; i < 4; i++)
			cur[i] = rd32(m + a + 4u * i);
		if (have && !memcmp(cur, prev, sizeof(cur))) {
			if (!folded)
				printf("ipa: mem *\n");
			folded = 1;
			continue;
		}
		printf("ipa: mem %#06x: %08x %08x %08x %08x\n", a, cur[0], cur[1], cur[2], cur[3]);
		memcpy(prev, cur, sizeof(cur));
		have = 1;
		folded = 0;
	}
}

static int memdump(int argc, char **argv)
{
	if (gate())
		return 1;
	uint32_t base, size;
	shared_mem(&base, &size);
	uint64_t m = IPA_SHARED + base;
	printf("ipa: local memory base %#x size %#x (window %#llx)\n", base, size, (unsigned long long)IPA_SHARED);
	if (argc > 3) {
		uint32_t o = (uint32_t)strtoul(argv[2], NULL, 0) & ~15u, n = (uint32_t)strtoul(argv[3], NULL, 0);
		if (o + n > size || n == 0) {
			printf("ipa: range %#x+%#x outside the local memory\n", o, n);
			return 1;
		}
		memdump_range(m, o, n);
		return 0;
	}
	printf("ipa: tables 0x280..0x600 (flt v4/v6 hash/nhash 0x288/0x308/0x388/0x408, rt v4/v6 hash/nhash 0x488/0x508/0x588/0x608)\n");
	memdump_range(m, 0x280, 0x380 + 0x80);
	printf("ipa: stats_drop 0x27b0+0x20\n");
	memdump_range(m, 0x27b0, 0x20);
	return 0;
}

int ipa_main(int argc, char **argv)
{
	/* argv[0] = "ipahw", argv[1] = subcommand */
	if (argc < 2) {
		fprintf(stderr, "usage: wland ipahw regs|smmu|hwinit [--dry-run] [--route]|meminit [--dry-run]|memdump [OFST LEN]\n");
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
		return ipa_smmu();
	if (!strcmp(argv[1], "hwinit"))
		return hwinit(route);
	if (!strcmp(argv[1], "meminit"))
		return ipa_meminit();
	if (!strcmp(argv[1], "memdump"))
		return memdump(argc, argv);
	fprintf(stderr, "wland ipa: unknown subcommand %s\n", argv[1]);
	return 2;
}
