/*
 * Redmi Note 10 Pro bootstrap console. No allocation, locks, ADT parsing or
 * per-CPU state: panic() can reach this before a current thread exists.
 * start.s maps the splash framebuffer VA=PA as Normal Non-Cacheable memory.
 * arm_vm_init installs a reserved kernel VA alias before removing TTBR0;
 * retain this console until the normal framebuffer serial console is ready.
 */
#include <arm64/sweet_early.h>

#if defined(SWEET)
#include <stdint.h>
#include <kern/misc_protos.h>

static volatile uint32_t *early_fb = (volatile uint32_t *)(uintptr_t)0x9c000000;
#define EARLY_FB early_fb
#define EARLY_WIDTH 1080u
#define EARLY_HEIGHT 2400u
#define EARLY_STRIDE (4320u / 4)
#define EARLY_SCALE 2u
#define EARLY_CELL_W (8u * EARLY_SCALE)
#define EARLY_CELL_H (16u * EARLY_SCALE)
#define EARLY_ROWS ((EARLY_HEIGHT - 32u) / EARLY_CELL_H)
#define EARLY_COLS (EARLY_WIDTH / EARLY_CELL_W)

extern unsigned char iso_font[256 * 16];

/* Initialized data must remain writable until disable, including panics. */
static unsigned int early_row, early_col;
static unsigned int early_started, early_disabled, early_panicking;

static void
sweet_early_clear_row(void)
{
	for (unsigned int y = 0; y < EARLY_CELL_H; y++) {
		volatile uint32_t *line = EARLY_FB + (early_row * EARLY_CELL_H + y) * EARLY_STRIDE;
		for (unsigned int x = 0; x < EARLY_WIDTH; x++) {
			line[x] = 0xff000000u;
		}
	}
}

void
sweet_early_putc(char c)
{
	if (early_disabled) {
		return;
	}
	if (!early_started) {
		early_started = 1;
		sweet_early_clear_row();
	}
	if (c == '\r') {
		early_col = 0;
		return;
	}
	if (c == '\n' || early_col == EARLY_COLS) {
		early_col = 0;
		early_row = (early_row + 1) % EARLY_ROWS;
		sweet_early_clear_row();
		if (c == '\n') {
			return;
		}
	}
	const unsigned char *glyph = iso_font + (unsigned char)c * 16;
	for (unsigned int gy = 0; gy < 16; gy++) {
		for (unsigned int sy = 0; sy < EARLY_SCALE; sy++) {
			volatile uint32_t *line = EARLY_FB +
			    (early_row * EARLY_CELL_H + gy * EARLY_SCALE + sy) * EARLY_STRIDE +
			    early_col * EARLY_CELL_W;
			for (unsigned int gx = 0; gx < 8; gx++) {
				uint32_t pixel = (glyph[gy] & (1u << gx)) ? 0xffffffffu : 0xff000000u;
				for (unsigned int sx = 0; sx < EARLY_SCALE; sx++) {
					line[gx * EARLY_SCALE + sx] = pixel;
				}
			}
		}
	}
	early_col++;
}

static void
sweet_early_string(const char *s)
{
	while (*s) {
		sweet_early_putc(*s++);
	}
}

void
sweet_early_mark(unsigned int stage, const char *label)
{
	if (early_disabled || stage > 20) {
		return;
	}
	uint32_t color = 0xff00ff00u | ((stage & 7u) * 32u << 16);
	for (unsigned int y = EARLY_HEIGHT - 32; y < EARLY_HEIGHT; y++) {
		for (unsigned int x = stage * 48; x < (stage + 1) * 48; x++) {
			EARLY_FB[y * EARLY_STRIDE + x] = color;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	sweet_early_string("SWEET EARLY ");
	if (stage >= 10) {
		sweet_early_putc((char)('0' + stage / 10));
	}
	sweet_early_putc((char)('0' + stage % 10));
	sweet_early_string(": ");
	sweet_early_string(label);
	sweet_early_putc('\n');
}

void
sweet_early_panic(const char *fmt, va_list *args)
{
	if (early_disabled || early_panicking) {
		return;
	}
	early_panicking = 1;
	sweet_early_string("\nSWEET EARLY PANIC: ");
	va_list copy;
	va_copy(copy, *args);
	_doprnt(fmt, &copy, sweet_early_putc, 16);
	va_end(copy);
	sweet_early_putc('\n');
	__asm__ volatile("dsb sy" ::: "memory");
}

void
sweet_early_relocate(uint64_t base)
{
	sweet_early_mark(8, "leaving bootstrap TTBR0");
	early_fb = (volatile uint32_t *)(uintptr_t)base;
	__asm__ volatile("dsb sy" ::: "memory");
	sweet_early_mark(9, "kernel framebuffer VA live");
}

void
sweet_early_disable(void)
{
	early_disabled = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}
#endif /* SWEET */
