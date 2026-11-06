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
  verify". `bench/plugin.py` makes three edits (cache under /var/local/hmetal, device for `HAKIOS_METAL_ALL` processes).
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
