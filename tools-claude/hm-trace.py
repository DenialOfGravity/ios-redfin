#!/usr/bin/env python3
"""hm-trace.py: read hakios-metal's HM_TRACE dumps (ws-hm-trace) and say where the time went.

The phone writes them to /var/local/hmetal/trace/NAME-PID-SEQ.hmt (or the process's DARWIN_USER_TEMP_DIR when that
folder is not writable) each time /var/local/hmetal/ca/trace-go is touched, holding the last HM_TRACE_RING_KB of
records before the touch; and NAME-PID.hmt every HM_TRACE_FLUSH_S seconds when that is set.

  python3 hm-trace.py FILE.hmt [FILE2.hmt ...] [--top N] [--frames-only] [--hitches N] [--csv OUT.csv]

Lines (all times in microseconds, "made" relative to the process's trace start):
  C id fr= f= made= slot= enc= first= commit= c2d= gpu= st= passes= draws= disp= copies= clears= cpu= blk= cw= cc= GROUP=us/calls...
    (ws-hm-defer) and dq= deferred thread us, dj= its jobs, qd= its queue depth at the commit, dl= commit -> core commit us,
    ks= commit -> kernel submit returned us (0 when not deferred / not sent)
      one command buffer: slot wait, made->commit (enc), the -commit call, commit->completed (c2d), the GPU's ns,
      CPU per entry-point group, blocked (slot + waitUntil* + compiler lock), compiler wait, compiling
  P id i kind WxH draws= gpu=        a pass's GPU time (stamps at its start and after its tail)
  D id i pass= fsw= area=x,y,w,h gmem= cpu= gpu= NAME     a draw (HM_TRACE=2); gpu -1: not stamped (HM_TRACE_DRAW_TS=0, GMEM)
  E start dur bytes f= KIND WHAT     a first-use event (a compile, a cache read, a GEM object, a map, an upload...)
  H id fr= f= enc= c2d= gpu= blk= cc= events: KIND:ms:KB:WHAT ...   a hitch and the first-use work inside it
"""
import argparse
import collections
import re
import sys

KV = re.compile(r'(\w+)=([^\s]+)')


def num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def parse(paths):
    cbs, passes, draws, events, hitches, heads = {}, [], [], [], [], []
    for path in paths:
        fid = len(heads)
        with open(path, errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line:
                    continue
                if line.startswith('#'):
                    heads.append((path, line))
                    continue
                tag = line[0]
                if tag == 'C':
                    parts = line.split()
                    cid = (fid, int(parts[1]))
                    rec = {'id': cid, 'groups': {}}
                    for k, v in KV.findall(line):
                        if '/' in v and k not in ('fr',):
                            us, calls = v.split('/', 1)
                            rec['groups'][k] = (num(us), int(num(calls)))
                        else:
                            rec[k] = num(v)
                    cbs[cid] = rec
                elif tag == 'P':
                    p = line.split()
                    kv = dict(KV.findall(line))
                    passes.append({'id': (fid, int(p[1])), 'i': int(p[2]), 'kind': p[3], 'size': p[4],
                                   'draws': int(num(kv.get('draws'))), 'gpu': num(kv.get('gpu'))})
                elif tag == 'D':
                    m = re.match(r'D (\d+) (\d+) pass=(-?\d+) fsw=(\d+) area=([\d,]+) gmem=(\d) cpu=(-?[\d.]+) gpu=(-?[\d.]+) (.*)', line)
                    if m:
                        a = [int(x) for x in m.group(5).split(',')]
                        draws.append({'id': (fid, int(m.group(1))), 'i': int(m.group(2)), 'pass': int(m.group(3)),
                                      'fsw': int(m.group(4)), 'area': a, 'gmem': int(m.group(6)), 'cpu': float(m.group(7)),
                                      'gpu': float(m.group(8)), 'name': m.group(9)})
                elif tag == 'E':
                    p = line.split(' ', 6)
                    if len(p) >= 6:
                        events.append({'start': num(p[1]), 'dur': num(p[2]), 'bytes': int(num(p[3])), 'f': p[4][2:],
                                       'kind': p[5], 'what': p[6] if len(p) > 6 else '', 'file': fid})
                elif tag == 'H':
                    hitches.append((fid, line))
    return cbs, passes, draws, events, hitches, heads


def short(name, n=70):
    """the program's name without the texture/sampler detail HM_PROF=3 adds"""
    return name if len(name) <= n else name[:n - 3] + '...'


def table(title, rows, cols):
    print('\n== ' + title)
    if not rows:
        print('   (none)')
        return
    widths = [max(len(str(c)), *(len(str(r[i])) for r in rows)) for i, c in enumerate(cols)]
    print('   ' + '  '.join(str(c).rjust(w) if i else str(c).ljust(w) for i, (c, w) in enumerate(zip(cols, widths))))
    for r in rows:
        print('   ' + '  '.join(str(v).rjust(w) if i else str(v).ljust(w) for i, (v, w) in enumerate(zip(r, widths))))


def pct(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(q / 100.0 * (len(s) - 1))))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--frames-only', action='store_true', help='only buffers that drew the display (fr=1)')
    ap.add_argument('--hitches', type=int, default=30, help='hitch lines to list')
    ap.add_argument('--csv', help='write one row per command buffer')
    a = ap.parse_args()
    cbs, passes, draws, events, hitches, heads = parse(a.files)
    for path, h in heads:
        print('%s: %s' % (path, h))
    sel = {k: v for k, v in cbs.items() if not a.frames_only or v.get('fr') == 1}
    if not sel:
        print('no command buffer records (HM_TRACE unset, or the trigger was touched before anything was drawn)')
    n = len(sel)
    frames = [v for v in sel.values() if v.get('fr') == 1]
    span = (max(v['made'] for v in sel.values()) - min(v['made'] for v in sel.values())) / 1e6 if sel else 0
    print('\n%d command buffers (%d frames) over %.1f s; %d passes, %d draws recorded, %d first-use events, %d hitches'
          % (n, len(frames), span, len(passes), len(draws), len(events), len(hitches)))

    # per-phase table
    phases = [('slot wait', 'slot'), ('made->commit (encode)', 'enc'), ('CPU in hm entry points', 'cpu'), ('-commit call', 'commit'),
              ('commit->completed', 'c2d'), ('GPU (buffer)', 'gpu'), ('blocked (slot+waits+compiler lock)', 'blk'),
              ('compiler wait', 'cw'), ('compiling', 'cc')]
    rows = []
    for label, k in phases:
        vals = [v.get(k, 0) / (1e6 if k == 'gpu' else 1e3) for v in sel.values()]   # (gpu in ns, the rest in us) -> ms
        rows.append((label, '%.2f' % (sum(vals) / max(1, len(vals))), '%.2f' % pct(vals, 50), '%.2f' % pct(vals, 95), '%.2f' % max(vals or [0]),
                     '%.0f' % sum(vals)))
    table('per buffer, ms (mean / p50 / p95 / max / total)', rows, ['phase', 'mean', 'p50', 'p95', 'max', 'total'])

    # entry points
    g = collections.defaultdict(lambda: [0.0, 0])
    for v in sel.values():
        for k, (us, calls) in v['groups'].items():
            g[k][0] += us
            g[k][1] += calls
    tot = sum(x[0] for k, x in g.items() if k != 'wait') or 1
    rows = sorted(((k, '%.1f' % (us / 1e3), calls, '%.2f' % (us / calls if calls else 0), '%.0f%%' % (100 * us / tot if k != 'wait' else 0))
                   for k, (us, calls) in g.items()), key=lambda r: -float(r[1]))
    table('hm entry points by CPU time (outermost call counted; "wait" = waitUntil*, not in the %)', rows,
          ['group', 'total ms', 'calls', 'us/call', 'share'])

    # programs: GPU and CPU
    ids = set(sel)
    dsel = [d for d in draws if d['id'] in ids]
    if dsel:
        pg = collections.defaultdict(lambda: [0.0, 0.0, 0, 0, 0, 0.0])
        for d in dsel:
            key = short(d['name'].split(' 0:')[0])
            r = pg[key]
            r[2] += 1
            if d['gpu'] >= 0:
                r[0] += d['gpu']
                r[3] += 1
            if d['cpu'] >= 0:
                r[1] += d['cpu']
            r[4] += d['area'][2] * d['area'][3]
        stamped = sum(r[3] for r in pg.values())
        rows = sorted(((k, r[2], '%.2f' % (r[0] / 1e3), '%.1f' % (r[0] / r[3]) if r[3] else '-', '%.2f' % (r[1] / 1e3),
                        '%.1f' % (r[1] / r[2]), '%.1f' % (r[4] / r[2] / 1e3)) for k, r in pg.items()), key=lambda x: -float(x[2]))
        if stamped:
            table('programs by GPU time (HM_TRACE_DRAW_TS=1: the time each draw added at the end of the pipe; sysmem only)',
                  rows[:a.top], ['program', 'draws', 'GPU ms', 'GPU us/draw', 'CPU ms', 'CPU us/draw', 'kpx/draw'])
        else:
            # no stamps: the pass's GPU time shared out by area (an estimate, said so)
            pmap = {(p['id'], p['i']): p for p in passes}
            areas = collections.defaultdict(float)
            for d in dsel:
                areas[(d['id'], d['pass'])] += max(1, d['area'][2] * d['area'][3])
            est = collections.defaultdict(float)
            for d in dsel:
                p = pmap.get((d['id'], d['pass']))
                if p:
                    est[short(d['name'].split(' 0:')[0])] += p['gpu'] * max(1, d['area'][2] * d['area'][3]) / areas[(d['id'], d['pass'])]
            rows = sorted(((k, r[2], '%.2f' % (est[k] / 1e3), '%.2f' % (r[1] / 1e3), '%.1f' % (r[1] / r[2]), '%.1f' % (r[4] / r[2] / 1e3))
                           for k, r in pg.items()), key=lambda x: -float(x[2]))
            table('programs by GPU time ESTIMATED (pass GPU time shared by covered area; HM_TRACE_DRAW_TS=0)', rows[:a.top],
                  ['program', 'draws', 'est GPU ms', 'CPU ms', 'CPU us/draw', 'kpx/draw'])
        rows2 = sorted(rows, key=lambda x: -float(x[-3 if not stamped else 4]))
        table('programs by CPU encode time (the whole draw call)', rows2[:a.top],
              ['program', 'draws'] + (['GPU ms', 'GPU us/draw', 'CPU ms', 'CPU us/draw', 'kpx/draw'] if stamped else
                                      ['est GPU ms', 'CPU ms', 'CPU us/draw', 'kpx/draw']))
    # passes
    psel = [p for p in passes if p['id'] in ids]
    if psel:
        pk = collections.defaultdict(lambda: [0, 0.0, 0])
        for p in psel:
            r = pk[(p['kind'], p['size'])]
            r[0] += 1
            r[1] += p['gpu']
            r[2] += p['draws']
        rows = sorted((('%s %s' % k, r[0], '%.2f' % (r[1] / 1e3), '%.1f' % (r[1] / r[0]), '%.1f' % (r[2] / r[0])) for k, r in pk.items()),
                      key=lambda x: -float(x[2]))
        table('passes by GPU time (kind, target)', rows[:a.top], ['pass', 'count', 'GPU ms', 'us/pass', 'draws/pass'])
    # first-use events
    if events:
        ek = collections.defaultdict(lambda: [0, 0.0, 0.0, 0])
        for e in events:
            r = ek[e['kind']]
            r[0] += 1
            r[1] += e['dur']
            r[2] = max(r[2], e['dur'])
            r[3] += e['bytes']
        rows = sorted(((k, r[0], '%.1f' % (r[1] / 1e3), '%.2f' % (r[1] / r[0] / 1e3), '%.1f' % (r[2] / 1e3), '%.1f' % (r[3] / 1048576.0))
                       for k, r in ek.items()), key=lambda x: -float(x[2]))
        table('first-use events by kind', rows, ['kind', 'count', 'total ms', 'mean ms', 'max ms', 'MB'])
        rows = [('%.3f' % (e['start'] / 1e6), e['kind'], '%.1f' % (e['dur'] / 1e3), '%.0f' % (e['bytes'] / 1024.0), e['f'], e['what'][:70])
                for e in sorted(events, key=lambda e: -e['dur'])[:a.top]]
        table('slowest first-use events', rows, ['at s', 'kind', 'ms', 'KB', 'frame', 'what'])
    # hitches
    if hitches:
        print('\n== hitches (made->commit over HM_TRACE_HITCH_ENC_MS or commit->completed over HM_TRACE_HITCH_GPU_MS), first %d' % a.hitches)
        causes = collections.Counter()
        for fid, h in hitches:
            for m in re.finditer(r' (\S+?):([\d.]+)ms:', h):
                causes[m.group(1)] += 1
            if 'none (no first-use' in h:
                causes['(no first-use work)'] += 1
        for fid, h in hitches[:a.hitches]:
            print('   ' + h[:400])
        print('   causes over all hitches: ' + ', '.join('%s %d' % kv for kv in causes.most_common()))
    if a.csv:
        with open(a.csv, 'w') as f:
            keys = ['made', 'fr', 'f', 'slot', 'enc', 'first', 'commit', 'c2d', 'gpu', 'passes', 'draws', 'disp', 'copies', 'cpu', 'blk', 'cw', 'cc']
            groups = sorted({k for v in sel.values() for k in v['groups']})
            f.write(','.join(['file', 'id'] + keys + groups) + '\n')
            for (fid, cid), v in sorted(sel.items(), key=lambda kv: kv[1].get('made', 0)):
                f.write(','.join([str(fid), str(cid)] + ['%g' % v.get(k, 0) for k in keys] + ['%g' % v['groups'].get(k, (0, 0))[0] for k in groups]) + '\n')
        print('\nwrote %s' % a.csv)


if __name__ == '__main__':
    main()
