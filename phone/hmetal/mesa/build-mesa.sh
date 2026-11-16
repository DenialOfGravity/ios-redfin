#!/bin/bash
# Mesa 25.0.7 with hakios' AIR front end, twice: for this build host (the host tests' `make test MESA=1` and the
# fixtures' ir3_compiler) and for iOS 16 arm64 (the compiler that phone/hmetal/build.sh MESA=1 links into
# libhakios-metal.a and the probe). Only the static libraries hakios-metal links are built (tools/kernels.mk
# MESA_ARCHIVE_PATHS + src/compiler/air/libair.a), plus the host's ir3_compiler; no driver, no dylib.
#
#   phone/hmetal/mesa/build-mesa.sh WORK TARBALL      (normally from tools-claude/localbuild build.sh build_hmesa)
#
# WORK keeps everything between runs (incremental): src-host/, src-ios/ (the tarball unpacked and patched; re-made
# only when the patches here change), build-host/, build-ios/ (meson), libdrm/ (host only: gallium freedreno's
# configure wants libdrm_freedreno, which the build host may not have; a static one is built here), and
# WORK/hmesa.env: MESA_SRC MESA_BUILD IOS_MESA_SRC IOS_MESA_BUILD for phone/hmetal/build.sh.
# Patches (this directory; hakios-xnu-kernel 2631f5d tools/mesa, MIT): host = air-*.patch + ir3-*.patch (as hakios'
# tools/air/mk-host.sh) + glibc-call-once.py; iOS = every *.patch + hakios-drm/ (as hakios' tools/mk-mesa.sh), the
# AIR front end air/ copied to src/compiler/air in both.
# iOS: the cross compiler is the build host's xcrun (tools-claude/localbuild bin/xcrun, or a Mac's), the cross file
# made as tools/build-turnip-ios.sh makes Turnip's. Needs meson, ninja, bison, flex, python3 with mako/pyyaml.
# PARTS=host,ios (default both).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:?work dir}; TAR=${2:?mesa-25.0.7.tar.xz}
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
LIBDRM=2.4.124
PARTS=${PARTS:-host,ios}
mkdir -p "$W"
echo "$SHA256  $TAR" | sha256sum -c --quiet

# the archives hakios-metal links (tools/kernels.mk MESA_ARCHIVE_PATHS) and the front end's
ARCHIVES="src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a src/freedreno/isa/libir3decode.a
 src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/freedreno/fdl/libfreedreno_layout.a
 src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a
 src/c11/impl/libmesa_util_c11.a src/compiler/air/libair.a"

# have BUILD ARCHIVES...: those the build defines (libmesa_util_sse41.a is x86 only in some versions)
have() { local b=$1 t; shift; local all; all=$(ninja -C "$b" -t targets all 2>/dev/null | cut -d: -f1)
	for t in "$@"; do grep -qxF "$t" <<< "$all" && echo "$t" || echo "hmesa: (no $t in $b)" >&2; done; }

# prep KIND PATCHES...: WORK/src-KIND/mesa-VERSION, the tarball with PATCHES applied; kept while they are the same
prep() {
	local kind=$1; shift
	local d=$W/src-$kind stamp
	stamp=$( { cat "$@"; tar cf - -C "$HERE" air hakios-drm glibc-call-once.py; } | sha256sum | cut -c1-16)
	if [ "$(cat "$d/mesa-$VERSION/.hmesa-patches" 2>/dev/null)" != "$stamp" ]; then
		echo "hmesa: unpacking Mesa $VERSION for $kind (patches $stamp)"
		rm -rf "$d" "$W/build-$kind"; mkdir -p "$d"
		tar xJf "$TAR" -C "$d"
		local p; for p in "$@"; do patch -s --no-backup-if-mismatch -d "$d/mesa-$VERSION" -p1 < "$p"; done
		rm -rf "$d/mesa-$VERSION/src/compiler/air"; cp -r "$HERE/air" "$d/mesa-$VERSION/src/compiler/air"
		if [ "$kind" = ios ]; then cp -r "$HERE/hakios-drm" "$d/mesa-$VERSION/src/hakios-drm"
		else python3 -I "$HERE/glibc-call-once.py" "$d/mesa-$VERSION"; fi
		echo "$stamp" > "$d/mesa-$VERSION/.hmesa-patches"
	fi
}

# options both builds share (hakios' mk-mesa.sh / mk-host.sh): gallium freedreno so that src/freedreno and the
# ir3_compiler are configured; nothing optional
COMMON="-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled
 -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dshader-cache=disabled
 -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled
 -Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dzstd=disabled
 -Dxmlconfig=disabled -Dexpat=disabled"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

case ",$PARTS," in *,host,*)
	# libdrm (static, freedreno only) for the host configure
	L=$W/libdrm
	if [ ! -f "$L/prefix/lib/pkgconfig/libdrm_freedreno.pc" ]; then
		mkdir -p "$L"
		[ -s "$L/libdrm-$LIBDRM.tar.xz" ] || curl -fsSL -o "$L/libdrm-$LIBDRM.tar.xz" https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM.tar.xz
		sha256sum "$L/libdrm-$LIBDRM.tar.xz" | tee "$L/SHA256"
		rm -rf "$L/libdrm-$LIBDRM" "$L/build"; tar xJf "$L/libdrm-$LIBDRM.tar.xz" -C "$L"
		meson setup "$L/build" "$L/libdrm-$LIBDRM" --prefix="$L/prefix" --libdir=lib --default-library=static \
			-Dfreedreno=enabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled -Dintel=disabled -Dvmwgfx=disabled \
			-Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled -Dtegra=disabled -Dcairo-tests=disabled \
			-Dman-pages=disabled -Dvalgrind=disabled -Dtests=false > "$L/setup.log" 2>&1 || { tail -30 "$L/setup.log"; exit 1; }
		ninja -C "$L/build" install > "$L/build.log" 2>&1 || { tail -30 "$L/build.log"; exit 1; }
	fi
	export PKG_CONFIG_PATH="$L/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	prep host "$HERE"/air-*.patch "$HERE"/ir3-*.patch
	B=$W/build-host
	[ -f "$B/build.ninja" ] || CC=${HOST_CC:-cc} CXX=${HOST_CXX:-c++} meson setup "$B" "$W/src-host/mesa-$VERSION" \
		--buildtype=debugoptimized $COMMON > "$W/setup-host.log" 2>&1 || { tail -40 "$W/setup-host.log"; exit 1; }
	ninja -C "$B" $(have "$B" $ARCHIVES) src/gallium/drivers/freedreno/ir3_compiler > "$W/ninja-host.log" 2>&1 ||
		{ grep -E "error|FAILED" "$W/ninja-host.log" | head -30; exit 1; }
	echo "hmesa: host build ok ($B)";;
esac

case ",$PARTS," in *,ios,*)
	prep ios "$HERE"/*.patch
	B=$W/build-ios
	sdk=$(xcrun --sdk iphoneos --show-sdk-path)
	cross=$W/ios16.cross
	flags="'-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk', '-fstack-protector-strong'"
	cat > "$cross.new" <<EOF
[binaries]
c = '$(xcrun --sdk iphoneos --find clang)'
cpp = '$(xcrun --sdk iphoneos --find clang++)'
objc = '$(xcrun --sdk iphoneos --find clang)'
ar = '$(command -v llvm-ar || xcrun --sdk iphoneos --find ar)'
ranlib = '$(command -v llvm-ranlib || xcrun --sdk iphoneos --find ranlib)'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$flags]
cpp_args = [$flags, '-fuse-cxa-atexit']
objc_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]
objc_link_args = [$flags]
EOF
	# a changed cross file (another SDK, other wrappers) invalidates meson's cached checks: configure from scratch
	cmp -s "$cross.new" "$cross" || { mv "$cross.new" "$cross"; rm -rf "$B"; }
	rm -f "$cross.new"
	[ -f "$B/build.ninja" ] || meson setup "$B" "$W/src-ios/mesa-$VERSION" --cross-file "$cross" --wrap-mode=nofallback \
		--buildtype=release -Db_ndebug=true -Dosmesa=true -Dshared-glapi=enabled $COMMON > "$W/setup-ios.log" 2>&1 ||
		{ tail -40 "$W/setup-ios.log"; exit 1; }
	ninja -C "$B" $(have "$B" $ARCHIVES) > "$W/ninja-ios.log" 2>&1 || { grep -E "error|FAILED" "$W/ninja-ios.log" | head -30; exit 1; }
	echo "hmesa: ios build ok ($B)";;
esac

cat > "$W/hmesa.env" <<EOF
MESA_VERSION=$VERSION
MESA_SRC=$W/src-host/mesa-$VERSION
MESA_BUILD=$W/build-host
IOS_MESA_SRC=$W/src-ios/mesa-$VERSION
IOS_MESA_BUILD=$W/build-ios
EOF
# the manifest: what was built from what
{
	echo "mesa $VERSION $SHA256"; echo "patches host $(cat "$W/src-host/mesa-$VERSION/.hmesa-patches" 2>/dev/null) ios $(cat "$W/src-ios/mesa-$VERSION/.hmesa-patches" 2>/dev/null)"
	cat "$W/libdrm/SHA256" 2>/dev/null | sed 's|^|libdrm (host) |'
	for k in host ios; do for a in $ARCHIVES; do
		f=$W/build-$k/$a; [ -f "$f" ] && echo "$k $(sha256sum "$f" | cut -c1-16) $(stat -c %s "$f") $a"
	done; done
	[ -x "$W/build-host/src/gallium/drivers/freedreno/ir3_compiler" ] && echo "host ir3_compiler"
} > "$W/MANIFEST.txt"
cat "$W/hmesa.env"
