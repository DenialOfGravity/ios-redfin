#!/bin/sh
# Build SweetFPS.dylib (iOS 16 arm64, signed ad hoc) into phone/fps/out (ws-u-fps; see SweetFPS.m).
# ws-n-ctl: SweetHUD.m too (FPS overlay, off unless SweetControl turns it on).
# ws-z-ui: SweetSB.m rides along (status-bar clock clear of the camera hole; off without /var/local/sweetsb.m|t).
# ws-n-pin: SweetPIN.m rides along (SIM PIN UI -> wland; off without /var/local/sweetpin.on).
# Install to a NEW file and rename it over (2026-11-05): files put out/SweetFPS.dylib /var/local/lib/SweetFPS.dylib.new 644;
#   mv /var/local/lib/SweetFPS.dylib.new /var/local/lib/SweetFPS.dylib; respring. Overwriting the loaded dylib in place keeps
#   the kernel's cached code signature on the vnode: SpringBoard goes CS-invalid, loses its entitlements and aborts in
#   +[SpringBoardUI load] ("should not be linking SpringBoardUI") on every launch until a reboot. Same for any dylib/binary.
# Originally: files put out/SweetFPS.dylib /var/local/lib/SweetFPS.dylib (0644), then give SpringBoard
# DYLD_INSERT_LIBRARIES=/var/local/lib/SweetFPS.dylib and SWEET_FPS=MODE (launchd EnvironmentVariables) and restart it.
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc -dynamiclib \
	-install_name /var/local/lib/SweetFPS.dylib \
	-framework Foundation -framework QuartzCore -framework UIKit -framework CoreGraphics \
	-o out/SweetFPS.dylib SweetFPS.m SweetSB.m SweetPIN.m SweetHUD.m SweetCell.m SweetCT.m
codesign -f -s - out/SweetFPS.dylib
ls -la out/SweetFPS.dylib
# The PC's local userland builder has a fixed component list (it runs phone/haptics/build.sh, which calls this):
# the dylib goes into the build log as base64 between these markers (tools-claude: sed -n '/^SWEETFPS-B64-BEGIN/,/^SWEETFPS-B64-END/p').
if [ -n "${SWEETFPS_B64:-}" ]; then
	echo SWEETFPS-B64-BEGIN
	base64 < out/SweetFPS.dylib
	echo SWEETFPS-B64-END
fi
