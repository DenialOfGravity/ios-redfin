#!/var/local/bin/dash
# sweet-sysctl.sh: boot-time sysctl loader (M0, ws-n-bb-int). Started once at boot from sensd-probe.sh (launchd job
# org.iosonpixel.sensdprobe in /System/Library/xpc/launchd.plist), before its fastrpcd wait.
# Reads /var/local/sweet-sysctl.conf: one "NAME VALUE" per line, decimal VALUE (optional '-'), '#' comments, blank
# lines ignored. Each line is applied with /var/local/bin/sctl NAME VALUE; everything goes to /var/local/sweet-sysctl.log.
# No conf file = no-op (nothing logged). Disable without removing anything: touch /var/local/sweet-sysctl.off
export PATH=/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
C=/var/local/sweet-sysctl.conf L=/var/local/sweet-sysctl.log S=/var/local/bin/sctl
[ -f "$C" ] || exit 0
[ -e /var/local/sweet-sysctl.off ] && { echo "$(date +%FT%T) boot: sweet-sysctl.off present, nothing applied" >> $L; exit 0; }
echo "$(date +%FT%T) boot: applying $C" >> $L
n=0 ok=0
while read -r name value extra || [ -n "$name" ]; do
	n=$((n + 1))
	case $name in ''|'#'*) continue;; esac
	case $value in '#'*) value=;; esac
	case $extra in ''|'#'*) ;; *) echo "  line $n: skipped (more than NAME VALUE): $name $value $extra" >> $L; continue;; esac
	case $name in *[!a-z0-9_.]*|.*|*.) echo "  line $n: skipped (bad name): $name" >> $L; continue;; esac
	v=${value#-}
	case $v in ''|*[!0-9]*) echo "  line $n: skipped (value not decimal): $name '$value'" >> $L; continue;; esac
	if out=$($S "$name" "$value" 2>&1); then ok=$((ok + 1)); echo "  $out" >> $L
	else echo "  line $n: FAILED $name $value: $out" >> $L; fi
done < "$C"
echo "$(date +%FT%T) boot: $ok applied" >> $L
exit 0
