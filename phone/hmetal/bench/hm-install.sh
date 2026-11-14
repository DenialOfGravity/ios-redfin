#!/bin/sh
# hm-install.sh SRC TAG: install the Metal dispatcher + hakios-metal on the phone WITHOUT overwriting files in place.
#
#   SRC  a folder on the phone holding this build's bench/ output: SweetMetal2-shim.bundle/, HakiosMetal.bundle/,
#        hm-env.default, hm-install.sh (copy the build's bench/ folder there with `files put` first; dSYMs and seed/ need not go)
#  env  (hm-int4) the phone's /var/local/hmetal/env is never replaced: the template goes to env.default and the template
#        lines the env lacks or sets differently are printed for a hand merge. HMI_BENCH=1 also installs hmbench + metallib.
#   TAG  names the backups: FILE.pre-TAG (made once; an existing one is kept)
#
# Why: a Mach-O written over in place while processes have it mapped keeps the kernel's signature blob of the OLD file
# on its vnode. New pages then fail their hashes ("CODE SIGNING: cs_invalid_page ... allowing (remove VALID)"), the
# process loses CS_VALID, tccd stops trusting its entitlements and its first TCC request aborts it (SpringBoard,
# kTCCServiceMotion, 2026-11-14). Writing NAME.new and renaming it over NAME gives a new vnode with its own signature.
# Restoring a backup goes the same way: hm-install.sh restore TAG.
#
# Afterwards: restart backboardd once (psx -k backboardd); mediaserverd too if it should load the new driver.
set -eu
T=/var/local/bin/toybox
[ -x "$T" ] || T=
DST_SHIM=/System/Library/Extensions/SweetMetal2.bundle
DST_HM=/var/local/hmetal/lib/HakiosMetal.bundle
DST_ENV=/var/local/hmetal/env

put() {	# SRC DST TAG: backup DST once, then copy SRC -> DST.new and rename it over DST
	src=$1 dst=$2 tag=$3
	[ -f "$src" ] || { echo "missing $src"; exit 1; }
	if [ -e "$dst" ] && [ ! -e "$dst.pre-$tag" ]; then $T cp -p "$dst" "$dst.pre-$tag"; fi
	$T cp "$src" "$dst.new"
	$T mv -f "$dst.new" "$dst"
	echo "$($T sha256sum "$dst")"
}

restore() {	# DST TAG: DST.pre-TAG -> DST, by copy + rename
	dst=$1 tag=$2
	[ -f "$dst.pre-$tag" ] || { echo "no $dst.pre-$tag: left alone"; return 0; }
	$T cp "$dst.pre-$tag" "$dst.new"
	$T mv -f "$dst.new" "$dst"
	echo "restored $($T sha256sum "$dst")"
}

envmerge() {	# TEMPLATE ENV TAG (hm-int4): the phone's env is kept as it is. The template goes to ENV.default (copy + rename) and
	# every template setting the phone's env lacks or sets differently is printed ("+ new", "~ differs"), for a hand merge.
	# A first install (no ENV yet) takes the template as the env.
	tpl=$1 env=$2 tag=$3
	[ -f "$tpl" ] || { echo "missing $tpl"; exit 1; }
	if [ ! -f "$env" ]; then put "$tpl" "$env" "$tag"; echo "env: first install, template taken as $env"; return 0; fi
	put "$tpl" "$env.default" "$tag" >/dev/null
	echo "env: $env kept; template at $env.default; template settings the env lacks or sets differently:"
	n=0
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue;; esac
		key=${line%%=*}
		if $T grep -qxF -- "$line" "$env"; then continue; fi
		cur=; while IFS= read -r e; do case "$e" in "$key="*) cur=$e;; esac; done < "$env"
		if [ -n "$cur" ]; then echo "  ~ template: $line    phone: $cur"; else echo "  + $line"; fi
		n=$((n+1))
	done < "$tpl"
	echo "env: $n template line(s) not in the phone's env (none applied; edit $env by hand, then psx -k backboardd)"
}

case ${1:-} in
restore)
	TAG=${2:?tag}
	for f in SweetMetal2 Info.plist _CodeSignature/CodeResources; do restore "$DST_SHIM/$f" "$TAG"; done
	for f in HakiosMetal Info.plist _CodeSignature/CodeResources; do restore "$DST_HM/$f" "$TAG"; done
	restore "$DST_ENV" "$TAG"
	restore "$DST_ENV.default" "$TAG"
	[ -f "/var/local/bin/hmbench.pre-$TAG" ] && restore /var/local/bin/hmbench "$TAG"
	[ -f "/var/local/hmetal/hmbench.metallib.pre-$TAG" ] && restore /var/local/hmetal/hmbench.metallib "$TAG"
	;;
"")
	echo "usage: hm-install.sh SRC TAG | hm-install.sh restore TAG"; exit 1
	;;
*)
	SRC=$1 TAG=${2:?tag}
	for f in SweetMetal2 Info.plist _CodeSignature/CodeResources; do put "$SRC/SweetMetal2-shim.bundle/$f" "$DST_SHIM/$f" "$TAG"; done
	for f in HakiosMetal Info.plist _CodeSignature/CodeResources; do put "$SRC/HakiosMetal.bundle/$f" "$DST_HM/$f" "$TAG"; done
	envmerge "$SRC/hm-env.default" "$DST_ENV" "$TAG"
	# optional part of the build output (hm-int3 file set): the benchmark and its metallib
	if [ "${HMI_BENCH:-0}" = 1 ]; then
		[ -f "$SRC/hmbench" ] && put "$SRC/hmbench" /var/local/bin/hmbench "$TAG"
		[ -f "$SRC/hmbench.metallib" ] && put "$SRC/hmbench.metallib" /var/local/hmetal/hmbench.metallib "$TAG"
	fi
	# host-only parts, never installed: seed/ (hm-seed, mkseed.sh: run on the PC), *.dSYM
	[ -f "$SRC/SHA256SUMS" ] && echo "build SHA256SUMS: $($T grep 'HakiosMetal.bundle/HakiosMetal' "$SRC/SHA256SUMS" | $T cut -c1-16) HakiosMetal"
	;;
esac
echo "done; now: /var/local/bin/psx -k backboardd (once)"
