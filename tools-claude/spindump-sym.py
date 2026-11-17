#!/usr/bin/env python3
"""spindump-sym.py REPORT [-o OUT] [--kernel KDIR] [--top N]: symbolicate an on-phone spindump report.

The phone's spindump (/usr/sbin/spindump PROC SECS MS -o FILE) samples every process's user + kernel stacks but
prints shared-cache frames as `??? (QuartzCore + 2651160) [0x19a39f418]`: the device has no local symbols. This
rewrites them as `CA::Render::... + 28 (QuartzCore + 2651160)` using the dyld shared cache on the PC
(~/redfin-work/dsc, the phone's iOS 16.7 cache; per-image symbol tables from `ipsw dyld macho IMG --symbols`, cached
in PF/work/syms/), and kernel frames `__kernel__ + OFF` from the kernel Mach-O (--kernel builds/xnu/<dir>, default:
the last-booted image's kernel). Images outside the cache (backboardd, app binaries) stay as they are.
--top N adds a summary: for the focus process, the N heaviest leaf frames of threads that were not idle.

  python3 tools-claude/spindump-sym.py logs/research/mtltrace/sd2.spindump.txt -o sd2.sym.txt --top 25
"""
import argparse, bisect, gzip, os, re, subprocess, sys, collections

PF = os.environ.get('PF', '/mnt/d/ClaudeProjects/iOS_Pixel5')
DSC = os.environ.get('DSC', os.path.expanduser('~/redfin-work/dsc/dyld_shared_cache_arm64'))
SYMDIR = os.path.join(PF, 'work', 'syms')
FRAME = re.compile(r'\?\?\? \((?P<img>[^()]+?) \+ (?P<off>\d+)\) \[(?P<addr>0x[0-9a-f]+)\]')
IDLE = re.compile(r'mach_msg|__psynch_cvwait|__workq_kernreturn|__semwait|kevent|__select|__ulock_wait|thread_suspend'
                  r'|mach_absolute_time|__sigwait|semaphore_wait|nanosleep|machine_idle|thread_block|waitq|_continue \+ 0|machine_switch_context')


class Table:
    def __init__(self, entries):
        entries.sort()
        self.addrs = [a for a, _ in entries]
        self.names = [n for _, n in entries]

    def look(self, a):
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return None
        return f'{self.names[i]} + {a - self.addrs[i]}'


def image_table(img, cache={}):
    if img in cache:
        return cache[img]
    os.makedirs(SYMDIR, exist_ok=True)
    f = os.path.join(SYMDIR, re.sub(r'[^A-Za-z0-9._+-]', '_', img) + '.txt')
    if not os.path.exists(f):
        r = subprocess.run(['ipsw', 'dyld', 'macho', DSC, img, '--symbols'], capture_output=True, text=True)
        open(f, 'w').write(r.stdout)
    ents = []
    for line in open(f, errors='replace'):
        m = re.match(r'(0x[0-9a-f]+):\s+\((__TEXT|__AUTH|__DATA)[^)]*\)\s+\S*\s+(.+?)\s*$', line)
        if m and m.group(2) == '__TEXT':
            ents.append((int(m.group(1), 16), m.group(3)))
    cache[img] = Table(ents) if ents else None
    return cache[img]


def kernel_table(kdir):
    k = os.path.join(kdir, 'kernel.development.sweet.gz')
    if not os.path.exists(k):
        return None, 0
    raw = os.path.join(SYMDIR, os.path.basename(kdir.rstrip('/')) + '.kernel')
    os.makedirs(SYMDIR, exist_ok=True)
    if not os.path.exists(raw):
        open(raw, 'wb').write(gzip.open(k).read())
    nm = next((x for x in ('llvm-nm', 'nm') if subprocess.run(['which', x], capture_output=True).returncode == 0), None)
    if not nm:
        return None, 0
    ents, base = macho_syms(raw, nm)
    return (Table(ents) if ents else None), base


def macho_syms(path, nm='llvm-nm'):
    """(text symbols, header address) of a local Mach-O; spindump offsets count from the Mach-O header"""
    out = subprocess.run([nm, '-m', '-n', '--defined-only', path], capture_output=True, text=True).stdout
    ents, base = [], 0
    for line in out.splitlines():
        m = re.match(r'([0-9a-f]+) \((__\w+),(\w+)\) (?:\[[^]]*\] )?(?:non-)?external (?:\[[^]]*\] )?(\S.*?)( \(from .*\))?$', line)
        if not m:
            continue
        a, name = int(m.group(1), 16), m.group(4)
        if name.startswith('__mh_') and name.endswith('_header'):
            base = a
        elif m.group(3) in ('__text', '__stubs', '__auth_stubs'):
            ents.append((a, name))
    return ents, base


LOCAL = {}


def local_table(img, dirs):
    if img in LOCAL:
        return LOCAL[img]
    LOCAL[img] = (None, 0)
    for d in dirs:
        for root, _, files in os.walk(d):
            if img in files:
                ents, base = macho_syms(os.path.join(root, img))
                if ents:
                    LOCAL[img] = (Table(ents), base)
                    return LOCAL[img]
    return LOCAL[img]


def last_kernel_dir():
    try:
        lb = open(os.path.join(PF, 'agent/last-boot.txt')).read().strip()
        m = re.search(r'^kernel\s+(builds/xnu/[^/\s]+)', open(os.path.join(PF, lb, 'README.txt')).read(), re.M)
        return os.path.join(PF, m.group(1)) if m else None
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('report')
    ap.add_argument('-o', '--out')
    ap.add_argument('--kernel', help='kernel build dir (default: the last-booted image\'s)')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--macho-dir', action='append', default=[],
                    help='where to find images outside the shared cache by file name (default: the hm-int14 build)')
    a = ap.parse_args()
    text = open(a.report, errors='replace').read()
    m = re.search(r'slide (0x[0-9a-f]+)', text)
    slide = int(m.group(1), 16) if m else 0
    kdir = a.kernel or last_kernel_dir()
    ktab, kbase = kernel_table(kdir) if kdir else (None, 0)
    dirs = a.macho_dir or [os.path.join(PF, 'builds/hmetal/ws-hm-int14-3a027e8')]

    def sub(mm):
        img, off, addr = mm.group('img'), int(mm.group('off')), int(mm.group('addr'), 16)
        name = None
        if img == '__kernel__':
            if ktab:
                # spindump prints the kernel's own (unslid on these boots) address; OFF counts from __TEXT_EXEC
                name = ktab.look(addr)
        elif not img.startswith('<'):
            t = image_table(img)
            if t:
                name = t.look(addr - slide)
            else:
                lt, lb = local_table(img, dirs)
                if lt:
                    name = lt.look(lb + off)
        if name:
            name = re.sub(r'^\(was a private external\)\s*', '', name)
        return f'{name} ({img} + {off})' if name else mm.group(0)

    out = FRAME.sub(sub, text)
    if a.out:
        open(a.out, 'w').write(out)
    if a.top:
        focus = out.split('\nProcess:', 2)[1] if '\nProcess:' in out else out
        leaves = collections.Counter()
        for th in re.split(r'\n  Thread ', focus)[1:]:
            lines = [l for l in th.split('\n')[1:] if l.strip() and re.match(r'\s*\*?\d+ ', l)]
            if not lines:
                continue
            # leaf = deepest line of each branch: a line whose next line is not deeper
            for i, l in enumerate(lines):
                d = len(l) - len(l.lstrip())
                nd = len(lines[i + 1]) - len(lines[i + 1].lstrip()) if i + 1 < len(lines) else -1
                if nd <= d:
                    n, fr = l.strip().lstrip('*').split(' ', 1)
                    if not IDLE.search(fr):
                        leaves[re.sub(r' \[0x[0-9a-f]+\]', '', fr)] += int(n)
        hdr = re.search(r'Process:\s+(.+)', out)
        print(f'busiest leaf frames in {hdr.group(1) if hdr else "?"} (samples, idle waits left out):')
        for fr, n in leaves.most_common(a.top):
            print(f'  {n:5d}  {fr[:150]}')
    if not a.out and not a.top:
        sys.stdout.write(out)


if __name__ == '__main__':
    main()
