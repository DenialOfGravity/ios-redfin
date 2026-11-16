/* m2shim_test.c: the dispatcher's choice (bench/m2shim.m) on the host, without Objective-C: lists, env lines, the
 * crash trip (one for every process, expiring with the bundle), CA prefs, the log and cache places. cc -Wno-format-truncation -o t m2shim_test.c && ./t */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
static size_t
strlcpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
	return l;
}
static size_t
strlcat(char *d, const char *s, size_t n)
{
	size_t l = strnlen(d, n);
	return l == n ? n + strlen(s) : l + strlcpy(d + l, s, n - l);
}
#endif

#define HM_DIR "/tmp/m2shim-test"
static const char t_root[] = HM_DIR;
static char t_tmp[256], t_cache[256];
#define M2_REAL0 "/nonexistent/SweetMetal2"
#define M2_CLIENTS HM_DIR "/mtl2-clients"
#define SHIM_HOST_TEST 1
#define _CS_DARWIN_USER_TEMP_DIR 0x10001
#define _CS_DARWIN_USER_CACHE_DIR 0x10002
static size_t
t_confstr(int which, char *out, size_t size)
{
	const char *v = which == _CS_DARWIN_USER_TEMP_DIR ? t_tmp : which == _CS_DARWIN_USER_CACHE_DIR ? t_cache : "";
	snprintf(out, size, "%s", v);
	return strlen(v) + 1;
}
#define confstr t_confstr
#include "m2shim.m"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void
put(const char *rel, const char *text)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", t_root, rel);
	FILE *f = fopen(p, "w");
	fputs(text, f);
	fclose(f);
}

static void
bbd_reset(void)
{
	(void)!unlink(HM_DIR "/ca/bbd-hm.starts");
}

static bool
wants(const char *name, const char **why)
{
	shim_begin(name);
	return shim_wants_hm(strcmp(name, "backboardd") == 0, why);
}

static void
bundle(const char *text)
{
	put("lib/HakiosMetal.bundle/HakiosMetal", text);
}

static void
first_line(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); }
	out[strcspn(out, "\n")] = 0;
}

int
main(void)
{
	const char *why;
	char p[512], q[512], key1[64], line[128];

	(void)!system("rm -rf " HM_DIR);
	snprintf(t_tmp, sizeof(t_tmp), "%s/tmp/", t_root);
	snprintf(t_cache, sizeof(t_cache), "%s/ucache/", t_root);
	mkdir(t_root, 0755);
	mkdir(t_tmp, 0755);
	mkdir(t_cache, 0755);
	snprintf(p, sizeof(p), "%s/ca", t_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/log", t_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/lib", t_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/lib/HakiosMetal.bundle", t_root); mkdir(p, 0755);
	bundle("one");

	/* default: every process on hakios-metal, backboardd too (no ca/bbd-hm flag needed) */
	CHECK(wants("com.apple.WebKit.GPU", &why) && strcmp(why, "default") == 0);
	bbd_reset();
	CHECK(wants("backboardd", &why) && strcmp(why, "default") == 0);
	CHECK(strncmp(g_key, "m", 1) == 0 && strstr(g_key, "-s3") != NULL);
	strlcpy(key1, g_key, sizeof(key1));
	/* the log: log/NAME.log; backboardd's own */
	shim_begin("MobileSafari");
	snprintf(p, sizeof(p), "%s/log/MobileSafari.log", t_root);
	CHECK(strcmp(g_log, p) == 0);
	shim_begin("backboardd");
	CHECK(strstr(g_log, "ca/bbd-hm.log") != NULL);
	/* m2-procs, comments and blanks */
	put("m2-procs", "# kept\n\n  Camera  # YCbCr\nMaps\n");
	CHECK(!wants("Camera", &why) && strcmp(why, "m2-procs") == 0);
	CHECK(!wants("Maps", &why));
	CHECK(wants("Photos", &why));
	/* hm-only and all-off are gone: ignored */
	put("hm-only", "com.apple.WebKit.GPU\n");
	put("all-off", "");
	CHECK(wants("Photos", &why) && strcmp(why, "default") == 0);
	snprintf(p, sizeof(p), "%s/all-off", t_root); unlink(p);
	snprintf(p, sizeof(p), "%s/hm-only", t_root); unlink(p);
	/* hm-off: everyone, backboardd too */
	put("hm-off", "");
	CHECK(!wants("Photos", &why) && strcmp(why, "hm-off") == 0);
	bbd_reset();
	CHECK(!wants("backboardd", &why) && strcmp(why, "hm-off") == 0);
	snprintf(p, sizeof(p), "%s/hm-off", t_root); unlink(p);

	/* crash trip: clean exits are not counted */
	CHECK(wants("Notes", &why));
	shim_clean_exit();
	CHECK(wants("Notes", &why));
	shim_clean_exit();
	CHECK(wants("Notes", &why));      /* 2 earlier starts, both clean: no trip */
	/* old-format start lines (no key) are not counted */
	snprintf(p, sizeof(p), "%shakios-metal-Old.starts", t_tmp);
	{ FILE *f = fopen(p, "w"); for (int i = 0; i < 5; i++) fprintf(f, "%ld %d\n", (long)time(NULL), 7); fclose(f); }
	CHECK(wants("Old", &why));
	/* three unclean starts: the third trips */
	CHECK(wants("Weather", &why));
	CHECK(wants("Weather", &why));    /* (same pid: the host test cannot fork cheaply; pids only matter for 'x') */
	CHECK(!wants("Weather", &why) && strcmp(why, "crash trip") == 0);
	CHECK(!wants("Weather", &why));   /* holds */
	CHECK(shim_listed_key(HM_FALLBACK, "Weather", key1));
	snprintf(p, sizeof(p), "%shakios-metal-Weather.m2", t_tmp);
	first_line(p, line, sizeof(line));
	CHECK(strcmp(line, key1) == 0);
	/* the marker alone holds it (m2-fallback cleared or not writable) */
	put("m2-fallback", "");
	CHECK(!wants("Weather", &why));
	/* an m2-fallback line alone holds it; a line without the key does not */
	unlink(p);
	snprintf(q, sizeof(q), "Weather %s\n", key1);
	put("m2-fallback", q);
	CHECK(!wants("Weather", &why));
	put("m2-fallback", "Weather\n");
	snprintf(q, sizeof(q), "%shakios-metal-Weather.starts", t_tmp); unlink(q);
	CHECK(wants("Weather", &why));
	/* a trip expires with a new bundle */
	CHECK(wants("Weather", &why));
	CHECK(!wants("Weather", &why) && strcmp(why, "crash trip") == 0);
	bundle("two!");
	CHECK(wants("Weather", &why) && strcmp(why, "default") == 0);
	CHECK(access(p, F_OK) != 0);      /* the old marker removed */
	CHECK(strcmp(g_key, key1) != 0);
	/* backboardd: the same trip, its files in ca/ */
	bbd_reset();
	CHECK(wants("backboardd", &why));
	CHECK(wants("backboardd", &why));
	CHECK(!wants("backboardd", &why) && strcmp(why, "crash trip") == 0);
	snprintf(p, sizeof(p), "%s/ca/bbd-hm.tripped", t_root);
	first_line(p, line, sizeof(line));
	CHECK(strcmp(line, g_key) == 0);
	CHECK(!wants("backboardd", &why));
	bundle("three");
	CHECK(wants("backboardd", &why));
	CHECK(access(p, F_OK) != 0);

	/* SweetMetal2's own gate, as SweetMetal2.mm reads it */
	CHECK(!shim_m2_clients_has("PosterBoard"));
	put("mtl2-clients", "SpringBoard, *\n-chronod\n");
	CHECK(shim_m2_clients_has("PosterBoard") && !shim_m2_clients_has("chronod") && shim_m2_clients_has("backboardd"));

	/* env: plain lines backboardd's, '*:' everyone's, 'NAME:' one process's; XR10 forced off unless allowed */
	put("env", "HM_TRACE=1\n*:HM_FAILLOG=1\ncom.apple.WebKit.GPU:HM_GMEM=0\nMaps:HM_GMEM=2\nPATH=/x\nHM_XR10=1\n");
	unsetenv("HM_TRACE"); unsetenv("HM_FAILLOG"); unsetenv("HM_GMEM"); unsetenv("HAKIOS_METAL_ALL");
	unsetenv("HAKIOS_METAL_CACHE"); unsetenv("HAKIOS_METAL_CACHE_SEED"); unsetenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION");
	shim_begin("com.apple.WebKit.GPU");
	shim_prepare_hm(false);
	CHECK(getenv("HM_TRACE") == NULL);
	CHECK(getenv("HM_FAILLOG") && strcmp(getenv("HM_FAILLOG"), "1") == 0);
	CHECK(getenv("HM_GMEM") && strcmp(getenv("HM_GMEM"), "0") == 0);
	CHECK(getenv("HAKIOS_METAL_ALL") != NULL);
	CHECK(strcmp(getenv("HM_PREWARM"), "0") == 0);
	CHECK(strcmp(getenv("HM_XR10"), "0") == 0);
	CHECK(getenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION") && strcmp(getenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION"), "1") == 0);
	CHECK(getenv("HM_LUMA_KNOB") && strtoull(getenv("HM_LUMA_KNOB"), NULL, 16) == (unsigned long long)(uintptr_t)&m2shim_luma_boost);
	/* no writable cache/: its own, cache/ and cache-seed read */
	CHECK(strstr(getenv("HAKIOS_METAL_CACHE"), "ucache/hakios-metal-cache") != NULL);
	CHECK(strstr(getenv("HAKIOS_METAL_CACHE_SEED"), "/cache:") != NULL);
	unsetenv("HM_TRACE"); unsetenv("HAKIOS_METAL_CACHE"); unsetenv("HAKIOS_METAL_CACHE_SEED");
	snprintf(p, sizeof(p), "%s/cache", t_root); mkdir(p, 01777);
	shim_begin("backboardd");
	shim_prepare_hm(true);
	CHECK(getenv("HM_TRACE") && strcmp(getenv("HM_TRACE"), "1") == 0);
	CHECK(strcmp(getenv("HM_XR10"), "0") == 0);     /* HM_XR10=1 in the env, not allowed */
	CHECK(strcmp(getenv("HAKIOS_METAL_CACHE"), p) == 0);
	put("env", "HM_XR10=1\nHM_XR10_ALLOW=1\n");
	shim_prepare_hm(true);
	CHECK(strcmp(getenv("HM_XR10"), "1") == 0);
	/* CA prefs: every one set by default; an env line flips one */
	unsetenv("HM_CA_DEEP_FB"); unsetenv("HM_CA_FB_COMPRESSION"); unsetenv("HM_CA_IOSURFACE_COMPRESSION");
	CHECK(shim_ca_prefs() == 7);
	put("env", "HM_CA_DEEP_FB=1\n*:HM_CA_IOSURFACE_COMPRESSION=1\n");
	shim_env(true);
	CHECK(shim_ca_prefs() == 2);      /* backboardd: the plain line and the *: line */
	unsetenv("HM_CA_DEEP_FB");
	shim_begin("Photos");
	shim_env(false);
	CHECK(shim_ca_prefs() == 3);

	/* luma knob file */
	CHECK(shim_luma_file() == 0);
	put("luma-boost", "1\n");
	CHECK(shim_luma_file() == 1);
	put("luma-boost", "0\n");
	CHECK(shim_luma_file() == 0);

	/* logs past 16 MB -> .1 */
	snprintf(p, sizeof(p), "%s/log/Big.log", t_root);
	put("log/Big.log", "x");
	CHECK(truncate(p, LOG_ROTATE + 1) == 0);
	shim_begin("Big");
	snprintf(q, sizeof(q), "%s.1", p);
	CHECK(access(q, F_OK) == 0);

	printf("%s: %d failures\n", fails ? "FAILED" : "ok", fails);
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", t_root);
	(void)!system(cmd);
	return fails != 0;
}
