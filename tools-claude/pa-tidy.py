#!/usr/bin/env python3
"""pa-tidy.py: find what can be archived in the project folder and WSL, and (only with --apply) move it to an attic.

  pa tidy [AREA...] [--days N] [--keep-recent K] [--json]     dry run (default): what would move, why the rest stays
  pa tidy AREA --apply                                         move AREA's candidates (never deletes anything)

AREAS: images (PF/<name>/redfin-<name>.img folders), builds (PF/builds/<target>/<dir>), logs (PF/logs top level and
logs/hmetal/*), worktrees (~/wt/*, ~/redfin-work/*), scratch (~/*scratch* and similar), all (default).

Kept, whatever its age:
  - anything named in the keep sources: agent/cycle-base.txt, agent/last-boot.txt, the last 150 lines of KNOWN-GOOD.txt,
    the state manifest (state/manifest-ref.txt), state/temp-overrides.json, tools-claude/tidy-keep.txt (one name or
    glob per line; the owner's list), and the README.txt of every kept image (its base image and kernel dir);
  - anything modified in the last --days (default 7) days;
  - the --keep-recent (default 10 images, 5 builds per target) newest of each kind;
  - worktrees with uncommitted changes or unpushed commits, or whose branch is not merged into origin/redfin-int
    or the newest origin/ws-int-k* and was touched in the last --days.
--apply moves PF items to PF/attic/<date>/<area>/ and WSL items to ~/attic/<date>/<area>/, writes INDEX.tsv there
(original path, size, reason) and, for worktrees, runs `git worktree prune` (the branches stay). To undo: move back.
Report: PF/logs/tidy/<area>-<time>.txt (+ .json).
"""
import argparse, datetime, fnmatch, json, os, re, subprocess, sys, time

PF = os.environ.get('PF', '/mnt/d/ClaudeProjects/iOS_Pixel5')
HOME = os.path.expanduser('~')
REPO = os.path.join(HOME, 'ios-redfin')
NOW = time.time()
DAY = 86400


def du(path):
    tot = 0
    for root, dirs, files in os.walk(path, onerror=lambda e: None):
        for f in files:
            try:
                tot += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return tot


def newest_mtime(path, limit=2000):
    """newest mtime among the dir itself and up to `limit` entries below it"""
    m, n = os.lstat(path).st_mtime, 0
    for root, dirs, files in os.walk(path, onerror=lambda e: None):
        for f in files + dirs:
            try:
                m = max(m, os.lstat(os.path.join(root, f)).st_mtime)
            except OSError:
                pass
            n += 1
            if n >= limit:
                return m
    return m


def hsize(b):
    for u in ('B', 'K', 'M', 'G', 'T'):
        if b < 1024 or u == 'T':
            return f'{b:.0f}{u}' if u in 'BK' else f'{b:.1f}{u}'
        b /= 1024


def read(p, tail=None):
    try:
        t = open(p, errors='replace').read()
    except OSError:
        return ''
    return '\n'.join(t.splitlines()[-tail:]) if tail else t


def git(*a, cwd=REPO):
    r = subprocess.run(['git', *a], cwd=cwd, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


def keep_sources():
    blob = [read(f'{PF}/agent/cycle-base.txt'), read(f'{PF}/agent/last-boot.txt'), read(f'{PF}/KNOWN-GOOD.txt', 150),
            read(f'{PF}/state/temp-overrides.json')]
    ref = read(f'{PF}/state/manifest-ref.txt').strip()
    if ref.startswith('git:'):
        r, p = ref[4:].split(':', 1)
        blob.append(git('show', f'{r}:{p}'))
    elif ref:
        blob.append(read(os.path.join(PF, ref)))
    globs = [l.strip() for l in read(f'{PF}/tools-claude/tidy-keep.txt').splitlines() if l.strip() and not l.startswith('#')]
    return '\n'.join(blob), globs


class Area:
    def __init__(self, name, a, blob, globs):
        self.name, self.a, self.blob, self.globs = name, a, blob, globs
        self.items = []   # dicts: path, size, mtime, action move|keep, why

    def named(self, name):
        if any(fnmatch.fnmatch(name, g) for g in self.globs):
            return 'tidy-keep.txt'
        if re.search(r'(?<![A-Za-z0-9_.-])' + re.escape(name) + r'(?![A-Za-z0-9_-])', self.blob):
            return 'named in a keep source'
        return None

    def add(self, path, why=None, size=None, mtime=None):
        mtime = mtime if mtime is not None else newest_mtime(path)
        if why is None and NOW - mtime < self.a.days * DAY:
            why = f'changed in the last {self.a.days} days'
        self.items.append({'path': path, 'size': du(path) if size is None else size, 'mtime': mtime,
                           'action': 'keep' if why else 'move', 'why': why or f'older than {self.a.days} days, not named'})


def image_dirs():
    out = []
    for e in os.scandir(PF):
        if e.is_dir() and os.path.isfile(os.path.join(e.path, f'redfin-{e.name}.img')):
            out.append(e)
    return out


def area_images(a, blob, globs):
    ar = Area('images', a, blob, globs)
    dirs = sorted(image_dirs(), key=lambda e: e.stat().st_mtime, reverse=True)
    recent = {e.name for e in dirs[:a.keep_recent]}
    keep_names = set()
    for e in dirs:
        if ar.named(e.name) or e.name in recent or NOW - e.stat().st_mtime < a.days * DAY:
            keep_names.add(e.name)
    # bases of kept images stay too (repacks need them)
    for n in list(keep_names):
        m = re.search(r'^base\s+(\S+)', read(f'{PF}/{n}/README.txt'), re.M)
        if m:
            keep_names.add(m.group(1))
    for e in dirs:
        n = e.name
        why = ar.named(n) or ('one of the newest' if n in recent else None) or \
            (f'changed in the last {a.days} days' if NOW - e.stat().st_mtime < a.days * DAY else None) or \
            ('base of a kept image' if n in keep_names else None)
        ar.add(e.path, why, mtime=e.stat().st_mtime)
    # kernels named by kept images, for the builds area
    a.kept_kernels = set()
    for n in keep_names:
        m = re.search(r'^kernel\s+(builds/\S+?)/', read(f'{PF}/{n}/README.txt'), re.M)
        if m:
            a.kept_kernels.add(m.group(1).split('/')[-1])
    return ar


def area_builds(a, blob, globs):
    ar = Area('builds', a, blob, globs)
    if not hasattr(a, 'kept_kernels'):
        area_images(a, blob, globs)
    root = f'{PF}/builds'
    for t in sorted(os.listdir(root)):
        tp = os.path.join(root, t)
        if not os.path.isdir(tp) or t in ('logs',):
            continue
        ds = sorted([e for e in os.scandir(tp) if e.is_dir()], key=lambda e: e.stat().st_mtime, reverse=True)
        for i, e in enumerate(ds):
            why = ar.named(e.name) or ('kernel of a kept image' if e.name in a.kept_kernels else None) or \
                (f'newest {a.keep_builds} {t}' if i < a.keep_builds else None)
            if not why and os.path.exists(os.path.join(e.path, 'DO-NOT-BOOT.txt')):
                why = None  # a failed build is a fine candidate
            ar.add(e.path, why, mtime=e.stat().st_mtime)
    return ar


def area_logs(a, blob, globs):
    ar = Area('logs', a, blob, globs)
    root = f'{PF}/logs'
    for e in sorted(os.scandir(root), key=lambda e: e.name):
        if e.name == 'hmetal':
            for h in sorted(os.scandir(e.path), key=lambda x: x.name):
                ar.add(h.path, ar.named(h.name))
        elif e.name in ('tidy', 'mon', 'research'):
            ar.add(e.path, f'logs/{e.name}: always kept')
        else:
            ar.add(e.path, ar.named(e.name))
    return ar


def wt_dirs():
    out = []
    for base in (f'{HOME}/wt', f'{HOME}/redfin-work'):
        if os.path.isdir(base):
            out += [e.path for e in os.scandir(base) if e.is_dir()]
    return sorted(out)


def area_worktrees(a, blob, globs):
    ar = Area('worktrees', a, blob, globs)
    git('fetch', '-q', 'origin')
    ints = [r for r in git('for-each-ref', '--sort=-committerdate', '--format=%(refname:short)', 'refs/remotes/origin').split()
            if re.fullmatch(r'origin/ws-int-k\d+', r)][:1] + ['origin/redfin-int']
    for p in wt_dirs():
        why = None
        if os.path.ismount(p) or not os.path.exists(os.path.join(p, '.git')):
            ar.items.append({'path': p, 'size': 0, 'mtime': os.lstat(p).st_mtime, 'action': 'keep',
                             'why': 'mount point' if os.path.ismount(p) else 'not a git checkout: check by hand'})
            continue
        br = git('rev-parse', '--abbrev-ref', 'HEAD', cwd=p)
        dirty = git('status', '--porcelain', '--untracked-files=no', cwd=p)
        unpushed = git('log', '--oneline', '@{u}..', cwd=p) if git('rev-parse', '@{u}', cwd=p) else \
            git('log', '--oneline', 'HEAD', '--not', '--remotes', cwd=p)
        merged = any(subprocess.run(['git', 'merge-base', '--is-ancestor', 'HEAD', i], cwd=p,
                                    capture_output=True).returncode == 0 for i in ints)
        last = float(git('log', '-1', '--format=%ct', cwd=p) or 0)
        if dirty:
            why = 'uncommitted changes'
        elif unpushed:
            why = 'unpushed commits'
        elif ar.named(os.path.basename(p)) or (br and ar.named(br)):
            why = 'named in a keep source'
        elif not merged and NOW - last < a.days * DAY:
            why = f'branch {br} not merged, active in the last {a.days} days'
        ar.items.append({'path': p, 'size': 0 if why else du(p), 'mtime': last, 'action': 'keep' if why else 'move',
                         'why': why or (f'branch {br} merged into {",".join(ints)}' if merged else f'branch {br} pushed, idle > {a.days} days'),
                         'branch': br})
    return ar


SCRATCH = re.compile(r'(scratch|^k\d|^k-work|^k\d+-work|^fpr|^hmi\d|^hm[a-z]+-|^int\d|^scr|^wt-|^rf$|^upstream-scan$|^qcdis$)')


def area_scratch(a, blob, globs):
    ar = Area('scratch', a, blob, globs)
    for e in sorted(os.scandir(HOME), key=lambda e: e.name):
        if e.is_dir() and not e.name.startswith('.') and SCRATCH.search(e.name) and e.name not in ('wt',) \
                and not os.path.ismount(e.path) and not os.path.exists(os.path.join(e.path, '.git')):
            ar.add(e.path, ar.named(e.name))
    return ar


AREAS = {'images': area_images, 'builds': area_builds, 'logs': area_logs, 'worktrees': area_worktrees,
         'scratch': area_scratch}


def report(ar, out):
    mv = [i for i in ar.items if i['action'] == 'move']
    kp = [i for i in ar.items if i['action'] == 'keep']
    lines = [f'== {ar.name}: {len(ar.items)} items, {hsize(sum(i["size"] for i in ar.items))}; '
             f'would move {len(mv)} ({hsize(sum(i["size"] for i in mv))}), keep {len(kp)} ({hsize(sum(i["size"] for i in kp))})']
    reasons = {}
    for i in kp:
        r = re.sub(r'\d+', 'N', i['why']) if i['why'].startswith('branch') else i['why']
        reasons[r] = reasons.get(r, 0) + 1
    lines.append('   kept because: ' + '; '.join(f'{k} {v}' for k, v in sorted(reasons.items(), key=lambda x: -x[1])))
    lines.append('   largest to move:')
    for i in sorted(mv, key=lambda i: -i['size'])[:12]:
        lines.append(f'     {hsize(i["size"]):>7}  {datetime.date.fromtimestamp(i["mtime"]) if i["mtime"] else "?"}  {i["path"]}')
    txt = '\n'.join(lines)
    os.makedirs(f'{PF}/logs/tidy', exist_ok=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    with open(f'{PF}/logs/tidy/{ar.name}-{stamp}.txt', 'w') as f:
        f.write(txt + '\n\nall items:\n' + '\n'.join(f'{i["action"]}\t{hsize(i["size"])}\t{i["path"]}\t{i["why"]}' for i in ar.items) + '\n')
    json.dump(ar.items, open(f'{PF}/logs/tidy/{ar.name}-{stamp}.json', 'w'), indent=0)
    out.write(txt + f'\n   full list: logs/tidy/{ar.name}-{stamp}.txt\n')


def apply(ar):
    day = datetime.date.today().isoformat()
    mv = [i for i in ar.items if i['action'] == 'move']
    base = f'{PF}/attic/{day}/{ar.name}' if ar.name in ('images', 'builds', 'logs') else f'{HOME}/attic/{day}/{ar.name}'
    os.makedirs(base, exist_ok=True)
    with open(f'{base}/INDEX.tsv', 'a') as idx:
        for i in mv:
            rel = os.path.relpath(i['path'], PF if base.startswith(PF) else HOME)
            dst = os.path.join(base, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            r = subprocess.run(['mv', '-n', i['path'], dst])
            idx.write(f'{i["path"]}\t{i["size"]}\t{i["why"]}\t{"ok" if r.returncode == 0 else "FAILED"}\n')
    if ar.name == 'worktrees':
        git('worktree', 'prune')
    print(f'{ar.name}: moved {len(mv)} items to {base} (INDEX.tsv)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('areas', nargs='*', default=['all'])
    ap.add_argument('--days', type=int, default=7)
    ap.add_argument('--keep-recent', type=int, default=10)
    ap.add_argument('--keep-builds', type=int, default=5)
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    names = list(AREAS) if a.areas == ['all'] else a.areas
    bad = [n for n in names if n not in AREAS]
    if bad:
        sys.exit(f'unknown area {bad}: {" ".join(AREAS)}')
    if a.apply and len(names) != 1:
        sys.exit('--apply takes exactly one AREA (check its dry run first)')
    blob, globs = keep_sources()
    for n in names:
        ar = AREAS[n](a, blob, globs)
        report(ar, sys.stdout)
        sys.stdout.flush()
        if a.apply:
            apply(ar)


if __name__ == '__main__':
    main()
