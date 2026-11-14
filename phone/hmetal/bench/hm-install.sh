#!/bin/sh
# hm-install.sh SRC TAG: install the Metal dispatcher + hakios-metal on the phone WITHOUT overwriting files in place.
#
#   SRC  a folder on the phone holding this build's bench/ output: SweetMetal2-shim.bundle/, HakiosMetal.bundle/,
#        hm-env.default (copy the build's bench/ folder there with `files put` first)
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

case ${1:-} in
restore)
	TAG=${2:?tag}
	for f in SweetMetal2 Info.plist _CodeSignature/CodeResources; do restore "$DST_SHIM/$f" "$TAG"; done
	for f in HakiosMetal Info.plist _CodeSignature/CodeResources; do restore "$DST_HM/$f" "$TAG"; done
	restore "$DST_ENV" "$TAG"
	;;
"")
	echo "usage: hm-install.sh SRC TAG | hm-install.sh restore TAG"; exit 1
	;;
*)
	SRC=$1 TAG=${2:?tag}
	for f in SweetMetal2 Info.plist _CodeSignature/CodeResources; do put "$SRC/SweetMetal2-shim.bundle/$f" "$DST_SHIM/$f" "$TAG"; done
	for f in HakiosMetal Info.plist _CodeSignature/CodeResources; do put "$SRC/HakiosMetal.bundle/$f" "$DST_HM/$f" "$TAG"; done
	if [ -f "$DST_ENV" ]; then
		echo "env: $DST_ENV kept (template: $SRC/hm-env.default)"
	else
		put "$SRC/hm-env.default" "$DST_ENV" "$TAG"
	fi
	;;
esac
echo "done; now: /var/local/bin/psx -k backboardd (once)"
