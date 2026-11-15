#!/usr/bin/env python3
"""plugin.py IN.m OUT.m: hakios' render-capable Metal plug-in (userland/src/HakiosMetalCA.m) made usable by hmbench.

The file is not in this repository: bench/build.sh fetches it at build time from oosexgang/hakios-xnu-kernel at the
pinned commit (sha256 checked), because its origin (vcvkk/darwin-redwood, branch hakios-workspace, eb9fdf5) carries a
licence hakios lists as "to verify" (its NOTICE, THIRD_PARTY_LICENSES.md). hakios' generic plug-in
(userland/src/HakiosMetal.m, hakios-metal/plugin/HakiosMetal.m; MIT) cannot be used for the benchmark: its render
pipeline state is a stand-in "that holds nothing", it has no render command encoder, and its GPUStartTime is a stub.

Edits, each by exact text, refused if the text is not found once (a span edit: its first and last text each once):
  - the compiled-stage cache in /var/local/hmetal/cache (was /private/var/db/...: nothing of ours outside /var/local)
  - the read-only seed cache at /var/local/hmetal/cache-seed (was /redwood/share/..., which does not exist here)
  - the device for any process whose environment has HAKIOS_METAL_ALL (was: backboardd alone, or boot-arg metal_all=1)
  - (H3) every hm_print line also appended to the file $HAKIOS_METAL_LOG names (bench/m2shim.m sets it in backboardd:
    a launchd job's stderr here is not a log anybody reads)
  - (hmvar; replaces H3 fix 1) how CoreAnimation's uber shaders (fixed_*) are chosen, HAKIOS_METAL_CA_PICK in the
    process's environment (bench/m2shim.m passes /var/local/hmetal/ca/env through):
      exact   (default) the pinned plug-in's pre-specialised variant out of QuartzCore's default.pipelinelib only where
              a label names it exactly (the function's label, or the pipeline's label in pipelines.desc); a pipeline
              CoreAnimation makes at run time, which the library does not name, is compiled from the function itself
              with the constants QuartzCore specialised it by (both stages: the vertex variant is not guessed from the
              fragment's, which is what read 64 bytes of constant buffer 3 where 16 were bound, and no near fragment
              variant with flags added stands in, as TcmaBpldA2LlnXhfcx did for TcmaBpldA2LlnXhf)
      own     every uber shader from the function with its constants (H3 fix 1 as it was)
      nearest upstream's label pick with hm_ca_nearest() for run-time pipelines (3b9db78; H3 before fix 1)
    HAKIOS_METAL_CA_VARIANTS=1 keeps its meaning (the label pick: = nearest) and wins over HAKIOS_METAL_CA_PICK.
    exact and own need QuartzCore's default.metallib (the unspecialised functions; iOS 16.7 has it); without it, nearest.
    A pipeline that fails to link with a function compiled whole is linked once more with the label pick.
  - (hmvar) one line per render pipeline: "CA pipeline LABEL: vs NAME [pick] constants N (...) | fs ... | ok/refused",
    HAKIOS_METAL_CA_LOG=0 silences it; a refused draw names its pipeline's label and picks.
  - (hmv2) HM_PROF=2's per-pass profile, HM_UBWC_RT=1 compressed CA render targets, the core's notices in this log
  - (hmv3) HM_GMEM=0 (default: every pass sysmem) | 1 (upstream's metal_gmem_draws rule) | N (GMEM from N draws), ",T"
    at most T tiles; HM_PROF's frames are the buffers that drew the display's IOSurface when nothing is presented
    (the window server presents no drawable), and its classes carry colour attachment 0's load/store actions
  - (hmv4) HM_TRACE=1 (pipelines' ms and their compiler share, 10 s buffer summaries, hangs past 100 ms with the last
    20 events, stalls of an open buffer sampled, ca_prof by HM_CA_PROF_US); HM_PREWARM=1 (default; HM_PREWARM_DELAY s,
    default 30) QuartzCore's library's pipelines made once in the background into the disk cache; the specialiser's
    compiles background ones (core: HM_COMPILE_PRIO); HM_XR10=0 (default) no XR10 formats offered and 550/551 as BGRA8,
    as SweetMetal2 does
  - (hmv5) HM_SPEC=0 (default): no uber-shader specialisation (1 = hakios' background specialiser, which preceded every
    GPU hang in hmt6); HM_FAILLOG=1 (default): command buffers that end in error or leave work out are said
  - (hmv7) HM_SPEC=2 (an uber shader drawn only through the program specialised for the draw's configuration, compiled
    on the spot), HM_DENY=word,... (pipelines whose label holds a word: the other pick, and their uber shaders as
    HM_SPEC=2), HM_HANG_BLAME=1 (default; after HM_HANG_BLAME_AFTER=2 hung buffers a program is drawn as HM_SPEC=2 draws it)
  - (hmv8) HM_SHADOW_FRESH=1 (default): a read-only copy of an unaligned surface's rows made again once a command buffer
    whatever its seed (0: only when the seed moved); HM_LOG_TIME=1 (default): the time of day in each line's prefix;
    HM_TRACE's 10 s 'trace: surfaces:' line (live textures over IOSurfaces, row copies a s and forced ones, XR decodes,
    ring barriers) and 'trace: display-sized surface N let go' (the display torn down: ms since the last buffer);
    the core's HM_XR_SYNC (render.cpp) is set in the same env file
  - (hmv10) (always since hm-int2; was HM_SCHED_WAIT) -waitUntilScheduled waits for a held commit's send (was: returned at once), and
    -commitAndWaitUntilSubmitted = -commit + -waitUntilScheduled; the surfaces line also counts the core's HM_XR_LATE
    decodes, HM_XR_CHECK's 'changed after decode', HM_XR_WB_SYNC write-backs, held commits and scheduled waits
  - (hmv11) the surfaces line also counts the core's HM_XR_GPU decodes (xr_decode on the GPU, no ring barrier), their
    buffers, fallbacks to the processor and HM_XR_GPU_VERIFY's checks
  - (hmv12) HM_SHADOW_WAIT=1 (default; HM_SHADOW_WAIT_MS=250): a copy of an unaligned surface's rows is not rewritten by
    the processor while a committed buffer still reads or draws it (the refresh waits; an unmoved-seed HM_SHADOW_FRESH
    refresh is put off instead), and a write-back is deferred to a later committed writer; 0 = as before
  - (hmyc) HM_YCBCR=native (default) | cpu: '420f'/'420v' surfaces sampled as 500/540 are wrapped over both planes
    (hakios_metal_texture_new_ycbcr) and converted to RGBA8 on the GPU by the core (ycbcr_decode), only when the seed moves;
    the processor copy (cpu, or planes the kernel cannot read) uses the core's integers; one matrix rule (hm_ycbcr_kind:
    attachment, else HD '420v' BT.709, else BT.601); YCbCr counts in the surfaces line
  - (hm-cache) HM_COMPILE_LOG (default with HM_PROF>=1): "compile: pipeline ...: wall, compiling, CPU, front end, folding,
    waited; compiled, cached (memo, seed)"; the waited-for specialisation's line says where its stage came from (compiled,
    memo, cache, seed), its CPU ms and the configuration's bytes; HM_SPEC_QOS=1 (default) the specialiser's queue at
    user-initiated QoS on the core's background compiler; HM_SPEC_ASYNC=0 (default; 1: an unseen configuration of a
    denied uber shader compiled on the queue and its draws left out meanwhile); HM_PREWARM=1 (default) rebuilt on the
    stage cache's manifests (missing stages under the current key compiled at utility QoS after HM_PREWARM_DELAY=60 s,
    waiting while frames are drawn); ca-prof only with HM_CA_PROF_US=N or the boot-arg (was on with HM_TRACE);
    HM_HANG_EXEC=1 (default): the HANG trace splits GPU time into queued + running and skips buffers that only queued
The variant tables are not in this repository or in the build: the plug-in reads them at run time from the phone's
own QuartzCore (default.pipelinelib/pipelines.desc + functions). bench/ca-variants.py reads the same files off the
phone (apple-ref/<version>/) to compare versions and to replay a log's pipeline labels through the same choice.
  - (hm-int8) HM_PROF>=2: "prof: pace 5.0 s: N frames | mean/p95 ms a frame: blocked (buffer slot, waitUntil*) | gap last
    commit -> first buffer made | encode first buffer made -> commit | commit -> kernel submit (kernel call) | commit ->
    completed | committed with an earlier frame unretired X of N (most in flight) | not sent", per frame = a buffer that
    drew the display's IOSurface; the kernel submit times come from hakios_metal_cb_submit_times (core, hm-int8).
    CoreAnimation's own swap-ring wait (SwapWait / can_update_) is outside hm: it shows as the gap. Default unchanged.
"""
import sys

CA_HELPERS = r'''/* (redfin hmvar) How CoreAnimation's uber shaders are chosen (bench/plugin.py): HAKIOS_METAL_CA_PICK exact (default),
 * own or nearest; HAKIOS_METAL_CA_VARIANTS=1 = nearest, as before. What each pipeline got goes into one line per pipeline
 * (hm_ca_log_pipeline) and, for a draw refused later, beside the refusal (hm_ca_info_of). */
#define HM_QC_METALLIB "/System/Library/Frameworks/QuartzCore.framework/default.metallib"
enum { HM_CA_NEAREST = 0, HM_CA_EXACT = 1, HM_CA_OWN = 2 };
static __thread int hm_ca_force_nearest;          /* the one retry of a pipeline that would not link compiled whole */
static __thread char hm_ca_pick_of[2][192];       /* this thread's last choice: [0] the fragment stage, [1] the vertex */
static NSMutableDictionary *hm_ca_program_info;   /* program -> "label: vs pick, fs pick" */

static int
hm_ca_pick_mode(void)
{
	static int mode = -1;

	if (mode < 0) {
		const char *v = getenv("HAKIOS_METAL_CA_VARIANTS"), *p = getenv("HAKIOS_METAL_CA_PICK");
		const int qc = access(HM_QC_METALLIB, R_OK) == 0;
		int m = HM_CA_EXACT;

		if (v != NULL && v[0] == '1') {
			m = HM_CA_NEAREST;
		} else if (p != NULL && strcmp(p, "nearest") == 0) {
			m = HM_CA_NEAREST;
		} else if (p != NULL && strcmp(p, "own") == 0) {
			m = HM_CA_OWN;
		} else if (p != NULL && p[0] != '\0' && strcmp(p, "exact") != 0) {
			hm_print("HAKIOS_METAL_CA_PICK=%s is none of exact, own, nearest: exact\n", p);
		}
		if (m != HM_CA_NEAREST && !qc) {
			m = HM_CA_NEAREST;
		}
		hm_print("CoreAnimation's uber shaders: pick %s (HAKIOS_METAL_CA_PICK=%s, HAKIOS_METAL_CA_VARIANTS=%s, " HM_QC_METALLIB " %s)\n",
			 m == HM_CA_OWN ? "own (every one compiled from the function, with the constants QuartzCore specialised it by)" :
			 m == HM_CA_EXACT ? "exact (the library's variant where a label names it, else the function with its constants)" :
			 "nearest (the variant a label names, else the nearest one: upstream's label pick)",
			 p != NULL ? p : "unset", v != NULL ? v : "unset", qc ? "present" : "absent: exact and own need it");
		mode = m;
	}
	return mode;
}

static void
hm_ca_note(BOOL vertex, const char *how, NSString *chosen)
{
	NSString *c = [chosen hasPrefix:@"com.apple.coreanimation."] ? [chosen substringFromIndex:24] : chosen;

	snprintf(hm_ca_pick_of[vertex ? 1 : 0], sizeof(hm_ca_pick_of[0]), "%s%s%s", how, c != nil ? " " : "", c != nil ? [c UTF8String] : "");
}

static NSString *
hm_ca_constants_text(const hakios_metal_constant *c, unsigned n)
{
	NSMutableString *s = [NSMutableString stringWithFormat:@"%u", n];
	unsigned zero = 0;

	for (unsigned i = 0; i < n; i++) {
		if (c[i].bits == 0) zero++;
	}
	if (n != 0) {
		[s appendFormat:@" (%u zero;", zero];
		for (unsigned i = 0; i < n; i++) {
			if (c[i].bits != 0) [s appendFormat:@" %u=%llu", c[i].index, (unsigned long long)c[i].bits];
		}
		[s appendString:@")"];
	}
	return s;
}

static const char *
hm_ca_info_of(const void *program)
{
	@synchronized ([NSMutableArray class]) {
		NSString *s = [hm_ca_program_info objectForKey:[NSValue valueWithPointer:program]];
		return s != nil ? [s UTF8String] : "no pipeline line";
	}
}

static void
hm_ca_log_pipeline(id descriptor, id vs, id fs, const hakios_metal_constant *vc, unsigned vn, const hakios_metal_constant *fc,
		   unsigned fn, const void *program, const char *why, BOOL retried)
{
	static int quiet = -1;
	NSString *label = [descriptor label];
	const char *vp = hm_ca_pick_of[1][0] ? hm_ca_pick_of[1] : "function", *fp = hm_ca_pick_of[0][0] ? hm_ca_pick_of[0] : "function";

	if (quiet < 0) {
		const char *q = getenv("HAKIOS_METAL_CA_LOG");

		quiet = q != NULL && q[0] == '0';
	}
	if (program != NULL) {
		@synchronized ([NSMutableArray class]) {
			if (hm_ca_program_info == nil) hm_ca_program_info = [[NSMutableDictionary alloc] init];
			[hm_ca_program_info setObject:[NSString stringWithFormat:@"pipeline %@: vs %s, fs %s", label != nil ? label : @"(no label)", vp,
										fs != nil ? fp : "none"]
					       forKey:[NSValue valueWithPointer:program]];
		}
	}
	if (quiet) {
		return;
	}
	hm_print("CA pipeline %s: vs %s [%s] constants %s | fs %s [%s] constants %s | %s%s%s\n", label != nil ? [label UTF8String] : "(no label)",
		 [[vs name] UTF8String], vp, [hm_ca_constants_text(vc, vn) UTF8String], fs != nil ? [[fs name] UTF8String] : "none",
		 fs != nil ? fp : "-", [hm_ca_constants_text(fc, fn) UTF8String], program != NULL ? "ok" : "refused: ",
		 program != NULL ? "" : (why != NULL ? why : "?"), retried ? " (after a retry with the label pick)" : "");
}

'''

BITCODE_FOR_START = '\tif (name == nil || ![name hasPrefix:@"fixed_"]) {\n\t\treturn own;\n\t}\n\thm_ca_load();\n\tif ([function respondsToSelector:@selector(label)]) {'
BITCODE_FOR_END = '\treturn variant != nil ? variant : own;\n}\n'
BITCODE_FOR_NEW = r'''	if (name == nil || ![name hasPrefix:@"fixed_"]) {
		return own;
	}
	{   /* (redfin hmvar) HAKIOS_METAL_CA_PICK: exact (default), own, nearest (= HAKIOS_METAL_CA_VARIANTS=1, upstream's pick) */
		int mode = hm_ca_force_nearest ? HM_CA_NEAREST : hm_ca_pick_mode();
		const char *how = NULL;

		/* (redfin hmv7) HM_DENY: a pipeline whose label holds one of its words gets the other pick */
		if (pipeline != nil && hm_v7_denied_text([pipeline UTF8String])) {
			const int was = mode;

			mode = mode == HM_CA_EXACT ? HM_CA_OWN : mode == HM_CA_OWN ? HM_CA_NEAREST : HM_CA_OWN;
			if (mode == HM_CA_OWN && own == nil) mode = HM_CA_NEAREST;
			hm_print("HM_DENY: pipeline %s (%s): pick %s instead of %s\n", [pipeline UTF8String], [name UTF8String],
				 mode == HM_CA_OWN ? "own" : "nearest", was == HM_CA_EXACT ? "exact" : was == HM_CA_OWN ? "own" : "nearest");
		}

		if (mode == HM_CA_OWN && own != nil) {
			hm_ca_note(vertex, "own", nil);
			return own;
		}
		hm_ca_load();
		if ([function respondsToSelector:@selector(label)]) {
			label = [function label];
		}
		if (label != nil && (variant = hm_ca_variants[label]) != nil) {
			chosen = label;
			how = "exact-function";
		} else if (pipeline != nil && hm_ca_pipelines[pipeline] != nil) {
			NSArray *pair = hm_ca_pipelines[pipeline];

			chosen = vertex ? pair[0] : ([pair count] > 1 ? pair[1] : nil);
			variant = chosen != nil ? hm_ca_variants[chosen] : nil;
			how = "exact-pipeline";
		} else if (mode == HM_CA_EXACT && own != nil) {
			how = "own";   /* a pipeline the library does not name: the function itself, with its constants */
		} else if (pipeline != nil && [pipeline hasPrefix:@"com.apple.coreanimation.draw."]) {
			/* a pipeline CoreAnimation makes at run time, which the library does not name: its fragment function is the
			 * spec after the first '_' (so in all 986 the library names) and its vertex one the one every pipeline of that
			 * fragment function has (one each, in all 444) -- picked apart, a pipeline got the wrong vertex variant and
			 * the link refused it ("reads varying slot 33, which the vertex stage does not write"), and CoreAnimation
			 * aborted the window server */
			NSString *spec = [pipeline substringFromIndex:[@"com.apple.coreanimation.draw." length]];
			NSRange under = [spec rangeOfString:@"_"];
			NSString *frag = under.location != NSNotFound ?
				[@"com.apple.coreanimation.frag." stringByAppendingString:[spec substringFromIndex:NSMaxRange(under)]] : nil;
			NSString *asked = frag;

			if (frag != nil && hm_ca_variants[frag] == nil) {
				frag = hm_ca_nearest(frag);
			}
			chosen = frag == nil ? nil : (vertex ? hm_ca_frag_vert[frag] : frag);
			variant = chosen != nil ? hm_ca_variants[chosen] : nil;
			how = frag != nil && ![frag isEqualToString:asked] ? (vertex ? "vert-of-nearest" : "nearest") :
			      (vertex ? "vert-of-frag" : "frag-label");
		}
		if (variant == nil && (how == NULL || strcmp(how, "own") != 0)) {
			how = "own (no variant)";
		}
		hm_ca_note(vertex, how, variant != nil ? chosen : nil);
		if ((variant == nil && strcmp(how, "own") != 0) || (mode == HM_CA_NEAREST && said++ < 40)) {
			hm_print("CoreAnimation's %s (label %s, pipeline %s): %s%s\n", [name UTF8String], label ? [label UTF8String] : "none",
				 pipeline ? [pipeline UTF8String] : "none", variant ? "the variant " : "no variant: its own module",
				 variant ? [chosen UTF8String] : "");
		}
		return variant != nil ? variant : own;
	}
}
'''

HMV2_HELPERS = r'''
/* (redfin hmv2) the core's one-time notices (a short constant binding padded, HM_CB_PAD) into this log */
static void
hm_notice(const char *text)
{
	hm_print("%s\n", text);
}

/* (redfin hmv2) HM_PROF in the environment: 1 the metal_fps report (every 2 s; boot-arg metal_fps does the same), 2 that
 * and every 5 s where the GPU's time goes: passes a frame, draws a pass, each kind of pass's GPU time from its
 * CP_ALWAYS_ON_COUNTER stamps, sysmem or GMEM (tiles), the target's format and layout, and the bytes cleared, loaded
 * into GMEM and stored from it. (hmv3) and colour attachment 0's load/store actions, by class and over all draw passes.
 * (hmv3) A frame is a presentDrawable where the process presents so; the window server never does (it draws into the
 * display's IOSurfaces), so there a frame is a command buffer with a render pass into an IOSurface as large as the
 * largest drawn so far (hm_frame_target): hmv2's "frames" stayed 0 and its "a frame" figures were per 5 s window */
static int hm_presents_seen;   /* (redfin hmv3) a presentDrawable completed in this process */
static int
hm_prof_level(void)
{
	static int level = -1;

	if (level < 0) {
		const char *e = getenv("HM_PROF");
		level = e != NULL ? atoi(e) : 0;
	}
	return level;
}

#define HM_PROF_CLASSES 24
struct hm_prof_class {
	unsigned kind, format, width, height, tiling, gmem, count, draws, tiles;
	unsigned load, store;   /* (redfin hmv3) colour attachment 0's MTLLoadAction / MTLStoreAction, 255 none */
	double us;
	unsigned long long cleared, loaded, stored;
};
static struct {
	pthread_mutex_t lock;
	double since;
	unsigned presents, buffers, passes, draws, gmemPasses, tiles, other;
	unsigned loads[4];   /* (redfin hmv3) draw passes by colour attachment 0's load action: DontCare, Load, Clear, none */
	double gpuUs, passUs;
	unsigned long long cleared, loaded, stored;
	unsigned hist[6];   /* draw passes by their draws: 0, 1, 2-4, 5-15, 16-55, 56 and more */
	unsigned long long sync[28];   /* (ws-hm-perf2) hakios_metal_cb_sync_counts summed over the buffers ((ws-hm-int6) 28: the UBWC classes) */
	unsigned synced;
	struct hm_prof_class cls[HM_PROF_CLASSES];
} hm_prof = { PTHREAD_MUTEX_INITIALIZER };

static int
hm_prof_cmp(const void *a, const void *b)
{
	const double x = ((const struct hm_prof_class *)a)->us, y = ((const struct hm_prof_class *)b)->us;
	return x < y ? 1 : x > y ? -1 : 0;
}

/* (ws-hm-sbar) HM_PROF=3: HM_PROF=2 and every draw of a sysmem pass timed alone (a wait for idle before and after it, so
 * the frame rate drops: for finding WHICH draws cost, not how fast things are), every 5 s by the target's size and the
 * program's name (the core adds " FETCH" for a framebuffer-fetching draw), with where the draws lie; and how many draws
 * of the display-sized passes lie in the status bar's rows (0-169 of 2436: the bar is 132) */
#define HM_PROF_DRAWS 96
static struct {
	unsigned n, fetchN, sbN, sbFetchN;
	double us, fetchUs, sbUs;
	struct { unsigned w, h, count, sb; double us, aw, ah, ay; char name[160]; } d[HM_PROF_DRAWS];
	unsigned other;
} hm_prof_draws;

static void
hm_prof_draw_add(unsigned w, unsigned h, const char *name, double us, const unsigned area[4])
{
	const BOOL fetch = strstr(name, " FETCH") != NULL;
	const BOOL screen = w >= 1000 && h >= 2000;
	const BOOL sb = screen && area[3] < 170;
	int k;

	hm_prof_draws.n++, hm_prof_draws.us += us;
	if (fetch) hm_prof_draws.fetchN++, hm_prof_draws.fetchUs += us;
	if (sb) hm_prof_draws.sbN++, hm_prof_draws.sbUs += us, hm_prof_draws.sbFetchN += fetch ? 1 : 0;
	for (k = 0; k < HM_PROF_DRAWS; k++) {
		if (hm_prof_draws.d[k].count == 0 || (hm_prof_draws.d[k].w == w && hm_prof_draws.d[k].h == h &&
		    strncmp(hm_prof_draws.d[k].name, name, sizeof(hm_prof_draws.d[k].name) - 1) == 0)) {
			break;
		}
	}
	if (k == HM_PROF_DRAWS) {
		hm_prof_draws.other++;
		return;
	}
	if (hm_prof_draws.d[k].count == 0) {
		hm_prof_draws.d[k].w = w, hm_prof_draws.d[k].h = h;
		strlcpy(hm_prof_draws.d[k].name, name, sizeof(hm_prof_draws.d[k].name));
	}
	hm_prof_draws.d[k].count++;
	hm_prof_draws.d[k].sb += sb ? 1 : 0;
	hm_prof_draws.d[k].us += us;
	hm_prof_draws.d[k].aw += area[2] >= area[0] ? area[2] - area[0] + 1 : 0;
	hm_prof_draws.d[k].ah += area[3] >= area[1] ? area[3] - area[1] + 1 : 0;
	hm_prof_draws.d[k].ay += area[1];
}

static int
hm_prof_draw_cmp(const void *a, const void *b)
{
	const double x = ((const __typeof__(hm_prof_draws.d[0]) *)a)->us, y = ((const __typeof__(hm_prof_draws.d[0]) *)b)->us;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void
hm_prof_note(hakios_metal_cb *cb, double gpu, BOOL present)
{
	static const char *const kinds[] = {"draw", "copy", "clear", "dispatch"};
	double now;

	if (hm_prof_level() < 2) {
		return;
	}
	pthread_mutex_lock(&hm_prof.lock);
	now = hm_now();
	if (hm_prof.since == 0) {
		hm_prof.since = now;
	}
	if (present) {
		hm_prof.presents++;
	} else if (cb != NULL) {
		hm_prof.buffers++;
		hm_prof.gpuUs += gpu * 1e6;
		{   /* (ws-hm-perf2) the sync line: the GPU's own time, the census, the pass switches' counts */
			unsigned long long v[28];
			const int got = hakios_metal_cb_sync_counts(cb, v, 28);

			for (int k = 0; k < got; k++) {
				hm_prof.sync[k] += v[k];
			}
			if (got > 0) {
				hm_prof.synced++;
			}
		}
		for (unsigned i = 0;; i++) {
			unsigned kind = 0, w = 0, h = 0, draws = 0, format = 0, tiling = 0, gmem, la = 255, sa = 255;
			unsigned long long c = 0, l = 0, st = 0;
			double us = 0;
			int tiles, k;

			if (hakios_metal_cb_pass_time(cb, i, &kind, &w, &h, &draws, &us) <= (int)i) {
				break;
			}
			hakios_metal_cb_pass_info(cb, i, &format, &tiling, &c, &l, &st);
			tiles = hakios_metal_cb_pass_tiles(cb, i);
			hakios_metal_cb_pass_actions(cb, i, &la, &sa);   /* (redfin hmv3) */
			gmem = tiles > 0;
			if (kind == 0) {
				hm_prof.loads[la < 3 ? la : 3]++;
			}
			hm_prof.passes++;
			hm_prof.draws += draws;
			hm_prof.passUs += us;
			hm_prof.cleared += c, hm_prof.loaded += l, hm_prof.stored += st;
			if (gmem) {
				hm_prof.gmemPasses++;
				hm_prof.tiles += (unsigned)tiles;
			}
			if (kind == 0) {
				hm_prof.hist[draws == 0 ? 0 : draws == 1 ? 1 : draws < 5 ? 2 : draws < 16 ? 3 : draws < 56 ? 4 : 5]++;
			}
			for (k = 0; k < HM_PROF_CLASSES; k++) {
				struct hm_prof_class *x = &hm_prof.cls[k];
				if (x->count == 0 || (x->kind == kind && x->format == format && x->width == w && x->height == h && x->tiling == tiling && x->gmem == gmem &&
				    x->load == la && x->store == sa)) {
					break;
				}
			}
			if (k == HM_PROF_CLASSES) {
				hm_prof.other++;
				continue;
			}
			{
				struct hm_prof_class *x = &hm_prof.cls[k];
				x->kind = kind, x->format = format, x->width = w, x->height = h, x->tiling = tiling, x->gmem = gmem;
				x->load = la, x->store = sa;
				x->count++, x->draws += draws, x->tiles += gmem ? (unsigned)tiles : 0, x->us += us;
				x->cleared += c, x->loaded += l, x->stored += st;
			}
		}
		for (unsigned i = 0; hm_prof_level() >= 3; i++) {   /* (ws-hm-sbar) HM_PROF=3: the buffer's draws, each timed alone */
			const char *name = NULL;
			unsigned pass = 0, area[4] = {0, 0, 0, 0}, kind = 0, w = 0, h = 0, draws = 0;
			double dus = 0, pus = 0;

			if (hakios_metal_cb_draw_time(cb, i, &name, &dus) <= (int)i || name == NULL ||
			    hakios_metal_cb_draw_place(cb, i, &pass, area) <= (int)i) {
				break;
			}
			hakios_metal_cb_pass_time(cb, pass, &kind, &w, &h, &draws, &pus);
			hm_prof_draw_add(w, h, name, dus, area);
		}
	}
	if (now - hm_prof.since >= 5.0) {
		const double secs = now - hm_prof.since;
		const double frames = hm_prof.presents != 0 ? hm_prof.presents : 1;

		hm_print("prof: %.1f s: %u frames (%.1f fps), %u buffers, %u passes (%.1f a frame), %u draws (%.1f a pass); GPU %.2f ms a frame "
			 "(buffers), %.2f (passes' stamps); GMEM %u passes, %u tiles; MB a frame cleared %.2f, loaded %.2f, stored %.2f; draw passes by "
			 "draws 0:%u 1:%u 2-4:%u 5-15:%u 16-55:%u 56+:%u; draw passes by load DontCare:%u Load:%u Clear:%u none:%u (%s)%s\n",
			 secs, hm_prof.presents, hm_prof.presents / secs, hm_prof.buffers, hm_prof.passes, hm_prof.passes / frames, hm_prof.draws,
			 hm_prof.passes != 0 ? (double)hm_prof.draws / hm_prof.passes : 0.0, hm_prof.gpuUs / 1e3 / frames, hm_prof.passUs / 1e3 / frames,
			 hm_prof.gmemPasses, hm_prof.tiles, hm_prof.cleared / 1e6 / frames, hm_prof.loaded / 1e6 / frames, hm_prof.stored / 1e6 / frames,
			 hm_prof.hist[0], hm_prof.hist[1], hm_prof.hist[2], hm_prof.hist[3], hm_prof.hist[4], hm_prof.hist[5],
			 hm_prof.loads[0], hm_prof.loads[1], hm_prof.loads[2], hm_prof.loads[3],
			 hm_presents_seen ? "frames = presentDrawable" : "frames = buffers that drew the display's IOSurface",
			 hm_prof.other != 0 ? " (passes past the classes not listed)" : "");
		{   /* (ws-hm-perf2) the sync line, a frame: the GPU's stamped time against the passes' stamps, what lies outside them */
			static unsigned long long was[4];
			unsigned long long sp[4];
			const unsigned long long *y = hm_prof.sync;
			const double gpuMs = y[0] / 1e6 / frames, preMs = y[1] / 1e6 / frames, passMs = hm_prof.passUs / 1e3 / frames;

			hakios_metal_pass_splits(sp);
			hm_print("prof: sync: a frame: GPU stamped %.2f ms (+%.2f in decode buffers), passes' stamps %.2f, outside them %.2f; "
				 "%.1f passes, %.1f WFIs, CCU flushes %.1f colour %.1f depth, %.1f cache flushes, %.1f waits, %.1f CACHE_INVALIDATE, "
				 "%.1f CCU invalidates, %.0f words (%.0f register), %.0f words by CP_MEMCPY, %.1f CP copies/fills (%.0f KB, unstamped), "
				 "%.1f blits; HM_PASS_FLUSH tails left out %.1f, barriers %.1f full %.1f invalidate, %.1f late tails; HM_PASS_STATIC heads carried %.1f; "
				 "HM_PASS_MERGE store joins %.1f; reopened on the same target (5 s) by a Clear %llu, state %llu, GMEM %llu, between %llu (%s)\n",
				 gpuMs, preMs, passMs, gpuMs - passMs, y[20] / frames, y[4] / frames, y[5] / frames, y[6] / frames, y[7] / frames,
				 y[8] / frames, y[9] / frames, y[10] / frames, y[2] / frames, y[3] / frames, y[11] / frames, y[18] / frames,
				 y[19] / 1e3 / frames, y[22] / frames, y[12] / frames, y[13] / frames, y[14] / frames, y[15] / frames, y[16] / frames,
				 y[17] / frames, sp[0] - was[0], sp[1] - was[1], sp[2] - was[2], sp[3] - was[3],
				 getenv("HM_PASS_FLUSH") ? getenv("HM_PASS_FLUSH") : "lazy");
			memcpy(was, sp, sizeof(was));
		}
		{   /* (ws-hm-int6) the UBWC pass classes a frame, and HM_BLEND_OPT's / the flag zeroing's running totals */
			static unsigned long long wasU[3];
			unsigned long long u[3];
			const unsigned long long *y = hm_prof.sync;

			hakios_metal_ubwc_counts(u);
			hm_print("prof: sync: ubwc: a frame: %.1f UBWC draw passes (%.1f in GMEM), %.1f UBWC 2D copies, %.1f compressed/linear switches; "
				 "blendsMadeWrites %llu (+%llu, HM_BLEND_OPT=%s), UBWC textures made %llu (+%llu), flag buffers zeroed %llu (+%llu); "
				 "HM_UBWC_RT=%s HM_GMEM=%s\n",
				 y[24] / frames, y[25] / frames, y[26] / frames, y[27] / frames, u[0], u[0] - wasU[0],
				 getenv("HM_BLEND_OPT") ? getenv("HM_BLEND_OPT") : "unset", u[1], u[1] - wasU[1], u[2], u[2] - wasU[2],
				 getenv("HM_UBWC_RT") ? getenv("HM_UBWC_RT") : "unset", getenv("HM_GMEM") ? getenv("HM_GMEM") : "unset");
			memcpy(wasU, u, sizeof(wasU));
		}
		{   /* (ws-hm-sbar) HM_FETCH_ORDER: framebuffer-fetch barriers in sysmem passes, and fetching draws ordered without one */
			static unsigned long long wasF[2];
			unsigned long long f[3];

			hakios_metal_fetch_counts(f);
			hm_print("prof: sync: fetch: a frame: %.1f fetch barriers, %.1f fetching draws ordered without one (HM_FETCH_ORDER=%llu)\n",
				 (f[0] - wasF[0]) / frames, (f[1] - wasF[1]) / frames, f[2]);
			wasF[0] = f[0], wasF[1] = f[1];
		}
		if (hm_prof_level() >= 3 && hm_prof_draws.n != 0) {   /* (ws-hm-sbar) HM_PROF=3's draw table */
			hm_print("prof: draws: %u timed alone (%.1f a frame), %.2f ms a frame; FETCH %.1f a frame, %.2f ms; status-bar rows of "
				 "display-sized passes: %.1f a frame (%.1f FETCH), %.2f ms%s\n",
				 hm_prof_draws.n, hm_prof_draws.n / frames, hm_prof_draws.us / 1e3 / frames, hm_prof_draws.fetchN / frames,
				 hm_prof_draws.fetchUs / 1e3 / frames, hm_prof_draws.sbN / frames, hm_prof_draws.sbFetchN / frames,
				 hm_prof_draws.sbUs / 1e3 / frames, hm_prof_draws.other != 0 ? " (draws past the table not listed)" : "");
			qsort(hm_prof_draws.d, HM_PROF_DRAWS, sizeof(hm_prof_draws.d[0]), hm_prof_draw_cmp);
			for (int k = 0; k < 24 && hm_prof_draws.d[k].count != 0; k++) {
				const double n = hm_prof_draws.d[k].count;

				hm_print("prof:   draw into %4ux%-4u %5.1f a frame, %7.1f us each, %6.2f ms a frame; ~%.0fx%.0f at y %.0f; %u in the bar rows | %s\n",
					 hm_prof_draws.d[k].w, hm_prof_draws.d[k].h, n / frames, hm_prof_draws.d[k].us / n, hm_prof_draws.d[k].us / 1e3 / frames,
					 hm_prof_draws.d[k].aw / n, hm_prof_draws.d[k].ah / n, hm_prof_draws.d[k].ay / n, hm_prof_draws.d[k].sb,
					 hm_prof_draws.d[k].name);
			}
			memset(&hm_prof_draws, 0, sizeof(hm_prof_draws));
		}
		qsort(hm_prof.cls, HM_PROF_CLASSES, sizeof(hm_prof.cls[0]), hm_prof_cmp);
		for (int k = 0; k < 12 && hm_prof.cls[k].count != 0; k++) {
			const struct hm_prof_class *x = &hm_prof.cls[k];
			char tiles[32] = "";

			if (x->gmem) {
				snprintf(tiles, sizeof(tiles), ", %.1f tiles", (double)x->tiles / x->count);
			}
			hm_print("prof:   %-8s fmt %3u %4ux%-4u %-6s %-6s %s/%s x%-5u %5.1f draws, %7.1f us a pass, %6.2f ms a frame; KB a pass cleared %.0f loaded %.0f stored %.0f%s\n",
				 x->kind < 4 ? kinds[x->kind] : "?", x->format, x->width, x->height, x->tiling ? "UBWC" : "linear",
				 x->load == 0 ? "dontcare" : x->load == 1 ? "load" : x->load == 2 ? "clear" : "-",
				 x->store == 0 ? "dontcare" : x->store == 1 ? "store" : "-",
				 x->gmem ? "GMEM" : "sysmem", x->count, (double)x->draws / x->count, x->us / x->count, x->us / 1e3 / frames,
				 x->cleared / 1e3 / x->count, x->loaded / 1e3 / x->count, x->stored / 1e3 / x->count,
				 tiles);
		}
		memset(&hm_prof.since, 0, sizeof(hm_prof) - offsetof(__typeof__(hm_prof), since));
	}
	pthread_mutex_unlock(&hm_prof.lock);
}

/* (redfin hmv2) HM_UBWC_RT=1: CoreAnimation's own render targets compressed too -- a private 2D, one-level, single-sampled
 * RGBA8 or BGRA8 (or sRGB) texture drawn into and sampled, never written by a kernel. 0 (default) linear, as before */
static BOOL
hm_ubwc_rt(MTLTextureDescriptor *descriptor, MTLTextureUsage usage)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_UBWC_RT");
		on = e != NULL && e[0] == '1';
		hm_print("HM_UBWC_RT=%s: render targets %s (ws-hm-ubwc2: HM_UBWC_MIN=%s HM_UBWC_ALIGN=%s HM_UBWC_FETCH=%s HM_UBWC_ZERO=%s HM_UBWC_LOG=%s)\n",
			 e != NULL ? e : "unset", on ? "compressed (UBWC) where served" : "linear",
			 getenv("HM_UBWC_MIN") ? getenv("HM_UBWC_MIN") : "unset(512x512)", getenv("HM_UBWC_ALIGN") ? getenv("HM_UBWC_ALIGN") : "unset(0)",
			 getenv("HM_UBWC_FETCH") ? getenv("HM_UBWC_FETCH") : "unset(1)", getenv("HM_UBWC_ZERO") ? getenv("HM_UBWC_ZERO") : "unset(2048)",
			 getenv("HM_UBWC_LOG") ? getenv("HM_UBWC_LOG") : "unset(0)");
	}
	return on && [descriptor textureType] == MTLTextureType2D && [descriptor mipmapLevelCount] == 1 && [descriptor sampleCount] == 1 &&
	       ([descriptor pixelFormat] == MTLPixelFormatRGBA8Unorm || [descriptor pixelFormat] == MTLPixelFormatBGRA8Unorm ||
		[descriptor pixelFormat] == MTLPixelFormatRGBA8Unorm_sRGB || [descriptor pixelFormat] == MTLPixelFormatBGRA8Unorm_sRGB) &&
	       (usage & MTLTextureUsageRenderTarget) != 0 && (usage & MTLTextureUsageShaderWrite) == 0 &&
	       (usage & ~(MTLTextureUsage)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) == 0 &&
	       [descriptor storageMode] == MTLStorageModePrivate &&
	       /* (ws-hm-ubwc2) the core's rule set: HM_UBWC_MIN (default 512x512), HM_UBWC_ALIGN, Mesa's 16-wide floor; a
	        * target it keeps linear is made linear here (the reason in HM_UBWC_LOG's line), never refused */
	       hakios_metal_ubwc_rt_ok((unsigned)[descriptor width], (unsigned)[descriptor height], (unsigned)usage);
}
'''

HMV3_HELPERS = r'''
/* (redfin hmv3) HM_GMEM in the environment (bench/m2shim.m passes it from /var/local/hmetal/ca/env): where a render pass
 * is drawn.
 *   0      every pass in sysmem, straight into memory (as metal2vulkan's TU_DEBUG=sysmem); the default unless the
 *          boot-args name metal_gmem=1 or metal_gmem_draws=
 *   1      upstream's rule: GMEM for a pass of at least metal_gmem_draws draws (56 unless the boot-arg says), boot-arg
 *          metal_gmem=1 and metal_gmem_off as before; the default when the boot-args name either
 *   N >= 2 GMEM for a pass of at least N draws
 *   ...,T  (any of the above) and only where the pass's plan has at most T tiles (bins)
 *   auto   (redfin ws-hm-ubwc) per pass, turnip's autotuner as far as it applies here: a pass whose like had 5 draws or
 *          fewer last time stays in sysmem (tu_autotune.cc fallback_use_bypass), a compressed (UBWC) target always does
 *          (gmemPlan: the tiles' loads and stores take linear targets), a plan of more than T tiles (default 8: the full
 *          1125x2436 BGRA8 screen is 24 bins of 576x208 and never qualifies) does; the rest is measured both ways by the
 *          passes' own GPU stamps and the cheaper kept (CommandQueue::preferGmem). Loads skipped for Clear/DontCare,
 *          stores for DontCare, binning as metal_gmem_off says, as for 1.
 * Why 0: hmt4 on the A620 (512 KiB GMEM) drew the 1125x2436 BGRA8 screen in 52 tiles. A GMEM pass of ~91 draws took
 * 15.2 ms (0.29 ms a tile; each pass loading ~11 MB and storing 11-22 MB, ~2.5-3 ms of bandwidth alone, and every
 * draw replayed in each tile it reaches), 62-66 % of the GPU's time; a full-screen sysmem pass of ~18 draws took 1.7 ms,
 * so ~91 draws in sysmem come to ~8-9 ms, and m2v (Turnip, sysmem) draws the same frames in 8.9 ms. */
#define HM_GMEM_AUTO 1000   /* (redfin ws-hm-ubwc) HM_GMEM=auto */
static int
hm_gmem_mode(unsigned *tiles)
{
	static int mode = -1;
	static unsigned limit;

	if (mode < 0) {
		const char *e = getenv("HM_GMEM"), *comma;
		char args[1024] = {0};
		size_t size = sizeof(args) - 1;
		const int bootarg = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
				    (strstr(args, "metal_gmem=1") != NULL || strstr(args, "metal_gmem_draws=") != NULL);
		int m;

		if (e == NULL || e[0] == '\0') {
			m = bootarg ? 1 : 0;
		} else if (strncmp(e, "auto", 4) == 0) {   /* (redfin ws-hm-ubwc) */
			m = HM_GMEM_AUTO;
			limit = 8;
			if ((comma = strchr(e, ',')) != NULL && atoi(comma + 1) > 0) limit = (unsigned)atoi(comma + 1);
		} else {
			m = atoi(e);
			if (m < 0) m = 0;
			if ((comma = strchr(e, ',')) != NULL && atoi(comma + 1) > 0) limit = (unsigned)atoi(comma + 1);
		}
		hm_print("HM_GMEM=%s: %s%s%u%s\n", e != NULL ? e : (bootarg ? "unset (boot-arg metal_gmem*: upstream's rule)" : "unset (default 0)"),
			 m == 0 ? "every pass in sysmem" : m == 1 ? "upstream's rule (metal_gmem_draws, 56 unless the boot-arg says)" :
			 m == HM_GMEM_AUTO ? "per pass (auto: sysmem for 5 draws or fewer and for UBWC targets, else the measured cheaper)" : "GMEM for passes of at least N draws",
			 limit != 0 ? ", at most " : "", limit, limit != 0 ? " tiles" : "");
		mode = m;
	}
	if (tiles != NULL) *tiles = limit;
	return mode;
}

/* (redfin hmv3) whether a render pass into TEXTURE draws the display: an IOSurface target as large as the largest seen */
static BOOL
hm_frame_target(id texture)
{
	static unsigned long long largest;
	unsigned long long area;

	if (texture == nil || ![texture respondsToSelector:@selector(iosurface)] || [texture iosurface] == NULL) {
		return NO;
	}
	area = (unsigned long long)[texture width] * [texture height];
	if (area < largest) {
		return NO;
	}
	largest = area;
	return YES;
}
'''

PICK_RESET_OLD = ('\tvsAir = hm_bitcode_for(vs, [descriptor label], YES);\n'
                  '\tfsAir = fs != nil ? hm_bitcode_for(fs, [descriptor label], NO) : nil;   /* nil: the built-in fragment function */\n')
PICK_RESET_NEW = '\thm_ca_pick_of[0][0] = hm_ca_pick_of[1][0] = \'\\0\';   /* (redfin hmvar) */\n' + PICK_RESET_OLD

LINK_OLD = ('\t\tprogram = hakios_metal_program_new_bitcode_constants([vsAir bytes], [vsAir length], vsConstants, vsCount, fsAir != nil ? [fsAir bytes] : NULL,\n'
            '\t\t\t\t\t\t\t\t     fsAir != nil ? [fsAir length] : 0,\n'
            '\t\t\t\t\t\t\t\t     fsConstants, fsCount, format);\n')
LINK_NEW = LINK_OLD + r'''		{   /* (redfin hmvar) a function compiled whole that does not link: once more with the label pick; then the pipeline's line */
			BOOL retried = NO;

			if (program == NULL && !hm_ca_force_nearest && hm_ca_pick_mode() != HM_CA_NEAREST &&
			    (strncmp(hm_ca_pick_of[0], "own", 3) == 0 || strncmp(hm_ca_pick_of[1], "own", 3) == 0)) {
				char first[512];
				NSData *v2, *f2;

				strlcpy(first, hakios_metal_error() != NULL ? hakios_metal_error() : "?", sizeof(first));
				hm_ca_log_pipeline(descriptor, vs, fs, vsConstants, vsCount, fsConstants, fsCount, NULL, first, NO);
				hm_ca_force_nearest = 1;
				v2 = hm_bitcode_for(vs, [descriptor label], YES);
				f2 = fs != nil ? hm_bitcode_for(fs, [descriptor label], NO) : nil;
				hm_ca_force_nearest = 0;
				if ([v2 length] != 0 && (fs == nil || [f2 length] != 0)) {
					program = hakios_metal_program_new_bitcode_constants([v2 bytes], [v2 length], vsConstants, vsCount, f2 != nil ? [f2 bytes] : NULL,
											     f2 != nil ? [f2 length] : 0, fsConstants, fsCount, format);
					if (program != NULL) {
						vsAir = v2;
						fsAir = f2;
					}
				}
				retried = YES;
			}
			hm_ca_log_pipeline(descriptor, vs, fs, vsConstants, vsCount, fsConstants, fsCount, program,
					   program == NULL ? hakios_metal_error() : NULL, retried);
		}
'''

DRAW_OLD = ('"buffer slots: %s\\n", hakios_metal_program_name([_pipeline hakiosProgram]), (unsigned long)primitive, (unsigned long)count,')
DRAW_NEW = ('"buffer slots: %s [%s]\\n", hakios_metal_program_name([_pipeline hakiosProgram]), (unsigned long)primitive, (unsigned long)count,')
DRAW_OLD2 = '_vsHighest, _fsHighest, hakios_metal_error());\n\t\thakios_metal_cb_refuse(cb, hakios_metal_error());'
DRAW_NEW2 = ('_vsHighest, _fsHighest, hakios_metal_error(), hm_ca_info_of([_pipeline hakiosProgram]));   /* (redfin hmvar) */\n'
             '\t\thakios_metal_cb_refuse(cb, hakios_metal_error());')

HMV4_HELPERS = r'''
/* (redfin hmv4) HM_TRACE=1: what the window server's hangs are made of, cheap enough to leave on.
 *   - every pipeline made: its ms, of them waiting for the compiler, stages compiled and read from the disk cache, the
 *     frame (command buffers committed so far) and the thread ("trace: pipeline ..."; a pipeline past 50 ms is said
 *     even without HM_TRACE)
 *   - every 10 s: the buffers' encode (made to committed), commit (the call) and GPU (committed to completed) ms,
 *     average and most; the pipelines made and their ms
 *   - a buffer past 100 ms from made to completed: "trace: HANG" and the last 20 events (buffers, pipelines, the
 *     specialiser, write-backs)
 *   - a buffer still open (made, not committed) past 100 ms: the thread that made it sampled every 25 ms, its frames
 *     ("trace: stall N ms: ...", at most 40 a stall): where the render thread is while nothing is committed
 *   - the ca_prof sampler (every thread every HM_CA_PROF_US, default 25000 us: top stacks every 400 looks, 10 s) when
 *     the boot-arg ca_prof= is absent; HM_CA_PROF_US=0 leaves it off */
static int
hm_trace_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_TRACE");
		on = e != NULL && atoi(e) != 0;
	}
	return on;
}

#define HM_TR_RING 64
static struct {
	pthread_mutex_t lock;
	unsigned next;
	struct { double t; char what[160]; } ev[HM_TR_RING];
	double since;
	unsigned buffers, hangs, pipelines;
	double encSum, encMax, comSum, comMax, gpuSum, gpuMax, pipeMs, pipeMax;
	unsigned long long committed;
} hm_tr = { PTHREAD_MUTEX_INITIALIZER };
static volatile double hm_tr_open_at;           /* the oldest buffer made and not yet committed (0: none) */
static volatile mach_port_t hm_tr_open_thread;   /* ...the thread that made it */

static void hm_tr_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
hm_tr_event(const char *fmt, ...)
{
	va_list ap;

	if (!hm_trace_on()) return;
	pthread_mutex_lock(&hm_tr.lock);
	{
		typeof(hm_tr.ev[0]) *e = &hm_tr.ev[hm_tr.next++ % HM_TR_RING];

		e->t = hm_now();
		va_start(ap, fmt);
		vsnprintf(e->what, sizeof(e->what), fmt, ap);
		va_end(ap);
	}
	pthread_mutex_unlock(&hm_tr.lock);
}

static void
hm_tr_dump(const char *what, double ms)
{
	typeof(hm_tr.ev[0]) copy[20];
	unsigned n = 0, next;
	const double now = hm_now();

	pthread_mutex_lock(&hm_tr.lock);
	next = hm_tr.next;
	for (unsigned i = next >= 20 ? next - 20 : 0; i < next; i++) copy[n++] = hm_tr.ev[i % HM_TR_RING];
	hm_tr.hangs++;
	pthread_mutex_unlock(&hm_tr.lock);
	hm_print("trace: HANG %s %.0f ms (frame %llu); the last %u events:\n", what, ms, hm_tr.committed, n);
	for (unsigned i = 0; i < n; i++) hm_print("trace:   %7.1f ms ago: %s\n", (now - copy[i].t) * 1e3, copy[i].what);
}

static unsigned long long hm_tr_frame(void) { return hm_tr.committed; }

/* (redfin hmv8) surfaces: textures over an IOSurface alive now; copies of a surface's rows (-hakiosShadow:'s refreshes) and
 * of them the ones made while the seed had not moved (HM_SHADOW_FRESH); the last buffer's numbers (display-off trace) */
static volatile long hm_surf_live, hm_surf_live_max, hm_shadow_copies, hm_shadow_forced;
/* (redfin hmv10) commits held (sent later from the queue's serial queue: an event waited for, or one held before them)
 * and -waitUntilScheduled calls that waited for such a send (HM_SCHED_WAIT) */
static volatile long hm_held_commits, hm_sched_waits;
static double hm_tr_last_done, hm_tr_last_enc, hm_tr_last_gpu;
/* (redfin hmv8) HM_SHADOW_FRESH: 1 (default) a read-only copy of a surface's rows made again once a command buffer whatever
 * its seed; 0 only when the seed moved (as before) */
static int
hm_shadow_fresh(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_SHADOW_FRESH");

		on = e == NULL || e[0] != '0';
	}
	return on;
}

/* a buffer's numbers at its completion (seconds) */
static void
hm_tr_buffer(double encode, double commit, double gpu, double total)
{
	char line[700] = "", line2[800] = "";

	hm_tr_last_done = hm_now(), hm_tr_last_enc = encode, hm_tr_last_gpu = gpu;   /* (redfin hmv8) */

	if (!hm_trace_on()) return;
	hm_tr_event("buffer done: encode %.1f commit %.1f gpu %.1f ms", encode * 1e3, commit * 1e3, gpu * 1e3);
	pthread_mutex_lock(&hm_tr.lock);
	{
		const double now = hm_now();

		if (hm_tr.since == 0) hm_tr.since = now;
		hm_tr.buffers++;
		hm_tr.encSum += encode, hm_tr.comSum += commit, hm_tr.gpuSum += gpu;
		if (encode > hm_tr.encMax) hm_tr.encMax = encode;
		if (commit > hm_tr.comMax) hm_tr.comMax = commit;
		if (gpu > hm_tr.gpuMax) hm_tr.gpuMax = gpu;
		if (now - hm_tr.since >= 10.0) {
			const double b = hm_tr.buffers ? hm_tr.buffers : 1;
			static unsigned long long was[4];
			unsigned long long sp[4];
			int at;

			hakios_metal_pass_splits(sp);

			snprintf(line, sizeof(line), "trace: %.1f s: %u buffers (%.1f a s); encode %.2f ms avg %.1f most; commit %.2f avg %.1f most; "
				 "GPU %.2f avg %.1f most; %u pipelines made in %.0f ms (%.0f most); %u hangs past 100 ms",
				 now - hm_tr.since, hm_tr.buffers, hm_tr.buffers / (now - hm_tr.since), hm_tr.encSum / b * 1e3, hm_tr.encMax * 1e3,
				 hm_tr.comSum / b * 1e3, hm_tr.comMax * 1e3, hm_tr.gpuSum / b * 1e3, hm_tr.gpuMax * 1e3, hm_tr.pipelines, hm_tr.pipeMs,
				 hm_tr.pipeMax, hm_tr.hangs);
			at = (int)strlen(line);
			snprintf(line + at, sizeof(line) - (size_t)at, "; passes reopened on the same target: %llu by a Clear, %llu by state, %llu by GMEM, "
				 "%llu after a copy/dispatch/clear between", sp[0] - was[0], sp[1] - was[1], sp[2] - was[2], sp[3] - was[3]);
			memcpy(was, sp, sizeof(was));
			{   /* (redfin hmv8) the surfaces' line: live textures over IOSurfaces, copies a second, XR decodes and ring barriers */
				static unsigned long long xwas[10];
				static long cwas, fwas, hwas, wwas;
				unsigned long long xr[10];
				const double secs = now - hm_tr.since > 0 ? now - hm_tr.since : 1;
				const long copies = hm_shadow_copies, forced = hm_shadow_forced;

				hakios_metal_xr_stats(xr);
				snprintf(line2, sizeof(line2), "trace: surfaces: %ld textures over IOSurfaces live (%ld most); row copies %.1f a s (%ld made "
					 "with the seed unmoved: HM_SHADOW_FRESH); XR decodes %.1f a s (%llu of surfaces); ring barriers %llu, %.1f ms avg, "
					 "%llu timed out (HM_XR_SYNC)", hm_surf_live, hm_surf_live_max, (copies - cwas) / secs, forced - fwas,
					 (xr[0] - xwas[0]) / secs, xr[1] - xwas[1], xr[2] - xwas[2],
					 xr[2] - xwas[2] ? (double)(xr[3] - xwas[3]) / 1e3 / (double)(xr[2] - xwas[2]) : 0.0, xr[4] - xwas[4]);
				{   /* (redfin hmv10) */
					const int at2 = (int)strlen(line2);
					const long held = hm_held_commits, waits = hm_sched_waits;

					snprintf(line2 + at2, sizeof(line2) - (size_t)at2, "; XR decodes at commit %llu (HM_XR_LATE), checked %llu, changed after "
						 "decode %llu (HM_XR_CHECK); write-backs at completion %llu, %.1f ms avg (HM_XR_WB_SYNC); held commits %ld, "
						 "scheduled waits %ld (HM_SCHED_WAIT)", xr[5] - xwas[5], xr[6] - xwas[6], xr[7] - xwas[7], xr[8] - xwas[8],
						 xr[8] - xwas[8] ? (double)(xr[9] - xwas[9]) / 1e3 / (double)(xr[8] - xwas[8]) : 0.0, held - hwas, waits - wwas);
					hwas = held, wwas = waits;
				}
				{   /* (redfin hmv11) HM_XR_GPU */
					static unsigned long long gwas[6];
					unsigned long long g[6];
					const int at3 = (int)strlen(line2);

					hakios_metal_xr_gpu_stats(g);
					snprintf(line2 + at3, sizeof(line2) - (size_t)at3, "; XR GPU decodes %llu in %llu buffers, %.2f ms avg to encode, "
						 "%llu fell back to the processor, verified %llu (%llu differ) (HM_XR_GPU)", g[0] - gwas[0], g[1] - gwas[1],
						 g[1] - gwas[1] ? (double)(g[5] - gwas[5]) / 1e3 / (double)(g[1] - gwas[1]) : 0.0, g[2] - gwas[2], g[3] - gwas[3],
						 g[4] - gwas[4]);
					memcpy(gwas, g, sizeof(gwas));
				}
				memcpy(xwas, xr, sizeof(xwas));
				cwas = copies, fwas = forced;
			}
			hm_tr.since = now;
			hm_tr.buffers = hm_tr.pipelines = hm_tr.hangs = 0;
			hm_tr.encSum = hm_tr.encMax = hm_tr.comSum = hm_tr.comMax = hm_tr.gpuSum = hm_tr.gpuMax = hm_tr.pipeMs = hm_tr.pipeMax = 0;
		}
	}
	pthread_mutex_unlock(&hm_tr.lock);
	if (line[0]) hm_print("%s\n", line);
	if (line2[0]) hm_print("%s\n", line2);
	if (total > 0.1) {
		char what[160];

		snprintf(what, sizeof(what), "a buffer made to completed (encode %.0f, commit %.0f, GPU %.0f ms):", encode * 1e3, commit * 1e3, gpu * 1e3);
		hm_tr_dump(what, total * 1e3);
	}
}

/* a pipeline made: its ms and what of them the compiler's (the thread's compile stats before and after) */
static void
hm_tr_pipeline(NSString *label, double ms, double waitMs, double compileMs, unsigned compiled, unsigned cached)
{
	char name[64] = "";
	const BOOL isMain = pthread_main_np() != 0;

	pthread_getname_np(pthread_self(), name, sizeof(name));
	if (hm_trace_on()) {
		pthread_mutex_lock(&hm_tr.lock);
		hm_tr.pipelines++;
		hm_tr.pipeMs += ms;
		if (ms > hm_tr.pipeMax) hm_tr.pipeMax = ms;
		pthread_mutex_unlock(&hm_tr.lock);
		hm_tr_event("pipeline %s: %.0f ms (wait %.0f, compile %.0f; %u compiled, %u cached) on %s", label != nil ? [label UTF8String] : "(no label)",
			    ms, waitMs, compileMs, compiled, cached, name[0] ? name : isMain ? "main" : "(unnamed)");
	}
	if (hm_trace_on() || ms > 50) {
		hm_print("trace: pipeline %s: %.0f ms (waited %.0f for the compiler, compiling %.0f; %u stages compiled, %u from the disk cache), "
			 "frame %llu, thread %s\n", label != nil ? [label UTF8String] : "(no label)", ms, waitMs, compileMs, compiled, cached,
			 hm_tr_frame(), name[0] ? name : isMain ? "main" : "(unnamed)");
	}
}

/* the stall sampler: the thread with a buffer open past 100 ms, its frames every 25 ms */
static void *
hm_tr_watch(__unused void *argument)
{
	double stallAt = 0;
	unsigned said = 0;

	pthread_setname_np("hakios.trace.watch");
	for (;;) {
		usleep(25000);
		const double opened = hm_tr_open_at;
		const mach_port_t t = hm_tr_open_thread;

		if (opened == 0 || t == MACH_PORT_NULL || hm_now() - opened < 0.1) {
			continue;
		}
		if (opened != stallAt) {
			stallAt = opened;
			said = 0;
		}
		if (said++ >= 40) continue;
		{
			arm_thread_state64_t st;
			mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
			uint64_t frames[12] = { 0 };
			unsigned depth = 0;
			char line[1400];
			int at;

			if (thread_suspend(t) != KERN_SUCCESS) continue;
			if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
				uint64_t sp = arm_thread_state64_get_sp(st), fp = arm_thread_state64_get_fp(st), top = 0;
				pthread_t pt = pthread_from_mach_thread_np(t);

				if (pt != NULL) top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pt);
				frames[depth++] = arm_thread_state64_get_pc(st);
				frames[depth++] = arm_thread_state64_get_lr(st);
				while (depth < 12 && top != 0 && fp >= sp && fp + 16 <= top && (fp & 15) == 0) {
					const uint64_t *record = (const uint64_t *)(uintptr_t)fp;

					frames[depth++] = record[1];
					if (record[0] <= fp) break;
					fp = record[0];
				}
			}
			thread_resume(t);
			at = snprintf(line, sizeof(line), "trace: stall %.0f ms (frame %llu):", (hm_now() - opened) * 1e3, hm_tr_frame());
			for (unsigned d = 0; d < depth && at < (int)sizeof(line) - 100; d++) {
				Dl_info where = { 0 };
				const char *image = "?", *slash;
				uint64_t a = frames[d] & 0x7fffffffffull, offset = a;   /* (the pointer's authentication bits off) */

				if (a == 0) continue;
				if (dladdr((void *)(uintptr_t)a, &where) != 0 && where.dli_fname != NULL) {
					image = (slash = strrchr(where.dli_fname, '/')) != NULL ? slash + 1 : where.dli_fname;
					offset -= (uint64_t)(uintptr_t)where.dli_fbase;
				}
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %s+0x%llx(%s)", image, (unsigned long long)offset,
					       where.dli_sname != NULL ? where.dli_sname : "?");
			}
			hm_print("%s\n", line);
		}
	}
	return NULL;
}

/* a buffer made (the thread that made it is the one the sampler looks at while it stays open) */
static void
hm_tr_made(void)
{
	static dispatch_once_t once;

	if (!hm_trace_on()) return;
	dispatch_once(&once, ^{
		pthread_t watcher;
		pthread_create(&watcher, NULL, hm_tr_watch, NULL);
		hm_print("trace: HM_TRACE=1: pipelines, 10 s buffer summaries, hangs past 100 ms with their last 20 events, stalls sampled\n");
	});
	if (hm_tr_open_at == 0) {
		hm_tr_open_thread = pthread_mach_thread_np(pthread_self());
		hm_tr_open_at = hm_now();
	}
}

static void
hm_tr_committed(double encode)
{
	if (!hm_trace_on()) return;
	hm_tr_open_at = 0;
	__atomic_add_fetch(&hm_tr.committed, 1, __ATOMIC_RELAXED);
	if (encode > 0.05) hm_tr_event("buffer committed after %.0f ms of encoding", encode * 1e3);
}

/* (redfin hmv4) HM_PREWARM=1 (default): every pipeline QuartzCore's library names made once on a thread of its own, at
 * background priority, HM_PREWARM_DELAY s (default 30) after the library opened, one at a time with a pause between, its
 * compiles waiting while the render thread wants the compiler (hakios_metal_compile_background, HM_COMPILE_PRIO). Each
 * stage lands in the disk cache (HM_CACHE_DIR), which outlives the process, so a pipeline CoreAnimation first asks for
 * later -- a rotation, an app's first launch -- is a read of the cache, not a compile of up to a second on the render
 * thread. The pipelines themselves are released at once. A run that found everything cached already says so in one line. */
static int
hm_prewarm_on(void)
{
	const char *e = getenv("HM_PREWARM");
	return e == NULL || e[0] == 0 || atoi(e) != 0;
}

static void
hm_prewarm_start(id library, NSString *path)
{
	static dispatch_once_t once;

	if (library == nil || !hm_prewarm_on() || [path rangeOfString:@"QuartzCore.framework"].location == NSNotFound ||
	    ![library respondsToSelector:@selector(pipelineNames)]) {
		return;
	}
	dispatch_once(&once, ^{
		const char *d = getenv("HM_PREWARM_DELAY");
		const double delay = d != NULL && d[0] ? atof(d) : 30.0;
		id held = [library retain];

		hm_ca_load();
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
			SEL make = sel_registerName("newRenderPipelineStateWithName:options:reflection:error:");
			NSArray *names = [[library pipelineNames] copy];
			const double began = hm_now();
			double w0 = 0, c0 = 0, w1 = 0, c1 = 0;
			unsigned h0 = 0, m0 = 0, h1 = 0, m1 = 0, made = 0, failed = 0;

			pthread_setname_np("hakios.prewarm");
			hakios_metal_compile_background(1);
			hakios_metal_compile_stats(&w0, &c0, &h0, &m0);
			if (![held respondsToSelector:make]) {
				hm_print("prewarm: the pipeline library answers no -newRenderPipelineStateWithName:...; nothing made\n");
				[names release];
				[held release];
				return;
			}
			hm_print("prewarm: %lu pipelines of %s, one at a time in the background\n", (unsigned long)[names count], [path fileSystemRepresentation]);
			for (NSString *name in names) {
				@autoreleasepool {
					NSError *error = nil;
					unsigned mm = 0;
					id state;

					hakios_metal_compile_stats(NULL, NULL, NULL, &mm);
					state = ((id (*)(id, SEL, NSString *, NSUInteger, id *, NSError **))objc_msgSend)(held, make, name, 0, NULL, &error);
					if (state != nil) made++; else failed++;
					[state release];
					{
						unsigned after = 0;
						hakios_metal_compile_stats(NULL, NULL, NULL, &after);
						if (after != mm) usleep(20000);   /* (a compile happened: the render thread's turn for a moment) */
					}
					if ((made + failed) % 100 == 0) {
						hakios_metal_compile_stats(&w1, &c1, &h1, &m1);
						hm_print("prewarm: %u of %lu made (%u refused), %u stages compiled, %u from the cache, %.0f s\n", made + failed,
							 (unsigned long)[names count], failed, m1 - m0, h1 - h0, hm_now() - began);
					}
				}
			}
			hakios_metal_compile_stats(&w1, &c1, &h1, &m1);
			hm_print("prewarm: done: %u pipelines made, %u refused; %u stages compiled (%.0f s compiling, %.0f s waiting for the render "
				 "thread), %u from the disk cache; %.0f s in all\n", made, failed, m1 - m0, (c1 - c0) / 1e3, (w1 - w0) / 1e3, h1 - h0,
				 hm_now() - began);
			[names release];
			[held release];
		});
	});
}

/* (redfin hmv4) HM_XR10=0 (default): no extended-range 10-bit formats, as SweetMetal2 answers (NativeBridgeMetalFrontend.mm:
 * supportsExtendedXR10Formats 0, and 550/551 made BGRA8Unorm(_sRGB)). Asked nothing, _MTLDevice's answer let CoreAnimation
 * draw its snapshots and wide-colour layers into BGR10_XR targets (refused here until HM_XR_RT) and ask for
 * MTLPixelFormatRGB10A8_2P_XR10 (550) textures, which no table here has: the rotation's and the app switcher's snapshots
 * then showed what their surfaces held before, an old landscape frame or black. HM_XR10=1: the device's own answer. */
static int
hm_xr10_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_XR10");
		on = e != NULL && atoi(e) != 0;
	}
	return on;
}

static MTLPixelFormat
hm_xr10_format(MTLPixelFormat f)
{
	if (hm_xr10_on() || (f != (MTLPixelFormat)550 && f != (MTLPixelFormat)551)) return f;
	{
		static int said;
		if (!said++) hm_print("HM_XR10=0: pixel format %lu (RGB10A8_2P_XR10) served as BGRA8Unorm%s, as SweetMetal2 does\n", (unsigned long)f, f == 551 ? "_sRGB" : "");
	}
	return f == (MTLPixelFormat)550 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatBGRA8Unorm_sRGB;
}

static MTLTextureDescriptor *
hm_xr10_descriptor(MTLTextureDescriptor *d)
{
	if (d == nil || hm_xr10_format([d pixelFormat]) == [d pixelFormat]) return d;
	d = [[d copy] autorelease];
	[d setPixelFormat:hm_xr10_format([d pixelFormat])];
	return d;
}

/* (redfin hmv4) HM_ROTFIX=1 (default): a turned framebuffer (a landscape texture over the portrait display surface, drawn
 * into a texture of its own and turned into the surface at commit) loads what the SURFACE holds when the surface was
 * written since that texture last turned into it. Before, its own texture kept the last landscape frame it drew: after
 * a turn to portrait (passes straight into the surface) and back, or with another texture over the same surface, a pass
 * that loads (CoreAnimation redraws only what changed) put that old landscape frame back -- the stale landscape UI and
 * the Notes app flashing. Each IOSurface drawn into counts its writes here (hm_surface_gen). */
static int
hm_rotfix_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_ROTFIX");
		on = e == NULL || e[0] == 0 || atoi(e) != 0;
	}
	return on;
}

#define HM_SURF_GENS 512
static struct { uint32_t id, gen; } hm_surf_gens[HM_SURF_GENS];
static pthread_mutex_t hm_surf_gen_lock = PTHREAD_MUTEX_INITIALIZER;

/* the surface's write count, after BUMP more (a pass drew into it) */
static uint32_t
hm_surface_gen(uint32_t id, uint32_t bump)
{
	uint32_t gen = 0;

	if (id == 0) return 0;
	pthread_mutex_lock(&hm_surf_gen_lock);
	for (unsigned i = id % HM_SURF_GENS, n = 0; n < HM_SURF_GENS; i = (i + 1) % HM_SURF_GENS, n++) {
		if (hm_surf_gens[i].id == id || hm_surf_gens[i].id == 0) {
			hm_surf_gens[i].id = id;
			gen = hm_surf_gens[i].gen += bump;
			break;
		}
		if (n == HM_SURF_GENS - 1) {   /* full: start again (a refresh too many is the cost) */
			memset(hm_surf_gens, 0, sizeof(hm_surf_gens));
			hm_surf_gens[id % HM_SURF_GENS].id = id;
			gen = hm_surf_gens[id % HM_SURF_GENS].gen = bump + 1;
		}
	}
	pthread_mutex_unlock(&hm_surf_gen_lock);
	return gen;
}
'''

HMV7_HELPERS = r'''/* (redfin hmv7) Uber shaders that may not run as they are (bench/plugin.py). hmt7 (var6): every one of backboardd's GPU
 * resets (2 s with the CP inside the buffer, SP/HLSQ busy, VFD idle: a fragment shader that never ends) was a buffer
 * drawing fixed_vert_lph_gen+fixed_frag_lph_cpf, the 32064-word uber shader (the pipelinelib's frag.A2Xghfc), which
 * walks its configuration buffer's bytes at run time (62 loops whose trip counts are bytes of that buffer). The
 * specialised program (hakios_metal_program_specialize: the buffer's bytes folded in) has no such loop.
 *   HM_SPEC=2           every uber shader drawn only through the program specialised for the draw's configuration,
 *                       compiled on the spot the first time a configuration is seen (the draw waits, ~1 s), never as it is
 *   HM_DENY=a,b,...     pipelines whose CA line (draw label, picks, variant labels) holds one of the words: made with the
 *                       other pick (exact -> own, own -> nearest, nearest -> own), and an uber shader among them drawn
 *                       as HM_SPEC=2 draws it
 *   HM_HANG_BLAME=1     (default) a program in HM_HANG_BLAME_AFTER (default 2) hung buffers (the core's tally: only
 *                       programs of HM_HANG_BLAME_WORDS fragment words or more, default 1024) is from then on drawn as
 *                       HM_SPEC=2 draws it, or left out if it cannot be specialised; 0 = the tally is only logged
 * A draw that would have to run such a program as it is (no configuration to fold, the specialisation failed) is left
 * out and said (the first 20, then every 100th). */
static int
hm_v7_spec_wait_all(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_SPEC");

		on = e != NULL && atoi(e) == 2;
	}
	return on;
}

static int
hm_v7_blame_after(void)   /* 0: HM_HANG_BLAME=0 */
{
	static int after = -1;

	if (after < 0) {
		const char *e = getenv("HM_HANG_BLAME"), *a = getenv("HM_HANG_BLAME_AFTER");

		after = e != NULL && e[0] != '\0' && atoi(e) == 0 ? 0 : (a != NULL && atoi(a) > 0 ? atoi(a) : 2);
		hm_print("HM_HANG_BLAME=%s: %s; HM_SPEC=%s; HM_DENY=%s\n", e != NULL ? e : "unset (1)",
			 after ? "a program in that many hung buffers is drawn only specialised from then on" : "hangs only counted",
			 getenv("HM_SPEC") != NULL ? getenv("HM_SPEC") : "unset", getenv("HM_DENY") != NULL ? getenv("HM_DENY") : "unset");
		if (after) hm_print("HM_HANG_BLAME_AFTER=%d\n", after);
	}
	return after;
}

/* YES when TEXT holds one of HM_DENY's comma-separated words */
static BOOL
hm_v7_denied_text(const char *text)
{
	const char *e = getenv("HM_DENY");

	if (e == NULL || e[0] == '\0' || text == NULL) return NO;
	while (*e != '\0') {
		const char *end = strchr(e, ',');
		const size_t n = end != NULL ? (size_t)(end - e) : strlen(e);
		char word[128];

		if (n > 0 && n < sizeof(word)) {
			memcpy(word, e, n);
			word[n] = '\0';
			if (strstr(text, word) != NULL) return YES;
		}
		if (end == NULL) break;
		e = end + 1;
	}
	return NO;
}

/* how PROGRAM (a pipeline's own) may be drawn: 0 as it is, 1 only through its specialisations; said once per program */
static int
hm_v7_wait(hakios_metal_program *program)
{
	static NSMutableSet *said;
	const char *why = NULL;
	char text[96];
	const int after = hm_v7_blame_after();
	const unsigned hangs = after ? hakios_metal_program_hangs(program) : 0;

	if (hm_v7_spec_wait_all()) {
		why = "HM_SPEC=2";
	} else if (after && hangs >= (unsigned)after) {
		snprintf(text, sizeof(text), "HM_HANG_BLAME: in %u hung buffers", hangs);
		why = text;
	} else if (getenv("HM_DENY") != NULL && hm_v7_denied_text(hm_ca_info_of(program))) {
		why = "HM_DENY";
	}
	if (why == NULL) return 0;
	@synchronized ([NSMutableArray class]) {
		NSValue *key = [NSValue valueWithPointer:program];

		if (said == nil) said = [[NSMutableSet alloc] init];
		if (![said containsObject:key]) {
			[said addObject:key];
			hm_print("blame: %s (%p, %s): %s; from now on drawn only through programs specialised for its configuration\n",
				 hakios_metal_program_name(program), (void *)program, hm_ca_info_of(program), why);
		}
	}
	return 1;
}

static void
hm_v7_left_out(hakios_metal_program *program, const char *why)
{
	static unsigned n;

	if (n++ < 20 || n % 100 == 0) {
		hm_print("blame: a draw of %s left out (%u so far): %s\n", hakios_metal_program_name(program), n, why);
	}
}

'''

HMV5_HELPERS = r'''/* (redfin hmv5) HM_FAILLOG=1 (default): failed buffers and left-out work said (bench/plugin.py) */
static int
hm_faillog_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_FAILLOG");
		on = e == NULL || e[0] == 0 || atoi(e) != 0;
	}
	return on;
}

static unsigned hm_failed_total, hm_skipped_total;

static void
hm_fail_note(hakios_metal_cb *cb)
{
	const char *last = NULL;
	unsigned skipped;

	if (!hm_faillog_on() || cb == NULL) return;
	if (hakios_metal_cb_status(cb) == HAKIOS_METAL_ERROR) {
		const unsigned n = __atomic_add_fetch(&hm_failed_total, 1, __ATOMIC_RELAXED);

		if (n <= 20 || n % 100 == 0) {
			hm_print("trace: buffer failed (#%u, frame %llu): %s\n", n, hm_tr_frame(), hakios_metal_cb_error(cb));
		}
	}
	skipped = hakios_metal_cb_skipped(cb, &last);
	if (skipped != 0) {
		const unsigned n = __atomic_add_fetch(&hm_skipped_total, 1, __ATOMIC_RELAXED);

		if (n <= 20 || n % 100 == 0) {
			hm_print("trace: buffer left %u things out (#%u, frame %llu); the last: %s\n", skipped, n, hm_tr_frame(),
				 last != NULL ? last : "?");
		}
	}
}
'''

# (hmv7) uber shaders that may not run as they are: HM_SPEC=2 / HM_DENY / HM_HANG_BLAME (HMV7_HELPERS)
HMV7_FOR_START = '- (hakios_metal_program *)hakiosProgramFor:(hakios_metal_buffer *const *)buffers offsets:(const uint64_t *)offsets count:(unsigned)count\n{\n'
HMV7_FOR_END = '\t\t\t[self release];\n\t\t});\n\t}\n\treturn found;\n}\n'
HMV7_FOR_NEW = r'''- (hakios_metal_program *)hakiosProgramFor:(hakios_metal_buffer *const *)buffers offsets:(const uint64_t *)offsets count:(unsigned)count
{
	static dispatch_queue_t compiling;
	static dispatch_once_t once;

	/* (redfin hmv7) the fragment is kept whatever HM_SPEC says (hakiosKeepFragment), so that a blamed or denied uber
	 * shader can still be specialised; HM_SPEC=0 alone draws the pipeline's own program, as before */
	if (_specialised == nil) return _program;
	const int wait = hm_v7_wait(_program);
	if (!wait && hm_spec_off()) return _program;
	if (_specBinding < 0 || (unsigned)_specBinding >= count || buffers[_specBinding] == NULL) {
		if (wait) hm_v7_left_out(_program, "no configuration buffer bound to fold");
		return wait ? NULL : _program;
	}
	const uint8_t *bytes = (const uint8_t *)hakios_metal_buffer_contents(buffers[_specBinding]);
	const uint64_t length = hakios_metal_buffer_length(buffers[_specBinding]), at = offsets[_specBinding];
	if (bytes == NULL || at > length || length - at < _specLength) {
		if (wait) hm_v7_left_out(_program, "its configuration buffer is shorter than the program reads");
		return wait ? NULL : _program;
	}
	NSData *key = [NSData dataWithBytes:bytes + at length:_specLength];
	hakios_metal_program *found = wait ? NULL : _program;
	BOOL start = NO, mine = NO;

	@synchronized(self) {
		id have = [_specialised objectForKey:key];

		if ([have isKindOfClass:[NSValue class]]) {   /* (a failed one is a string, never an NSValue) */
			found = (hakios_metal_program *)[have pointerValue];
		} else if (have == nil && wait) {
			[_specialised setObject:[NSNull null] forKey:key];   /* (no limit: each one is a configuration CA draws) */
			mine = YES;
		} else if (have == nil && [_specialised count] < 64) {
			[_specialised setObject:[NSNull null] forKey:key];
			start = YES;
		} else if (wait && [have isKindOfClass:[NSString class]]) {
			found = NULL;   /* its specialisation failed before */
		}
	}
	if (wait && found == NULL && !mine) {
		/* being specialised on the queue (asked for before the blame): wait for it, up to 5 s */
		for (int i = 0; i < 2500 && found == NULL; i++) {
			id have;

			@synchronized(self) {
				have = [_specialised objectForKey:key];
			}
			if ([have isKindOfClass:[NSValue class]]) found = (hakios_metal_program *)[have pointerValue];
			else if (![have isKindOfClass:[NSNull class]]) break;
			else usleep(2000);
		}
		if (found == NULL) hm_v7_left_out(_program, "its specialisation for this configuration failed or is not done in 5 s");
		return found;
	}
	if (mine) {
		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		hakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,
									 (unsigned)_specBinding, [key bytes], [key length]);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		hm_tr_event("specialise (waited for) %s: %.0f ms", hakios_metal_program_name(_program),
			    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);   /* (redfin hmv8) */
		hm_print("specialise (waited for, hmv7): %s for %u bytes of its buffer %d: %s%u instruction words, not %u, in %.0f ms\n",
			 hakios_metal_program_name(_program), _specLength, _specBinding, p != NULL ? "" : "FAILED: ",
			 p != NULL ? hakios_metal_program_stage_words(p, 1) : 0, hakios_metal_program_stage_words(_program, 1),
			 (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
		if (p == NULL) hm_print("specialise: %s\n", hakios_metal_error());
		@synchronized(self) {
			[_specialised setObject:p != NULL ? (id)[NSValue valueWithPointer:p] : (id)@"failed" forKey:key];
		}
		if (p == NULL) hm_v7_left_out(_program, "its specialisation for this configuration failed");
		return p;
	}
	if (start) {
		dispatch_once(&once, ^{
			compiling = dispatch_queue_create("hakios.metal.specialise", DISPATCH_QUEUE_SERIAL);
		});
		[self retain];
		dispatch_async(compiling, ^{
			struct timespec t0, t1;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			hakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,
										 (unsigned)_specBinding, [key bytes], [key length]);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (p == NULL) {
				hm_print("specialise: %s for its buffer %d: %s; its own program goes on\n", hakios_metal_program_name(_program), _specBinding,
					 hakios_metal_error());
			} else if (hm_fps_on()) {
				hm_print("specialise: %s for %u bytes of its buffer %d: %u instruction words, not %u, in %.0f ms\n", hakios_metal_program_name(_program),
					 _specLength, _specBinding, hakios_metal_program_stage_words(p, 1), hakios_metal_program_stage_words(_program, 1),
					 (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
			}
			@synchronized(self) {
				[_specialised setObject:p != NULL ? (id)[NSValue valueWithPointer:p] : (id)@"failed" forKey:key];
			}
			[self release];
		});
	}
	return found;
}
'''


EDITS = [
    ('#define HM_CACHE_DIR  "/private/var/db/hakios-metal-cache"',
     '#define HM_CACHE_DIR  "/var/local/hmetal/cache"     /* (redfin hmbench) */'),
    ('#define HM_CACHE_SEED "/redwood/share/hakios-metal-cache"',
     '#define HM_CACHE_SEED "/var/local/hmetal/cache-seed"     /* (redfin hmbench) */'),
    ('if (!all && (name == NULL || strcmp(name, "backboardd") != 0)) {',
     'if (!all && getenv("HAKIOS_METAL_ALL") == NULL && (name == NULL || strcmp(name, "backboardd") != 0)) {   /* (redfin hmbench) */'),
    ('\t\thm_print_fd = fd;\n\t\t(void)!write(fd, line, strlen(line));\n',
     '\t\thm_print_fd = fd;\n\t\t(void)!write(fd, line, strlen(line));\n'
     '\t\t{   /* (redfin H3) a copy into $HAKIOS_METAL_LOG; (redfin hm-int2) past 16 MB it becomes NAME.1, a new one starts */\n'
     '\t\t\tstatic int log_fd = -2;\n'
     '\t\t\tstatic long long log_bytes;\n'
     '\t\t\tstatic char *log_path;\n'
     '\t\t\tstatic pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;\n'
     '\t\t\tpthread_mutex_lock(&log_lock);\n'
     '\t\t\tif (log_fd == -2) {\n'
     '\t\t\t\tconst char *p = getenv("HAKIOS_METAL_LOG");\n'
     '\t\t\t\tstruct stat ls;\n'
     '\t\t\t\tlog_path = p ? strdup(p) : NULL;\n'
     '\t\t\t\tlog_fd = log_path ? open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0666) : -1;\n'
     '\t\t\t\tlog_bytes = log_fd >= 0 && fstat(log_fd, &ls) == 0 ? (long long)ls.st_size : 0;\n'
     '\t\t\t}\n'
     '\t\t\tif (log_fd >= 0 && log_bytes > 16LL * 1024 * 1024) {\n'
     '\t\t\t\tchar old[1100];\n'
     '\t\t\t\tsnprintf(old, sizeof(old), "%s.1", log_path);\n'
     '\t\t\t\trename(log_path, old);\n'
     '\t\t\t\tclose(log_fd);\n'
     '\t\t\t\tlog_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0666);\n'
     '\t\t\t\tlog_bytes = 0;\n'
     '\t\t\t}\n'
     '\t\t\tif (log_fd >= 0) { (void)!write(log_fd, line, strlen(line)); log_bytes += (long long)strlen(line); }\n'
     '\t\t\tpthread_mutex_unlock(&log_lock);\n'
     '\t\t}\n'),
    # (hmvar) the pick's helpers, just before hm_bitcode_for
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     CA_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # (hmvar) hm_bitcode_for's choice for fixed_*, from its first test of the name to its return
    (BITCODE_FOR_START, BITCODE_FOR_END, BITCODE_FOR_NEW),
    (PICK_RESET_OLD, PICK_RESET_NEW),
    (LINK_OLD, LINK_NEW),
    (DRAW_OLD, DRAW_NEW),
    # (hmvar) the UBWC words beside the device's line, as hakios-metal took them at open
    ('\t_name = [[NSString alloc] initWithFormat:@"Adreno (hakios-metal, chip %#llx)", (unsigned long long)hakios_metal_gpu_id(_gpu)];\n',
     '\t_name = [[NSString alloc] initWithFormat:@"Adreno (hakios-metal, chip %#llx)", (unsigned long long)hakios_metal_gpu_id(_gpu)];\n'
     '\t{   /* (redfin hmvar) */\n'
     '\t\tunsigned hbb = 0, swz = 0, mt = 0;\n'
     '\t\tconst int said = hakios_metal_ubwc(_gpu, &hbb, &swz, &mt);\n'
     '\t\thm_print("UBWC: highest bank bit %u, swizzle levels %#x, macrotile %u (%s)\\n", hbb, swz, mt, said ? "the kernel\'s" : "the chip table\'s: the kernel did not say");\n'
     '\t}\n'),
    (DRAW_OLD2, DRAW_NEW2),
    # (hmv2) the notice hook, the profile and the render-target switch's helpers, before hm_bitcode_for as hmvar's
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     HMV2_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # (hmv3) HM_GMEM and the frame mark's helpers, after hmv2's
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     HMV3_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # (hmv3) HM_GMEM: sysmem / GMEM per command buffer, read once
    ('\t\thakios_metal_cb_set_gmem(_cb, hm_gmem_on() || hm_gmem_draws() != 0);\n\t\thakios_metal_cb_set_gmem_min_draws(_cb, hm_gmem_draws());\n',
     '\t\t{   /* (redfin hmv3) HM_GMEM: 0 sysmem (default), 1 upstream\'s rule, N GMEM from N draws; ,T at most T tiles */\n'
     '\t\t\tunsigned tiles = 0;\n'
     '\t\t\tconst int mode = hm_gmem_mode(&tiles);\n\n'
     '\t\t\thakios_metal_cb_set_gmem(_cb, mode == 1 ? (hm_gmem_on() || hm_gmem_draws() != 0) : mode >= 2);\n'
     '\t\t\thakios_metal_cb_set_gmem_min_draws(_cb, mode == 1 ? hm_gmem_draws() : mode >= 2 && mode != HM_GMEM_AUTO ? (unsigned)mode : 0);\n'
     '\t\t\thakios_metal_cb_set_gmem_max_tiles(_cb, tiles);\n'
     '\t\t\thakios_metal_cb_set_gmem_auto(_cb, mode == HM_GMEM_AUTO);   /* (redfin ws-hm-ubwc) */\n'
     '\t\t}\n'),
    # (hmv3) a frame: the command buffer that drew the display's IOSurface (the window server presents no drawable)
    ('\tNSMutableArray *_turned;                 /* HakiosMetalTexture: turned framebuffers drawn here, copied into their surfaces at commit */\n',
     '\tNSMutableArray *_turned;                 /* HakiosMetalTexture: turned framebuffers drawn here, copied into their surfaces at commit */\n'
     '@public\n\tBOOL _hmFrame;                           /* (redfin hmv3, public: hm_completed is a C function) a render pass here drew the display\'s IOSurface (hm_frame_target) */\n'),
    ('\t\thm_shadow_targets(descriptor, (id<MTLCommandBuffer>)self);\n\n\t\t[encoder hakiosTakeResolves:descriptor];',
     '\t\thm_shadow_targets(descriptor, (id<MTLCommandBuffer>)self);\n'
     '\t\tif (hm_frame_target(texture)) _hmFrame = YES;   /* (redfin hmv3) */\n\n\t\t[encoder hakiosTakeResolves:descriptor];'),
    ('\thm_fps_note(NO, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9, hm_now() - [self GPUStartTime], cb);\n',
     '\tif (self->_hmFrame && !hm_presents_seen) {\n\t\thm_fps_note(YES, 0, 0, NULL);   /* (redfin hmv3) a frame drawn, nothing presented */\n\t}\n'
     '\thm_fps_note(NO, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9, hm_now() - [self GPUStartTime], cb);\n'),
    ('\t\t[drawable release];\n\t\thm_fps_note(YES, 0, 0, NULL);\n',
     '\t\t[drawable release];\n\t\thm_presents_seen = 1;   /* (redfin hmv3) */\n\t\thm_fps_note(YES, 0, 0, NULL);\n'),
    # (hmv2) the core's notices into this log, set where the device is made
    ('\t{   /* (redfin hmvar) */\n\t\tunsigned hbb = 0, swz = 0, mt = 0;\n',
     '\thakios_metal_set_notice(hm_notice);   /* (redfin hmv2) */\n\t{   /* (redfin hmvar) */\n\t\tunsigned hbb = 0, swz = 0, mt = 0;\n'),
    # (hmv2) HM_PROF >= 1 turns on what boot-arg metal_fps does (the GPU's stamps a pass included)
    ('\t\ton = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps") != NULL;\n',
     '\t\ton = hm_prof_level() >= 1 || (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_fps") != NULL);   /* (redfin hmv2) HM_PROF */\n'),
    # (hmv2) HM_PROF=2's per-pass summary, fed from each completed buffer and each present
    ('\tif (present) {\n\t\thm_fps.presents++;\n\t} else {\n',
     '\thm_prof_note(cb, gpu, present);   /* (redfin hmv2) HM_PROF=2 */\n\tif (present) {\n\t\thm_fps.presents++;\n\t} else {\n'),
    # (hmv2) HM_UBWC_RT: CoreAnimation's private render targets made compressed
    ('\t} else if (hm_compress(descriptor, usage)) {\n',
     '\t} else if (hm_compress(descriptor, usage) || hm_ubwc_rt(descriptor, usage)) {   /* (redfin hmv2) HM_UBWC_RT */\n'),
    # (hmv4) HM_TRACE, HM_PREWARM: helpers after hmv3's
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     HMV4_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # (hmv4) a pipeline's ms and its compiler's share
    ("\thm_ca_pick_of[0][0] = hm_ca_pick_of[1][0] = '\\0';   /* (redfin hmvar) */\n",
     "\tconst double hmTrBegan = hm_now();   /* (redfin hmv4) HM_TRACE: the pipeline's time, and the compiler's in it */\n"
     "\tdouble hmTrW0 = 0, hmTrC0 = 0;\n\tunsigned hmTrH0 = 0, hmTrM0 = 0;\n\n"
     "\thakios_metal_compile_stats(&hmTrW0, &hmTrC0, &hmTrH0, &hmTrM0);\n"
     "\thm_ca_pick_of[0][0] = hm_ca_pick_of[1][0] = '\\0';   /* (redfin hmvar) */\n"),
    ('\t\t\thm_ca_log_pipeline(descriptor, vs, fs, vsConstants, vsCount, fsConstants, fsCount, program,\n'
     '\t\t\t\t\t   program == NULL ? hakios_metal_error() : NULL, retried);\n',
     '\t\t\thm_ca_log_pipeline(descriptor, vs, fs, vsConstants, vsCount, fsConstants, fsCount, program,\n'
     '\t\t\t\t\t   program == NULL ? hakios_metal_error() : NULL, retried);\n'
     '\t\t\t{   /* (redfin hmv4) */\n'
     '\t\t\t\tdouble w = 0, c = 0;\n\t\t\t\tunsigned h = 0, m = 0;\n\n'
     '\t\t\t\thakios_metal_compile_stats(&w, &c, &h, &m);\n'
     '\t\t\t\thm_tr_pipeline([descriptor label], (hm_now() - hmTrBegan) * 1e3, w - hmTrW0, c - hmTrC0, m - hmTrM0, h - hmTrH0);\n'
     '\t\t\t}\n'),
    # (hmv4) the specialiser's compiles are background ones (they wait while the render thread wants the compiler), traced
    ('\t\tdispatch_async(compiling, ^{\n\t\t\tstruct timespec t0, t1;\n\n',
     '\t\tdispatch_async(compiling, ^{\n\t\t\tstruct timespec t0, t1;\n\n'
     '\t\t\thakios_metal_compile_background(1);   /* (redfin hmv4) */\n'),
    ('\t\t\t} else if (hm_fps_on()) {\n\t\t\t\thm_print("specialise: %s for %u bytes',
     '\t\t\t} else if (hm_fps_on() || hm_trace_on()) {   /* (redfin hmv4) */\n'
     '\t\t\t\thm_tr_event("specialise %s: %.0f ms", hakios_metal_program_name(_program), (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);\n'
     '\t\t\t\thm_print("specialise: %s for %u bytes'),
    # (hmv4) the buffer's life for HM_TRACE: made, committed (and the commit call's time), completed
    ('\t_createdAt = hm_now();\n', '\t_createdAt = hm_now();\n\thm_tr_made();   /* (redfin hmv4) */\n'),
    ('\t_committed = YES;\n\t_committedAt = hm_now();\n',
     '\t_committed = YES;\n\t_committedAt = hm_now();\n\thm_tr_committed(_committedAt - _createdAt);   /* (redfin hmv4) */\n'),
    ('\t\thakios_metal_cb_commit(_cb, hm_completed, self);\n\t\thm_fps_commit(hm_now() - began);\n',
     '\t\thakios_metal_cb_commit(_cb, hm_completed, self);\n\t\tself->_hmCommitSecs = hm_now() - began;   /* (redfin hmv4) */\n'
     '\t\thm_fps_commit(hm_now() - began);\n'),
    ('@public\n\tBOOL _hmFrame;', '@public\n\tdouble _hmCommitSecs;                    /* (redfin hmv4) HM_TRACE: the commit call\'s time */\n\tBOOL _hmFrame;'),
    ('\tif (self->_hmFrame && !hm_presents_seen) {\n',
     '\thm_tr_buffer([self GPUStartTime] - [self hakiosCreatedAt], self->_hmCommitSecs, hm_now() - [self GPUStartTime],\n'
     '\t\t     hm_now() - [self hakiosCreatedAt]);   /* (redfin hmv4) HM_TRACE */\n'
     '\tif (self->_hmFrame && !hm_presents_seen) {\n'),
    # (hmv4) the ca_prof sampler by HM_TRACE when the boot-arg does not ask for it (HM_CA_PROF_US, default 25000)
    ('\t\tif (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (at = strstr(args, "ca_prof=")) == NULL) return;\n'
     '\t\tca_prof_us = (unsigned)atoi(at + 8);\n',
     '\t\tif (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (at = strstr(args, "ca_prof=")) != NULL) {\n'
     '\t\t\tca_prof_us = (unsigned)atoi(at + 8);\n'
     '\t\t} else if (hm_trace_on()) {   /* (redfin hmv4) */\n'
     '\t\t\tconst char *us = getenv("HM_CA_PROF_US");\n'
     '\t\t\tca_prof_us = us != NULL && us[0] ? (unsigned)atoi(us) : 25000;\n'
     '\t\t} else {\n\t\t\treturn;\n\t\t}\n'),
    # (hmv4) HM_XR10=0: no XR10 formats offered; 550/551 as BGRA8 (own-memory textures, views, pipelines)
    ('- (uint64_t)registryID\n{\n',
     '/* (redfin hmv4) HM_XR10 (see hm_xr10_on) */\n- (BOOL)supportsExtendedXR10Formats\n{\n\treturn hm_xr10_on() ? YES : NO;\n}\n\n'
     '- (uint64_t)registryID\n{\n'),
    ('- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor\n{\n\thakios_metal_texture *texture;\n\tMTLTextureUsage usage;\n\tMTLTextureSwizzleChannels swizzle;\n\n',
     '- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor\n{\n\thakios_metal_texture *texture;\n\tMTLTextureUsage usage;\n\tMTLTextureSwizzleChannels swizzle;\n\n'
     '\tdescriptor = hm_xr10_descriptor(descriptor);   /* (redfin hmv4) */\n'),
    ('\t\t\t slices:(NSRange)slices asked:(SEL)cmd\n{\n\tHakiosMetalTexture *view;\n',
     '\t\t\t slices:(NSRange)slices asked:(SEL)cmd\n{\n\tHakiosMetalTexture *view;\n\n\tpixelFormat = hm_xr10_format(pixelFormat);   /* (redfin hmv4) */\n'),
    ('\tformat = (unsigned)[[[descriptor colorAttachments] objectAtIndexedSubscript:0] pixelFormat];\n',
     '\tformat = (unsigned)hm_xr10_format([[[descriptor colorAttachments] objectAtIndexedSubscript:0] pixelFormat]);   /* (redfin hmv4) */\n'),
    # (hmv4) HM_ROTFIX: a turned framebuffer loads the surface when the surface was written since it last turned into it
    ('\thakios_metal_texture *_turnedSurface; /* a framebuffer with a rotation: the surface itself, upright, that _texture is turned into */\n',
     '\thakios_metal_texture *_turnedSurface; /* a framebuffer with a rotation: the surface itself, upright, that _texture is turned into */\n'
     '\tuint32_t _turnGen;                  /* (redfin hmv4) the surface\'s write count when _texture last matched it (HM_ROTFIX) */\n'
     '\tBOOL _turnGenKnown;\n'),
    ('- (void)hakiosSetTurned:(hakios_metal_texture *)surface rotation:(unsigned)rotation;\n',
     '- (void)hakiosSetTurned:(hakios_metal_texture *)surface rotation:(unsigned)rotation;\n'
     '- (void)hakiosRefreshTurned:(hakios_metal_cb *)cb;   /* (redfin hmv4) */\n'),
    ('/* the framebuffer as drawn, turned into the surface the display scans out */\n- (void)hakiosTurnInto:(hakios_metal_cb *)cb\n{\n'
     '\tif (_turnedSurface != NULL && hakios_metal_cb_blit_rotated(cb, _texture, _turnedSurface, _turn) != 0) {\n'
     '\t\tHM_ONCE("refused: a turned framebuffer\'s copy into its surface: %s\\n", hakios_metal_error());\n\t}\n}\n',
     '/* the framebuffer as drawn, turned into the surface the display scans out */\n- (void)hakiosTurnInto:(hakios_metal_cb *)cb\n{\n'
     '\tif (_turnedSurface != NULL && hakios_metal_cb_blit_rotated(cb, _texture, _turnedSurface, _turn) != 0) {\n'
     '\t\tHM_ONCE("refused: a turned framebuffer\'s copy into its surface: %s\\n", hakios_metal_error());\n\t}\n'
     '\tif (_turnedSurface != NULL && _iosurface != NULL) {   /* (redfin hmv4) the surface now holds this texture\'s frame */\n'
     '\t\t_turnGen = hm_surface_gen(IOSurfaceGetID(_iosurface), 1);\n\t\t_turnGenKnown = YES;\n\t}\n}\n\n'
     '/* (redfin hmv4) HM_ROTFIX: before a pass that loads, the surface turned back into this texture if something else wrote it */\n'
     '- (void)hakiosRefreshTurned:(hakios_metal_cb *)cb\n{\n'
     '\tif (_turnedSurface == NULL || _iosurface == NULL || !hm_rotfix_on()) return;\n'
     '\tconst uint32_t gen = hm_surface_gen(IOSurfaceGetID(_iosurface), 0);\n'
     '\tif (_turnGenKnown && gen == _turnGen) return;\n'
     '\tif (hakios_metal_cb_blit_rotated(cb, _turnedSurface, _texture, _turn == 2 ? 2 : 4 - _turn) != 0) {\n'
     '\t\tHM_ONCE("refused: a surface turned back into its turned framebuffer (HM_ROTFIX): %s\\n", hakios_metal_error());\n'
     '\t\treturn;\n\t}\n'
     '\thm_tr_event("turned framebuffer over surface %u loaded from the surface (write %u, had %u)", IOSurfaceGetID(_iosurface), gen, _turnGen);\n'
     '\t_turnGen = gen;\n\t_turnGenKnown = YES;\n}\n'),
    ('\t\tif (t != nil && [t isKindOfClass:[HakiosMetalTexture class]] && [(HakiosMetalTexture *)t hakiosIsTurned]) {\n'
     '\t\t\t[(HakiosMetalCommandBuffer *)cb hakiosTurnAtCommit:t];\n\t\t\tcontinue;\n\t\t}\n',
     '\t\tif (t != nil && [t isKindOfClass:[HakiosMetalTexture class]] && [(HakiosMetalTexture *)t hakiosIsTurned]) {\n'
     '\t\t\tif ([[[d colorAttachments] objectAtIndexedSubscript:i] loadAction] == MTLLoadActionLoad) {   /* (redfin hmv4) HM_ROTFIX */\n'
     '\t\t\t\t[(HakiosMetalTexture *)t hakiosRefreshTurned:[(HakiosMetalCommandBuffer *)cb hakiosCommandBuffer]];\n\t\t\t}\n'
     '\t\t\t[(HakiosMetalCommandBuffer *)cb hakiosTurnAtCommit:t];\n\t\t\tcontinue;\n\t\t}\n'
     '\t\tif (t != nil && [t isKindOfClass:[HakiosMetalTexture class]] && [(HakiosMetalTexture *)t iosurface] != NULL) {\n'
     '\t\t\thm_surface_gen(IOSurfaceGetID([(HakiosMetalTexture *)t iosurface]), 1);   /* (redfin hmv4) a pass straight into a surface */\n\t\t}\n'),
    # (hmv4) HM_TRACE: every texture made over an IOSurface (rotation, WebKit, switcher snapshots: what arrives)
    ('\t\t\t surface != NULL ? IOSurfaceGetID(surface) : 0);\n\t}\n\tMTLTextureUsage usage;\n\thakios_metal_texture *texture;\n\tHakiosMetalTexture *t;\n',
     '\t\t\t surface != NULL ? IOSurfaceGetID(surface) : 0);\n\t}\n'
     '\tdescriptor = hm_xr10_descriptor(descriptor);   /* (redfin hmv4) HM_XR10=0: 550/551 (a b3a8 surface) as BGRA8 over its first plane */\n'
     '\tif (hm_trace_on() && surface != NULL && descriptor != nil) {   /* (redfin hmv4) */\n'
     '\t\tconst uint32_t fcc = IOSurfaceGetPixelFormat(surface);\n'
     '\t\tconst NSUInteger rot = [descriptor respondsToSelector:sel_getUid("rotation")] ?\n'
     '\t\t\t((NSUInteger (*)(id, SEL))objc_msgSend)(descriptor, sel_getUid("rotation")) : 0;\n'
     '\t\thm_print("trace: surface %u %zux%zu rows %zu \'%c%c%c%c\' planes %zu seed %u alloc %zu -> texture %lux%lu format %lu usage %#lx plane %lu rotation %lu (frame %llu)\\n",\n'
     '\t\t\t IOSurfaceGetID(surface), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetBytesPerRow(surface),\n'
     '\t\t\t (char)(fcc >> 24), (char)(fcc >> 16), (char)(fcc >> 8), (char)fcc, IOSurfaceGetPlaneCount(surface), IOSurfaceGetSeed(surface),\n'
     '\t\t\t IOSurfaceGetAllocSize(surface), (unsigned long)[descriptor width], (unsigned long)[descriptor height],\n'
     '\t\t\t (unsigned long)[descriptor pixelFormat], (unsigned long)[descriptor usage], (unsigned long)plane, (unsigned long)rot, hm_tr_frame());\n'
     '\t}\n'
     '\tMTLTextureUsage usage;\n\thakios_metal_texture *texture;\n\tHakiosMetalTexture *t;\n'),
    # (hmv4) the YCbCr copy by tables (the per-pixel float formula, ~30 ms a full-screen picture), and HM_TRACE's copy line
    ('\t\t\tconst float y = (yl[i] - yo) * ys, cb = (cl[c] - 128.0f) * cs, cr = (cl[c + 1] - 128.0f) * cs;\n'
     '\t\t\tconst float r = y + 2.0f * (1.0f - kr) * cr, b = y + 2.0f * (1.0f - kb) * cb;\n'
     '\t\t\tconst float g = (y - kr * r - kb * b) / kg;\n',
     '\t\t\t/* (redfin hmv4) the same formula, linear in each input: by tables of 256 (built once a copy) */\n'
     '\t\t\tconst float y = tY[yl[i]], r = y + tRcr[cl[c + 1]], b = y + tBcb[cl[c]], g = y - tGcb[cl[c]] - tGcr[cl[c + 1]];\n'
     '\t\t\t(void)kg;\n'),
    ('\tuint8_t *rows = malloc(w * h * 4);\n\n\tif (rows == NULL || luma == NULL || chroma == NULL || cw == 0 || ch == 0) {\n',
     '\tuint8_t *rows = malloc(w * h * 4);\n'
     '\tfloat tY[256], tRcr[256], tBcb[256], tGcb[256], tGcr[256];   /* (redfin hmv4) */\n\n'
     '\tfor (int v = 0; v < 256; v++) {\n'
     '\t\tconst float c = (v - 128.0f) * cs;\n\n'
     '\t\ttY[v] = (v - yo) * ys;\n'
     '\t\ttRcr[v] = 2.0f * (1.0f - kr) * c;\n'
     '\t\ttBcb[v] = 2.0f * (1.0f - kb) * c;\n'
     '\t\ttGcr[v] = kr * 2.0f * (1.0f - kr) * c / kg;\n'
     '\t\ttGcb[v] = kb * 2.0f * (1.0f - kb) * c / kg;\n'
     '\t}\n'
     '\tif (rows == NULL || luma == NULL || chroma == NULL || cw == 0 || ch == 0) {\n'),
    ('\thm_fps_shadow(YES, hm_now() - began);\n',
     '\thm_fps_shadow(YES, hm_now() - began);\n'
     '\tif (hm_trace_on() || hm_now() - began > 0.008) {   /* (redfin hmv4) a surface copied in on the processor */\n'
     '\t\tconst double ms = (hm_now() - began) * 1e3;\n\n'
     '\t\thm_tr_event("surface %u copied in (%s, %zux%zu rows %zu): %.1f ms", IOSurfaceGetID(_iosurface), _ycbcr ? "YCbCr" : "rows",\n'
     '\t\t\t    IOSurfaceGetWidth(_iosurface), IOSurfaceGetHeight(_iosurface), IOSurfaceGetBytesPerRow(_iosurface), ms);\n'
     '\t\tif (ms > 8) hm_print("trace: surface %u copied in on the processor (%s, %zux%zu, rows %zu, seed %u): %.1f ms\\n", IOSurfaceGetID(_iosurface),\n'
     '\t\t\t\t      _ycbcr ? "YCbCr converted" : "rows", IOSurfaceGetWidth(_iosurface), IOSurfaceGetHeight(_iosurface),\n'
     '\t\t\t\t      IOSurfaceGetBytesPerRow(_iosurface), seed, ms);\n'
     '\t}\n'),
    # (hmv5) HM_SPEC=0 (default): no uber-shader specialisation (hakios' background recompile of fixed_frag_lph_cpf for
    # its constant buffer's bytes). hmt6: all 19 GPU hangs (kernel "v2 fence wait: GPU hang or fault ... the job of
    # pid <backboardd> hung", 2 s each, reset) came 0.2-2.9 s after a "specialise: fixed_vert_lph_gen+fixed_frag_lph_cpf"
    # line; the one at 13850 took MobileSafari's and WebKit.GPU's SweetMetal2 devices down (WaitFence timeouts,
    # "device refused: DeviceLost"): the black web view. HM_SPEC=1 = upstream's specialiser again.
    ('\t\toff = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_nospec") != NULL;\n\t}\n\treturn off != 0;\n',
     '\t\toff = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "metal_nospec") != NULL;\n'
     '\t\t{   /* (redfin hmv5) HM_SPEC: 0 (default) off, 1 on */\n'
     '\t\t\tconst char *e = getenv("HM_SPEC");\n\n'
     '\t\t\tif (e == NULL || e[0] == 0 || atoi(e) == 0) off = 1;\n'
     '\t\t\thm_print("uber-shader specialisation %s (HM_SPEC=%s)\\n", off ? "off" : "on", e != NULL ? e : "unset");\n'
     '\t\t}\n\t}\n\treturn off != 0;\n'),
    # (hmv5) HM_FAILLOG=1 (default): a command buffer that ends in error, or that left work out (refused draws, copies,
    # passes skipped), is said: first 20 of each, then every 100th. Such a buffer reaches the GPU with nothing (error)
    # or less than asked, and nothing said it before (the error was only built if CoreAnimation asked). hmt6: after the
    # turn back to portrait CA swapped 93-255 frames per 5 s while backboardd's GPU submits fell to 9 and 0 (kernel
    # SWEET GPU / SCANOUT gate lines): frames without their GPU work, the frozen screen.
    ('\thm_fps_note(NO, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9, hm_now() - [self GPUStartTime], cb);\n\t/* hakios-metal\'s queue thread',
     '\thm_fail_note(cb);   /* (redfin hmv5) HM_FAILLOG */\n'
     '\thm_fps_note(NO, (double)hakios_metal_cb_gpu_nanoseconds(cb) / 1e9, hm_now() - [self GPUStartTime], cb);\n\t/* hakios-metal\'s queue thread'),
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     HMV5_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # (hmv4) HM_PREWARM: QuartzCore's library's pipelines made once in the background, into the disk cache
    ('\tif (error != NULL) *error = library != nil ? nil : (second != nil ? second : first);\n\treturn library;\n',
     '\tif (error != NULL) *error = library != nil ? nil : (second != nil ? second : first);\n'
     '\thm_prewarm_start(library, path);   /* (redfin hmv4) */\n\treturn library;\n'),
    # (hmv7) HM_SPEC=2 / HM_DENY / HM_HANG_BLAME (HMV7_HELPERS): helpers before hmv5's, the draw's program picked by them
    ('/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE',
     HMV7_HELPERS + '/* The module to compile for FUNCTION (of the pipeline labelled PIPELINE'),
    # the fragment kept for every uber shader (HM_SPEC=0 too): a blamed one is specialised on demand
    ('\t_specBinding = -1;\n\tif (hm_spec_off() || hakios_metal_program_stage_words(_program, 1) < 4096) return;\n',
     '\t_specBinding = -1;\n\tif (hakios_metal_program_stage_words(_program, 1) < 4096) return;   /* (redfin hmv7) kept with HM_SPEC=0 too */\n'),
    (HMV7_FOR_START, HMV7_FOR_END, HMV7_FOR_NEW),
    ('static void hm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));\n',
     '#include <sys/time.h>   /* (redfin hmv8) HM_LOG_TIME */\n#include <time.h>\n'
     'static void hm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));\n'),
    # (hmv8) HM_LOG_TIME=1 (default): the time of day in every line's prefix, inside the brackets (pull2.sh's sed keeps working)
    ('\tn = snprintf(line, sizeof(line), "%s: ", who);\n',
     '\t{   /* (redfin hmv8) HM_LOG_TIME */\n'
     '\t\tstatic int stamp = -1;\n'
     '\t\tif (stamp < 0) { const char *e = getenv("HM_LOG_TIME"); stamp = e == NULL || e[0] != \'0\'; }\n'
     '\t\tif (stamp) {\n'
     '\t\t\tstruct timeval tv;\n\t\t\tstruct tm tmv;\n'
     '\t\t\tgettimeofday(&tv, NULL);\n\t\t\tlocaltime_r(&tv.tv_sec, &tmv);\n'
     '\t\t\tn = snprintf(line, sizeof(line), "%.*s %02d:%02d:%02d.%03d]: ", (int)strlen(who) - 1, who, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,\n'
     '\t\t\t\t     (int)(tv.tv_usec / 1000));\n'
     '\t\t} else {\n\t\t\tn = snprintf(line, sizeof(line), "%s: ", who);\n\t\t}\n\t}\n'),
    # (hmv8) the shadow copy's frame, for HM_SHADOW_FRESH
    ('\tuint32_t _shadowSeed;\n',
     '\tuint32_t _shadowSeed;\n\tunsigned long long _shadowFrame;   /* (redfin hmv8) hm_tr_frame() at its last refresh */\n'),
    # (hmv8) HM_SHADOW_FRESH=1 (default): a read-only copy of a surface's rows is made again once a command buffer even when
    # the seed has not moved: another process's GPU writes (SweetMetal2 in WebKit's GPU process) move no seed (the hmv4
    # note), and the copy kept their old rows -- or none -- for as long as nothing else wrote the surface
    ('\tif (!_shadow || _iosurface == NULL || (seed = IOSurfaceGetSeed(_iosurface)) == _shadowSeed) {\n\t\treturn;\n\t}\n',
     '\tif (!_shadow || _iosurface == NULL) {\n\t\treturn;\n\t}\n'
     '\tBOOL forced = NO;\n'
     '\tif ((seed = IOSurfaceGetSeed(_iosurface)) == _shadowSeed) {   /* (redfin hmv8) HM_SHADOW_FRESH */\n'
     '\t\tif (!hm_shadow_fresh() || _ycbcr != 0 || (_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0 ||\n'
     '\t\t    _shadowFrame == hm_tr_frame()) {\n\t\t\treturn;\n\t\t}\n'
     '\t\tforced = YES;\n\t}\n'
     '\t_shadowFrame = hm_tr_frame();\n'
     '\t__sync_fetch_and_add(&hm_shadow_copies, 1);\n'
     '\tif (forced) __sync_fetch_and_add(&hm_shadow_forced, 1);\n'),
    # (hmv8) live textures over IOSurfaces (HM_TRACE's surfaces line)
    ('\tif (surface != NULL) {\n\t\tCFRetain(surface);\n\t}\n\tif (_iosurface != NULL) {\n\t\tCFRelease(_iosurface);\n\t}\n\t_iosurface = surface;\n',
     '\tif (surface != NULL) {\n\t\tCFRetain(surface);\n\t}\n\tif (_iosurface != NULL) {\n\t\tCFRelease(_iosurface);\n\t}\n'
     '\tif ((_iosurface == NULL) != (surface == NULL)) {   /* (redfin hmv8) */\n'
     '\t\tconst long live = __sync_add_and_fetch(&hm_surf_live, surface != NULL ? 1 : -1);\n'
     '\t\tif (live > hm_surf_live_max) hm_surf_live_max = live;\n\t}\n'
     '\t_iosurface = surface;\n'),
    # (hmv8) display-off trace: a texture over a display-sized surface let go (CA tears its display down on power-off), with
    # the time since the last buffer completed and that buffer's numbers; and the live count
    ('\tif (_iosurface != NULL) {\n\t\tCFRelease(_iosurface);\n\t}\n\tfree(_shadowBase);\n',
     '\tif (_iosurface != NULL) {\n'
     '\t\t__sync_fetch_and_sub(&hm_surf_live, 1);   /* (redfin hmv8) */\n'
     '\t\tstatic int said;\n'
     '\t\tif (hm_trace_on() && IOSurfaceGetWidth(_iosurface) * IOSurfaceGetHeight(_iosurface) >= 2000000 && _parent == nil &&\n'
     '\t\t    (_usage & MTLTextureUsageRenderTarget) != 0 && said++ < 400) {\n'
     '\t\t\thm_print("trace: display-sized surface %u (%zux%zu) let go (frame %llu, %.0f ms after the last buffer completed: encode %.1f GPU %.1f ms; "\n'
     '\t\t\t\t "%ld textures over surfaces left)\\n", IOSurfaceGetID(_iosurface), IOSurfaceGetWidth(_iosurface), IOSurfaceGetHeight(_iosurface),\n'
     '\t\t\t\t hm_tr_frame(), (hm_now() - hm_tr_last_done) * 1e3, hm_tr_last_enc * 1e3, hm_tr_last_gpu * 1e3, hm_surf_live);\n'
     '\t\t\thm_tr_event("display-sized surface %u let go", IOSurfaceGetID(_iosurface));\n'
     '\t\t}\n'
     '\t\tCFRelease(_iosurface);\n\t}\n\tfree(_shadowBase);\n'),
    ('\thakios_metal_program *program = [_pipeline hakiosProgramFor:fs offsets:_fsOffsets count:_fsHighest];\n',
     '\thakios_metal_program *program = [_pipeline hakiosProgramFor:fs offsets:_fsOffsets count:_fsHighest];\n'
     '\tif (program == NULL) {   /* (redfin hmv7) an uber shader that may not run as it is, and no specialisation for this draw */\n'
     '\t\treturn;\n'
     '\t}\n'),
    # (hmv10) HM_SCHED_WAIT: held commits counted; -waitUntilScheduled waits for a held commit's send; and
    # -commitAndWaitUntilSubmitted (QuartzCore 16.7 calls it; SweetMetal2 has it) = -commit + -waitUntilScheduled
    ('\t\t_sent = dispatch_semaphore_create(0);\n',
     '\t\t_sent = dispatch_semaphore_create(0);\n\t\t__sync_fetch_and_add(&hm_held_commits, 1);   /* (redfin hmv10) */\n'),
    ('\t/* committed: hakios_metal_cb_commit returned with it scheduled, so there is nothing left to wait for */\n}\n',
     '\t/* committed: hakios_metal_cb_commit returned with it scheduled, so there is nothing left to wait for -- (redfin hmv10)\n'
     '\t * unless it was held: then until it has been sent (HM_SCHED_WAIT) */\n'
     '\tif (_committed && _sent != NULL) {   /* always since hm-int2 (was HM_SCHED_WAIT) */\n'
     '\t\t__sync_fetch_and_add(&hm_sched_waits, 1);\n'
     '\t\tdispatch_semaphore_wait(_sent, DISPATCH_TIME_FOREVER);\n'
     '\t\tdispatch_semaphore_signal(_sent);\n'
     '\t}\n}\n\n'
     '/* (redfin hmv10) private Metal API CoreAnimation uses before its swap: committed and on the GPU when it returns */\n'
     '- (BOOL)commitAndWaitUntilSubmitted\n{\n'
     '\t[self commit];\n'
     '\t[self waitUntilScheduled];\n'
     '\treturn [self error] == nil;\n}\n'),
    # (hmv12) HM_SHADOW_WAIT=1 (default; HM_SHADOW_WAIT_MS=250 the longest wait): a copy of an unaligned surface's rows
    # (-hakiosShadow:) is the texture the GPU reads and draws, and the processor rewrote it at ENCODE time (the refresh:
    # the seed moved, or HM_SHADOW_FRESH's once a buffer) while earlier committed buffers were still sampling or drawing
    # it; the completion write-back read it while a later committed buffer was drawing it. A full-screen quad caught
    # between its two triangles kept the old picture in one: the screenshots' (1125x2436, 4512-byte rows) diagonal split
    # (IMG_0114: edges on the screen diagonal, 0.46 px a row) and the App Library's stale and ghosted snapshot (0112/0113).
    # Now each such texture lists the committed buffers using it (readers until complete, writers until their write-back
    # has run); a refresh waits for them, and a write-back is deferred to a later committed writer (whose own write-back,
    # against the same base, carries both). 0 = as before.
    ('static volatile long hm_surf_live, hm_surf_live_max, hm_shadow_copies, hm_shadow_forced;\n',
     'static volatile long hm_surf_live, hm_surf_live_max, hm_shadow_copies, hm_shadow_forced;\n'
     '/* (redfin hmv12) HM_SHADOW_WAIT: refreshes that waited for the GPU, the microseconds, waits cut at HM_SHADOW_WAIT_MS,\n'
     ' * write-backs deferred to a later writer */\n'
     'static volatile long hm_shadow_waits, hm_shadow_wait_us, hm_shadow_wait_cut, hm_shadow_wb_deferred, hm_shadow_fresh_put_off;\n'
     'static int\nhm_shadow_wait(void)\n{\n'
     '\tstatic int on = -1;\n\n'
     '\tif (on < 0) {\n\t\tconst char *e = getenv("HM_SHADOW_WAIT");\n\n\t\ton = e == NULL || e[0] != \'0\';\n\t}\n'
     '\treturn on;\n}\n'
     'static double\nhm_shadow_wait_secs(void)\n{\n'
     '\tstatic double secs = -1;\n\n'
     '\tif (secs < 0) {\n\t\tconst char *e = getenv("HM_SHADOW_WAIT_MS");\n\n'
     '\t\tsecs = (e != NULL && atoi(e) > 0 ? atoi(e) : 250) / 1e3;\n\t}\n'
     '\treturn secs;\n}\n'),
    ('\tunsigned long long _shadowFrame;   /* (redfin hmv8) hm_tr_frame() at its last refresh */\n',
     '\tunsigned long long _shadowFrame;   /* (redfin hmv8) hm_tr_frame() at its last refresh */\n'
     '\tNSMutableArray *_shadowUsers;      /* (redfin hmv12) HM_SHADOW_WAIT: { command buffer, writer } using the copy */\n'),
    ('- (void)hakiosRefreshShadow;\n',
     '- (void)hakiosRefreshShadow;\n'
     '- (void)hakiosShadowUse:(id)cb writer:(BOOL)writer;   /* (redfin hmv12) */\n'
     '- (void)hakiosWriteBackShadowFrom:(id)cb;\n'),
    ('- (double)hakiosCreatedAt;\n',
     '- (double)hakiosCreatedAt;\n- (BOOL)hakiosCommitted;   /* (redfin hmv12) */\n'),
    ('- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)hakios_metal_cb_status(_cb); }\n',
     '- (MTLCommandBufferStatus)status { return (MTLCommandBufferStatus)hakios_metal_cb_status(_cb); }\n'
     '- (BOOL)hakiosCommitted { return _committed; }   /* (redfin hmv12) a held commit too, before its send */\n'),
    ('\t_shadowFrame = hm_tr_frame();\n',
     '\tif (![self hakiosShadowWaitUsers:forced]) {   /* (redfin hmv12) HM_SHADOW_WAIT: no buffer still reading or drawing the copy */\n'
     '\t\treturn;   /* (a forced refresh with buffers in flight: the next buffer tries again) */\n'
     '\t}\n'
     '\t_shadowFrame = hm_tr_frame();\n'),
    ('- (void)hakiosRefreshShadow\n{\n',
     '/* (redfin hmv12) HM_SHADOW_WAIT: CB reads (or, WRITER, draws and writes back) this copy. Readers stay listed until\n'
     ' * their buffer completes, writers until their write-back has run; buffers not committed are not waited for (the one\n'
     ' * being encoded), and one that only this list still holds was given up. */\n'
     '- (void)hakiosShadowUse:(id)cb writer:(BOOL)writer\n{\n'
     '\tif (_parent != nil) {\n\t\t[_parent hakiosShadowUse:cb writer:writer];\n\t\treturn;\n\t}\n'
     '\tif (!_shadow || cb == nil || ![cb isKindOfClass:[HakiosMetalCommandBuffer class]] || !hm_shadow_wait()) {\n\t\treturn;\n\t}\n'
     '\t@synchronized (self) {\n'
     '\t\tif (_shadowUsers == nil) _shadowUsers = [[NSMutableArray alloc] init];\n'
     '\t\t[self hakiosShadowPrune];\n'
     '\t\tif (!writer) {\n'
     '\t\t\tfor (NSArray *u in _shadowUsers) {\n'
     '\t\t\t\tif ([u objectAtIndex:0] == cb) return;\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\t[_shadowUsers addObject:[NSArray arrayWithObjects:cb, [NSNumber numberWithBool:writer], nil]];\n'
     '\t}\n}\n\n'
     '/* (lock held) readers whose buffer completed, and entries whose buffer nobody else holds */\n'
     '- (void)hakiosShadowPrune\n{\n'
     '\tNSMutableIndexSet *gone = [NSMutableIndexSet indexSet];\n\n'
     '\tfor (NSUInteger i = 0; i < [_shadowUsers count]; i++) {\n'
     '\t\tNSArray *u = [_shadowUsers objectAtIndex:i];\n'
     '\t\tHakiosMetalCommandBuffer *b = [u objectAtIndex:0];\n\n'
     '\t\tif ([b retainCount] <= 1 || (![[u objectAtIndex:1] boolValue] && hakios_metal_cb_status([b hakiosCommandBuffer]) >= HAKIOS_METAL_COMPLETED)) {\n'
     '\t\t\t[gone addIndex:i];\n'
     '\t\t}\n'
     '\t}\n'
     '\t[_shadowUsers removeObjectsAtIndexes:gone];\n}\n\n'
     '/* before the processor rewrites the copy: every committed buffer listed done with it (at most HM_SHADOW_WAIT_MS);\n'
     ' * NO (and no wait) when ONLY_IF_IDLE and one is not: HM_SHADOW_FRESH\'s refresh with the seed unmoved is left to a\n'
     ' * later buffer rather than stalling every frame behind the GPU */\n'
     '- (BOOL)hakiosShadowWaitUsers:(BOOL)onlyIfIdle\n{\n'
     '\tif (!hm_shadow_wait() || _shadowUsers == nil) {\n\t\treturn YES;\n\t}\n'
     '\tconst double began = hm_now();\n'
     '\tBOOL waited = NO;\n\n'
     '\tfor (;;) {\n'
     '\t\tBOOL busy = NO;\n\n'
     '\t\t@synchronized (self) {\n'
     '\t\t\t[self hakiosShadowPrune];\n'
     '\t\t\tfor (NSArray *u in _shadowUsers) {\n'
     '\t\t\t\tif ([(HakiosMetalCommandBuffer *)[u objectAtIndex:0] hakiosCommitted]) busy = YES;\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tif (!busy) break;\n'
     '\t\tif (onlyIfIdle) {\n\t\t\t__sync_fetch_and_add(&hm_shadow_fresh_put_off, 1);\n\t\t\treturn NO;\n\t\t}\n'
     '\t\tif (hm_now() - began > hm_shadow_wait_secs()) {\n'
     '\t\t\t__sync_fetch_and_add(&hm_shadow_wait_cut, 1);\n'
     '\t\t\tbreak;\n'
     '\t\t}\n'
     '\t\twaited = YES;\n'
     '\t\tusleep(200);\n'
     '\t}\n'
     '\tif (waited) {\n'
     '\t\t__sync_fetch_and_add(&hm_shadow_waits, 1);\n'
     '\t\t__sync_fetch_and_add(&hm_shadow_wait_us, (long)((hm_now() - began) * 1e6));\n'
     '\t}\n\treturn YES;\n}\n\n'
     '/* the write-back of CB\'s drawing: deferred when a later committed buffer is still drawing the copy (reading it now\n'
     ' * would catch that drawing half done); that buffer\'s write-back, against the same base, carries this one\'s too */\n'
     '- (void)hakiosWriteBackShadowFrom:(id)cb\n{\n'
     '\tif (_parent != nil) {\n\t\t[_parent hakiosWriteBackShadowFrom:cb];\n\t\treturn;\n\t}\n'
     '\tBOOL later = NO;\n\n'
     '\tif (hm_shadow_wait() && _shadowUsers != nil) {\n'
     '\t\t@synchronized (self) {\n'
     '\t\t\tfor (NSArray *u in _shadowUsers) {\n'
     '\t\t\t\tHakiosMetalCommandBuffer *b = [u objectAtIndex:0];\n\n'
     '\t\t\t\tif (b != cb && [[u objectAtIndex:1] boolValue] && [b hakiosCommitted] &&\n'
     '\t\t\t\t    hakios_metal_cb_status([b hakiosCommandBuffer]) < HAKIOS_METAL_COMPLETED) {\n'
     '\t\t\t\t\tlater = YES;\n'
     '\t\t\t\t}\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n'
     '\tif (later) {\n'
     '\t\t__sync_fetch_and_add(&hm_shadow_wb_deferred, 1);\n'
     '\t} else {\n'
     '\t\t[self hakiosWriteBackShadow];\n'
     '\t}\n'
     '\tif (_shadowUsers != nil) {\n'
     '\t\t@synchronized (self) {\n'
     '\t\t\tfor (NSUInteger i = 0; i < [_shadowUsers count]; i++) {\n'
     '\t\t\t\tNSArray *u = [_shadowUsers objectAtIndex:i];\n\n'
     '\t\t\t\tif ([u objectAtIndex:0] == cb && [[u objectAtIndex:1] boolValue]) {\n'
     '\t\t\t\t\t[_shadowUsers removeObjectAtIndex:i];\n'
     '\t\t\t\t\tbreak;\n'
     '\t\t\t\t}\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t}\n}\n\n'
     '- (void)hakiosRefreshShadow\n{\n'),
    ('\t\tCFRelease(_iosurface);\n\t}\n\tfree(_shadowBase);\n',
     '\t\tCFRelease(_iosurface);\n\t}\n\tfree(_shadowBase);\n\t[_shadowUsers release];   /* (redfin hmv12) */\n'),
    ('\t\t[(HakiosMetalTexture *)t hakiosRefreshShadow];\n\t\t{\n',
     '\t\t[(HakiosMetalTexture *)t hakiosRefreshShadow];\n'
     '\t\t[(HakiosMetalTexture *)t hakiosShadowUse:(id)cb writer:YES];   /* (redfin hmv12) HM_SHADOW_WAIT */\n\t\t{\n'),
    ('\t\t\t\t(void)done;\n\t\t\t\t[drawn hakiosWriteBackShadow];\n',
     '\t\t\t\t[drawn hakiosWriteBackShadowFrom:(id)done];   /* (redfin hmv12) */\n'),
    ('\t[(HakiosMetalTexture *)texture hakiosRefreshShadow];\n'
     '\tif ([(HakiosMetalTexture *)texture hakiosIsShadow] && ([(HakiosMetalTexture *)texture usage] & MTLTextureUsageShaderWrite) != 0) {\n',
     '\t[(HakiosMetalTexture *)texture hakiosRefreshShadow];\n'
     '\t[(HakiosMetalTexture *)texture hakiosShadowUse:_commandBuffer writer:NO];   /* (redfin hmv12) HM_SHADOW_WAIT */\n'
     '\tif ([(HakiosMetalTexture *)texture hakiosIsShadow] && ([(HakiosMetalTexture *)texture usage] & MTLTextureUsageShaderWrite) != 0) {\n'
     '\t\t[(HakiosMetalTexture *)texture hakiosShadowUse:_commandBuffer writer:YES];\n'),
    ('\t\t\t[written hakiosWriteBackShadow];\n',
     '\t\t\t[written hakiosWriteBackShadowFrom:cb];   /* (redfin hmv12) */\n'),
    ('\ttable[index] = texture;\n\t[(HakiosMetalTexture *)texture hakiosRefreshShadow];\n',
     '\ttable[index] = texture;\n\t[(HakiosMetalTexture *)texture hakiosRefreshShadow];\n'
     '\t[(HakiosMetalTexture *)texture hakiosShadowUse:_commandBuffer writer:NO];   /* (redfin hmv12) HM_SHADOW_WAIT */\n'),
    ('char line[700] = "", line2[800] = "";', 'char line[700] = "", line2[1200] = "";   /* (redfin hmv12) 800 -> 1200 */'),
    ('\t\t\t\tmemcpy(xwas, xr, sizeof(xwas));\n\t\t\t\tcwas = copies, fwas = forced;\n',
     '\t\t\t\t{   /* (redfin hmv12) HM_SHADOW_WAIT */\n'
     '\t\t\t\t\tstatic long swas, uwas, cutwas, dwas, pwas;\n'
     '\t\t\t\t\tconst long sw = hm_shadow_waits, us = hm_shadow_wait_us, cut = hm_shadow_wait_cut, d = hm_shadow_wb_deferred,\n'
     '\t\t\t\t\t\t   po = hm_shadow_fresh_put_off;\n'
     '\t\t\t\t\tconst int at4 = (int)strlen(line2);\n\n'
     '\t\t\t\t\tsnprintf(line2 + at4, sizeof(line2) - (size_t)at4, "; copy refreshes that waited for the GPU %ld, %.1f ms avg, %ld cut "\n'
     '\t\t\t\t\t\t "at the limit; write-backs deferred to a later writer %ld; unmoved-seed refreshes put off %ld (HM_SHADOW_WAIT)", sw - swas,\n'
     '\t\t\t\t\t\t sw - swas ? (double)(us - uwas) / 1e3 / (double)(sw - swas) : 0.0, cut - cutwas, d - dwas, po - pwas);\n'
     '\t\t\t\t\tswas = sw, uwas = us, cutwas = cut, dwas = d, pwas = po;\n'
     '\t\t\t\t}\n'
     '\t\t\t\tmemcpy(xwas, xr, sizeof(xwas));\n\t\t\t\tcwas = copies, fwas = forced;\n'),
]

# (redfin hmyc) HM_YCBCR=native (default) | cpu: a two-plane '420f' / '420v' surface sampled as 500 (CoreAnimation's photos,
# the HEIC wallpaper, video frames) or 540 (VideoToolbox's live wallpaper frames) is no longer converted to RGBA8 on the
# processor (24-68 ms a 1080x1920 frame, h6): the core wraps both planes (hakios_metal_texture_new_ycbcr) and a draw
# samples an RGBA8 copy the GPU converts from them (ycbcr_decode), again only when the surface's seed moves. Planes the
# kernel cannot read (rows or starts not a multiple of 4 bytes), or HM_YCBCR=cpu, keep the processor copy, which now uses
# the core's integers (hakios_metal_ycbcr_convert) so both paths draw the same bytes. The matrix: the surface's
# IOSurfaceYCbCrMatrix attachment as before, and without one (SweetSurface drops values) an HD '420v' picture (a decoder's,
# 1280x720 and up) is BT.709, as CoreVideo tags those and SweetMetal2 reads them; everything else BT.601 as before.
EDITS += [
    ('\tMTLPixelFormat _ycbcrFormat;        /* ... and the private format it was asked as (500, 540), which it answers */\n',
     '\tMTLPixelFormat _ycbcrFormat;        /* ... and the private format it was asked as (500, 540), which it answers */\n'
     '\tBOOL _ycbcrNative;                  /* (redfin hmyc) HM_YCBCR=native: the core converts on the GPU (no copy here) */\n'
     '\tuint32_t _ycbcrSeed;                /* (redfin hmyc) ... the surface seed it was last told */\n'),
    ('- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format;\n',
     '- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format;\n'
     '- (void)hakiosYCbCrNative:(IOSurfaceRef)surface format:(MTLPixelFormat)format;   /* (redfin hmyc) */\n'),
    ('- (MTLPixelFormat)pixelFormat       { return _ycbcr != 0 ? _ycbcrFormat : (MTLPixelFormat)hakios_metal_texture_format(_texture); }',
     '- (MTLPixelFormat)pixelFormat       { return _ycbcr != 0 || _ycbcrNative ? _ycbcrFormat : (MTLPixelFormat)hakios_metal_texture_format(_texture); }'),
    ('\t\thakios_metal_texture *copy = NULL;\n\t\tif (hm_compress_ok() && [descriptor width] * [descriptor height] >= 65536) {\n',
     '\t\t/* (redfin hmyc) HM_YCBCR=native (default): no copy on the processor -- the core\'s texture over both planes, which\n'
     '\t\t * draws sample through an RGBA8 copy the GPU converts from them (ycbcr_decode), again only when the seed moves.\n'
     '\t\t * HM_YCBCR=cpu, or planes the kernel cannot read: the RGBA8 copy below. */\n'
     '\t\tif (hakios_metal_ycbcr_mode() == 1) {\n'
     '\t\t\tint matrix = 1, full = 1;\n'
     '\t\t\tconst uint8_t *b0 = IOSurfaceGetBaseAddress(surface), *p0 = IOSurfaceGetBaseAddressOfPlane(surface, 0),\n'
     '\t\t\t\t      *p1 = IOSurfaceGetBaseAddressOfPlane(surface, 1);\n'
     '\t\t\thakios_metal_texture *yt = NULL;\n\n'
     '\t\t\thm_ycbcr_kind(surface, &matrix, &full);\n'
     '\t\t\tif (b0 != NULL && p0 != NULL && p1 != NULL && p0 >= b0 && p1 >= b0) {\n'
     '\t\t\t\tyt = hakios_metal_texture_new_ycbcr(_gpu, IOSurfaceGetID(surface), (unsigned)[descriptor width], (unsigned)[descriptor height],\n'
     '\t\t\t\t\t\t\t\t     (uint64_t)(p0 - b0), (unsigned)IOSurfaceGetBytesPerRowOfPlane(surface, 0),\n'
     '\t\t\t\t\t\t\t\t     (uint64_t)(p1 - b0), (unsigned)IOSurfaceGetBytesPerRowOfPlane(surface, 1),\n'
     '\t\t\t\t\t\t\t\t     (unsigned)matrix, full, [descriptor pixelFormat] == 540);\n'
     '\t\t\t}\n'
     '\t\t\tif (yt != NULL) {\n'
     '\t\t\t\tHM_ONCE("a two-plane YCbCr surface sampled as format %lu is converted on the GPU from its planes, again when the "\n'
     '\t\t\t\t\t"surface changes (HM_YCBCR=native)\\n", (unsigned long)[descriptor pixelFormat]);\n'
     '\t\t\t\tt = [[HakiosMetalTexture alloc] initWithHakiosDevice:self texture:yt descriptor:descriptor usage:MTLTextureUsageShaderRead];\n'
     '\t\t\t\t[t hakiosYCbCrNative:surface format:[descriptor pixelFormat]];\n'
     '\t\t\t\treturn t;\n'
     '\t\t\t}\n'
     '\t\t\t{\n'
     '\t\t\t\tstatic int said;\n\n'
     '\t\t\t\tif (said++ < 8) hm_print("HM_YCBCR: surface %u (%lux%lu, planes of %zu and %zu bytes a row) not converted on the GPU: %s; "\n'
     '\t\t\t\t\t\t\t "an RGBA8 copy on the processor\\n", IOSurfaceGetID(surface), (unsigned long)[descriptor width],\n'
     '\t\t\t\t\t\t\t (unsigned long)[descriptor height], IOSurfaceGetBytesPerRowOfPlane(surface, 0),\n'
     '\t\t\t\t\t\t\t IOSurfaceGetBytesPerRowOfPlane(surface, 1), hakios_metal_error());\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\thakios_metal_texture *copy = NULL;\n\t\tif (hm_compress_ok() && [descriptor width] * [descriptor height] >= 65536) {\n'),
    # the matrix and range, one rule for both paths
    ('\tCFTypeRef matrix = IOSurfaceCopyValue(surface, CFSTR("IOSurfaceYCbCrMatrix"));\n\n\t_ycbcr = 1;\n',
     '_ycbcrFull = IOSurfaceGetPixelFormat(surface) != 0x34323076;   /* not \'420v\' */\n',
     '\tint matrix = 1, full = 1;   /* (redfin hmyc) hm_ycbcr_kind: the native path\'s rule */\n\n'
     '\thm_ycbcr_kind(surface, &matrix, &full);\n'
     '\t_ycbcr = matrix;\n'
     '\t_ycbcrFormat = format;\n'
     '\t_ycbcrFull = full != 0;\n'),
    ('- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format\n{\n',
     '/* (redfin hmyc) the matrix (1 BT.601, 2 BT.709) and range of a two-plane surface: its IOSurfaceYCbCrMatrix attachment\n'
     ' * when it has one; without (SweetSurface drops values), an HD \'420v\' picture (1280x720 and up, a decoder\'s) BT.709 as\n'
     ' * CoreVideo tags those and SweetMetal2 takes them, anything else BT.601 (the wallpaper\'s HEIC, the camera\'s 420f).\n'
     ' * Full range unless \'420v\'. */\n'
     'static void\n'
     'hm_ycbcr_kind(IOSurfaceRef surface, int *matrix, int *full)\n'
     '{\n'
     '\tCFTypeRef value = IOSurfaceCopyValue(surface, CFSTR("IOSurfaceYCbCrMatrix"));\n'
     '\tconst BOOL video = IOSurfaceGetPixelFormat(surface) == 0x34323076;   /* \'420v\' */\n\n'
     '\t*full = !video;\n'
     '\t*matrix = video && IOSurfaceGetWidth(surface) * IOSurfaceGetHeight(surface) >= 1280 * 720 ? 2 : 1;\n'
     '\tif (value != NULL) {\n'
     '\t\tif (CFGetTypeID(value) == CFStringGetTypeID()) {\n'
     '\t\t\tif (CFStringFind((CFStringRef)value, CFSTR("709"), 0).location != kCFNotFound) *matrix = 2;\n'
     '\t\t\telse if (CFStringFind((CFStringRef)value, CFSTR("601"), 0).location != kCFNotFound) *matrix = 1;\n'
     '\t\t}\n'
     '\t\tCFRelease(value);\n'
     '\t}\n'
     '}\n\n'
     '- (void)hakiosShadowYCbCr:(IOSurfaceRef)surface format:(MTLPixelFormat)format\n{\n'),
    ('- (void)hakiosConvertYCbCr\n{\n',
     '\t(void)hakios_metal_texture_replace(_texture, 0, 0, (unsigned)w, (unsigned)h, rows, (uint64_t)w * 4);\n\tfree(rows);\n}\n',
     '/* (redfin hmyc) HM_YCBCR=native: the GPU converts at the next draw whenever the surface\'s seed moved (hakiosRefreshShadow\n'
     ' * tells the core at each bind); nothing is copied here */\n'
     '- (void)hakiosYCbCrNative:(IOSurfaceRef)surface format:(MTLPixelFormat)format\n'
     '{\n'
     '\t[self hakiosSetIOSurface:surface plane:0];\n'
     '\t_ycbcrNative = YES;\n'
     '\t_ycbcrFormat = format;\n'
     '\t_ycbcrSeed = IOSurfaceGetSeed(surface);\n'
     '\thakios_metal_texture_ycbcr_touch(_texture, _ycbcrSeed);\n'
     '}\n\n'
     '/* (redfin hmyc) the core\'s conversion (hakios_metal_ycbcr_convert): the integers ycbcr_decode uses on the GPU, so\n'
     ' * HM_YCBCR=cpu and native draw the same bytes (nearest chroma, 16.16 fixed point, within 1 LSB of the exact formula);\n'
     ' * 540 the planes as they are (R Cr, G Y, B Cb) -- VideoToolbox applies the matrix itself */\n'
     '- (void)hakiosConvertYCbCr\n'
     '{\n'
     '\tconst size_t w = hakios_metal_texture_width(_texture), h = hakios_metal_texture_height(_texture);\n'
     '\tconst uint8_t *luma = IOSurfaceGetBaseAddressOfPlane(_iosurface, 0), *chroma = IOSurfaceGetBaseAddressOfPlane(_iosurface, 1);\n'
     '\tconst size_t lumaRow = IOSurfaceGetBytesPerRowOfPlane(_iosurface, 0), chromaRow = IOSurfaceGetBytesPerRowOfPlane(_iosurface, 1);\n'
     '\tconst size_t cw = IOSurfaceGetWidthOfPlane(_iosurface, 1), ch = IOSurfaceGetHeightOfPlane(_iosurface, 1);\n'
     '\tuint8_t *rows = malloc(w * h * 4);\n\n'
     '\tif (rows == NULL || luma == NULL || chroma == NULL || cw < (w + 1) / 2 || ch < (h + 1) / 2 || lumaRow < w || chromaRow < 2 * cw) {\n'
     '\t\tfree(rows);\n'
     '\t\treturn;\n'
     '\t}\n'
     '\thakios_metal_ycbcr_convert(luma, lumaRow, chroma, chromaRow, (unsigned)w, (unsigned)h, (unsigned)_ycbcr, _ycbcrFull ? 1 : 0,\n'
     '\t\t\t\t   _ycbcrFormat == (MTLPixelFormat)540, rows, (uint64_t)w * 4);\n'
     '\t(void)hakios_metal_texture_replace(_texture, 0, 0, (unsigned)w, (unsigned)h, rows, (uint64_t)w * 4);\n'
     '\tfree(rows);\n'
     '}\n'),
    ('\t\t[_parent hakiosRefreshShadow];   /* the rows a view reads are its parent\'s, copy included */\n\t\treturn;\n\t}\n',
     '\t\t[_parent hakiosRefreshShadow];   /* the rows a view reads are its parent\'s, copy included */\n\t\treturn;\n\t}\n'
     '\tif (_ycbcrNative && _iosurface != NULL) {   /* (redfin hmyc) the GPU converts again at the next draw if the seed moved */\n'
     '\t\tconst uint32_t ys = IOSurfaceGetSeed(_iosurface);\n\n'
     '\t\tif (ys != _ycbcrSeed) {\n'
     '\t\t\t_ycbcrSeed = ys;\n'
     '\t\t\thakios_metal_texture_ycbcr_touch(_texture, ys);\n'
     '\t\t}\n'
     '\t\treturn;\n'
     '\t}\n'),
    ('\tchar line[700] = "", line2[1200] = "";   /* (redfin hmv12) 800 -> 1200 */\n',
     '\tchar line[700] = "", line2[1600] = "";   /* (redfin hmv12) 800 -> 1200; (redfin hmyc) -> 1600 */\n'),
    ('\t\t\t\t{   /* (redfin hmv12) HM_SHADOW_WAIT */\n',
     '\t\t\t\t{   /* (redfin hmyc) HM_YCBCR=native */\n'
     '\t\t\t\t\tstatic unsigned long long ywas[8];\n'
     '\t\t\t\t\tunsigned long long y[8];\n'
     '\t\t\t\t\tconst int aty = (int)strlen(line2);\n\n'
     '\t\t\t\t\thakios_metal_ycbcr_stats(y);\n'
     '\t\t\t\t\tsnprintf(line2 + aty, sizeof(line2) - (size_t)aty, "; YCbCr GPU decodes %llu in %llu buffers, %.2f ms avg to encode, "\n'
     '\t\t\t\t\t\t "copies reused %llu, processor %llu, undecoded %llu, verified %llu (%llu differ) (HM_YCBCR)", y[0] - ywas[0],\n'
     '\t\t\t\t\t\t y[1] - ywas[1], y[1] - ywas[1] ? (double)(y[5] - ywas[5]) / 1e3 / (double)(y[1] - ywas[1]) : 0.0, y[6] - ywas[6],\n'
     '\t\t\t\t\t\t y[2] - ywas[2], y[7] - ywas[7], y[3] - ywas[3], y[4] - ywas[4]);\n'
     '\t\t\t\t\tmemcpy(ywas, y, sizeof(ywas));\n'
     '\t\t\t\t}\n'
     '\t\t\t\t{   /* (redfin hmv12) HM_SHADOW_WAIT */\n'),
]
EDITS += [
    ('static void hm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));\n',
     'static void hm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));\n'
     'static void hm_ycbcr_kind(IOSurfaceRef surface, int *matrix, int *full);   /* (redfin hmyc) */\n'),
]


# (hm-int2) luma boost: the dispatcher's knob (m2shim.m, /var/local/hmetal/luma-boost; its address in HM_LUMA_KNOB) draws it
# too, read at every luma_boost draw (CoreAnimation records none while the knob is off: CASetDebugOption(91))
EDITS.append(('\t\tHM_ONCE("luma_boost: %s (metal_luma_boost=1 draws it)\\n", keep ? "drawn" : "left out, the picture as it is");\n\t}\n\treturn !keep;\n',
              '\t\tHM_ONCE("luma_boost: %s (metal_luma_boost=1 or the luma-boost knob draws it)\\n", keep ? "drawn" : "left out, the picture as it is");\n\t}\n'
              '\t{   /* (redfin hm-int2) the dispatcher\'s knob, its value now */\n'
              '\t\tstatic volatile int *knob = (volatile int *)-1;\n\n'
              '\t\tif (knob == (volatile int *)-1) {\n'
              '\t\t\tconst char *a = getenv("HM_LUMA_KNOB");\n\n'
              '\t\t\tknob = a != NULL ? (volatile int *)(uintptr_t)strtoull(a, NULL, 16) : NULL;\n'
              '\t\t}\n'
              '\t\tif (knob != NULL && *knob) {\n'
              '\t\t\tHM_ONCE("luma_boost: drawn (the luma-boost knob is on)\\n");\n'
              '\t\t\treturn NO;\n'
              '\t\t}\n'
              '\t}\n'
              '\treturn !keep;\n'))
# ---- (redfin hm-cache) the stage cache, compile waits, the pre-warm, ca-prof, the HANG split (bench/plugin.py) ----
HMC_HELPERS = r'''/* (redfin hm-cache) helpers of the cache / compile-wait edits (bench/plugin.py):
 *   HM_COMPILE_LOG=1 (default with HM_PROF>=1): a pipeline whose stages were compiled or took 20 ms: "compile: pipeline
 *     LABEL: wall W ms, compiling C ms (CPU P ms; front end F, folding K), waited for the compiler X ms; N compiled, H
 *     cached (M memo, S seed)". Wall much above CPU = the thread waited for a core or a lock, not for the compiler.
 *   HM_SPEC_ASYNC=0 (default): a configuration of a denied/blamed uber shader not specialised before is specialised
 *     on the asking thread and the draw waits (the uber program itself must never draw: it hangs the GPU). 1: it is
 *     specialised on the specialiser's queue and the draw is left out until it is done (a layer missing for ~0.4 s,
 *     possibly kept in a cached offscreen texture: test before making it the default).
 *   HM_SPEC_QOS=1 (default): the specialiser's queue at user-initiated QoS, its compiles on the core's background
 *     compiler (HM_COMPILE_SPLIT), so a render thread that waits for one is not waiting behind a default-QoS queue. */
static int
hm_compile_log_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_COMPILE_LOG");
		on = e != NULL && e[0] ? atoi(e) != 0 : hm_prof_level() >= 1;
	}
	return on;
}

static int
hm_spec_async(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_SPEC_ASYNC");
		on = e != NULL && atoi(e) != 0;
	}
	return on;
}

static int
hm_spec_qos_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_SPEC_QOS");
		on = e == NULL || e[0] == 0 || atoi(e) != 0;
	}
	return on;
}

/* the thread's compile figures: [0] wait ms [1] compile ms [2] hits [3] misses, then hakios_metal_compile_stats_more's
 * [4] CPU ms [5] front end ms [6] folding ms [7] memo hits [8] seed hits */
static void
hm_compile_snap(double out[9])
{
	unsigned h = 0, m = 0;

	memset(out, 0, 9 * sizeof(double));
	hakios_metal_compile_stats(&out[0], &out[1], &h, &m);
	out[2] = h, out[3] = m;
	hakios_metal_compile_stats_more(out + 4, 5);
}

static void
hm_compile_note(const char *what, double wallMs, const double a[9], const double b[9])
{
	if (!hm_compile_log_on() || (b[3] - a[3] == 0 && wallMs < 20)) return;
	hm_print("compile: %s: wall %.0f ms, compiling %.0f ms (CPU %.0f ms; front end %.0f, folding %.0f), waited for the compiler %.0f ms; "
		 "%.0f compiled, %.0f cached (%.0f memo, %.0f seed)\n", what, wallMs, b[1] - a[1], b[4] - a[4], b[5] - a[5], b[6] - a[6],
		 b[0] - a[0], b[3] - a[3], b[2] - a[2], b[7] - a[7], b[8] - a[8]);
}

/* how a specialisation's stage came: compiled, or read from the memo, a seed or the cache */
static const char *
hm_compile_source(const double a[9], const double b[9])
{
	return b[3] > a[3] ? "compiled" : b[7] > a[7] ? "from the memo" : b[8] > a[8] ? "from a seed" : b[2] > a[2] ? "from the cache" : "?";
}

'''

PREWARM_NEW = r'''/* (redfin hm-cache) HM_PREWARM=1 (default): the pre-warm is the stage cache's own. Each stage compiled into the
 * writable cache leaves a manifest beside it (core HM_CACHE_MANIFEST: src/NAME.src + bc/HASH.bc), so after
 * HAKIOS_METAL_CACHE_FORMAT or the Mesa changed -- or a stage was lost -- the stages this process compiled before
 * (CoreAnimation's pipelines and the specialisations of the denied uber shaders among them) are compiled again here, in
 * the background, before they are asked for. A stage already in the cache or a seed under this key is skipped without a
 * compile. The thread runs at utility QoS, on the core's background compiler (HM_COMPILE_SPLIT: the render thread never
 * waits for it), starts HM_PREWARM_DELAY s (default 60) after QuartzCore's library opened, and waits while frames are
 * being drawn (a buffer completed in the last 250 ms). (Before: QuartzCore's library's -pipelineNames, which answer
 * nothing on this device: "prewarm: done: 0 pipelines made" on every boot.) */
static int
hm_prewarm_on(void)
{
	const char *e = getenv("HM_PREWARM");
	return e == NULL || e[0] == 0 || atoi(e) != 0;
}

static int
hm_prewarm_busy(__unused void *ctx)
{
	return hm_now() - hm_tr_last_done < 0.25;
}

static void *
hm_prewarm_main(void *arg)
{
	const double delay = *(double *)arg;
	unsigned n[5] = { 0 };
	double a[9], b[9];

	free(arg);
	pthread_setname_np("hakios.prewarm");
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
	hakios_metal_compile_background(1);
	usleep((useconds_t)(delay * 1e6));
	hm_compile_snap(a);
	const double began = hm_now();
	hm_print("prewarm: the cache's manifests, stages missing under key \"%s\" compiled again, while no frame is drawn\n", hakios_metal_cache_key());
	hakios_metal_cache_rewarm(hm_prewarm_busy, NULL, n);
	hm_compile_snap(b);
	hm_print("prewarm: done: %u manifests, %u present, %u made, %u failed, %u unreadable; %.0f s compiling (CPU %.0f s), %.0f s in all\n",
		 n[0], n[1], n[2], n[3], n[4], (b[1] - a[1]) / 1e3, (b[4] - a[4]) / 1e3, hm_now() - began);
	return NULL;
}

static void
hm_prewarm_start(id library, NSString *path)
{
	static dispatch_once_t once;

	if (library == nil || !hm_prewarm_on() || [path rangeOfString:@"QuartzCore.framework"].location == NSNotFound) {
		return;
	}
	dispatch_once(&once, ^{
		const char *d = getenv("HM_PREWARM_DELAY");
		double *delay = malloc(sizeof(double));
		pthread_t th;
		pthread_attr_t attr;

		*delay = d != NULL && d[0] ? atof(d) : 60.0;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 4 << 20);   /* (the compiler's recursion over an uber shader) */
		if (pthread_create(&th, &attr, hm_prewarm_main, delay) != 0) free(delay);
		pthread_attr_destroy(&attr);
	});
}
'''

SPEC_MINE_NEW = r'''		struct timespec t0, t1;
		double ca[9], cb[9];

		hm_compile_snap(ca);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		hakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,
									 (unsigned)_specBinding, [key bytes], [key length]);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		hm_compile_snap(cb);
		hm_tr_event("specialise (waited for) %s: %.0f ms", hakios_metal_program_name(_program),
			    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);   /* (redfin hmv8) */
		{   /* (redfin hm-cache) where the stage came from, the CPU time, and the configuration's bytes */
			char hex[2 * 256 + 1] = "";
			const unsigned char *kb = [key bytes];

			for (NSUInteger i = 0; i < [key length] && i < 256; i++) snprintf(hex + 2 * i, 3, "%02x", kb[i]);
			hm_print("specialise (waited for, hmv7): %s for %u bytes of its buffer %d: %s%u instruction words, not %u, in %.0f ms "
				 "(%s; CPU %.0f ms, front end %.0f, folding %.0f); configuration %s\n",
				 hakios_metal_program_name(_program), _specLength, _specBinding, p != NULL ? "" : "FAILED: ",
				 p != NULL ? hakios_metal_program_stage_words(p, 1) : 0, hakios_metal_program_stage_words(_program, 1),
				 (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, hm_compile_source(ca, cb), cb[4] - ca[4],
				 cb[5] - ca[5], cb[6] - ca[6], hex);
		}
		if (p == NULL) hm_print("specialise: %s\n", hakios_metal_error());
'''

HMC_EDITS = [
    # helpers, before hm_tr_pipeline (after hm_prof_level, before the pipeline and specialiser code that use them)
    ('/* a pipeline made: its ms and what of them the compiler\'s (the thread\'s compile stats before and after) */\n',
     HMC_HELPERS + '/* a pipeline made: its ms and what of them the compiler\'s (the thread\'s compile stats before and after) */\n'),
    # item 4: wall vs CPU per pipeline (HM_PROF / HM_COMPILE_LOG)
    ('\thakios_metal_compile_stats(&hmTrW0, &hmTrC0, &hmTrH0, &hmTrM0);\n',
     '\thakios_metal_compile_stats(&hmTrW0, &hmTrC0, &hmTrH0, &hmTrM0);\n\tdouble hmCa[9];\n\n\thm_compile_snap(hmCa);   /* (redfin hm-cache) */\n'),
    ('\t\t\t\thakios_metal_compile_stats(&w, &c, &h, &m);\n',
     '\t\t\t\thakios_metal_compile_stats(&w, &c, &h, &m);\n'
     '\t\t\t\t{   /* (redfin hm-cache) HM_COMPILE_LOG */\n'
     '\t\t\t\t\tdouble hmCb[9];\n\t\t\t\t\tchar what[300];\n\n'
     '\t\t\t\t\thm_compile_snap(hmCb);\n'
     '\t\t\t\t\tsnprintf(what, sizeof(what), "pipeline %s", [descriptor label] != nil ? [[descriptor label] UTF8String] : "(no label)");\n'
     '\t\t\t\t\thm_compile_note(what, (hm_now() - hmTrBegan) * 1e3, hmCa, hmCb);\n'
     '\t\t\t\t}\n'),
    # the specialisation the draw waits for: its source (memo / cache / seed / compiled), CPU, configuration bytes
    ('\t\tstruct timespec t0, t1;\n\n\t\tclock_gettime(CLOCK_MONOTONIC, &t0);\n\t\thakios_metal_program *p = hakios_metal_program_specialize(',
     '\t\tif (p == NULL) hm_print("specialise: %s\\n", hakios_metal_error());\n', SPEC_MINE_NEW),
    # HM_SPEC_ASYNC (opt-in): a configuration not specialised before compiled on the queue, its draws left out meanwhile
    ('\tif (wait && found == NULL && !mine) {\n\t\t/* being specialised on the queue',
     '\tif (mine && hm_spec_async()) {   /* (redfin hm-cache) HM_SPEC_ASYNC=1 */\n'
     '\t\tmine = NO;\n\t\tstart = YES;\n'
     '\t\thm_v7_left_out(_program, "its specialisation for this configuration is being compiled (HM_SPEC_ASYNC)");\n'
     '\t} else if (wait && found == NULL && !mine && hm_spec_async()) {\n'
     '\t\thm_v7_left_out(_program, "its specialisation for this configuration is being compiled or failed (HM_SPEC_ASYNC)");\n'
     '\t\treturn NULL;\n\t}\n'
     '\tif (wait && found == NULL && !mine) {\n\t\t/* being specialised on the queue'),
    # HM_SPEC_QOS: the specialiser's queue at user-initiated QoS, on the background compiler
    ('\t\t\tcompiling = dispatch_queue_create("hakios.metal.specialise", DISPATCH_QUEUE_SERIAL);\n',
     '\t\t\tcompiling = dispatch_queue_create("hakios.metal.specialise", hm_spec_qos_on() ?   /* (redfin hm-cache) HM_SPEC_QOS */\n'
     '\t\t\t\tdispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0) : DISPATCH_QUEUE_SERIAL);\n'),
    ('\t\t\tclock_gettime(CLOCK_MONOTONIC, &t0);\n\t\t\thakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,\n'
     '\t\t\t\t\t\t\t\t\t\t (unsigned)_specBinding, [key bytes], [key length]);\n\t\t\tclock_gettime(CLOCK_MONOTONIC, &t1);\n',
     '\t\t\tclock_gettime(CLOCK_MONOTONIC, &t0);\n\t\t\tif (hm_spec_qos_on()) hakios_metal_compile_background(1);   /* (redfin hm-cache) the background compiler */\n'
     '\t\t\thakios_metal_program *p = hakios_metal_program_specialize(_program, [_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,\n'
     '\t\t\t\t\t\t\t\t\t\t (unsigned)_specBinding, [key bytes], [key length]);\n'
     '\t\t\thakios_metal_compile_background(0);   /* (a GCD worker: nothing kept for its next block) */\n'
     '\t\t\tclock_gettime(CLOCK_MONOTONIC, &t1);\n'),
    # item 5: the pre-warm, rebuilt on the cache's manifests
    ('/* (redfin hmv4) HM_PREWARM=1 (default): every pipeline QuartzCore\'s library names made once on a thread of its own, at\n',
     '\t\t\t[names release];\n\t\t\t[held release];\n\t\t});\n\t});\n}\n', PREWARM_NEW),
    # coordinator: ca-prof only when asked for (HM_CA_PROF_US=N or the boot-arg), no longer by HM_TRACE alone
    ('\t\t\tca_prof_us = us != NULL && us[0] ? (unsigned)atoi(us) : 25000;\n',
     '\t\t\tca_prof_us = us != NULL && us[0] ? (unsigned)atoi(us) : 0;   /* (redfin hm-cache) was 25000: 79 % of the log, screen off too */\n'),
    # coordinator: the HANG trace's GPU time split into queued and running; HM_HANG_EXEC=1 (default) reports only a
    # buffer whose own time (encode + commit + running) passed 100 ms, not one that only queued behind others
    ('static double hm_tr_last_done, hm_tr_last_enc, hm_tr_last_gpu;\n',
     'static double hm_tr_last_done, hm_tr_last_enc, hm_tr_last_gpu;\n'
     'static __thread double hm_tr_split_q, hm_tr_split_r;   /* (redfin hm-cache) the buffer being completed: queued, running (s) */\n'
     'static int\nhm_hang_exec_on(void)\n{\n\tstatic int on = -1;\n\n\tif (on < 0) {\n\t\tconst char *e = getenv("HM_HANG_EXEC");\n'
     '\t\ton = e == NULL || e[0] == 0 || atoi(e) != 0;\n\t}\n\treturn on;\n}\n'),
    ('\tif (total > 0.1) {\n\t\tchar what[160];\n\n\t\tsnprintf(what, sizeof(what), "a buffer made to completed (encode %.0f, commit %.0f, GPU %.0f ms):", encode * 1e3, commit * 1e3, gpu * 1e3);\n',
     '\tif (total > 0.1 && (!hm_hang_exec_on() || encode + commit + hm_tr_split_r > 0.1)) {   /* (redfin hm-cache) HM_HANG_EXEC */\n\t\tchar what[200];\n\n'
     '\t\tsnprintf(what, sizeof(what), "a buffer made to completed (encode %.0f, commit %.0f, GPU %.0f ms = queued %.0f + running %.0f):", encode * 1e3,\n'
     '\t\t\t commit * 1e3, gpu * 1e3, hm_tr_split_q * 1e3, hm_tr_split_r * 1e3);\n'),
    ('\thm_tr_buffer([self GPUStartTime] - [self hakiosCreatedAt], self->_hmCommitSecs, hm_now() - [self GPUStartTime],\n',
     '\t{   /* (redfin hm-cache) the GPU time split for the HANG trace */\n\t\tint64_t q = 0, r = 0;\n\n'
     '\t\thakios_metal_cb_gpu_split(cb, &q, &r);\n\t\thm_tr_split_q = (double)q / 1e9, hm_tr_split_r = (double)r / 1e9;\n\t}\n'
     '\thm_tr_buffer([self GPUStartTime] - [self hakiosCreatedAt], self->_hmCommitSecs, hm_now() - [self GPUStartTime],\n'),
]
EDITS += HMC_EDITS

# ---- (redfin hm-int4) HMI4_EDITS, applied last ----
# HM_SPEC_MASK=1 (default; 0: the whole buffer as before): a configuration's key holds only the bytes of the uber shader's
# buffer its fragment function reads (hakios_metal_known_read_mask: CoreAnimation's fixed_frag_lph_cpf reads 34 of the 52
# bytes of its FragmentShaderConfig; the other 18 are padding and two fields only the CPU uses), the rest given as 0 to the
# specialiser and the stage cache, so that what CA leaves in them makes no new configuration. The mask is taken once per
# pipeline when the fragment is kept (one front-end translation, kept for the specialisations: HM_FE_MEMO), and printed:
#   "spec mask: PROGRAM reads N of L bytes of its buffer B: rrr.rrr. ..."
# Each first-time configuration's line now also names how many there are and the bytes it differs in from the nearest
# earlier one ("new configuration #N; nearest #M differs in bytes 37,41"), so that a field that changes with every frame
# shows as such.
HMI4_IVARS_OLD = '\tint _specBinding;\n\tunsigned _specLength;\n\tNSMutableDictionary *_specialised;\n}'
HMI4_IVARS_NEW = ('\tint _specBinding;\n\tunsigned _specLength;\n\tNSMutableDictionary *_specialised;\n'
                  '\tunsigned char _specMask[256];   /* (redfin hm-int4) HM_SPEC_MASK: 1 for a byte of the buffer the fragment reads */\n'
                  '\tint _specMaskRead;               /* how many it reads; -1: no mask (the whole buffer is the key) */\n'
                  '\tunsigned _specSeen;              /* configurations specialised so far (the waited-for ones) */\n}')
HMI4_KEEP_OLD = '\t_specialised = [[NSMutableDictionary alloc] init];\n}\n'
HMI4_KEEP_NEW = r'''	_specialised = [[NSMutableDictionary alloc] init];
	_specMaskRead = -1;
	_specSeen = 0;
	if (hm_spec_mask_on() && _specLength <= sizeof(_specMask)) {   /* (redfin hm-int4) HM_SPEC_MASK, on a utility queue */
		[self retain];
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			unsigned char mask[256];
			char m[256 + 1] = "";

			hakios_metal_compile_background(1);
			const int r = hakios_metal_known_read_mask([_fsAir bytes], [_fsAir length], _fsConstants, _fsConstantCount,
								   (unsigned)_specBinding, mask, _specLength);
			hakios_metal_compile_background(0);
			for (unsigned i = 0; r >= 0 && i < _specLength; i++) m[i] = mask[i] ? 'r' : '.';
			if (r >= 0 && (unsigned)r < _specLength) {   /* (all read: nothing to leave out) */
				memcpy(_specMask, mask, _specLength);
				__atomic_store_n(&_specMaskRead, r, __ATOMIC_RELEASE);
			}
			hm_print("spec mask: %s reads %d of %u bytes of its buffer %d: %s%s\n", hakios_metal_program_name(_program), r, _specLength,
				 _specBinding, r >= 0 ? m : "no mask: ", r >= 0 ? "" : hakios_metal_error());
			[self release];
		});
	}
}

/* (redfin hm-int4) the configurations a pipeline was specialised for, against KEY: the nearest one (fewest bytes
 * apart) and the bytes it differs in, into OUT */
static void
hm_spec_nearest(NSDictionary *seen, NSData *key, char *out, size_t size)
{
	const unsigned char *k = [key bytes];
	NSData *best = nil;
	unsigned bestN = ~0u;

	for (NSData *o in seen) {
		const unsigned char *b = [o bytes];
		unsigned n = 0;

		if (o == key || [o isEqual:key] || [o length] != [key length]) continue;
		for (NSUInteger i = 0; i < [key length]; i++) n += b[i] != k[i];
		if (n < bestN) bestN = n, best = o;
	}
	if (best == nil) {
		snprintf(out, size, "the first");
		return;
	}
	int at = snprintf(out, size, "nearest earlier one differs in %u byte(s):", bestN);
	const unsigned char *b = [best bytes];

	for (NSUInteger i = 0; i < [key length] && at > 0 && (size_t)at + 8 < size; i++) {
		if (b[i] != k[i]) at += snprintf(out + at, size - at, " %u(%02x>%02x)", (unsigned)i, b[i], k[i]);
	}
}
'''
HMI4_HELPER_OLD = 'static int\nhm_spec_qos_on(void)\n{'
HMI4_HELPER_NEW = r'''/* (redfin hm-int4) HM_SPEC_MASK: 1 (default) a configuration's key and specialisation without the bytes the fragment
 * never reads; 0 the whole buffer, as before */
static int
hm_spec_mask_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_SPEC_MASK");
		on = e == NULL || e[0] == 0 || atoi(e) != 0;
	}
	return on;
}

static int
hm_spec_qos_on(void)
{'''
HMI4_KEY_OLD = '\tNSData *key = [NSData dataWithBytes:bytes + at length:_specLength];\n'
HMI4_KEY_NEW = r'''	NSData *key;
	if (__atomic_load_n(&_specMaskRead, __ATOMIC_ACQUIRE) >= 0) {   /* (redfin hm-int4) HM_SPEC_MASK: the bytes it never reads left out (as 0) */
		unsigned char kb[256];

		for (unsigned i = 0; i < _specLength; i++) kb[i] = _specMask[i] ? bytes[at + i] : 0;
		key = [NSData dataWithBytes:kb length:_specLength];
	} else {
		key = [NSData dataWithBytes:bytes + at length:_specLength];
	}
'''
HMI4_LOG_OLD = ('\t\t\tfor (NSUInteger i = 0; i < [key length] && i < 256; i++) snprintf(hex + 2 * i, 3, "%02x", kb[i]);\n'
                '\t\t\thm_print("specialise (waited for, hmv7): %s for %u bytes of its buffer %d: %s%u instruction words, not %u, in %.0f ms "\n'
                '\t\t\t\t "(%s; CPU %.0f ms, front end %.0f, folding %.0f); configuration %s\\n",\n')
HMI4_LOG_NEW = ('\t\t\tchar near[400] = "";\n\n'
                '\t\t\tfor (NSUInteger i = 0; i < [key length] && i < 256; i++) snprintf(hex + 2 * i, 3, "%02x", kb[i]);\n'
                '\t\t\t@synchronized(self) {   /* (redfin hm-int4) which configuration this is, and how it differs */\n'
                '\t\t\t\t_specSeen++;\n'
                '\t\t\t\thm_spec_nearest(_specialised, key, near, sizeof(near));\n'
                '\t\t\t}\n'
                '\t\t\thm_print("specialise (waited for, hmv7): %s for %u bytes of its buffer %d: %s%u instruction words, not %u, in %.0f ms "\n'
                '\t\t\t\t "(%s; CPU %.0f ms, front end %.0f, folding %.0f); configuration %s (#%u, %s%s)\\n",\n')
HMI4_LOG2_OLD = '\t\t\t\t cb[5] - ca[5], cb[6] - ca[6], hex);\n'
HMI4_LOG2_NEW = '\t\t\t\t cb[5] - ca[5], cb[6] - ca[6], hex, _specSeen, near, _specMaskRead >= 0 ? "; never-read bytes masked" : "");\n'
# HM_DENY_WORDS=N (default 24576; 0 off): a kept uber shader whose own fragment program is N instruction words or more is
# drawn only through its specialisations, as if HM_DENY named it. CoreAnimation's frag.A2Xghfc (fixed_frag_lph_cpf, 30784
# words on the host, 32064 on the phone) is the one that hung the A620; with a specialisation at ~30 ms on the host
# (HM_FE_MEMO + HM_SPEC_SHRINK) the deny list need not name it. Said once per program, as the other reasons are:
#   "blame: ... HM_DENY_WORDS (32064 fragment words, at least 24576); from now on drawn only through programs specialised ..."
HMI4_DENYW_OLD = '''	} else if (getenv("HM_DENY") != NULL && hm_v7_denied_text(hm_ca_info_of(program))) {
		why = "HM_DENY";
	}
'''
HMI4_DENYW_NEW = '''	} else if (getenv("HM_DENY") != NULL && hm_v7_denied_text(hm_ca_info_of(program))) {
		why = "HM_DENY";
	} else if (hm_deny_words() != 0 && hakios_metal_program_stage_words(program, 1) >= hm_deny_words()) {
		snprintf(text, sizeof(text), "HM_DENY_WORDS (%u fragment words, at least %u)", hakios_metal_program_stage_words(program, 1),
			 hm_deny_words());   /* (redfin hm-int4) */
		why = text;
	}
'''
HMI4_DENYW_HELPER_OLD = '''/* YES when TEXT holds one of HM_DENY's comma-separated words */
'''
HMI4_DENYW_HELPER_NEW = '''/* (redfin hm-int4) HM_DENY_WORDS: the fragment size from which a kept uber shader is drawn only specialised (0: off) */
static unsigned
hm_deny_words(void)
{
	static int words = -1;

	if (words < 0) {
		const char *e = getenv("HM_DENY_WORDS");

		words = e != NULL && e[0] != '\\0' ? atoi(e) : 24576;
		if (words < 0) words = 0;
	}
	return (unsigned)words;
}

/* YES when TEXT holds one of HM_DENY's comma-separated words */
'''
HMI4_EDITS = [
    (HMI4_DENYW_HELPER_OLD, HMI4_DENYW_HELPER_NEW),
    (HMI4_DENYW_OLD, HMI4_DENYW_NEW),
    (HMI4_IVARS_OLD, HMI4_IVARS_NEW),
    (HMI4_HELPER_OLD, HMI4_HELPER_NEW),
    (HMI4_KEEP_OLD, HMI4_KEEP_NEW),
    (HMI4_KEY_OLD, HMI4_KEY_NEW),
    (HMI4_LOG_OLD, HMI4_LOG_NEW),
    (HMI4_LOG2_OLD, HMI4_LOG2_NEW),
]
# HM_POOL_LOG=N (seconds; default 0 off, the env template sets 30 for every process): "pool: N MB: app A, textures T, pieces P
# (free F), programs G, private V, tables B, command streams C" at most every N s, from the buffers' completions, and with
# every HM_PROF report. The core trims idle memory (HM_POOL_TRIM=1, HM_POOL_IDLE_S=10) and on a refused allocation;
# HM_POOL_SOFT_MB=N trims earlier and says so.
HMI4_POOL_OLD = '\tconst double now = hm_now();\n\n\tif (!hm_fps_on()) {\n\t\treturn;\n\t}\n\tpthread_mutex_lock(&hm_fps.lock);\n\tif (hm_fps.since == 0) {\n'
HMI4_POOL_NEW = '\tconst double now = hm_now();\n\n\thm_pool_note(now, 0);   /* (redfin hm-int4) HM_POOL_LOG */\n\tif (!hm_fps_on()) {\n\t\treturn;\n\t}\n\tpthread_mutex_lock(&hm_fps.lock);\n\tif (hm_fps.since == 0) {\n'
HMI4_POOL_FN_OLD = 'static void\nhm_fps_note(BOOL present, double gpu, double latency, hakios_metal_cb *cb)\n{\n'
HMI4_POOL_FN_NEW = r"""/* (redfin hm-int4) HM_POOL_LOG: this process's GPU memory by kind (hakios_metal_pool_stats), every N s at most; FORCE: now */
static void
hm_pool_note(double now, int force)
{
	static double every = -1, last = 0;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (every < 0) {
		const char *e = getenv("HM_POOL_LOG");
		every = e != NULL ? atof(e) : 0;
	}
	if (!force && (every <= 0 || now - last < every)) return;
	pthread_mutex_lock(&lock);
	if (!force && now - last < every) {
		pthread_mutex_unlock(&lock);
		return;
	}
	last = now;
	pthread_mutex_unlock(&lock);
	unsigned long long s[8];
	hakios_metal_pool_stats(s);
	const unsigned long long known = s[0] + s[1] + s[2] + s[3] + s[4] + s[5];
	hm_print("pool: %.1f MB: app %.1f, textures %.1f, pieces %.1f (free %.1f), programs %.1f, private %.1f, tables %.1f, command streams %.1f\n",
		 s[7] / 1048576.0, s[0] / 1048576.0, s[1] / 1048576.0, s[2] / 1048576.0, s[6] / 1048576.0, s[3] / 1048576.0, s[4] / 1048576.0,
		 s[5] / 1048576.0, s[7] > known ? (s[7] - known) / 1048576.0 : 0.0);
}

static void
hm_fps_note(BOOL present, double gpu, double latency, hakios_metal_cb *cb)
{
"""
# HM_DEADFILL's count on the HM_PROF line
HMI4_PROF_OLD = '''			 hm_presents_seen ? "frames = presentDrawable" : "frames = buffers that drew the display's IOSurface",
			 hm_prof.other != 0 ? " (passes past the classes not listed)" : "");
'''
HMI4_PROF_NEW = '''			 hm_presents_seen ? "frames = presentDrawable" : "frames = buffers that drew the display's IOSurface",
			 hm_prof.other != 0 ? " (passes past the classes not listed)" : "");
		{   /* (redfin hm-int4) HM_DEADFILL: draws taken back in these seconds; and the pool */
			static unsigned long long droppedBefore;
			const unsigned long long dropped = hakios_metal_fills_dropped();
			const char *df = getenv("HM_DEADFILL");

			hm_print("prof: fills dropped %llu (%.1f a frame; HM_DEADFILL=%s)\\n", dropped - droppedBefore, (dropped - droppedBefore) / frames,
				 df != NULL ? df : "1");
			droppedBefore = dropped;
			hm_pool_note(now, 1);
		}
'''
HMI4_EDITS += [
    ('static BOOL hm_fps_on(void);', 'static BOOL hm_fps_on(void);\nstatic void hm_pool_note(double now, int force);   /* (redfin hm-int4) HM_POOL_LOG */\n'
     'static int hm_prof_level(void);   /* (ws-hm-sbar) HM_PROF=3 */'),
    ('\t\thakios_metal_cb_set_profile_draws(_cb, hm_fps_draws());',
     '\t\thakios_metal_cb_set_profile_draws(_cb, hm_fps_draws() || hm_prof_level() >= 3);   /* (ws-hm-sbar) HM_PROF=3 */'),
    (HMI4_POOL_FN_OLD, HMI4_POOL_FN_NEW),
    (HMI4_POOL_OLD, HMI4_POOL_NEW),
    (HMI4_PROF_OLD, HMI4_PROF_NEW),
]
EDITS += HMI4_EDITS


# ---- hm-int5 ------------------------------------------------------------------------------------------------------
# (hm-int5 item 1) HM_DEPTH16=1 (default): Depth16Unorm (250) is a depth attachment format a render pipeline may name
# (PridePosterExtension's), made like Depth32Float (render-target usage, tiled) by usage Unknown and the heap's size
# rule; the core lays it out and draws it (texture.cpp depth16On). 0: refused as hm-int4.
# -setDepthBias:slopeScale:clamp: is kept in the pass (depth_bias, depth_slope_scale, depth_bias_clamp) and the core
# applies it to each later draw with a depth attachment (HM_DEPTH_BIAS=0 there: never).
HMI5_EDITS = [
    ('\tif ([d depthAttachmentPixelFormat] != MTLPixelFormatInvalid && [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float &&\n'
     '\t    [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float_Stencil8) {\n',
     '\tif ([d depthAttachmentPixelFormat] != MTLPixelFormatInvalid && [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float &&\n'
     '\t    [d depthAttachmentPixelFormat] != MTLPixelFormatDepth32Float_Stencil8 &&\n'
     '\t    !([d depthAttachmentPixelFormat] == MTLPixelFormatDepth16Unorm && hakios_metal_depth16_on())) {   /* (redfin hm-int5) HM_DEPTH16 */\n'),
    ('"(260) are the depth formats served", (unsigned long)[d depthAttachmentPixelFormat]];',
     '"(260) are the depth formats served (Depth16Unorm, 250, too unless HM_DEPTH16=0)", (unsigned long)[d depthAttachmentPixelFormat]];'),
    ('\t\tif ([descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8) {\n'
     '\t\t\tusage = MTLTextureUsageRenderTarget;',
     '\t\tif ([descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8 ||\n'
     '\t\t    [descriptor pixelFormat] == MTLPixelFormatDepth16Unorm) {   /* (redfin hm-int5) */\n'
     '\t\t\tusage = MTLTextureUsageRenderTarget;'),
    ('\tconst BOOL depth = [descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8;\n',
     '\tconst BOOL depth = [descriptor pixelFormat] == MTLPixelFormatDepth32Float || [descriptor pixelFormat] == MTLPixelFormatDepth32Float_Stencil8 ||\n'
     '\t\t\t   [descriptor pixelFormat] == MTLPixelFormatDepth16Unorm;   /* (redfin hm-int5) */\n'),
    ('\tif (depthBias != 0.0f || slopeScale != 0.0f || clamp != 0.0f) {\n'
     '\t\tHM_ONCE("a depth bias (%g, slope %g, clamp %g) was set; there is no depth attachment, so it is not applied\\n",\n'
     '\t\t\t(double)depthBias, (double)slopeScale, (double)clamp);\n'
     '\t}\n',
     '\t/* (redfin hm-int5) kept for each later draw; the core applies it with a depth attachment (HM_DEPTH_BIAS) */\n'
     '\t_pass.depth_bias = depthBias;\n'
     '\t_pass.depth_slope_scale = slopeScale;\n'
     '\t_pass.depth_bias_clamp = clamp;\n'),
]
# (hm-int5 item 2) HM_SPEC_STEAL=1 (default): a configuration queued on the specialiser's queue (a pipeline that may still
# draw its own program asked for it) that a draw must now wait for (the program was denied or blamed since) is compiled
# by the waiting thread itself, at its own QoS, instead of the waiter polling behind the user-initiated serial queue and
# whatever it holds before that one; the queued block finds it taken and does nothing. 0: as hm-int4.
HMI5_EDITS += [
    ('static int\nhm_spec_qos_on(void)\n{',
     'static int\nhm_spec_steal_on(void)\n{\n\tstatic int on = -1;\n\n\tif (on < 0) {\n\t\tconst char *e = getenv("HM_SPEC_STEAL");\n'
     '\t\ton = e == NULL || e[0] == 0 || atoi(e) != 0;\n\t}\n\treturn on;\n}\n\n'
     '/* (redfin hm-int5) the mark of a configuration queued and not yet started (HM_SPEC_STEAL) */\n'
     'static id\nhm_spec_queued(void)\n{\n\tstatic id mark;\n\tstatic dispatch_once_t once;\n\n'
     '\tdispatch_once(&once, ^{ mark = [[NSObject alloc] init]; });\n\treturn mark;\n}\n\n'
     'static int\nhm_spec_qos_on(void)\n{'),
    ('\t\t} else if (have == nil && [_specialised count] < 64) {\n\t\t\t[_specialised setObject:[NSNull null] forKey:key];\n\t\t\tstart = YES;\n',
     '\t\t} else if (have == nil && [_specialised count] < 64) {\n\t\t\t[_specialised setObject:(hm_spec_steal_on() ? hm_spec_queued() : [NSNull null]) forKey:key];\n\t\t\tstart = YES;\n'
     '\t\t} else if (wait && have == hm_spec_queued()) {   /* (redfin hm-int5) HM_SPEC_STEAL: queued, not started: this thread compiles it */\n'
     '\t\t\t[_specialised setObject:[NSNull null] forKey:key];\n\t\t\tmine = YES;\n'),
    ('\t\tdispatch_async(compiling, ^{\n\t\t\tstruct timespec t0, t1;\n\n\t\t\tclock_gettime(CLOCK_MONOTONIC, &t0);\n',
     '\t\tdispatch_async(compiling, ^{\n\t\t\tstruct timespec t0, t1;\n\n'
     '\t\t\tif (hm_spec_steal_on()) {   /* (redfin hm-int5) taken over by a waiting thread: nothing left to do */\n'
     '\t\t\t\tBOOL taken;\n\n\t\t\t\t@synchronized(self) {\n\t\t\t\t\ttaken = [_specialised objectForKey:key] != hm_spec_queued();\n'
     '\t\t\t\t\tif (!taken) [_specialised setObject:[NSNull null] forKey:key];\n\t\t\t\t}\n'
     '\t\t\t\tif (taken) {\n\t\t\t\t\tif (hm_fps_on()) hm_print("specialise: %s: a waiting draw compiled it first (HM_SPEC_STEAL)\\n", hakios_metal_program_name(_program));\n'
     '\t\t\t\t\t[self release];\n\t\t\t\t\treturn;\n\t\t\t\t}\n\t\t\t}\n'
     '\t\t\tclock_gettime(CLOCK_MONOTONIC, &t0);\n'),
]
EDITS += HMI5_EDITS


# (redfin hm-int8) HM_PROF>=2's pace line: where a frame's CPU-side time goes, per frame (a frame = the command buffer that
# drew the display's IOSurface, _hmFrame, or one presented). Every 5 s: "prof: pace ..." (bench/plugin.py docstring).
HM_PACE_HELPERS = r'''/* (redfin hm-int8) HM_PROF>=2: per frame (a buffer that drew the display's IOSurface): blocked (the buffer slot wait in
 * -initWithHakiosQueue: and the waitUntilScheduled / waitUntilCompleted calls since the last frame's commit), the gap
 * from the last frame's commit to this frame's first buffer made (time outside hm: CoreAnimation's swap ring wait, its
 * layout), the CPU encode from the first buffer made to the frame's commit, commit -> kernel submit returned (held
 * commits, the core's stream copy, the kernel call with its scanout gate), that kernel call alone, commit -> completed,
 * and frames committed while an earlier frame was still unretired. A line "prof: pace" every 5 s. */
#define HM_PF_N 512
static struct {
	pthread_mutex_t lock;
	double since, firstMade, lastCommit, blocked, slot, waits;
	int open, inflight, maxInflight;
	unsigned frames, overlapped, done, noKernel;
	float blk[HM_PF_N], gap[HM_PF_N], enc[HM_PF_N], c2k[HM_PF_N], kcall[HM_PF_N], c2d[HM_PF_N];
	unsigned nb, nd;
	double slotSum, waitSum;
} hm_pf = { PTHREAD_MUTEX_INITIALIZER };

static int64_t
hm_pf_cont_ns(void)    /* the core's transport clock (iokit_transport.cpp nowNs: mach_continuous_time) */
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) mach_timebase_info(&tb);
	return (int64_t)(mach_continuous_time() * tb.numer / tb.denom);
}

/* a command buffer made: began = before the slot wait, made = after it */
static void
hm_pf_made(double began, double made)
{
	if (hm_prof_level() < 2) return;
	pthread_mutex_lock(&hm_pf.lock);
	if (!hm_pf.open) {
		hm_pf.open = 1;
		hm_pf.firstMade = began;
		hm_pf.slot = hm_pf.waits = 0;
	}
	hm_pf.slot += made - began;
	pthread_mutex_unlock(&hm_pf.lock);
}

static void
hm_pf_wait(double secs)
{
	if (hm_prof_level() < 2) return;
	pthread_mutex_lock(&hm_pf.lock);
	hm_pf.waits += secs;
	pthread_mutex_unlock(&hm_pf.lock);
}

/* a frame's buffer committed (the -commit call's time): its commit time in the transport's clock */
static int64_t
hm_pf_commit(double at)
{
	if (hm_prof_level() < 2) return 0;
	const int64_t ns = hm_pf_cont_ns();
	pthread_mutex_lock(&hm_pf.lock);
	if (hm_pf.nb < HM_PF_N) {
		const unsigned i = hm_pf.nb++;
		hm_pf.blk[i] = (float)((hm_pf.open ? hm_pf.slot + hm_pf.waits : 0) * 1e3);
		hm_pf.gap[i] = (float)(hm_pf.open && hm_pf.lastCommit != 0 && hm_pf.firstMade > hm_pf.lastCommit ?
				       (hm_pf.firstMade - hm_pf.lastCommit) * 1e3 : 0);
		hm_pf.enc[i] = (float)(hm_pf.open && at > hm_pf.firstMade ? (at - hm_pf.firstMade) * 1e3 : 0);
	}
	if (hm_pf.open) hm_pf.slotSum += hm_pf.slot, hm_pf.waitSum += hm_pf.waits;
	hm_pf.frames++;
	if (hm_pf.inflight > 0) hm_pf.overlapped++;
	hm_pf.inflight++;
	if (hm_pf.inflight > hm_pf.maxInflight) hm_pf.maxInflight = hm_pf.inflight;
	hm_pf.lastCommit = at;
	hm_pf.open = 0;
	pthread_mutex_unlock(&hm_pf.lock);
	return ns;
}

static int
hm_pf_cmp(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static void
hm_pf_stat(float *v, unsigned n, double *mean, double *p95)
{
	double sum = 0;

	*mean = *p95 = 0;
	if (n == 0) return;
	for (unsigned i = 0; i < n; i++) sum += v[i];
	qsort(v, n, sizeof(v[0]), hm_pf_cmp);
	*mean = sum / n;
	*p95 = v[(n * 95 + 99) / 100 - 1];
}

/* a frame's buffer completed (the completion handler): commit -> kernel submit, the kernel call, commit -> completed */
static void
hm_pf_done(hakios_metal_cb *cb, int64_t commitNs)
{
	if (hm_prof_level() < 2 || commitNs == 0) return;
	int64_t cns = 0, kin = 0, kout = 0;
	const int sent = hakios_metal_cb_submit_times(cb, &cns, &kin, &kout);
	const int64_t now = hm_pf_cont_ns();
	const double t = hm_now();
	pthread_mutex_lock(&hm_pf.lock);
	if (hm_pf.inflight > 0) hm_pf.inflight--;
	if (hm_pf.since == 0) hm_pf.since = t;
	if (hm_pf.nd < HM_PF_N) {
		const unsigned i = hm_pf.nd++;
		hm_pf.c2k[i] = sent == 0 && kout > commitNs ? (float)((kout - commitNs) / 1e6) : 0;
		hm_pf.kcall[i] = sent == 0 && kout > kin ? (float)((kout - kin) / 1e6) : 0;
		hm_pf.c2d[i] = now > commitNs ? (float)((now - commitNs) / 1e6) : 0;
	}
	hm_pf.done++;
	if (sent != 0) hm_pf.noKernel++;
	if (t - hm_pf.since >= 5.0) {
		double m[6], q[6];
		const double secs = t - hm_pf.since, fr = hm_pf.frames ? hm_pf.frames : 1;

		hm_pf_stat(hm_pf.blk, hm_pf.nb, &m[0], &q[0]);
		hm_pf_stat(hm_pf.gap, hm_pf.nb, &m[1], &q[1]);
		hm_pf_stat(hm_pf.enc, hm_pf.nb, &m[2], &q[2]);
		hm_pf_stat(hm_pf.c2k, hm_pf.nd, &m[3], &q[3]);
		hm_pf_stat(hm_pf.kcall, hm_pf.nd, &m[4], &q[4]);
		hm_pf_stat(hm_pf.c2d, hm_pf.nd, &m[5], &q[5]);
		hm_print("prof: pace %.1f s: %u frames (%.1f fps) | mean/p95 ms a frame: blocked %.2f/%.2f (buffer slot %.2f, "
			 "waitUntil* %.2f) | gap last commit -> first buffer made %.2f/%.2f | encode first buffer made -> commit "
			 "%.2f/%.2f | commit -> kernel submit %.2f/%.2f (kernel call %.2f/%.2f) | commit -> completed %.2f/%.2f | "
			 "committed with an earlier frame unretired %u of %u (most in flight %d) | not sent %u\n",
			 secs, hm_pf.frames, hm_pf.frames / secs, m[0], q[0], hm_pf.slotSum * 1e3 / fr, hm_pf.waitSum * 1e3 / fr,
			 m[1], q[1], m[2], q[2], m[3], q[3], m[4], q[4], m[5], q[5], hm_pf.overlapped, hm_pf.frames,
			 hm_pf.maxInflight, hm_pf.noKernel);
		hm_pf.since = t;
		hm_pf.frames = hm_pf.overlapped = hm_pf.done = hm_pf.noKernel = 0;
		hm_pf.nb = hm_pf.nd = 0;
		hm_pf.slotSum = hm_pf.waitSum = 0;
		hm_pf.maxInflight = hm_pf.inflight;
	}
	pthread_mutex_unlock(&hm_pf.lock);
}

'''
HM_PACE_EDITS = [
    ('#define HM_PROF_CLASSES 24\n',
     '/* (redfin hm-int8) HM_PROF>=2 pace line (defined before hm_completed) */\n'
     'static void hm_pf_made(double began, double made);\nstatic void hm_pf_wait(double secs);\n'
     'static int64_t hm_pf_commit(double at);\nstatic void hm_pf_done(hakios_metal_cb *cb, int64_t commitNs);\n'
     '#define HM_PROF_CLASSES 24\n'),
    ('static void\nhm_completed(hakios_metal_cb *cb, void *context)\n{\n',
     HM_PACE_HELPERS + 'static void\nhm_completed(hakios_metal_cb *cb, void *context)\n{\n'),
    ('\tself = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];\n',
     '\tconst double hmPfBegan = hm_now();   /* (redfin hm-int8) the slot wait below, for HM_PROF>=2\'s pace line */\n'
     '\tself = [super initWithQueue:queue retainedReferences:YES synchronousDebugMode:NO];\n'
     '\thm_pf_made(hmPfBegan, hm_now());\n'),
    ('@public\n\tdouble _hmCommitSecs;',
     '@public\n\tint64_t _hmPfCommitNs;                   /* (redfin hm-int8) HM_PROF>=2 pace: a frame\'s commit (transport clock) */\n\tdouble _hmCommitSecs;'),
    ('\thm_tr_committed(_committedAt - _createdAt);   /* (redfin hmv4) */\n',
     '\thm_tr_committed(_committedAt - _createdAt);   /* (redfin hmv4) */\n'
     '\tif (_hmFrame) self->_hmPfCommitNs = hm_pf_commit(_committedAt);   /* (redfin hm-int8) */\n'),
    ('\t\t__sync_fetch_and_add(&hm_sched_waits, 1);\n\t\tdispatch_semaphore_wait(_sent, DISPATCH_TIME_FOREVER);\n',
     '\t\tconst double hmPfW = hm_now();   /* (redfin hm-int8) */\n'
     '\t\t__sync_fetch_and_add(&hm_sched_waits, 1);\n\t\tdispatch_semaphore_wait(_sent, DISPATCH_TIME_FOREVER);\n'
     '\t\thm_pf_wait(hm_now() - hmPfW);\n'),
    ('\t\thm_fps_waited(hm_now() - began);\n',
     '\t\thm_fps_waited(hm_now() - began);\n\t\thm_pf_wait(hm_now() - began);   /* (redfin hm-int8) */\n'),
    ('\t\t     hm_now() - [self hakiosCreatedAt]);   /* (redfin hmv4) HM_TRACE */\n',
     '\t\t     hm_now() - [self hakiosCreatedAt]);   /* (redfin hmv4) HM_TRACE */\n'
     '\tif (self->_hmFrame) hm_pf_done(cb, self->_hmPfCommitNs);   /* (redfin hm-int8) HM_PROF>=2 pace */\n'),
]

EDITS += HM_PACE_EDITS   # (redfin hm-int8)


# (ws-hm-alib) HM_DD=1: what a frame's draws sample, where each texture came from, and what it holds. For the App
# Library's settled backdrop (black when static): one capture tells a cached render target, an IOSurface another process
# wrote (SpringBoard's MPS blur, VideoToolbox's VTMTSComputeFunction), a blit copy or a compute output apart, and says
# whether the texture's memory is zero when the draw samples it.
#   HM_DD=1          on (off: nothing collected, no cost)
#   HM_DD_PASSES=5   a buffer of 1..N render passes is printed (at most one every HM_DD_EVERY=3 s, HM_DD_MAX=20 in all);
#                    0: whatever its number of passes (the App Library's settled redraw has more than 5)
#   scope            the env file's plain HM_DD=1 line is backboardd's alone (bench/m2shim.m shim_env): SpringBoard:HM_DD=1
#                    (NAME:KEY=VALUE, getprogname) or *:HM_DD=1 for the others; the "dd: on" line names the process
#   lines            every line is one hm_print with the log's prefix, a texture's ("dd:     fs t3 = ...") too
#   trigger file     /var/local/hmetal/ca/dd-go (HM_DD_TRIGGER): when its mtime changes, the next HM_DD_FORCE=3 buffers
#                    are printed whatever their passes
#   compute          every buffer with a dispatch is printed (HM_DD_MAX_COMPUTE=60 in all), and its dispatches' textures
#                    probed again when it completes ("dd: buffer N done:")
# Lines start "dd: ". A texture: object, core, WxH, fmt, usage, storage kind, origin (0 own, 1 IOSurface, 2 buffer,
# 3 compressed), the surface (id, size, fourcc, seed, SHADOW), a view's parent, its label, the last write this process
# made to it (render pass / blit / compute / replaceRegion, which buffer, how long ago) and a probe: how many of 9 texels
# on a 3x3 grid are not all-zero bytes, and the centre texel's bytes.
HM_DD_HELPERS = r'''/* (ws-hm-alib) HM_DD: see bench/plugin.py */
static pthread_mutex_t hm_dd_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long hm_dd_seq;   /* command buffers this process committed */
#define HM_DD_W 4096
static struct { const void *t; unsigned long long seq; double at; char kind[40]; unsigned a, b; } hm_dd_w[HM_DD_W];
static NSMutableDictionary *hm_dd_bufs;   /* NSValue(command buffer) -> { lines, passes, probe, targets } */
static unsigned long long hm_dd_count;    /* "dd: on" lines and buffers printed (the counter the capture checks) */

static int
hm_dd_env(const char *name, int dflt)
{
	const char *e = getenv(name);
	return e != NULL && e[0] != '\0' ? atoi(e) : dflt;
}

static int
hm_dd_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_DD");
		on = e != NULL && e[0] == '1';
		if (on) {
			const char *tr = getenv("HM_DD_TRIGGER");
			const int np = hm_dd_env("HM_DD_PASSES", 5);
			char passes[24];
			if (np > 0) snprintf(passes, sizeof(passes), "1..%d", np); else strlcpy(passes, "any number of", sizeof(passes));
			/* (ws-hm-alib) the process named: the env file's plain HM_DD=1 is backboardd's alone (m2shim.m shim_env);
			 * SpringBoard:HM_DD=1 or *:HM_DD=1 for the others */
			hm_print("dd: on (HM_DD=1 in %s): buffers of %s render passes (HM_DD_PASSES, 0 any) every %d s (at most %d), every buffer with a dispatch (at most %d), "
				 "the next %d after %s changes\n", getprogname(), passes, hm_dd_env("HM_DD_EVERY", 3), hm_dd_env("HM_DD_MAX", 20),
				 hm_dd_env("HM_DD_MAX_COMPUTE", 60), hm_dd_env("HM_DD_FORCE", 3), tr != NULL ? tr : "/var/local/hmetal/ca/dd-go");
		}
	}
	return on;
}

static void
hm_dd_note(const void *t, const char *kind, unsigned a, unsigned b)
{
	if (t == NULL || !hm_dd_on()) return;
	pthread_mutex_lock(&hm_dd_lock);
	unsigned i = (unsigned)(((uintptr_t)t >> 4) % HM_DD_W);
	for (unsigned k = 0; k < 32; k++) {
		const unsigned j = (i + k) % HM_DD_W;
		if (hm_dd_w[j].t == t || hm_dd_w[j].t == NULL) { i = j; break; }
	}
	hm_dd_w[i].t = t;
	hm_dd_w[i].seq = hm_dd_seq;
	hm_dd_w[i].at = hm_now();
	strlcpy(hm_dd_w[i].kind, kind != NULL ? kind : "?", sizeof(hm_dd_w[i].kind));
	hm_dd_w[i].a = a, hm_dd_w[i].b = b;
	pthread_mutex_unlock(&hm_dd_lock);
}

static void
hm_dd_writer(const void *t, char *out, size_t n)
{
	snprintf(out, n, "none seen in this process");
	if (t == NULL) return;
	pthread_mutex_lock(&hm_dd_lock);
	const unsigned i = (unsigned)(((uintptr_t)t >> 4) % HM_DD_W);
	for (unsigned k = 0; k < 32; k++) {
		const unsigned j = (i + k) % HM_DD_W;
		if (hm_dd_w[j].t == NULL) break;
		if (hm_dd_w[j].t == t) {
			snprintf(out, n, "%s %u/%u in buffer %llu (%llu buffers ago, %.2f s)", hm_dd_w[j].kind, hm_dd_w[j].a, hm_dd_w[j].b,
				 hm_dd_w[j].seq + 1, hm_dd_seq - hm_dd_w[j].seq, hm_now() - hm_dd_w[j].at);
			break;
		}
	}
	pthread_mutex_unlock(&hm_dd_lock);
}

/* 9 texels on a 3x3 grid: how many are not all zero bytes, and the centre's bytes */
static NSString *
hm_dd_probe(hakios_metal_texture *t)
{
	if (t == NULL) return @"-";
	const unsigned origin = hakios_metal_texture_origin(t), bpp = hakios_metal_texture_bytes_per_pixel(t);
	const unsigned w = hakios_metal_texture_width(t), h = hakios_metal_texture_height(t);
	if (origin == 3 || bpp == 0 || bpp > 16 || w == 0 || h == 0 || hakios_metal_texture_samples(t) != 1) return @"(not probed)";
	unsigned nz = 0;
	char hex[40] = "";
	for (unsigned j = 0; j < 3; j++) {
		for (unsigned i = 0; i < 3; i++) {
			uint8_t px[16];
			memset(px, 0, sizeof(px));
			if (hakios_metal_texture_read(t, w * (1 + 2 * i) / 6, h * (1 + 2 * j) / 6, 1, 1, px, 16) != 0) return @"(read refused)";
			BOOL any = NO;
			for (unsigned k = 0; k < bpp; k++) any = any || px[k] != 0;
			nz += any;
			if (i == 1 && j == 1) {
				for (unsigned k = 0; k < bpp; k++) snprintf(hex + 2 * k, 3, "%02x", px[k]);
			}
		}
	}
	return [NSString stringWithFormat:@"%u/9 not zero, centre %s", nz, hex];
}

static NSString *
hm_dd_fourcc(uint32_t f)
{
	char c[5] = { (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f, 0 };
	for (int i = 0; i < 4; i++) if (c[i] < 32 || c[i] > 126) return [NSString stringWithFormat:@"0x%x", f];
	return [NSString stringWithUTF8String:c];
}

@interface HakiosMetalTexture (HMDD)
- (NSString *)hmddDescribe;
- (hakios_metal_texture *)hmddCore;
@end

@implementation HakiosMetalTexture (HMDD)
- (hakios_metal_texture *)hmddCore
{
	return _texture;
}
- (NSString *)hmddDescribe
{
	char wr[200];
	hm_dd_writer(_texture, wr, sizeof(wr));
	NSMutableString *s = [NSMutableString stringWithFormat:@"%p core %p %lux%lu fmt %lu usage 0x%lx kind %u origin %u", self, _texture,
		(unsigned long)[self width], (unsigned long)[self height], (unsigned long)[self pixelFormat], (unsigned long)_usage,
		[self hakiosStorageKind], _texture != NULL ? hakios_metal_texture_origin(_texture) : 99u];
	if (_iosurface != NULL) {
		[s appendFormat:@" surface %u (%lux%lu %@ seed %u plane %lu%s)", IOSurfaceGetID(_iosurface), (unsigned long)IOSurfaceGetWidth(_iosurface),
			(unsigned long)IOSurfaceGetHeight(_iosurface), hm_dd_fourcc(IOSurfaceGetPixelFormat(_iosurface)), IOSurfaceGetSeed(_iosurface),
			(unsigned long)_iosurfacePlane, _shadow ? " SHADOW" : ""];
	}
	if (_parent != nil) {
		char pw[200];
		hm_dd_writer([_parent hmddCore], pw, sizeof(pw));
		[s appendFormat:@" view of %p (level %lu slice %lu%s%s; parent's writer: %s)", _parent, (unsigned long)_parentLevel,
			(unsigned long)_parentSlice, _ownsView ? " own core" : "", _swizzled ? " swizzled" : "", pw];
	}
	if (_ycbcr != 0) [s appendFormat:@" ycbcr %d%s", _ycbcr, _ycbcrNative ? " native" : ""];
	NSString *label = [self respondsToSelector:@selector(label)] ? [(id<MTLTexture>)self label] : nil;
	if (label != nil) [s appendFormat:@" label '%@'", label];
	[s appendFormat:@"; writer: %s; probe %@", wr, hm_dd_probe(_texture)];
	return s;
}
@end

static NSMutableDictionary *
hm_dd_entry(id cb)
{
	NSValue *key = [NSValue valueWithPointer:(const void *)cb];
	if (hm_dd_bufs == nil) hm_dd_bufs = [[NSMutableDictionary alloc] init];
	NSMutableDictionary *e = [hm_dd_bufs objectForKey:key];
	if (e == nil) {
		e = [NSMutableDictionary dictionaryWithObjectsAndKeys:[NSMutableArray array], @"lines", [NSMutableArray array], @"probe",
			[NSMutableArray array], @"targets", [NSNumber numberWithUnsignedInt:0], @"passes", [NSNumber numberWithUnsignedInt:0], @"draws", nil];
		[hm_dd_bufs setObject:e forKey:key];
	}
	return e;
}

static void
hm_dd_line(id cb, NSString *line, BOOL pass, BOOL draw)
{
	if (!hm_dd_on() || cb == nil) return;
	@synchronized ([HakiosMetalTexture class]) {
		NSMutableDictionary *e = hm_dd_entry(cb);
		NSMutableArray *lines = [e objectForKey:@"lines"];
		if (pass) [e setObject:[NSNumber numberWithUnsignedInt:[[e objectForKey:@"passes"] unsignedIntValue] + 1] forKey:@"passes"];
		if (draw) [e setObject:[NSNumber numberWithUnsignedInt:[[e objectForKey:@"draws"] unsignedIntValue] + 1] forKey:@"draws"];
		if ([lines count] < 600) [lines addObject:line];
	}
}

static void
hm_dd_keep(id cb, NSString *what, id texture)
{
	if (!hm_dd_on() || cb == nil || texture == nil) return;
	@synchronized ([HakiosMetalTexture class]) {
		[[hm_dd_entry(cb) objectForKey:what] addObject:texture];
	}
}

static unsigned
hm_dd_passes(id cb)
{
	if (!hm_dd_on() || cb == nil) return 0;
	@synchronized ([HakiosMetalTexture class]) {
		return [[hm_dd_entry(cb) objectForKey:@"passes"] unsignedIntValue];
	}
}

/* -commit: print the buffer's lines or drop them */
static void
hm_dd_commit(id cb)
{
	if (!hm_dd_on()) return;
	static double lastCheck, lastAuto;
	static long baseMtime = -1;
	static int forced, autoN, computeN;
	const double now = hm_now();
	unsigned long long seq;

	pthread_mutex_lock(&hm_dd_lock);
	seq = ++hm_dd_seq;
	pthread_mutex_unlock(&hm_dd_lock);
	NSDictionary *e;
	@synchronized ([HakiosMetalTexture class]) {
		NSValue *key = [NSValue valueWithPointer:(const void *)cb];
		e = [[[hm_dd_bufs objectForKey:key] retain] autorelease];
		if (e != nil) [hm_dd_bufs removeObjectForKey:key];
		if (now - lastCheck >= 0.25) {
			struct stat st;
			const char *tr = getenv("HM_DD_TRIGGER");
			const long m = stat(tr != NULL ? tr : "/var/local/hmetal/ca/dd-go", &st) == 0 ? (long)st.st_mtime * 1000 + (long)(st.st_mtimespec.tv_nsec / 1000000) : 0;
			lastCheck = now;
			if (baseMtime < 0) {
				baseMtime = m;
			} else if (m != baseMtime) {
				baseMtime = m;
				forced = hm_dd_env("HM_DD_FORCE", 3);
				hm_print("dd: trigger seen: the next %d buffers are printed (buffer %llu)\n", forced, seq + 1);
			}
		}
	}
	if (e == nil) return;
	const unsigned passes = [[e objectForKey:@"passes"] unsignedIntValue], draws = [[e objectForKey:@"draws"] unsignedIntValue];
	NSArray *probe = [e objectForKey:@"probe"], *targets = [e objectForKey:@"targets"], *lines = [e objectForKey:@"lines"];
	const char *why = NULL;
	if (forced > 0) {
		forced--, why = "trigger";
	} else if ([probe count] != 0 && computeN < hm_dd_env("HM_DD_MAX_COMPUTE", 60)) {
		computeN++, why = "dispatch";
	} else if (passes >= 1 && (hm_dd_env("HM_DD_PASSES", 5) <= 0 || (int)passes <= hm_dd_env("HM_DD_PASSES", 5)) && now - lastAuto >= hm_dd_env("HM_DD_EVERY", 3) &&
		   autoN < hm_dd_env("HM_DD_MAX", 20)) {
		autoN++, lastAuto = now, why = "few passes";
	}
	if (why == NULL) return;
	hm_dd_count++;
	hm_print("dd: buffer %llu (%s; dumped %llu): %u render passes, %u draws, %lu dispatched textures\n", seq, why, hm_dd_count, passes, draws,
		 (unsigned long)[probe count]);
	/* (ws-hm-alib) one hm_print a line: a draw's or a dispatch's texture lines ("dd:     fs t3 = ...") carry the log's
	 * process-and-time prefix too, so a grep for the prefix keeps them (k4-1115's dd-bbd.txt had lost every one) */
	for (NSString *l in lines) {
		for (NSString *piece in [l componentsSeparatedByString:@"\n"]) {
			hm_print("%s%s\n", [piece hasPrefix:@"dd:"] ? "" : "dd: ", [piece UTF8String]);
		}
	}
	if ([probe count] != 0 || [targets count] != 0) {
		NSArray *p = [[probe copy] autorelease], *t = [[targets copy] autorelease];
		[(id<MTLCommandBuffer>)cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
			(void)done;
			for (HakiosMetalTexture *x in p) hm_print("dd: buffer %llu done: dispatched %s\n", seq, [[x hmddDescribe] UTF8String]);
			for (HakiosMetalTexture *x in t) hm_print("dd: buffer %llu done: target %s\n", seq, [[x hmddDescribe] UTF8String]);
		}];
	}
}

@implementation HakiosMetalDepthStencilState
'''

HM_DD_EDITS = [
    ('static int hm_prof_level(void);   /* (ws-hm-sbar) HM_PROF=3 */',
     'static void hm_dd_note(const void *t, const char *kind, unsigned a, unsigned b);   /* (ws-hm-alib) HM_DD */\n'
     'static int hm_prof_level(void);   /* (ws-hm-sbar) HM_PROF=3 */'),
    ('@implementation HakiosMetalDepthStencilState\n', HM_DD_HELPERS),
    # -commit: the buffer's lines printed or dropped (after the committed-twice check)
    ('\t_committed = YES;\n\t_committedAt = hm_now();\n',
     '\thm_dd_commit(self);   /* (ws-hm-alib) HM_DD */\n\t_committed = YES;\n\t_committedAt = hm_now();\n'),
    # a render pass: its target, actions, and the target noted as written
    ('\t\tif (hm_frame_target(texture)) _hmFrame = YES;   /* (redfin hmv3) */\n',
     '\t\tif (hm_frame_target(texture)) _hmFrame = YES;   /* (redfin hmv3) */\n'
     '\t\tif (hm_dd_on()) {   /* (ws-hm-alib) HM_DD */\n'
     '\t\t\tunsigned more = 0;\n'
     '\t\t\tfor (NSUInteger i = 1; i < 8; i++) more += [[descriptor colorAttachments] objectAtIndexedSubscript:i].texture != nil;\n'
     '\t\t\thm_dd_line(self, [NSString stringWithFormat:@"pass %u: load %u store %u clear (%.3f %.3f %.3f %.3f), %u more colour attachments, depth %s; target %@",\n'
     '\t\t\t\thm_dd_passes(self) + 1, pass.load_action, pass.store_action, pass.clear_color[0], pass.clear_color[1], pass.clear_color[2],\n'
     '\t\t\t\tpass.clear_color[3], more, [[descriptor depthAttachment] texture] != nil ? "yes" : "no",\n'
     '\t\t\t\ttexture != nil ? [texture hmddDescribe] : @"none"], YES, NO);\n'
     '\t\t\thm_dd_keep(self, @"targets", texture);\n'
     '\t\t\tif (texture != nil) hm_dd_note([texture hmddCore], "render pass load/store", pass.load_action, pass.store_action);\n'
     '\t\t}\n'),
    # a draw: the pipeline, the vertex buffer's first floats, every texture it samples
    ('\thm_dump_config(hakios_metal_program_name(program), fs, _fsOffsets, _fsHighest);\n',
     '\thm_dump_config(hakios_metal_program_name(program), fs, _fsOffsets, _fsHighest);\n'
     '\tif (hm_dd_on()) {   /* (ws-hm-alib) HM_DD */\n'
     '\t\tNSString *pl = [(id<MTLRenderPipelineState>)_pipeline label];\n'
     '\t\tNSMutableString *d = [NSMutableString stringWithFormat:@"  draw in pass %u: %@ [%s] prim %lu count %lu inst %lu into %ux%u",\n'
     '\t\t\thm_dd_passes(_commandBuffer), pl != nil ? pl : @"-", hakios_metal_program_name(program), (unsigned long)primitive,\n'
     '\t\t\t(unsigned long)count, (unsigned long)instances, _pass.color0 != NULL ? hakios_metal_texture_width(_pass.color0) : 0,\n'
     '\t\t\t_pass.color0 != NULL ? hakios_metal_texture_height(_pass.color0) : 0];\n'
     '\t\tif (_vsHighest > 1 && vs[1] != NULL && _vsOffsets[1] + 64 <= hakios_metal_buffer_length(vs[1])) {\n'
     '\t\t\tconst float *v = (const float *)((const uint8_t *)hakios_metal_buffer_contents(vs[1]) + _vsOffsets[1]);\n'
     '\t\t\t[d appendString:@" vb1"];\n'
     '\t\t\tfor (unsigned k = 0; k < 16; k++) [d appendFormat:@" %g", v[k]];\n'
     '\t\t}\n'
     '\t\tfor (unsigned k = 0; k < _fsHighestTexture; k++) {\n'
     '\t\t\tif (_fsTextures[k] != nil) [d appendFormat:@"\\ndd:     fs t%u = %@", k, [_fsTextures[k] hmddDescribe]];\n'
     '\t\t}\n'
     '\t\tfor (unsigned k = 0; k < _vsHighestTexture; k++) {\n'
     '\t\t\tif (_vsTextures[k] != nil) [d appendFormat:@"\\ndd:     vs t%u = %@", k, [_vsTextures[k] hmddDescribe]];\n'
     '\t\t}\n'
     '\t\thm_dd_line(_commandBuffer, d, NO, YES);\n'
     '\t}\n'),
    # a dispatch: the kernel, its textures (all noted as written: the kernel decides), probed again at completion
    ('\thakios_metal_cb_set_threadgroup_length(cb, (unsigned)_threadgroupLength);\n',
     '\tif (hm_dd_on()) {   /* (ws-hm-alib) HM_DD */\n'
     '\t\tconst char *kn = hakios_metal_kernel_name([_pipeline hakiosKernel]);\n'
     '\t\tNSMutableString *d = [NSMutableString stringWithFormat:@"dispatch %s: %ux%ux%u groups of %ux%ux%u", kn != NULL ? kn : "?",\n'
     '\t\t\tg[0], g[1], g[2], t[0], t[1], t[2]];\n'
     '\t\tfor (unsigned k = 0; k < _highestTexture; k++) {\n'
     '\t\t\tif (_textures[k] == nil) continue;\n'
     '\t\t\t[d appendFormat:@"\\ndd:     t%u = %@", k, [_textures[k] hmddDescribe]];\n'
     '\t\t\thm_dd_keep(_commandBuffer, @"probe", _textures[k]);\n'
     '\t\t\tchar kind[40];\n'
     '\t\t\tsnprintf(kind, sizeof(kind), "dispatch %s t", kn != NULL ? kn : "?");\n'
     '\t\t\thm_dd_note([_textures[k] hmddCore], kind, k, 0);\n'
     '\t\t}\n'
     '\t\thm_dd_line(_commandBuffer, d, NO, NO);\n'
     '\t}\n'
     '\thakios_metal_cb_set_threadgroup_length(cb, (unsigned)_threadgroupLength);\n'),
    # -replaceRegion: (level form) and blit destinations noted as writes
    ('\tif (hakios_metal_texture_replace_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,\n',
     '\thm_dd_note(_texture, "replaceRegion", (unsigned)region.size.width, (unsigned)region.size.height);   /* (ws-hm-alib) HM_DD */\n'
     '\tif (hakios_metal_texture_replace_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,\n'),
    ('hm_blit_texture(hakios_metal_cb *cb, id object, const char *what)\n{\n',
     'hm_blit_texture(hakios_metal_cb *cb, id object, const char *what)\n{\n'
     '\tif (hm_dd_on() && strcmp(what, "source") != 0 && [object isKindOfClass:[HakiosMetalTexture class]]) {   /* (ws-hm-alib) HM_DD */\n'
     '\t\thm_dd_note([(HakiosMetalTexture *)object hmddCore], what[0] == \'d\' ? "blit destination" : "blit (mipmaps)", 0, 0);\n'
     '\t}\n'),
]

EDITS += HM_DD_EDITS   # (ws-hm-alib)


# (ws-hm-alib) HM_BLIT_WB=1 (default; 0 the old behaviour for A/B): a SHADOW copy (an IOSurface whose rows the GPU cannot
# use directly, drawn through a private copy) that a BLIT writes is written back into the surface when its command buffer
# completes, as passes and kernels were -- before, a blit into one left the surface as it was (zeros), which other
# processes (backboardd showing SpringBoard's pre-blurred App Library backdrop) then drew. Under the same switch:
#   - copyFromTexture: (every form: region, slices/levels, whole), copyFromBuffer:toTexture: (both forms; also the
#     layered/3D and block-format paths) refresh the target's copy from the surface first and list it for write-back,
#     once a buffer however many blits land on it. Only bytes the GPU changed from the copy's base go back (the existing
#     diff of hakiosWriteBackShadow, over level 0); a blit into level > 0 or into a YCbCr copy is counted and not written.
#   - a blit SOURCE that is a SHADOW copy (copyFromTexture: source, copyFromTexture:toBuffer:, generateMipmaps' level 0)
#     is refreshed from the surface when the seed moved, as a sampled texture is (and listed as a reader for HM_SHADOW_WAIT).
#   - a texture VIEW of a SHADOW copy counts as SHADOW (hakiosIsShadow): passes and kernels writing through a view write
#     back too (they did not: only the view's parent had _shadow set).
#   - replaceRegion: on a SHADOW copy (2D, level 0) also writes the caller's rows into the surface (and the copy's base).
# HM_WB_SYNC=1 (default; 0 the old handlers for passes and kernels): every write-back runs on the completing thread in
# hakiosCompletedNow BEFORE the buffer's shared events are signalled and before its completed handlers are delivered --
# before, they were completed handlers themselves, dispatched to the daemon's completion queue, so -waitUntilCompleted
# could return (and an event be signalled) before the surface had the new rows. Blits always use this path.
# Proof in a capture: HM_PROF>=2 "prof: shadow write-backs: ..." every 5 s; HM_DD=1 "dd: blit wb: surface ID WxH fmt ..."
# per blit write-back (at most HM_DD_MAX_WB=200) and "dd: replaceRegion wb: ...".
def _tabs(s):
    out = []
    for line in s.split('\n'):
        n = len(line) - len(line.lstrip(' '))
        out.append('\t' * (n // 4) + ' ' * (n % 4) + line[n:])
    return '\n'.join(out)


HM_BLIT_WB_HELPERS = _tabs(r'''/* (ws-hm-alib) HM_BLIT_WB / HM_WB_SYNC: see bench/plugin.py */
static volatile long hm_wb_blit, hm_wb_blit_deferred, hm_wb_blit_skipped, hm_wb_blit_reads, hm_wb_put, hm_wb_sync_runs;
static int
hm_blit_wb(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("HM_BLIT_WB");

        on = e == NULL || e[0] != '0';
    }
    return on;
}
static int
hm_wb_sync(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("HM_WB_SYNC");

        on = e == NULL || e[0] != '0';
    }
    return on;
}

''')

HM_BLIT_WB_PROF = _tabs(r'''        {   /* (ws-hm-alib) HM_BLIT_WB */
            static long wbB, wbD, wbS, wbR, wbP, wbY;
            const long b = hm_wb_blit, d = hm_wb_blit_deferred, sk = hm_wb_blit_skipped, r = hm_wb_blit_reads, p = hm_wb_put, y = hm_wb_sync_runs;

            hm_print("prof: shadow write-backs: blit targets written back %ld, deferred to a later writer %ld, not written (level/YCbCr) %ld; "
                     "blit sources refreshed %ld; replaceRegion %ld; write-backs run at completion %ld (HM_BLIT_WB=%d HM_WB_SYNC=%d)\n",
                     b - wbB, d - wbD, sk - wbS, r - wbR, p - wbP, y - wbY, hm_blit_wb(), hm_wb_sync());
            wbB = b, wbD = d, wbS = sk, wbR = r, wbP = p, wbY = y;
        }
''')

HM_BLIT_WB_IFACE = _tabs(r'''
/* (ws-hm-alib) HM_BLIT_WB: blits, views and replaceRegion over a SHADOW copy (see bench/plugin.py) */
@interface HakiosMetalTexture (HMBlitWB)
- (HakiosMetalTexture *)hmwbRoot;
- (void)hmwbBlitRead:(id)cb;
- (BOOL)hmwbBlitWrite:(id)cb level:(NSUInteger)level;
- (void)hmwbPutRegion:(MTLRegion)region level:(NSUInteger)level bytes:(const void *)bytes bytesPerRow:(NSUInteger)bytesPerRow;
- (void)hmwbNoteBlitWriteBack:(id)cb deferred:(BOOL)deferred seed:(uint32_t)seed ms:(double)ms;
@end
''')

HM_BLIT_WB_IMPL = _tabs(r'''
/* (ws-hm-alib) HM_BLIT_WB: 9 elements of a surface's memory on a 3x3 grid, as hm_dd_probe does for a texture */
static NSString *
hm_wb_surface_probe(IOSurfaceRef s)
{
    if (s == NULL || IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) return @"(not probed)";
    const size_t bpe = IOSurfaceGetBytesPerElement(s), w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
    const uint8_t *base = IOSurfaceGetBaseAddress(s);
    unsigned nz = 0;
    char hex[40] = "";

    if (base != NULL && bpe != 0 && bpe <= 16 && w != 0 && h != 0) {
        for (unsigned j = 0; j < 3; j++) {
            for (unsigned i = 0; i < 3; i++) {
                const uint8_t *px = base + (h * (1 + 2 * j) / 6) * bpr + (w * (1 + 2 * i) / 6) * bpe;
                BOOL any = NO;

                for (size_t k = 0; k < bpe; k++) any = any || px[k] != 0;
                nz += any;
                if (i == 1 && j == 1) {
                    for (size_t k = 0; k < bpe; k++) snprintf(hex + 2 * k, 3, "%02x", px[k]);
                }
            }
        }
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    return [NSString stringWithFormat:@"%u/9 not zero, centre %s", nz, hex];
}

@implementation HakiosMetalTexture (HMBlitWB)

/* the SHADOW texture whose copy this is: itself, or the one a view (of a view) is of; nil for any other */
- (HakiosMetalTexture *)hmwbRoot
{
    if (_parent != nil) return [_parent hmwbRoot];
    return _shadow && _iosurface != NULL ? self : nil;
}

/* a blit reads this texture: the copy as the surface is now (the seed moved), as for a sampled texture */
- (void)hmwbBlitRead:(id)cb
{
    if (!hm_blit_wb() || [self hmwbRoot] == nil) return;
    [self hakiosRefreshShadow];
    [self hakiosShadowUse:cb writer:NO];
    __sync_fetch_and_add(&hm_wb_blit_reads, 1);
}

/* a blit writes this texture at LEVEL: the copy made current first (a blit into part of it keeps the rest), a base kept
 * (what the write-back tells the blit's bytes from), and the copy written back when CB completes -- listed once a buffer */
- (BOOL)hmwbBlitWrite:(id)cb level:(NSUInteger)level
{
    HakiosMetalTexture *root = hm_blit_wb() ? [self hmwbRoot] : nil;

    if (root == nil || cb == nil || ![cb isKindOfClass:[HakiosMetalCommandBuffer class]]) return NO;
    if (level + (_parent != nil ? _parentLevel : 0) != 0 || root->_ycbcr != 0 || root->_ycbcrNative) {
        __sync_fetch_and_add(&hm_wb_blit_skipped, 1);
        HM_ONCE("blit wb: a blit into level %lu of a surface's copy (or a YCbCr copy) is not written back: a surface has level 0 only\n",
                (unsigned long)level);
        return NO;
    }
    root->_blitWritten = YES;
    [root hakiosRefreshShadow];
    if (root->_shadowBase == NULL) {
        const size_t bytes = IOSurfaceGetBytesPerRow(root->_iosurface) * IOSurfaceGetHeight(root->_iosurface);

        if (IOSurfaceLock(root->_iosurface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {
            uint8_t *b = malloc(bytes);

            if (b != NULL) memcpy(b, IOSurfaceGetBaseAddress(root->_iosurface), bytes);
            IOSurfaceUnlock(root->_iosurface, kIOSurfaceLockReadOnly, NULL);
            root->_shadowBase = b;
        }
    }
    if ([(HakiosMetalCommandBuffer *)cb hakiosAddWriteBack:root blit:YES]) {
        [root hakiosShadowUse:cb writer:YES];   /* HM_SHADOW_WAIT: a writer until its write-back has run */
    }
    return YES;
}

/* replaceRegion on a SHADOW copy: the caller's rows into the surface too (and the copy's base, so a later write-back
 * does not take them for GPU drawing); 2D level 0 of an uncompressed format of the copy's own pixel size */
- (void)hmwbPutRegion:(MTLRegion)region level:(NSUInteger)level bytes:(const void *)bytes bytesPerRow:(NSUInteger)bytesPerRow
{
    HakiosMetalTexture *root = hm_blit_wb() ? [self hmwbRoot] : nil;

    if (root == nil || bytes == NULL || level + (_parent != nil ? _parentLevel : 0) != 0 || root->_ycbcr != 0 || root->_ycbcrNative) return;
    IOSurfaceRef s = root->_iosurface;
    const unsigned bpp = hakios_metal_texture_bytes_per_pixel(root->_texture), fmt = hakios_metal_texture_format(root->_texture);
    const size_t bpr = IOSurfaceGetBytesPerRow(s);

    if (bpp == 0 || (fmt >= 130 && fmt <= 236) || hakios_metal_texture_bytes_per_pixel(_texture) != bpp ||
        region.origin.x + region.size.width > IOSurfaceGetWidth(s) || region.origin.y + region.size.height > IOSurfaceGetHeight(s) ||
        (region.origin.x + region.size.width) * bpp > bpr || bytesPerRow < region.size.width * bpp) {
        HM_ONCE("replaceRegion wb: a region of a surface's copy is not written into the surface (format %u, %u bytes a pixel)\n", fmt, bpp);
        return;
    }
    if (IOSurfaceLock(s, 0, NULL) != kIOReturnSuccess) return;
    uint8_t *surface = IOSurfaceGetBaseAddress(s);

    for (NSUInteger y = 0; y < region.size.height; y++) {
        const size_t at = (region.origin.y + y) * bpr + region.origin.x * bpp;
        const uint8_t *row = (const uint8_t *)bytes + y * bytesPerRow;

        memcpy(surface + at, row, region.size.width * bpp);
        if (root->_shadowBase != NULL) memcpy(root->_shadowBase + at, row, region.size.width * bpp);
    }
    IOSurfaceUnlock(s, 0, NULL);
    root->_shadowSeed = IOSurfaceGetSeed(s);
    __sync_fetch_and_add(&hm_wb_put, 1);
    if (hm_dd_on()) {
        hm_print("dd: replaceRegion wb: surface %u %lux%lu at (%lu, %lu) of %lux%lu fmt %s, seed now %u\n", IOSurfaceGetID(s),
                 (unsigned long)region.size.width, (unsigned long)region.size.height, (unsigned long)region.origin.x, (unsigned long)region.origin.y,
                 (unsigned long)IOSurfaceGetWidth(s), (unsigned long)IOSurfaceGetHeight(s), [hm_dd_fourcc(IOSurfaceGetPixelFormat(s)) UTF8String],
                 IOSurfaceGetSeed(s));
    }
}

/* a blit target's write-back has run (or was deferred to a later writer's, which carries it): counted, and with HM_DD
 * one line, the surface's own memory probed after it */
- (void)hmwbNoteBlitWriteBack:(id)cb deferred:(BOOL)deferred seed:(uint32_t)seed ms:(double)ms
{
    static long lines;

    __sync_fetch_and_add(deferred ? &hm_wb_blit_deferred : &hm_wb_blit, 1);
    if (!hm_dd_on() || _iosurface == NULL || lines >= hm_dd_env("HM_DD_MAX_WB", 200)) return;
    lines++;
    hm_print("dd: blit wb: surface %u %lux%lu fmt %s (texture fmt %lu, rows %lu) seed %u -> %u, %.2f ms, buffer %p%s; surface probe %s\n",
             IOSurfaceGetID(_iosurface), (unsigned long)IOSurfaceGetWidth(_iosurface), (unsigned long)IOSurfaceGetHeight(_iosurface),
             [hm_dd_fourcc(IOSurfaceGetPixelFormat(_iosurface)) UTF8String], (unsigned long)[self pixelFormat],
             (unsigned long)IOSurfaceGetBytesPerRow(_iosurface), seed, IOSurfaceGetSeed(_iosurface), ms, (void *)cb,
             deferred ? " DEFERRED to a later writer's write-back" : "", [hm_wb_surface_probe(_iosurface) UTF8String]);
}
@end
''')

HM_BLIT_WB_CB = _tabs(r'''
/* (ws-hm-alib) HM_BLIT_WB / HM_WB_SYNC: a SHADOW copy's write-back into its surface, run on the completing thread before
 * this buffer's shared events are signalled and before its completed handlers are delivered (on the daemon's completion
 * queue or here): -waitUntilCompleted, every handler and a waiter on an event see the surface's new rows. A blit target
 * is listed once a buffer (NO: already listed); a pass's or a kernel's once a binding, as their handlers were. */
- (BOOL)hakiosAddWriteBack:(id)texture blit:(BOOL)blit
{
    NSArray *entry = [NSArray arrayWithObjects:texture, [NSNumber numberWithBool:blit], nil];

    @synchronized (self) {
        if (hakios_metal_cb_status(_cb) < HAKIOS_METAL_COMPLETED) {
            if (_writeBacks == nil) _writeBacks = [[NSMutableArray alloc] init];
            if (blit) {
                for (NSArray *e in _writeBacks) {
                    if ([e objectAtIndex:0] == texture && [[e objectAtIndex:1] boolValue]) return NO;
                }
            }
            [_writeBacks addObject:entry];
            return YES;
        }
    }
    [self hakiosRunWriteBack:entry];   /* (completed already: at once, as a late completed handler is) */
    return YES;
}

- (void)hakiosRunWriteBack:(NSArray *)entry
{
    HakiosMetalTexture *t = [entry objectAtIndex:0];
    const BOOL blit = [[entry objectAtIndex:1] boolValue];
    const long d0 = hm_shadow_wb_deferred;
    const uint32_t seed = [t iosurface] != NULL ? IOSurfaceGetSeed([t iosurface]) : 0;
    const double began = hm_now();

    [t hakiosWriteBackShadowFrom:self];
    __sync_fetch_and_add(&hm_wb_sync_runs, 1);
    if (blit) [t hmwbNoteBlitWriteBack:self deferred:hm_shadow_wb_deferred != d0 seed:seed ms:(hm_now() - began) * 1e3];
}

- (void)hakiosRunWriteBacks
{
    NSArray *list;

    @synchronized (self) {
        list = _writeBacks;
        _writeBacks = nil;
    }
    for (NSArray *e in list) [self hakiosRunWriteBack:e];
    [list release];
}
''')

HM_BLIT_WB_EDITS = [
    ('static void\nhm_prof_note(hakios_metal_cb *cb, double gpu, BOOL present)\n{\n',
     HM_BLIT_WB_HELPERS + 'static void\nhm_prof_note(hakios_metal_cb *cb, double gpu, BOOL present)\n{\n'),
    ('\t\tqsort(hm_prof.cls, HM_PROF_CLASSES, sizeof(hm_prof.cls[0]), hm_prof_cmp);\n',
     HM_BLIT_WB_PROF + '\t\tqsort(hm_prof.cls, HM_PROF_CLASSES, sizeof(hm_prof.cls[0]), hm_prof_cmp);\n'),
    ('\tuint8_t *_shadowBase;',
     '\tBOOL _blitWritten;                  /* (ws-hm-alib) HM_BLIT_WB: a blit wrote this copy: a base kept, written back as a writer\'s */\n'
     '\tuint8_t *_shadowBase;'),
    ('- (unsigned)hakiosStorageKind;   /* metal_fps=timeline: 0 a surface\'s, else 1 + its storage mode */\n@end\n',
     '- (unsigned)hakiosStorageKind;   /* metal_fps=timeline: 0 a surface\'s, else 1 + its storage mode */\n@end\n' + HM_BLIT_WB_IFACE),
    ('\tNSMutableArray *_turned;',
     '\tNSMutableArray *_writeBacks;             /* (ws-hm-alib) { SHADOW texture, blit? }: written back at completion, before events and handlers */\n'
     '\tNSMutableArray *_turned;'),
    ('- (BOOL)hakiosCommitted;   /* (redfin hmv12) */\n',
     '- (BOOL)hakiosCommitted;   /* (redfin hmv12) */\n- (BOOL)hakiosAddWriteBack:(id)texture blit:(BOOL)blit;   /* (ws-hm-alib) HM_BLIT_WB */\n'),
    ('- (BOOL)hakiosIsShadow { return _shadow; }',
     '- (BOOL)hakiosIsShadow { return _shadow || (_parent != nil && hm_blit_wb() && [_parent hakiosIsShadow]); }   /* (ws-hm-alib) a view of a copy too */'),
    ('(_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) == 0) {',
     '((_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) == 0 && !_blitWritten)) {   /* (ws-hm-alib) */'),
    ('\t\tif ((_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0) {\n\t\t\tconst size_t bytes',
     '\t\tif ((_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0 || _blitWritten) {   /* (ws-hm-alib) */\n\t\t\tconst size_t bytes'),
    ('(_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0 ||\n\t\t    _shadowFrame',
     '(_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) != 0 || _blitWritten ||\n\t\t    _shadowFrame'),
    ('\t[s appendFormat:@"; writer: %s; probe %@", wr, hm_dd_probe(_texture)];\n\treturn s;\n}\n@end\n',
     '\t[s appendFormat:@"; writer: %s; probe %@", wr, hm_dd_probe(_texture)];\n\treturn s;\n}\n@end\n' + HM_BLIT_WB_IMPL),
    ('\tif (hakios_metal_texture_replace_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,\n'
     '\t\t\t\t\t       (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow) != 0) {\n',
     '\tconst BOOL hmwbOk = hakios_metal_texture_replace_level(_texture, (unsigned)level, (unsigned)region.origin.x, (unsigned)region.origin.y,\n'
     '\t\t\t\t\t       (unsigned)region.size.width, (unsigned)region.size.height, bytes, bytesPerRow) == 0;\n'
     '\tif (hmwbOk) [self hmwbPutRegion:region level:level bytes:bytes bytesPerRow:bytesPerRow];   /* (ws-hm-alib) HM_BLIT_WB */\n'
     '\tif (!hmwbOk) {\n'),
    ('\t[_turned release];\n\t[_waits release];\n',
     '\t[_turned release];\n\t[_waits release];\n\t[_writeBacks release];   /* (ws-hm-alib) */\n'),
    ('- (BOOL)hakiosCommitted { return _committed; }   /* (redfin hmv12) a held commit too, before its send */\n',
     '- (BOOL)hakiosCommitted { return _committed; }   /* (redfin hmv12) a held commit too, before its send */\n' + HM_BLIT_WB_CB),
    ('\t/* the events this buffer signals, now that its work is done: before its handlers, which may read them */\n',
     '\t[self hakiosRunWriteBacks];   /* (ws-hm-alib) HM_BLIT_WB / HM_WB_SYNC: surfaces written before events and handlers */\n'
     '\t/* the events this buffer signals, now that its work is done: before its handlers, which may read them */\n'),
    ('\t\t{\n\t\t\tHakiosMetalTexture *drawn = [(HakiosMetalTexture *)t retain];\n',
     '\t\tif (hm_wb_sync()) {   /* (ws-hm-alib) HM_WB_SYNC */\n'
     '\t\t\t[(HakiosMetalCommandBuffer *)cb hakiosAddWriteBack:t blit:NO];\n'
     '\t\t} else {\n\t\t\tHakiosMetalTexture *drawn = [(HakiosMetalTexture *)t retain];\n'),
    ('\t\tHakiosMetalTexture *written = [(HakiosMetalTexture *)texture retain];\n'
     '\t\t[_commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {\n'
     '\t\t\t(void)cb;\n\t\t\t[written hakiosWriteBackShadowFrom:cb];   /* (redfin hmv12) */\n\t\t\t[written release];\n\t\t}];\n',
     '\t\tif (hm_wb_sync()) {   /* (ws-hm-alib) HM_WB_SYNC */\n'
     '\t\t\t[(HakiosMetalCommandBuffer *)_commandBuffer hakiosAddWriteBack:texture blit:NO];\n'
     '\t\t} else {\n'
     '\t\tHakiosMetalTexture *written = [(HakiosMetalTexture *)texture retain];\n'
     '\t\t[_commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {\n'
     '\t\t\t(void)cb;\n\t\t\t[written hakiosWriteBackShadowFrom:cb];   /* (redfin hmv12) */\n\t\t\t[written release];\n\t\t}];\n'
     '\t\t}\n'),
    ('\tif (hakios_metal_cb_blit(cb, from, (unsigned)sourceLevel, (unsigned)sourceOrigin.x, (unsigned)sourceOrigin.y, (unsigned)sourceSize.width,\n',
     '\t[(HakiosMetalTexture *)sourceTexture hmwbBlitRead:_commandBuffer];   /* (ws-hm-alib) HM_BLIT_WB */\n'
     '\t[(HakiosMetalTexture *)destinationTexture hmwbBlitWrite:_commandBuffer level:destinationLevel];\n'
     '\tif (hakios_metal_cb_blit(cb, from, (unsigned)sourceLevel, (unsigned)sourceOrigin.x, (unsigned)sourceOrigin.y, (unsigned)sourceSize.width,\n'),
    ('\tif (to == NULL) {\n\t\treturn;\n\t}\n\t/* a layer of an array or cube',
     '\tif (to == NULL) {\n\t\treturn;\n\t}\n'
     '\t[(HakiosMetalTexture *)destinationTexture hmwbBlitWrite:_commandBuffer level:destinationLevel];   /* (ws-hm-alib) HM_BLIT_WB */\n'
     '\t/* a layer of an array or cube'),
    ('\tif (from == NULL) {\n\t\treturn;\n\t}\n\tif (to == NULL) {\n\t\thakios_metal_cb_refuse(cb, "a texture-to-buffer copy',
     '\tif (from == NULL) {\n\t\treturn;\n\t}\n'
     '\t[(HakiosMetalTexture *)sourceTexture hmwbBlitRead:_commandBuffer];   /* (ws-hm-alib) HM_BLIT_WB */\n'
     '\tif (to == NULL) {\n\t\thakios_metal_cb_refuse(cb, "a texture-to-buffer copy'),
    ('\tif (t != NULL && hakios_metal_cb_generate_mipmaps(cb, t) != 0) {\n',
     '\tif (t != NULL) {   /* (ws-hm-alib) HM_BLIT_WB: level 0 is read; the levels written are none of a surface\'s */\n'
     '\t\t[(HakiosMetalTexture *)texture hmwbBlitRead:_commandBuffer];\n\t}\n'
     '\tif (t != NULL && hakios_metal_cb_generate_mipmaps(cb, t) != 0) {\n'),
]

EDITS += HM_BLIT_WB_EDITS   # (ws-hm-alib) HM_BLIT_WB


# (ws-hm-alib) HM_CA_B3A8=1 (default 0: the known-good behaviour, nothing changes): the App Library's black static
# backdrop (alib2-1115). When the App Library settles, its backdrop is a deep render-server snapshot: the snapshot
# surface is 'b3a8' (CARenderServerCreateSnapshots with the deep option: an XR10 colour plane and an A8 plane, Metal's
# RGB10A8_2P_XR10 550). backboardd's CoreAnimation takes a 'b3a8' surface only if its Metal context's limits have the
# bit MTLGPUFamilyApple3 sets (QuartzCore new_metal_context: supportsFamily:1003 -> limits 0x100;
# four_cc_to_mtl_format('b3a8') -> 550 only with it). This device's feature profile 1 answers Apple1 and Apple2 alone,
# so CA logged "Unexpected framebuffer format b3a8" (MetalContext::get_surface, the snapshot not drawn) and then
# "unsupported surface format: b3a8" ~11 times a second (MetalContext::update_surface, the layer left without
# contents): the solid black fill (PBGRAXm_A2Xhf) where the blurred wallpaper belongs. Under HM_CA_B3A8=1:
#   - -supportsFamily:1003 answers YES when QuartzCore asks (the caller's return address in QuartzCore's image; its
#     only two questions for 1003 are in new_metal_context: the 'b3a8' bit, and a set_surface divisor of 128 that is
#     then left 0). Every other caller, and every other family, gets the base class's answer.
#   - a texture of 550/551 over a 'b3a8' surface's plane 0 is BGR10_XR (554/555) over it: the codes decoded for a draw
#     and written back after one, as over a 'w30r' surface (core: Texture::wrapSurface's 'b3a8' exception now takes
#     the 4-byte XR formats). The alpha plane is not read: alpha 1, which a snapshot of the opaque screen is.
#   - plane 1 of a 'b3a8' surface (its A8 plane) is a texture of its own of the asked format (drawn into, never read
#     back into the surface), not a refusal that would fail the whole snapshot.
HM_B3A8_HELPERS = r"""/* (ws-hm-alib) HM_CA_B3A8: see bench/plugin.py */
static int
hm_ca_b3a8_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("HM_CA_B3A8");
		on = e != NULL && atoi(e) != 0;
	}
	return on;
}

static BOOL
hm_b3a8_surface(IOSurfaceRef s)
{
	if (s == NULL) return NO;
	const OSType f = IOSurfaceGetPixelFormat(s);
	return f == 0x62336138u || f == 0x26623338u;   /* 'b3a8', '&b38' */
}

/* RA (a method's return address) is in QuartzCore's image */
static BOOL
hm_caller_is_quartzcore(const void *ra)
{
	static const void *base;
	Dl_info info;

	if (ra == NULL || dladdr(ra, &info) == 0 || info.dli_fbase == NULL) return NO;
	if (base != NULL) return info.dli_fbase == base;
	if (info.dli_fname != NULL && strstr(info.dli_fname, "/QuartzCore.framework/QuartzCore") != NULL) {
		base = info.dli_fbase;
		return YES;
	}
	return NO;
}

"""

HM_B3A8_EDITS = [
    ('static MTLTextureDescriptor *\nhm_xr10_descriptor(MTLTextureDescriptor *d)\n{\n',
     HM_B3A8_HELPERS + 'static MTLTextureDescriptor *\nhm_xr10_descriptor(MTLTextureDescriptor *d)\n{\n'),
    ('/* (redfin hmv4) HM_XR10 (see hm_xr10_on) */\n- (BOOL)supportsExtendedXR10Formats\n',
     '/* (ws-hm-alib) HM_CA_B3A8: QuartzCore\'s Apple3 question (its \'b3a8\' surfaces), see bench/plugin.py */\n'
     '- (BOOL)supportsFamily:(MTLGPUFamily)family\n{\n'
     '\tif (family == (MTLGPUFamily)1003 && hm_ca_b3a8_on() && hm_caller_is_quartzcore(__builtin_return_address(0))) {\n'
     '\t\tHM_ONCE("HM_CA_B3A8=1: QuartzCore asked for MTLGPUFamilyApple3: YES (its Metal context then takes \'b3a8\' surfaces; '
     'RGB10A8_2P_XR10 is served as BGR10_XR over the colour plane)\\n");\n'
     '\t\treturn YES;\n\t}\n'
     '\tstruct objc_super sup = { self, class_getSuperclass([HakiosMetalDevice class]) };\n'
     '\treturn ((BOOL (*)(struct objc_super *, SEL, MTLGPUFamily))objc_msgSendSuper)(&sup, _cmd, family);\n}\n\n'
     '/* (redfin hmv4) HM_XR10 (see hm_xr10_on) */\n- (BOOL)supportsExtendedXR10Formats\n'),
    ('\tdescriptor = hm_xr10_descriptor(descriptor);   /* (redfin hmv4) HM_XR10=0: 550/551 (a b3a8 surface) as BGRA8 over its first plane */\n',
     '\tif (hm_ca_b3a8_on() && hm_b3a8_surface(surface) && descriptor != nil) {   /* (ws-hm-alib) HM_CA_B3A8 */\n'
     '\t\tconst MTLPixelFormat f = [descriptor pixelFormat];\n'
     '\t\tif (plane == 0 && (f == (MTLPixelFormat)550 || f == (MTLPixelFormat)551)) {\n'
     '\t\t\tHM_ONCE("HM_CA_B3A8=1: format %lu over the \'b3a8\' surface %u (%zux%zu) served as BGR10_XR%s over its colour plane, alpha 1\\n",\n'
     '\t\t\t\t(unsigned long)f, IOSurfaceGetID(surface), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), f == 551 ? "_sRGB" : "");\n'
     '\t\t\tdescriptor = [[descriptor copy] autorelease];\n'
     '\t\t\t[descriptor setPixelFormat:f == (MTLPixelFormat)550 ? (MTLPixelFormat)554 : (MTLPixelFormat)555];\n'
     '\t\t} else if (plane == 1) {\n'
     '\t\t\tHM_ONCE("HM_CA_B3A8=1: plane 1 (alpha) of the \'b3a8\' surface %u as a texture of its own, format %lu (not read back)\\n",\n'
     '\t\t\t\tIOSurfaceGetID(surface), (unsigned long)f);\n'
     '\t\t\treturn [self newTextureWithDescriptor:descriptor];\n'
     '\t\t}\n'
     '\t}\n'
     '\tdescriptor = hm_xr10_descriptor(descriptor);   /* (redfin hmv4) HM_XR10=0: 550/551 (a b3a8 surface) as BGRA8 over its first plane */\n'),
]

EDITS += HM_B3A8_EDITS   # (ws-hm-alib) HM_CA_B3A8


def apply(src, edits=EDITS):
    for e in edits:
        if len(e) == 2:
            old, new = e
            n = src.count(old)
            if n != 1:
                sys.exit('plugin.py: %r found %d times' % (old[:120], n))
            src = src.replace(old, new)
        else:
            first, last, new = e
            if src.count(first) != 1:
                sys.exit('plugin.py: span start %r found %d times' % (first[:120], src.count(first)))
            a = src.index(first)
            b = src.find(last, a)
            if b < 0:
                sys.exit('plugin.py: span end %r not found after its start' % last[:120])
            src = src[:a] + new + src[b + len(last):]
    return src


def main():
    src = apply(open(sys.argv[1]).read())
    open(sys.argv[2], 'w').write(src)
    print('plugin.py: %d edits' % len(EDITS))


if __name__ == '__main__':
    main()
