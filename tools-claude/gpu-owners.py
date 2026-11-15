#!/usr/bin/env python3
"""Per-phase, per-process GPU busy from a klog (SWEET PHASE + SWEET GPU: 5s lines). hm-vs-turnip 2026-11-15."""
import re, sys
for f in sys.argv[1:]:
    cur = None; ph = {}; order = []
    for l in open(f, errors='replace'):
        m = re.search(r'SWEET PHASE: (\S+)', l)
        if m:
            cur = m[1]
            if cur == 'unlock':
                ph = {}; order = []   # a new fpsbench run: keep only the last run in the klog
            if cur not in ph:
                ph[cur] = {}; order.append(cur)
            continue
        m = re.search(r'SWEET GPU: 5s: (\d+) submits.*?GPU busy (\d+) ms.*?;(.*)$', l)
        if m and cur:
            d = ph[cur]
            d['_tot'] = d.get('_tot', 0) + int(m[2]); d['_sub'] = d.get('_sub', 0) + int(m[1])
            for name, n, ms in re.findall(r' (\S+) (\d+)/(\d+)ms', m[3]):
                a = d.setdefault(name, [0, 0]); a[0] += int(n); a[1] += int(ms)
    print('==', f)
    for p in order:
        d = ph[p]
        own = ' '.join('%s %d/%dms' % (k, v[0], v[1]) for k, v in sorted(d.items(), key=lambda x: -x[1][1] if isinstance(x[1], list) else 0) if isinstance(v, list))
        print('  %-9s tot %5d ms subs %5d | %s' % (p, d.get('_tot', 0), d.get('_sub', 0), own))
