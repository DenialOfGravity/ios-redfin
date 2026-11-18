# xnu-main (ios-redfin)

The XNU kernel of the Pixel 5 (redfin) iOS port as a git tree: Apple's xnu-8796.141.3 with every change as a commit.
It lives on ios-redfin's branch `xnu-main` (its own history, unrelated to the other branches) and replaces the patch
series in `xnu/patches/` on `redfin-int`. Kernel work branches start from `xnu-main` (name them `xnu/<topic>`).

## History

1. `xnu-8796.141.3`: Apple's source (APSL 2.0), as tagged in apple-oss-distributions/xnu.
2. `build: darwin-xnu-build's fakeroot paths`: the four seds of blacktop/darwin-xnu-build (MIT).
3. `0019`/`0020`/`0026-stage-*`: the output of ios-redfin's staging scripts (libpthread-514.100.2, BearSSL,
   hfs-627.100.6; each keeps its own licence).
4. One commit per ios-redfin patch, `0001` ... `1256`, in the order the build applied them. The subject is the old
   patch name; the body names the ios-redfin commits that added and last changed it; the author and date are those
   of the commit that added it. `0160-sunfish-iomfb-log-async` and darwin-xnu-build's `TrustCache`,
   `iobuffermemd_monterey`, `skywalk_sonoma` and `syntax_checker_sonoma` never applied and are not here.
5. `dxb: *`: darwin-xnu-build's patches that apply to 13.5's xnu.
6. `build: upstream clang and Linux host fixes`: the source edits the Linux build used to make each time.

The tree at the commit that adds this file is the source of the Touch ID baseline kernel (ios-redfin ec02376).

## Working in it

- `xnu-main` is the kernel the phone runs as its baseline. New work goes on a branch, one commit per change.
  Fix a change in place (`git commit --fixup` + `git rebase --autosquash`) while it's on a branch; once it's on
  `xnu-main`, fix forward with a new commit.
- Subject: `area: what it does`, naming the switch that gates it (`... only with sweetfoo=1`, `kern.sweet_foo`).
  New behaviour is off by default.
- Code taken from another project gets an `Imported-from: <repo> <commit>` line and keeps its author.
- Build on the PC: `pa build xnu-tree BRANCH|SHA` (push first; default `xnu-main`), or in a cycle
  `pa cycle NAME tree:BRANCH ...`. Same outputs as `pa build xnu`
  (`kernel.development.sweet.gz`, `COMPILER.txt`, `KLOG_PA.txt`, the ISA check).
- `TargetConditionals.h` (macOS SDK) is not in git: the build copies it into `bsd/pthread/sweet_libpthread/` and
  `bsd/sweet_hfs/`.

## Licences

Apple's files and changes to them: APSL 2.0 (`APPLE_LICENSE`). Files added by ios-sweet and the redfin port: MIT,
as in ios-redfin's `LICENSE` and `CREDITS.md`. Staged sources keep their own licence files.
No iOS files, vendor firmware or device dumps go in this repository.
