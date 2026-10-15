/* Screen diagnostics through bootstrap TTBR0 and the final kernel TTBR1. */
#ifndef _ARM64_SWEET_EARLY_H_
#define _ARM64_SWEET_EARLY_H_

#include <pexpert/arm64/board_config.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(SWEET)
void sweet_early_mark(unsigned int stage, const char *label);
void sweet_early_putc(char c);
void sweet_early_panic(const char *fmt, va_list *args);
void sweet_early_relocate(uint64_t base);
void sweet_early_disable(void);
#else
static inline void sweet_early_mark(unsigned int stage, const char *label)
{
	(void)stage;
	(void)label;
}
static inline void sweet_early_disable(void) {}
#endif

#endif
