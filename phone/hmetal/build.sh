#!/bin/sh
# Build hakios-metal for redfin (iOS 16 arm64) on our kernel's GPU (NativeAdreno ABI) into phone/hmetal/out:
#   libhakios-metal.a      the core (phone/hmetal/upstream) + the NativeAdreno transport (phone/hmetal/redfin)
#   hakios-metal-probe     upstream's on-phone probe (tools/probe.cpp) on that transport, signed ad hoc
#   hmetal-host-tests.txt  the host tests' summary (both chips), run first: a red host suite stops the build
#   bench/                 hmbench, hmbench.metallib, HakiosMetal.bundle (bench/build.sh; README.md "hmbench")
#
#   phone/hmetal/build.sh              (WSL local build: tools-claude/localbuild sets up ios-cc; or on a Mac)
#   HMETAL_HOST_TESTS=0 phone/hmetal/build.sh   skip the host tests
#
# Inputs: work/userland/bin/ios-cc (clang -target arm64-apple-ios16.0 -isysroot <iPhoneOS SDK>), as phone/metal2/build.sh.
# MESA: without MESA=1 the library is built WITHOUT the in-process compiler (compileAir refuses by name; the probe's
# fixtures are precompiled ir3, so its compute and draw stages still run). MESA=1 builds it WITH the compiler (AIR ->
# NIR -> ir3, -DHAKIOS_METAL_MESA) against the two Mesa builds of mesa/build-mesa.sh (Mesa 25.0.7 + hakios' AIR front
# end), named by MESA_SRC MESA_BUILD (host: the host tests, compiler_test against tests/fixtures/ for the A635 and
# tests/fixtures/a620/ for the A620) and IOS_MESA_SRC IOS_MESA_BUILD (iOS: linked into the library and the probe);
# WORK/hmesa.env of build-mesa.sh holds all four (localbuild: `build hmesa`, then `build hmetal-mesa`).
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
	if [ -n "${MESA:-}" ]; then
		: "${MESA_SRC:?MESA=1 wants MESA_SRC/MESA_BUILD (mesa/build-mesa.sh)}" "${MESA_BUILD:?}"
		export MESA MESA_SRC MESA_BUILD
		# the fixture tools and the kernels' bitcode first, then the fixtures made newer than them: `make test MESA=1`
		# would otherwise rewrite every fixture with this host's Mesa and llvm-as and compare the compiler with itself
		bcs=$(for f in "$UP"/tools/kernels/*.ll; do echo "out/host/kernels/$(basename "$f" .ll).bc"; done)
		make -s -C "$UP" CXX="$HOSTCXX" out/host/ir3-kernel out/host/ir3_compiler $bcs > "$OUT/hmetal-host-tests.txt" 2>&1 ||
			{ cat "$OUT/hmetal-host-tests.txt"; exit 1; }
		touch "$UP"/tests/fixtures/*.hpp; [ ! -d "$UP/tests/fixtures/a620" ] || touch "$UP"/tests/fixtures/a620/*.hpp
	fi
	make -s -C "$UP" test CXX="$HOSTCXX" >> "$OUT/hmetal-host-tests.txt" 2>&1 || { cat "$OUT/hmetal-host-tests.txt"; exit 1; }
	make -s -C redfin/tests all CXX="$HOSTCXX" >> "$OUT/hmetal-host-tests.txt" 2>&1 || { cat "$OUT/hmetal-host-tests.txt"; exit 1; }
	grep -E "^==|^chip:|tests, " "$OUT/hmetal-host-tests.txt"
fi

# 2. the library: upstream's CORE list (Makefile), plus the transport; not upstream's iokit_transport.cpp (HakiosGPU)
[ -x "$CC" ] || { echo "no $CC (the iOS cross compiler: tools-claude/localbuild ios_env, or a Mac's phone/metal2 setup)"; exit 1; }
CXXFLAGS="-x c++ -std=c++17 -O2 -g -Wall -Wextra -Werror -DHAKIOS_METAL_NATIVE_ADRENO -I$UP/include -Iredfin -I../turnip"
LIBS=
if [ -n "${MESA:-}" ]; then
	: "${IOS_MESA_SRC:?MESA=1 wants IOS_MESA_SRC/IOS_MESA_BUILD (mesa/build-mesa.sh)}" "${IOS_MESA_BUILD:?}"
	S=$IOS_MESA_SRC B=$IOS_MESA_BUILD
	# Mesa's own -D set for its iOS objects (off ir3.c.o's ninja line, as upstream tools/compiler.mk reads it)
	DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' "$B/build.ninja" | grep -m1 ' ARGS = ' |
		tr ' ' '\n' | grep '^-D' | grep -v "[\"'\\\\]" | tr '\n' ' ')	# (not the quoted PACKAGE_* strings: no shell here to unquote them)
	[ -n "$DEFS" ] || { echo "no -D set for ir3.c.o in $B/build.ninja"; exit 1; }
	INC=""
	for d in "$S/src" "$S/include" "$S/src/freedreno" "$S/src/freedreno/common" "$S/src/compiler" "$S/src/compiler/nir" \
		"$S/src/compiler/air" "$S/src/gallium/include" "$B/src" "$B/src/compiler" "$B/src/compiler/nir" "$B/src/freedreno" \
		"$B/src/freedreno/common" "$B/src/freedreno/isa"; do INC="$INC -isystem $d"; done
	for a in src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a \
		src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a \
		src/freedreno/fdl/libfreedreno_layout.a src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a \
		src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a; do
		[ -f "$B/$a" ] && LIBS="$LIBS $B/$a"
	done
	LIBS="$LIBS -lz -lm"
	# the compiled-stage cache's key (src/compiler.cpp kCacheBuild), as upstream's Makefile makes it
	CB=$(cat "$UP/src/compiler.cpp" "$UP"/include/hakios-metal/*.hpp $LIBS 2>/dev/null | sha256sum | cut -c1-24)
	CXXFLAGS="$CXXFLAGS -DHAKIOS_METAL_MESA -DHAKIOS_METAL_CACHE_BUILD=\"$CB\" $DEFS $INC"
fi
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
"$CC" $CXXFLAGS -Wno-missing-field-initializers -o "$OUT/hakios-metal-probe" "$UP/tools/probe.cpp" -x none "$OUT/libhakios-metal.a" $LIBS -lc++ \
	-framework IOKit -framework CoreFoundation -framework IOSurface 2>&1 | grep -v "was built for newer 'iOS' version" || true
[ -f "$OUT/hakios-metal-probe" ] || exit 1
strip -x "$OUT/hakios-metal-probe" 2>/dev/null || true
codesign -f -s - "$OUT/hakios-metal-probe" 2>/dev/null || true
# 4. the benchmark (bench/build.sh: hmbench, its metallib, hakios' render plug-in as HakiosMetal.bundle); HMETAL_BENCH=0 skips
[ "${HMETAL_BENCH:-1}" = 0 ] || HMB_LIBS="$LIBS" sh bench/build.sh
(cd "$OUT" && shasum -a 256 libhakios-metal.a hakios-metal-probe 2>/dev/null || sha256sum libhakios-metal.a hakios-metal-probe) > "$OUT/SHA256SUMS"
echo "MESA=${MESA:-} ${MESA:+(the in-process compiler: $IOS_MESA_BUILD)}" > "$OUT/MESA.txt"
ls -la "$OUT"
