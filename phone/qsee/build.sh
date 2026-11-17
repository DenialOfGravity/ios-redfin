#!/bin/sh
# qseeprobe (ws-qsee-a, ws-qsee-b): QSEE queries + app load/send/unload over /dev/pmem PMEM_SMC, into phone/qsee/out with work/userland/bin/ios-cc,
# signed ad hoc. The host test of the SMC whitelist runs first (any host cc) and fails the build if it fails.
# Called by phone/tools/build.sh; copies into the userland output (../../../out, holding dropbearmulti) if it exists.
set -e
cd "$(dirname "$0")"
HOSTCC=${HOSTCC:-$(command -v cc || command -v gcc || command -v clang || true)}
if [ -n "$HOSTCC" ]; then
	mkdir -p out/host
	$HOSTCC -O1 -Wall -o out/host/test_wl test_wl.c qsee_wl.c
	out/host/test_wl
else
	echo "qsee: no host cc, whitelist test skipped"
fi
CC=../../work/userland/bin/ios-cc
mkdir -p out
$CC -O2 -Wall -Wextra -Wno-unused-parameter -o out/qseeprobe qseeprobe.c qsee_wl.c ../video/pmem.c
codesign -f -s - out/qseeprobe
ls -la out
U=../../../out
if [ -f "$U/dropbearmulti" ]; then
	cp out/qseeprobe "$U/"
	echo "qsee: copied to the userland output"
fi
