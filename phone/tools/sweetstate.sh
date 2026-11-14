#!/var/local/bin/dash
# sweetstate.sh: one machine-readable dump of the phone's managed state for tools/state.py (ws-tools-state).
# Read-only: it lists, hashes and reads; it never writes, moves, kills or restarts anything.
#
# Not installed on the phone: the PC sends it on each run over ssh stdin
#   ssh root@192.168.7.2 '/var/local/bin/dash -s -- [ARGS]' < phone/tools/sweetstate.sh
# (phone agent: `state-dump NAME` / `state diff`; the live agent before the switch:
#   phone echo <base64 of this file> | /var/local/bin/toybox base64 -d | /var/local/bin/dash -s -- [ARGS])
#
# ARGS (all optional, any order):
#   -x GLOB    no sha256 for paths matching GLOB (find -path, relative to a root: './hmetal/ca/*.var*')
#   -n NAME    sysctl to read when the sweetstate helper (phone/tools/sweetstate.c) is not installed (repeatable;
#              a NAME without a dot means kern.sweet_NAME: dropbear refuses commands over ~9000 bytes)
#   -q         no sha256 at all (fast listing)
# Output: one record per line, TAB separated; the first field is the record type:
#   #sweetstate 1                 format version
#   H key value                   header: now, boottime, uptime, kern.version/uuid/bootargs/osversion, helper
#   R root realpath               a managed root; F/K/L/C lines after it use paths relative to it ('./bin/wland')
#   F type mode uid gid size mtime path        (type: f d l o)
#   K sha256 path                 hash of a regular file
#   L path target                 symlink
#   C path redacted base64        content of a small text file (<= 4 KiB) in a flag directory; values of keys that
#                                 look like PIN/PUK/ICCID/IMSI/IMEI/MSISDN are replaced ON THE PHONE by @PHONE@
#                                 (redacted = how many)
#   I path files kbytes           an ignored directory (scratch, caches, logs): only its size
#   S name value                  a sysctl (all kern.sweet_* with the helper; else the -n names)
#   P pid ppid uid start comm     a process (start = epoch seconds, '-' without the helper)
#   M pid comm path               an image mapped by a process (helper only: which Metal/audio bundle is live)
#   E text                        a non-fatal error
set -u
T=/var/local/bin/toybox
[ -x /var/local/bin/toybox-full ] && T=/var/local/bin/toybox-full	# has grep/tr/sort (ws-tools-state)
export PATH=/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
TAB=$(printf '\t')
HELPER=/var/local/bin/sweetstate
NOHASH= NAMES= QUICK=0
while [ $# -gt 0 ]; do
	case $1 in
	-x) NOHASH="$NOHASH
$2"; shift;;
	-n) case $2 in *.*) NAMES="$NAMES $2";; *) NAMES="$NAMES kern.sweet_$2";; esac; shift;;
	-q) QUICK=1;;
	esac
	shift
done
# never hashed: logs (they change every second) and the ignored dirs (pruned anyway)
NOHASH="$NOHASH
*.log
*.log.*
*.out"
# hash only, never content: personal identifiers, keys, modem data
SECRET_FILES='abp-policy* modem-profile* *.key sc-preferences* *MobileGestalt* *simpin* *iccid* *imsi*'
# PIN/PUK/ICCID/IMSI/IMEI/MSISDN values never leave the phone. toybox here has no grep/tr/awk: dash case globs.
# A line that mentions one is printed token by token with the value of every such KEY=VALUE replaced by @PHONE@
# (every bare word on that line too); "pin"/"puk" count at the start or after a non-letter (pin=, sim_pin=, SIMPIN=).
# state.py writes such a file back by filling @PHONE@ from the file already on the phone (apply / set).
secret_tok() {	# TOKEN -> 0 if it is secret
	case $1 in
	[Pp][Ii][Nn]*|*[!A-Za-z][Pp][Ii][Nn]*|[Pp][Uu][Kk]*|*[!A-Za-z][Pp][Uu][Kk]*|*[Ss][Ii][Mm][Pp][Ii][Nn]*|\
	*[Ii][Cc][Cc][Ii][Dd]*|*[Ii][Mm][Ss][Ii]*|*[Ii][Mm][Ee][Ii]*|*[Mm][Ss][Ii][Ss][Dd][Nn]*) return 0;;
	esac
	return 1
}
secret_lines() {	# count | keep   (stdin)
	c=0
	set -f
	while IFS= read -r l || [ -n "$l" ]; do
		if secret_tok "$l"; then
			r=
			for t in $l; do
				case $t in
				*=*) secret_tok "${t%%=*}" && case ${t#*=} in
					0|1) ;;	# an on/off switch (cell.on pin=1 = wland's PIN socket on), not a secret
					*) c=$((c + 1)); t="${t%%=*}=@PHONE@";;
					esac;;
				*) c=$((c + 1)); t=@PHONE@;;	# a bare word on such a line could be the value ("pin 1234")
				esac
				r="$r${r:+ }$t"
			done
			[ "$1" = keep ] && printf '%s\n' "$r"
		else
			[ "$1" = keep ] && printf '%s\n' "$l"
		fi
	done
	set +f
	[ "$1" = count ] && echo $c
	return 0
}

echo "#sweetstate 1"
now=$($T date +%s)
echo "H${TAB}now${TAB}$now"
if [ -x $HELPER ]; then
	echo "H${TAB}helper${TAB}$($HELPER -v 2>&1)"
	$HELPER -b
else
	echo "H${TAB}helper${TAB}none"
	# kern.boottime = struct timeval, printed by sctl as hex bytes (little endian): tv_sec = the first 8 bytes
	h=$(sctl kern.boottime 2>/dev/null) && {
		h=${h#*: }; s=
		for i in 15 13 11 9 7 5 3 1; do s=$s$(printf %s "$h" | $T cut -c$i-$((i + 1))); done
		echo "H${TAB}boottime${TAB}$((0x$s))"
		echo "H${TAB}uptime${TAB}$((now - 0x$s))"
	}
fi
for k in kern.version kern.uuid kern.bootargs kern.osversion hw.memsize; do
	v=$(sctl $k 2>/dev/null) && echo "H${TAB}$k${TAB}${v#*: }"
done

# roots: real path first (find does not follow a symlinked root), the name the manifest uses second
MG=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches
ROOTS="/private/var/local=/var/local
/System/Library/Extensions/SweetMetal2.bundle=/System/Library/Extensions/SweetMetal2.bundle
/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver=/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver
/System/Library/xpc=/System/Library/xpc
/Applications/SweetControl.app=/Applications/SweetControl.app
$MG=$MG"
# ignored (pruned) dirs under /var/local: scratch area, caches, test output, live modem EFS
IGN='./scratch ./tmp ./m2v-cache ./hmetal/cache ./hmetal/log ./hmetal/dump ./fw/rmtfs'
# flag dirs: small text files here are dumped (contents)
FLAGDIRS='/var/local:. /var/local:./hmetal /var/local:./hmetal/ca /var/local:./sweetctl'

prune_expr() {	# the find arguments that prune the ignored dirs: -path A -o -path B ...
	o=
	for d in $IGN; do printf '%s -path %s' "$o" "$d"; o=' -o'; done
}

echo "$ROOTS" | while IFS='=' read -r real name; do
	[ -e "$real" ] || { echo "E${TAB}no root $real"; continue; }
	echo "R${TAB}$name${TAB}$real"
	cd "$real" || continue
	if [ "$name" = /var/local ]; then
		for d in $IGN; do
			[ -d "$d" ] || continue
			echo "I${TAB}$d${TAB}$($T find "$d" -type f 2>/dev/null | $T wc -l)${TAB}$($T du -sk "$d" 2>/dev/null | $T cut -f1)"
		done
		# shellcheck disable=SC2046
		set -- \( $(prune_expr) \) -prune -o
	else
		set --
	fi
	# F: type|mode|uid|gid|size|mtime|path  (%F: regular file / directory / symbolic link / ...)
	$T find . "$@" -print0 2>/dev/null | $T xargs -0 $T stat -c "%F|%a|%u|%g|%s|%Y|%n" 2>/dev/null |
	while IFS='|' read -r ty mo u g sz mt p; do
		case $ty in 'regular file'|'regular empty file') t=f;; directory) t=d;; 'symbolic link') t=l;; *) t=o;; esac
		echo "F${TAB}$t${TAB}$mo${TAB}$u${TAB}$g${TAB}$sz${TAB}$mt${TAB}$p"
	done
	$T find . "$@" -type l -print 2>/dev/null | while read -r p; do echo "L${TAB}$p${TAB}$($T readlink "$p")"; done
	if [ $QUICK = 0 ]; then
		set -- "$@" -type f
		# the no-hash expression goes into the positional list (a pipe would lose it: subshell); no globbing here
		old_ifs=$IFS; IFS='
'
		set -f
		for g in $NOHASH; do [ -n "$g" ] && set -- "$@" ! -path "$g" ! -name "$g"; done
		set +f
		IFS=$old_ifs
		$T find . "$@" -print0 2>/dev/null | $T xargs -0 $T sha256sum 2>/dev/null |
		while read -r h p; do echo "K${TAB}$h${TAB}$p"; done
	fi
done

# C: contents of small text files in the flag dirs (no backups, no secrets)
for fd in $FLAGDIRS; do
	real=/private/var/local; rel=${fd#*:}
	[ -d "$real/$rel" ] || continue
	cd "$real/$rel" || continue
	for f in * .[!.]*; do
		[ -f "$f" ] || continue
		p=$rel/$f
		case $f in *.pre-*|*.knowngood*|*.bak*|*.orig|*-orig|*.before-*|*.log|*.log.*|*.out|*.bin|*.tar|*.tgz|*.txt|*.text|*.data|*.str|*.nv12|*.ips) continue;; esac
		skip=0
		for s in $SECRET_FILES; do case $f in $s) skip=1;; esac; done
		[ $skip = 1 ] && continue
		sz=$($T stat -c %s "$f" 2>/dev/null) || continue
		[ "$sz" -le 4096 ] || continue
		n=$(secret_lines count < "$f")
		if [ "$n" -gt 0 ]; then
			echo "C${TAB}$p${TAB}$n${TAB}$(secret_lines keep < "$f" | $T base64 -w0)"
		else
			echo "C${TAB}$p${TAB}0${TAB}$($T base64 -w0 < "$f")"
		fi
	done
done
# Info.plist of the bundles (versions; the PC parses XML or binary plists)
cd /
for b in /System/Library/Extensions/SweetMetal2.bundle /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver \
	/Applications/SweetControl.app /private/var/local/hmetal/lib/HakiosMetal.bundle /private/var/local/hmetal/lib/SweetMetal2.bundle \
	/private/var/local/hmetal/ca/HakiosMetal.bundle; do
	f=$b/Info.plist; [ -f "$f" ] || continue
	echo "C${TAB}$f${TAB}0${TAB}$($T base64 -w0 < "$f")"
done

# sysctls and processes
if [ -x $HELPER ]; then
	$HELPER -S kern.sweet_ -P -M backboardd,SpringBoard,mediaserverd,CommCenter,wland,sensd,fastrpcd
else
	# plus every name the boot conf sets
	[ -f /var/local/sweet-sysctl.conf ] && while read -r n v || [ -n "$n" ]; do
		case $n in kern.*) case " $NAMES " in *" $n "*) ;; *) NAMES="$NAMES $n";; esac;; esac
	done < /var/local/sweet-sysctl.conf
	for n in $NAMES; do
		v=$(sctl "$n" 2>/dev/null) || continue
		# one line: tabs/newlines -> spaces (no tr here)
		set -f; o=$IFS; IFS=$(printf '\n\t'); set -- ${v#*: }; IFS=' '; v="$*"; IFS=$o; set +f
		echo "S${TAB}$n${TAB}$v"
	done
	psx | while read -r pid ppid uid cpu comm; do echo "P${TAB}$pid${TAB}$ppid${TAB}$uid${TAB}-${TAB}$comm"; done
fi
echo "H${TAB}end${TAB}$($T date +%s)"
