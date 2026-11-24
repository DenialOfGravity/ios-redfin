# redfin: iOS services switched off on the phone (revisit when the feature they need is ported)

Each entry: what, why, how it was switched off, how to undo. The phone-side backups are listed in the PC's KNOWN-GOOD.txt.

| Service | Off since | Revisit with | Why | How | Undo |
|---|---|---|---|---|---|
| **vmd** (Visual Voicemail, `com.apple.voicemail.vmd`, /System/Library/PrivateFrameworks/VisualVoicemail.framework/vmd) | 2026-10-29 (r63a); **restored 2026-11-24 (raud14)** | **telephony** (CommCenter / CoreTelephony, modem) — **done** | Previously no CommCenter: vmd retried `com.apple.commcenter.coretelephony.xpc` in a tight loop (Code=4099 "connection invalidated"), ~1 full core, footprint 219 MB -> 912 MB in ~20 min, ~100k os_log lines. Restored on raud14 (`launchd.plist` sha256 `9a22e4d834`): connects cleanly to CommCenter, 0.19s CPU, 9 MB RSS / 2 MB footprint, 0 Code=4099 errors, idle after init. | Job dict from `/System/Library/xpc/com.apple.voicemail.vmd.job.plist.disabled` merged back into `/System/Library/xpc/launchd.plist` LaunchDaemons (`state/files/vmd/launchd.plist-9a22e4d834`). | Restored 2026-11-24. |
| fairplayd (`com.apple.fairplayd.H2`) | earlier (quickwins) | DRM / App Store | relaunch loop + locationd deadlock (tools-claude/quickwins-launchd.py --no-fairplayd) | job removed | launchd.plist.before-quickwins |
| AegirPoster (lock-screen poster extension) | 2026-10-28 | wallpaper / poster work | idle 70 -> 52 °C | .appex renamed | logs/aegir-disable.txt |

When telephony work starts: restore vmd first and check it no longer loops once CommCenter answers.
