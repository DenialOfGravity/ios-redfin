#!/bin/bash
# The compiler fixtures for the A620, and a check of the A635 ones, with the host Mesa of build-mesa.sh.
#   phone/hmetal/mesa/gen-fixtures.sh OUTDIR        (MESA_SRC, MESA_BUILD in the environment: WORK/hmesa.env)
# For each tests/fixtures/NAME_air.hpp that has a tools/kernels/NAME.ll: the patched ir3_compiler (upstream
# tools/kernels.mk: out/host/ir3_compiler, ir3-dump.patch) run on the kernel's bitcode
#   --chip-id 0x06020000 -> OUTDIR/a620/NAME_air.hpp, namespace hakios::metal::kernels::a620 (tests/fixtures/a620/)
#   --chip-id 0x06030500 -> OUTDIR/a635/NAME_air.hpp, compared with the committed tests/fixtures/NAME_air.hpp
# (OUTDIR/a635-diff.txt: an empty diff means this Mesa compiles as hakios' did). Nothing in the tree is written.
set -e
OUTD=${1:?outdir}
: "${MESA_SRC:?}" "${MESA_BUILD:?}"
cd "$(dirname "$0")/../upstream"
mkdir -p "$OUTD/a620" "$OUTD/a635"
M="make -s MESA=1 MESA_SRC=$MESA_SRC MESA_BUILD=$MESA_BUILD CHIP_ID=0x06030500"
$M out/host/ir3_compiler
: > "$OUTD/a635-diff.txt"; : > "$OUTD/refused.txt"
n=0 same=0
for f in tests/fixtures/*_air.hpp; do
	name=$(basename "$f" _air.hpp)
	[ -f tools/kernels/$name.ll ] || continue
	$M out/host/kernels/$name.bc
	for c in a620:0x06020000 a635:0x06030500; do
		chip=${c%%:*} id=${c#*:}
		if ! out/host/ir3_compiler --chip-id $id -o "$OUTD/$chip/${name}_air.hpp" -s tools/kernels/$name.ll -n ${name}_air \
				out/host/kernels/$name.bc > "$OUTD/$chip/$name.listing" 2>&1; then
			echo "$chip $name: ir3_compiler failed: $(tail -1 "$OUTD/$chip/$name.listing")" >> "$OUTD/refused.txt"
			rm -f "$OUTD/$chip/${name}_air.hpp"
		fi
	done
	[ -f "$OUTD/a620/${name}_air.hpp" ] && sed -i 's/^namespace hakios::metal::kernels {$/namespace hakios::metal::kernels::a620 {/' "$OUTD/a620/${name}_air.hpp"
	n=$((n + 1))
	if [ -f "$OUTD/a635/${name}_air.hpp" ] && cmp -s "$OUTD/a635/${name}_air.hpp" "$f"; then same=$((same + 1))
	else diff -u "$f" "$OUTD/a635/${name}_air.hpp" >> "$OUTD/a635-diff.txt" 2>&1 || true; fi
done
echo "gen-fixtures: $n kernels; a620: $(ls "$OUTD"/a620/*_air.hpp 2>/dev/null | wc -l) fixtures; a635: $same of $n identical to the committed ones; refused: $(wc -l < "$OUTD/refused.txt")"
