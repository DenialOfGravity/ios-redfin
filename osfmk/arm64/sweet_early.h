/* Early screen diagnostics, valid only while the bootstrap TTBR0 is live. */
#ifndef _ARM64_SWEET_EARLY_H_
#define _ARM64_SWEET_EARLY_H_

#include <pexpert/arm64/board_config.h>
#include <stdarg.h>

#if defined(SWEET)
void sweet_early_mark(unsigned int stage, const char *label);
void sweet_early_putc(char c);
void sweet_early_panic(const char *fmt, va_list *args);
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
