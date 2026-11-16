# ios-redfin

**Apple's iOS 16 running on a Google Pixel 5.**

A port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) to the Pixel 5 (`redfin`, Qualcomm SM7250 /
Snapdragon 765G, Adreno 620). This repository is a fork of [ios-flame](https://github.com/NWylynko/ios-flame) (the
Pixel 4 port), by way of its [sunfish branch](https://github.com/Yofuness/ios-flame/tree/sunfish). Apple's
open-source XNU kernel (xnu-8796, the iOS 16 generation), with ios-sweet's patches and the redfin ones on top, runs
the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**. The phone is booted with `fastboot boot` from
Google's own bootloader. Nothing is flashed to the boot chain; the iOS root lives in the `userdata` partition.

> Experimental bring-up, not a usable phone OS. It **erases Android's user data**, Apple's security model is mostly
> switched off (permissive AMFI and sandbox, stub keystore), and many parts are missing. Do not put anything
> personal on it.

## Status (2026-11-16)

Booted RAM-only with `fastboot boot`; everything listed here has been run on the phone. Kernel patches up to 1127,
hakios-metal int14, sweetlocd (GNSS) and SweetControl are merged into `main`.

**Working**

| Area | State |
| --- | --- |
| Boot | Google ABL (unlocked) -> `fastboot boot` of the shim -> XNU, RAM only; nothing flashed |
| Graphics | CoreAnimation on native Metal (hakios-metal on the Adreno 620): draw culling (GPU time about 1/3 lower), no CPU reads of uncached GPU memory, render-target reuse. App Library blur fixed |
| Frame rate | While moving, after a restart (90 Hz panel): home pages ~47-53 fps, home ~58-62, Settings ~72-75, Control Centre ~52-56 |
| Cellular | Calls and SMS. Mobile data works but still needs final integration |
| Wi-Fi | Joins and stays connected (userland `wland`) |
| Sensors | Accelerometer |
| Display | Brightness slider |
| Charging | USB charging while iOS runs: 900 mA on USB 3 ports with charging control (`kern.sweet_chg`) |
| Settings | SweetControl app for the port's switches, with pinned settings |

Storage, touch, buttons, sound, microphones, haptics and the rest of the r57a (2026-10-29) baseline carry over.

**Partial / in progress**

- NFC: the chip probe works; iOS's `nfcd` gets through most of its start-up against a kernel persona. No tag reading yet.
- GNSS: the `sweetlocd` daemon reaches the modem's location service; the engine-lock unlock is under test (switch, off
  by default). Bridge to locationd / Maps next.
- Compressed (UBWC) display surfaces: work without errors but are slower, so off by default.
- Night Shift: the hardware colour-matrix path is built (off by default) but does not work yet.
- Hardware video decode/encode: power-rail fix in progress.
- Signal bars: hit or miss.

**Not working / not started**

Proximity sensor during calls; auto-brightness; gyroscope and compass (likely not working); True Tone (only possible
if colour channels reach iOS); fingerprint (needs emulation plus Titan M work); camera (semi-working under the old
Vulkan path, to be verified on native Metal); Wi-Fi auto-join and a scan stall; Bluetooth call audio; sleep / power
management; 5G (untested). Apple Pay / card emulation is out of scope.

## How it boots

```
ABL (Google's bootloader, unlocked)
  -> fastboot boot redfin-rNN.img       LZ4 kernel + AVB hash footer, or ABL refuses it
       shim (shim/): acts as iBoot. Logs to UFS, sets the PON reason, builds the boot args
       and jumps to XNU with an Apple device tree (tools/mkadt.py)
  -> XNU (xnu/patches): every driver built in: UFS, display, GPU, touch, keys, USB, RTC, RPMh,
       audio DSP (APR, FastRPC, QRTR), speaker amps, mics, haptics, cameras, flash, thermal,
       CPU frequency, battery, sensor HID services, Bluetooth UART bridge, en0 for wifid
  -> launchd and the iPhone X userland from userdata
       + phone/: SweetMetal2.bundle (Metal), RedwoodAudio.driver (sound, haptics, camera),
         wland (Wi-Fi), btd + bluetool (Bluetooth), fastrpcd + sensd (DSP files, sensors), debug tools
```

## Repository and branches

`main` carries the redfin work that has run on the phone (kernel, hakios-metal, userland); `redfin-int` follows it.

- [`known-good-r57a-kernel`](https://github.com/DenialOfGravity/ios-redfin/tree/known-good-r57a-kernel): the
  older r57a known-good kernel (258 patches), kept as a fallback.
- `ws-*` branches: workstreams (GPU, display pacing, Wi-Fi/Bluetooth, storage, ...), merged into integration
  branches once tested on the phone. `builds-*` branches hold CI build outputs.

| Path | What |
| --- | --- |
| `xnu/patches/` | 0001-0081 from ios-sweet, 0082-0124 from ios-flame, 0125-0160 from the sunfish series, **0900-1127 for redfin** ([one line per patch](NOTES.md)) |
| `shim/` | The bare-metal shim ABL boots (`shim/build.sh`) |
| `tools/` | Host tools: ADT and payload, `redfin-boot.sh` (build and boot an image), root image and install scripts for WSL, log pulls, camera tables, GPU and Metal debugging |
| `phone/` | Code that runs in iOS: `metal2/` + `turnip/` (Metal on Turnip), `audio/` (RedwoodAudio.driver), `wifi/` (wland), `bt/` (btd, bluetool), `fastrpc/` (fastrpcd), `sensors/`, `tools/` (psx, pstack, lsreg, oslog, ...) |
| `.github/workflows/` | CI builds of the kernel, shim, userland and Metal bundle |
| [`docs/redfin/`](https://github.com/DenialOfGravity/ios-redfin/tree/known-good-r57a-kernel/docs/redfin) | Port plan and hardware map, quality-of-life plan (power, audio, sensors, Bluetooth), direct-scanout plan |
| [NOTES.md](NOTES.md), [HANDOFF.md](HANDOFF.md) | Hardware facts, Google ABL boot image rules, patch notes; the engineering log |
| [RECOVERY.md](RECOVERY.md), [DEVICE.md](DEVICE.md) | Back to stock Android; the test phone and host setup |
| `docs/ios-sweet/` | ios-sweet's README and notes (the history of patches 0001-0081) |

## Not in git: what you need to get yourself

Scripts expect these at the paths shown (all gitignored). None of them can be shared: they are Apple's or
Google's/Qualcomm's files, or data from your own phone.

- **The phone**: a Pixel 5 (`redfin`) with the bootloader unlocked, on the final Android 14 build
  `UP1A.231105.001.B2`. Keep Google's factory image ([RECOVERY.md](RECOVERY.md)). Back up the per-device partitions
  first (persist, modemst1/2, fsg, fsc, ...; `tools/redfin-backup.ps1`). They hold your IMEI and calibration data,
  so keep them private.
- **Boot image tools**: AOSP `mkbootimg` and `avbtool.py`, and the stock DTB from the factory image's `boot.img`.
- **TWRP** for redfin (3.7.0_11-0). It writes the root image and reads logs back.
- **The iOS root**: the iPhone X iOS 16.7.16 (20H392) IPSW. `tools/remotezip.py` fetches single files from Apple's
  CDN. Build and write the HFS+ image with `tools/redfin-mkroot-wsl.sh` and `tools/redfin-install-root-wsl.sh`, then
  `tools/sweet-root-tweaks.py`.
- **Firmware** from the factory image's `vendor.img`:
  - GPU: `a650_gmu.bin`, `a650_sqe.fw` and `a620_zap.mdt`/`.bNN` (put into the device tree by `mkpayload.py --gpufw`).
  - Audio DSP: `adsp.*` in `/private/var/local/fw` on the phone; the sensor files for `fastrpcd`
    (`tools/redfin-install-hexagon-wsl.sh`).
  - Wi-Fi and modem firmware plus your own modemst/fsg images (`tools/redfin-install-wifi-wsl.sh`).
  - Bluetooth: `crbtfw32.tlv`, `crnv32.bin` in `/private/var/local/fw`.
  - Cameras: register tables made from the vendor camera module files with `tools/redfin-camtab.py`.
- **Toolchains**: the CI workflows (macOS runners with Xcode), or a local build in WSL with UniXNU's Linux toolchain
  (no Mac needed; `tools-localbuild` branch). Mesa and metal2vulkan checkouts for the Metal bundle (pins in
  `tools/build-turnip-ios.sh` and `tools/build-m2v-bridge.sh`).

## Run it

```sh
python3 tools/mkadt.py work/boot/redfin-hi.adt --highmem     # Apple device tree for redfin
tools/redfin-boot.sh r1 KERNEL [EXTRA BOOT-ARGS...]          # build the boot image and fastboot boot it (RAM only)
tools/redfin-usb-wsl.sh                                      # attach the phone's USB Ethernet to WSL
tools/redfin-pull-logs-wsl.sh                                # read the kernel log back
```

`tools/redfin-boot.sh` packs the kernel, the ADT and the boot args into a shim boot image, waits for fastboot and
boots it. `PRESET=camera` and the `GPU=`, `FLIP=` and `GPUFREQ=` variables pick the feature set; the boot args of
the current known-good image are listed in the redfin NOTES.md and HANDOFF.md on the branch above.

## Credits

Built on ios-sweet by dipa-studio and ios-flame (and its sunfish branch), with sound, haptics and camera code from
HackiOS, Wi-Fi from ios-topaz and HackiOS, Metal from UniXNU, metal2vulkan and Mesa, `fastrpcd` from hexagonrpcd,
and Bluetooth transport findings from ios-asteroids. Licences and details in [CREDITS.md](CREDITS.md).
