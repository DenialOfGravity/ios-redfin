# iOS on Pixel 4 (flame) - working notes

Port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (XNU xnu-8796 + iPhone X iOS 16.7.16 userland on
a Redmi Note 10 Pro, SM7150) to the Pixel 4 (`flame`, SM8150). Read `docs/ios-sweet/NOTES.md`
for the history of ios-sweet's kernel patches (0001-0081); this file records what differs on flame.

What works now is in [README.md](README.md). [HANDOFF.md](HANDOFF.md) is the engineering log, newest
first: every boot, what broke and why. Paths under `work/`, `logs/`, `backup/`, `dtb/`, `stock/`,
`downloads/`, `images/` and `research/` in either file are local and not in git (see the README).

## Test loop (no buttons, no screen)

1. Phone in fastboot (`adb reboot bootloader` from TWRP).
2. `fastboot boot shim/out/shim-boot.img` (shim alone = probe; with a payload = XNU).
3. The shim sets the PM8150 PON reason to bootloader, so a reset lands in fastboot.
4. `fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img`, then
   `tools/flame-log-pull.sh shim logs/X.txt` (and `xnu` for the XNU ring).
- `boot_a` holds TWRP (flashed 2026-10-19) so any reset without the PON reason boots TWRP
  (adb root), not Android. `boot_b` is stock. Restore everything with `images/.../flash-all.sh`.
- macOS asks "Allow accessory to connect?" the first time a new USB identity appears; until
  then `ioreg -p IOUSB` shows the phone `!registered`.

## Google ABL boot image rules (found the hard way)

`fastboot boot` fails with "Error verifying the received boot.img: ..." unless:
- the kernel is **LZ4 (frame) compressed** (uncompressed -> `Unsupported`),
- the Image header has **text_offset 0x80000** (0 -> `Invalid Parameter`),
- the image has an **AVB hash footer** (unsigned, algorithm NONE, padded to 64 MiB),
  `avbtool add_hash_footer --partition_size 67108864 --partition_name boot`.
Header v2, base 0, kernel 0x8000, ramdisk 0x1000000, tags 0x100, dtb 0x1f00000, page 4096,
stock `dtb` (two SM8150 base DTBs; ABL picks v2 and applies dtbo idx 11).
ABL loads the kernel at 0x80080000, the DTB at 0x84500000, the ramdisk at 0x84700000
(TWRP's 30 MB ramdisk went to 0xc0000000). max-download-size 256 MiB.

## Hardware facts (live FDT `dtb/live.dtb` + shim probe)

| | flame | (sweet) |
|---|---|---|
| CPU | 4x A55 (cpu0-3) + 3x A76 + 1x A76 prime, MPIDR 0x81000n00, MIDR 0x51df805e | 6+2 |
| RAM | <0x80000000 0x3cc00000>, <0xc0000000 0x140000000> (6 GB) | |
| GIC | GIC-600, GICD 0x17a00000, GICR 0x17a60000 stride 0x20000 | same |
| Timer | 19.2 MHz, vtimer INTID 19 | same |
| PSCI | 1.1 smc; CPU_ON, SYSTEM_RESET yes; RESET2, SYSTEM_SUSPEND no | |
| UFS | UFSHCI 3.0 @0x1d84000, LUN0 sda, userdata sda15 first 4K LBA 0x25e000, 0xc80ffb blocks | 2.1, same addr |
| Display | NT37280 cmd-mode DSC panel 1080x2280; ABL fb 0x9c400000 stride 4320 a8r8g8b8; CTL0, VIG0+VIG1 (540 each), LM0/1, DSC0/1, INTF1; **ABL enables INTF1 autorefresh** | video mode, 0x9c000000 1080x2400 |
| Touch | ST FTS (ftm5) on GENI SPI @0x890000 CS0, irq GPIO122, reset GPIO127, vdd L14A 1.8V, avdd L17A | Goodix I2C |
| Keys | power KPDPWR + vol- RESIN on PM8150 PON (SID0 0x800); vol+ PM8150 GPIO6 (SID0 0xc500), active low | vol+ PM6150L GPIO2 |
| PMIC arb | v5 @0xc440000, chnls 0xc600000, obs 0xe600000, cnfg 0xc40a000, EE 0, 512 APIDs | same |
| Reboot reason | PM8150 PON SOFT_RB_SPARE 0x88f bits [7:1] = 2 (bootloader) -> fastboot; Linux clears it to 0 before every reset; 0x10 (= WDT) at boot | IMEM ignored |
| Watchdog | APSS WDT 0x17c10000, 32765 Hz, ABL leaves it off; bite register 20 bits (~32 s max) | |
| USB | DWC3 @0xa600000, SNPS femto HS PHY 0x88e2000, QMP SS PHY 0x88e8000 | QUSB2 |
| ramoops | 0xa47ff000 (2 MB), alt 0xa49ff000 (TWRP /dev/access-ramoops) | |

DRAM partly survives resets (shim saw TWRP's kernel pointers at 0x100000000), but a marker
written to the alt ramoops region via /dev/access-ramoops did not survive reboot -> bootloader -> TWRP.

## Layout

- `shim/` flame shim (`build.sh`, Homebrew llvm + lld), `tools/` host tools, `phone/` code that runs in
  iOS on the phone (each directory has a `build.sh`; they use `work/userland/bin/ios-cc`)
- `xnu/patches/` ios-sweet 0001-0081 + flame patches from 0082
- `work/dxb/` blacktop darwin-xnu-build checkout; `work/dxb/xnu` is a git repo whose HEAD is
  "baseline + applied patches", new patches come from `git diff`
- Not for git: `backup/` (partition dumps, serials), `dtb/`, `stock/`, `downloads/` (TWRP,
  IPSW parts, AOSP tools), `work/`, `logs/`

## Flame patches

- 0082 IORPC.kernelContent: Xcode 16+ iig emits it; field added (kernel only) and set at the
  two user->kernel IOUserServer dispatch sites, as in newer XNU.
- 0083 flame basics: early console on the 0x9c400000 / 2280-row framebuffer (SWEET.h
  constants used by start.s, sweet_early.c, arm_vm_init.c), vol+ on PM8150 GPIO6, the Redmi
  touch driver off by default (sweettouch=1 forces it; it would vote L11C to 3.2 V), APSS
  watchdog keepalive in SweetPlatformExpert (flamewdt=0 off, flamereboot=N stop after N s).
- 0084 early framebuffer block mapped Device-GRE, not Normal-NC (no speculative reads into pil_slpi).
- 0085 ST FTS touch (ftm5) over GENI SPI; 0086 its HW clock gating + cpufreq for all 8 cores.
- 0087 fixed-rate vsync + fps stats. 0088 flame USB offsets.
- 0089 log timestamps, RAM mirror of the console ring, poke scripts; with 0090: USB through the apps SMMU
  the way ABL does it (own context bank, translation off) and the UDP command server.
- 0091 USB ARP fixes, TCP file server, `sync` command (0092-0093 included).
- 0094 /dev/pmem (physical memory and SCM calls for userland drivers), file server READ.
- 0095 the A640 core in the kernel, GPU present (frames scaled to fit by the GPU).
- 0096 SweetAccelerator (IOAcceleratorES for Metal, `sweetmtl=1`); 0097 its user client.
- 0098 panic logs into the persistent log ring. 0099 PM8150 RTC time of day (`sweetrtcoff`).
- 0100 clonefile copy fallback on HFS, tap/swipe commands (0101 included).
- 0102 RPMh ARC/VRM votes from userland (PMEM_RPMH). 0103 AMFI stand-in answers trust cache queries.
- 0104 planar IOSurfaces report summed bytes-per-row/element (blank Safari pages).
- 0105-0109 GPU: VA reuse, more page tables, perf vote (`sweetgpufreq`), recovery, async submit.
- 0110-0112 Apple continuous display corners (final: 0112, GPU scissored bands + CPU row masks).
- 0113 wait for a client's last fence before unmapping its memory.
- 0114 the AMFI policy answers dyld's policy query: @rpath frameworks of third-party apps load.
- 0115 v2 GPU user client (NativeAdreno ABI) for Turnip: GPU buffers from RAM XNU does not manage
  (no cacheable physmap alias), multi-IB submits, progress-based hang detection.
- 0116 `sweetmtl=2` selects SweetMetal2 (metal2vulkan + Turnip). 0117-0118 pool accounting, submit timing.
- 0119 present not waited for. 0120 per-process GPU stats, BCM (bus/DDR) votes over PMEM_RPMH.
- 0121 vsync notifications carry the refresh period (CA's frame deadlines were always past).
- 0122 DDR + memory NoC voted up at GPU start (`sweetddr`), GPU pool caps (`sweetgpucap`, `sweetgpureserve`).
- 0123 en0 + Apple80211 for wifid (ios-topaz 0900 + 0128, from HackiOS), the station over /dev/wlan to
  wland in userland.
- 0124 10-bit surfaces as BGRA only for WebContent (`sweetsurf10`); the joined network keeps its scan record.

## Redfin patches

Pixel 5 (`redfin`, SM7250 lito) on top of the sunfish series; plan and hardware map in
[docs/redfin/port-plan.md](docs/redfin/port-plan.md). Numbered 09xx (were 0150-0152 until
2026-10-24) so upstream's next sunfish patches (0150, 0151, ...) never share a number; they apply after
all upstream patches. Kernels "0151"/"0152" in the logs = 0901/0902.

- 0900 redfin basics: early framebuffer 0xa0000000 (SWEET.h), vol+ PM8150L GPIO5 (0xc400, SID 4),
  USB femto HS PHY at 0x088e3000 with the redfin param-override-seq, battery/charger reads on PM7250B
  (SID 2) and the SPMI read allowlist for it (RTC stays on SID 0).
- 0901 sec_ts touch (`bsd/dev/sweet_sects.c`, `sectouch=1`): Samsung S6SY79x on GENI SPI SE0 (0x880000),
  mode 3, TX-only/RX-only framing as sec_ts.c; L17A 3304 mV allowed in sweet_rpmh; RPMh cmd-db falls
  back to lito's fixed region 0x80860000; flame's FTS driver now off by default (its TLMM address is
  the GPU on lito).
- 0902 USB on lito: GCC USB clock offsets from gcc-lito.c (sleep 0xf018, utmi 0xf01c, cfg-noc 0xf07c,
  aggre 0xf080, aux 0xf054, clkref 0x8c010), DWC3 SMMU stream `USB_SID` 0xe0 (was 0x140); cpufreq-hw prime
  cluster 0x18327800 (boost = fewer cores than the cluster, as Linux); battery label PM7250B.
- 0903 upstream sunfish 0151's QUSB2 v2 init off by default (`sweetqusb=1` to force): redfin's HS PHY is
  the femto at 0x088e3000 and 0x088e2000 is its EUD register. Upstream 0150 (GPU pool skips wland's
  rmtfs buffer at 0xe1000000) applies to redfin unchanged: same pool range, same rmtfs address.
- 0904 Adreno 620 (`A6_GPU_ID` 620 in sweet_a6xx.h; 618/640 keep their values): lito addresses (GPU
  0x3d00000, GPUCC 0x3d90000, SMMU 0x3da0000, RSCC 0x3de0000), A650-family GMU (kgsl gmu_vma: DTCM at
  0x10004000, cache memory from a650_gmu.bin's PREALLOC_REQ blocks), RSC/PDC with cmd-db addresses,
  HFI v2 with 275/400/500/625 MHz and lito's BCMs, A620 hw_init (512 KB GMEM, UBWC 3 hbb 14, APRIV,
  a650_sqe.fw), a620_zap into 0x8bc15400, Mesa FD620 values for the present blit, Turnip device info
  0x06020000. GPU buffer pool skips kregistry_mem 0xc8000000 and the 0xe2000000 log trail.
- 0905 double-buffered GPU present (`sweetfbflip=1`, off by default): blit into the buffer not on screen
  (second one at 0xa0a00000 in the splash region), flip VIG0+VIG1 once the fence lands, never write the
  old buffer before the flip latched (as ios-sailfish 0164).
- 0906 panel brightness: DCS 0x51 (10-bit, 0x4..0x3ff) through DSI0's command DMA (0x0ae94000), packet at
  0xa2200000 in the splash region, sent from its own thread between autorefresh frames (topaz 0104/0107);
  CoreBrightness via IOMFBBrightnessLevel, plus AppleARMBacklight with `sweetbl=1` (digital dimming then
  runs too: opt-in while CA renders on the CPU). `sweetbl_dcs=0` disables it.
- 0907 DRV2624 vibration motor (`sweethaptic=1`; `=2` buzzes once when ready): I2C 0x5a on GENI SE9
  (0x98c000, sweet_touch.c's FIFO code), enable line PM7250B GPIO1 via the APPS SPMI channel, SE9 clocks
  and pins gpio46/47. `tools/flame-cmd.py 'v 200'` buzzes 200 ms. Not hooked to iOS yet.
- 0909 haptics: skip the halt check of the hardware-gated wrapper 1 s_ahb branch (r9 stopped there).
- 0908 a620 zap shader into 1 MB-aligned memory (b02's p_align; r8 failed PAS mem setup at 0x8bc15400 with
  0xffcfffba after GMU, HFI, GX and ME_INIT had all worked): `sweetzappa=PA`, 0xc8100000, then the DT region.
- 0910 no set/way D-cache clean on every idle WFI (sailfish 0180: idle CPU 133% -> 22%); AppleKeyStore
  says no passcode (lock state 0x6, sls 3) so coreduetd stops polling it; AKS logs 5 lines per selector.
- 0911 GIC SPIs for SWEET drivers (sailfish 0181: `sweet_spi_attach`, dispatched from sleh_irq); sec_ts on
  its attention interrupt (TLMM gpio9 -> summary SPI 208 = INTID 240) with a polling fallback;
  `sectouchirq=0` polls as before.
- 0912 vibration for iOS: sysctl `kern.sweet_haptic` (ms | strength << 16) queues a pulse;
  `sweetui=1` inserts /var/local/lib/SweetHaptics.dylib (phone/haptics) into SpringBoard and apps,
  mapping UIKit feedback generators and the vibrate sound onto it; `sweetenv=` from sailfish 0104/0163.
- 0913 network privileges from the iOS Sandbox's entitlement map (socket delegation, network statistics,
  NECP, global proc info) for the kernel, platform binaries and entitled processes. r11 showed WebKit
  networking, nsurlsessiond, trustd and mDNSResponder denied PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, which
  makes necp_client_add refuse every connection they open for an app.
- 0914 `/dev/redwood-audio` + `/dev/redwood-haptic`, the kernel ends of HackiOS's RedwoodAudio.driver
  (phone/audio, MIT): iOS plays its haptics as waveforms on the Actuator audio device once the ADT has
  /product/haptics closed-loop (tools/mkadt.py now copies it from the D22 tree); each write's peak drives
  the DRV2624 (`sweet_haptic_drive`). Speaker frames are dropped until the ADSP path exists. Install the
  plug-in with tools/redfin-install-audio.sh; SweetHaptics.dylib (`sweetui=1`) is no longer needed.
  Works (r13c): the root also needs /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin back (sweet's image
  moved it to HAL.disabled; without a device it crashed mediaserverd) and a fresh MobileGestalt cache
  (it keeps "no haptics" from the first boot).
- 0915 the audio DSP (`adsp=1`, `adsp_step=N`): HackiOS's redwood_adsp.c with lito's DSP memory
  (0x89200000, 40 MB); firmware in /private/var/local/fw (Android's /vendor/firmware/adsp.*).
- 0918 flash LED as a flashlight (`sweettorch=1` look, `=2` usable, `=3` blink at 5 mA when ready): PM8150L
  qpnp-flash-led-v2 (SPMI SID 5, 0xd300) channels 0+1, torch mode as leds-qpnp-flash-v2.c; poke `t MA [MS]`,
  sysctl `kern.sweet_torch` 0..1000; <= `sweettorch_ma` (100, cap 200) per channel, <= `sweettorch_secs` (120).
- 0919 camera sensors (`sweetcam=1` look, `=2` poke `c N` probes sensor N, `=3` all three at boot): rear IMX363
  (CCI0 m0, 0x20), ultra-wide IMX481 (CCI0 m1, 0x34), front IMX355 (CCI1 m0, 0x34) powered up in their module files'
  order (SLG51000 LDOs over SE9 at 0x75 with Google's tuning, PM8150L GPIO 2/8, reset GPIO 23/25/21, MCLK from camcc
  at 19.2 MHz, titan top GDSC), chip ID register 0x0016 read by bit-banged I2C on the CCI pins. Needs sweethaptic=1.
- 0920 XNU's I/O map (osfmk/arm/io_map.c) 8 -> 32 MiB: ml_io_map() is KMA_NOFAIL and SWEET drivers never unmap;
  ~600 KB more (0919's first version) and every boot reset a few seconds later (r16a-r16e).
- 0921 camera capture, step 1 ("poke c 20", sweetcam=2): camcc PLL0 + Titan RCGs/branches, GCC camera AXI, mx.lvl
  vote, apps-SMMU bypass context bank for the IFE streams, CSID-lite test pattern -> IFE-lite RDI0 -> write master 0
  into a GPU-pool buffer (1600x1200 RAW10). HackiOS redwood_camera.c's sequence with lito's clocks; all undone after.
- 0922 camera capture, step 2 ("poke c 21 SENSOR MODE OPTIONS"): camcc PLL2 for the 24 MHz MCLK, the sensor's tables
  from /private/var/local/fw/camera-<sensor>.tab (tools/redfin-camtab.py; vendor data, phone only), CSIPHY v1.2.2
  D-PHY, CSID-lite RX -> RDI0 -> IFE-lite WM0. c 20/21 run on their own thread (pokes run on the thread that sends
  the log: r17a/r17b's c 20 went down with nothing logged).
- 0923 /dev/redwood-camera (sweetcamdev=1 with sweetcam=2): HackiOS cam=5's device for its camera_device.c in
  mediaserverd: frame header + RAW10 per read, EXPOSURE/SENSOR/FOCUS writes; sessions on stream_up(), three GPU-pool
  places, next address at each RDI0 SOF; sensors 1 back IMX363, 2 front IMX355, 3 ultra-wide IMX481; linear gain.
- 0924 Sandbox stand-in: extension issue (op 5) writes a token into libsystem_sandbox's buffer, consume (op 6) gives
  handle 1. tccd took the old ENOENT for a refusal: Camera.app's AVCaptureDeviceInput failed -11852, black preview.
  Also needed on the phone: MediaToolbox SubServerConfiguration.plist StartRecorderServer=true (capture servers).
- 0925 AppleCLCD DisplayPipePlaneBaseAlignment as { PlaneBaseAlignmentLinear = 64 } (a dictionary, as on an iPhone):
  VideoToolbox's FigGetAlignmentForIOSurfaceOutputToDisplayService took the number for one and mediaserverd died
  ("-[__NSCFNumber objectForKey:]") each time Camera.app's capture session started.
- 0926 /dev/redwood-camera: PMIC GPIO writes tried three times; a session whose sensor did not start is tried again
  (three failures in a row before EIO for the boot). r18d gave the device up after one gpio8 read-back of 0.
- 0927 IOSurface lock result: a plane's bytes per row also at +0x10 of its entry, where IOSurface.framework (16.7)
  reads it. Bi-planar surfaces had 0 bytes per row per plane: green photographs, black camera preview, VT -12902.
- 0928 SweetAccelerator: every refused v2 call logged (first 40 per client), refused imports/maps with details.
- 0929 the rear camera's lens: the IMX363 module's AF motor is driven by its OIS controller, an ON Semi LC898129
  (module file: actuator/OIS/EEPROM "lc898129dp", slave 0x3b, 16-bit registers, 32-bit data; live DT actuator@0/ois@0 on
  CCI 0 master 0 with VAF): status 0xF100 polled, servos on (0xF010 <- 7), position 0xF01A <- 0x20000 | code (codes
  400..725 for 0..1023 until the per-unit calibration is read; `sweetcam_af_inf`/`_mac`, `sweetcam_af=none|NAME`);
  FOCUS works, frames carry `focus` and LENS_MOVING; parked before VAF goes off. (0930 is kept for the calibration.)
- 0931 /dev/redwood-camera exposure contract: EXPOSURE.d digital gain (0x020e.., 256 = 1x), reserved[3] its range,
  reserved[4] fps x 1000; group hold from the "holdon"/"holdoff" tables; margin/black/analog cap from the table file's
  new "info" table (tools/redfin-camtab.py; old files keep 40/64/8x-16x); frames report the clamped values.
- 0932 /dev/redwood-camera modes: SENSOR.c class (auto/photo, video 16:9 >= 30 fps, video60) when b is ~0; the same
  sensor+mode again is a no-op, another mode of the streaming sensor switches without a power cycle, MODE_CHANGED on the
  first frame after; a failed select keeps the previous sensor; table files read once ("c 23" forgets them).
- 0933 flash for camera frames: sweet_torch_flash() with the PM8150L module's safety timer (<= 300 mA/channel,
  <= 100 ms; pre-flash at torch current <= 500 ms); FLASH lights at the SOF before the frame, off after its end,
  FLASH_LIT on the lit frames; SOF time in reserved[1]/[2].
- 0934 /dev/redwood-camera stop/fault hygiene: halt at a frame boundary, SMMU fault only for FSR & 0x800001fe, the
  write master's next place never read, read() waits <= 1 s (EAGAIN at close/SENSOR), 100 ms start debounce, 300 ms
  close linger.
- 0940 IOSurface rows: 0159's 64-byte rounding also for a one-plane surface given with plane info and for every plane of
  a planar surface (a plane whose rows grew is moved past the one before it; first 16 logged `SWEET SURF: 0940`):
  `sweetsurfplanar=0` leaves planes as asked, `sweetsurfalign=0` all of it. For Turnip's explicit layouts (pitchalign 6).
- 0941 render server GPU resilience: a GPU reset caused by another process's job is not reported to backboardd/SpringBoard
  (their device stays; the hung job's pid is logged; `sweetgpuspare=0` as before), their refused imports are tried again
  (4 x, 2-16 ms), refusals log both prepare() results.
- 0942 the `present:` line per 300-frame window: window ms and presents/s, per-frame clean/blit, put off / flips / late in
  the window, mode (single buffer / flip) and `sweetgpufreq`, totals in brackets.
- 0943 `SWEET FB: 5s: swaps N shown N dropped N | le11 a le18 b le26 c le35 d gt35 e | p50 X.X p95 Y.Y ms | blitq Q us fence F us | hz H` (always); `sweetfbx=` 0x1 TimingElements/SyncRate + GetBlock(0x40) + sel 77, 0x2 no dcache clean unless the surface's seed moved; `sweetfb_hz=90` = DCS 0x53 0x38 + 11.1 ms ticks; poke `p fc`, `p hz 60|90 [tc]`.
- 0944 `sweetfbx` 0x4 (off by default, needs the present thread): present at SwapEnd (2-deep queue, SwapEnd held <= 50 ms when full, put-off presents retried every 1 ms, landed 0905 flips flushed at once) and vsync ticks phased 0.5 ms after INTF1's TE (tear-check frame/line counters; `SWEET FB: tick phased:` x3, free-running if the counter stops).
- 0946 `sweetfbx` 0x8 (off by default): CPU present on the present thread (fit-scale nearest sample, 1125x2436 -> 1080x2339, corner mattes, source clean+invalidate; back buffer + flip with `sweetfbflip=1`), no wait behind the GPU ring; 0944 phasing falls back to INTF1 frame counter 0x0ac when the TE counter reads 0; stats line + `| vis V us cpu C us` (SwapEnd -> panel-visible, CPU copy); present thread bound to CPU6 while 0x8 (`sweetfbcpu=N`); poke `p fbx HEX` sets 0x2/0x4/0x8 live.
- 0945 touch stats (always): `SWEET SECTS: 5s: events N (moves M, batched B) rate R/s | dt p50/p95/max ms | gaps>20ms G | irq->dispatch p50/p95 us | wake p50/p95 us | per drain avg A max B (C us max) | x X` (sample time = attention IRQ) and `SWEET HID: real|swipe: moves N dt p5/p50/p95/max ms | gaps>20ms G | dispatch avg/max us` (HID timestamps iOS gets; 5 s or at lift after >= 10 moves). `sectouchx=` 0x1 HID timestamp = IRQ time + one move per finger per drain, 0x2 READ_ALL_EVENT for >= 3 queued, 0x4 read-only probe of 0x30/0x34, 0x8 touch thread priority 92.
- 0947 fit within 2 px takes the whole panel (row 2339 no longer flickers; GPU blit and CPU present); once fitted, swaps with no layer / the panel's own surface / not fittable keep the last frame instead of the crop copy (which flipped back to buffer 0: the CPU-present tearing). `SWEET FB: swap N (layers ..) kept the fitted frame` / `not fitted` x4.
- 0949 CPU present waits for the GPU fence submitted at the frame's SwapEnd (100 us sleeps, <= 50 ms) before reading it (it read half-rendered frames: black regions with a stepped edge); stats line + `gpuwait W us late L`.
- 0977 vsync pacing (`sweetpace=1`, poke `p pace 0|1`; default 0 = as before): while frames flow, a 90 Hz tick that finds CA still drawing (no SwapEnd since the last notification) is held and sent at that frame's SwapEnd, so CA (CA_FORCE_TIMER=1, starts frames only on type-5 notifications) is not quantised to 2-3 ticks per frame; a held tick no SwapEnd takes lapses to the next tick. Line `SWEET FB: pace 5s: held H at-swap A (U us before the next tick) lapsed L`.
- 0950 ADSP FastRPC (WS-D): sweet_adsp.c answers `fastrpcglink-apps-dsp` (64 x 0x64 intents) and `glink_ssr`;
  `/dev/redwood-fastrpc` (0600, one opener) carries 40-byte messages / 16-32-byte answers for phone/fastrpc/fastrpcd;
  ioctls FASTRPC_SMMU_IDENTITY (SID 0x1403-5 -> apps SMMU bank with translation off, logged `SWEET FASTRPC: SMMU:`)
  and FASTRPC_ALLOC (RAM < 4 GiB from the GPU pool, zeroed, 32 buffers kept; `SWEET FASTRPC: N bytes for the DSP at PA
  (GPU pool ...)`); `adsp=2` starts the DSP on the first open of the device (fastrpcd up first); `adsp_sleep=1` Android's other APPS SMP2P
  entries ("sleepstate" = 1, "rdbg" = 0); `adsp_qlog=1` IPCRTR datagrams/intents logged (`SWEET QLOG:`); `adsp_fastrpc=0` old way, `=2` log every message; poke `f 0` status, `f 1` DSP
  words + last words, `f 2` QRTR services/ports, `f 3 SID` identity, `f 5` SMMU faults (`f 9` help).
- 0951 pd-mapper (QMI 0x40, port 0x4001) in the ADSP's kernel QRTR: msm/adsp/{root,sensor,audio}_pd + avs/audio,
  instance 74; announced after the DSP's HELLO and to its lookups; PD failure reports logged (`SWEET PDMAP:`);
  `adsp_pdm=0` off; poke `f 4` announces again.
- 0956 `/dev/redwood-qrtr` (qol-plan 3.1): one QRTR port (0x5000+n) per open, {node, port, data} datagrams, ioctl
  QRTR_LOOKUP, DEL_CLIENT on close; sysctl `kern.sweet_adsp_services` ("svc:inst@node:port,...").
- 0952 speakers' path through the ADSP (`bsd/dev/sweet_sound.c`, HackiOS redwood_sound.c's APR layouts): `sound=1` LPASS
  core votes (AFE 0x100f4 blocks 3/4), LPI gpio6-9 func2, QUIN TDM clock 0x208 6.144 MHz + port 0x9040 (4 x 32-bit slots,
  mask 0x3; no group, as Android); `sound=2` + SMMU stream 0x1401 identity bank, ADM/ASM session 1, -30 dBFS tone with the
  amps off, path left up; `sndsecs` `sndlpi=0`; poke `a status|tone [MS]`. Default off.
- 0953 the two CS35L41 amps (`sweet_cs35l41.c`, SE9 via sweet_se9_xfer, ASP bypass, OTP trims, gain 17) + HackiOS guard
  (`sweet_guard.c`): `sound=3` tone audible at <= -20 dB vs Android, amps only while playing (off after 2 s);
  `sndbelow`, `sndgpio=1` (PMIC GPIO resets: PM8150L GPIO11 left, PM7250B GPIO11 right; allow-listed SPMI writes);
  poke `a gain DB|off|on`.
- 0954 `/dev/redwood-audio` -> ring -> ASM (`sound=4`): `redwood_audio_set_sink()`/`redwood_audio_volume()` in
  sweet_audio.c, iOS volume added to `sndbelow`, `sndgain` (guard gain, default 12 dB), amps gated after 2 s of silence.
- 0955 microphones (`sweet_mic.c`, `mic=1`): ALC5513 (rt5514, plain TDM mode) on SE7 0x57, reset TLMM 91, PRI TDM TLMM
  49-52, AFE 0x9001 (8 x 32, mask 1), ADM record + ASM session 2 reads, `/dev/redwood-mic` per the plug-in ABI;
  `micgain`; poke `a mic SECS` -> `SWEET MIC: files read /dev/pmem PA LEN`.
- 0960 TSENS temperatures (bsd/dev/sweet_thermal.c, read only): `SWEET THERMAL:` every 5 s (silver = hottest A55,
  gold = sensors 9/11, prime = 10/12, cpuss, gpu, ddr, camera, npu, aoss), sysctl `kern.sweet_temp_*` (deci-degC);
  once at 20 s the PM7250B charger/QG/SDAM registers (`SWEET BATTERY: boot regs ...` + decoded FCC/FV/JEITA/WD line);
  SPMI read allow-list + 0x15/0x16/0xb1. `sweettherm=0` off.
- 0961 cpufreq readback: tables parsed the Linux way (`~` skip, `*` boost rows), the old boot selection kept for
  `flamecpufreq=0/1/N`; `SWEET CPUFREQ: busy ...` every 5 s (per-CPU busy % from recount idle time via
  sched_prim.c `sweet_cpu_idle_abs()`, perf-state readback, cycle-counter MHz, LMh ceiling); command server
  `w stat|freq DOM IDX|table|gov|chg` (sweet_poke.c hands "w WORD" to sweet_power_cmd; "w ADDR VAL" unchanged).
- 0962 `sweetbig=2`: foreground threads try CPU6 (gold) first, CPU7 (prime) only while CPU6 is busy (1 = old order,
  default). tools/mkadt.py: redfin clock values (1804.8/2208/2400 MHz); cluster-type stays 0 (no AMP in this kernel).
- 0963 DVFS governor, `flamecpufreq=2` (default 1 = pinned max as before): 30 ms per-domain tick, util = busiest CPU of
  the domain, target = cur * util / 80 % (x2 at >= 95 %), lowest valid row above it; up at once, down after 3 ticks;
  silver floor 1.1 GHz; perf-state writes only. `w gov 0|1 [MS]`; `w freq` takes a domain out until `w gov 1`.
- 0964 thermal caps (sweet_thermal.c, 1 s sampling): hot = max(gold, prime, cpuss, camera); level 1 > 75 C (gold/prime
  <= 1.8 GHz), 2 > 85 C (gold/prime 1.2, silver 1.5 GHz, GPU level 2), 3 > 100 C (gold/prime lowest row, silver 1.0 GHz,
  GPU 1), each released 10 C lower, logged once per change. `sweettcap` bit 0 CPU (default 1), bit 1 GPU (runtime GMU
  HFI perf vote via sweet_gpu_level_cap()/a6_perf_vote(), off by default), 0 = log only. `w cap N|auto`, `w gpu N`.
- 0965 PSCI idle, `sweetidle=1` (default off): CPUs 1-7 call PSCI CPU_SUSPEND 0x40000003 (lpm "pc"; never the
  notify-rpm cluster states) instead of WFI when the next timer is beyond 1774/2207 us; resume via resume_idle_cpu ->
  arm_init_idle_cpu + sweet_idle_resume() (GIC ICC regs, timer re-arm). Broadcast wake = arm,armv7-timer-mem frame 0
  (0x17c21000, SPI 8 on CPU0) armed at the earliest sleeper deadline minus exit latency, IPIs the sleepers. Probes
  PSCI_FEATURES first; a PSCI error turns it off. `SWEET IDLE:` at boot; counters in the `SWEET CPUFREQ:` line.
- 0966 battery: 4000 mAh, 5 s poll, SOC seeded from QG SDAM 0xb147 (if 0xb146 valid and within 15 % of the voltage
  estimate) then coulomb-counted from QG LAST_ADC_I with a 0.2 %/poll pull to the voltage estimate at |I| < 100 mA;
  1 %/poll slew; IsCharging = I > +50 mA; Temperature from the JEITA zone (0x100d). `SWEET BATTERY: SOC seeded ...`.
- 0967 SPMI write path (sweet_spmi_write): APID channel EXT_WRITEL (Linux pmic-arb v5), owner EE 0 checked, only SID 2
  0x1061/0x1070/0x1341/0x1342/0x1365/0x1366/0x1370, read back + `SWEET CHG: write ...`; nothing unless `sweetchg=1`.
  `w chg wr ADDR VAL`.
- 0968 charger policy (bsd/dev/sweet_charger.c, every 5 s after the 20 s register dump): DCP/CDP/Rp1.5 -> 1500 mA,
  Rp3.0 -> 2000 mA ICL in HC mode (0x1370, 0x1366.0=1, 0x1342.0=0, 0x1365.4=1), FCC 2000 mA (1000 when JEITA soft or
  SMB die over bound), FV lowered to 4.45 V only if higher; SDP/float left to the hardware; dry run unless `sweetchg=1`;
  `sweetchgpol=0` off; <= 200 writes/boot. `w chg` (decoded state), `w chg apsd`, `w chg pol 0|1`.
- 0969 USB `ud_thread`: 10 ms polls after 200 quiet 1 ms polls (no event, no TX in flight), 1 ms again on the next event;
  `sweetudpoll=1` = always 1 ms.
- 0970 big-core governor + boost (default off): `sweetgov=XXYY` (e.g. 4060) gold/prime up >= XX % busy, top >= YY %, down one step/tick after 100 ms hold under XX-10 %; touch (SweetHID down/move) and SwapEnds < 50 ms apart hold >= `sweetboost=MHZ` (1800) for `sweetboostms=MS` (200); 0964 caps stay the ceiling; `SWEET GOV: 5s: boosts N capped N | gold MHz:ms .. | prime ..`; poke `w boost 0|1` (needs the governor, `w gov 1` / flamecpufreq=2).
- 0971 sensor HID services `SweetHIDAccel/Gyro/ALS/Prox/Compass/Pressure/Orientation` (SweetHID.cpp; usage pairs 0xff00/3,
  9, 4, 8, 0xa from IOHIDFamily-1035.70.7's AppleHIDUsageTables.h, filled in; pressure 0x20/0x31, orientation 0x20/0x8a
  with DeviceOrientationUsage 0x64 + UIDeviceOrientation, which locationd's CLOrientationNotifier matches/accepts):
  every open, user client (pid), copyEvent type and setProperties key/value logged `SWEET MOTION:`, ReportInterval kept;
  `sweetmotion=MASK` (default 0x77 = all but prox, which crash-looped backboardd; 0x7f all;
  0x100 = Transport "SPU"; 0x200 = neutral copyEvent answers on every service; ALS and prox always give one);
  user-space property reads logged.
- 0972 `/dev/redwood-motion` (sensd's samples, qol-plan §3.3; read() = status, ioctl MOTION_GET_WANT) and `poke m`
  (`m fake accel|gyro|als|prox|compass|pressure|orient [V..]`, `m rot N`, `m stop|secs|status|log|knob|knobs`):
  bsd/dev/sweet_motion.c.
- 0978 CPU present maps the source surface copyback-cacheable (kernelAddress() is write-combined = uncached for non-IOBMD surfaces: 41 ms vs 4.3 ms copies on r36a); last 4 maps kept.
- 0980 Bluetooth probe logging (`sweetbtlog=1`, off by default): every kernel-control `CTLIOCGINFO` name, every failing
  open() of /dev bt*/uart*/cu.*/*bluetooth* nodes (`SWEET BTLOG:`, 300 each), and the sweetiolog IOKit trace (`IOK name:`,
  1000 lines) on bluetoothd, BlueTool, wirelessproxd, bluetoothuserd, BTLEServer.
- 0981 /dev/btuart (`btuart=1`, no node by default): WCN3998 HCI UART on QUP0 SE5 (0x894000, SPI 606 via 0911) for
  phone/bt/btd: GENI FIFO mode, RTS/CTS, 16 KiB RX ring, baud/flow/drain/flush/timeout/stats ioctls; ENXIO without
  XBL-loaded UART firmware; `btuartos=16|32`; poke `b 0` state, `b 1` HCI version, `b 2` QCA EDL version.
- (no patch) `phone/fastrpc/fastrpcd` (WS-D, GPL-3.0+, hexagonrpcd port): attaches to the ADSP sensors PD over
  `/dev/redwood-fastrpc` (buffers FASTRPC_ALLOC + /dev/pmem, Android adsprpc layout, session 3) and serves its
  apps_std file calls from `/private/var/local/hexagon` (tools/redfin-install-hexagon-wsl.sh); launchd job
  `org.iosonpixel.fastrpcd`, log `/var/local/fastrpcd.log`, exit 3 when the DSP goes; `-v`, `--mmap-method 2|4`.
- 0957 poke `a gain N` (or `-N`): N dB under Android's level; hard cap 6 dB under (ASP bypass, no protection firmware)
  for sound=3 and 4, also for `sndbelow`.
- 0958 CS35L41 lazy re-probe: an amp that missed the boot probe (r29b 0x40 NO ANSWER) is reset+probed+configured again at the next ON (watcher thread, >= 10 s apart, <= 20/boot); `poke a probe` forces it; boot failure logs the SE9 state; log labels 0x40 = top, 0x41 = bottom (routing unchanged)
- 0982 stand-in Bluetooth nubs + AppleOLYHAL (`sweetbtnub` bitmask: 1 "bluetooth", 2 "marconi-bt", 4 AppleOLYHAL;
  off by default): SweetBTNub services for bluetoothd's IONameMatch notifications carrying the D22 bluetooth-node
  properties (transport-encoding 3, 3 Mbaud, vendor/product/profiles/coex, compatible "bluetooth,n88"); AppleOLYHAL
  is an empty IOService for BlueTool's IOProviderClass match. Property reads logged found/MISSING, user-client
  requests refused + logged, setProperties keys logged (`SWEET BTNUB:`).
- 1010 (was ws-q-bt's 0983) Bluetooth bridge (`btbridge=1`, no nodes by default): /dev/cu.bluetooth + /dev/uart.bluetooth (one H4 pipe
  bluetoothd opens) <-> /dev/btbridge (btd --bridge) + dummy /dev/btwake. Two 16 KiB rings, blocking/O_NONBLOCK/
  select; btd forwards standard HCI to the WCN3998 and fakes/logs vendor 0xFC__ commands (`SWEET BTBR:`, btd `BTD BRIDGE:`).
- 0973 AppleARMBacklight publishes the panel's nits for CoreBrightness (MinNits/MaxNits/MaxNitsPanel,
- 0984 vsync pacing modes, `p pace N [MIN_US]` / `sweetpace=N sweetpace_min=US` (N & 7: 0 as before, 1 0977 hold, 2 extra tick at each SwapEnd, 3 tick at max(SwapEnd, last + MIN), 4 quiet with a 100 ms watchdog for a CA on its own clock; +16 early ticks only while the 0944 swap queue is empty; MIN 8000 us). Line `SWEET FB: pace 5s: held .. at-swap .. lapsed .. | early E skipped S gated G dog D | pace N min M`.
- 0987 swap-complete ticks + swap trace, `p swapdone [MASK|trace]` / `sweetswapdone=MASK` (1: a type-5 tick when a frame's present is done: blit fence landed, CPU copy done, scanout latched, at least sweetpace_min after the last tick; 2: the next 256 IOMFB/submit events with thread ids as `SWEET SWAPTRACE` lines). CA only ever enables notification type 5 and learns swap completion from SwapWait, so there is no other swap-complete channel. Default 0.
- 0988 `sweetgputs=1`: a submit's end stamp by CP_EVENT_WRITE RB_DONE_TS | TIMESTAMP and its start = max(own stamp, previous end): accurate 5 s `GPU busy` without the 0157 WFI (CP_REG_TO_MEM stamps time the CP, not the GPU). Default 0.
- 0992 sampling profiler for one process, `p prof start NAME|PID [HZ [SECS [DEPTH [WAITHZ]]]]` (8 MiB buffer read over /dev/pmem by tools-claude/sweet-prof.py), plus `p disp` and `p phase`.
- 0994 kdebug frame-trace events, class DBG_DRIVERS subclass 0xF5 (0x06F5xxxx; codes in iokit/Kernel/arm/SweetTrace.h): vsync tick/notification/notify result, IOMFB external methods START/END, swap path, present, shown, swap done, scanout queue/flip/latch/back pressure, GPU submit/gate/fence stamps/waits/clock calibration, SweetAccelerator v2 methods, touch. Only while kdebug is enabled (phone/tools/sweettrace, tools-claude/sweet-trace.py); no boot-arg.
- 0995 vsync tick precision, all off by default (boot-arg sweetvsync=MASK sweetvsync_lead=US, poke `p vsync [MASK [LEAD_US]]`): 1 the tick timer without kernel timer-coalescing slop (IOTimerEventSource kIOTimeOptionsSweetCritical -> THREAD_CALL_DELAY_SYS_CRITICAL; ticks came 0.7-1.5 ms after the edge, not 0.5), 2 0946's calibration keeps its tick (17-20 ticks per 8 s were lost: the calibration tick ran after its edge and polled a whole period), 4 notifications carry the measured period; LEAD = tick after the edge (default 500 us). sweetfb_refresh=1|NS / `p vsync refresh [NS]`: IOMFBDisplayRefresh.displayMinRefreshInterval (2^-32 s), CA's heartbeat source without CA_HEARTBEAT_RATE. Boot shortcut (the cmdline has 8 characters left): sweetfbx 0x10 = sweetvsync 3, 0x20 = sweetfb_refresh=1. Analysis: `sweet-trace.py --pace`.
- 0996 direct scanout tribuf, off by default (poke `p scanout tribuf 0|1`, boot sweetfbx 0x40): SwapWait calls with 0x80000000 in scalar 1 (CA's can_update_ poll) answer kIOReturnBusy while the swap is queued/flipping, 0 once it or a later one is latched, dropped or late (100 ms cap); at chain depth 3 the swapper's submits wait for K-1 on screen, not K. Aim: CA keeps K-1's surface until K is latched and takes a 3rd one (needs CA's swap ring of 3; with a ring of 2 CA would wait instead). 5 s line `SCANOUT 5s tribuf` (polls busy/done/stale, gate at K-1, short = depth below the gate's, chain).
- 0997 file server (TCP 2223) socket timeouts: SO_RCVTIMEO/SO_SNDTIMEO 15 s per accepted connection (one server thread with blocking calls: a quiet peer wedged it, later connects sat in the backlog -> "timeout at the header read" on r41a/r46a while ssh/pokes/log worked); `p fs` logs its state (state, request, connections, timeouts, last err, bytes), `p fs reset` shuts the current connection down.
- 0998 IOMFBDisplayRefresh as a variable-refresh display, off by default (boot sweetfbx 0x80 = vrr 3 + sweetfb_refresh=1; poke `p vsync vrr N [STEP_NS]`, then restart backboardd): adds displayMaxRefreshInterval = N periods and displayRefreshStep (default one period), each with a ...MachTime twin, to 0995's displayMinRefreshInterval, the key set the A15 DCP firmware publishes (IOMobileFramebuffer::setDisplayRefreshProperties, iOS 16.7.2). Experiment: UIKit's settle animations run at 30 Hz on the 90 Hz heartbeat. Result (r53a, 2026-10-28): no effect; iOS 16.7 QuartzCore reads only displayMinRefreshInterval (no max/step strings), dead lead.
- 0999 boot sweetfbx 0x100 (default off): 0912's UI-process insertion (SpringBoard, /Applications, containers) also takes /var/local/lib/SweetFPS.dylib (phone/fps, built from phone/haptics/build.sh) when present, colon-joined with SweetHaptics under sweetui=1, plus SWEET_FPS=2 unless set: CADisplayLink requests in [46, 90) fps become 90. Why: QuartzCore maps fps to a heartbeat frame interval n = round(1 / (heartbeat_period * fps)) (0x1882e5038), so UIKit's 60 fps settle animations / scroll deceleration get interval 2 on the 90 Hz heartbeat (45 Hz, 1-or-3-vsync jitter in traces).
  HardwareAccessibleMin/MaxNits, user-accessible-max-nits, digital-dimming-min-nits, brightness-nits(-cap),
  DisplayPanel/ProductLuminanceMin/Mid/Max: 2..500, integer nits; 500 nits <-> level 1023): `sweetnits=0` off.
  `tools/mkadt.py --backlight` (opt-in) also copies D22AP's /backlight node with max-nit-value/LmaxProduct = 500.
- 1011 (was ws-q-bt's 0990) Sandbox op 4 (container path of a pid) answered: `bsd/kern/redfin_sandbox_container.c` returns the target's
  `CFFIXED_USER_HOME=` from the strings exec left at the top of its user stack (as sysctl_procargsx reads them), so
  PlugInKit's `-[PKService checkEnvironment:]` stops killing every .appex ("plugin has invalid sandbox or container:
  error 2 path ?"; 0050/0924 answered ENOENT). No variable = ENOENT as before; `sweetcontainer=0` = always ENOENT.
  Logs `SWEET SANDBOX: container <caller>[pid] asks for <target>[pid]: "<path>"` / `none (no CFFIXED_USER_HOME,
  argslen N)` / `error N`, once per pid, 300 lines at most. Code: HackiOS `kern_minimac.c`
  `minisandbox_container_path` (APSL 2.0) via ios-topaz 0936 (same code, topaz's 0149 weak hook not needed: 0050 calls it
  directly), plus bounds checks and the log. If a pid logs `none`, pkd/RunningBoard did not set CFFIXED_USER_HOME.
- 1012 = Yofuness/ios-flame sunfish 0188 (apple80211 auto-join diagnostics: log caps, 60 s summary, scan elements, set 383 dump).
- 1013 = sunfish 0193: COUNTRY_CODE (51) get/set kept in the kernel, set 383 taken (`sweetwifiacc=0` refuses both, as before).
- 1014 Wi-Fi signal for wifid: wland's FW_RSSI (joined network's signal every 2-10 s) feeds RSSI/CURRENT_NETWORK; NOISE 17
  (-95), RATE 8, PHY_MODE 14, MCS 57 answered; while joined a timer posts the signal (`sweetwifilqm=` / sysctl
  `kern.sweet_wifi_lqm`: 1 LINK_QUALITY 17, 2 RSSI_CHANGED 39, 4 LINK_CHANGED again, 8 snr/noise in LINK_CHANGED;
  default 9; `kern.sweet_wifi_rssi` last told); a link wland made by itself posts SSID/BSSID/LINK (`sweetwifilink=0` off).
  `SWEET WIFI: signal` lines. wland (same branch): re-auth when the AP kicks a stale association (reason 8, the
  first auto-join after a reboot), 2 retries of a failed wifid join, uptime-stamped wland.log.
- 1015 kernel control `com.apple.uart.bluetooth` on the BT bridge (with `btbridge=1`; `btbridge=3` nodes only): iOS 16
  bluetoothd/BlueTool use a PF_SYSTEM control, not a tty (ios-asteroids docs/BT_IOS_TRANSPORT.md). Client bytes -> h2b
  (btd), btd's -> the socket; termios get/set kept and logged (`SWEET BTBR: kernel control ...`). Needs bluetoothd's
  `BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,3000000` (tools-claude/bt-launchd.py --transport). Userland:
  phone/bt/bluetool.c (BlueTool stand-in: every request result 0 after btd is up; `--bluetool`), btd bridge HCI compat
  (vendor answers carry the sub-opcode, Unknown Command / LE Command Disallowed -> success; `--bridge-raw` off).
- 1034 USB Ethernet fast path, off by default: `kern.sweet_usb_txbatch N` (boot `sweetusbtxb=N`, 2..16) = up to N queued TX frames per bulk IN StartTransfer (one unchained TRB each, own contiguous buffer, queue cap 64; needs default sweetusbdma/sweetusbsmmu), `kern.sweet_usb_poll_us US` (boot `sweetusbpollus=US`, 50..1000) = SYS_CRITICAL event polls while TX is in flight or <5 ms after an event; counters `kern.sweet_usb_tx_frames/_tx_xfers/_tx_drops/_rx_frames`. Why: 1.23 MB/s = one frame per 1 ms poll (one single-TRB transfer per frame, next one only after the polled XferComplete).
- 1035 early swap retire on the 0944 GPU-present path, `kern.sweet_fb_early_retire` MASK (default 0 = as before): 1 swap-complete tick at ring acceptance (no tick at the blit fence, no sweetswapdone needed), 2 re-kick a put-off queued present at each vsync tick / when the previous blit's fence is seen (not 1 ms later), 4 SwapWait polls (bit 31) answer Busy while that swap is queued or being presented, 0 once its blit is on the ring (100 ms cap). Safe for GPU-rendered surfaces (one in-order ring; blit IB starts with CACHE_INVALIDATE+WFI); a CPU write into a retired surface could tear. Line `SWEET FB: early retire 5s: retired .. | polls busy .. done .. stale .. | rekick .. | mode ..`. Idea: UniXNU GopFramebuffer sheng early retire.
- 1036 GPU DCVS, `kern.sweet_gpu_dcvs` MASK (default 0 = fixed `sweetgpufreq` level as before): 1 = 16 ms governor on the GMU XO busy counter (Linux a6xx: CX_BUSY_MASK 0xff000000, POWER_COUNTER_SELECT_0 0x20, XOCLK_0; falls back to the CP submit stamps if it never counts), ~50 % target: up at once to 1.1x need, top after >85 % or 50 ms busy, down one level after 4 quiet windows, floor after 80 ms idle; touch / frames-flowing (0970's boost hook) = top for `kern.sweet_gpu_dcvs_boost_ms` (1000); +2 no touch boost, +4 no SwapEnd boost. 0964's cap is the ceiling; votes = HFI perf vote (msg 30, 20 us ack poll). `kern.sweet_gpu_dcvs_floor` MHz (400), `kern.sweet_gpu_mhz` (read). Line `SWEET GPU DCVS 5s: 275:ms 400:ms 500:ms 625:ms ms | switches | busy % | boosts capped | floor cap | src gmu|cp`. Ref: UniXNU sheng GpuPowerProbe gpuDcvsThread.
- 1037 CPU hold while frames flow, `kern.sweet_gov_framehold` PCT (0..100, default 0 = off): after each SwapEnd every domain (silver 0963 rule, gold/prime 0970 policy) stays >= PCT % of its top for 300 ms (first SwapEnd wakes the governor at once); 0964 caps stay the ceiling; needs the governor (flamecpufreq=2). SWEET GOV 5s line gains `hold N`. Ref: UniXNU sheng-cpufreq=4 (>= 60 %).
- 1038 `kern.sweet_gpu_ibmax` N (1..128, default 16 = as before): IBs per v2 submit. ABI-compatible: past 16 the extra NativeAdrenoAbiSubmitBuffer entries follow the fixed record (input size = sizeof(NativeAdrenoAbiSubmit) + (count-16) x 24); old clients unchanged. Ring math: 27 + 4N dwords per submit (539 at 128) of 8192, room check unchanged. Turnip needs a matching change to send more than 16.
- 1039 `kern.sweet_gpu_tlb` (default 0 = full TLBIALL flush after every map/unmap, as before): 1 = unmaps <= 512 pages get one TLBIVA per page (GPU CB + 0x600, ASID 0) + TLBSYNC; maps into all-invalid 4K entries get no invalidate (map_4k checks the old entries; a replaced valid entry or failed map = full flush); 2 unmap part only, 3 map part only. Setting it flushes once and logs CB ACTLR (CPRE prefetch). Line `SWEET GPU: tlb 5s: full .. | va .. pages in .. unmaps | map skipped .. | mode ..`.
- 1040 1000's GPU interrupt fence waits on by default on redfin (validated r64u `p gpuirq 1`: backboardd wakeups ~5000/s -> ~200/s, 113 late of 23k, no fallback, no fps loss); `sweetgpuirq=0` keeps polling, `p gpuirq 0|1` live. Boot klog `SWEET GPU: gpu irq on (default (1040); sweetgpuirq=0 for polling): INTID 332, look every 5000 us`.
- 1041 CA threads on the big cores, `kern.sweet_ui_bigcore` MASK (default 0 = as before): threads named `com.apple.coreanimation.display*` (1) / `com.apple.coreanimation.render-server*` (2) are tagged when named (bsd_setthreadname, TH_SFLAG 0x40000000/0x20000000); choose_processor gives them an idle CPU6/7 (sweetbig order), else preempts the big core running the lowest priority below theirs, else the normal choice (never waits, no binding). Line `SWEET UI: bigcore 5s: runs big/little, ms: display .. | render-server .. | placed idle P preempt Q fallback F | mode M tagged T` (always, also at 0 = baseline). Why: display.primary ran 8/174 frames on an A55 at ~19.7 ms (44 busy ticks).
- 1042 `kern.sweet_vsync_lock` 0|1 (default 0): runtime switch over 0995 (cmdline full): 1 = sweetvsync 7 (no timer slop, calibration keeps its tick, notifications carry the measured INTF1 period) + sweetfb_refresh 1 (IOMFBDisplayRefresh from the measured period, published at once); 0 restores and removes the property if 1042 added it. CA reads it at backboardd start and only without CA_HEARTBEAT_RATE. Line `SWEET FB: vsync lock 5s: period .. ns (.. Hz) published .. | ticks .. | cal ok/late/lost | drift corrected N x avg/max us`.
- 1043 `kern.sweet_gpu_mhz` is a proc reading gpu.perf_level (the sweetgpufreq level from boot, DCVS's level while on): read 0 until kern.sweet_gpu_dcvs was written. Boot klog `level N = M MHz at boot (1043 ...)`.
- 1044 direct scanout smoothness, always on: second 5 s line `SWEET SCANOUT: 5s frames: latches N | vsyncs 1:a 2:b 3:c 4+:d | p50/p95/max ms | jitter us | repeats>=3 | pauses | touch->latch n p50 p95 ms` (latch-to-latch in measured panel periods; touch = SweetHID's newest event before the swap's SwapEnd).
- 1045 USB Ethernet recovery after unplug/replug, `kern.sweet_usb_recover` 1 (default; boot `sweetusbrecover=0` = old behaviour): bus reset / disconnect / SET_CONFIGURATION end the bulk OUT/IN + interrupt IN transfers (End Transfer ForceRM, recorded resource index), clear tx_busy, free the TX queue, LINK_OFF; SET_CONFIGURATION sets the endpoints up from scratch and arms RX (`ECM endpoints configured again (rearms N)`); a repeated SET_INTERFACE alt 1 does the same; stale completions ignored. Watchdog: configured + alt 1 + TX busy + no RX/TX completion 5 s with the host present (link L0 or new events) -> `SWEET USB: stuck, re-arming`, max once/10 s. Counters `kern.sweet_usb_resets/_rearms/_stuck`, also on the 10 s `ue0 rx` line. 1034 defaults now txbatch 16 / poll_us 100 (`sweetusbtxb=0` / `sweetusbpollus=0` or the sysctls = old path). Why: cable to a charger at 1244 s left TX busy and ud.configured true, so the PC's re-enumeration skipped ud_configure; rx froze at 124285 until reboot.
- 1046 boot defaults = the live settings (cmdline full): `sweetscanout` absent = 2, now "slots + direct on by itself at the first slot-surface SwapEnd" (same sweet_scan_set as `p scanout 1`; display off/on as with the poke, 0989); 0 off, 1 on at start, 3 slots only (the old 2); `p scanout 0|1` live and disarms. Klog `SWEET SCANOUT: auto on (1046 ...)`. `kern.sweet_ui_bigcore` default 1 (was 0). No reboot timer: flamereboot default 0 (WDT pets forever), nothing else resets.
- 1048 recorded key-set surfaces keep their row (Yofuness 0251), `kern.sweet_surfkeep` 0|1 / boot `sweetsurfkeep=1`, default 0 = as before: a surface created from <= 9 keys with an explicit IOSurfaceBytesPerRow skips 0159's 64-byte row rounding and 0940's plane rounding (CA's archive decoder copies a blob at the client's 32-byte row and never reads the row back: sheared glyph stripes in Control Center). New surfaces only. Klog `SWEET SURF: 1048 N keys WxH fmt row R kept|rounded (proc)` for the first 32 candidates (also with 0).
- 1049 vsync ticks on CA's display-link grid (Yofuness 0274/0275), `kern.sweet_vsyncphase` US / boot `sweetvsyncphase=US`, default 0 = off (as before; sunfish runs 5000): each tick goes US us before a grid point k * grid - US so app CADisplayLinks fire US after the tick; 0944/0946 panel-edge phasing is skipped while on and vsync notifications carry the grid period. Grid `kern.sweet_vsyncphase_grid` / `sweetvsyncphasegrid`: 0 auto (IOMFBDisplayRefresh period if published by 0995/1042, else 1 s / panel Hz), <= 1000 Hz, else ns. Both live from the next tick. Klog every 5 s while on: `SWEET FB: vsync phase 5s: ... | ticks N | wake late avg/max`.
- 1054 low-bank RAM to the VM, boot-arg `xram` (default 0 = as before: ptov_table, physmap padding/sizing unchanged, extra aperture entries in their own table; fits the 591-byte cmdline): 1 = the System RAM pieces of <0x80000000 0x3bb00000> between the firmware carve-outs (0x84400000-0x86000000, 0x86f00000-0x87400000, 0x9d800000-0x9f400000, 0xa2400000-0xb7e00000, 0xb8654000-0xbbb00000: 459.7 MB), 2 = also pil_npu/pil_video/pil_cdsp (+40 MB). With `sweetvenus` != 0 (1057), or xram=3 / xram=4 (= 1 / 2 for a cmdline with no room for sweetvenus=), 0xa2600000-0xaca00000 and pil_video stay out (295.7 / 330.7 MB). Only when the window is at 0x100000000. Mechanism = HakiOS 1512 (Scion-Project/hackios-xnu-kernel PR 179) + ARM_LARGE_MEMORY physmap sizing. Klog `SWEET XRAM: ... given to the VM` / `SWEET XRAM: xram=T venus|no venus: P pages (M MB) in R ranges ...`.
- 1051 Wi-Fi signal for wifid without 1014, `sweetwifisig=` / `kern.sweet_wifi_sig` bits (default 0 = as before): 0x1 each FW_RSSI from wland posts RSSI_CHANGED (39) in its 0xb4-byte layout (has_rssi@0, rssi@4, rates@0x40; Scion PR #191: the only event wifid scales the status-bar bars from), from wland's write, no kernel timer; 0x2 RSSI/CURRENT_NETWORK give that live signal; 0x4 scan results/CURRENT_NETWORK carry noise -95 + snr (auto-join test); 0x10 a link wland made itself (wlan.conf) is told to wifid as SSID/BSSID/LINK. FW_RSSI always accepted (stored only). `kern.sweet_wifi_live_rssi`; klog `SWEET WIFI 1051:`. Not with 1014.
- wland 2026-11-04 (ws-n-wifi): settings file /var/local/wland.conf (`key=N`, env WLAND_KEY wins); `ht=1` = Yofuness b13a681 HT20+WMM assoc + QoS TX (default 0 = legacy as before); `role=1` = foreground darwin role for wland (default 0); FW_RSSI carries the link rate; install script WLAND_JOB_INTERACTIVE=1 = 9730bab job ProcessType Interactive (default 0).
- 1057 Venus buffer window for /dev/pmem (track V, hardware video decode; Yofuness/ios-flame 0277+0278): boot-arg `sweetvenus` / RW `kern.sweet_venus` 0 (default, unchanged: device windows) | 1 write-combined | 2 cacheable + DC CIVAC, one mapping of 0xa2a00000+160 MB (free low-bank RAM below 4 GB; queues at 0xa2600000 stay on device windows); a change drops the old mapping and device aliases. Guard: pmem I/O in 0xa2600000-0xaca00000 is refused (EPERM, logged once) if that RAM is XNU's (e.g. 1054 xram=1 hands 0xa2400000-0xb7e00000 to the VM): Venus and xram>=1 exclude each other. Klog `SWEET PMEM: /dev/pmem ready (major N), Venus window device (sweetvenus=0)|write-combined|cached+maintenance`. Userland: phone/video (venusctl, venusd, venusc, vtvenus.dylib, vtdecode, avplay), off unless /var/local/venus.on.

SweetMetal2 bridge patches (phone/metal2/bridge/patches, applied in name order onto metal2vulkan 720ef1ce):
- 0120 (was ws-y-rotfb4's bridge 0103) rotated-copy-skip-and-tiles: a rotated (landscape framebuffer) texture's copy in is skipped
  while nothing but its own copy back wrote the plane (`SWEET_M2V_ROT_SKIP=0` off), rotation kernel in 8x8 tiles, one-region copy
  back (`SWEET_M2V_ROT_ROWS=1` per row), `native-metal: rotated 5s` stats. Renumbered in ws-z-merge (ws-u-combo's 0103-0111 keep
  their numbers) and regenerated on top of them: encoder.rs records call 0107's sweet_seed::written and rotated::note_writes; the
  one-region copy back and the stats apply to turned frames only (0104's rotation-0 repack copies back per row, as on ws-u-combo).

## Metal bridge patches ported from UniXNU (phone/metal2/bridge/patches 0130-0137)

Ports of UniXNU AdrenoMetal bridge patches 0007-0016 (bluseliu50, BSD-2-Clause-Patent) onto metal2vulkan 720ef1ce + our
0001-0111; mapping and skipped ones (0008 = our 0101, 0009 = our 0107 SWEET_RPCACHE) in phone/metal2/bridge/UNIXNU-PORT.md.
Each defaults off (= ce44728 behaviour) unless noted; knobs via the process env or /var/local/mtl2-env.
- 0130 (U0007) textures over the render server's two-plane RGB '_RB4' surfaces (widget snapshots), `SWEET_U0007=1`.
- 0131 (U0010) viewports/scissors and push-field bytes held inline: no Vec allocations per draw (always on, same values).
- 0132 (U0011) the encoder keeps the last draw's DrawCommands and refills its vectors, `SWEET_U0011=1`.
- 0133 (U0012) the encoder keeps the last draw's use and argument-sampler lists, `SWEET_U0012=1`; tracker KeyUses inline (always).
- 0134 (U0013) devices refuse rebinds so encoders keep no call log while no-copy buffers live, `SWEET_U0013=1`.
- 0135 (U0014) the encoder's tracker maps hashed by the runtime's KeyHasher instead of SipHash, `SWEET_U0014=1`.
- 0136 (U0015) a two-plane texture's shadow starts as its memory lies (no zero fill at creation), `SWEET_U0015=1`.
- 0137 (U0016) a padded render/write texture aliases only where 16x4 tile stores stay inside its rows, else shadow, `SWEET_U0016=1`.
- g4 enc2 (frontend): SWEET_ENCTHREAD=3 (backboardd) hands recorded encoder commands to 'sweet.encthread' in per-thread batches (64 / encoder end / event / commit / before a flush; UniXNU 6c7b0a0+f1f51bf design, credited), worker sleeps on a condvar; new 5 s "T enc-lat" line (commit->submit, wake->submit, submit call, frame 1st-enc->submit p50/p95, batches/s, cmds/batch, big-core %, wakes/s) for modes 1 and 3; modes 0/1/2 unchanged, default 0.
- bridge 0160 `SWEET_FP16=wide|native` (default off): `wide` keeps half in f32 without the f16 round trip; `native` takes MetalHalfArithmetic/Conversion/Fma so half + - * fma are 16-bit ops. In cache key.
- bridge 0161 `SWEET_M2V_CONTRACT=1` (default off): AIR ops with `contract`/`fast` emitted without NoContraction (incl. kept-apart adds, 16-bit half ops); colour-write rounding keeps it. In cache key.
- bridge 0162 `SWEET_M2V_COLOR_WRITES=plain` (opt-in): as `fetch`, plus non-fetching fragments store 8-bit unorm channels as written (A6xx RB converts; differs from Metal only within 1 code at exact ties); sRGB/10-16-bit/snorm/half keep Metal rounding; 1/0 blend stays off the Vulkan state for those targets. CA texless hot shader 119 -> 24 SPIR-V insts, 36 -> 0 NoContraction. In cache key.
- ovd (frontend): `SWEET_DRAWBOX=N` in /var/local/mtl2-live (backboardd, timeline on; re-armed on each file change, max 20): next N frames log every pass and draw as `DB` lines (pipeline, label, blend, viewport, scissor, position bbox raw/px, vertex bytes). Off = no change.
- g9 (frontend): `SWEET_DEADFILL=1|2` (env or /var/local/mtl2-live, backboardd, default 0): CA's full-target opaque constant-colour fill (`draw.PBGRAXm_A2Xhf`, one rectangle, same transform) is held and dropped when the next draw is such a fill (=1); =2 also turns a Load/DontCare pass's first surviving fill (pixel-space) into a Clear. 5 s `DF 5s` line. `SWEET_LUMA_BOOST=0` (env or live): CASetDebugOption(91 = CA_DISABLE_LUMA_BOOST, 1) in backboardd (option table read from QuartzCore 16.7), `luma:` line; unset = unchanged.
- g10 (frontend): `SWEET_DEADFILL` default 2 (unset; `=0` off, `=1` drops only, live override kept). Mode 2 never cleared in g9: the hold required colour 0 alone and every CA pass has the memoryless scratch as colour 1. Now: only backboardd passes whose colour 0 is an 8-bit unorm single-sample 2D IOSurface (+4: any such texture), no depth/stencil/visibility; fills also need single-sample, no alpha-to-coverage, cull none, fill mode fill; a drop needs the same pipeline object + same other vertex bindings; the Clear needs CA's fixed vertex function, the transform mapping the fill onto clip-space [-1,1]^2 (w 1), no non-memoryless colour 1..7 written; clear colour rounded to the 8-bit code. `DF 5s` adds held_passes/no_clear; DRAWBOX `DF no clear (why ..)`.

- SweetSB (ws-z-ui2/ui3; kernel 1047 cutout ring parked, not merged — branch ws-z-ui, userland, SweetFPS.dylib): status-bar clock clear of the camera hole. iOS 16 Split1125: region "leading" is a UILayoutGuide in _UIStatusBarForegroundView (leading == UIStatusBarMainRegions.leading + 13.33); flag /var/local/sweetsb.x (size 100+DX) + /var/local/sweetsb.xm (102 = mode 2) adds DX to that constraint after -[_UIStatusBar layoutSubviews] (DX 18: clock panel x 82-187 -> 134-239, Home + apps; lock screen unchanged). Mode 1 (item centre shift) hides the clock (region overflow); .m/.t (v1) do nothing. /var/local/sweetsb.d = one-shot hierarchy dump (SpringBoard, /var/tmp/sweetsb.log). No flags = unchanged.
- SweetSB ws-z-ui3 (user-approved final layout, "variant A"): /var/local/sweetsb.y = common DY for the leading+trailing guides, /var/local/sweetsb.r = trailing inset (half-point sizes: 100 + 2·pt). Applied on the known-good phone: .x 123 (+23 pt), .xm 102, .y 107 (+3.5 pt down), .r 108 (+4 pt in). Clock panel x 149-252 y 62-92 (centred on the hole, y 77); Wi-Fi/battery x 897-1015 on the same line. SweetFPS.dylib 52c34ef sha256 1d46e229…
- cell (track T step 1, wland, ws-n-tel): read-only QMI client in wland's own modem QRTR link (phone/wifi/cell.c, client
  port 0x30): DMS model/revision/IDs (IMEI only counted)/operating mode, UIM card status, NAS serving system/system
  info/signal info every 30 s; one `cell:` line in wland.log + /var/local/cell.status (`wland cell`). Off unless
  /var/local/cell.on exists (checked every 10 s, no restart). Never registers, sets a mode, or touches SMS/data/voice.
- cell (ws-n-tel, step 1b): UIM Get Slots Status 0x47 per physical slot (`slots=P1:...|P2:euicc,...`, active slot, protocol,
  apps; ICCID length only); card status walks every logical card and app (`sim=L1:present usim:ready/pin1=...`, `apps=0`
  = eUICC with no enabled profile, the 2026-11-05 phone result); DMS hardware revision 0x2c (`hw=`, model is "0" on redfin).
- cell (ws-n-tel, step 1c, after cfc3df9 wedged boots with a SIM in): every field append clamped (cfc3df9's per-app loop
  could write past f_sim once a reply had many apps); the client starts only 15 s after wland's WLFW bring-up ended
  (cell_allow; 3 min fallback), never sends from the receive path (200 ms gap), drops a step after 3 unanswered tries,
  pauses 5 min after 2 unanswered in a round; Get Slots Status and DMS hw revision only with `slots=1` / `hw=1` in cell.on.
- 2026-11-05 defaults flipped: 1048 sweet_surfkeep 1 (set before SpringBoard draws, so CC/lock-screen glyphs are clean at boot), 1049 sweet_vsyncphase 5000, 1051 sweet_wifi_sig 3. Boot-args sweetsurfkeep=0 / sweetvsyncphase=0 / sweetwifisig=0 give the old behaviour.
- cell (ws-n-tel, step 2): two writes, each only with its word in cell.on and never automatic: `slotswitch=1` UIM Switch
  Slot 0x46 (logical 1 -> physical 1, once per cell.on change, only if P1 holds a card and is inactive; rmtfs images
  copied to *.pre-slot first) and `online=1` / `online=0` DMS Set Operating Mode 0x2e online / low power (2 tries, only
  if the mode differs; *.pre-online copies first). An offline (3) modem refuses (0x35); no reset is ever sent.
- SIM PIN (ws-n-pin, userland: wland + SweetFPS.dylib's SweetPIN.m): `pin=1` in cell.on opens /var/run/wland-cell.sock
  (root:mobile 0660, peers uid 0/501): "S" = PIN1 status of the card's sim/usim app, "V"+digits = one UIM Verify PIN 0x26
  (session 0 if it is the GW primary provisioning app, else non-prov slot N + AID); never repeated, refused with <=1 try
  left, no PUK; log "cell: verify pin -> ok/err, retries left N", the PIN only in a zeroed buffer. SweetPIN (flag
  /var/local/sweetpin.on, size 100+mode: 0 native then fallback, 1 fallback, 2 discovery, 3 native): CoreTelephonyClient
  getSIMStatus/getRemainingPINAttemptCount/unlockPIN/promptForSIMUnlock answered from wland (block signatures checked at
  run time); SpringBoard opens SIMSetupUIService's unlock remote alert (iOS 16 has no SBSIMLockManager; VC name found in
  the app binary or /var/local/sweetpin.vc), else a UIKit SIM PIN alert. Discovery -> /var/tmp/sweetpin.log.
- cell (ws-n-wds, data design X0, read-only): `wds=1` in cell.on adds the WDS server (0x01) to cell.c's rounds: Get Packet
  Service Status 0x22, Get Current Data Bearer Technology 0x44, Get Profile List 0x2a (3GPP), Get Default Profile Number
  0x49 every 30 s; Get Profile Settings 0x2b once per listed profile (again on a cell.on/list/server change). Status line
  gets `| wds=disconnected bearer=... profiles=N default=K apn='...' pdp=ipv4v6`; each profile logged once as `cell: wds
  profile N apn=... pdp=... auth=... name=... roaming=...`. Username/password TLVs never read. No WDS write of any kind.
- cell (ws-n-wds, data design X1, one data call): only with BOTH `datatest=1` and `wds=1` in cell.on (optional `apn=<1-63 of
  A-Za-z0-9.->`, `ipfam=4|6`), once per wland run (once more per cell.on change) when WDS is up, NAS reg=registered ps=1 and the
  SIM app ready: WRITE WDS Start Network 0x20 (0x14 APN, else 0x31 = X0's default profile; 0x19 family; no auth TLVs), Get
  Current Settings 0x2d (mask 0xa314), WRITE Stop Network 0x21 (handle) 20 s later. Logs `cell: WRITE wds start network ...`,
  `cell: wds settings ip=... gw=... dns1=... mtu=... fam=...`; status gets ` x1=idle|started|settings-ok|stopped|err0xE(end
  N/T:R)|...`. Never repeated; no IPA, no data path.
- cell (ws-n-wds, safety switches): `nolte=1` / `csonly=1` in cell.on: once per wland run (once more per cell.on change), as
- cell.c `domain=N` (ws-n-wds): NAS 0x33 TLV 0x18 Service Domain Preference = N instead of csonly's 0 (3 PS attach, 4 PS detach); 2026-11-05: csonly=1 alone did not stop the ps_rm ERR-FATAL (modem still attached PS on GSM and crashed 13 s after registering).
- wland `pas-load ID DIR NAME PA SIZE` (ws-n-wds): generic TrustZone PAS load (init image, mem setup if relocatable, segments, auth+reset) into a carve-out; for the IPA firmware: `wland pas-load 15 /var/local/fw/ipa ipa_fws 0x8bc00000 0x10000` after the IP0 vote (X2).
- wland `ipahw regs|smmu|hwinit|meminit` (ws-n-ipa-hw, phone/wifi/ipa.c): IPA v4.7 AP-side setup, gated on COMP_HW_VERSION 0x40070000; smmu = streams 0x520/0x522 on translation-off banks via radio.c smmu_stream_identity (WLAN path unchanged); hwinit/meminit = downstream redbull register values and ipa_4_7_mem_part canaries; --dry-run.
- wland `smp2p-ipa VALUE` (ws-n-ipa-hw): our outbound SMP2P entry "ipa" (AP IPA clock answer: bit 0 valid, bit 1 on), added after master-kernel; ipaqmi INIT_DRIVER timeout fixed (unsigned wrap made it time out at once). 2026-11-06: modem sent INDICATION_REGISTER to our 0x31 at once; INIT_DRIVER without the SMP2P ipa entry: no answer.
- wland `ipa=1` in cell.on (ws-n-ipa-hw, wland.c "IPA auto"): once per boot (guard /var/local/wland.ipa = boot uuid/boottime), 15 s after the Wi-Fi bring-up, in the running wland: IP0 vote -> pas_load 15 ipa_fws -> ipa_smmu -> ipa_meminit -> SMP2P ipa=3 -> ipaqmi on + one INIT_DRIVER (ipaqmi=2's own send suppressed) -> answer; `ipa: auto step N` lines; first failure ends it, never retried; cell.c holds the online write, PIN verify and X1 (cell_ipa_hold) until it ended. Same order as the 2026-11-06 manual run that made the modem accept INIT_DRIVER and survive SIM unlock.
- cell X1 data port (ws-n-ipa-hw, `datatest=1` + ipa=1's INIT_DRIVER accepted): before Start Network, once per arm, logged WRITEs, chain stops at the first error/no answer (10 s)/missing server: DPM 0x2f Open Port 0x20 (0x11: 1 x {4 embedded, iface 1, rx ep 2 = our AP_MODEM_TX, tx ep 16 = our AP_MODEM_RX}, ModemManager's swap), WDA 0x1a Set Data Format 0x20 (raw IP, QMAP up/down, DL 16 / 0x3800, endpoint {4,1}), WDS Bind Mux Data Port 0xa2 ({4,1}, mux 1, client type 1); status x1=dpm-/wda-/bind-err0xE|-noanswer|-noserver|bound. Without the accepted INIT_DRIVER X1 is unchanged.
- wland ipagsi GSI phase 1 (ws-n-ipa-gsi1 d81cc43: the user's agent's ipagsi + ws-n-ipa-plumb as `ipaphase=0`, ipa_ok gate, hold only while running): 2026-11-06 r5au1 phase 0 ok (utun5, 3 pkts counted); phase 1 failed-3: event rings 2/3 allocated, then 'ch 2 not free: state disabled (0)' (all ch/evt contexts read 0), no channel written; Stop Network unanswered afterwards; 0 modem crashes.
- ipagsi phase 1 rerun (ws-n-ipa-gsi1 45dd92a, state values fixed): 2026-11-06 r5au1 all ch/evt not-alloc, evt 2/3 + ch 2/7 allocated, utun5 up, IPA dump logged; channel START stuck in 'allocated' -> failed-5; modem ERR-FATAL 2.6 s later 'ipa_hal.c:7354:[ipa_ctl]IPA Assert: status == GSI_STATUS_SUCCESS failed'. Phone left in ipaphase=0.
- ipagsi: GSI event-ring/channel context is now programmed AFTER the ALLOCATE command (mainline gsi_channel_setup_one order); it was written before, which left ch start stuck in 'allocated' (failed-5) and the modem asserting ipa_hal.c:7354 (45dd92a crash run). ch start failure now logs ctx0-3, qos, scratch0-3 and GSI ERROR_LOG. Syntax-checked phases 1+2; untested on the phone (a bad GSI result still crashes the modem: reboot to recover).
- ipagsi phase 1 try 3 (ws-n-ipa-gsi1 3ec5221, context after ALLOCATE): 2026-11-06 r5au1 same result: ch 7 START stuck in 'allocated' (ctx/qos/scratch/GSI error log logged) -> failed-5; modem ERR-FATAL 12.6 s later, same ipa_hal.c:7354 assert. Phone left in ipaphase=0.
- ipagsi: GSI_CHANNEL_TYPE_GPI is 0x2 (gsi_reg.h), not 1 (=XHCI): channels and event rings were programmed as XHCI; ch 2 (tx) started anyway, ch 7 (rx) stuck in 'allocated' (3ec5221 run) and the modem asserted. Now 2. Also zero GSI ERROR_LOG before setup as mainline does (the efd6e751 seen at the stuck start was stale). Syntax-checked phases 1+2; untested on the phone.
- ipagsi phase 1 try 4 (ws-n-ipa-gsi1 761b4b6, GPI type + ERROR_LOG clear): 2026-11-06 r5au1 same: ch 7 START stuck in 'allocated', GSI error log 0 -> failed-5; modem ERR-FATAL 12 s later, same ipa_hal.c:7354 assert. Third identical run: next change should split the experiment (ch 2 only / ch 7 only / no start) rather than another full phase 1.
- ipagsi: phase 1 run-time switch gsistart=alloc|tx|rx|rxfirst in cell.on (default: start ch 2 then ch 7 as before); any switch parks in state 'up' after the starts with no ring traffic. ch_start logs 'ch N started', and on a stuck start also the GSI IRQ/status registers and 4 x 500 ms state/error-log polls. Reason: 45dd92a/3ec5221/761b4b6 all stuck at ch 7 START with an empty error log, then modem ERR-FATAL; split the experiment. Syntax-checked phases 1+2; untested on the phone.
- ipagsi gsistart=alloc (ws-n-ipa-gsi1 44976ce): 2026-11-06 r5au1 evt 2/3 + ch 2/7 allocated, NO START, parked up; modem ERR-FATAL 11.7 s later, same ipa_hal.c:7354 assert: the START is not the trigger, our EE0 ring/channel allocation is. Phone left in ipaphase=0.
- ipagsi: gsistart=alloc (44976ce) crashed the modem ~12 s after the allocations with no START issued, so START is not the trigger. New allocation-split switches (phase 1, never START): gsistart=none (read-only control), evonly (event rings 2+3), ch2 / ch7 (one event ring + one channel). Syntax-checked phases 1+2; untested on the phone.
  soon as NAS is up and before the online write or any PIN verify (both held until it is answered, max 10 s): WRITE NAS Set
  System Selection Preference 0x33 with 0x11 mode u16 = 0x000c GSM|UMTS (nolte), 0x18 service domain u32 = 0 CS only
  (csonly), 0x17 change duration u8 = 0 power cycle; one message for both. Then Get 0x34 once: `cell: nas sys-sel mode=0x0c
  (...) domain=cs(0) (wanted ...)`; status gets ` sysel=gsm+umts/cs`. Keeps the modem off LTE/PS until IPA exists (ERR-FATAL
  ps_rm 0xd1 ~45 s after SIM unlock while registering LTE).
- wland ipaqmi.c (ws-n-ipa-qmi, data design D2/D3, protocol only, untested on the phone): `ipaqmi=1` in cell.on announces QMI 0x31 instance 1 at 1:0x13 and answers the modem's 0x20/0x35/0x27/0x23/0x24 with success (0x23 echoes rule_id from byte 168 of each 173-byte filter_spec_ex in TLV 0x15; others only logged); INIT_COMPLETE 0x22 once registered + 0x35 + INIT_DRIVER accepted. INIT_DRIVER 0x21 (ipa_4_7_mem_part offsets, table ipa47_mem[], `ipasmem=` base, no 0x18) only by `wland ipa init-driver` or once with `ipaqmi=2`; `wland ipa show` prints its 154 bytes. Default off: nothing announced.
- ipagsi: channel/event-ring state enums corrected to mainline gsi.h (channel NOT_ALLOCATED=0, ALLOCATED=1, STARTED=2, STOPPED=3, STOP_IN_PROC=4, FLOW_CONTROLLED=5, ERROR=0xf; evt ring 0/1/0xf); fixes phase 1 'ch 2 not free: state disabled (0)' failed-3. Syntax-checked phases 1 and 2; not yet run on the phone.
- ipagsi allocsplit runs (ws-n-ipa-gsi1 b64b047): 2026-11-06 r5au1 gsistart=none, evonly, ch2, ch7 each clean (88-92 s after up); gsistart=alloc (both channels) modem ERR-FATAL 9.2 s after up, same ipa_hal.c:7354 assert: the crash needs both channels (or both rings+channels) allocated. Phone left in ipaphase=0.
- ipagsi: b64b047 allocsplit result: each channel alone clean (watched 88-92 s), ch2+ch7 together crash the modem at +9.2 s. More phase-1 probes (no START): gsistart=evother (ch2 on ev0, ch7 on ev1), drop2, drop7 (alloc all, release one channel+ring). Syntax-checked phases 1+2; untested on the phone.
- ipagsi probe gsistart=onering (ws-n-ipa-gsi1 f8e42d9): 2026-11-06 r5au1 ch2+ch7 both on event ring 2 -> no crash 78 s after up. Crash needs two channels on two event rings (ev3 with ch7 while ch2 on ev2). Phone left in ipaphase=0.
- ipagsi: f8e42d9 onering clean (78 s) => crash needs ev2+ev3+ch2+ch7 together. New probes gsistart=onering3 (4 allocations, one mapping) and swap (ch2 on ev3, ch7 on ev2) to tell 'four contexts' from 'two rings in use'. Syntax-checked phases 1+2; untested on the phone.
- ipagsi probes 5e71b26 (2026-11-07 r5au1): onering3 (ev2+ev3, ch2[2] ch7[2]) clean 92 s; swap (ch2[3] ch7[2]) modem assert ipa_hal.c:7354 at +8.8 s. Crash = two channels on two distinct event rings; count of four allocations alone is not it. Phone left in ipaphase=0.
- ipagsi: 5e71b26 results: onering3 clean, swap crashes => modem asserts when two channels report to two DIFFERENT event rings; one shared ring is clean (onering/onering3). New probe gsistart=onestart (ev2 only, ch2[2]+ch7[2], START tx then rx, parks). Shared-ring data path design in PLAN, not implemented until onestart is clean. Syntax-checked phases 1+2; untested on the phone.
- ipagsi probe 45d6f8c onestart (2026-11-07 r5au1): ev2 shared, ch2+ch7 both START to 'started', then modem assert ipa_hal.c:7354 at +9.3 s. Allocation on one ring is clean, START of both on it is not. Phone left in ipaphase=0.
- ipagsi: 45d6f8c onestart: both channels START ok on one shared ring, modem still asserts +9.3 s. New probes (shared ring): gsistart=onetx|onerx|c2start|c7start (START one channel). Syntax-checked phases 1+2; untested on the phone.
- ipagsi probes ead4c88 (2026-11-07 r5au1): c2start clean 96 s, c7start clean 96 s, onetx (ch2+ch7 on ev2, ch2 STARTed) modem assert +8.9 s. A started channel crashes the modem only with the other channel allocated. One boot had wland auto give up (up returned 1 x5 at stage start, before ipagsi); next boot fine. Phone left in ipaphase=0.
- ipagsi probe a45ac02 ids34 (2026-11-07 r5au1): ch3+ch4 on ev2, both STARTed -> modem assert +8.2 s. Not the channel ids. Phone left in ipaphase=0.
- 2026-11-07 00:11 ipagsi.c: gsistart=ids34 probe (channels 3+4 on ev2, both STARTed); release covers ch 3/4
- 2026-11-07 00:31 ipagsi.c: gsistart=late (45 s delay then onestart shape) and tlvmin (onestart with tlv 2/2); crashes cluster at ~157-158 s uptime
- ipagsi ed88147 late run (2026-11-07 r5au1): channels allocated on ev2 after 45 s but not STARTed: gsistart re-read at setup-5 undoes the late/tlvmin -> onestart mapping, nm[] lacks tlvmin/late (prints (null), OOB). No crash. tlvmin not run. Phone left in ipaphase=0.
- 2026-11-07 00:58 ipagsi.c: fix late/tlvmin (no gsistart re-read at setup-5, nm[] extended)
- ipagsi 5d54ae9 late (2026-11-07 r5au1): ch2+ch7 STARTed on ev2 at 205 s (45 s after the call) -> modem assert ipa_hal.c:7354 at 293 s (+88 s, 106->107 took 3.7 s). Late START delays but does not prevent the crash. tlvmin not run. Phone left in ipaphase=0.
- 2026-11-07 01:23 ipagsi.c: gsistart=tlvbig (c2start with tlv 30). late(fixed) crashed at +88 s: modem's own schedule
- ipagsi tlv probes (2026-11-07 r5au1): tlvmin (5d54ae9, ch2+ch7 tlv 2/2 STARTed) modem assert +9.1 s; tlvbig (2a18c75, ch2 alone tlv 30 STARTed) clean 235 s. Not the TLV budget. Phone left in ipaphase=0.
- 2026-11-07 01:50 ipagsi.c (agent): gsistart=ev4 (event rings 0-3, no channels) and ch3 (ev2 + ch 2/3/7, none started): test the resource-count model.
- ipagsi 72f72f1 ev4 (2026-11-07 r5au1): event rings 0-3, no channels -> modem assert ipa_hal.c:7354 at +93 s (106->107 3.6 s). Event rings alone count. ch3 not run. Phone left in ipaphase=0.
- ipagsi long-watch on 72f72f1 (2026-11-07 r5au1): none, evonly, c2start clean 260+ s; ch3 (ev2 + ch2/3/7 allocated, none started) modem assert +4.4 s (106 at +0.8 s, 106->107 3.6 s). Phone left in ipaphase=0.
- 2026-11-07 02:54 ipagsi.c (agent): gsistart=ev4hi (event rings 4-7, no channels) and ev01 (rings 0,1): count vs. ring-id collision.
- ipagsi e59808d ev4hi (2026-11-07 r5au1): event rings 4-7, no channels -> modem assert +8.9 s. A count, not the ring ids. ev01 not run. Phone left in ipaphase=0.
- 2026-11-07 03:19 ipagsi.c (agent): gsistart=eedump (ch3 shape + read-only dump of the modem EE1 GSI channel/ring states every 250 ms for 15 s).
- ipagsi 64a1491 eedump (2026-11-07 r5au1): EE1 readable. Modem EE1 ch 7 stopped->started at ~163.9 s next to smp2p 106 (163.83), assert 164.45 (+8.2 s); EE1 err 0 throughout.
- ipagsi 64a1491 onering long (2026-11-07 r5au1): ev2 + ch2/ch7 allocated, clean 270 s. Phone left in ipaphase=0.
- 2026-11-07 03:46 ipagsi.c (agent): eedump=1 as a separate cell.on word (any gsistart mode): logs modem EE1 states on change + ring base words.
- ipagsi 9a9576e eedump=1 (2026-11-07 r5au1): none clean (modem EE1 ch3-7 start/stop at +45/+55 s), c2start clean (EE1 idle), ch3 CLEAN 180+ s (EE1 idle; ch3 crashed twice before with the same alloc path). Modem rings at 0x9ba5/0x9d5c, ours 0x106e/0x1070. Phone left in ipaphase=0.
- ipagsi rate runs 9a9576e (2026-11-07 r5au1): ch3 x3 -> modem cycled 2/3, both crashed right after its EE1 ch7 start; c2start x3 -> modem cycled 3/3 (EE1 ch4-7 started, stayed), no crash. No AP-side event before the modem cycles. Phone left in ipaphase=0.
- 2026-11-07 05:24 ipagsi.c (agent): gsistart=c2join (c2start, then add + start ch 7 rx on ev2 once the modem's EE1 ch 7 has been started for 5 s).
- ipagsi 0196fb7 c2join x3 (2026-11-07 r5au1): r1 modem cycled +93 s, join (ch7 rx alloc+start on ev2, ch2+ch7 started) survived 224 s; r2/r3 no modem cycle in 402 s (no join). Scan map: 7/10 modem events inside a Wi-Fi scan. Phone left in ipaphase=0.
- 2026-11-07 06:10 ipagsi.c (agent): gsistart=c2joint (c2join, but joins ch 7 rx 120 s after up even if the modem has not cycled).
- ipagsi fd390dc c2joint (2026-11-07 r5au1): r1/r2 modem cycled at +8 s, late join (ch2+ch7 started on ev2) clean 605 s / 489 s; timeout join not exercised; r3 not run. Phone left in ipaphase=0.
- ipagsi fd390dc c2joint r3 (2026-11-07 r5au1): timeout join at +120 s before any modem cycle -> modem assert 145 s later (smp2p 106 422.9, 107 426.6). Join only after the modem's ch4-7 sequence. Phone left in ipaphase=0.
- 2026-11-07 07:21 ipagsi.c (agent): gsistart=p2late (phase-2 data path in the default build: ev2 shared + ch2 TX, wait for the modem's ch4-7 start (no timeout by default), join ch7 RX, endpoints (read-only), RX buffers, utun; p2tx=1 / p2join= / p2ep= words; chid demux); deferred plain path waits forever.
