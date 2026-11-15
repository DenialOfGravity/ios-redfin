#!/bin/sh
# Host tests for sweetlocd (pdsemu.c, loc.c with a stubbed QRTR link). Run: sh phone/locd/test/run.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
mkdir -p "$T/var"
CC=${CC:-cc}
W="-Wall -Wextra -Wno-unused-parameter -I. -I../wifi"
$CC -Wall -Wextra -Wno-unused-parameter -o "$T/pdsemu_test" test/pdsemu_test.c pdsemu.c -lm
$CC $W -DLOC_ON="\"$T/var/loc.on\"" -DLOC_STATUS="\"$T/var/loc.status\"" -o "$T/loc_test" test/loc_test.c loc.c pdsemu.c ../wifi/qmi.c
$CC $W -DLOC_ON="\"$T/var/loc.on\"" -DLOC_STATUS="\"$T/var/loc.status\"" -o "$T/pdsr_test" test/pdsr_test.c loc.c pdsemu.c ../wifi/qmi.c
"$T/pdsemu_test"
"$T/loc_test" > "$T/loc_test.log" 2>&1 || { cat "$T/loc_test.log"; exit 1; }
tail -1 "$T/loc_test.log"
"$T/pdsr_test" > "$T/pdsr_test.log" 2>&1 || { cat "$T/pdsr_test.log"; exit 1; }
tail -1 "$T/pdsr_test.log"
rm -rf "$T"
