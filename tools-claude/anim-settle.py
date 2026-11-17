#!/usr/bin/env python3
"""anim-settle.py PERFETTO.json (a pa trace folder's perfetto.json): SpringBoard settle window (700 ms after each swipe's last TOUCH) split by phase:
'scroll' = a SpringBoard display link with interval 1 (CA 0x00a a4 = 213333 ticks) fired within the last 12 ms,
'tail' = otherwise (only slower links running, e.g. the in-process manager at interval 3).
Commit = CA 0x001 on SpringBoard threads. Also on-screen frame intervals (SCAN_LATCH) per phase."""
import json, sys, collections, bisect
d = json.load(open(sys.argv[1]))
ev = d['traceEvents'] if isinstance(d, dict) else d
pn = {e['pid']: e['args']['name'] for e in ev if e.get('ph') == 'M' and e.get('name') == 'process_name'}
sb = {pid for pid, n in pn.items() if n.startswith('SpringBoard')}
touch = sorted(e['ts'] for e in ev if e.get('name') == 'TOUCH')
sw, cur = [], [touch[0]]
for t in touch[1:]:
    if t - cur[-1] > 150000:
        sw.append((cur[0], cur[-1])); cur = [t]
    else:
        cur.append(t)
sw.append((cur[0], cur[-1]))
commits = sorted(e['ts'] for e in ev if e.get('pid') in sb and e.get('name', '').startswith('CA 0x001'))
fast = sorted(e['ts'] for e in ev if e.get('pid') in sb and e.get('name', '').startswith('CA 0x00a') and e.get('args', {}).get('a4') == 213333)
slow = collections.Counter(e.get('args', {}).get('a4') for e in ev if e.get('pid') in sb and e.get('name', '').startswith('CA 0x00a'))
latch = sorted(e['ts'] for e in ev if e.get('name') == 'SCAN_LATCH')
P = 11111.0
def phase(t):
    i = bisect.bisect_right(fast, t)
    return 'scroll' if i and t - fast[i - 1] < 12000 else 'tail'
g = collections.defaultdict(list)
lat = collections.defaultdict(list)
dur = collections.defaultdict(float)
for a, b in sw:
    cs = [c for c in commits if b < c <= b + 700000]
    for x, y in zip(cs, cs[1:]):
        g[phase(y)].append(y - x)
    ls = [l for l in latch if b < l <= b + 700000]
    for x, y in zip(ls, ls[1:]):
        lat[phase(y)].append(y - x)
    cs2 = [c for c in commits if a <= c <= b]
    for x, y in zip(cs2, cs2[1:]):
        g['touch'].append(y - x)
def st(label, gaps):
    if not gaps:
        print(f'{label}: none'); return
    v = [x / P for x in gaps]
    late = sum(1 for x in v if x >= 1.5)
    h = collections.Counter(min(int(x + 0.5), 6) for x in v)
    print(f'{label}: n {len(v)} late(>=1.5 vsync) {late} ({100*late/len(v):.0f} %) <5.6ms {sum(1 for x in gaps if x < 5600)} hist {dict(sorted(h.items()))} span {sum(gaps)/1000:.0f} ms')
print('swipes', len(sw), 'SpringBoard link intervals (a4 ticks):', dict(slow))
for k in ('touch', 'scroll', 'tail'):
    st('commit gaps ' + k, g[k])
for k in ('scroll', 'tail'):
    st('on-screen gaps ' + k, lat[k])
allg = g['scroll'] + g['tail']
st('commit gaps settle all', allg)
