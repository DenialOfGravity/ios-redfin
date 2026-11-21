#!/bin/sh
# ws-cam-icp: build icpctl (public part) into phone/icp/out with work/userland/bin/ios-cc, signed ad hoc.
# ICP_PRIVATE_DIR=DIR (a private camera tree with tools/icp_ipe.c and regs/icp-iface.h): also link the IPE pass (M2);
# the result then contains firmware-interface layouts: never commit or publish it.
set -e
cd "$(dirname "$0")"
# outside tools-claude/localbuild (which puts its wrappers on PATH): take them from its env.sh
LBENV=/mnt/d/ClaudeProjects/iOS_Pixel5/tools-claude/localbuild/env.sh
if ! command -v codesign >/dev/null && [ -f "$LBENV" ]; then
	. "$LBENV"
	export PATH="$LBIN:$PATH" IOS_SDK SDKROOT=$IOS_SDK
fi
CC=${CC:-../../work/userland/bin/ios-cc}
W="-O2 -Wall -Wextra -Wno-unused-parameter"
mkdir -p out
if [ -n "${ICP_PRIVATE_DIR:-}" ]; then
	$CC $W -DICP_PRIVATE -I. -I../video -I"$ICP_PRIVATE_DIR/regs" -o out/icpctl icpctl.c icp_hw.c ../video/pmem.c \
		"$ICP_PRIVATE_DIR/tools/icp_ipe.c"
else
	$CC $W -o out/icpctl icpctl.c icp_hw.c ../video/pmem.c
fi
codesign -f -s - out/icpctl
ls -la out/icpctl
