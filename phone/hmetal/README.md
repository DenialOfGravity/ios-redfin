# hmetal: hakios-metal on redfin

hakios' Metal driver core (C++; writes Adreno PM4 itself, Mesa only for ir3), ported to our kernel's GPU.

| Path | What |
|---|---|
| `upstream/` | oosexgang/hakios-xnu-kernel `hakios-metal/` at 2631f5d (MIT, LICENSE + NOTICE). Our changes on top: the per-chip table, the gated gen4 writes, the test runner's `--chip`. No Apple file in it (checked file by file at the vendor commit). |
| `redfin/native_adreno_transport.{hpp,cpp}` | `hakios::metal::Transport` on our NativeAdreno ABI v1 (`phone/turnip/NativeAdrenoABI.h`): IOAcceleratorES, open type 0x4147, methods tunnelled `{'NTNL', method, payload}` through selector 0, client-chosen IOVAs, BoMap rw / writeback 0 / shareability 2, WC CPU maps through BoCpuMap tokens. `openNativeAdrenoGpu()` replaces upstream's `openKernelGpu()` (HakiosGPU) when built with `-DHAKIOS_METAL_NATIVE_ADRENO`. |
| `redfin/tests/` | a small IOKit/IOSurface/mach shim and an in-process fake of our kernel's v2 client (its argument rules), so the transport builds and runs on Linux. |
| `build.sh` | the iOS build: host tests first, then `out/libhakios-metal.a` and `out/hakios-metal-probe`. |

## The chips

`a6xx::ChipInfo` (upstream/include/hakios-metal/a6xx.hpp) has a row for the A635 (upstream's phone, unchanged) and the
A620 (Mesa FD620 = a6xx_base + a6xx_gen1). The row is picked when a `Gpu` is made, from the chip id the transport
reports (our kernel: 0x06020000, xnu/patches/0904). On the A620: no SP_CS_CNTL_0/1 or HLSQ_CS_CTRL_REG1 (gen4 only), no LRZ
feedback mask, no early preamble, a 64-group program preload cap, RB_CCU_CNTL 0x08000000 (sysmem) / 0x3e400000 (GMEM),
FD620's magic registers, PC/VFD_POWER_CNTL 0, 2048 fibers on 1 SP for private memory (4096 for the wave layout: probe F2), CP_WAIT_FOR_ME before
indirect draws. Open: the cubic sampler filter is not refused on the A620 yet (A620 has no `has_tex_filter_cubic`), and
RB_RENDER_CNTL is a plain register write (Mesa uses CP_REG_WRITE where `has_cp_reg_write`).

## Cache maintenance

Our kernel does none, and the A620 does not snoop. Driver-made buffers are write-combined for the CPU and need none.
Imported pages (IOSurfaces, request 0x7e; userptr, 0x7d) keep the caller's cacheable mapping, so the transport does
`dc civac` over each imported buffer a submission names, right before the submission, and again over each one named
as written once its fence is seen to pass (WAIT_FENCE, CPU_PREP). This is phone/metal2's rule without its seed-based
skipping, so it is correct but slower on large surfaces.

## Host tests (no SDK needed)

    make -C phone/hmetal/upstream test            # the core: 263 cases as the A635, then as the A620 (CHIP=a620 for one)
    make -C phone/hmetal/redfin/tests all         # transport_test (9 cases x 2 chips) + e2e: upstream's command, compute
                                                  # and render suites through the transport -> fake service -> FakeGpu
                                                  # (139 x 2; 6 surface cases skipped, see the Makefile)

MESA=1 (in-process compiler): localbuild `build hmesa REF` (phone/hmetal/mesa/build-mesa.sh: Mesa 25.0.7 + hakios' AIR
front end, host + iOS arm64 static libs, A620 fixtures via gen-fixtures.sh) then `build hmetal-mesa REF`.
tests/fixtures/a620/*_air.hpp = the host ir3_compiler's output for chip 0x06020000 (namespace kernels::a620); compiler_test
and probe stage 6 compare with them on the A620, with tests/fixtures/ (hakios', re-made bit-identical) on the A635.

Formerly not covered here: `MESA=1` (the in-process ir3 compiler; needs a Mesa host build with hakios' AIR front end) and
`tools/check-a6xx.py` (needs Mesa's generated a6xx.xml.h).

## H2: first run on the phone (not done yet; steps only)

1. Build in WSL: push the branch, then `build hmetal` is not a localbuild target yet. Either add a `build_hmetal()` to
   tools-claude/localbuild/build.sh calling `ios_env; cd "$R"; sh phone/hmetal/build.sh; cp phone/hmetal/out/* "$OUT/"`,
   or run it by hand in WSL after `ios_env` (so that work/userland/bin/ios-cc exists): `sh phone/hmetal/build.sh`.
2. Copy `out/hakios-metal-probe` to the phone with the agent's `files put` (e.g. /var/local/bin/hakios-metal-probe).
   No reboot and no new image: it is a userland program on the existing GPU service.
3. Run it while SpringBoard is up (it is one more v2 client of IOAcceleratorES, as Turnip is). First run:
   `hakios-metal-probe 2>&1 | tee /var/local/hmetal-probe.log`. The first line says `probe: GPU 0, chip id 0x6020000,
   625 MHz, FAULTS n`. Stages 1-4 are command-processor-only blits (copy, fills, 40 command buffers, copy speed):
   check these first. Stage 5 runs the precompiled ir3 kernels (fill_id, chains), stage 7 textures, stage 9 the first
   draw. Stage 6 (the on-device compiler) refuses by name without MESA=1.
4. The check: stages 1-3 PASS with `FAULTS` unchanged and no `SWEET MTL: v2 ... refused` in the klog. Then stage 5
   end_only/fill_id, then stage 9.
5. If a stage hangs: the kernel's hang recovery applies to every v2 client (Turnip included), so run it when a GPU reset
   is acceptable. `--bisect` (stage 11) switches dispatch parts off one at a time. Do not use `--recover` (it hangs the
   GPU on purpose).

## hmbench: the two Metal implementations side by side (bench/)

`hmbench --device m2v|hm` runs the same Metal-API workload (W1 empty command buffer, W2 clear pass, W3 40 textured
alpha-blended quads, W4 200 small draws; BGRA8 1080x2340 IOSurface target) on SweetMetal2.bundle (metal2vulkan +
Turnip, loaded read-only from /System) or on hakios-metal (`/var/local/hmetal/HakiosMetal.bundle`), making the device
as Metal.framework does (NSBundle + `-initWithAcceleratorPort:` on the IOAcceleratorES port). One device per process.
Logs `/var/local/hmetal/bench-<dev>.log`, lines `RESULT dev wl draws enc_us us/draw commit_us done_ms gpu_ms fps1 fps3 checksum`.

- Shaders: hakios-metal's own AIR (`upstream/tools/kernels/tri_attr_vs, tri_tex_fs, tri_color_fs`), `llvm-as` +
  `bench/mkmetallib.py` (our MTLB writer). No Apple metallib.
- The hakios plug-in is upstream's render-capable one, `userland/src/HakiosMetalCA.m`, fetched by `bench/build.sh` at
  2631f5d (sha256 pinned) and never committed: its origin (vcvkk/darwin-redwood) has a licence hakios lists as "to
  verify". `bench/plugin.py` edits it by exact text (cache under /var/local/hmetal, device for `HAKIOS_METAL_ALL` processes, log copy, the CA pick).
- CoreAnimation's uber shaders in the plug-in (window server test, hmvar): `HAKIOS_METAL_CA_PICK` = `exact` (default: the
  pinned plug-in's pre-specialised variant from QuartzCore's default.pipelinelib only where the function's or the pipeline's
  label names one exactly; a run-time pipeline is compiled from QuartzCore's default.metallib function with its own
  constants, both stages), `own` (always the function: H3 fix 1), `nearest` (upstream 3b9db78's label pick with
  hm_ca_nearest; also `HAKIOS_METAL_CA_VARIANTS=1`, as before). exact/own need QuartzCore's default.metallib, else nearest.
  A pipeline that does not link compiled whole is linked again with the label pick. One log line per pipeline:
  `CA pipeline LABEL: vs NAME [pick] constants N (zero count; idx=value ...) | fs ... | ok|refused: why`; a refused draw
  ends with `[pipeline LABEL: vs PICK, fs PICK]`. `HAKIOS_METAL_CA_LOG=0` silences the pipeline lines. Set them without a
  rebuild in `/var/local/hmetal/ca/env` (KEY=VALUE, HAKIOS_METAL_* only; m2shim sets them in backboardd), then
  `psx -k backboardd`.
- The variant tables are not built or committed: the plug-in reads the phone's own QuartzCore files at run time.
  `bench/ca-variants.py QC [--compare QC2] [--replay bbd-hm.log] [--table OUT]` reads the same files off the phone
  (QC = apple-ref/16.7 or a QuartzCore.framework copy: default.metallib and default.pipelinelib/ are plain files in the
  framework directory, not in the dyld shared cache), checks hm_ca_nearest's label form, and replays a log's draw labels
  through the three picks. `--selftest` runs on a synthetic library; `--table` refuses a path inside the repository.
  The generic MIT plug-in (`HakiosMetal.m`) cannot render (stand-in pipeline states, no render encoder).
- Pipelines on the hakios side need its compiler (`MESA=1`) or its compiled-stage cache (`HAKIOS_METAL_CACHE`,
  `/var/local/hmetal/cache`); without either, W3/W4 report "no pipeline" and W1/W2 still run.
- SweetMetal2 serves only listed processes: `/var/local/mtl2-clients` has `hmbench` (KNOWN-GOOD.txt).
- The hakios device needs what Metal's MTLAddDevice does after `-initWithAcceleratorPort:` (MTLAddDevice itself asserts
  on Metal's device array): hmbench sends `initLimits`, `initGPUFamilySupport`, `initFeatureQueries` (`--register`);
  SweetMetal2's device needs none (and must get none). The metallib header must be iOS's (platform 1, 2.7, os 0x82).
- Install a changed Mach-O under a new name and `mv` it into place: overwriting in place kept the old file's code
  signature attached to the vnode ("code signature does not cover entire file").
- First numbers and the frame comparison: project folder logs/hmetal/bench-summary-2026-11-06.md.

## Dispatcher: every process on hakios-metal, SweetMetal2 as the fallback (hmapp, hm-int2; bench/m2shim.m)

The kernel's IOAccelerator names SweetMetal2 (MetalPluginName/MetalPluginClassName = SweetMetal2 / SweetMTL2Device), so
Metal opens `/System/Library/Extensions/SweetMetal2.bundle` in every process. Its executable is the dispatcher
(bench/m2shim.m), which picks a driver per process (getprogname) and loads it as a plain bundle:

| path (/var/local/hmetal/...) | what |
|---|---|
| `lib/HakiosMetal.bundle` | hakios-metal (before the switch: `ca/HakiosMetal.bundle`, used when lib/ lacks it) |
| `lib/SweetMetal2.bundle` | the real SweetMetal2 (before the switch: `/var/local/SweetMetal2.bundle.pre-hmca`) |
| `env` | the one env file (before the switch: `ca/env`). `KEY=V` backboardd's (as before); `*:KEY=V` every hakios-metal process; `NAME:KEY=V` one process. Keys HAKIOS_METAL_* and HM_* only. |
| `hm-off` | exists: every process, backboardd too, on SweetMetal2 (the operator's global off switch) |
| `m2-procs` | names kept on SweetMetal2 (one a line, `#` comments, `*` = all) |
| `luma-boost` | `1`: CoreAnimation's luma boost pass on (both drivers); absent/other: off (default). Looked at every 2 s by backboardd |
| `m2-fallback` | `NAME KEY` lines the crash trip wrote; a line holds only while bundle KEY is installed |
| `ca/bbd-hm.starts`, `ca/bbd-hm.tripped` | backboardd's crash-trip files (the same trip as every process's) |
| `log/NAME.log` | each process's lines (dispatcher + plug-in) where it may append; else `$DARWIN_USER_TEMP_DIR/hakios-metal-NAME.log` |
| `cache/` | compiled stages, written by backboardd (prewarmed: HM_PREWARM); read-only seed for every other process, whose own writable cache is `$DARWIN_USER_CACHE_DIR/hakios-metal-cache` (HM_PREWARM=0 there) |
| `cache/src/`, `cache/bc/` | (hm-cache) what made each stage compiled into `cache/` (HM_CACHE_MANIFEST=1): the pre-warm compiles the missing ones again after a key change, and `bench/mkseed.sh` on the PC makes a seed of them. Phone-only data (Apple bitcode): PF only, never git |
| `cache-seed/` | read-only stages for every process, looked in after `cache/` (HAKIOS_METAL_CACHE_SEED); from `mkseed.sh compile` (host recompile under the current key) or `mkseed.sh harvest` |

(hm-int2) hakios-metal is the default for every process, backboardd included; gone: `hm-only` (staged list), `all-off`
and backboardd's `ca/bbd-hm` flag (ignored; backboardd logs a line when the old files are still there).

Crash trip, one for every process: each start on hakios-metal adds `time pid KEY` to its starts file (backboardd:
`ca/bbd-hm.starts`; others: `$DARWIN_USER_TEMP_DIR/hakios-metal-NAME.starts`, writable even when sandboxed), a clean exit
`x pid`. The 3rd start within 120 s whose earlier ones (same KEY) did not exit cleanly puts the process on SweetMetal2:
marker (`ca/bbd-hm.tripped` / `hakios-metal-NAME.m2`) holding KEY, `NAME KEY` in `m2-fallback`, and one line
`m2shim[pid NAME]: TRIP NAME -> SweetMetal2: ...` to /dev/console (kernel log), syslog and `ca/shim.log`. KEY is the
installed `HakiosMetal` executable's mtime and size, so installing a new bundle expires every trip (`trip expired: ...`
line). Clear by hand: rm the marker and the m2-fallback line.

Set in every hakios-metal process before the plug-in loads (env keys, so an env line flips each): `HM_CA_DEEP_FB=0`
(CAEnableDeepFramebuffer=false), `HM_CA_FB_COMPRESSION=0` (CAEnableFramebufferCompression=false +
CA_DISABLE_FRAMEBUFFER_COMPRESSION=1), `HM_CA_IOSURFACE_COMPRESSION=0` (CAEnableIOSurfaceCompression=false), as SweetMetal2
does in its processes, so CA makes no 550/551 deep surfaces; `1` leaves CA's own value. One `CA prefs:` line a process.
XR10 is off in every hakios-metal process (HM_XR10=0 forced: supportsExtendedXR10Formats NO, so CoreAnimation makes BGRA8
layers and tiles another process samples with no decode); `HM_XR10_ALLOW=1` in a process's env lets its HM_XR10 stand.
HM_TRACE is off unless the env says 1; the plug-in's log copy rotates at 16 MB (`NAME.log.1`), the dispatcher rotates
`ca/shim.log` and the process log at start. Luma boost: `luma-boost` = 1 on, else off; backboardd applies it with
CASetDebugOption(91 = CA_DISABLE_LUMA_BOOST) and hakios-metal draws the pass while the knob is on (its address in
HM_LUMA_KNOB); SweetMetal2 processes get SWEET_LUMA_BOOST (a line in mtl2-env/mtl2-live still wins there).
Template env file: bench/hm-env.default. Host test: `cc -o t bench/m2shim_test.c && ./t` (bench/build.sh runs it).

Multi-process on our kernel: each process opens its own v2 client (IOAcceleratorES type 0x4147) and gets its own 4 GiB
GPU window (64 windows, xnu/patches/0115 sweet_gpu_window_alloc), BOs from the one kernel GPU pool (as Turnip's); all
clients submit to the one ring, so another process's earlier submissions (an IOSurface it drew) are ordered before ours.
An IOSurface made elsewhere is imported as the caller's own pages (userptr) and cleaned/invalidated around submissions
(Cache maintenance above). The compiled-stage cache writes `.tmp` + rename, so readers never see half a file.

Later (kernel/ADT, not done): an accelerator that names a `HakiosMetal` plug-in directly (MetalPluginName =
HakiosMetal, the dispatcher as that bundle and SweetMetal2.bundle back to the real one), so /System's SweetMetal2 is
no longer replaced.

## Stage cache (hm-cache)

The key a stage is filed under is the bitcode, its function constants, IBO_BASE, the known buffer (a specialisation's
configuration) and `hm-stage-format N mesa VERSION+PATCHES` (compiler.cpp `HAKIOS_METAL_CACHE_FORMAT`, BUMP BY HAND when
codegen, a lowering, the AIR front end's output or the Stage/Kernel layout changes; the Mesa id comes from build.sh).
Compute kernels (compileAir: ycbcr_decode, xr_decode, apps' compute pipelines) are filed the same way since hm-int3, IBO_BASE
= kKernelCacheTag ("KRNL"); their manifests are re-made by compileAir in the pre-warm and in hm-seed. Format 2 = hm-int3.
A plug-in or core rebuild that changes no codegen keeps the cache. `HM_CACHE_KEY=build` = the old per-build key.
Switches (all core env, set in `/var/local/hmetal/env`): `HM_CACHE_VERIFY=N` (1 in N hits compiled again in the
background; "hm-cache: STALE ..." when it differs, the file replaced), `HM_STAGE_MEMO=1` (+`HM_STAGE_MEMO_MB=48`),
`HM_CACHE_MANIFEST=1`, `HM_COMPILE_SPLIT=1` (background threads on their own ir3 compiler: the render thread never waits
behind one). Host tool `out/bench/seed/hm-seed` (`build CACHE SEED`, `time LIB FN ...`, `compile`, `rewarm`).
