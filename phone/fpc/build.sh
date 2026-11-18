#!/bin/sh
# sweetfpcd (ws-fpcd-a): FPC fingerprint TA daemon for SweetMesa, into phone/fpc/out with work/userland/bin/ios-cc,
# signed ad hoc. Uses phone/qsee's qsee_lib (shared with qseeprobe). Called by phone/tools/build.sh; copies into the
# userland output (../../../out, holding dropbearmulti) if it exists, like qseeprobe.
set -e
cd "$(dirname "$0")"
CC=../../work/userland/bin/ios-cc
V=$(git rev-parse --short HEAD 2>/dev/null || date +%F)
mkdir -p out
$CC -O2 -Wall -Wextra -Wno-unused-parameter -DFPCD_VERSION="\"$V\"" -o out/sweetfpcd \
	sweetfpcd.c fpcd_hold.c ../qsee/qsee_lib.c ../qsee/qsee_gk.c ../qsee/qsee_wl.c ../video/pmem.c -framework IOKit -framework CoreFoundation
codesign -f -s - out/sweetfpcd
ls -la out
U=../../../out
if [ -f "$U/dropbearmulti" ]; then
	cp out/sweetfpcd "$U/"
	cp com.redfin.sweetfpcd.plist "$U/"
	echo "fpc: copied to the userland output"
fi
