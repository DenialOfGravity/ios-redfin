#!/bin/sh
# Host tests for sweetlocd (pdsemu.c, loc.c with a stubbed QRTR link). Run: sh phone/locd/test/run.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
mkdir -p "$T/var"
CC=${CC:-cc}
W="-Wall -Wextra -Wno-unused-parameter -I. -I../wifi"
$CC -Wall -Wextra -Wno-unused-parameter -o "$T/pdsemu_test" test/pdsemu_test.c pdsemu.c -lm
D="-DLOC_ON=\"$T/var/loc.on\" -DLOC_STATUS=\"$T/var/loc.status\" -DLOCK_ORIG=\"$T/var/locd.lock-orig\" -DEFS_DIR=\"$T/efs\""
$CC $W $D -o "$T/loc_test" test/loc_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
$CC $W $D -o "$T/pdsr_test" test/pdsr_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
$CC $W $D -o "$T/lock_test" test/lock_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
$CC $W $D -o "$T/rfm_test" test/rfm_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
$CC $W $D -o "$T/probe2_test" test/probe2_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
"$T/pdsemu_test"
"$T/loc_test" > "$T/loc_test.log" 2>&1 || { cat "$T/loc_test.log"; exit 1; }
tail -1 "$T/loc_test.log"
"$T/pdsr_test" > "$T/pdsr_test.log" 2>&1 || { cat "$T/pdsr_test.log"; exit 1; }
tail -1 "$T/pdsr_test.log"
"$T/lock_test" > "$T/lock_test.log" 2>&1 || { cat "$T/lock_test.log"; exit 1; }
tail -1 "$T/lock_test.log"
"$T/probe2_test" > "$T/probe2_test.log" 2>&1 || { cat "$T/probe2_test.log"; exit 1; }
tail -1 "$T/probe2_test.log"
$CC $W $D -o "$T/gnssk_test" test/gnssk_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
"$T/gnssk_test" > "$T/gnssk_test.log" 2>&1 || { cat "$T/gnssk_test.log"; exit 1; }
grep "gnssk_test:" "$T/gnssk_test.log"
# ws-gnss-k: no coordinates anywhere: no lat/lon/alt field may be referenced outside pdsemu.c (parser + locationd encoder,
# which prints nothing), and the runtime test checks the log and status file
if grep -nE '(\.|->)(lat|lon|alt_ell|alt_msl)([^a-z_]|$)' loc.c sweetlocd.c rfm.c englock.c ntp.c; then
	echo "nocoord: a latitude/longitude/altitude field is used outside pdsemu.c"; exit 1
fi
if grep -nE 'printf' pdsemu.c | grep -E 'lat|lon|alt'; then echo "nocoord: pdsemu.c prints a position field"; exit 1; fi
$CC $W $D -o "$T/nocoord_test" test/nocoord_test.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
"$T/nocoord_test" 2> "$T/nocoord_test.err" >/dev/null || { cat "$T/nocoord_test.err"; exit 1; }
tail -1 "$T/nocoord_test.err"
"$T/rfm_test" > "$T/rfm_test.log" 2>&1 || { grep -n "FAIL\|REG_EVENTS\|> 0x" "$T/rfm_test.log"; exit 1; }
tail -1 "$T/rfm_test.log"
rm -rf "$T"
