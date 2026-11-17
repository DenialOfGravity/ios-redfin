#!/var/local/bin/dash
export PATH=/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
n=0; while [ $n -lt 400 ]; do grep -q 'init2 ok' /var/local/fastrpcd.log 2>/dev/null && break; /var/local/bin/toybox sleep 0.25; n=$((n+1)); done
/var/local/bin/toybox date +%T > /var/local/sensd-probe.log
/var/local/bin/sensd --list > /var/local/sensd-probe-1.log 2>&1
# sns 2026-10-29: run the sensor daemon after the probe (auto-rotate); revert: cp sensd-probe.sh.pre-sns sensd-probe.sh
[ -e /var/local/sensd.off ] || exec /var/local/bin/sensd --daemon >> /var/local/sensd.log 2>&1
