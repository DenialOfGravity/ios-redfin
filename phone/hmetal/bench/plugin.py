#!/usr/bin/env python3
"""plugin.py IN.m OUT.m: hakios' render-capable Metal plug-in (userland/src/HakiosMetalCA.m) made usable by hmbench.

The file is not in this repository: bench/build.sh fetches it at build time from oosexgang/hakios-xnu-kernel at the
pinned commit (sha256 checked), because its origin (vcvkk/darwin-redwood, branch hakios-workspace, eb9fdf5) carries a
licence hakios lists as "to verify" (its NOTICE, THIRD_PARTY_LICENSES.md). hakios' generic plug-in
(userland/src/HakiosMetal.m, hakios-metal/plugin/HakiosMetal.m; MIT) cannot be used for the benchmark: its render
pipeline state is a stand-in "that holds nothing", it has no render command encoder, and its GPUStartTime is a stub.

Edits, each by exact text, refused if the line is not found once:
  - the compiled-stage cache in /var/local/hmetal/cache (was /private/var/db/...: nothing of ours outside /var/local)
  - the read-only seed cache at /var/local/hmetal/cache-seed (was /redwood/share/..., which does not exist here)
  - the device for any process whose environment has HAKIOS_METAL_ALL (was: backboardd alone, or boot-arg metal_all=1)
  - (H3) every hm_print line also appended to the file $HAKIOS_METAL_LOG names (bench/m2shim.m sets it in backboardd:
    a launchd job's stderr here is not a log anybody reads)
  - (H3 fix 1) CoreAnimation's uber shaders (fixed_*) compiled from the function itself, with the constants QuartzCore
    specialised it by, when QuartzCore has a default.metallib (iOS 16.7 here; hakios' iPhone X 16.5 image had none, so
    the plug-in picks a pre-specialised variant out of default.pipelinelib by label). On 16.7 the label pick gave
    stand-ins for run-time pipelines the library does not name ("... is not in the library: ... stands in for it") and
    a refused draw ("fixed_vert_lph_spc reads 64 bytes at 0 of constant buffer 3; 16 are bound").
    HAKIOS_METAL_CA_VARIANTS=1 in the process's environment restores the label pick.
"""
import sys

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
    ('\tif (name == nil || ![name hasPrefix:@"fixed_"]) {\n\t\treturn own;\n\t}\n\thm_ca_load();\n',
     '\tif (name == nil || ![name hasPrefix:@"fixed_"]) {\n\t\treturn own;\n\t}\n'
     '\t{   /* (redfin H3 fix 1) QuartzCore with a default.metallib: the function as it is, its constants applied by the caller */\n'
     '\t\tstatic int use_own = -1;\n'
     '\t\tif (use_own < 0) {\n'
     '\t\t\tconst char *e = getenv("HAKIOS_METAL_CA_VARIANTS");\n'
     '\t\t\tuse_own = !(e != NULL && e[0] == \'1\') && access("/System/Library/Frameworks/QuartzCore.framework/default.metallib", R_OK) == 0;\n'
     '\t\t\thm_print("CoreAnimation\'s uber shaders: %s\\n", use_own ? "compiled from the functions QuartzCore specialised (its default.metallib; HAKIOS_METAL_CA_VARIANTS=1: the label pick)" : "the variant its label names (default.pipelinelib)");\n'
     '\t\t}\n'
     '\t\tif (use_own && own != nil) {\n'
     '\t\t\treturn own;\n'
     '\t\t}\n'
     '\t}\n'
     '\thm_ca_load();\n'),
]


def main():
    src = open(sys.argv[1]).read()
    for old, new in EDITS:
        n = src.count(old)
        if n != 1:
            sys.exit('plugin.py: %r found %d times' % (old, n))
        src = src.replace(old, new)
    open(sys.argv[2], 'w').write(src)
    print('plugin.py: %d edits' % len(EDITS))


if __name__ == '__main__':
    main()
