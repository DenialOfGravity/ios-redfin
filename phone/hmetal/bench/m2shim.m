/*
 * m2shim: the Metal dispatcher. It is the executable of the bundle the kernel's IOAccelerator names
 * (IOAcceleratorES MetalPluginName/MetalPluginClassName = SweetMetal2 / SweetMTL2Device, boot-arg sweetmtl=2,
 * xnu/patches/0116), so Metal opens /System/Library/Extensions/SweetMetal2.bundle in every process and asks this file's
 * SweetMTL2Device for the device. It picks, per process, one of two plain bundles:
 *
 *   hakios-metal  HakiosMetal.bundle's HakiosMetalDevice (hakios' CoreAnimation plug-in, bench/plugin.py's edits)
 *   SweetMetal2   the real SweetMetal2 (AdrenoMetal frontend -> metal2vulkan -> Turnip), dlopen of its Mach-O
 *
 * Layout (phone/hmetal/README.md "Dispatcher"), every path under /var/local/hmetal:
 *   lib/HakiosMetal.bundle, lib/SweetMetal2.bundle   the two drivers (before the switch: ca/HakiosMetal.bundle and
 *                                                    /var/local/SweetMetal2.bundle.pre-hmca, used when lib/ lacks them)
 *   env                                              the one env file (before the switch: ca/env); see shim_env()
 *   all-off                                          exists: every process on SweetMetal2 (backboardd still follows ca/bbd-hm)
 *   m2-procs                                         process names (getprogname, one a line, '#' comments) kept on SweetMetal2
 *   hm-only                                          exists: ONLY the names it lists go to hakios-metal (staged roll-out);
 *                                                    absent: every process does
 *   m2-fallback                                      names the crash fallback put on SweetMetal2 (cleared by editing it)
 *   ca/bbd-hm                                        backboardd: on hakios-metal while it exists (its own trip: bbd-hm.tripped)
 *   log/NAME.log                                     each process's lines (the shim's and the plug-in's), when it may write
 *                                                    there; else DARWIN_USER_TEMP_DIR/hakios-metal-NAME.log (sandboxed ones)
 *   cache/                                           backboardd's compiled stages (writable for it), read-only for the rest,
 *                                                    whose own writable cache is DARWIN_USER_CACHE_DIR/hakios-metal-cache
 *
 * Crash fallback, per process: each start on hakios-metal is recorded (DARWIN_USER_TEMP_DIR/hakios-metal-NAME.starts:
 * "time pid"; a clean exit adds "x pid"); the third start within 120 s whose earlier ones did not exit cleanly puts the
 * process on SweetMetal2 for good: its name appended to m2-fallback (when writable) and a marker
 * DARWIN_USER_TEMP_DIR/hakios-metal-NAME.m2 (always writable), both read at every start. Clear: edit m2-fallback, rm the
 * marker. backboardd keeps its own (H3) trip: bbd-hm -> bbd-hm.tripped.
 *
 * XR10 off everywhere: HM_XR10=0 in every hakios-metal process (supportsExtendedXR10Formats NO: CoreAnimation makes BGRA8
 * layers and tiles, which another process samples with no decode); HM_XR10_ALLOW=1 in a process's env lets its HM_XR10 stand.
 * Manual reference counting, as the plug-ins.
 */
#ifndef SHIM_HOST_TEST     /* bench/m2shim_test.c includes this file for the choice's logic, without Objective-C */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#define HM_ENV      HM_DIR "/env"
#define HM_ENV0     HM_DIR "/ca/env"                                   /* before the switch */
#define HM_ALL_OFF  HM_DIR "/all-off"
#define HM_M2_PROCS HM_DIR "/m2-procs"
#define HM_ONLY     HM_DIR "/hm-only"
#define HM_FALLBACK HM_DIR "/m2-fallback"
#define HM_LOGDIR   HM_DIR "/log"
#define HM_CACHE    HM_DIR "/cache"
#define HM_SEED     HM_DIR "/cache-seed"
#define BBD_FLAG    HM_DIR "/ca/bbd-hm"
#define BBD_STARTS  HM_DIR "/ca/bbd-hm.starts"
#define BBD_LOG     HM_DIR "/ca/bbd-hm.log"
#define SHIM_LOG    HM_DIR "/ca/shim.log"

#define TRIP_STARTS 3
#define TRIP_WINDOW 120

static char g_name[64];
static char g_log[PATH_MAX];      /* this process's log ("" until chosen) */
static char g_tmp[PATH_MAX];      /* DARWIN_USER_TEMP_DIR, '/' at the end */

static void
shim_write(const char *path, const char *line)
{
	int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);

	if (fd >= 0) {
		(void)!write(fd, line, strlen(line));
		close(fd);
	}
}

/* a line into this process's log and the shared shim.log (each when it may be written) */
static void
shim_log(const char *fmt, ...)
{
	char line[600];
	va_list ap;
	int n = snprintf(line, sizeof(line), "m2shim[%d %s] %ld: ", getpid(), g_name, (long)time(NULL));

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	strlcat(line, "\n", sizeof(line));
	if (g_log[0]) shim_write(g_log, line);
	shim_write(SHIM_LOG, line);
}

static const char *
shim_first(const char *a, const char *b)
{
	return access(a, F_OK) == 0 ? a : b;
}

/* NAME on a line of PATH (a list: one name a line, blanks and '#' lines skipped; '*' = every process) */
static bool
shim_listed(const char *path, const char *name)
{
	FILE *f = fopen(path, "r");
	char line[256];
	bool hit = false;

	while (f != NULL && !hit && fgets(line, sizeof(line), f) != NULL) {
		char *s = line + strspn(line, " \t");
		s[strcspn(s, " \t\r\n#")] = '\0';
		hit = s[0] != '\0' && (strcmp(s, name) == 0 || strcmp(s, "*") == 0);
	}
	if (f != NULL) fclose(f);
	return hit;
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

/* (H3) backboardd: the third hakios-metal start within 120 s renames the flag */
static bool
shim_bbd_tripped(void)
{
	long now = (long)time(NULL), t, recent = 0;
	FILE *f = fopen(BBD_STARTS, "r");

	while (f != NULL && fscanf(f, "%ld", &t) == 1) {
		if (now - t < TRIP_WINDOW) recent++;
	}
	if (f != NULL) fclose(f);
	if (recent >= TRIP_STARTS - 1) {
		rename(BBD_FLAG, BBD_FLAG ".tripped");
		shim_log("tripped: %ld hakios-metal starts in %d s before this one; flag -> bbd-hm.tripped, SweetMetal2", recent, TRIP_WINDOW);
		return true;
	}
	if ((f = fopen(BBD_STARTS, "a")) != NULL) {
		fprintf(f, "%ld\n", now);
		fclose(f);
	}
	return false;
}

/* (hmapp) every other process: the starts file in its own temp directory */
static char g_starts[PATH_MAX];

static void
shim_clean_exit(void)
{
	FILE *f = fopen(g_starts, "a");

	if (f != NULL) {
		fprintf(f, "x %d\n", getpid());
		fclose(f);
	}
}

static bool
shim_proc_tripped(void)
{
	char marker[PATH_MAX];
	long now = (long)time(NULL), unclean = 0;
	struct { long t; int pid; bool clean; } seen[64];
	int count = 0;
	char line[64];
	FILE *f;

	snprintf(marker, sizeof(marker), "%shakios-metal-%s.m2", g_tmp, g_name);
	if (access(marker, F_OK) == 0 || shim_listed(HM_FALLBACK, g_name)) {
		shim_log("on the crash fallback list (m2-fallback or %s): SweetMetal2", marker);
		return true;
	}
	snprintf(g_starts, sizeof(g_starts), "%shakios-metal-%s.starts", g_tmp, g_name);
	if ((f = fopen(g_starts, "r")) != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			long t;
			int pid;
			if (sscanf(line, "x %d", &pid) == 1) {
				for (int i = 0; i < count; i++) if (seen[i].pid == pid) seen[i].clean = true;
			} else if (sscanf(line, "%ld %d", &t, &pid) == 2) {
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
		FILE *m = fopen(marker, "w");
		if (m != NULL) fclose(m);
		if ((m = fopen(HM_FALLBACK, "a")) != NULL) {
			fprintf(m, "%s\n", g_name);
			fclose(m);
		}
		shim_log("tripped: %ld hakios-metal starts in %d s without a clean exit before this one: SweetMetal2 from now on "
			 "(m2-fallback%s, marker %s)", unclean, TRIP_WINDOW, access(HM_FALLBACK, W_OK) == 0 ? "" : " not writable", marker);
		return true;
	}
	/* a short file: rewrite it when it grows */
	if ((f = fopen(g_starts, count > 48 ? "w" : "a")) != NULL) {
		if (count > 48) {
			for (int i = count - 16; i < count; i++) {
				fprintf(f, "%ld %d\n", seen[i].t, seen[i].pid);
				if (seen[i].clean) fprintf(f, "x %d\n", seen[i].pid);
			}
		}
		fprintf(f, "%ld %d\n", now, getpid());
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

	if (bbd) {
		strlcpy(g_log, BBD_LOG, sizeof(g_log));
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
	if (bbd) {
		*why = "backboardd: ca/bbd-hm";
		return access(BBD_FLAG, F_OK) == 0 && !shim_bbd_tripped();
	}
	if (access(HM_ALL_OFF, F_OK) == 0) { *why = "all-off"; return false; }
	if (shim_listed(HM_M2_PROCS, g_name)) { *why = "m2-procs"; return false; }
	if (access(HM_ONLY, F_OK) == 0 && !shim_listed(HM_ONLY, g_name)) { *why = "not in hm-only"; return false; }
	if (shim_proc_tripped()) { *why = "crash fallback"; return false; }
	*why = "default";
	return true;
}

/* the start of each process's choice (also the host test's entry) */
static void
shim_begin(const char *name)
{
	strlcpy(g_name, name, sizeof(g_name));
	shim_user_dir(_CS_DARWIN_USER_TEMP_DIR, g_tmp, sizeof(g_tmp), "/private/var/tmp/");
	shim_choose_log(strcmp(g_name, "backboardd") == 0);
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
	shim_cache();
}

#ifndef SHIM_HOST_TEST
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

	shim_begin(getprogname() ?: "?");
	bbd = strcmp(g_name, "backboardd") == 0;
	/* (self not released: before -init, _MTLDevice's -dealloc reads what -init never set; both plug-ins do the same) */
	if (shim_wants_hm(bbd, &why)) {
		shim_prepare_hm(bbd);
		NSBundle *b = [NSBundle bundleWithPath:@(shim_first(HM_BUNDLE, HM_BUNDLE0))];
		NSError *e = nil;
		if (b != nil && [b loadAndReturnError:&e]) {
			Class cls = NSClassFromString(@"HakiosMetalDevice");
			device = cls != Nil ? ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], @selector(initWithAcceleratorPort:), port) : nil;
			shim_log("hakios-metal (%s): class %s, device %s, log %s, cache %s", why, cls != Nil ? "found" : "MISSING",
				 device != nil ? [[[device name] description] UTF8String] : "nil", g_log[0] ? g_log : "none",
				 getenv("HAKIOS_METAL_CACHE") ?: "?");
		} else {
			shim_log("hakios-metal: bundle not loaded: %s", e != nil ? [[e description] UTF8String] : "?");
		}
		if (device != nil) {
			return device;
		}
		shim_log("hakios-metal gave no device: SweetMetal2 instead");
	}
	const char *real = shim_first(M2_REAL, M2_REAL0);
	void *h = dlopen(real, RTLD_NOW);
	Class cls = h != NULL ? (Class)dlsym(h, "OBJC_CLASS_$_SweetMTL2Device") : Nil;
	if (cls == Nil) {
		shim_log("SweetMetal2 not loaded from %s: %s", real, dlerror() ?: "no class symbol");
		return nil;
	}
	device = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], @selector(initWithAcceleratorPort:), port);
	if (bbd || strcmp(why, "default") != 0) {
		shim_log("SweetMetal2 (%s): device %s", why, device != nil ? "yes" : "nil");
	}
	return device;
}
@end
#endif
