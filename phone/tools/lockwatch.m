/*
 * lockwatch: what SpringBoard reports about the lock screen and the display, as CommCenter sees it.
 *
 * CommCenter (SubscriberUserAlertManager) queues the SIM PIN alert at boot and shows it only when
 *   SBSGetScreenLockStatus() == false (lock screen / cover sheet not up)  or  the device is passcode-locked.
 * It re-checks when com.apple.springboard.lockstate is posted (SystemObserver re-queries
 * SBSGetScreenLockStatus), and its "display on" comes from com.apple.springboard.hasBlankedScreen.
 * See claude/redfin-simpin-alert-gate-2026-11-18.md.
 *
 *   lockwatch                 print the state once
 *   lockwatch watch [SECS]    print the state, then one line per notification and per polled change (default 60 s,
 *                             0 = until killed). Poll period 0.5 s.
 *   lockwatch post NAME       notify_post(NAME) as root, then print the state (e.g. com.apple.springboard.lockstate
 *                             to make CommCenter re-check its pending SIM PIN alert)
 *
 * Line format: t=SECS sb_lock=0|1 passcode=0|1 mkb=N lockstate=N blanked=N sbstart=N kb=N [event=NAME]
 *   sb_lock   SBSGetScreenLockStatus() return: CommCenter's "coversheet active"
 *   passcode  its out-parameter (passcode enabled)
 *   mkb       MKBGetDeviceLockState(NULL): CommCenter's "display locked" (0 unlocked, 1 locked, 2 locking, 3 disabled)
 *   lockstate/blanked/sbstart/kb  notify state of com.apple.springboard.lockstate / .hasBlankedScreen /
 *             .finishedstartup / com.apple.mobile.keybagd.lock_status (-1 = could not register)
 * Read-only apart from `post`. No entitlements needed (CommCenter has none for these calls).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static BOOL (*pSBSGetScreenLockStatus)(BOOL *);
static int (*pMKBGetDeviceLockState)(CFDictionaryRef);
static double t0;
static char last[256];

static const char *kNames[] = {
	"com.apple.springboard.lockstate",
	"com.apple.springboard.hasBlankedScreen",
	"com.apple.springboard.finishedstartup",
	"com.apple.mobile.keybagd.lock_status",
	"com.apple.springboard.lockcomplete",
};
#define NNAMES (sizeof(kNames) / sizeof(kNames[0]))
static int tokens[NNAMES];

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long long nstate(int i)
{
	uint64_t s = 0;
	if (tokens[i] < 0 || notify_get_state(tokens[i], &s) != NOTIFY_STATUS_OK)
		return -1;
	return (long long)s;
}

/* returns a compact signature so the watcher can print only changes */
static void state(char *buf, size_t n)
{
	BOOL pass = NO;
	int lock = pSBSGetScreenLockStatus ? pSBSGetScreenLockStatus(&pass) : -1;
	int mkb = pMKBGetDeviceLockState ? pMKBGetDeviceLockState(NULL) : -1;
	snprintf(buf, n, "sb_lock=%d passcode=%d mkb=%d lockstate=%lld blanked=%lld sbstart=%lld kb=%lld", lock, pass ? 1 : 0,
		 mkb, nstate(0), nstate(1), nstate(2), nstate(3));
}

static void line(const char *event)
{
	char b[256];
	state(b, sizeof b);
	printf("t=%.2f %s%s%s\n", now() - t0, b, event ? " event=" : "", event ? event : "");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		t0 = now();
		void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
		void *mkb = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_NOW);
		pSBSGetScreenLockStatus = sbs ? dlsym(sbs, "SBSGetScreenLockStatus") : NULL;
		pMKBGetDeviceLockState = mkb ? dlsym(mkb, "MKBGetDeviceLockState") : NULL;
		if (!pSBSGetScreenLockStatus)
			fprintf(stderr, "no SBSGetScreenLockStatus (%s)\n", dlerror());
		for (unsigned i = 0; i < NNAMES; i++)
			if (notify_register_check(kNames[i], &tokens[i]) != NOTIFY_STATUS_OK)
				tokens[i] = -1;

		if (argc >= 3 && !strcmp(argv[1], "post")) {
			line("before");
			uint32_t r = notify_post(argv[2]);
			printf("notify_post(%s) = %u\n", argv[2], r);
			usleep(1000000);
			line("after+1s");
			return r ? 1 : 0;
		}
		if (argc >= 2 && strcmp(argv[1], "watch")) {
			fprintf(stderr, "usage: lockwatch [watch [SECS] | post NAME]\n");
			return 2;
		}
		line(NULL);
		if (argc < 2)
			return 0;

		double secs = argc >= 3 ? atof(argv[2]) : 60;
		dispatch_queue_t q = dispatch_queue_create("lockwatch", DISPATCH_QUEUE_SERIAL);
		for (unsigned i = 0; i < NNAMES; i++) {
			int tok;
			const char *nm = kNames[i];
			notify_register_dispatch(nm, &tok, q, ^(int t) {
				(void)t;
				line(nm);
			});
		}
		state(last, sizeof last);
		dispatch_source_t tm = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(tm, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), 500 * NSEC_PER_MSEC,
					  50 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(tm, ^{
			char cur[256];
			state(cur, sizeof cur);
			if (strcmp(cur, last)) {
				strcpy(last, cur);
				line("poll-change");
			}
			if (secs > 0 && now() - t0 >= secs) {
				line("end");
				exit(0);
			}
		});
		dispatch_resume(tm);
		dispatch_main();
	}
}
