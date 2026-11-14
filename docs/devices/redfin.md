# Google Pixel 5 (redfin): status

Made by `tools/docs-gen.py redfin` from [devices/redfin/device.conf](../../devices/redfin/device.conf); edit that file, not this page.

| | |
|---|---|
| title | Google Pixel 5 |
| vendor | Google |
| soc | sm7250 (lito, Snapdragon 765G) |
| ios | iOS 16.7.16 (iPhone X userland) |
| known-good | rbb2b (kernel ws-n-bba2 @ 7100798, 2026-11-14; known-good tracking retired: state/redfin.toml + tools/state.py diff) |
| boot | boot.conf (RAM only: fastboot boot, never flashed) |
| firmware | pull-firmware.sh (output fw-redfin/, git-ignored) |
| soc-args | pull-soc.sh + tools/soc-args.py --check (SoC addresses the code hard-codes vs the phone's DT) |
| plan | docs/redfin/port-plan.md, docs/redfin/qol-plan.md |

## Subsystems

15 works, 4 partial, 2 missing (of 21).

| Subsystem | State | Notes |
|---|---|---|
| boot | works | RAM-only fastboot boot of the shim + XNU payload into iOS on userdata (0900); power and volume keys (vol+ PM8150L GPIO5) |
| display | works | S6E3HC2 cmd-mode DSC panel, 1080x2340 at 90 Hz, direct scanout (0943-0998) |
| touch | works | Samsung sec_ts touch controller (0901) |
| gpu | works | Adreno 620; native Metal (HakiosMetal plug-in via the dispatcher in SweetMetal2.bundle) for backboardd, SweetMetal2/Turnip as the per-process fallback (0904, 0908, 1000) |
| cpu | works | cpufreq with readback and a DVFS governor (0961-0963, 0970) |
| thermal | works | TSENS temperature readout, tsens0/tsens1 (0960; caps 0964) |
| storage | works | HFS+ root on userdata (journaled, 109 GiB), Files > On My iPhone and iPhone Storage (1030) |
| backlight | works | panel brightness in nits for CoreBrightness (0906, 0973) |
| torch | works | camera flash LED as the torch, Control Center button (0918, 0933, 1062) |
| haptics | works | TI DRV2624 on QUP I2C SE3 (0907, 0909, 0912) |
| audio | works | speaker (two CS35L41 amps) and microphone through the ADSP (0915-0917, 0952-0958) |
| camera | works | Camera.app capture (0919-0934); needs fw/camera-<sensor>.tab made by tools/redfin-camtab.py from the vendor module file |
| usb | works | USB networking (CDC Ethernet) and the live kernel log |
| wifi | works | WCN3990 via the modem DSP, wland starts it at boot; Settings > Wi-Fi scans and joins (1012 + 1013) |
| bluetooth | works | WCN3998 HCI over UART (btd 43320fd: select()-based bridge, IBS wake ack, vayu HCI shim); scan, pairing and headphone (A2DP) audio work (1016 Skywalk privilege) |
| battery | partial | charge level is an estimate (SDAM-seeded coulomb count + voltage, 0966); not calibrated |
| charging | partial | charges while iOS runs, slowly on the PC's USB cable (SMB5 policy 0967/0968); full-rate charging in fastboot |
| sensors | partial | accelerometer, gyroscope, orientation via the ADSP (fastrpcd, sensd); auto-rotate works (1031; landscape 3-5 s); proximity on demand (r5au1s) |
| cellular | partial | CommCenter + kernel QMI router (1067-1095): native SIM PIN, registration, SMS, voice calls with two-way audio; mobile data moving from wland to CommCenter + kernel IPA (1075/1076/1080), not passing data on rbb2b; no home/lock-screen bars, no speakerphone button, no DTMF uplink |
| nfc | missing | not started |
| gps | missing | not started |
