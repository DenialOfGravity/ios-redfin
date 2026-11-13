/*
 * smemdump: read-only view of the SMEM partition table ($TOC) and of the GLINK FIFO indices of one host pair,
 * through /dev/pmem (boot-arg sweetpmem=1). Runs next to wland (it never writes, never takes the hw mutex).
 *   smemdump toc                       every $TOC entry: offset, size, flags, hosts, cacheline
 *   smemdump pair H0 H1 [--read]       TOC entry of that pair; with APPS (0) in the pair, or with --read, also the
 *                                      $PRT header and GLINK items 478 (descriptor) / 479 / 480 (FIFOs): sizes and
 *                                      the four head/tail indices (never FIFO contents)
 *   smemdump pair H0 H1 watch N [--read]   one line per second for N seconds, '*' when any index moved
 * Hosts: 0 APPS, 1 MPSS (modem), 2 LPASS (ADSP), 3 DSPS (SLPI), 5 CDSP, 0xfffe global.
 * A partition without APPS is not read unless --read is given: Linux qcom smem skips such partitions, and on the
 * Pixel 4 reading one (ADSP-SLPI) stalled the bus for good (phone needs a hard reset). Same layout as
 * phone/wifi/smem.c (Linux drivers/soc/qcom/smem.c, qcom_glink_smem.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SMEM_BASE	0x80900000ull	/* lito smem_mem */
#define SMEM_SIZE	0x200000ull
#define PTABLE		(SMEM_BASE + SMEM_SIZE - 0x1000)
#define TOC_MAGIC	0x434f5424	/* "$TOC" */
#define PRT_MAGIC	0x54525024	/* "$PRT" */
#define CANARY		0xa5a5
#define HOST_APPS	0
#define HOST_GLOBAL	0xfffe

static int fd = -1;
static int rd_err;

static uint32_t rd32(uint64_t pa)
{
	uint32_t v;
	if (pread(fd, &v, 4, (off_t)pa) != 4) {
		if (!rd_err)
			fprintf(stderr, "smemdump: read at %llx: %s\n", (unsigned long long)pa, strerror(errno));
		rd_err = 1;
		return 0;
	}
	return v;
}

static uint16_t rd16(uint64_t pa)
{
	uint32_t w = rd32(pa & ~3ull);
	return (uint16_t)(w >> (8 * (pa & 2)));
}

struct ent {
	unsigned idx;
	uint32_t off, size, flags, cacheline;
	uint16_t h0, h1;
};

static int toc_read(struct ent *e, unsigned max, unsigned *n)
{
	uint32_t m = rd32(PTABLE);
	if (rd_err || m != TOC_MAGIC) {
		printf("no $TOC at %llx (%08x)\n", (unsigned long long)PTABLE, m);
		return -1;
	}
	uint32_t cnt = rd32(PTABLE + 8);
	*n = 0;
	for (uint32_t i = 0; i < cnt && i < 80 && *n < max; i++) {
		uint64_t a = PTABLE + 32 + 48ull * i;
		struct ent *p = &e[*n];
		p->idx = i;
		p->off = rd32(a);
		p->size = rd32(a + 4);
		p->flags = rd32(a + 8);
		uint32_t h = rd32(a + 12);
		p->h0 = (uint16_t)h;
		p->h1 = (uint16_t)(h >> 16);
		p->cacheline = rd32(a + 16);
		(*n)++;
	}
	return rd_err ? -1 : 0;
}

static void ent_print(const struct ent *p)
{
	printf("toc %u: hosts %u-%u off %x size %x flags %08x cacheline %u%s\n", p->idx, p->h0, p->h1, p->off,
	       p->size, p->flags, p->cacheline,
	       p->h0 == HOST_APPS || p->h1 == HOST_APPS || (p->h0 == HOST_GLOBAL && p->h1 == HOST_GLOBAL)
		       ? " [apps]" : " [no apps: not read by Linux]");
}

/* item lookup in a partition: uncached list from the header up, cached list from the end down */
static int item_find(uint64_t pa, uint32_t psize, uint32_t cl, unsigned item, uint64_t *ipa, uint32_t *isz)
{
	uint32_t free_unc = rd32(pa + 12), free_cac = rd32(pa + 16);
	if (rd_err || free_unc > psize || free_cac > psize)
		return -2;
	for (uint64_t e = pa + 32; e < pa + free_unc;) {
		if (rd16(e) != CANARY || rd_err)
			return -2;
		uint32_t sz = rd32(e + 4);
		uint16_t pad_data = rd16(e + 8), pad_hdr = rd16(e + 10);
		if (rd16(e + 2) == item) {
			*ipa = e + 16 + pad_hdr;
			*isz = sz - pad_data;
			return 0;
		}
		e += 16 + pad_hdr + sz;
	}
	if (!cl)
		cl = 64;
	uint32_t hdr = (16 + cl - 1) & ~(cl - 1);
	for (uint64_t e = pa + psize - hdr; e > pa + free_cac;) {
		if (rd16(e) != CANARY || rd_err)
			return -2;
		uint32_t sz = rd32(e + 4);
		if (rd16(e + 2) == item) {
			*ipa = e - sz;
			*isz = sz - rd16(e + 8);
			return 0;
		}
		e -= sz + hdr;
	}
	return -1;
}

static void now(char *buf, size_t n)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	snprintf(buf, n, "%ld.%03ld", (long)ts.tv_sec, ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
	int force = 0, watch = 0;
	unsigned h0 = 0, h1 = 0, secs = 0;
	int pos = 0;
	const char *args[6];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--read"))
			force = 1;
		else if (pos < 6)
			args[pos++] = argv[i];
	}
	if (pos == 1 && !strcmp(args[0], "toc")) {
		/* fine */
	} else if (pos >= 3 && !strcmp(args[0], "pair") &&
		   (pos == 3 || (pos == 5 && !strcmp(args[3], "watch")))) {
		h0 = (unsigned)strtoul(args[1], NULL, 0);
		h1 = (unsigned)strtoul(args[2], NULL, 0);
		if (pos == 5) {
			watch = 1;
			secs = (unsigned)strtoul(args[4], NULL, 0);
		}
	} else {
		fprintf(stderr, "usage: smemdump toc | smemdump pair H0 H1 [watch SECONDS] [--read]\n");
		return 2;
	}
	fd = open("/dev/pmem", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (boot-arg sweetpmem=1, run as root)\n", strerror(errno));
		return 1;
	}
	struct ent toc[80];
	unsigned n;
	if (toc_read(toc, 80, &n))
		return 1;
	if (!strcmp(args[0], "toc")) {
		printf("$TOC version %u, %u entries\n", rd32(PTABLE + 4), rd32(PTABLE + 8));
		for (unsigned i = 0; i < n; i++)
			ent_print(&toc[i]);
		return 0;
	}
	struct ent *p = NULL;
	for (unsigned i = 0; i < n; i++)
		if ((toc[i].h0 == h0 && toc[i].h1 == h1) || (toc[i].h0 == h1 && toc[i].h1 == h0))
			p = &toc[i];
	if (!p) {
		printf("pair %u-%u: no $TOC entry (no private partition for this pair)\n", h0, h1);
		return 1;
	}
	ent_print(p);
	if (!p->off || !p->size) {
		printf("pair %u-%u: entry has no offset/size: partition not set up\n", h0, h1);
		return 1;
	}
	int apps = p->h0 == HOST_APPS || p->h1 == HOST_APPS;
	if (!apps && !force) {
		printf("partition not read: APPS is not a host of it, so it is likely XPU-protected from APPS and a read can\n"
		       "stall the bus until a hard reset. Add --read to try anyway (first read = one word, the $PRT magic).\n");
		return 3;
	}
	uint64_t pa = SMEM_BASE + p->off;
	if (!apps) {
		printf("reading %llx (--read)...\n", (unsigned long long)pa);
		fflush(stdout);
		sync();
	}
	uint32_t m = rd32(pa);
	if (rd_err || m != PRT_MAGIC) {
		printf("partition at %llx: no $PRT magic (%08x%s): no read access from APPS or not set up\n",
		       (unsigned long long)pa, m, rd_err ? ", read error" : "");
		return 1;
	}
	uint32_t ph = rd32(pa + 4);
	printf("prt %llx: hosts %u-%u size %x free unc %x cac %x\n", (unsigned long long)pa, ph & 0xffff, ph >> 16,
	       rd32(pa + 8), rd32(pa + 12), rd32(pa + 16));
	uint64_t desc = 0, f479 = 0, f480 = 0;
	uint32_t dsz = 0, s479 = 0, s480 = 0;
	int rd = item_find(pa, p->size, p->cacheline, 478, &desc, &dsz);
	int r9 = item_find(pa, p->size, p->cacheline, 479, &f479, &s479);
	int r0 = item_find(pa, p->size, p->cacheline, 480, &f480, &s480);
	if (rd == -2 || r9 == -2 || r0 == -2)
		printf("partition walk hit a bad canary/read error (allocation in progress or unreadable)\n");
	printf("items: 478 desc %s pa %llx size %u | 479 fifo %s size %u | 480 fifo %s size %u\n",
	       rd ? "MISSING" : "ok", (unsigned long long)desc, dsz, r9 ? "MISSING" : "ok", s479,
	       r0 ? "MISSING" : "ok", s480);
	if (rd) {
		printf("no GLINK descriptor: the edge for this pair was never set up\n");
		return 1;
	}
	/* desc words as the allocating side sees them: [0] tx tail [1] tx head [2] rx tail [3] rx head; side A's tx
	 * = FIFO 479 (A -> B), side B writes FIFO 480 (B -> A) */
	uint32_t prev[4] = { 0 }, moved_lines = 0;
	for (unsigned t = 0;; t++) {
		uint32_t d[4];
		for (int i = 0; i < 4; i++)
			d[i] = rd32(desc + 4 * i);
		if (rd_err)
			return 1;
		int mv = t && memcmp(d, prev, sizeof(d));
		moved_lines += mv;
		char ts[32];
		now(ts, sizeof(ts));
		printf("%s pair %u-%u 479(A->B) tail %u head %u | 480(B->A) tail %u head %u%s\n", ts, h0, h1, d[0], d[1],
		       d[2], d[3], mv ? " *" : "");
		fflush(stdout);
		memcpy(prev, d, sizeof(d));
		if (!watch || t >= secs)
			break;
		sleep(1);
	}
	if (watch)
		printf("summary: %u of %u intervals moved%s\n", moved_lines, secs,
		       moved_lines ? " (edge alive)" : " (indices frozen)");
	return 0;
}
