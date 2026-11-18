#!/bin/bash
# Touch ID bring-up for ws-tid-g/h kernels. sweetfpcd is started BEFORE biometrickitd's first catacomb load
# (biometrickitd loads at first unlock; if sweetfpcd is offline the load fails and is not retried).
# Usage: tid-bringup5.sh LOGTAG [WAKE]
tag=${1:-tid25}; wake=${2:-0}
for i in $(seq 1 20); do pa sh 'true' >/dev/null 2>&1 && break; sleep 3; done
set -x
pa state set kern.sweet_fpc 3 --temp
pa state set kern.sweet_passcode 1 --temp
pa state set kern.sweet_fb_sleep_blank 1 --temp
pa state set kern.sweet_rpmb 1 --temp
pa state set kern.sweet_mesa_match 2 --temp
pa state set kern.sweet_mesa_catacomb 7 --temp
pa state set kern.sweet_mesa_offline 1 --temp
pa state set kern.sweet_passcode_biostate 1 --temp
pa state set kern.sweet_mesa_fix 0x17 --temp
pa state set kern.sweet_mesa_lockout 0xf --temp
pa state set kern.sweet_passcode_bio_buzz 0x7f000c --temp
[ "$wake" != 0 ] && pa state set kern.sweet_mesa_wake $wake --temp
pa state set kern.sweet_mesa 0 --temp
pa sh '/var/local/bin/sweetctl kill biometrickitd'
pa state set kern.sweet_mesa 3 --temp
pa sh '/var/local/bin/sweetctl kill biometrickitd'
sleep 2
pa sh '/var/local/bin/sweetctl respring'
sleep 8
pa sh "/var/local/bin/psx | grep -E 'biometrickitd|sweetfpcd|SpringBoard'; for k in sweet_mesa sweet_mesa_catacomb sweet_mesa_lockout sweet_mesa_wake sweet_passcode_biostate; do echo \$k \$(/var/local/bin/sctl kern.\$k); done; grep -E 'start|bridge' /var/local/tmp/sweetfpcd.log | head -3"
