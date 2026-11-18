/*
 * Host test of fpcd_hold.c (no TA, no phone): cc -O2 -Wall -pthread -o /tmp/test_hold test_hold.c fpcd_hold.c && /tmp/test_hold
 * 1 wake-hold, then an immediate Cancel from the bridge thread (tid26 hang): returns 1 at once, image kept, and the
 *   bridge thread's lock of the state mutex never blocks for long; 2 the op already moved on before the hold;
 * 3 nothing happens: returns 0 after max_ms with the image dropped; 4 the image is taken; 5 the state mutex is free
 *   after every return (a self-deadlock or a leaked lock fails here). alarm(5) fails the test on any hang.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "fpcd_hold.h"

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop;
static int cancel;
static uint32_t gen = 7;
static long held;
static int fails;
static const struct fpcd_hold_st H = { &mx, &stop, &cancel, &gen, &held };

static long ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	fails += !ok;
}

static void mx_free(const char *when)
{
	int r = pthread_mutex_trylock(&mx);
	if (!r)
		pthread_mutex_unlock(&mx);
	char b[96];
	snprintf(b, sizeof(b), "state mutex free %s", when);
	check(r == 0, b);
}

static long bridge_lock_ms;
static void *bridge_cancel(void *arg)	/* h_cancel: lock, read, unlock; op_set_nowait: lock, gen++, unlock */
{
	usleep((useconds_t)(long)arg * 1000);
	long t = ms();
	pthread_mutex_lock(&mx);
	gen++;
	cancel = 0;
	pthread_mutex_unlock(&mx);
	bridge_lock_ms = ms() - t;
	return NULL;
}

static void *taker(void *arg)	/* run_match taking the image */
{
	usleep(30000);
	pthread_mutex_lock(&mx);
	held = 0;
	pthread_mutex_unlock(&mx);
	return arg;
}

int main(void)
{
	alarm(5);
	pthread_t t;
	long t0;
	int r;

	held = 1000, gen = 7;
	pthread_create(&t, NULL, bridge_cancel, (void *)0L);	/* Cancel right away */
	t0 = ms();
	r = fpcd_hold_wait(&H, 7, 1500);
	pthread_join(t, NULL);
	check(r == 1 && ms() - t0 < 200, "1 wake-hold + immediate Cancel: returns 1 within 200 ms");
	check(held == 1000, "1 image kept for the next op");
	check(bridge_lock_ms < 50, "1 bridge thread's lock not blocked by the hold");
	mx_free("after 1");

	held = 1000;
	r = fpcd_hold_wait(&H, 7, 1500);	/* gen is 8 now */
	check(r == 1, "2 op already moved on: returns 1 at once");
	mx_free("after 2");

	held = 1000;
	t0 = ms();
	r = fpcd_hold_wait(&H, gen, 100);
	check(r == 0 && ms() - t0 >= 100 && ms() - t0 < 400, "3 no claim: returns 0 after max_ms");
	check(held == 0, "3 image dropped");
	mx_free("after 3");

	held = 1000;
	pthread_create(&t, NULL, taker, NULL);
	r = fpcd_hold_wait(&H, gen, 1500);
	pthread_join(t, NULL);
	check(r == 2, "4 image taken: returns 2");
	mx_free("after 4");

	held = 1000;
	pthread_create(&t, NULL, bridge_cancel, (void *)20L);	/* Cancel during the hold */
	r = fpcd_hold_wait(&H, gen, 1500);
	pthread_join(t, NULL);
	check(r == 1 && held == 1000, "5 Cancel 20 ms into the hold: returns 1, image kept");
	mx_free("after 5");

	printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
