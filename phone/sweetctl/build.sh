#!/bin/sh
# ws-n-ctl: SweetControl.app (settings menu) and its setuid helper sweetctl, into phone/sweetctl/out.
# Called by phone/tools/build.sh (the local builder's component list and CI both run that); copies into the userland
# output like phone/video does. Install (phone agent `files put`, then on the phone):
#   out/sweetctl            -> /var/local/bin/sweetctl, then: toybox chown root:wheel /var/local/bin/sweetctl && chmod 4755 /var/local/bin/sweetctl
#   out/SweetControl.app/*  -> /Applications/SweetControl.app/ (root:wheel, binary 755), then: lsreg /Applications/SweetControl.app; respring
#   (optional) app/Knobs.json -> /var/local/sweetctl/knobs.json to edit the menu on the phone without a rebuild
# The FPS overlay is in SweetFPS.dylib (phone/fps/SweetHUD.m).
set -e
cd "$(dirname "$0")"
CC=../../work/userland/bin/ios-cc
rm -rf out; mkdir -p out/SweetControl.app
$CC -O2 -Wall -Wextra -Wno-unused-parameter -o out/sweetctl sweetctl.c
codesign -f -s - out/sweetctl
$CC -O2 -Wall -Wno-unused-parameter -fobjc-arc -o out/SweetControl.app/SweetControl app/main.m \
	-framework Foundation -framework UIKit -framework QuartzCore
cp app/Info.plist app/Knobs.json app/AppIcon60x60@2x.png app/AppIcon60x60@3x.png out/SweetControl.app/
# compile Info.plist to binary when a converter is there (both forms work)
if command -v plutil >/dev/null 2>&1; then plutil -convert binary1 out/SweetControl.app/Info.plist || true; fi
python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' app/Knobs.json
codesign -f -s - --entitlements app/SweetControl-ents.plist out/SweetControl.app
ls -la out out/SweetControl.app
U=../../../out
if [ -f "$U/dropbearmulti" ]; then
	cp out/sweetctl "$U/" && rm -rf "$U/SweetControl.app" && cp -R out/SweetControl.app "$U/"
	echo "sweetctl: copied to the userland output"
fi
