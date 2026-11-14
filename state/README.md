# state/: the phone's configuration, owned by git

`state/redfin.toml` says what the Pixel 5 must hold: the boot image/kernel/cmdline, every installed file and bundle
(with its sha256 and the build it came from), every flag/env file with its content, the boot sysctls, the
processes that must be up, and the paths that must be gone (`retired`). `tools/state.py` compares it with the
phone and fixes the difference. The phone is not the source of truth any more; this file is.

## Use (phone agent requests; or `python3 tools/state.py ...` in WSL, which goes through the agent's inbox)

| request | what it does |
| --- | --- |
| `state diff` | one dump of the phone (`phone/tools/sweetstate.sh` over ssh) against the manifest. Last line `state diff: no drift (...)` or `state diff: DRIFT n item(s): ...`. `retired` and `temp` lines are information, not drift |
| `state apply --dry-run` / `state apply` | installs exactly the drifted files/bundles/flags from their `source`, sets drifted sysctls, restarts only the processes those entries name, re-dumps and verifies. Never reboots, never flashes, refuses paths outside `[managed].roots` and anything under `[managed].firmware`. Overwritten files of unknown provenance are first copied to `PF/state/attic/` |
| `state apply --retire` | also moves the retired paths that are still there into `/var/local/scratch/retired-DATE/` (ask the user first: on 2026-11-14 that was 622 paths, about 1.2 GB of old copies) |
| `state set KEY VALUE --temp` | an experiment: `kern.sweet_x 3` (sctl now) or `/var/local/cell.on:p2keep 3` (rewrites that token/line, restart the reader yourself). Recorded in `PF/state/temp-overrides.json`; `diff` lists it as `temp`, and as `temp-lost` after a reboot |
| `state revert KEY` / `state revert all` | back to the manifest value |
| `state promote KEY --manifest ~/wt/BR/state/redfin.toml` | writes the temp value into a manifest file; commit + push it |
| `state snapshot [LABEL]` | dated dump + diff + summary in `PF/state/snapshots/`; prints the line for the status doc |
| `state summary` | the manifest in a few lines (what KNOWN-GOOD.txt used to say) |
| `state adopt --out FILE` | writes the phone's current state as a manifest (bootstrap or a deliberate re-baseline); unknown provenance shows as `source = "unknown"` |

Which manifest: `PF/state/manifest-ref.txt` (one line, e.g. `git:origin/main:state/redfin.toml`; state.py fetches
that branch first), or `--manifest PATH|git:REF:PATH` on the command line.

## Manifest sections

- `[boot]` image, base, kernel_dir/branch/sha, `kernel_uuid` (= `kern.uuid` on the phone; `diff` checks it and
  `kern.bootargs`), cmdline_file. Not applied: booting is `cycle`/`boot`.
- `[managed]` `roots` (what state.py may write), `ignore` (never listed: scratch, tmp, caches, live logs dirs,
  modem EFS), `volatile` (listed but never compared: logs, status files), `stock` (Apple's own), `firmware`
  (read-only for apply), `secret` (hash only, content never leaves the phone), `nohash` (dump speed).
- `[sysctl]` NAME = value: the boot set. state.py writes `/var/local/sweet-sysctl.conf` from it; the phone applies
  that file at boot with `/var/local/bin/sweet-sysctl.sh`, started from `/var/local/bin/sensd-probe.sh` (launchd job
  `org.iosonpixel.sensdprobe` in `/System/Library/xpc/launchd.plist`). Neither script is in git yet (they live in
  `PF/agent/stage/`); the manifest pins their sha256.
- `[sysctl_expect]` running values that are checked but not written to the conf (defaults, boot-arg results).
- `[[process]]` name (+ `restart` command; default `psx -k NAME`, launchd restarts the job).
- `[[file]]`, `[[bundle]]` path, `source` (PF-relative: `builds/<target>/<ref>-<sha10>/...`, `agent/stage/...`,
  `r23-user/...`; or `stock`, `moved-aside`, `firmware-copy`, `unknown`), `sha256` / `files`, `mode`, `owner`,
  `restart`, plus `target`/`ref`/`sha` when the source is a build folder.
- `[[link]]`, `[[flag]]` (content; `redacted = n` when PIN-like values were replaced by `@PHONE@` on the phone:
  apply fills them back in on the phone, the PC never sees them; `secret = true` = sha256 only).
- `[[retired]]` path + reason.

## Rules

- Change the phone by changing this file (in a worktree branch), pushing, and `state apply`. No hand `files put`,
  no hand-edited flags or conf files, except under `/var/local/scratch/` (ignored by diff; to be wiped at boot once
  `sensd-probe.sh` gets its `rm -rf /var/local/scratch/*` line through the manifest).
- Experiments: `state set ... --temp`, then `revert` or `promote`.
- Status docs start with the `state diff` (or `state snapshot`) line instead of hand-written hashes.
- Never in this file: SIM PIN/PUK, ICCID, IMSI, IMEI, Wi-Fi passwords, abp-policy.conf content. The dump redacts
  them on the phone; `secret` files are pinned by hash only.
