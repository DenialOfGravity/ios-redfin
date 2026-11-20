/* m2shim_test.c: the dispatcher's choice (bench/m2shim.m) on the host, without Objective-C: lists, env lines, the
 * crash trip (one for every process, expiring with the bundle; only crashes with Metal code count), CA prefs, the log and cache places. cc -Wno-format-truncation -o t m2shim_test.c && ./t */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

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
#define CRASH_DIR HM_DIR "/crash"
#define TRIP_REPORT_WAIT_MS 0         /* the host test does not wait for crash reports */
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

/* (ws-hm-tripfilter) the test's stand-in for the dispatcher's NSJSONSerialization reader: a small JSON reader (objects,
 * arrays, strings, numbers, true/false/null) and the same walk over the crashing thread and lastExceptionBacktrace */
typedef struct jv { char t; char *s; double n; int len; char **keys; struct jv **vals; } jv;

static void j_ws(const char **p) { while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++; }

static jv *
j_parse(const char **p)
{
	jv *v = calloc(1, sizeof(*v));
	j_ws(p);
	if (**p == '{' || **p == '[') {
		char close = **p == '{' ? '}' : ']';
		v->t = **p == '{' ? 'o' : 'a';
		(*p)++;
		for (;;) {
			char *key = NULL;
			jv *c;
			j_ws(p);
			if (**p == close) { (*p)++; break; }
			if (v->t == 'o') {
				jv *k = j_parse(p);
				if (k == NULL || k->t != 's') return NULL;
				key = k->s;
				j_ws(p);
				if (**p != ':') return NULL;
				(*p)++;
			}
			if ((c = j_parse(p)) == NULL) return NULL;
			v->keys = realloc(v->keys, sizeof(char *) * (size_t)(v->len + 1));
			v->vals = realloc(v->vals, sizeof(jv *) * (size_t)(v->len + 1));
			v->keys[v->len] = key;
			v->vals[v->len++] = c;
			j_ws(p);
			if (**p == ',') (*p)++;
			else if (**p != close) return NULL;
		}
	} else if (**p == '"') {
		size_t n = 0;
		v->t = 's';
		v->s = malloc(strlen(*p) + 1);
		for ((*p)++; **p && **p != '"'; (*p)++) {
			if (**p == '\\') {
				(*p)++;
				if (**p == 'u') { v->s[n++] = '?'; *p += 4; continue; }
				v->s[n++] = **p == 'n' ? '\n' : **p == 't' ? '\t' : **p;
			} else v->s[n++] = **p;
		}
		if (**p != '"') return NULL;
		(*p)++;
		v->s[n] = '\0';
	} else if (strncmp(*p, "true", 4) == 0 || strncmp(*p, "null", 4) == 0) {
		v->t = **p == 't' ? 'b' : 'z'; v->n = **p == 't'; *p += 4;
	} else if (strncmp(*p, "false", 5) == 0) {
		v->t = 'b'; *p += 5;
	} else {
		char *end;
		v->t = 'n';
		v->n = strtod(*p, &end);
		if (end == *p) return NULL;
		*p = end;
	}
	return v;
}

static jv *
j_get(jv *o, const char *key)
{
	if (o == NULL || o->t != 'o') return NULL;
	for (int i = 0; i < o->len; i++) if (strcmp(o->keys[i], key) == 0) return o->vals[i];
	return NULL;
}

static const char *
j_str(jv *v)
{
	return v && v->t == 's' ? v->s : NULL;
}

static void
j_frames(shim_report *r, jv *frames, jv *images, bool leb)
{
	for (int i = 0; i < frames->len; i++) {
		jv *fr = frames->vals[i], *ix = j_get(fr, "imageIndex"), *im = NULL;
		if (ix && ix->t == 'n' && images && images->t == 'a' && ix->n >= 0 && ix->n < images->len) im = images->vals[(int)ix->n];
		shim_report_frame(r, j_str(j_get(im, "name")), j_str(j_get(im, "path")), j_str(j_get(fr, "symbol")), leb);
	}
}

static bool
shim_report_read(const char *path, shim_report *r)
{
	char *all, *nl;
	const char *p;
	long n;
	jv *b, *pid, *th, *ft, *leb, *images;
	FILE *f = fopen(path, "r");

	if (f == NULL) return false;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	all = calloc(1, (size_t)n + 1);
	n = (long)fread(all, 1, (size_t)n, f);
	fclose(f);
	if ((nl = strchr(all, '\n')) == NULL) return false;
	p = nl + 1;
	if ((b = j_parse(&p)) == NULL || b->t != 'o') return false;
	pid = j_get(b, "pid");
	r->pid = pid && pid->t == 'n' ? (int)pid->n : -1;
	images = j_get(b, "usedImages");
	th = j_get(b, "threads");
	ft = j_get(b, "faultingThread");
	if (th && th->t == 'a' && ft && ft->t == 'n' && ft->n >= 0 && ft->n < th->len) {
		jv *frames = j_get(th->vals[(int)ft->n], "frames");
		if (frames && frames->t == 'a') { r->crash = true; j_frames(r, frames, images, false); }
	}
	if ((leb = j_get(b, "lastExceptionBacktrace")) != NULL && leb->t == 'a') { r->crash = true; j_frames(r, leb, images, true); }
	return true;
}

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

	/* (ws-hm-fallback) a restart on purpose is not counted: by pid (intent/NAME), or the start running when intent/_ui
	 * was written (a start within 30 s of it); a trip explains itself; trip-test; a clear ("c TIME") */
	{
		long now = (long)time(NULL);
		char text[256];
		snprintf(p, sizeof(p), "%s/intent", t_root); mkdir(p, 0777);
		snprintf(p, sizeof(p), "%s/crash", t_root); mkdir(p, 0777);
		put("crash/Music-2026-11-19-101800.ips", "x");
		shim_begin("Music");
		snprintf(text, sizeof(text), "%ld 101 %s\n%ld 102 %s\n", now - 60, g_key, now - 30, g_key);
		snprintf(p, sizeof(p), "%shakios-metal-Music.starts", t_tmp);
		{ FILE *f = fopen(p, "w"); fputs(text, f); fclose(f); }
		snprintf(text, sizeof(text), "%ld 101 9\n", now - 31);
		put("intent/Music", text);
		CHECK(wants("Music", &why));                     /* 101 killed on purpose: one unclean before this start */
		CHECK(wants("Music", &why) == false && strcmp(why, "crash trip") == 0);   /* 102 + the last one: trips */
		snprintf(p, sizeof(p), "%shakios-metal-Music.m2.why", t_tmp);
		first_line(p, line, sizeof(line));
		CHECK(strncmp(line, "reason: crash trip", 18) == 0);
		snprintf(p, sizeof(p), "%s/trips/Music.why", t_root);
		CHECK(access(p, F_OK) == 0);
		{
			char all[4096] = ""; FILE *f = fopen(p, "r"); size_t n = f ? fread(all, 1, sizeof(all) - 1, f) : 0; if (f) fclose(f); all[n] = 0;
			CHECK(strstr(all, "pid 101") && strstr(all, "killed on purpose"));
			CHECK(strstr(all, "pid 102 ended without a clean exit"));
			CHECK(strstr(all, "Music-2026-11-19-101800.ips"));
		}
		snprintf(p, sizeof(p), "%s/fallback.log", t_root);
		{
			char all[4096] = ""; FILE *f = fopen(p, "r"); size_t n = f ? fread(all, 1, sizeof(all) - 1, f) : 0; if (f) fclose(f); all[n] = 0;
			CHECK(strstr(all, "Music pid") && strstr(all, "-> SweetMetal2: crash trip"));
		}
		/* pa fallback clear: marker and m2-fallback line gone, "c TIME" in the starts file */
		snprintf(p, sizeof(p), "%shakios-metal-Music.m2", t_tmp); unlink(p);
		put("m2-fallback", "");
		snprintf(p, sizeof(p), "%shakios-metal-Music.starts", t_tmp);
		{ FILE *f = fopen(p, "a"); fprintf(f, "c %ld\n", now); fclose(f); }
		CHECK(wants("Music", &why) && strcmp(why, "default") == 0);
		/* the render server (or SpringBoard) killed on purpose: the start running then is not counted */
		shim_begin("Posters");
		snprintf(text, sizeof(text), "%ld 201 %s\n%ld 202 %s\n", now - 50, g_key, now - 20, g_key);
		snprintf(p, sizeof(p), "%shakios-metal-Posters.starts", t_tmp);
		{ FILE *f = fopen(p, "w"); fputs(text, f); fclose(f); }
		snprintf(text, sizeof(text), "%ld\n", now - 10);
		put("intent/_ui", text);
		CHECK(wants("Posters", &why));
		/* an old _ui note (past 30 s) does not excuse anything */
		shim_begin("Posters2");
		snprintf(text, sizeof(text), "%ld 301 %s\n%ld 302 %s\n", now - 90, g_key, now - 60, g_key);
		snprintf(p, sizeof(p), "%shakios-metal-Posters2.starts", t_tmp);
		{ FILE *f = fopen(p, "w"); fputs(text, f); fclose(f); }
		snprintf(text, sizeof(text), "%ld\n", now - 55);
		put("intent/_ui", text);
		CHECK(!wants("Posters2", &why));
		/* trip-test: exact names only */
		put("trip-test", "*\n");
		CHECK(wants("Clock", &why));
		put("trip-test", "Clock\n");
		CHECK(!wants("Clock", &why));
		snprintf(p, sizeof(p), "%s/trips/Clock.why", t_root);
		first_line(p, line, sizeof(line));
		CHECK(strncmp(line, "reason: test", 12) == 0);
		snprintf(p, sizeof(p), "%s/trip-test", t_root); unlink(p);
	}

	/* (ws-hm-tripfilter) only crashes with Metal code count: SYNTHETIC .ips fixtures (no real report's content) */
	{
		long now = (long)time(NULL);
		char text[2048], all[8192];
		/* images: 0 the process, 1 libsystem_kernel, 2 CoreFoundation, 3 libobjc, 4 FakeCapture, 5 HakiosMetal */
		static const char imgs[] = "\"usedImages\":[{\"name\":\"fakesrvd\",\"path\":\"/usr/sbin/fakesrvd\"},"
			"{\"name\":\"libsystem_kernel.dylib\"},{\"name\":\"CoreFoundation\"},{\"name\":\"libobjc.A.dylib\"},"
			"{\"name\":\"FakeCapture\",\"path\":\"/System/Library/PrivateFrameworks/FakeCapture.framework/FakeCapture\"},"
			"{\"name\":\"HakiosMetal\",\"path\":\"/var/local/hmetal/lib/HakiosMetal.bundle/HakiosMetal\"}]";
		static const char ft_plain[] = "\"faultingThread\":1,\"threads\":[{\"frames\":[{\"imageIndex\":5,\"symbol\":\"idle\"}]},"
			"{\"frames\":[{\"imageIndex\":4,\"symbol\":\"-[FakeGraph stop:]\"},{\"imageIndex\":0,\"symbol\":\"main\"}]}]";
		static const char ft_abort[] = "\"faultingThread\":0,\"threads\":[{\"frames\":[{\"imageIndex\":1,\"symbol\":\"__pthread_kill\"},"
			"{\"imageIndex\":0,\"symbol\":\"main\"}]}]";
		static const char ft_metal[] = "\"faultingThread\":0,\"threads\":[{\"frames\":[{\"imageIndex\":1,\"symbol\":\"__pthread_kill\"},"
			"{\"imageIndex\":5,\"symbol\":\"-[HakiosMetalDevice newBuffer]\"}]}]";
		static const char leb_plain[] = "\"lastExceptionBacktrace\":[{\"imageIndex\":2,\"symbol\":\"__exceptionPreprocess\"},"
			"{\"imageIndex\":3,\"symbol\":\"objc_exception_throw\"},{\"imageIndex\":4,\"symbol\":\"fake_resetStorage\"}]";
		static const char leb_metal[] = "\"lastExceptionBacktrace\":[{\"imageIndex\":2,\"symbol\":\"__exceptionPreprocess\"},"
			"{\"imageIndex\":5,\"symbol\":\"hm_throw\"}]";
#define IPS(file, pid, a, b) do { snprintf(text, sizeof(text), "{\"bug_type\":\"309\",\"name\":\"x\"}\n{\"pid\":%d,%s,%s%s%s}\n", \
			(pid), imgs, a, b[0] ? "," : "", b); put(file, text); } while (0)
#define STARTS(name, a, b) do { shim_begin(name); snprintf(text, sizeof(text), "%ld %d %s\n%ld %d %s\n", now - 60, (a), g_key, \
			now - 30, (b), g_key); snprintf(p, sizeof(p), "%shakios-metal-%s.starts", t_tmp, name); \
			{ FILE *f = fopen(p, "w"); fputs(text, f); fclose(f); } } while (0)
#define READ(path) do { FILE *f = fopen(path, "r"); size_t n = f ? fread(all, 1, sizeof(all) - 1, f) : 0; if (f) fclose(f); \
			all[n] = 0; } while (0)
		/* the image set */
		CHECK(shim_metal_image("HakiosMetal", NULL) && shim_metal_image("SweetMetal2", NULL) && shim_metal_image("Metal", NULL));
		CHECK(shim_metal_image("MetalTools", NULL) && shim_metal_image("libMTLCapture.dylib", NULL) && shim_metal_image("AGXMetalA14", NULL));
		CHECK(shim_metal_image("IOGPU", NULL) && shim_metal_image("IOAccelerator", NULL) && shim_metal_image("MPSCore", NULL));
		CHECK(shim_metal_image("MetalPerformanceShadersGraph", NULL) && shim_metal_image("GPUCompiler", NULL));
		CHECK(shim_metal_image("libhmx.dylib", NULL) && shim_metal_image(NULL, "/var/local/hmetal/lib/SweetMetal2.bundle/x"));
		CHECK(shim_metal_image("Unnamed", "/var/local/SweetMetal2.bundle.pre-hmca/SweetMetal2"));
		CHECK(!shim_metal_image("FakeCapture", "/System/Library/PrivateFrameworks/FakeCapture.framework/FakeCapture"));
		CHECK(!shim_metal_image("libsystem_kernel.dylib", NULL) && !shim_metal_image("CoreFoundation", NULL) && !shim_metal_image(NULL, NULL));

		/* no Metal code anywhere (crashing thread, or an exception's backtrace): not counted, said once, no trip */
		IPS("crash/Fakesrvd-2026-11-20-000001.ips", 401, ft_plain, "");
		IPS("crash/Fakesrvd-2026-11-20-000002.ips", 402, ft_abort, leb_plain);
		STARTS("Fakesrvd", 401, 402);
		CHECK(wants("Fakesrvd", &why) && strcmp(why, "default") == 0);
		snprintf(p, sizeof(p), "%s/fallback.log", t_root);
		READ(p);
		CHECK(strstr(all, "Fakesrvd pid 401 crash not counted: no Metal code on the crashing thread "
				  "(Fakesrvd-2026-11-20-000001.ips, top: FakeCapture -[FakeGraph stop:])") != NULL);
		CHECK(strstr(all, "Fakesrvd pid 402 crash not counted: no Metal code on the crashing thread "
				  "(Fakesrvd-2026-11-20-000002.ips, top: FakeCapture fake_resetStorage)") != NULL);
		CHECK(strstr(all, "Fakesrvd pid 401 -> SweetMetal2") == NULL && strstr(all, "this start only") == NULL);
		snprintf(q, sizeof(q), "%shakios-metal-Fakesrvd.starts", t_tmp);
		READ(q);
		CHECK(strstr(all, "n 401\n") && strstr(all, "n 402\n"));
		CHECK(wants("Fakesrvd", &why));                  /* again: 'n' lines kept, no second log line */
		READ(p);
		CHECK(strstr(all, "pid 401 crash not counted") && !strstr(strstr(all, "pid 401 crash not counted") + 1, "pid 401 crash not counted"));
		snprintf(q, sizeof(q), "%shakios-metal-Fakesrvd.m2", t_tmp);
		CHECK(access(q, F_OK) != 0);

		/* HakiosMetal on the crashing thread: counted (with the other one, no report: trips) */
		IPS("crash/Fakemet-2026-11-20-000001.ips", 501, ft_metal, "");
		STARTS("Fakemet", 501, 502);
		CHECK(!wants("Fakemet", &why) && strcmp(why, "crash trip") == 0);
		/* HakiosMetal only in lastExceptionBacktrace: counted */
		IPS("crash/Fakeleb-2026-11-20-000001.ips", 601, ft_abort, leb_metal);
		IPS("crash/Fakeleb-2026-11-20-000002.ips", 602, ft_metal, "");
		STARTS("Fakeleb", 601, 602);
		CHECK(!wants("Fakeleb", &why) && strcmp(why, "crash trip") == 0);
		/* no report at all, a report for another pid, a report older than the start: counted */
		IPS("crash/Fakenone-2026-11-20-000001.ips", 799, ft_plain, "");
		IPS("crash/Fakenone-2026-11-20-000002.ips", 702, ft_plain, "");
		snprintf(q, sizeof(q), "%s/crash/Fakenone-2026-11-20-000002.ips", t_root);
		{ struct utimbuf u = { now - 40, now - 40 }; utime(q, &u); }   /* before 702's start (now - 30) */
		STARTS("Fakenone", 701, 702);
		CHECK(!wants("Fakenone", &why) && strcmp(why, "crash trip") == 0);
		/* one Metal-free crash and one without a report: the second alone does not trip */
		IPS("crash/Fakehalf-2026-11-20-000001.ips", 801, ft_plain, "");
		STARTS("Fakehalf", 801, 802);
		CHECK(wants("Fakehalf", &why));
		/* trip-any: every unclean exit counts (the old rule) */
		IPS("crash/Fakeany-2026-11-20-000001.ips", 901, ft_plain, "");
		IPS("crash/Fakeany-2026-11-20-000002.ips", 902, ft_abort, leb_plain);
		put("trip-any", "");
		STARTS("Fakeany", 901, 902);
		CHECK(!wants("Fakeany", &why) && strcmp(why, "crash trip") == 0);
		snprintf(p, sizeof(p), "%s/trip-any", t_root); unlink(p);
		snprintf(p, sizeof(p), "%s/fallback.log", t_root);
		READ(p);
		CHECK(strstr(all, "Fakeany pid 901 crash not counted") == NULL && strstr(all, "Fakemet pid 501 crash not counted") == NULL);
#undef IPS
#undef STARTS
#undef READ
	}

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
