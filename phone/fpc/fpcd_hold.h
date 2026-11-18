/* xnu 1235 wake: sweetfpcd's hold of the screen-off detect's wake-touch image (fpcd_hold.c, tested by test_hold.c) */
#ifndef FPCD_HOLD_H
#define FPCD_HOLD_H
#include <pthread.h>
#include <signal.h>
#include <stdint.h>

struct fpcd_hold_st {
	pthread_mutex_t *mx;			/* guards cancel, gen, held_ms (sweetfpcd: st_mx) */
	const volatile sig_atomic_t *stop;	/* g_stop (signal) */
	const int *cancel;			/* S.cancel */
	const uint32_t *gen;			/* S.gen: the op generation */
	long *held_ms;				/* S.held_ms: != 0 while the image waits for a match op */
};

/*
 * Wait while the image is held, without holding mx in between (the caller must NOT hold mx). Returns 1 when the op
 * moved on (stop, cancel or a new generation: held_ms is kept for the next op, run_match takes it), 2 when held_ms
 * was taken, 0 after max_ms with neither (held_ms cleared: detection goes on).
 */
int fpcd_hold_wait(const struct fpcd_hold_st *h, uint32_t gen, long max_ms);
#endif
