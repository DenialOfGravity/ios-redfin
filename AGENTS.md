# Working on ios-flame

- Read `README.md`, then `NOTES.md` (hardware facts, ABL boot image rules, test loop) and the top of
  `HANDOFF.md` ("Start here": the newest state, newest first). ios-sweet's history is in `docs/ios-sweet/`.
- Record each milestone in `HANDOFF.md` under "Start here", with the kernel number, the boot args, exact
  results and what is still open. Keep observations apart from guesses. A new kernel patch is one line in
  `NOTES.md` ("Flame patches") too.
- Kernel changes: edit `work/dxb/xnu` (a git repo, HEAD = baseline + applied patches), commit, and save the diff
  as the next `xnu/patches/NNNN-flame-*.patch`. `tools/build-xnu.sh` builds locally in minutes.
- Device bring-up uses temporary `fastboot boot` images. Never flash ABL, XBL, TZ, modem or other firmware
  partitions, never relock the bootloader on modified software, and keep the partition backups intact.
  Confirm each destructive step with the user before running it. `RECOVERY.md` restores stock Android.
- Keep out of git: partition dumps, device trees and logs from a phone (serials, IMEI, network names),
  Apple's files (IPSW parts, the root image, shared cache, kernelcache, anything translated from Apple's
  shaders), Google and Qualcomm images and firmware, and Wi-Fi credentials (`/var/local/wlan.conf` lives on
  the phone only).

## Phone state (redfin): `state/redfin.toml` is the source of truth

- The phone's configuration lives in git: `state/redfin.toml` (files + build provenance + sha256, flag/env file
  contents, boot sysctls, expected processes, retired paths). See `state/README.md`.
- To change the phone: edit the manifest in your worktree branch, push, then the phone agent request
  `state apply --manifest git:origin/BRANCH:state/redfin.toml` (dry run first with `--dry-run`). It installs only
  what drifted, restarts only the processes named, re-dumps and verifies. It never reboots or flashes.
- No ad-hoc `files put`, `phone cp/mv`, hand-edited flags, env or conf files on the phone. The one exception is
  `/var/local/scratch/` (ignored by `state diff`, wiped at boot).
- Experiments flip values with `state set KEY VALUE --temp` (recorded in `PF/state/temp-overrides.json`), then
  `state revert KEY` or `state promote KEY --manifest FILE` + commit.
- Every status doc starts with the output line of `state diff` or `state snapshot` (expected: `no drift`) instead
  of hand-written hashes; `KNOWN-GOOD.txt` is no longer written by hand (`state summary` / snapshots replace it).
- Never put a SIM PIN/PUK, ICCID, IMSI, IMEI, Wi-Fi password or `abp-policy.conf` content into the manifest,
  a status doc or a log in git. The dump replaces such values with `@PHONE@` on the phone.
