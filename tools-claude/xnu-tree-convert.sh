#!/bin/bash
# Used once on 2026-11-18 (redfin-int d07a2b7 -> xnu-main af6d91e). Env: PRISTINE, DXB, REDFIN_MD.
# convert.sh SRC_REPO REF OUT: build xnu-redfin history from ios-redfin's xnu/patches at REF.
# Replays tools-claude/localbuild/xnu.sh step 1 (pristine xnu-8796.141.3, darwin-xnu-build seds, staging scripts,
# patches in C order with patch-skip-allow.txt, Linux source fixes), one commit per step.
set -euo pipefail
SRC=$1 REF=$2 OUT=$3
PRISTINE=${PRISTINE:?apple xnu-8796.141.3 checkout} DXB=${DXB:?darwin-xnu-build at 6639065}
REFSHA=$(git -C "$SRC" rev-parse --short=7 "$REF")
export LC_ALL=C MACOS_VERSION=13.5 MACHINE_CONFIG=SWEET
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT"; git init -q -b xnu-main
git config user.name "DenialOfGravity"; git config user.email "DenialOfGravity@users.noreply.github.com"
git config core.autocrlf false
commit() {	# commit SUBJECT BODY [AUTHOR DATE]
	git add -A -f .
	if [ -n "${3:-}" ]; then GIT_AUTHOR_NAME="${3%%|*}" GIT_AUTHOR_EMAIL="${3#*|}" GIT_AUTHOR_DATE="$4" \
		git commit -q --allow-empty -m "$1" -m "$2"
	else git commit -q --allow-empty -m "$1" -m "$2"; fi
}
# 1. pristine
git -C "$PRISTINE" archive HEAD | tar x
commit "xnu-8796.141.3" "Apple's XNU as tagged in apple-oss-distributions/xnu (1b191cb, macOS 13.5). APSL 2.0." \
	"Apple OSS Distributions|91980991+AppleOSSDistributions@users.noreply.github.com" "$(git -C "$PRISTINE" log -1 --format=%aI)"
# 2. darwin-xnu-build seds (as in localbuild/xnu.sh)
sed -i 's|^AVAILABILITY_PL="${SDKROOT}/${DRIVERKITROOT}|AVAILABILITY_PL="${FAKEROOT_DIR}|g' bsd/sys/make_symbol_aliasing.sh
sed -i 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig
sed -i 's|^LDFLAGS_KERNEL_SDK	= -L$(SDKROOT).*|LDFLAGS_KERNEL_SDK	= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel|g' makedefs/MakeInc.def
sed -i 's|^\(INCFLAGS_SDK[[:space:]]*=\) -I$(SDKROOT)/usr/local/include/\(kernel\)\{0,1\}|\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel|' makedefs/MakeInc.def
commit "build: darwin-xnu-build's fakeroot paths" "The four seds of blacktop/darwin-xnu-build's patches() (6639065), as localbuild/xnu.sh runs them."
# 3. staging scripts (TargetConditionals.h is the macOS SDK's: not committed, the build copies it in)
STUB=$(mktemp -d); mkdir -p "$STUB/sdk/usr/include"; : > "$STUB/sdk/usr/include/TargetConditionals.h"
printf '#!/bin/sh\necho %s/sdk\n' "$STUB" > "$STUB/xcrun"; chmod +x "$STUB/xcrun"
for s in "$SRC"/xnu/patches/*.sh; do
	b=$(basename "$s")
	PATH="$STUB:$PATH" bash "$s" >/dev/null
	find . -name TargetConditionals.h -newer "$STUB/xcrun" -path './bsd/*' -delete
	p=xnu/patches/$b
	a=$(git -C "$SRC" log "$REF" --diff-filter=A --format='%an|%ae%x09%aI%x09%h %s' -- "$p" | tail -1)
	commit "${b%.sh}" "Output of ios-redfin $p at $REFSHA (added in ${a##*$'\t'}).
TargetConditionals.h from the macOS SDK is copied in at build time, not kept here." "${a%%$'\t'*}" "$(echo "$a" | cut -f2)"
done
printf '# from the macOS SDK at build time\nbsd/pthread/sweet_libpthread/TargetConditionals.h\nbsd/sweet_hfs/TargetConditionals.h\n' > .gitignore.sdk
# 4. patches, C order (ours 0001.. then dxb's), skip-allow as in the build
mkdir -p "$OUT.p"; rm -f "$OUT.p"/*; cp "$DXB"/patches/*.patch "$SRC"/xnu/patches/*.patch "$OUT.p"/
rm .gitignore.sdk
napp=0 nskip=0
for pp in "$OUT.p"/*.patch; do
	b=$(basename "$pp")
	if git apply --check "$pp" 2>/dev/null; then git apply "$pp"
	elif grep -qxF "$b" "$SRC/tools-claude/localbuild/patch-skip-allow.txt"; then echo "skip $b"; nskip=$((nskip+1)); continue
	else echo "FAIL $b"; exit 1; fi
	napp=$((napp+1))
	if [ -f "$SRC/xnu/patches/$b" ]; then
		p=xnu/patches/$b
		a=$(git -C "$SRC" log "$REF" --diff-filter=A --format='%an|%ae%x09%aI%x09%h %s' -- "$p" | tail -1)
		l=$(git -C "$SRC" log "$REF" -1 --format='%h %s' -- "$p")
		commit "${b%.patch}" "From ios-redfin $p at $REFSHA.
Added in ${a##*$'\t'}
Last changed in $l" "${a%%$'\t'*}" "$(echo "$a" | cut -f2)"
	else
		commit "dxb: ${b%.patch}" "blacktop/darwin-xnu-build patches/$b (6639065)." "blacktop|blacktop@users.noreply.github.com" "2026-09-05T15:59:54-06:00"
	fi
done
echo "applied $napp skipped $nskip"
# 5. Linux / upstream-clang source fixes (the python block of localbuild/xnu.sh, idempotent there)
sed -n "/python3 - <<'PY'/,/^PY$/p" "$SRC/tools-claude/localbuild/xnu.sh" | sed '1d;$d' > "$STUB/fix.py"
python3 "$STUB/fix.py"
commit "build: upstream clang and Linux host fixes" "The source edits of localbuild/xnu.sh (after UniXNU scripts/linux/prepare-xnu.py): makesyscalls.sh mawk escape,
arm64_isa_compatibility.h, kextsymboltool lseek on /dev/null, kalloc.h type check in C++."
printf 'bsd/pthread/sweet_libpthread/TargetConditionals.h\nbsd/sweet_hfs/TargetConditionals.h\n' >> .git/info/exclude
git log --oneline | wc -l
# 6. repo notes
cp "${REDFIN_MD:?REDFIN.md to add}" REDFIN.md
printf '\n# redfin: the macOS SDK header the staged sources need, copied in by the build\nbsd/pthread/sweet_libpthread/TargetConditionals.h\nbsd/sweet_hfs/TargetConditionals.h\n' >> .gitignore
commit "REDFIN.md: what this tree is and how to work in it" "Start of the git-tree workflow; ios-redfin xnu/patches at $REFSHA is the source of every commit before this one."
git log --oneline | wc -l
