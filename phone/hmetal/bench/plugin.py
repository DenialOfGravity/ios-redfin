#!/usr/bin/env python3
"""plugin.py IN.m OUT.m: hakios' render-capable Metal plug-in (userland/src/HakiosMetalCA.m) made usable by hmbench.

The file is not in this repository: bench/build.sh fetches it at build time from oosexgang/hakios-xnu-kernel at the
pinned commit (sha256 checked), because its origin (vcvkk/darwin-redwood, branch hakios-workspace, eb9fdf5) carries a
licence hakios lists as "to verify" (its NOTICE, THIRD_PARTY_LICENSES.md). hakios' generic plug-in
(userland/src/HakiosMetal.m, hakios-metal/plugin/HakiosMetal.m; MIT) cannot be used for the benchmark: its render
pipeline state is a stand-in "that holds nothing", it has no render command encoder, and its GPUStartTime is a stub.

Edits, each by exact text, refused if the text is not found the number of times given (1 unless said):
  - the compiled-stage cache in /var/local/hmetal/cache (was /private/var/db/...: nothing of ours outside /var/local)
  - the read-only seed cache at /var/local/hmetal/cache-seed (was /redwood/share/..., which does not exist here)
  - the device for any process whose environment has HAKIOS_METAL_ALL (was: backboardd alone, or boot-arg metal_all=1)
  - (ws-hm-perf) the plug-in's boot-arg switches (metal_fps, metal_fps=sample, metal_defer=1, ...) read from the
    environment's HAKIOS_METAL_ARGS when it is set, instead of kern.bootargs (25 places): diagnostics per process,
    no reboot
  - (ws-hm-perf) GPUStartTime/GPUEndTime from the GPU's own counter stamps (hakios_metal_cb_gpu_times), the old
    commit-to-fence span when there are none
"""
import sys

BOOTARGS = 'sysctlbyname("kern.bootargs", args, &size, NULL, 0)'
EDITS = [
    ('static int hm_print_fd = -1;',
     'static int hm_print_fd = -1;\n'
     '/* (redfin ws-hm-perf) HAKIOS_METAL_ARGS, when set, stands for kern.bootargs in every switch below */\n'
     'static int hm_bootargs(char *args, size_t *size) {\n'
     '\tconst char *e = getenv("HAKIOS_METAL_ARGS");\n'
     '\tif (e == NULL) return sysctlbyname("kern.bootargs", args, size, NULL, 0);\n'
     '\tstrlcpy(args, e, *size);\n'
     '\t*size = strlen(args);\n'
     '\treturn 0;\n'
     '}', 1),
    (BOOTARGS, 'hm_bootargs(args, &size)', 25),
    ('- (CFTimeInterval)GPUStartTime    { return _committedAt; }',
     '- (CFTimeInterval)GPUStartTime    { int64_t s, e; return hakios_metal_cb_gpu_times(_cb, &s, &e) == 0 ? _committedAt + (double)s / 1e9 : _committedAt; }   /* (redfin) */', 1),
    ('- (CFTimeInterval)GPUEndTime      { return _committedAt + (double)hakios_metal_cb_gpu_nanoseconds(_cb) / 1e9; }',
     '- (CFTimeInterval)GPUEndTime      { int64_t s, e; return hakios_metal_cb_gpu_times(_cb, &s, &e) == 0 ? _committedAt + (double)e / 1e9 : '
     '_committedAt + (double)hakios_metal_cb_gpu_nanoseconds(_cb) / 1e9; }   /* (redfin) */', 1),
    ('#define HM_CACHE_DIR  "/private/var/db/hakios-metal-cache"',
     '#define HM_CACHE_DIR  "/var/local/hmetal/cache"     /* (redfin hmbench) */'),
    ('#define HM_CACHE_SEED "/redwood/share/hakios-metal-cache"',
     '#define HM_CACHE_SEED "/var/local/hmetal/cache-seed"     /* (redfin hmbench) */'),
    ('if (!all && (name == NULL || strcmp(name, "backboardd") != 0)) {',
     'if (!all && getenv("HAKIOS_METAL_ALL") == NULL && (name == NULL || strcmp(name, "backboardd") != 0)) {   /* (redfin hmbench) */'),
]
EDITS = [e if len(e) == 3 else (e[0], e[1], 1) for e in EDITS]


def main():
    src = open(sys.argv[1]).read()
    for old, new, want in EDITS:
        n = src.count(old)
        if n != want:
            sys.exit('plugin.py: %r found %d times, not %d' % (old, n, want))
        src = src.replace(old, new)
    open(sys.argv[2], 'w').write(src)
    print('plugin.py: %d edits' % len(EDITS))


if __name__ == '__main__':
    main()
