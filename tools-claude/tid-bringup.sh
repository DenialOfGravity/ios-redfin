#!/bin/bash
# Touch ID bring-up for ws-tid-d kernels (1206 catacomb, 1207 offline, 1210-1213 opfix). Usage: tid-bringup2.sh LOGTAG
tag=${1:-tid19}
for i in $(seq 1 20); do pa sh 'true' >/dev/null 2>&1 && break; sleep 3; done
set -x
pa state set kern.sweet_fpc 3 --temp
pa state set kern.sweet_passcode 1 --temp
pa state set kern.sweet_fb_sleep_blank 1 --temp
pa state set kern.sweet_rpmb 1 --temp
pa state set kern.sweet_mesa_match 2 --temp
pa state set kern.sweet_mesa_catacomb 1 --temp
pa state set kern.sweet_mesa_offline 1 --temp
pa state set kern.sweet_mesa 0 --temp
pa sh '/var/local/bin/sweetctl kill biometrickitd'
sleep 2
pa state set kern.sweet_mesa 3 --temp
sleep 3
pa sh "cd /var/local/tmp && FPCD_KEEP_TA=1 toybox-full nohup toybox-full setsid toybox-full timeout 86400 /var/local/bin/sweetfpcd > /var/local/tmp/fpcd-$tag.log 2>&1 < /dev/null &"
sleep 5
pa sh '/var/local/bin/sweetctl respring'
sleep 8
pa sh "/var/local/bin/psx | grep -E 'biometrickitd|sweetfpcd|SpringBoard'; for k in sweet_fpc sweet_passcode sweet_mesa sweet_mesa_match sweet_mesa_catacomb sweet_mesa_offline sweet_mesa_fix sweet_rpmb; do echo \$k \$(/var/local/bin/sctl kern.\$k); done; grep -E 'start|bridge|sensor init|catacomb|error' /var/local/tmp/fpcd-$tag.log | head -8"
