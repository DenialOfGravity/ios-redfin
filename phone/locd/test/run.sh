#!/bin/sh
# Host tests for sweetlocd (pdsemu.c, loc.c with a stubbed QRTR link). Run: sh phone/locd/test/run.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
mkdir -p "$T/var"
CC=${CC:-cc}
W="-Wall -Wextra -Wno-unused-parameter -I. -I../wifi"
$CC -Wall -Wextra -Wno-unused-parameter -o "$T/pdsemu_test" test/pdsemu_test.c pdsemu.c -lm
D="-DLOC_ON=\"$T/var/loc.on\" -DLOC_STATUS=\"$T/var/loc.status\" -DLOCK_ORIG=\"$T/var/locd.lock-orig\""
$CC $W $D -o "$T/loc_test" test/loc_test.c loc.c englock.c pdsemu.c ../wifi/qmi.c
$CC $W $D -o "$T/pdsr_test" test/pdsr_test.c loc.c englock.c pdsemu.c ../wifi/qmi.c
$CC $W $D -o "$T/lock_test" test/lock_test.c loc.c englock.c pdsemu.c ../wifi/qmi.c
"$T/pdsemu_test"
"$T/loc_test" > "$T/loc_test.log" 2>&1 || { cat "$T/loc_test.log"; exit 1; }
tail -1 "$T/loc_test.log"
"$T/pdsr_test" > "$T/pdsr_test.log" 2>&1 || { cat "$T/pdsr_test.log"; exit 1; }
tail -1 "$T/pdsr_test.log"
"$T/lock_test" > "$T/lock_test.log" 2>&1 || { cat "$T/lock_test.log"; exit 1; }
tail -1 "$T/lock_test.log"
rm -rf "$T"
