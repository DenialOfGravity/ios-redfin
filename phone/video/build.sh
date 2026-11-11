#!/bin/sh
# Build the Venus hardware video stack (redfin track V; ported from Yofuness/ios-flame 598ba5b, sunfish) into
# phone/video/out with work/userland/bin/ios-cc, signed ad hoc. Nothing here runs unless /var/local/venus.on exists
# on the phone (venusd exits, vtvenus refuses: VideoToolbox keeps its software decoders).
#   venusctl   step-by-step bring-up from a shell (probe, power, pas-check, boot, init, ping, session, decode, memtest)
#   venusd     the root daemon owning Venus for clients (socket /var/run/venusd.sock; org.iosonpixel.venusd.plist)
#   venusc     venusd's test client (Annex B file in, per-frame CRC/MD5 out)
#   vtvenus.dylib  the VideoToolbox decoder (H.264 + HEVC 8-bit) in the free decoder slot
#              /System/Library/VideoDecoders/ExampleIPB.videodecoder (a venusd client, loaded by mediaserverd)
#   vtstub.dylib   slot probe for the same path (registers nothing useful; logs that it was loaded)
#   vtdecode   VTDecompressionSession probe (FILE: AVAssetReader; --raw IN.h264 --hw require: one VT session)
#   avplay     AVPlayer probe (frames out of a stock player item)
# Called by phone/tools/build.sh (the local userland build); when the userland output directory (../../../out next to
# the build's repo checkout, holding dropbearmulti) exists, the results are copied there too.
set -e
cd "$(dirname "$0")"
CC=../../work/userland/bin/ios-cc
W="-O2 -Wall -Wextra -Wno-unused-parameter"
mkdir -p out
sign() {	# FILE [ENTITLEMENTS]
	if [ -n "${2:-}" ]; then codesign -f -s - --entitlements "$2" "$1"; else codesign -f -s - "$1"; fi
}
$CC $W -o out/venusctl venusctl.c venus_core.c venus_h264.c venus_hevc.c pmem.c smmu.c
sign out/venusctl
$CC $W -o out/venusd venusd.c venus_core.c venus_h264.c venus_hevc.c pmem.c smmu.c -framework CoreFoundation -framework IOSurface
sign out/venusd
$CC $W -o out/venusc venusc.c venus_h264.c venus_hevc.c -framework CoreFoundation -framework CoreVideo -framework IOSurface
sign out/venusc
$CC -O2 -Wall -dynamiclib -install_name /System/Library/VideoDecoders/ExampleIPB.videodecoder -o out/vtstub.dylib vtstub.c
$CC $W -dynamiclib -install_name /System/Library/VideoDecoders/ExampleIPB.videodecoder \
	-framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox \
	-o out/vtvenus.dylib vtvenus.c venus_hevc.c
for f in vtstub vtvenus; do
	sign out/$f.dylib
	nm -gU out/$f.dylib 2>/dev/null | grep -q '_ExampleIPBDecoderRegister' || { echo "$f: no ExampleIPBDecoderRegister export"; exit 1; }
done
$CC -O2 -Wall -fobjc-arc -o out/vtdecode vtdecode.m venus_h264.c -framework Foundation -framework AVFoundation \
	-framework VideoToolbox -framework CoreMedia -framework CoreVideo
sign out/vtdecode vtdecode-ents.plist
$CC -O2 -Wall -fobjc-arc -o out/avplay avplay.m -framework Foundation -framework AVFoundation -framework CoreMedia \
	-framework CoreVideo -framework QuartzCore
sign out/avplay vtdecode-ents.plist
cp org.iosonpixel.venusd.plist out/
ls -la out
U=../../../out
if [ -f "$U/dropbearmulti" ]; then
	cp out/venusctl out/venusd out/venusc out/vtvenus.dylib out/vtstub.dylib out/vtdecode out/avplay \
		out/org.iosonpixel.venusd.plist "$U/"
	echo "video: copied to the userland output"
fi
