/*
 * redfin 1067: stub of the kernel QMI router (filled in by 1068). With sweetbb & 2 and this stub nothing answers
 * CommCenter: do not boot 1067 alone with sweetbb & 2.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include "sweet_bb.h"

bool
sweet_qmux_on(void)
{
	return false;
}

void
sweet_qmux_start(void)
{
	printf("SWEET QMUX: not in this kernel (1067 stub)\n");
}

void
sweet_qmux_c2w(__unused unsigned pipe, __unused uint32_t sess, __unused unsigned type, __unused const void *data, __unused uint32_t len)
{
}

void
sweet_qmux_poll(void)
{
}

void
sweet_qmux_server(__unused uint32_t service, __unused uint32_t instance, __unused uint32_t node, __unused uint32_t port)
{
}

void
sweet_qmux_server_gone(__unused uint32_t node, __unused uint32_t port)
{
}

void
sweet_qmux_qrtr(__unused const struct sbb_qrtr_hdr *h, __unused const uint8_t *d, __unused uint32_t n)
{
}

int
sweet_qmux_stats(char *buf, size_t len)
{
	return snprintf(buf, len, "no router (1067 stub)");
}
#endif
