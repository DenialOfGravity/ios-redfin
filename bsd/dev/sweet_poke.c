/*
 * Register poke scripts: hardware experiments without a kernel rebuild.
 * tools/mkpayload.py --poke FILE puts FILE in the ADT as /chosen/sweet-poke;
 * a kernel thread runs it line by line and logs every step ("POKE:").
 *
 *   @SECS                  wait until SECS seconds after the timer started
 *   r ADDR [N]             read N (default 1) 32-bit words
 *   w ADDR VAL             write
 *   m ADDR MASK VAL        read-modify-write: (old & ~MASK) | (VAL & MASK)
 *   p ADDR MASK VAL MS     poll until (read & MASK) == VAL, at most MS ms
 *   s MS                   sleep
 *   # ...                  comment
 *
 * Numbers are hex with 0x, else decimal. Addresses are physical; each 4K
 * page is mapped once (device memory). A read of an unclocked block hangs
 * the SoC: the RAM log rescue (shim/ramlog.c) keeps the lines up to it.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <mach/mach_time.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
void sweet_poke_start(void);
void sweet_poke_run(const char *text, unsigned len);

#define POKE_MAPS       32
#define POKE_TICKS_S    19200000ULL     /* flame timer */

static struct {
	uint64_t page;
	vm_offset_t va;
} poke_map[POKE_MAPS];
static const char *poke_script;
static unsigned poke_len;

static volatile uint32_t *
poke_reg(uint64_t pa)
{
	uint64_t page = pa & ~0xfffULL;
	for (unsigned i = 0; i < POKE_MAPS; i++) {
		if (poke_map[i].va && poke_map[i].page == page) {
			return (volatile uint32_t *)(poke_map[i].va + (pa & 0xfff));
		}
	}
	for (unsigned i = 0; i < POKE_MAPS; i++) {
		if (!poke_map[i].va) {
			poke_map[i].va = ml_io_map((vm_offset_t)page, 0x1000);
			poke_map[i].page = page;
			return poke_map[i].va ? (volatile uint32_t *)(poke_map[i].va + (pa & 0xfff)) : NULL;
		}
	}
	return NULL;
}

static uint32_t
poke_rd(volatile uint32_t *r)
{
	uint32_t v = *r;
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
poke_wr(volatile uint32_t *r, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*r = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static void
poke_sleep_ms(unsigned ms)
{
	delay((int)ms * 1000);
}

/* Parse one number; returns false at end of line. */
static bool
poke_num(const char **p, const char *end, uint64_t *out)
{
	const char *s = *p;
	while (s < end && (*s == ' ' || *s == '\t')) {
		s++;
	}
	if (s >= end || *s == '\n' || *s == '#') {
		*p = s;
		return false;
	}
	uint64_t v = 0;
	unsigned base = 10;
	if (s + 1 < end && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	const char *d0 = s;
	for (; s < end; s++) {
		unsigned c = (unsigned char)*s, digit;
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (base == 16 && c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else if (base == 16 && c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		} else if (c == '_') {
			continue;
		} else {
			break;
		}
		v = v * base + digit;
	}
	*p = s;
	*out = v;
	return s > d0;
}

static void
poke_line(const char *s, const char *end, unsigned lineno)
{
	while (s < end && (*s == ' ' || *s == '\t')) {
		s++;
	}
	if (s >= end || *s == '#' || *s == '\n') {
		return;
	}
	char op = *s++;
	uint64_t a = 0, b = 0, c = 0, d = 0;
	bool ha = poke_num(&s, end, &a), hb = poke_num(&s, end, &b);
	bool hc = poke_num(&s, end, &c), hd = poke_num(&s, end, &d);
	volatile uint32_t *r = NULL;
	if (op != '@' && op != 's' && op != 'v') {
		if (!ha || (a & 3) || !(r = poke_reg(a))) {
			printf("POKE: line %u: bad address\n", lineno);
			return;
		}
	}
	switch (op) {
	case '@': {
		uint64_t target = a * POKE_TICKS_S;
		while (mach_absolute_time() < target) {
			poke_sleep_ms(10);
		}
		printf("POKE: @%llu s\n", a);
		break;
	}
	case 's':
		poke_sleep_ms((unsigned)a);
		break;
	case 'v': {
		/* redfin (0907): v MS [STRENGTH 1..127] buzzes the DRV2624 (bsd/dev/sweet_haptic.c, sweethaptic=1) */
		extern int sweet_haptic_buzz(unsigned ms, unsigned strength);
		printf("POKE: v %llu ms -> %d\n", a, sweet_haptic_buzz((unsigned)a, hb ? (unsigned)b : 127));
		break;
	}
	case 'r': {
		uint64_t n = hb ? b : 1;
		for (uint64_t i = 0; i < n && i < 256; i += 4) {
			char line[96];
			int len = snprintf(line, sizeof(line), "POKE: r %09llx:", a + 4 * i);
			for (uint64_t j = i; j < n && j < i + 4; j++) {
				volatile uint32_t *rj = poke_reg(a + 4 * j);
				len += snprintf(line + len, sizeof(line) - len, " %08x", rj ? poke_rd(rj) : 0xdeadbeef);
			}
			printf("%s\n", line);
		}
		break;
	}
	case 'w':
		poke_wr(r, (uint32_t)b);
		printf("POKE: w %09llx = %08llx -> %08x\n", a, b, poke_rd(r));
		break;
	case 'm': {
		uint32_t old = poke_rd(r);
		poke_wr(r, (old & ~(uint32_t)b) | ((uint32_t)c & (uint32_t)b));
		printf("POKE: m %09llx %08x -> %08x\n", a, old, poke_rd(r));
		break;
	}
	case 'p': {
		uint64_t ms = hd ? d : 100, t;
		uint32_t v = 0;
		for (t = 0; t <= ms; t++) {
			v = poke_rd(r);
			if ((v & (uint32_t)b) == (uint32_t)c) {
				break;
			}
			poke_sleep_ms(1);
		}
		printf("POKE: p %09llx & %08llx == %08llx: %s after %llu ms (%08x)\n", a, b, c,
		    t <= ms ? "ok" : "TIMEOUT", t, v);
		break;
	}
	default:
		printf("POKE: line %u: unknown op '%c'\n", lineno, op);
	}
	(void)hc;
}

/* Run script text in the calling thread (also used by the USB command server). */
void
sweet_poke_run(const char *text, unsigned len)
{
	const char *s = text, *end = text + len;
	unsigned lineno = 1;
	while (s < end && *s) {
		const char *e = s;
		while (e < end && *e && *e != '\n') {
			e++;
		}
		poke_line(s, e, lineno++);
		s = e < end && *e == '\n' ? e + 1 : e;
	}
}

static void
poke_thread(void *arg __unused, wait_result_t wr __unused)
{
	printf("POKE: running %u-byte script\n", poke_len);
	sweet_poke_run(poke_script, poke_len);
	printf("POKE: done\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_poke_start(void)
{
	DTEntry chosen;
	void const *script;
	unsigned size;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "sweet-poke", &script, &size) != kSuccess || size < 2) {
		return;
	}
	poke_script = (const char *)script;
	poke_len = size;
	thread_t thread;
	if (kernel_thread_start(poke_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
