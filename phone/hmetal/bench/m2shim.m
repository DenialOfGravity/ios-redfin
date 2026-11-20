/*
 * m2shim: the Metal dispatcher. It is the executable of the bundle the kernel's IOAccelerator names
 * (IOAcceleratorES MetalPluginName/MetalPluginClassName = SweetMetal2 / SweetMTL2Device, boot-arg sweetmtl=2,
 * xnu/patches/0116), so Metal opens /System/Library/Extensions/SweetMetal2.bundle in every process and asks this file's
 * SweetMTL2Device for the device. It picks, per process, one of two plain bundles:
 *
 *   hakios-metal  HakiosMetal.bundle's HakiosMetalDevice (hakios' CoreAnimation plug-in, bench/plugin.py's edits)
 *   SweetMetal2   the real SweetMetal2 (AdrenoMetal frontend -> metal2vulkan -> Turnip), dlopen of its Mach-O
 *
 * (hm-int2) hakios-metal is the default for EVERY process, backboardd included. SweetMetal2 only for:
 *   hm-off            exists: every process (backboardd too) on SweetMetal2 -- the operator's global off switch
 *   m2-procs          process names (getprogname, one a line, '#' comments, '*' = all) kept on SweetMetal2
 *   the crash trip    3 starts on hakios-metal within 120 s without a clean exit, crashes in Metal code only (below)
 *   no device         hakios-metal's bundle did not load or gave no device
 * Gone: hm-only (the staged list), all-off, and backboardd's ca/bbd-hm flag (both ignored, a line says so).
 *
 * Layout (phone/hmetal/README.md "Dispatcher"), every path under /var/local/hmetal:
 *   lib/HakiosMetal.bundle, lib/SweetMetal2.bundle   the two drivers (before the switch: ca/HakiosMetal.bundle and
 *                                                    /var/local/SweetMetal2.bundle.pre-hmca, used when lib/ lacks them)
 *   env                                              the one env file (before the switch: ca/env); see shim_env()
 *   m2-procs, hm-off                                 above
 *   luma-boost                                       "1": CoreAnimation's luma boost pass on (both drivers); else off
 *   m2-fallback                                      "NAME KEY" lines the crash trip wrote (KEY: the bundle's, below)
 *   log/NAME.log                                     each process's lines (the shim's and the plug-in's), when it may write
 *                                                    there; else DARWIN_USER_TEMP_DIR/hakios-metal-NAME.log (sandboxed ones)
 *   ca/bbd-hm.log, ca/shim.log                       backboardd's log; every process's dispatcher lines
 *   cache/                                           backboardd's compiled stages (writable for it), read-only for the rest,
 *                                                    whose own writable cache is DARWIN_USER_CACHE_DIR/hakios-metal-cache
 *
 * Crash trip, one mechanism for every process (backboardd's files are in ca/, everyone else's in its
 * DARWIN_USER_TEMP_DIR): each start on hakios-metal adds "time pid KEY" to the starts file (backboardd: ca/bbd-hm.starts,
 * others: hakios-metal-NAME.starts), a clean exit "x pid". The third start within 120 s whose two earlier ones (same KEY)
 * did not exit cleanly trips: a marker (ca/bbd-hm.tripped / hakios-metal-NAME.m2) holding KEY, "NAME KEY" appended to
 * m2-fallback, and ONE line "m2shim: TRIP ..." to /dev/console (the kernel log), syslog and shim.log.
 * KEY = the installed HakiosMetal executable's mtime and size: a trip holds only while the same bundle is installed;
 * a new bundle expires it (one line "m2shim: trip expired ..."). Clear: `pa fallback clear NAME` (marker, m2-fallback line,
 * a "c TIME" line in the starts file, then one restart on purpose).
 * (ws-hm-fallback) Restarts on purpose do not count: `sweetctl kill NAME` (and render/respring) writes intent/NAME
 * ("TIME PID SIG" lines; backboardd and SpringBoard also intent/_ui) before it signals; the killed instance's start is marked
 * "i PID" in the starts file. Every fallback explains itself: "HM FALLBACK: NAME -> SweetMetal2: REASON" (kernel log,
 * syslog, shim.log), a line in fallback.log, and for a trip MARKER.why + trips/NAME.why (time, the last starts and how
 * each ended, crash reports, bundle key). trip-test (exact names) trips a listed process at its next start ("test").
 * (ws-hm-tripfilter) Only crashes that may be Metal's count: before a start trips, each counted unclean exit's crash
 * report (CRASH_DIR/NAME-*.ips since that start, body "pid" = its pid) is read with NSJSONSerialization; when neither
 * the crashing thread nor lastExceptionBacktrace has a Metal/GPU image (HakiosMetal, SweetMetal2, Metal*, MTL, AGX, IOGPU,
 * IOAccelerator, MPS*, GPUCompiler, libhm*, Mesa/Turnip, QuartzCore, anything under the hm/SweetMetal2 bundles) it is
 * marked "n PID" and not counted, with one line "NAME pid PID crash not counted: no Metal code on the crashing thread
 * (REPORT, top: FRAME)" in fallback.log and the kernel log. No report (a hang, a watchdog, SIGKILL; waited for up to 3 s):
 * counted. HM_DIR/trip-any exists: every unclean exit counts (the rule before).
 *
 * Set in every hakios-metal process before the plug-in loads (each an env key, so a line in env flips it):
 *   HM_CA_DEEP_FB=0 (CAEnableDeepFramebuffer=false), HM_CA_FB_COMPRESSION=0 (CAEnableFramebufferCompression=false and
 *   CA_DISABLE_FRAMEBUFFER_COMPRESSION=1), HM_CA_IOSURFACE_COMPRESSION=0 (CAEnableIOSurfaceCompression=false): as
 *   SweetMetal2 sets them in its processes, so CoreAnimation makes no 550/551 deep or compressed surfaces. 1: CA left alone.
 *   XR10 off: HM_XR10=0 (supportsExtendedXR10Formats NO); HM_XR10_ALLOW=1 in a process's env lets its HM_XR10 stand.
 *   HM_TRACE: off unless the env file says HM_TRACE=1 (the plug-in rotates its log at 16 MB either way).
 * Luma boost (backboardd draws it; one knob for both drivers): luma-boost "1" = on, else off. backboardd applies it as
 * CASetDebugOption(91 = CA_DISABLE_LUMA_BOOST) at start and whenever the file changes (looked at every 2 s);
 * hakios-metal reads m2shim_luma_boost (its address in HM_LUMA_KNOB) to draw or leave out the pass; SweetMetal2 gets
 * SWEET_LUMA_BOOST.
 * Manual reference counting, as the plug-ins.
 */
#ifndef SHIM_HOST_TEST     /* bench/m2shim_test.c includes this file for the choice's logic, without Objective-C */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/message.h>
#include <dispatch/dispatch.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef HM_DIR
#define HM_DIR      "/var/local/hmetal"
#endif
#define HM_BUNDLE   HM_DIR "/lib/HakiosMetal.bundle"
#define HM_BUNDLE0  HM_DIR "/ca/HakiosMetal.bundle"                    /* before the switch */
#define M2_REAL     HM_DIR "/lib/SweetMetal2.bundle/SweetMetal2"
#ifndef M2_REAL0
#define M2_REAL0    "/var/local/SweetMetal2.bundle.pre-hmca/SweetMetal2"
#endif /* before the switch */
#ifndef M2_CLIENTS
#define M2_CLIENTS  "/var/local/mtl2-clients"                         /* SweetMetal2's own per-process gate */
#endif
#define HM_ENV      HM_DIR "/env"
#define HM_ENV0     HM_DIR "/ca/env"                                   /* before the switch */
#define HM_OFF      HM_DIR "/hm-off"
#define HM_M2_PROCS HM_DIR "/m2-procs"
#define HM_ONLY_OLD HM_DIR "/hm-only"                                  /* (gone: ignored) */
#define HM_ALL_OFF_OLD HM_DIR "/all-off"                               /* (gone: ignored) */
#define HM_FALLBACK HM_DIR "/m2-fallback"
#define HM_LUMA     HM_DIR "/luma-boost"
#define HM_LOGDIR   HM_DIR "/log"
#define HM_CACHE    HM_DIR "/cache"
#define HM_SEED     HM_DIR "/cache-seed"
#define BBD_STARTS  HM_DIR "/ca/bbd-hm.starts"
#define BBD_MARKER  HM_DIR "/ca/bbd-hm.tripped"
#define BBD_LOG     HM_DIR "/ca/bbd-hm.log"
#define SHIM_LOG    HM_DIR "/ca/shim.log"

#define TRIP_STARTS 3
#define TRIP_WINDOW 120
#define LOG_ROTATE  (16L * 1024 * 1024)
#define HM_INTENT   HM_DIR "/intent"        /* (ws-hm-fallback) sweetctl kill's notes: a restart on purpose */
#define HM_TRIPS    HM_DIR "/trips"         /* NAME.why for every trip */
#define HM_FBLOG    HM_DIR "/fallback.log"  /* one line for every fallback to SweetMetal2 */
#define HM_TRIP_TEST HM_DIR "/trip-test"    /* names: trip at the next start (reason "test") */
#define INTENT_WINDOW 30
#ifndef CRASH_DIR
#define CRASH_DIR   "/private/var/mobile/Library/Logs/CrashReporter"
#endif

static char g_name[64];
static char g_log[PATH_MAX];      /* this process's log ("" until chosen) */
static char g_tmp[PATH_MAX];      /* DARWIN_USER_TEMP_DIR, '/' at the end */
static char g_key[64];            /* the installed HakiosMetal executable: "mMTIME-sSIZE" ("none": not there) */
static char g_starts[PATH_MAX];   /* this process's starts file (once it started on hakios-metal) */

/* (hm-int2) the luma boost knob, read by the hakios-metal plug-in (address in HM_LUMA_KNOB): 1 drawn, 0 left out */
__attribute__((visibility("default"))) volatile int m2shim_luma_boost = 0;

static void
shim_write(const char *path, const char *line)
{
	int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);

	if (fd >= 0) {
		(void)!write(fd, line, strlen(line));
		close(fd);
	}
}

/* a log past 16 MB -> PATH.1 (one generation kept) */
static void
shim_rotate(const char *path)
{
	struct stat st;
	char old[PATH_MAX + 4];

	if (path[0] != '\0' && stat(path, &st) == 0 && st.st_size > LOG_ROTATE) {
		snprintf(old, sizeof(old), "%s.1", path);
		rename(path, old);
	}
}

/* a line into this process's log and the shared shim.log (each when it may be written) */
static void
shim_log(const char *fmt, ...)
{
	char line[700];
	va_list ap;
	int n = snprintf(line, sizeof(line), "m2shim[%d %s] %ld: ", getpid(), g_name, (long)time(NULL));

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	strlcat(line, "\n", sizeof(line));
	if (g_log[0]) shim_write(g_log, line);
	shim_write(SHIM_LOG, line);
}

/* a line that has to be seen: the kernel log (/dev/console, as SweetMetal2 and venusd write theirs), syslog, and
 * shim_log's places. Sandboxed processes may not open /dev/console; syslog still has it. */
static void
shim_shout(const char *fmt, ...)
{
	char text[600], line[700];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	shim_log("%s", text);
	snprintf(line, sizeof(line), "m2shim[%d %s]: %s", getpid(), g_name, text);
#ifndef SHIM_HOST_TEST
	syslog(LOG_ERR, "%s", line);
	strlcat(line, "\n", sizeof(line));
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK)) >= 0) {
		(void)!write(fd, line, strlen(line));
		close(fd);
	}
#else
	(void)fd;
#endif
}

static const char *
shim_first(const char *a, const char *b)
{
	return access(a, F_OK) == 0 ? a : b;
}

/* NAME on a line of PATH (a list: one name a line, blanks and '#' lines skipped; '*' = every process). KEY non-NULL:
 * the line's second word has to be KEY too (m2-fallback's "NAME KEY" lines; a line without one counts for nothing) */
static bool
shim_listed_key(const char *path, const char *name, const char *key)
{
	FILE *f = fopen(path, "r");
	char line[256];
	bool hit = false;

	while (f != NULL && !hit && fgets(line, sizeof(line), f) != NULL) {
		char *s = line + strspn(line, " \t"), *k;
		s[strcspn(s, "\r\n#")] = '\0';
		k = s + strcspn(s, " \t");
		if (*k != '\0') { *k++ = '\0'; k += strspn(k, " \t"); k[strcspn(k, " \t")] = '\0'; }
		hit = s[0] != '\0' && (strcmp(s, name) == 0 || strcmp(s, "*") == 0) && (key == NULL || strcmp(k, key) == 0);
	}
	if (f != NULL) fclose(f);
	return hit;
}

static bool
shim_listed(const char *path, const char *name)
{
	return shim_listed_key(path, name, NULL);
}

/* SweetMetal2's own gate (SweetMetal2.mm listed()): backboardd, or named / '*' in mtl2-clients and not "-NAME" */
static bool
shim_m2_clients_has(const char *name)
{
	FILE *f;
	char text[2048] = { 0 }, *save = NULL;
	bool yes = false;
	size_t n;

	if (strcmp(name, "backboardd") == 0) return true;
	if ((f = fopen(M2_CLIENTS, "r")) == NULL) return false;
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	for (char *w = strtok_r(text, " ,\t\r\n", &save); w != NULL; w = strtok_r(NULL, " ,\t\r\n", &save)) {
		if (w[0] == '-' && strcmp(w + 1, name) == 0) return false;
		if (strcmp(w, "*") == 0 || strcmp(w, name) == 0) yes = true;
	}
	return yes;
}

static bool
shim_dir_writable(const char *dir)
{
	return access(dir, W_OK | X_OK) == 0;
}

static void
shim_user_dir(int which, char *out, size_t size, const char *fallback)
{
	size_t n = confstr(which, out, size);

	if (n == 0 || n > size || out[0] == '\0') strlcpy(out, fallback, size);
	if (out[strlen(out) - 1] != '/') strlcat(out, "/", size);
}

/* (hm-int2b) this process's code-signing status (csops CS_OPS_STATUS); CS_VALID = bit 0. A page whose hash does not
 * match the signature the kernel holds for its file clears CS_VALID ("cs_invalid_page ... allowing (remove VALID)");
 * tccd then no longer trusts the process's entitlements and a TCC request aborts it (SpringBoard, kTCCServiceMotion,
 * 2026-11-14). The usual cause: a driver file overwritten IN PLACE while processes had it mapped (the vnode keeps the
 * old signature): install by copy to NAME.new + mv (bench/hm-install.sh), or reboot. */
#ifndef SHIM_HOST_TEST
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
static uint32_t
shim_cs_status(void)
{
	uint32_t st = 0;

	return csops(getpid(), 0 /* CS_OPS_STATUS */, &st, sizeof(st)) == 0 ? st : 0xffffffffu;
}

/* one line when loading WHAT took CS_VALID away (BEFORE: the status before it) */
static void
shim_cs_check(uint32_t before, const char *what)
{
	uint32_t now = shim_cs_status();

	if (before != 0xffffffffu && now != 0xffffffffu && (before & 1) && !(now & 1)) {
		shim_shout("CODE SIGNATURE lost CS_VALID loading %s (status %#x -> %#x): a page did not match the signature the kernel "
			   "holds for the file -- installed in place over a mapped file? Reinstall by copy + rename (bench/hm-install.sh) "
			   "or reboot; TCC requests of this process will now abort", what, before, now);
	}
}
#endif

/* the installed hakios-metal's key: its executable's mtime and size */
static void
shim_bundle_key(void)
{
	char exe[PATH_MAX];
	struct stat st;

	snprintf(exe, sizeof(exe), "%s/HakiosMetal", shim_first(HM_BUNDLE, HM_BUNDLE0));
	if (stat(exe, &st) == 0) {
		snprintf(g_key, sizeof(g_key), "m%lld-s%lld", (long long)st.st_mtime, (long long)st.st_size);
	} else {
		strlcpy(g_key, "none", sizeof(g_key));
	}
}

/* (hmapp) the env file: KEY=VALUE lines (keys HAKIOS_METAL_* and HM_* only) for backboardd, as before;
 * '*:KEY=VALUE' for every hakios-metal process, 'NAME:KEY=VALUE' for the process NAME. Applied in file order. */
static void
shim_env(bool bbd)
{
	FILE *f = fopen(shim_first(HM_ENV, HM_ENV0), "r");
	char line[256];

	if (f == NULL) return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *key = line, *colon, *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '\0' || line[0] == '#') continue;
		eq = strchr(line, '=');
		colon = strchr(line, ':');
		if (colon != NULL && (eq == NULL || colon < eq)) {
			*colon = '\0';
			if (strcmp(line, "*") != 0 && strcmp(line, g_name) != 0) continue;
			key = colon + 1;
		} else if (!bbd) {
			continue;     /* a plain line is backboardd's */
		}
		if (eq == NULL || (strncmp(key, "HAKIOS_METAL_", 13) != 0 && strncmp(key, "HM_", 3) != 0)) {
			shim_log("env: left out: %s", key);
			continue;
		}
		*eq = '\0';
		setenv(key, eq + 1, 1);
		shim_log("env: %s=%s", key, eq + 1);
	}
	fclose(f);
}

static void
shim_clean_exit(void)
{
	FILE *f = g_starts[0] ? fopen(g_starts, "a") : NULL;

	if (f != NULL) {
		fprintf(f, "x %d\n", getpid());
		fclose(f);
	}
}

/* (ws-hm-fallback) the starts file's lines: "TIME PID KEY" a start on hakios-metal; "x PID" its clean exit; "i PID" it
 * was killed on purpose (sweetctl kill, below); "c TIME" every start before was cleared (pa fallback clear). */
typedef struct { long t; int pid; char how; } shim_start;   /* how: 0 no clean exit seen, 'x', 'i', 'c', 'n' */

/* (ws-hm-fallback) an intentional restart: `sweetctl kill NAME` writes intent/NAME (NAME as p_comm: 16 characters at
 * most), one "TIME PID SIG" line for every process it signals, BEFORE it signals; a kill of backboardd (sweetctl
 * render) or SpringBoard (respring) also intent/_ui ("TIME"): the processes that go down with them (SpringBoard,
 * posters, apps). */
typedef struct { long t; int pids[16]; int n; } shim_intent;

static void
shim_intent_read(const char *name, shim_intent *in)
{
	char path[PATH_MAX], line[128];
	FILE *f;

	in->t = 0;
	in->n = 0;
	snprintf(path, sizeof(path), HM_INTENT "/%.16s", name);
	if ((f = fopen(path, "r")) == NULL) return;
	while (fgets(line, sizeof(line), f) != NULL) {
		long t;
		int pid, k = sscanf(line, "%ld %d", &t, &pid);
		if (k >= 1 && t > in->t) in->t = t;
		if (k == 2 && pid > 0 && in->n < 16) in->pids[in->n++] = pid;
	}
	fclose(f);
}

/* the start running at time T (the last one at or before it) was ended on purpose: 'i', when no exit was seen */
static int
shim_intent_mark(shim_start *seen, int count, long t)
{
	for (int i = count - 1; i >= 0; i--) {
		if (seen[i].t <= t + 1) {
			if (seen[i].how) return -1;
			seen[i].how = 'i';
			return i;
		}
	}
	return -1;
}

/* NAME on a line of PATH, exactly ('*' does not count): the trip-test list */
static bool
shim_listed_exact(const char *path, const char *name)
{
	return shim_listed_key(path, name, NULL) && !shim_listed_key(path, "\x01", NULL);   /* a '*' line matches any name */
}

static const char *
shim_how(char how)
{
	return how == 'x' ? "clean exit" : how == 'i' ? "killed on purpose" : how == 'c' ? "cleared" :
	       how == 'n' ? "crashed, not counted (no Metal code on the crashing thread)" : "NO clean exit";
}

static void
shim_utc(long t, char *out, size_t n)
{
	time_t tt = (time_t)t;
	struct tm tm;

	gmtime_r(&tt, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

/* (ws-hm-fallback) a fallback to SweetMetal2 explains itself: one "HM FALLBACK: NAME -> SweetMetal2: REASON" line
 * (kernel log, syslog, shim.log), one line in fallback.log, and for a trip (MARKER non-NULL) MARKER.why and
 * trips/NAME.why: the time, the process, the reason, the last starts, what is known of how they ended, the newest
 * crash reports, the hakios-metal bundle */
static void
shim_explain(const char *reason, const char *marker, const shim_start *seen, int count)
{
	char now_s[40], starts[900] = "", exits[900] = "", reports[400] = "", why[PATH_MAX + 8], line[2600];
	long now = (long)time(NULL), oldest = now;
	int first = count > 6 ? count - 6 : 0;
	FILE *f;
	DIR *d;

	shim_utc(now, now_s, sizeof(now_s));
	for (int i = first; i < count; i++) {
		char one[160];
		snprintf(one, sizeof(one), "%s%ld (%lds ago) pid %d: %s", starts[0] ? "; " : "", seen[i].t, now - seen[i].t,
			 seen[i].pid, shim_how(seen[i].how));
		strlcat(starts, one, sizeof(starts));
		if (!seen[i].how && now - seen[i].t < TRIP_WINDOW) {
			snprintf(one, sizeof(one), "%spid %d ended without a clean exit: a signal, a crash, a hang or a watchdog kill "
				 "(klog: CRASH %s[%d])", exits[0] ? "; " : "", seen[i].pid, g_name, seen[i].pid);
			strlcat(exits, one, sizeof(exits));
			if (seen[i].t < oldest) oldest = seen[i].t;
		}
	}
	if (count > 0) {
		char one[64];
		snprintf(one, sizeof(one), "; %ld pid %d: this start", now, getpid());
		strlcat(starts, one, sizeof(starts));
	}
	if ((d = opendir(CRASH_DIR)) != NULL) {
		struct dirent *e;
		size_t nl = strlen(g_name);
		int n = 0;
		while ((e = readdir(d)) != NULL && n < 3) {
			char p[PATH_MAX];
			struct stat st;
			if (strncmp(e->d_name, g_name, nl) != 0 || (e->d_name[nl] != '-' && e->d_name[nl] != '.')) continue;
			snprintf(p, sizeof(p), "%s/%s", CRASH_DIR, e->d_name);
			if (stat(p, &st) != 0 || (long)st.st_mtime < oldest - 5) continue;
			if (reports[0]) strlcat(reports, ", ", sizeof(reports));
			strlcat(reports, e->d_name, sizeof(reports));
			n++;
		}
		closedir(d);
	}
	snprintf(line, sizeof(line), "HM FALLBACK: %s -> SweetMetal2: %s", g_name, reason);
	shim_shout("%s", line);
	snprintf(line, sizeof(line), "%s %ld %s pid %d -> SweetMetal2: %s | starts: %s | hm bundle %s | marker %s\n", now_s, now,
		 g_name, getpid(), reason, starts[0] ? starts : "none", g_key, marker ? marker : "none (this start only)");
	shim_write(HM_FBLOG, line);
	if (marker == NULL) return;
	snprintf(line, sizeof(line),
		 "reason: %s\ntime: %ld (%s)\nprocess: %s pid %d\nstarts (last %d, oldest first): %s\nexits: %s\n"
		 "crash reports (%s, since the first counted start): %s\nhm bundle: %s (HakiosMetal mtime-size; pa fallback names "
		 "its build)\nmarker: %s\nclear: pa fallback clear %s (the owner allows clearing to return to the newest driver)\n",
		 reason, now, now_s, g_name, getpid(), count - first, starts[0] ? starts : "none",
		 exits[0] ? exits : "none counted", CRASH_DIR, reports[0] ? reports : "none found", g_key, marker, g_name);
	snprintf(why, sizeof(why), "%s.why", marker);
	if ((f = fopen(why, "w")) != NULL) { fputs(line, f); fclose(f); }
	mkdir(HM_TRIPS, 0777);
	snprintf(why, sizeof(why), HM_TRIPS "/%s.why", g_name);
	if ((f = fopen(why, "w")) != NULL) { fputs(line, f); fclose(f); }
}

/* (ws-hm-tripfilter) the crash trip counts only crashes that may be Metal's. Before a start would trip, each earlier
 * unclean start's crash report is looked for: CRASH_DIR/NAME-*.ips written since that start whose body's "pid" is its
 * pid. Found, and NEITHER the crashing thread's frames NOR lastExceptionBacktrace name a Metal/GPU image
 * (shim_metal_image): that exit is not counted ("n PID" in the starts file; one line "NAME pid PID crash not counted:
 * ..." in fallback.log and the kernel log). No report (a hang, a watchdog, SIGKILL, a report not written yet after
 * TRIP_REPORT_WAIT_MS): counted, as before. HM_DIR/trip-any exists: every unclean exit counts (the old rule). */
#define HM_TRIP_ANY HM_DIR "/trip-any"
#ifndef TRIP_REPORT_WAIT_MS
#define TRIP_REPORT_WAIT_MS 3000          /* how long a start that would trip waits for missing crash reports */
#endif

typedef struct {
	int pid;                  /* the body's "pid" (-1: none) */
	bool crash;               /* it has the crashing thread's frames or a lastExceptionBacktrace */
	bool metal;               /* a Metal/GPU image among them */
	char metal_img[160];      /* the first one, and where */
	char top_ft[160];         /* the first frame outside the plumbing (libsystem, libc++abi, libobjc, CoreFoundation, ...) */
	char top_leb[160];        /*   of the crashing thread / of lastExceptionBacktrace */
	char first[160];          /* the very first frame */
} shim_report;

/* an image that is (or drives) Metal: either driver (HakiosMetal, SweetMetal2 and everything under /var/local/hmetal or
 * the SweetMetal2 bundles), Apple's Metal stack (Metal, MetalTools, MetalKit, MetalPerformanceShaders*, MPS*, libMTL*,
 * AGX*, IOGPU, IOAccelerator, GPUCompiler*), Mesa/Turnip pieces, libhm*, and QuartzCore (CoreAnimation's renderer is
 * hakios-metal's main client). Matched in the image's name or path, case ignored. Conservative: a doubt counts. */
static bool
shim_metal_image(const char *name, const char *path)
{
	static const char *const any[] = { "metal", "mtl", "gpu", "agx", "ioaccel", "hakios", "adreno", "vulkan", "turnip",
					   "freedreno", "mesa", "quartzcore", NULL };
	static const char *const pfx[] = { "MPS", "libhm", NULL };
	for (int w = 0; w < 2; w++) {
		const char *s = w == 0 ? name : path;
		if (s == NULL || s[0] == '\0') continue;
		for (int i = 0; any[i]; i++) if (strcasestr(s, any[i]) != NULL) return true;
		if (w == 0) for (int i = 0; pfx[i]; i++) if (strncmp(s, pfx[i], strlen(pfx[i])) == 0) return true;
	}
	return false;
}

/* the plumbing every crash runs through (skipped when naming the top frame) */
static bool
shim_plumbing_image(const char *name)
{
	static const char *const p[] = { "libsystem_", "libc++abi", "libobjc", "CoreFoundation", "libdispatch", "libdyld",
					 "dyld", "libc++.", NULL };
	for (int i = 0; p[i]; i++) if (strncmp(name, p[i], strlen(p[i])) == 0) return true;
	return false;
}

/* one frame of the crashing thread (LEB false) or of lastExceptionBacktrace (true), in order */
static void
shim_report_frame(shim_report *r, const char *name, const char *path, const char *symbol, bool leb)
{
	char one[160], *top = leb ? r->top_leb : r->top_ft;

	snprintf(one, sizeof(one), "%s %s", name && name[0] ? name : path && path[0] ? path : "?", symbol ? symbol : "?");
	if (r->first[0] == '\0') strlcpy(r->first, one, sizeof(r->first));
	if (!r->metal && shim_metal_image(name, path)) {
		r->metal = true;
		snprintf(r->metal_img, sizeof(r->metal_img), "%s (%s)", one, leb ? "lastExceptionBacktrace" : "crashing thread");
	}
	if (top[0] == '\0' && name && name[0] && !shim_plumbing_image(name)) strlcpy(top, one, 160);
}

/* PATH, an iOS .ips crash report (line 1 a JSON header, the rest a JSON body), into R. false: not readable as one */
#ifdef SHIM_HOST_TEST
static bool shim_report_read(const char *path, shim_report *r);      /* bench/m2shim_test.c's own JSON reader */
#else
static void
shim_report_frames(shim_report *r, id frames, NSArray *images, bool leb)
{
	if (![frames isKindOfClass:[NSArray class]]) return;
	for (id fr in (NSArray *)frames) {
		NSNumber *ix;
		NSDictionary *im = nil;
		id name = nil, ipath = nil, sym;
		if (![fr isKindOfClass:[NSDictionary class]]) continue;
		ix = ((NSDictionary *)fr)[@"imageIndex"];
		sym = ((NSDictionary *)fr)[@"symbol"];
		if ([ix isKindOfClass:[NSNumber class]] && ix.longLongValue >= 0 && ix.longLongValue < (long long)images.count)
			im = images[(NSUInteger)ix.longLongValue];
		if ([im isKindOfClass:[NSDictionary class]]) { name = im[@"name"]; ipath = im[@"path"]; }
		shim_report_frame(r, [name isKindOfClass:[NSString class]] ? [name UTF8String] : NULL,
				  [ipath isKindOfClass:[NSString class]] ? [ipath UTF8String] : NULL,
				  [sym isKindOfClass:[NSString class]] ? [sym UTF8String] : NULL, leb);
	}
}

static bool
shim_report_read(const char *path, shim_report *r)
{
	bool ok = false;

	@autoreleasepool {
		NSData *all = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
		const char *b = (const char *)all.bytes, *nl = b ? memchr(b, '\n', all.length) : NULL;
		if (nl != NULL) {
			NSUInteger off = (NSUInteger)(nl + 1 - b);
			id j = [NSJSONSerialization JSONObjectWithData:[all subdataWithRange:NSMakeRange(off, all.length - off)]
							       options:0 error:NULL];
			if ([j isKindOfClass:[NSDictionary class]]) {
				NSDictionary *d = j;
				id pid = d[@"pid"], images = d[@"usedImages"], threads = d[@"threads"], ft = d[@"faultingThread"];
				id leb = d[@"lastExceptionBacktrace"];
				if (![images isKindOfClass:[NSArray class]]) images = @[];
				r->pid = [pid isKindOfClass:[NSNumber class]] ? [pid intValue] : -1;
				if ([threads isKindOfClass:[NSArray class]] && [ft isKindOfClass:[NSNumber class]] && [ft longLongValue] >= 0 &&
				    [ft longLongValue] < (long long)[threads count]) {
					id th = threads[(NSUInteger)[ft longLongValue]];
					id frames = [th isKindOfClass:[NSDictionary class]] ? th[@"frames"] : nil;
					if ([frames isKindOfClass:[NSArray class]]) {
						r->crash = true;
						shim_report_frames(r, frames, images, false);
					}
				}
				if ([leb isKindOfClass:[NSArray class]]) {
					r->crash = true;
					shim_report_frames(r, leb, images, true);
				}
				ok = true;
			}
		}
	}
	return ok;
}
#endif

/* PID's crash report: a NAME-*.ips in CRASH_DIR written at or after SINCE whose body's pid is PID, with frames */
static bool
shim_report_find(int pid, long since, shim_report *r, char *file, size_t n)
{
	size_t nl = strlen(g_name);
	bool found = false;
	struct dirent *e;
	DIR *d;

	if ((d = opendir(CRASH_DIR)) == NULL) return false;
	while (!found && (e = readdir(d)) != NULL) {
		char p[PATH_MAX];
		struct stat st;
		if (strncmp(e->d_name, g_name, nl) != 0 || e->d_name[nl] != '-' || strstr(e->d_name, ".ips") == NULL) continue;
		snprintf(p, sizeof(p), "%s/%s", CRASH_DIR, e->d_name);
		if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || (long)st.st_mtime < since) continue;
		memset(r, 0, sizeof(*r));
		r->pid = -1;
		if (shim_report_read(p, r) && r->crash && r->pid == pid) {
			strlcpy(file, e->d_name, n);
			found = true;
		}
	}
	closedir(d);
	return found;
}

/* the filter: SEEN's unclean starts within the window whose crash report shows no Metal code are marked 'n' (and
 * "n PID" written to the starts file); returns how many. Runs only when this start would otherwise trip. */
static long
shim_trip_filter(shim_start *seen, int count, long now)
{
	bool done[64] = { false };
	long dropped = 0;

	for (int waited = 0;; waited += 250) {
		int missing = 0;
		for (int i = 0; i < count; i++) {
			shim_report r;
			char file[256], line[900], now_s[40];
			if (done[i] || seen[i].how || now - seen[i].t >= TRIP_WINDOW) continue;
			if (!shim_report_find(seen[i].pid, seen[i].t, &r, file, sizeof(file))) { missing++; continue; }
			done[i] = true;
			if (r.metal) {
				shim_log("pid %d crash counted: Metal code in it, %s (%s)", seen[i].pid, r.metal_img, file);
				continue;
			}
			seen[i].how = 'n';
			dropped++;
			snprintf(line, sizeof(line), "n %d\n", seen[i].pid);
			shim_write(g_starts, line);
			snprintf(line, sizeof(line), "%s pid %d crash not counted: no Metal code on the crashing thread (%s, top: %s)",
				 g_name, seen[i].pid, file, r.top_leb[0] ? r.top_leb : r.top_ft[0] ? r.top_ft : r.first[0] ? r.first : "?");
			shim_shout("%s", line);
			shim_utc(now, now_s, sizeof(now_s));
			{
				char fl[1000];
				snprintf(fl, sizeof(fl), "%s %ld %s | crash trip stays off (trip-any restores the old rule) | hm bundle %s\n",
					 now_s, now, line, g_key);
				shim_write(HM_FBLOG, fl);
			}
		}
		if (missing == 0 || waited >= TRIP_REPORT_WAIT_MS) {
			if (missing) shim_log("%d unclean exit(s) without a crash report after %d ms: counted", missing, waited);
			break;
		}
		usleep(250000);
	}
	return dropped;
}

/* (hm-int2) the crash trip, one for every process (backboardd's files in ca/, the others' in their temp directory).
 * true: this process stays on SweetMetal2 (tripped now, or earlier with the bundle installed now).
 * (ws-hm-fallback) Only abnormal ends count: a start whose process sweetctl killed on purpose (its pid in intent/NAME,
 * or the start that was running when intent/NAME or intent/_ui was written, for a start within INTENT_WINDOW s of
 * it) is marked 'i' and not counted. trip-test (exact names) trips the listed process at its next start (reason
 * "test"); pa fallback test/clear write and remove it. (ws-hm-tripfilter) A crash whose report shows no Metal code is
 * marked 'n' and not counted (shim_trip_filter; trip-any turns that off). */
static bool
shim_tripped(bool bbd)
{
	char marker[PATH_MAX], held[64] = "", reason[300];
	long now = (long)time(NULL), unclean = 0;
	shim_start seen[64];
	shim_intent own, rend;
	int count = 0;
	char line[160];
	FILE *f;

	if (bbd) {
		strlcpy(marker, BBD_MARKER, sizeof(marker));
		strlcpy(g_starts, BBD_STARTS, sizeof(g_starts));
	} else {
		snprintf(marker, sizeof(marker), "%shakios-metal-%s.m2", g_tmp, g_name);
		snprintf(g_starts, sizeof(g_starts), "%shakios-metal-%s.starts", g_tmp, g_name);
	}
	/* a marker holds while its key is the installed bundle's; an older one (or one without a key) has expired */
	if ((f = fopen(marker, "r")) != NULL) {
		if (fgets(held, sizeof(held), f) == NULL) held[0] = '\0';
		fclose(f);
		held[strcspn(held, " \t\r\n")] = '\0';
		if (strcmp(held, g_key) == 0) {
			shim_log("crash trip holds (%s, bundle %s): SweetMetal2 (why: %s.why; pa fallback clear %s)", marker, g_key,
				 marker, g_name);
			return true;
		}
		unlink(marker);
		shim_shout("trip expired: hakios-metal bundle is now %s (tripped with %s): back on hakios-metal", g_key,
			   held[0] ? held : "no key");
	}
	if (shim_listed_key(HM_FALLBACK, g_name, g_key)) {
		shim_log("crash trip holds (m2-fallback \"%s %s\"): SweetMetal2", g_name, g_key);
		return true;
	}
	if ((f = fopen(g_starts, "r")) != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			long t;
			int pid;
			char k[64];
			if (sscanf(line, "x %d", &pid) == 1 || sscanf(line, "i %d", &pid) == 1 || sscanf(line, "n %d", &pid) == 1) {
				for (int i = 0; i < count; i++) if (seen[i].pid == pid) seen[i].how = line[0];
			} else if (sscanf(line, "c %ld", &t) == 1) {
				for (int i = 0; i < count; i++) if (!seen[i].how && seen[i].t <= t) seen[i].how = 'c';
			} else if (sscanf(line, "%ld %d %63s", &t, &pid, k) == 3 && strcmp(k, g_key) == 0) {
				if (count == 64) { memmove(seen, seen + 1, sizeof(seen[0]) * 63); count--; }
				seen[count].t = t; seen[count].pid = pid; seen[count].how = 0; count++;
			}
		}
		fclose(f);
	}
	/* intentional kills: by pid, or (a start within INTENT_WINDOW s of it) the start that was running then */
	shim_intent_read(g_name, &own);
	shim_intent_read("_ui", &rend);
	for (int i = 0; i < count; i++) {
		for (int j = 0; j < own.n && !seen[i].how; j++) {
			if (seen[i].pid == own.pids[j] && seen[i].t <= own.t + 1) {
				seen[i].how = 'i';
				snprintf(line, sizeof(line), "i %d\n", seen[i].pid);
				shim_write(g_starts, line);
			}
		}
	}
	for (int k = 0; k < 2; k++) {
		long t = k == 0 ? own.t : rend.t;
		int i;
		if (t == 0 || now - t > INTENT_WINDOW || now < t) continue;
		if ((i = shim_intent_mark(seen, count, t)) >= 0) {
			snprintf(line, sizeof(line), "i %d\n", seen[i].pid);
			shim_write(g_starts, line);
			shim_log("start of pid %d (%ld) ended on purpose (intent/%s %ld s ago): not counted", seen[i].pid, seen[i].t,
				 k == 0 ? g_name : "_ui", now - t);
		}
	}
	for (int i = 0; i < count; i++) {
		if (!seen[i].how && now - seen[i].t < TRIP_WINDOW) unclean++;
	}
	/* (ws-hm-tripfilter) about to trip: crashes without Metal code are not counted (trip-any: every one is) */
	if (unclean >= TRIP_STARTS - 1 && access(HM_TRIP_ANY, F_OK) != 0) unclean -= shim_trip_filter(seen, count, now);
	reason[0] = '\0';
	if (shim_listed_exact(HM_TRIP_TEST, g_name)) {
		snprintf(reason, sizeof(reason), "test (trip-test lists %s; pa fallback clear %s)", g_name, g_name);
	} else if (unclean >= TRIP_STARTS - 1) {
		snprintf(reason, sizeof(reason), "crash trip: start %ld on hakios-metal within %d s, the %ld before it without a "
			 "clean exit (not killed on purpose)", unclean + 1, TRIP_WINDOW, unclean);
	}
	if (reason[0]) {
		bool listed = false;
		if ((f = fopen(marker, "w")) != NULL) {
			fprintf(f, "%s\n", g_key);
			fclose(f);
		}
		if ((f = fopen(HM_FALLBACK, "a")) != NULL) {
			fprintf(f, "%s %s\n", g_name, g_key);
			fclose(f);
			listed = true;
		}
		shim_explain(reason, marker, seen, count);
		shim_shout("TRIP %s -> SweetMetal2: holds while bundle %s is installed (marker %s%s; why: %s.why)", g_name, g_key,
			   marker, listed ? ", m2-fallback" : ", m2-fallback not writable", marker);
		g_starts[0] = '\0';
		return true;
	}
	/* a short file: rewrite it when it grows (only this key's lines are kept) */
	if ((f = fopen(g_starts, count > 48 || count == 0 ? "w" : "a")) != NULL) {
		if (count > 48) {
			for (int i = count - 16; i < count; i++) {
				fprintf(f, "%ld %d %s\n", seen[i].t, seen[i].pid, g_key);
				if (seen[i].how == 'x' || seen[i].how == 'i' || seen[i].how == 'n') fprintf(f, "%c %d\n", seen[i].how, seen[i].pid);
			}
		}
		fprintf(f, "%ld %d %s\n", now, getpid(), g_key);
		fclose(f);
	}
	atexit(shim_clean_exit);
	return false;
}

/* where this process's lines go: log/NAME.log when it may append there, else its temp directory */
static void
shim_choose_log(bool bbd)
{
	char path[PATH_MAX];
	int fd;

	g_log[0] = '\0';
	if (bbd) {
		strlcpy(g_log, BBD_LOG, sizeof(g_log));
		shim_rotate(g_log);
		return;
	}
	snprintf(path, sizeof(path), HM_LOGDIR "/%s.log", g_name);
	if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666)) < 0) {
		snprintf(path, sizeof(path), "%shakios-metal-%s.log", g_tmp, g_name);
		fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
	}
	if (fd >= 0) {
		close(fd);
		strlcpy(g_log, path, sizeof(g_log));
		shim_rotate(g_log);
	}
}

/* the shader cache: backboardd (and whoever may write it) writes cache/; everyone else reads it (and cache-seed) and
 * writes its own DARWIN_USER_CACHE_DIR/hakios-metal-cache. The plug-in only fills what is not set. */
static void
shim_cache(void)
{
	char own[PATH_MAX], seed[2 * PATH_MAX];

	if (shim_dir_writable(HM_CACHE)) {
		setenv("HAKIOS_METAL_CACHE", HM_CACHE, 0);
		return;
	}
	shim_user_dir(_CS_DARWIN_USER_CACHE_DIR, own, sizeof(own), "/private/var/tmp/");
	strlcat(own, "hakios-metal-cache", sizeof(own));
	mkdir(own, 0700);
	setenv("HAKIOS_METAL_CACHE", shim_dir_writable(own) ? own : "", 0);     /* "": no writable cache, none tried */
	snprintf(seed, sizeof(seed), "%s:%s", HM_CACHE, HM_SEED);
	setenv("HAKIOS_METAL_CACHE_SEED", seed, 0);
}

/* the choice; *why says it */
static bool
shim_wants_hm(bool bbd, const char **why)
{
	if (access(HM_OFF, F_OK) == 0) { *why = "hm-off"; return false; }
	if (shim_listed(HM_M2_PROCS, g_name)) { *why = "m2-procs"; return false; }
	if (shim_tripped(bbd)) { *why = "crash trip"; return false; }
	*why = "default";
	return true;
}

/* the start of each process's choice (also the host test's entry) */
static void
shim_begin(const char *name)
{
	strlcpy(g_name, name, sizeof(g_name));
	g_starts[0] = '\0';
	shim_user_dir(_CS_DARWIN_USER_TEMP_DIR, g_tmp, sizeof(g_tmp), "/private/var/tmp/");
	shim_choose_log(strcmp(g_name, "backboardd") == 0);
	shim_bundle_key();
	if (strcmp(g_name, "backboardd") == 0) {
		shim_rotate(SHIM_LOG);
		if (access(HM_ONLY_OLD, F_OK) == 0) shim_log("hm-only is no longer read (every process on hakios-metal; m2-procs to keep one off)");
		if (access(HM_ALL_OFF_OLD, F_OK) == 0) shim_log("all-off is no longer read (hm-off: everything on SweetMetal2)");
	}
}

/* the luma knob: luma-boost holding "1" = on */
static int
shim_luma_file(void)
{
	FILE *f = fopen(HM_LUMA, "r");
	int c = f != NULL ? fgetc(f) : EOF;

	if (f != NULL) fclose(f);
	return c == '1';
}

/* (hm-int2) CA prefs as SweetMetal2 sets them in its processes; each HM_CA_* key 0 (default) = set, 1 = CA left alone.
 * Returns a bit per pref set (1 deep, 2 framebuffer compression, 4 IOSurface compression) */
static int
shim_ca_prefs(void)
{
	int set = 0;
	const char *e;

	if ((e = getenv("HM_CA_DEEP_FB")) == NULL || atoi(e) == 0) set |= 1;
	if ((e = getenv("HM_CA_FB_COMPRESSION")) == NULL || atoi(e) == 0) set |= 2;
	if ((e = getenv("HM_CA_IOSURFACE_COMPRESSION")) == NULL || atoi(e) == 0) set |= 4;
	if (set & 2) setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	/* (hm-int4) In the argument domain (volatile: this process only, never written to disk), not the app's own: the
	 * hm-int2/int3 CFPreferencesSetAppValue went through cfprefsd and was PERSISTED into each process's plist
	 * (/var/mobile/Library/Preferences/com.apple.backboardd.plist ...), so HM_CA_*=1 could not undo it. A value an
	 * earlier shim persisted is removed (CFPreferencesSetAppValue NULL + synchronize), whatever the switch says. */
	char did[320] = "";
#ifndef SHIM_HOST_TEST
	{
		static const char *const names[3] = {"CAEnableDeepFramebuffer", "CAEnableFramebufferCompression", "CAEnableIOSurfaceCompression"};
		NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
		NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
		NSMutableDictionary *args = [NSMutableDictionary dictionaryWithDictionary:[d volatileDomainForName:NSArgumentDomain] ?: @{}];
		int removed = 0;

		int kept = 0;
		for (int i = 0; i < 3; i++) {
			CFStringRef key = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
			CFPropertyListRef stale = CFPreferencesCopyValue(key, kCFPreferencesCurrentApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);

			/*
			 * (y1115) CAEnableDeepFramebuffer must be PERSISTED for backboardd. QuartzCore decides the display framebuffer
			 * format once, in AppleDisplay's constructor (CADeviceUseCIF10: MGGetBoolAnswer("Aixt/MEN2O2B7f+8m4TxUA"),
			 * true on D22, overridden by CFPreferencesGetAppBooleanValue("CAEnableDeepFramebuffer") when the key exists;
			 * IOMFBDisplay::framebuffer_pixel_format returns 'b3a8' (MTLPixelFormat 550, RGB10A8 2P XR10) instead of
			 * BGRA when it is true). That runs when backboardd creates its display, before CA loads the Metal plug-in
			 * and so before this shim: the argument-domain value comes too late. A launch drew in BGRA only when an
			 * earlier process had left the value persisted (int2/int3 shims, SweetMetal2), and this function then
			 * deleted it, so the NEXT launch made 'b3a8' framebuffers (black screen, "Unexpected framebuffer format
			 * b3a8"). Now: in backboardd, HM_CA_DEEP_FB=0 (default) persists false (read at the next launch, and kept);
			 * HM_CA_DEEP_FB=1 removes it (CA's own choice from the next launch).
			 */
			if (i == 0 && strcmp(g_name, "backboardd") == 0) {
				bool isfalse = stale != NULL && CFGetTypeID(stale) == CFBooleanGetTypeID() && !CFBooleanGetValue((CFBooleanRef)stale);
				if (set & 1) {
					if (!isfalse) {
						CFPreferencesSetAppValue(key, kCFBooleanFalse, kCFPreferencesCurrentApplication);
						removed |= 8;   /* persisted now */
					}
					kept = 1;
				} else if (stale != NULL) {
					CFPreferencesSetAppValue(key, NULL, kCFPreferencesCurrentApplication);
					removed |= 1;
				}
				if (stale != NULL) CFRelease(stale);
				if (set & 1) [args setObject:@NO forKey:(NSString *)key];
				else [args removeObjectForKey:(NSString *)key];
				CFRelease(key);
				continue;
			}
			if (stale != NULL) {   /* persisted by an earlier shim (or anyone): gone */
				CFRelease(stale);
				CFPreferencesSetAppValue(key, NULL, kCFPreferencesCurrentApplication);
				removed |= 1 << i;
			}
			if (set & (1 << i)) [args setObject:@NO forKey:(NSString *)key];
			else [args removeObjectForKey:(NSString *)key];
			CFRelease(key);
		}
		if (removed) CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
		[d setVolatileDomain:args forName:NSArgumentDomain];
		snprintf(did, sizeof(did), "; set in the argument domain (not persisted)%s%s%s%s%s%s",
			 (removed & 7) ? "; removed stale persisted value(s):" : "",
			 removed & 1 ? " deep" : "", removed & 2 ? " fbcomp" : "", removed & 4 ? " iosurfcomp" : "",
			 removed & 8 ? "; deep=false persisted for backboardd's next launch (y1115)" : "",
			 kept ? "; deep=false kept persisted (y1115)" : "");
		[pool release];
	}
#endif
	shim_log("CA prefs: CAEnableDeepFramebuffer=%s CAEnableFramebufferCompression=%s CAEnableIOSurfaceCompression=%s "
		 "(HM_CA_DEEP_FB / HM_CA_FB_COMPRESSION / HM_CA_IOSURFACE_COMPRESSION: 0 set false, 1 CA's own)%s",
		 set & 1 ? "false" : "CA's", set & 2 ? "false" : "CA's", set & 4 ? "false" : "CA's", did);
	return set;
}

/* the hakios-metal process's environment before the plug-in loads */
static void
shim_prepare_hm(bool bbd)
{
	if (g_log[0]) setenv("HAKIOS_METAL_LOG", g_log, 1);
	if (!bbd) {
		setenv("HAKIOS_METAL_ALL", "1", 1);   /* the plug-in's gate: not backboardd alone */
		setenv("HM_PREWARM", "0", 1);         /* backboardd fills the shared cache; others compile what they draw */
	}
	shim_env(bbd);
	if (getenv("HM_XR10_ALLOW") == NULL || atoi(getenv("HM_XR10_ALLOW")) == 0) {
		setenv("HM_XR10", "0", 1);            /* XR10 off everywhere: BGRA8 layers and tiles */
	}
	shim_ca_prefs();
	shim_cache();
	{   /* the luma knob's address for the plug-in (same process; this bundle is never unloaded) */
		char addr[32];
		snprintf(addr, sizeof(addr), "%p", (void *)&m2shim_luma_boost);
		setenv("HM_LUMA_KNOB", addr, 1);
	}
}

#ifndef SHIM_HOST_TEST
/* backboardd: CA_DISABLE_LUMA_BOOST as the knob says, now and at every change of luma-boost (looked at every 2 s) */
static void
shim_luma_apply(int on)
{
	static void (*set)(int, uint8_t);
	static int (*get)(int);
	static int applied = -1;

	m2shim_luma_boost = on;
	if (on == applied) return;
	applied = on;
	if (set == NULL) set = (void (*)(int, uint8_t))dlsym(RTLD_DEFAULT, "CASetDebugOption");
	if (get == NULL) get = (int (*)(int))dlsym(RTLD_DEFAULT, "CAGetDebugOption");
	if (set == NULL) {
		shim_log("luma: boost %s: NOT applied (no CASetDebugOption)", on ? "on" : "off");
		return;
	}
	/* off the render thread: CASetDebugOption posts a notification whose observers may take CA's locks */
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		set(91, (uint8_t)(on ? 0 : 1));
		shim_log("luma: boost %s (luma-boost %s): CASetDebugOption(91 = CA_DISABLE_LUMA_BOOST, %d), reads back %d",
			 on ? "on" : "off", on ? "1" : "absent/0", on ? 0 : 1, get != NULL ? get(91) : -1);
	});
}

static void
shim_luma_watch(void)
{
	static dispatch_source_t timer;

	shim_luma_apply(shim_luma_file());
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	if (timer == NULL) return;
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{ shim_luma_apply(shim_luma_file()); });
	dispatch_resume(timer);
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTL2Device : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTL2Device
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	id device = nil;
	const char *why = "";
	bool bbd;
	int luma;
	uint32_t cs0;

	shim_begin(getprogname() ?: "?");
	bbd = strcmp(g_name, "backboardd") == 0;
	cs0 = shim_cs_status();
	if (cs0 != 0xffffffffu && !(cs0 & 1)) {
		shim_shout("CODE SIGNATURE already without CS_VALID at Metal start (status %#x): this dispatcher or an image loaded "
			   "before it failed its page hashes -- installed in place over a mapped file? Reinstall by copy + rename "
			   "(bench/hm-install.sh) or reboot; TCC requests of this process will abort", cs0);
	}
	luma = shim_luma_file();
	m2shim_luma_boost = luma;
	/* (self not released: before -init, _MTLDevice's -dealloc reads what -init never set; both plug-ins do the same) */
	if (shim_wants_hm(bbd, &why)) {
		shim_prepare_hm(bbd);
		NSBundle *b = [NSBundle bundleWithPath:@(shim_first(HM_BUNDLE, HM_BUNDLE0))];
		NSError *e = nil;
		if (b != nil && [b loadAndReturnError:&e]) {
			Class cls = NSClassFromString(@"HakiosMetalDevice");
			device = cls != Nil ? ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], @selector(initWithAcceleratorPort:), port) : nil;
			shim_cs_check(cs0, shim_first(HM_BUNDLE, HM_BUNDLE0));
			shim_log("hakios-metal (%s): class %s, device %s, bundle %s, log %s, cache %s, HM_TRACE %s", why,
				 cls != Nil ? "found" : "MISSING", device != nil ? [[[device name] description] UTF8String] : "nil",
				 g_key, g_log[0] ? g_log : "none", getenv("HAKIOS_METAL_CACHE") ?: "?", getenv("HM_TRACE") ?: "off");
		} else {
			shim_log("hakios-metal: bundle not loaded: %s", e != nil ? [[e description] UTF8String] : "?");
		}
		if (device != nil) {
			if (bbd) shim_luma_watch();
			return device;
		}
		why = "hakios-metal gave no device";
		shim_explain("hakios-metal gave no device (this start only; the next start tries hakios-metal again; "
			     "see this process's log)", NULL, NULL, 0);
	}
	setenv("SWEET_LUMA_BOOST", luma ? "1" : "0", 1);   /* the knob for SweetMetal2 (a SWEET_LUMA_BOOST line in mtl2-env/-live wins) */
	const char *real = shim_first(M2_REAL, M2_REAL0);
	void *h = dlopen(real, RTLD_NOW);
	Class cls = h != NULL ? (Class)dlsym(h, "OBJC_CLASS_$_SweetMTL2Device") : Nil;
	if (cls == Nil) {
		shim_shout("SweetMetal2 not loaded from %s: %s", real, dlerror() ?: "no class symbol");
		return nil;
	}
	device = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], @selector(initWithAcceleratorPort:), port);
	shim_cs_check(cs0, real);
	shim_log("SweetMetal2 (%s): device %s%s", why, device != nil ? "yes" : "nil",
		 device == nil && !shim_m2_clients_has(g_name) ? " (not in " M2_CLIENTS ": SweetMetal2's gate gives none)" : "");
	if (bbd && device != nil) shim_luma_watch();
	return device;
}
@end
#endif
