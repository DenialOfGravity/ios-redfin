#!/bin/sh
# Build hakios-metal for redfin (iOS 16 arm64) on our kernel's GPU (NativeAdreno ABI) into phone/hmetal/out:
#   libhakios-metal.a      the core (phone/hmetal/upstream) + the NativeAdreno transport (phone/hmetal/redfin)
#   hakios-metal-probe     upstream's on-phone probe (tools/probe.cpp) on that transport, signed ad hoc
#   hmetal-host-tests.txt  the host tests' summary (both chips), run first: a red host suite stops the build
#
#   phone/hmetal/build.sh              (WSL local build: tools-claude/localbuild sets up ios-cc; or on a Mac)
#   HMETAL_HOST_TESTS=0 phone/hmetal/build.sh   skip the host tests
#
# Inputs: work/userland/bin/ios-cc (clang -target arm64-apple-ios16.0 -isysroot <iPhoneOS SDK>), as phone/metal2/build.sh.
# MESA: without MESA=1 the library is built WITHOUT the in-process compiler (compileAir refuses by name; the probe's
# fixtures are precompiled ir3, so its compute and draw stages still run). MESA=1 needs an iOS cross build of Mesa
# carrying hakios's AIR front end (libair.a: hakios-xnu-kernel tools/mk-mesa.sh) at IOS_MESA_BUILD/IOS_MESA_SRC, which
# this repo does not build yet (our Turnip build's Mesa is 85fda3b3 without the AIR front end): not wired here.
# Nothing here touches the phone; install with `files put` (README.md, H2).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"
UP=upstream
OUT=out
mkdir -p "$OUT/obj"

# 1. host tests (g++/clang++ of the build host): the core as both chips, the transport, the core through the transport
if [ "${HMETAL_HOST_TESTS:-1}" != 0 ]; then
	HOSTCXX=${HOSTCXX:-$(command -v g++ || command -v clang++)}
	make -s -C "$UP" test CXX="$HOSTCXX" > "$OUT/hmetal-host-tests.txt" 2>&1 || { cat "$OUT/hmetal-host-tests.txt"; exit 1; }
	make -s -C redfin/tests all CXX="$HOSTCXX" >> "$OUT/hmetal-host-tests.txt" 2>&1 || { cat "$OUT/hmetal-host-tests.txt"; exit 1; }
	grep -E "^==|^chip:|tests, " "$OUT/hmetal-host-tests.txt"
fi

# 2. the library: upstream's CORE list (Makefile), plus the transport; not upstream's iokit_transport.cpp (HakiosGPU)
[ -x "$CC" ] || { echo "no $CC (the iOS cross compiler: tools-claude/localbuild ios_env, or a Mac's phone/metal2 setup)"; exit 1; }
CXXFLAGS="-x c++ -std=c++17 -O2 -g -Wall -Wextra -Werror -DHAKIOS_METAL_NATIVE_ADRENO -I$UP/include -Iredfin -I../turnip"
CORE="pm4 gpu command compute texture render link compiler metallib c_api fake_gpu"
rm -f "$OUT"/obj/*.o
for f in $CORE; do "$CC" $CXXFLAGS -c "$UP/src/$f.cpp" -o "$OUT/obj/$f.o"; done
"$CC" $CXXFLAGS -c redfin/native_adreno_transport.cpp -o "$OUT/obj/native_adreno_transport.o"
rm -f "$OUT/libhakios-metal.a"
# (Apple's ld wants the Darwin archive format: upstream's Makefile found GNU ar's symbol table unreadable)
AR=${AR:-$(command -v llvm-ar || echo ar)}
"$AR" --format=darwin rcs "$OUT/libhakios-metal.a" "$OUT"/obj/*.o 2>/dev/null || "$AR" rcs "$OUT/libhakios-metal.a" "$OUT"/obj/*.o

# 3. the probe (upstream tools/probe.cpp: its fixtures are this tree's own kernels, nothing of Apple's)
# (-Wno-missing-field-initializers: upstream probe.cpp leaves Bindings::samplers to its default; clang 21 -Werror refuses)
"$CC" $CXXFLAGS -Wno-missing-field-initializers -o "$OUT/hakios-metal-probe" "$UP/tools/probe.cpp" -x none "$OUT/libhakios-metal.a" -lc++ \
	-framework IOKit -framework CoreFoundation -framework IOSurface 2>&1 | grep -v "was built for newer 'iOS' version" || true
[ -f "$OUT/hakios-metal-probe" ] || exit 1
strip -x "$OUT/hakios-metal-probe" 2>/dev/null || true
codesign -f -s - "$OUT/hakios-metal-probe" 2>/dev/null || true
(cd "$OUT" && shasum -a 256 libhakios-metal.a hakios-metal-probe 2>/dev/null || sha256sum libhakios-metal.a hakios-metal-probe) > "$OUT/SHA256SUMS"
ls -la "$OUT"
