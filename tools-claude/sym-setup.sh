#!/bin/bash
# 2026-10-28: symbolication kit in WSL: ipsw (blacktop) + the phone's iOS 16.7 dyld shared cache, from the IPSW dmgs.
set -uo pipefail
say() { echo "[$(date +%T)] $*"; }
BIN=$HOME/redfin-build/toolchain/bin; DSC=$HOME/redfin-work/dsc; W=$HOME/redfin-work
mkdir -p "$BIN" "$DSC"
if [ ! -x "$BIN/ipsw" ]; then
	url=$(curl -sfL https://api.github.com/repos/blacktop/ipsw/releases/latest | python3 -c "import json,sys; print(next(a['browser_download_url'] for a in json.load(sys.stdin)['assets'] if a['name'].startswith('ipsw_') and a['name'].endswith('_linux_x86_64.tar.gz')))")
	say "ipsw: $url"; t=$(mktemp -d); curl -sfL "$url" | tar xz -C "$t" && cp "$t"/ipsw "$BIN/ipsw" 2>/dev/null || cp $(find "$t" -name ipsw -type f | head -1) "$BIN/ipsw"; chmod +x "$BIN/ipsw"; rm -rf "$t"
fi
"$BIN/ipsw" version 2>&1 | head -2
if [ ! -f "$DSC/dyld_shared_cache_arm64" ]; then
	for d in $W/ipsw/087-86593-047.dmg $W/ipsw/087-86698-044.dmg; do
		m=$(mktemp -d); say "apfs-fuse $d"
		apfs-fuse -o allow_other "$d" "$m" 2>&1 || apfs-fuse "$d" "$m" 2>&1
		c=$(find "$m" -maxdepth 6 -type d -name com.apple.dyld 2>/dev/null | head -1)
		say "  dyld dir: ${c:-none}"
		if [ -n "$c" ] && ls "$c"/dyld_shared_cache_arm64* >/dev/null 2>&1; then cp -v "$c"/dyld_shared_cache_arm64* "$DSC/" | tail -2; fi
		fusermount -u "$m" 2>/dev/null || fusermount3 -u "$m" 2>/dev/null; rmdir "$m"
		[ -f "$DSC/dyld_shared_cache_arm64" ] && break
	done
fi
ls -la "$DSC" | head -40; du -sh "$DSC"
say DONE
