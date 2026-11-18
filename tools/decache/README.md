# tools/decache: de-cache relinker for extracted dyld-cache dylibs

`decache.py` turns a dylib extracted from a dyld shared cache by DyldExtractor (`dyldex -e`) into a standalone
MH_DYLIB that dyld can load from disk next to a *different* shared cache. It was written for libBKDM1 (the Touch ID
module of biometrickitd), which is only in the iPhone 8 20H392 cache, so that it can run as `/usr/lib/libBKDM1.dylib`
on the redfin's iPhone X 20H392 cache. Notes: logs/research/fingerprint-bkdm1-relink-2026-11-18.md (project folder).

No Apple binaries live here. Inputs and outputs stay in the scratch tree (`~/fpr2/...`).

## Usage

Python 3 with capstone (4.x or 5.x), run isolated (`python3 -I`), e.g. from `~/fpr2/venv-dx`:

    # symbol addresses of the image's undefined symbols, in the source (8) and target (X) caches
    ipsw dyld symaddr SRC_DSC/dyld_shared_cache_arm64 --in syms.json -o e8-syms.json   # syms.json: [{"pattern": "^_name$"}, ...]
    ipsw dyld symaddr X_DSC/dyld_shared_cache_arm64   --in syms.json -o x-syms.json

    python3 -I decache.py relink --dx libBKDM1.dx.dylib --cache SRC_DSC_DIR --syms e8-syms.json \
        -o libBKDM1.unsigned.dylib --report relink-report.json
    cp libBKDM1.unsigned.dylib libBKDM1.dylib && ldid -S libBKDM1.dylib        # ad-hoc signature
    python3 -I decache.py verify libBKDM1.dylib --xsyms x-syms.json \
        --xcache X_DSC/dyld_shared_cache_arm64 --cache SRC_DSC_DIR --report verify-report.json

`relink` refuses to write anything if any pointer, slot or ObjC reference can't be explained (exit 1, errors listed).
`verify` exits 0 only if every check passes. Both take ~1-30 s.

## Method

- **Layout.** Every segment keeps its offset from the mach header (one shift by -__TEXT.vmaddr, a multiple of 4 KiB),
  so no instruction is rewritten: all `adrp` pairs, branches, `__dso_handle` (adrp to the header) and relative ObjC
  method lists stay valid. Segments are padded out to 16 KiB pages. __DATA_CONST is extended over the cache's shared GOT
  pool, so the pool slots the code loads (found by a capstone scan of every `adrp`+`ldr`) become the image's own
  `__DATA_CONST,__pool_got` slots at the same addresses. DyldExtractor's `__EXTRA_OBJC` segment can't get its own page
  (it starts 0xfa0 after __DATA_DIRTY), so it becomes a section `__DATA_DIRTY,__extra_objc`. Leading
  `__decache_pad` sections cover the alignment padding (llvm-objdump takes a segment's start from its first section).
  **File layout == VM layout** (fileoff == vmaddr - base for every segment): iOS 16 dyld reads sections such as
  `__objc_imageinfo` from the plain file mapping at `mh + sectaddr` before it maps segments (first phone test,
  2026-11-18: SIGSEGV in dyld at base+0x2ab0165 = imageinfo+5 with a compacted file). The gaps between segments are
  therefore zeros in the file (libBKDM1: 133 MB file, ~1.4 MB of content).
- **EH slots.** The unwind personality pointer and the LSDA type-table entries (pc-relative indirect) point at
  cache-GOT pool slots that no instruction loads; they are bound too (___objc_personality_v0,
  _OBJC_EHTYPE_$_NSException).
- **Completeness.** relink stops if any `adrp` to a page outside the image has no tracked `ldr`/`add` consumer
  (libBKDM1: 1,630 such adrp, all consumed; only `ldr` from the pool, 2,182 sites, 217 slots).
- **Fixups.** LC_DYLD_INFO_ONLY (classic rebase/bind opcodes, non-lazy binds only), which iOS 16 dyld accepts for
  arm64 (only arm64e new-deployment binaries require chained fixups). The source is arm64 (A11 caches), so there is no
  PAC to carry; arm64e input is refused.
- **Where pointers are.** Exactly the source cache's slide-info (v2) locations inside the image's data segments, not
  a value scan (a scan also hits int pairs such as list headers `entsize | count<<32`). For each location the value in
  the extracted file (v) and the cache's original value (v8) decide:
  inside the image -> rebase; into `__EXTRA_OBJC` -> a protocol copy is redirected to the image's own protocol of the
  same name, a foreign class copy is bound by the name of v8 (`_OBJC_CLASS_$_X` / `_OBJC_METACLASS_$_X`), anything else
  (selector/type strings) is rebased; zeroed by the extractor -> bound by v8's name (`__objc_empty_cache`), except the
  class+0x18 preopt-cache pointer, which stays 0 as ld emits it; outside the image -> bound by name
  (`___CFConstantStringClassReference`, `_OBJC_CLASS_$_NSConstant*`, `___kCFBoolean*`, `__NSConcreteGlobalBlock`).
- **GOT slot names.** From the image's own `__stubs` (indirect symbol table) where a stub uses the slot, otherwise
  from the slot's value in the source cache (following a cache stub when the value points at one). Library ordinals
  come from the image's undefined symbols (two-level namespace, weak flag from N_WEAK_REF). ld's original
  `__DATA_CONST,__got` (unused after the cache builder redirected loads to the pool) is bound too.
- **ObjC.** imageinfo OptimizedByDyld (0x8) cleared. Method lists lose the "uniqued+sorted" bits (0x8000000f ->
  0x8000000c, 0x1b -> 0x18), so the runtime registers selectors and searches them linearly (their order was sorted by
  the source cache's selector addresses). Small (relative) method list names are pointed at the image's own
  `__objc_selrefs` entry for the same selector (as ld emits them), which the runtime registers in `_read_images`.
  DyldExtractor's private selector slots in `__extra_objc` are not fixed up by objc4 on iOS 16 (phone test v2:
  `-[BiometricKitXPCServerMesa init]` was never found and the lookup fell through to the superclass), so they are
  dropped and zeroed. Protocol fixed-up/canonical
  flags are cleared. Everything in `__extra_objc` that nothing references any more (foreign class and protocol
  copies) is zeroed, so no source-cache address is left in the file.
- **Kept:** LC_ID_DYLIB, the LC_LOAD_DYLIBs, build/source version, symbol table (values shifted, sections renumbered),
  export trie (header-relative, copied as is), function starts. New LC_UUID (uuid5 of the old one).
  Dropped: MH_DYLIB_IN_CACHE, LC_DYLD_EXPORTS_TRIE (now in LC_DYLD_INFO_ONLY), any old code signature.

## Checks done by `verify`

MH_DYLIB arm64, in-cache flag clear, LC_ID, 17 dylibs, page-aligned ordered segments, sections inside segments;
own decoder for the rebase/bind opcodes: rebase targets inside the image, no location both rebased and bound;
every bind resolves by name in its ordinal's dylib or a dylib it re-exports in the target cache (re-export graph read
with `ipsw dyld macho`); every target dependency exists; no rebase/bind location holds a source-cache address and
every 8-byte value left in __DATA* in the source cache's address range is an int32 pair; GOT slots all bound and zero
in the file; fileoff == vmaddr - base for every segment/section; every load-command blob inside the file and
__LINKEDIT; personality/LSDA slots bound; ObjC walk (classes, metaclasses, isa/superclass/cache binds, method names/types readable, IMPs in
__text, every relative method name via __objc_selrefs, protocols, selrefs). Independent cross-checks: `llvm-objdump --macho --rebase/--bind/--exports-trie`,
`ipsw class-dump`, `ipsw macho disass`, `ldid -S`, `ipsw macho info --sig`.

## Limits

- Slide info v2 only (arm64 caches up to iOS 17). Opcode fixups only. Images with categories, Swift, TLV or
  `__la_symbol_ptr` were not needed for libBKDM1 and aren't handled (the tool stops on unexplained pointers).
- The runtime check (dyld load, ObjC realization) can only be done on the phone.
