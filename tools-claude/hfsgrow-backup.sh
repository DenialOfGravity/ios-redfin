#!/bin/bash
# 2026-10-28: raw backup of the iOS root (first 16 GiB of userdata) from TWRP before growing it,
# then fsck of the copy, then back to fastboot. Read-only on the phone. Run in WSL (via build-diag).
set -uo pipefail
P=/mnt/d/ClaudeProjects/iOS_Pixel5
FB=/mnt/c/platform-tools/fastboot.exe; ADB=/mnt/c/platform-tools/adb.exe
D=/mnt/d/redfin-backup; OUT=$D/iosroot-pregrow-20261028.img
PART=/dev/block/sda19; SIZE=17179869184
say() { echo "[$(date +%T)] $*"; }
sh_() { timeout 600 "$ADB" -d shell "$@" </dev/null | tr -d '\r'; }
mkdir -p "$D" || { say "cannot create $D"; exit 1; }
say "start; phone state:"; timeout 10 "$FB" devices
if timeout 10 "$FB" devices | grep -q fastboot; then
	say "fastboot boot TWRP (RAM only)"
	timeout 120 "$FB" boot "$(wslpath -w "$P/twrp-3.7.0_11-0-redfin.img")" || { say "TWRP boot failed"; exit 1; }
fi
for i in $(seq 90); do [ "$(timeout 5 "$ADB" -d get-state 2>/dev/null | tr -d '\r')" = recovery ] && break; sleep 2; done
[ "$(timeout 5 "$ADB" -d get-state 2>/dev/null | tr -d '\r')" = recovery ] || { say "no TWRP adb after 180 s"; exit 1; }
sleep 5
say "TWRP up: $(sh_ getprop ro.product.device) $(sh_ id | cut -c1-20)"
[ "$(sh_ readlink -f /dev/block/by-name/userdata)" = $PART ] || { say "userdata is not $PART: stop"; exit 1; }
sh_ 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; grep -c " /data " /proc/mounts'
say "userdata bytes: $(sh_ blockdev --getsize64 $PART); signature: $(sh_ "dd if=$PART bs=1024 skip=1 count=1 2>/dev/null | od -An -c -N2" | tr -d ' ')"
say "copying 16 GiB -> $OUT"
t0=$(date +%s)
timeout 3000 "$ADB" -d exec-out "dd if=$PART bs=4194304 count=4096 2>/dev/null" > "$OUT.part"
got=$(stat -c %s "$OUT.part"); say "got $got bytes in $(( $(date +%s) - t0 )) s"
[ "$got" = $SIZE ] || { say "SIZE MISMATCH: stop"; exit 1; }
mv -f "$OUT.part" "$OUT"
say "per-GiB SHA-256, phone vs copy"
bad=0; : > "$OUT.gib.sha256"
for i in $(seq 0 15); do
	a=$(sh_ "dd if=$PART bs=4194304 skip=$((i * 256)) count=256 2>/dev/null | sha256sum" | cut -d' ' -f1)
	b=$(dd if="$OUT" bs=4194304 skip=$((i * 256)) count=256 status=none | sha256sum | cut -d' ' -f1)
	echo "$i $b" >> "$OUT.gib.sha256"
	if [ "$a" = "$b" ]; then say "  GiB $i ok"; else say "  GiB $i MISMATCH phone $a copy $b"; bad=1; fi
done
[ $bad = 0 ] && say "BACKUP VERIFIED" || { say "BACKUP NOT VERIFIED"; exit 1; }
say "back to fastboot"; timeout 20 "$ADB" -d reboot bootloader
say "fsck.hfsplus -fn on the copy (read-only; the journal is not replayed, so it may report journal-related noise)"
timeout 1200 fsck.hfsplus -fn "$OUT" 2>&1 | grep -v 'Illegal name' | tail -25
say "DONE"
