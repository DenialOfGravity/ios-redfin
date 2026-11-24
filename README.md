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

## Status (2026-11-24)

Booted RAM-only with `fastboot boot`; everything listed here has been run on the phone. Kernel patches up to `1298`,
native Metal (`HakiosMetal`), Touch ID (`sweetfpcd` + `biometrickitd`), Venus hardware video decode/encode, Night Shift,
Auto-Brightness, True Tone, and kernel-driven MPSS/IPA/QRTR modem bring-up are on `main`.

**Working**

| Area | State |
| --- | --- |
| Boot | Google ABL (unlocked) -> `fastboot boot` of the shim -> XNU, RAM only; nothing flashed. Early-boot sysctls & scratch cleanup via `sweetearly` |
| Graphics | CoreAnimation on native Metal (`HakiosMetal` on Adreno 620): draw culling, uncached-read avoidance, render-target reuse, per-plane textures, crash-trip filter, and `IOSurface` GPU/display use-count tracking (`1258`) |
| Display | 90 Hz panel (`1180–1182` AMP + `SweetCLPC`), multi-layer 2D composition (`1262–1264`, `1294`), brightness slider, **Night Shift** (DSPP PCC `kern.sweet_cm`), **Auto-Brightness** (fast 15 ms wake repost avoiding the 250 ms timeout flash, `1298`), and **True Tone** (`TCS3701` RGB -> CIE XYZ in XNU driving `CoreBrightness`, `1298`) |
| Hardware video | Venus H.264/HEVC decode & VideoToolbox encode (`venusd`, `vtvenc.dylib` with firmware rate control), `/dev/vmmu` SMMU isolation (`1270`), 2 MB carveout guard (`1268`), per-client RPMh bus votes & TCS completion (`1269`, `1277`) |
| Cellular | Calls (2-way audio), SMS RX/TX (incl. BlastDoor sandbox fix `1292`), LTE mobile data (`1265` SIM PIN hold, `1266` bind slots, `ipagsi` QMAP), Visual Voicemail (`vmd`), and XNU MPSS/IPA/QRTR/EFS bring-up (`1295–1297`) |
| Wi-Fi & BT | `wland` Wi-Fi auto-join + RSSI reporting (`1143`) and `btd` / `btd-bridge` Bluetooth |
| GNSS | `sweetlocd` (`pds=1`, MI-keep unlock, fresh XTRA injection) wired into `locationd` and Maps |
| Biometrics | Touch ID unlock/matching via `fpc` + `sweetfpcd` bridged to `biometrickitd` (`1195–1256`) |
| Sensors | Accelerometer, gyroscope, magnetometer/compass, barometer/pressure, proximity, ambient light (lux + RGB), device orientation (`sensd` + `SweetHID`) |
| Camera | Back (IMX363 12 MP stills & 4K/1080p60 `4032x2268` video) and front (IMX355) via `RedwoodAudio.driver` |
| Audio & Haptics | Speakers, microphones, haptics (`RedwoodAudio.driver` + ADSP FastRPC) |
| Charging | USB charging while iOS runs: up to 900 mA on USB 3 ports (`kern.sweet_chg`) |
| Controls & State | `SweetControl.app` (`sweetctl`) for switches/pinned boot settings; declarative phone manifest in `state/redfin.toml` (`tools/state.py`) |

**Partial / in progress**

- **NFC**: the chip probe and most of `nfcd` start-up run against a kernel persona (`1128–1134`); tag reading is incomplete.
- **Compressed (UBWC) display surfaces**: supported via `kern.sweet_ubwc_mdss`, off by default.
- **Camera IFE pixel path**: public `main` uses the baseline capture path; per-mode IFE clock/pixel-path tuning (`1257`) is kept local-only for now.
- **5G NR / signal bars**: LTE voice, SMS, and data work; 5G NR and deep radio/power edge cases remain untested.

**Not working / limitations**

Deep AP sleep / full power management; Bluetooth call audio (HFP); Wi-Fi scan/roam edge cases; SEP/Titan M hardware
keychain & FairPlay DRM (`fairplayd` disabled, Apple Pay / card emulation out of scope).

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
| `xnu/patches/` | 0001-0081 from ios-sweet, 0082-0124 from ios-flame, 0125-0160 from the sunfish series, **0900-1298 for redfin** ([one line per patch](NOTES.md)) |
| `shim/` | The bare-metal shim ABL boots (`shim/build.sh`) |
| `tools/` | Host tools: ADT and payload, `redfin-boot.sh` (build and boot an image), root image and install scripts for WSL, log pulls, camera tables, GPU and Metal debugging |
| `phone/` | Code that runs in iOS: `metal2/` + `turnip/` (HakiosMetal on Adreno), `audio/` (RedwoodAudio.driver), `video/` (`venusd`, `venusctl`, `vtvenc.dylib`), `wifi/` (`wland`), `bt/` (`btd`), `fastrpc/` (`fastrpcd`), `fpc/` (`sweetfpcd`), `gnss/` (`sweetlocd`), `sensors/` (`sensd`), `sweetctl/`, `tools/` (`psx`, `pstack`, `lsreg`, `oslog`, ...) |
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
