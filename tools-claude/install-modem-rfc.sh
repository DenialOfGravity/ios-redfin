#!/bin/bash
# install-modem-rfc.sh: put the modem's per-board RF card (RFC) modules on the phone, so that the TFTP server in wland
# (phone/wifi/tftp.c) can serve them. The modem asks for /readonly/firmware/image/modem_pr/so/549_38_0.mbn at every
# modem boot (logs/r55a-wland.log:221-253, "not found"). That file holds rfc_hwid549_r3_pvt_row_*, including the
# GNSS eLNA/antenna path tables (logs/research/gnss-rf-2026-11-16.md).
#
# Where the files go: tftp.c open_path() (tftp.c:98-108) maps "/readonly/firmware/image/<any/sub/dirs>/NAME" to
# /var/local/fw/modem/<name, lowercased>, so the files go FLAT into /var/local/fw/modem. Subfolders are not needed and
# would not be used. All 59 names are lowercase already and none clashes with modem.mdt/.bNN, modemr.jsn, wlanmdsp.mbn,
# modemuw.jsn or bdwlan*.bin.
#
# Source: Stock/extracted/redfin/modem_pr_so/ (from the factory radio image g7250-00264-230619-B. Its modem.mdt/.b22
# match the phone's own (recon/fw), so it is the same build). Google/Qualcomm files: never into git.
#
# Usage (WSL, with tools-claude/phone-agent.sh running and the phone up in iOS):
#   install-modem-rfc.sh                 put all 59 modules (about 46 MB)
#   install-modem-rfc.sh --only 549_38_0.mbn [NAME...]   put only these
#   install-modem-rfc.sh --remove [MANIFEST]  rollback: remove exactly the files a manifest says this script added
#                                        (default: the newest agent/stage/modem-rfc-manifest-*.txt)
#   add --dry-run to print the agent requests without sending them
# The modem reads the RFC only when it boots. After installing, reboot the phone (`wland shutdown; wland up` is
# untested, HANDOFF.md:41), then check the wland log: `grep modem_pr /var/local/wland.log` must show no "not found".
#
# Phone access is only through the phone agent: `files put` (tools/flame-files.py over sweet_files TCP 2223) to copy,
# `phone CMD` (SSH) for listing, sha256 (/var/local/tools/bin/sha256sum) and, in --remove, rm.
set -u -o pipefail
P=/mnt/d/ClaudeProjects/iOS_Pixel5
SRC=$P/Stock/extracted/redfin/modem_pr_so
DST=/var/local/fw/modem
SHA=/var/local/tools/bin/sha256sum
IN=$P/agent/inbox OUT=$P/agent/outbox
STAMP=$(date +%Y%m%d-%H%M%S)
DRY=0 MODE=install ONLY=() MAN=
while [ $# -gt 0 ]; do
	case $1 in
	--dry-run) DRY=1;;
	--remove) MODE=remove; if [ $# -gt 1 ] && [[ $2 != --* ]]; then MAN=$2; shift; fi;;
	--only) shift; while [ $# -gt 0 ] && [[ $1 != --* ]]; do ONLY+=("$1"); shift; done; continue;;
	-h|--help) sed -n 2,26p "$0"; exit 0;;
	*) echo "unknown argument: $1" >&2; exit 2;;
	esac
	shift
done

n=0
# req LINE: one phone-agent request (unique name), wait for its answer, print it; status = the request's exit code
req() {
	n=$((n + 1))
	local name="rfc-$STAMP-$n" t=0
	if [ $DRY = 1 ]; then echo "[dry-run] $name: $1"; return 0; fi
	printf '%s\n' "$1" > "$IN/$name.req.tmp" && mv "$IN/$name.req.tmp" "$IN/$name.req"
	while [ ! -e "$OUT/$name.out" ]; do
		sleep 2; t=$((t + 2))
		if [ $t -ge 400 ]; then echo "no answer to $name after 400 s: is phone-agent.sh running? (request left in inbox)" >&2; return 99; fi
	done
	cat "$OUT/$name.out"
	tail -1 "$OUT/$name.out" | grep -q '^== exit 0 ' 
}

if [ $MODE = install ]; then
	[ -d "$SRC" ] || { echo "no $SRC" >&2; exit 1; }
	if [ ${#ONLY[@]} -gt 0 ]; then files=(); for f in "${ONLY[@]}"; do files+=("$SRC/${f##*/}"); done
	else files=("$SRC"/*.mbn); fi
	for f in "${files[@]}"; do
		[ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
		b=${f##*/}; [[ $b =~ ^[a-z0-9_]+\.mbn$ ]] || { echo "unexpected name $b" >&2; exit 1; }
	done
	mkdir -p "$P/agent/stage"
	MAN=$P/agent/stage/modem-rfc-manifest-$STAMP.txt
	[ $DRY = 1 ] && MAN=/tmp/modem-rfc-manifest-$STAMP.dryrun
	{
		echo "# install-modem-rfc.sh $STAMP: files put into $DST (flat; tftp.c maps modem_pr/so/NAME there)"
		echo "# source $SRC"
		echo "# columns: sha256 size phone-path preexisting(yes/no)"
	} > "$MAN.tmp"
	# what is there before (so --remove never deletes a file we did not add)
	before=$(req "phone ls $DST" | grep -E '^[a-z0-9_]+\.mbn$' || true)
	[ $DRY = 1 ] || echo "$before" > "$P/agent/stage/modem-rfc-before-$STAMP.txt"
	total=0
	for f in "${files[@]}"; do
		b=${f##*/}
		pre=no; grep -qx "$b" <<<"$before" && pre=yes
		echo "$(sha256sum "$f" | cut -d' ' -f1) $(stat -c %s "$f") $DST/$b $pre" >> "$MAN.tmp"
		total=$((total + $(stat -c %s "$f")))
		req "files put $f $DST/$b 644" >/dev/null || { echo "files put $b failed (see $OUT/rfc-$STAMP-$n.out)" >&2; mv "$MAN.tmp" "$MAN"; exit 1; }
		echo "put $b"
	done
	echo "# total $total bytes, ${#files[@]} files" >> "$MAN.tmp"
	mv "$MAN.tmp" "$MAN"
	[ $DRY = 1 ] && { echo "manifest (dry run) $MAN"; exit 0; }
	# verify on the phone: every hash must match the manifest
	names=$(awk '!/^#/ {print $3}' "$MAN" | tr '\n' ' ')
	got=$(req "phone $SHA $names" | grep -E '^[0-9a-f]{64} ')
	bad=0
	while read -r h s p pre; do
		[[ $h == \#* ]] && continue
		grep -q "^$h  $p\$" <<<"$got" || { echo "MISMATCH or missing: $p" >&2; bad=1; }
	done < "$MAN"
	[ $bad = 0 ] && echo "verified ${#files[@]} files ($total bytes) on the phone; manifest $MAN" || { echo "verification failed; manifest $MAN" >&2; exit 1; }
	echo "next: reboot the phone (the modem reads the RFC at boot), then: phone grep -c 'modem_pr.*not found' /var/local/wland.log  (expect 0)"
	exit 0
fi

# --remove
[ -n "$MAN" ] || MAN=$(ls -t "$P"/agent/stage/modem-rfc-manifest-*.txt 2>/dev/null | head -1)
[ -f "$MAN" ] || { echo "no manifest" >&2; exit 1; }
rm_list=$(awk '!/^#/ && $4 == "no" && $3 ~ /^\/var\/local\/fw\/modem\/[a-z0-9_]+\.mbn$/ {print $3}' "$MAN" | tr '\n' ' ')
[ -n "$rm_list" ] || { echo "manifest $MAN lists nothing this script added" >&2; exit 0; }
echo "removing $(wc -w <<<"$rm_list") files listed as added in $MAN"
req "phone rm -f $rm_list" || { echo "rm failed" >&2; exit 1; }
[ $DRY = 1 ] && exit 0
left=$(req "phone ls $DST" | grep -cxF -f <(tr ' ' '\n' <<<"$rm_list" | sed 's#.*/##;/^$/d') || true)
echo "left on the phone from that list: $left (expect 0). Reboot (or restart the modem) for the modem to drop the RFC."
