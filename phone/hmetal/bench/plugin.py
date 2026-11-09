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
		const int mode = hm_ca_force_nearest ? HM_CA_NEAREST : hm_ca_pick_mode();
		const char *how = NULL;

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
