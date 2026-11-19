/*
 * pmemsnap: a screenshot of the surface the display scans, without the read's own black lines (ws-cam-lines).
 *
 *   pmemsnap RAW [REGS] [ROWS] [WIDTH]   (root; the surface's height and width, default 2436 and 1125)
 *
 * Reads VIG0's registers (/dev/pmem 0x0ae05000, 64 bytes: SRC0_ADDR +0x14, YSTRIDE0 +0x24) and the surface at
 * SRC0_ADDR, stride * ROWS bytes, into RAW (REGS: the 64 register bytes, as `pa shot` reads them).
 *
 * Why: /dev/pmem reads at ~10 MB/s (no mmap), so an 11 MB slot takes ~1.1 s to read, and a screen that is redrawn
 * every frame (the Camera preview: ~30 fps over two slots) redraws the slot being read some 15 times meanwhile. Each
 * redraw draws the black background first and the picture over it; where the read passes the redraw between the two
 * it reads black: thin full-width black lines, ~55-65 KB each (12-14 rows), one every read-rate / redraw-rate bytes
 * (~625-670 KB). They are in no picture of the chain (ws-cam-lines: the kernel's frames, the plug-in's buffers and
 * VideoToolbox's output all dumped clean; the period followed the read speed).
 *
 * So: where the first read has runs of 256 pixels to 64 rows of black (B, G, R == 0, in reading order, the rows'
 * padding skipped), those bytes are read again (up to PMEMSNAP_READS reads in all, default 12, 20 ms apart: they are
 * small) and each black pixel is taken from a later read that has picture there; a run three reads in a row mend
 * nothing of is the picture's own black and stays.
 * Prints "PA LEN STRIDE MS READS RUNS LEFT MENDED" (runs: black runs in the first read; left: runs still to read
 * again when the reads ran out; mended: pixels). PMEMSNAP_VERBOSE=1: a line per read on stderr.
 * PMEMSNAP_READS=1: one plain read (the old behaviour).
 *
 * Build (WSL): ~/redfin-build/toolchain/lb-bin/ios-cc -O2 -Wall -o pmemsnap pmemsnap.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define REGS_PA         0x0ae05000ull
#define RUN_MIN         256     /* pixels: shorter black runs are left alone (text, edges) */
#define RUN_MAX_ROWS    64      /* longer black runs are the picture's (a redraw caught by the read is ~12-14 rows) */
#define STILL_READS     3       /* a run black in this many reads in a row (PAUSE_US apart) is the picture's own */
#define PAUSE_US        20000   /* between reads: not the same moment of the next redraw */

static uint64_t
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static inline int
black(const uint8_t *p)
{
	return p[0] == 0 && p[1] == 0 && p[2] == 0;
}

static uint32_t g_width, g_stride;      /* visible pixels a row, bytes a row: runs skip the rows' padding */

static inline size_t
at(size_t k)
{
	return (k / g_width) * g_stride + (k % g_width) * 4;
}

struct run { size_t i, j; int still; };        /* visible pixels [i, j) in reading order; reads that mended none */

/* the runs of at least RUN_MIN black pixels of a within [i, j), appended to out (n, cap); returns their count */
static size_t
find_runs(const uint8_t *a, size_t i, size_t j, struct run **out, size_t *n, size_t *cap)
{
	size_t found = 0;

	while (i < j) {
		if (!black(a + at(i))) {
			i++;
			continue;
		}
		size_t k = i;

		while (k < j && black(a + at(k))) {
			k++;
		}
		if (k - i >= RUN_MIN && k - i <= (size_t)RUN_MAX_ROWS * g_width) {
			if (*n == *cap) {
				*cap = *cap ? *cap * 2 : 256;
				*out = realloc(*out, *cap * sizeof(**out));
				if (*out == NULL) {
					exit(1);
				}
			}
			(*out)[(*n)++] = (struct run){ i, k, 0 };
			found++;
		}
		i = k;
	}
	return found;
}

int
main(int argc, char **argv)
{
	uint8_t regs[64];
	uint32_t rows = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 2436, pa, stride;
	int fd, reads = getenv("PMEMSNAP_READS") ? atoi(getenv("PMEMSNAP_READS")) : 12, done = 1;
	unsigned first_runs = 0, left = 0;
	size_t mended = 0, mended_all = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: pmemsnap RAW [REGS] [ROWS]\n");
		return 1;
	}
	if ((fd = open("/dev/pmem", O_RDONLY)) < 0) {
		perror("pmemsnap: /dev/pmem");
		return 1;
	}
	if (pread(fd, regs, 64, (off_t)REGS_PA) != 64) {
		perror("pmemsnap: registers");
		return 1;
	}
	memcpy(&pa, regs + 0x14, 4);
	memcpy(&stride, regs + 0x24, 4);
	stride &= 0xffff;
	if (pa == 0 || stride == 0) {
		fprintf(stderr, "pmemsnap: VIG0 scans nothing (SRC0 %#x, stride %u)\n", pa, stride);
		return 1;
	}
	g_width = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 1125;
	g_stride = stride;
	if (g_width == 0 || g_width > stride / 4) {
		g_width = stride / 4;
	}
	size_t len = (size_t)stride * rows, px = (size_t)g_width * rows;
	uint8_t *a = malloc(len), *b = malloc(len);
	uint64_t t0 = now_ms();

	if (a == NULL || b == NULL || pread(fd, a, len, (off_t)pa) != (ssize_t)len) {
		fprintf(stderr, "pmemsnap: reading %zu bytes at %#x failed\n", len, pa);
		return 1;
	}
	/*
	 * Read again only the bytes under the runs (of RUN_MIN to RUN_MAX_ROWS rows' pixels), PAUSE_US apart; what a read
	 * has picture for is taken from it, and what is still black stays a run; a run that STILL_READS reads in a row
	 * mend nothing of is the picture's own black and is dropped.
	 */
	struct run *runs = NULL, *next = NULL;
	size_t n = 0, cap = 0, nn = 0, ncap = 0;

	first_runs = (unsigned)find_runs(a, 0, px, &runs, &n, &cap);
	while (n > 0 && done < reads) {
		uint8_t r2[64];
		uint32_t pa2;

		usleep(PAUSE_US);
		if (pread(fd, r2, 64, (off_t)REGS_PA) != 64) {
			break;
		}
		memcpy(&pa2, r2 + 0x14, 4);
		done++;
		mended = 0;
		nn = 0;
		for (size_t r = 0; r < n; r++) {
			size_t from = at(runs[r].i) & ~(size_t)63, to = at(runs[r].j - 1) + 4, got = 0;

			if (pread(fd, b + from, to - from, (off_t)(pa2 + from)) != (ssize_t)(to - from)) {
				continue;
			}
			for (size_t k = runs[r].i; k < runs[r].j; k++) {
				if (!black(b + at(k))) {
					memcpy(a + at(k), b + at(k), 4);
					got++;
				}
			}
			mended += got;
			if (got > 0) {
				size_t from_n = nn;

				find_runs(a, runs[r].i, runs[r].j, &next, &nn, &ncap);
				for (size_t q = from_n; q < nn; q++) {
					next[q].still = 0;
				}
			} else if (runs[r].still + 1 < STILL_READS) {
				if (nn == ncap) {
					ncap = ncap ? ncap * 2 : 256;
					next = realloc(next, ncap * sizeof(*next));
					if (next == NULL) {
						return 1;
					}
				}
				next[nn] = runs[r];
				next[nn++].still++;
			}
		}
		mended_all += mended;
		if (getenv("PMEMSNAP_VERBOSE") != NULL) {
			fprintf(stderr, "pmemsnap: read %d (%#x): %zu runs, %zu pixels mended, %zu runs to read again\n", done, pa2, n, mended, nn);
		}
		struct run *t = runs;

		runs = next;
		next = t;
		n = nn;
		size_t tc = cap;

		cap = ncap;
		ncap = tc;
	}
	left = (unsigned)n;
	uint64_t t1 = now_ms();
	FILE *f = fopen(argv[1], "wb");

	if (f == NULL || fwrite(a, 1, len, f) != len || fclose(f) != 0) {
		perror("pmemsnap: write");
		return 1;
	}
	if (argc > 2 && argv[2][0] != 0 && argv[2][0] != '-') {
		f = fopen(argv[2], "wb");
		if (f == NULL || fwrite(regs, 1, 64, f) != 64 || fclose(f) != 0) {
			perror("pmemsnap: write registers");
			return 1;
		}
	}
	printf("%#x %zu %u %llu %d %u %u %zu\n", pa, len, stride, (unsigned long long)(t1 - t0), done, first_runs, left, mended_all);
	return 0;
}
