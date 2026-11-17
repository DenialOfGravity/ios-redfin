Phone state and installs (ws-tools-state, 2026-11-14) - replacement text for the install parts of README-lab.txt
==========================================================================================================

The phone's configuration is state/redfin.toml in git (tools/state.py, state/README.md). Install through
`state apply` (phone agent request `state apply ...` after the switch; until then `python3 tools/state.py apply
--via agent ...` in WSL), not by hand.

NEVER WRITE A FILE ON THE PHONE IN PLACE
  `files put LOCAL /var/local/bin/X` over an existing X writes into the same vnode. If X is a mapped Mach-O (binary,
  dylib, bundle executable) the vnode keeps the OLD file's code-signature blob: new pages fail the hash check, the
  kernel logs "CODE SIGNING: cs_invalid_page ... allowing (remove VALID)", every process loading the file loses
  CS_VALID and tccd refuses its entitled requests (the 2026-11-14 SpringBoard crash loop).
  Always:   files put LOCAL /path/X.st-new MODE      then   phone toybox mv -f /path/X.st-new /path/X
  This holds for plists and _CodeSignature files, rollbacks and attic restores too. state.py does it for every
  install and fails the apply when the live kernel log (logs/live-*.txt) shows a new cs_invalid_page line.
  The old README-lab recipe "files put .../sweettrace /var/local/bin/sweettrace 755" is unsafe: do not use it.

New on the phone (2026-11-14, first state.py apply, new paths only)
  /var/local/bin/toybox-full     toybox 0.8.11 with grep/egrep/fgrep/tr/sort/killall/which/seq/timeout/sha*sum
  /var/local/tools/bin/<applet>  symlinks to toybox-full (the old /var/local/bin/tr etc. point at the stock toybox,
                                 which lacks them)
  /var/local/tools/bin/sctl      sctl with `-a [PREFIX]` (lists every kern.sweet_* value)
  /var/local/bin/sweetstate      dump helper: all sysctls, process start times, files mapped by a process
  /var/local/bin/sweet-early.sh  boot sysctls + scratch wipe; runs once the launchd job org.iosonpixel.sweetearly is in
                                 launchd.plist (held for the next boot window)
