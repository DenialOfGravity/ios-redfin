/*
 * flame (SM8150) CPU frequency: the OSM hardware DCVS keeps a frequency
 * table per cluster and takes a desired table index per cluster; voltage
 * (CPR) and thermal limits (LMh) stay in hardware. ABL leaves the clusters
 * slow and nothing under XNU raises them. Linux: drivers/clk/qcom/
 * clk-cpu-osm.c (msm-4.14), bases from the live DT qcom,cpucc node.
 *
 * Boot-arg flamecpufreq: 0 leave as is, 1 (default) highest all-core
 * frequency, N > 1 the highest entry at or below N MHz.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void sweet_cpufreq_init(void);

#define OSM_FREQ_REG    0x110u          /* + 32 * index */
#define OSM_ROW         32u
#define OSM_ROWS        40u
#define OSM_DESIRED     0x920u          /* + 4 * core in cluster; core 0 drives the policy */
#define OSM_XO_KHZ      19200u

static const struct {
	const char *name;
	uint32_t pa;
	unsigned ncores;
} osm_clusters[] = {
	{"pwrcl (A55 x6)", 0x18323000u, 6},
	{"perfcl (A76 x2)", 0x18325800u, 2},
	/* redfin (lito) cpufreq-hw third domain: the 2.4 GHz prime core (live DT qcom,cpufreq-hw reg 2) */
	{"prime (A76 x1)", 0x18327800u, 1},
};

static uint32_t
osm_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

void
sweet_cpufreq_init(void)
{
	unsigned want = 1;
	(void)PE_parse_boot_argn("flamecpufreq", &want, sizeof(want));
	for (unsigned c = 0; c < sizeof(osm_clusters) / sizeof(osm_clusters[0]); ++c) {
		vm_offset_t base = ml_io_map(osm_clusters[c].pa, 0x1000);
		if (!base) {
			printf("SWEET CPUFREQ: %s: map failed\n", osm_clusters[c].name);
			continue;
		}
		uint32_t cur = osm_rd(base, OSM_DESIRED);
		unsigned best = cur, best_khz = 0, prev_khz = 0, n = 0;
		char line[256];
		unsigned len = 0;
		for (unsigned i = 0; i < OSM_ROWS; ++i) {
			uint32_t data = osm_rd(base, OSM_FREQ_REG + i * OSM_ROW);
			unsigned src = data >> 30, lval = data & 0xffu, cores = (data >> 16) & 7u;
			unsigned khz = src ? OSM_XO_KHZ * lval : 300000u;
			if (i > 0 && khz == prev_khz) {
				break; /* a repeated frequency ends the table */
			}
			prev_khz = khz;
			n = i + 1;
			if (len < sizeof(line) - 16) {
				len += snprintf(line + len, sizeof(line) - len, " %u%s", khz / 1000,
				    cores < osm_clusters[c].ncores ? "*" : "");
			}
			/* fewer cores than the cluster has = boost entry: skip, as Linux's
			 * cpufreq-hw does (on the 1-core prime cluster every entry is normal) */
			bool boost = cores < osm_clusters[c].ncores;
			if (boost || (want > 1 && khz / 1000 > want)) {
				continue;
			}
			if (khz > best_khz) {
				best_khz = khz;
				best = i;
			}
		}
		printf("SWEET CPUFREQ: %s table (MHz, *=1-core boost):%s\n", osm_clusters[c].name, line);
		if (want == 0 || best_khz == 0 || best >= n) {
			printf("SWEET CPUFREQ: %s index %u kept\n", osm_clusters[c].name, cur);
			continue;
		}
		__asm__ volatile("dmb oshst" ::: "memory");
		*(volatile uint32_t *)(base + OSM_DESIRED) = best;
		__asm__ volatile("dsb osh" ::: "memory");
		printf("SWEET CPUFREQ: %s index %u -> %u (%u MHz), reads back %u\n",
		    osm_clusters[c].name, cur, best, best_khz / 1000, osm_rd(base, OSM_DESIRED));
	}
	extern void sweet_thermal_start(void);
	sweet_thermal_start();  /* 0960: TSENS sampling, bsd/dev/sweet_thermal.c */
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
