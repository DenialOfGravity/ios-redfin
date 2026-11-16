#!/bin/sh
# Build sweetlocd (GNSS daemon: QMI LOC client + locationd PDS bridge) for iOS 16 arm64 into phone/locd/out (signed ad hoc).
# Host tests: sh phone/locd/test/run.sh
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -I. -I../wifi -o out/sweetlocd \
	sweetlocd.c loc.c englock.c pdsemu.c rfm.c ntp.c ../wifi/qmi.c
codesign -f -s - out/sweetlocd 2>/dev/null
ls -la out/sweetlocd
