/*
 * surfinuse: IOSurfaceIsInUse probe for xnu 1258 (kern.sweet_surf_inuse).
 *   surfinuse              create a surface; IsInUse; IncrementUseCount -> IsInUse; DecrementUseCount -> IsInUse;
 *                          then Increment and exit without Decrement, checked from a second process (posix_spawn of itself): the
 *                          count must go with the client that made it (1258: SweetSurfaceRef::free)
 *   surfinuse scan [MS]    look up every surface id (1..4095) and sample IsInUse every 2 ms for MS (default 500):
 *                          per surface the fraction of samples in use (GPU holds flip it; the display's surfaces
 *                          stay in use); one line per surface seen in use, then a summary
 *   surfinuse id N [MS]    the same for one surface
 * Exit 0 when the create test passes (in use after Increment, not after Decrement).
 */
#include <IOSurface/IOSurfaceRef.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
extern char **environ;
#include <time.h>

static IOSurfaceRef
make(int w, int h)
{
	int bpe = 4, fmt = 'BGRA';
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
	CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe), nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
	CFDictionarySetValue(d, kIOSurfaceWidth, nw);
	CFDictionarySetValue(d, kIOSurfaceHeight, nh);
	CFDictionarySetValue(d, kIOSurfaceBytesPerElement, nb);
	CFDictionarySetValue(d, kIOSurfacePixelFormat, nf);
	IOSurfaceRef s = IOSurfaceCreate(d);
	CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nf); CFRelease(d);
	return s;
}

static uint64_t
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int
sample(uint32_t lo, uint32_t hi, int ms)
{
	static IOSurfaceRef s[4096];
	static uint32_t hits[4096], flips[4096];
	static uint8_t last[4096];
	uint32_t found = 0, n = 0;
	for (uint32_t id = lo; id <= hi && id < 4096; id++) {
		s[id] = IOSurfaceLookup(id);
		found += s[id] != NULL;
	}
	uint64_t end = now_us() + (uint64_t)ms * 1000;
	while (now_us() < end) {
		for (uint32_t id = lo; id <= hi && id < 4096; id++) {
			if (s[id] == NULL) {
				continue;
			}
			uint8_t u = IOSurfaceIsInUse(s[id]) ? 1 : 0;
			hits[id] += u;
			if (n != 0 && u != last[id]) {
				flips[id]++;
			}
			last[id] = u;
		}
		n++;
		usleep(2000);
	}
	uint32_t always = 0, some = 0;
	for (uint32_t id = lo; id <= hi && id < 4096; id++) {
		if (s[id] == NULL) {
			continue;
		}
		if (hits[id] != 0) {
			printf("surface %u %zux%zu fmt %.4s: in use %u/%u samples, %u flips\n", id, IOSurfaceGetWidth(s[id]),
			    IOSurfaceGetHeight(s[id]), (char *)&(uint32_t){ __builtin_bswap32(IOSurfaceGetPixelFormat(s[id])) },
			    hits[id], n, flips[id]);
			always += hits[id] == n;
			some += hits[id] != n;
		}
		CFRelease(s[id]);
	}
	printf("SUMMARY: %u surfaces looked up, %u samples each: always in use %u, sometimes %u\n", found, n, always, some);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
		return sample(1, 4095, argc >= 3 ? atoi(argv[2]) : 500);
	}
	if (argc >= 3 && strcmp(argv[1], "child") == 0) {
		IOSurfaceRef t = IOSurfaceLookup((uint32_t)strtoul(argv[2], NULL, 0));
		if (t == NULL) {
			return 3;
		}
		IOSurfaceIncrementUseCount(t);
		return IOSurfaceIsInUse(t) ? 0 : 4;
	}
	if (argc >= 3 && strcmp(argv[1], "id") == 0) {
		uint32_t id = (uint32_t)strtoul(argv[2], NULL, 0);
		return sample(id, id, argc >= 4 ? atoi(argv[3]) : 500);
	}
	IOSurfaceRef s = make(256, 256);
	if (s == NULL) {
		printf("FAIL: IOSurfaceCreate\n");
		return 2;
	}
	uint32_t id = IOSurfaceGetID(s);
	int a = IOSurfaceIsInUse(s);
	IOSurfaceIncrementUseCount(s);
	int b = IOSurfaceIsInUse(s);
	int32_t cnt = IOSurfaceGetUseCount(s);
	IOSurfaceDecrementUseCount(s);
	int c = IOSurfaceIsInUse(s);
	printf("surface %u: IsInUse new %d, after Increment %d (GetUseCount %d), after Decrement %d\n", id, a, b, cnt, c);
	/* a child looks the surface up, increments and exits without decrementing: its count must go with it */
	char ids[16];
	snprintf(ids, sizeof(ids), "%u", id);
	char *cargv[] = { argv[0], "child", ids, NULL };
	pid_t p = 0;
	if (posix_spawn(&p, argv[0], NULL, NULL, cargv, environ) != 0) {
		printf("posix_spawn failed\n");
	}
	int st = 0;
	waitpid(p, &st, 0);
	usleep(100000);
	int d = IOSurfaceIsInUse(s);
	printf("child (pid %d) incremented and exited (status %d): IsInUse now %d\n", p, WIFEXITED(st) ? WEXITSTATUS(st) : -1, d);
	int ok = a == 0 && b == 1 && c == 0;
	printf("%s: Increment/Decrement %s; client death %s\n", ok ? "PASS" : "FAIL", ok ? "flip IsInUse" : "do not flip IsInUse",
	    d == 0 ? "drops its count" : "leaves its count behind");
	CFRelease(s);
	return ok ? 0 : 1;
}
