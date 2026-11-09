#!/bin/sh
# hmbench and what it runs, into phone/hmetal/out/bench (called by phone/hmetal/build.sh after the library):
#   hmbench               the benchmark (bench/hmbench.m), signed ad hoc  -> /var/local/bin/hmbench
#   hmbench.metallib      hakios-metal's own AIR kernels (tri_attr_vs, tri_tex_fs, tri_color_fs) in a container made
#                         by bench/mkmetallib.py (llvm-as of the .ll; no Apple tool, no Apple metallib) -> /var/local/hmetal/
#   HakiosMetal.bundle    hakios' render-capable plug-in (HakiosMetalCA.m, fetched at the pinned commit, never committed;
#                         bench/plugin.py's edits) over out/libhakios-metal.a -> /var/local/hmetal/HakiosMetal.bundle
# HMB_LIBS: Mesa's archives when the library is built with MESA=1 (phone/hmetal/build.sh passes its LIBS).
# Needs: $CC (ios-cc), llvm-as (LLVM 15+: opaque pointers), python3, curl (or HMB_CA_SRC=FILE).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"
UP=../upstream
O=../out/bench
mkdir -p "$O/obj"
HAKIOS_COMMIT=2631f5d7d4f9d7322aca60991b34ba465dec343b
CA_SHA=2f905fa6f74c956a20a93b629c09762efbbaa3797aa20e847029bbdfc8baf1aa
LLVM_AS=${LLVM_AS:-$(command -v llvm-as || ls /usr/bin/llvm-as-* 2>/dev/null | sort -V | tail -1)}

# 1. the metallib
for k in tri_attr_vs tri_tex_fs tri_color_fs; do "$LLVM_AS" "$UP/tools/kernels/$k.ll" -o "$O/obj/$k.bc"; done
python3 mkmetallib.py "$O/hmbench.metallib" tri_attr_vs:vertex:"$O/obj/tri_attr_vs.bc" \
	tri_tex_fs:fragment:"$O/obj/tri_tex_fs.bc" tri_color_fs:fragment:"$O/obj/tri_color_fs.bc"

# 2. hmbench
"$CC" -x objective-c -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -o "$O/hmbench" hmbench.m \
	-framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>&1 |
	grep -v "was built for newer 'iOS' version" || true
[ -f "$O/hmbench" ]
strip -x "$O/hmbench" 2>/dev/null || true
codesign -f -s - "$O/hmbench" 2>/dev/null || true

# 3. the hakios plug-in (manual reference counting, as upstream builds it)
CA=$O/obj/HakiosMetalCA.orig.m
if [ -n "${HMB_CA_SRC:-}" ]; then cp "$HMB_CA_SRC" "$CA"; else
	curl -fsSL -o "$CA" "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/$HAKIOS_COMMIT/userland/src/HakiosMetalCA.m"; fi
echo "$CA_SHA  $CA" | (sha256sum -c 2>/dev/null || shasum -a 256 -c)
python3 plugin.py "$CA" "$O/obj/HakiosMetalCA.m"
B=$O/HakiosMetal.bundle
rm -rf "$B" && mkdir -p "$B"
U=""; for s in open first_problem status formats variant_hook; do U="$U -Wl,-U,_native_bridge_metal_$s"; done
"$CC" -x objective-c -fno-objc-arc -O2 -g -w -bundle -I$UP/include -o "$B/HakiosMetal" "$O/obj/HakiosMetalCA.m" \
	-x none ../out/libhakios-metal.a ${HMB_LIBS:-} -lc++ -lobjc $U -framework Foundation -framework IOKit -framework IOSurface \
	-framework Metal -framework CoreFoundation 2>&1 | grep -v "was built for newer 'iOS' version" || true
[ -f "$B/HakiosMetal" ]
strip -x "$B/HakiosMetal" 2>/dev/null || true
cp Info.plist "$B/Info.plist"
codesign -f -s - "$B" 2>/dev/null || codesign -f -s - "$B/HakiosMetal" 2>/dev/null || true
nm -u "$B/HakiosMetal" 2>/dev/null | grep -c . | sed 's/^/HakiosMetal: undefined symbols: /' || true
# 4. (H3) the SweetMetal2 stand-in for the window-server test (bench/m2shim.m): -> /System/Library/Extensions/SweetMetal2.bundle
#    for the test only, the real bundle kept at /var/local/SweetMetal2.bundle.pre-hmca
#    (hmapp) the dispatcher's choice on the build host first (bench/m2shim_test.c includes m2shim.m without Objective-C)
${HOSTCC:-cc} -Wno-format-truncation -o "$O/obj/m2shim_test" m2shim_test.c && "$O/obj/m2shim_test"
S=$O/SweetMetal2-shim.bundle
rm -rf "$S" && mkdir -p "$S"
"$CC" -x objective-c -fno-objc-arc -O2 -g -Wall -Wno-deprecated-declarations -bundle -o "$S/SweetMetal2" m2shim.m \
	-lobjc -framework Foundation -framework Metal 2>&1 | grep -v "was built for newer 'iOS' version" || true
[ -f "$S/SweetMetal2" ]
strip -x "$S/SweetMetal2" 2>/dev/null || true
cp m2shim.plist "$S/Info.plist"
codesign -f -s - "$S" 2>/dev/null || codesign -f -s - "$S/SweetMetal2" 2>/dev/null || true
rm -rf "$O/obj"
(cd "$O" && { sha256sum hmbench hmbench.metallib HakiosMetal.bundle/HakiosMetal SweetMetal2-shim.bundle/SweetMetal2 2>/dev/null ||
	shasum -a 256 hmbench hmbench.metallib HakiosMetal.bundle/HakiosMetal SweetMetal2-shim.bundle/SweetMetal2; }) > "$O/SHA256SUMS"
ls -la "$O" "$B" "$S"
