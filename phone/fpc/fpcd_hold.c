/* xnu 1235 wake: see fpcd_hold.h. tid26: the first version re-locked st_mx (cancelled()) while holding it: hang. */
#include <time.h>
#include <unistd.h>
#include "fpcd_hold.h"

static long hold_now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int fpcd_hold_wait(const struct fpcd_hold_st *h, uint32_t gen, long max_ms)
{
	long t0 = hold_now_ms();
	for (;;) {
		pthread_mutex_lock(h->mx);
		int moved = *h->stop || *h->cancel || *h->gen != gen;
		int taken = *h->held_ms == 0;
		int expired = !moved && !taken && hold_now_ms() - t0 >= max_ms;
		if (expired)
			*h->held_ms = 0;
		pthread_mutex_unlock(h->mx);
		if (moved)
			return 1;
		if (taken)
			return 2;
		if (expired)
			return 0;
		usleep(10000);
	}
}
