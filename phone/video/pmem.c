#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pmem.h"

static int pm_fd = -1;

/* redfin track V (venusctl --step): every write, SMC and RPMh vote logged to a file and fsynced BEFORE it is done, so
 * the last line survives a reset. Repeated wr32 of the same value to the same address (queue polling) is logged once. */
static int step_fd = -1, step_con = -1;
static off_t step_off;
static double step_t0;
static struct { uint64_t pa; uint32_t v; } step_last[16];
#define STEP_SIZE	(4 << 20)
#ifndef F_FULLFSYNC
#define F_FULLFSYNC	51	/* Darwin */
#endif

static void step_sync(void)
{
	if (fcntl(step_fd, F_FULLFSYNC) < 0)
		fsync(step_fd);
}

/*
 * rn4x: plain appends + fsync lost every line of the run that reset the phone (the file's new length is metadata, and
 * fsync on Darwin does not flush the drive). So the file is a fixed 4 MB of zeros, written once (F_FULLFSYNC), and
 * lines go in place with pwrite at the first zero byte + F_FULLFSYNC: no length or extent change per line. Read it
 * with `tr -d '\000' < FILE`. Every line also goes to /dev/console (the kernel log; VENUSCTL_STEP_CONSOLE=0: not).
 */
int pm_step_open(const char *path)
{
	step_fd = open(path, O_RDWR | O_CREAT, 0644);
	if (step_fd < 0) {
		fprintf(stderr, "step log %s: %s\n", path, strerror(errno));
		return -1;
	}
	static char buf[1 << 16];
	off_t size = lseek(step_fd, 0, SEEK_END), o;
	if (size < STEP_SIZE) {	/* new or short: fill to STEP_SIZE with zeros once */
		memset(buf, 0, sizeof(buf));
		for (o = size; o < STEP_SIZE; o += (off_t)sizeof(buf))
			if (pwrite(step_fd, buf, sizeof(buf), o) < 0)
				break;
		step_sync();
	}
	step_off = -1;	/* the first zero byte: where the last run stopped */
	for (o = 0; o < STEP_SIZE && step_off < 0; o += (off_t)sizeof(buf)) {
		ssize_t n = pread(step_fd, buf, sizeof(buf), o);
		if (n <= 0)
			break;
		for (ssize_t i = 0; i < n; i++)
			if (!buf[i]) {
				step_off = o + i;
				break;
			}
	}
	if (step_off < 0 || step_off > STEP_SIZE - 65536) {	/* full: start over */
		memset(buf, 0, sizeof(buf));
		for (o = 0; o < STEP_SIZE; o += (off_t)sizeof(buf))
			if (pwrite(step_fd, buf, sizeof(buf), o) < 0)
				break;
		step_sync();
		step_off = 0;
	}
	if (!getenv("VENUSCTL_STEP_CONSOLE") || atoi(getenv("VENUSCTL_STEP_CONSOLE")))
		step_con = open("/dev/console", O_WRONLY | O_NOCTTY);
	step_t0 = mono_ms();
	pm_step("---- step log open (pid %d, offset %lld)", getpid(), (long long)step_off);
	return 0;
}

int pm_stepping(void)
{
	return step_fd >= 0;
}

void pm_step(const char *fmt, ...)
{
	if (step_fd < 0)
		return;
	char line[512];
	int n = snprintf(line, sizeof(line), "[%9.3f] ", (mono_ms() - step_t0) / 1e3);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	if (step_con >= 0) {
		static const char tag[] = "venus step: ";
		if (write(step_con, tag, sizeof(tag) - 1) < 0 || write(step_con, line, (size_t)n) < 0) {
			/* the console is a mirror only */
		}
	}
	if (step_off + n < STEP_SIZE && pwrite(step_fd, line, (size_t)n, step_off) == n)
		step_off += n;
	step_sync();
}

static void step_wr(uint64_t pa, const void *buf, size_t len)
{
	if (step_fd < 0)
		return;
	if (len == 4) {
		uint32_t v;
		memcpy(&v, buf, 4);
		unsigned slot = (unsigned)((pa >> 2) % 16);
		if (step_last[slot].pa == pa && step_last[slot].v == v)
			return;
		step_last[slot].pa = pa, step_last[slot].v = v;
		pm_step("wr32 %#llx = %#010x", (unsigned long long)pa, v);
		return;
	}
	uint32_t w0 = 0;
	memcpy(&w0, buf, len < 4 ? len : 4);
	pm_step("pwrite %#llx len %#zx (first word %#010x)", (unsigned long long)pa, len, w0);
}
unsigned long long pm_calls[2], pm_bytes[2];	/* wifi batch: pread/pwrite calls and bytes ([0] read, [1] write) */

void pm_init(void)
{
	if (pm_fd >= 0)
		return;
	pm_fd = open("/dev/pmem", O_RDWR);
	if (pm_fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n",
			strerror(errno));
		exit(1);
	}
}

int pm_rd(uint64_t pa, void *buf, size_t len)
{
	ssize_t n = pread(pm_fd, buf, len, (off_t)pa);
	pm_calls[0]++, pm_bytes[0] += len;
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: read %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

int pm_wr(uint64_t pa, const void *buf, size_t len)
{
	step_wr(pa, buf, len);
	ssize_t n = pwrite(pm_fd, buf, len, (off_t)pa);
	pm_calls[1]++, pm_bytes[1] += len;
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: write %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

uint32_t rd32(uint64_t pa)
{
	uint32_t v;
	return pm_rd(pa, &v, 4) ? 0xdeadbeef : v;
}

void wr32(uint64_t pa, uint32_t v)
{
	pm_wr(pa, &v, 4);
}

void wr64(uint64_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

uint64_t pm_alloc(uint64_t size, uint64_t align)
{
	struct pmem_alloc a = { .size = size, .align = align };
	if (ioctl(pm_fd, PMEM_ALLOC, &a) < 0) {
		fprintf(stderr, "pmem: alloc %llu: %s\n", (unsigned long long)size, strerror(errno));
		return 0;
	}
	return a.pa;
}

uint64_t pm_alloc32(uint64_t size, uint64_t align)
{
	struct pmem_alloc a = { .size = size, .align = align };
	if (ioctl(pm_fd, PMEM_ALLOC32, &a) < 0) {
		fprintf(stderr, "pmem: alloc32 %llu: %s\n", (unsigned long long)size, strerror(errno));
		return 0;
	}
	return a.pa;
}

int pm_free(uint64_t pa)
{
	return ioctl(pm_fd, PMEM_FREE, &pa);
}

void pm_smc(uint64_t x[8])
{
	struct pmem_smc s;
	memcpy(s.x, x, sizeof(s.x));
	pm_step("smc x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
		(unsigned long long)x[2], (unsigned long long)x[3], (unsigned long long)x[4]);
	if (ioctl(pm_fd, PMEM_SMC, &s) < 0) {
		fprintf(stderr, "pmem: smc: %s\n", strerror(errno));
		x[0] = (uint64_t)-1;
		return;
	}
	memcpy(x, s.x, 4 * sizeof(uint64_t));
	pm_step("smc -> %#llx %#llx %#llx %#llx", (unsigned long long)x[0], (unsigned long long)x[1],
		(unsigned long long)x[2], (unsigned long long)x[3]);
}

int pm_list(struct pmem_list *l)
{
	return ioctl(pm_fd, PMEM_LIST, l);
}

void delay_ms(unsigned ms)
{
	usleep(ms * 1000);
}

double mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int pass_waits(void)	/* lat2: PMEM_PASS_WAITS=1: loop passes count as ms (the old timeouts) */
{
	static int n = -1;
	if (n < 0)
		n = getenv("PMEM_PASS_WAITS") && atoi(getenv("PMEM_PASS_WAITS"));
	return n;
}

int wait_over(double t0, unsigned pass, unsigned ms)
{
	return pass_waits() ? pass >= ms : mono_ms() - t0 >= ms;
}

unsigned wait_ms(double t0, unsigned pass)
{
	return pass_waits() ? pass : (unsigned)(mono_ms() - t0 + 0.5);
}

/* lat2: the bit is checked once more after the deadline (as after the last 1 ms step before) */
int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	double t0 = mono_ms();
	for (unsigned i = 0;; i++) {
		if (!!(rd32(pa) & mask) == set)
			return 0;
		if (wait_over(t0, i, ms))
			return -1;
		delay_ms(1);
	}
}

int pm_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val)
{
	struct pmem_rpmh r;
	memset(&r, 0, sizeof(r));
	snprintf(r.res, sizeof(r.res), "%s", res);
	r.n = n;
	pm_step("rpmh %s n %u: off %#x val %#x", res, n, n ? off[0] : 0, n ? val[0] : 0);
	for (unsigned i = 0; i < n && i < 4; i++) {
		r.offset[i] = off[i];
		r.value[i] = val[i];
	}
	return ioctl(pm_fd, PMEM_RPMH, &r) < 0 ? errno : 0;
}
