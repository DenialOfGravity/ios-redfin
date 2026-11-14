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
 *   the crash trip    3 starts on hakios-metal within 120 s without a clean exit (below)
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
 * a new bundle expires it (one line "m2shim: trip expired ..."). Clear by hand: rm the marker and the m2-fallback line.
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

/* (hm-int2) the crash trip, one for every process (backboardd's files in ca/, the others' in their temp directory).
 * true: this process stays on SweetMetal2 (tripped now, or earlier with the bundle installed now) */
static bool
shim_tripped(bool bbd)
{
	char marker[PATH_MAX], held[64] = "";
	long now = (long)time(NULL), unclean = 0;
	struct { long t; int pid; bool clean; } seen[64];
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
			shim_log("crash trip holds (%s, bundle %s): SweetMetal2", marker, g_key);
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
			if (sscanf(line, "x %d", &pid) == 1) {
				for (int i = 0; i < count; i++) if (seen[i].pid == pid) seen[i].clean = true;
			} else if (sscanf(line, "%ld %d %63s", &t, &pid, k) == 3 && strcmp(k, g_key) == 0) {
				if (count == 64) { memmove(seen, seen + 1, sizeof(seen[0]) * 63); count--; }
				seen[count].t = t; seen[count].pid = pid; seen[count].clean = false; count++;
			}
		}
		fclose(f);
	}
	for (int i = 0; i < count; i++) {
		if (!seen[i].clean && now - seen[i].t < TRIP_WINDOW) unclean++;
	}
	if (unclean >= TRIP_STARTS - 1) {
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
		shim_shout("TRIP %s -> SweetMetal2: %ld starts on hakios-metal in %d s without a clean exit before this one; "
			   "holds while bundle %s is installed (marker %s%s)", g_name, unclean, TRIP_WINDOW, g_key, marker,
			   listed ? ", m2-fallback" : ", m2-fallback not writable");
		g_starts[0] = '\0';
		return true;
	}
	/* a short file: rewrite it when it grows (only this key's lines are kept) */
	if ((f = fopen(g_starts, count > 48 || count == 0 ? "w" : "a")) != NULL) {
		if (count > 48) {
			for (int i = count - 16; i < count; i++) {
				fprintf(f, "%ld %d %s\n", seen[i].t, seen[i].pid, g_key);
				if (seen[i].clean) fprintf(f, "x %d\n", seen[i].pid);
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
#ifndef SHIM_HOST_TEST
	if (set & 1) CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	if (set & 2) CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	if (set & 4) CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
#endif
	shim_log("CA prefs: CAEnableDeepFramebuffer=%s CAEnableFramebufferCompression=%s CAEnableIOSurfaceCompression=%s "
		 "(HM_CA_DEEP_FB / HM_CA_FB_COMPRESSION / HM_CA_IOSURFACE_COMPRESSION: 0 set false, 1 CA's own)",
		 set & 1 ? "false" : "CA's", set & 2 ? "false" : "CA's", set & 4 ? "false" : "CA's");
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

	shim_begin(getprogname() ?: "?");
	bbd = strcmp(g_name, "backboardd") == 0;
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
		shim_shout("hakios-metal gave no device: SweetMetal2 instead");
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
	shim_log("SweetMetal2 (%s): device %s%s", why, device != nil ? "yes" : "nil",
		 device == nil && !shim_m2_clients_has(g_name) ? " (not in " M2_CLIENTS ": SweetMetal2's gate gives none)" : "");
	if (bbd && device != nil) shim_luma_watch();
	return device;
}
@end
#endif
