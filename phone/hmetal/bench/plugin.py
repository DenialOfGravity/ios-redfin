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
  - (hmv9) the core's tally counts every program (HM_HANG_BLAME_WORDS default 0, was 1024): a small program in 2 hung buffers
    gets its 'blame:' line too; an uber shader among them is then drawn as HM_SPEC=2 draws it
  - (hmv8) HM_SHADOW_FRESH=1 (default): a read-only copy of an unaligned surface's rows made again once a command buffer
    whatever its seed (0: only when the seed moved); HM_LOG_TIME=1 (default): the time of day in each line's prefix;
    HM_TRACE's 10 s 'trace: surfaces:' line (live textures over IOSurfaces, row copies a s and forced ones, XR decodes,
    ring barriers) and 'trace: display-sized surface N let go' (the display torn down: ms since the last buffer);
    the core's HM_XR_SYNC (render.cpp) is set in the same env file
The variant tables are not in this repository or in the build: the plug-in reads them at run time from the phone's
own QuartzCore (default.pipelinelib/pipelines.desc + functions). bench/ca-variants.py reads the same files off the
phone (apple-ref/<version>/) to compare versions and to replay a log's pipeline labels through the same choice.
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
	struct hm_prof_class cls[HM_PROF_CLASSES];
} hm_prof = { PTHREAD_MUTEX_INITIALIZER };

static int
hm_prof_cmp(const void *a, const void *b)
{
	const double x = ((const struct hm_prof_class *)a)->us, y = ((const struct hm_prof_class *)b)->us;
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
		hm_print("HM_UBWC_RT=%s: render targets %s\n", e != NULL ? e : "unset", on ? "compressed (UBWC) where served" : "linear");
	}
	return on && [descriptor textureType] == MTLTextureType2D && [descriptor mipmapLevelCount] == 1 && [descriptor sampleCount] == 1 &&
	       ([descriptor pixelFormat] == MTLPixelFormatRGBA8Unorm || [descriptor pixelFormat] == MTLPixelFormatBGRA8Unorm ||
		[descriptor pixelFormat] == MTLPixelFormatRGBA8Unorm_sRGB || [descriptor pixelFormat] == MTLPixelFormatBGRA8Unorm_sRGB) &&
	       (usage & MTLTextureUsageRenderTarget) != 0 && (usage & MTLTextureUsageShaderWrite) == 0 &&
	       (usage & ~(MTLTextureUsage)(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)) == 0 &&
	       [descriptor storageMode] == MTLStorageModePrivate;
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
 * Why 0: hmt4 on the A620 (512 KiB GMEM) drew the 1125x2436 BGRA8 screen in 52 tiles. A GMEM pass of ~91 draws took
 * 15.2 ms (0.29 ms a tile; each pass loading ~11 MB and storing 11-22 MB, ~2.5-3 ms of bandwidth alone, and every
 * draw replayed in each tile it reaches), 62-66 % of the GPU's time; a full-screen sysmem pass of ~18 draws took 1.7 ms,
 * so ~91 draws in sysmem come to ~8-9 ms, and m2v (Turnip, sysmem) draws the same frames in 8.9 ms. */
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
		} else {
			m = atoi(e);
			if (m < 0) m = 0;
			if ((comma = strchr(e, ',')) != NULL && atoi(comma + 1) > 0) limit = (unsigned)atoi(comma + 1);
		}
		hm_print("HM_GMEM=%s: %s%s%u%s\n", e != NULL ? e : (bootarg ? "unset (boot-arg metal_gmem*: upstream's rule)" : "unset (default 0)"),
			 m == 0 ? "every pass in sysmem" : m == 1 ? "upstream's rule (metal_gmem_draws, 56 unless the boot-arg says)" : "GMEM for passes of at least N draws",
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
	char line[700] = "", line2[400] = "";

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
				static unsigned long long xwas[5];
				static long cwas, fwas;
				unsigned long long xr[5];
				const double secs = now - hm_tr.since > 0 ? now - hm_tr.since : 1;
				const long copies = hm_shadow_copies, forced = hm_shadow_forced;

				hakios_metal_xr_stats(xr);
				snprintf(line2, sizeof(line2), "trace: surfaces: %ld textures over IOSurfaces live (%ld most); row copies %.1f a s (%ld made "
					 "with the seed unmoved: HM_SHADOW_FRESH); XR decodes %.1f a s (%llu of surfaces); ring barriers %llu, %.1f ms avg, "
					 "%llu timed out (HM_XR_SYNC)", hm_surf_live, hm_surf_live_max, (copies - cwas) / secs, forced - fwas,
					 (xr[0] - xwas[0]) / secs, xr[1] - xwas[1], xr[2] - xwas[2],
					 xr[2] - xwas[2] ? (double)(xr[3] - xwas[3]) / 1e3 / (double)(xr[2] - xwas[2]) : 0.0, xr[4] - xwas[4]);
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
 *                       programs of HM_HANG_BLAME_WORDS fragment words or more, default 0 since hmv9) is from then on drawn as
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
     '\t\t{   /* (redfin H3) a copy into $HAKIOS_METAL_LOG */\n'
     '\t\t\tstatic int log_fd = -2;\n'
     '\t\t\tif (log_fd == -2) { const char *p = getenv("HAKIOS_METAL_LOG"); log_fd = p ? open(p, O_WRONLY | O_APPEND | O_CREAT, 0666) : -1; }\n'
     '\t\t\tif (log_fd >= 0) (void)!write(log_fd, line, strlen(line));\n'
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
     '\t\t\thakios_metal_cb_set_gmem_min_draws(_cb, mode == 1 ? hm_gmem_draws() : mode >= 2 ? (unsigned)mode : 0);\n'
     '\t\t\thakios_metal_cb_set_gmem_max_tiles(_cb, tiles);\n'
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
]


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
