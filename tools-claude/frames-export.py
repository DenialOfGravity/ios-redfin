#!/usr/bin/env python3
"""frames-export.py OUT.json LABEL=REPORT [LABEL=REPORT ...]
Per-frame rows from sweet-trace.py reports (the 'per-frame critical path' table) for the frame-budget dashboard:
{label, i, t_ms, interval_ms, vsyncs, gpu_ms, bb_ms}. interval 0 rows (first frame) are skipped."""
import json, re, sys
out = []
for arg in sys.argv[2:]:
    label, path = arg.split('=', 1)
    per = 11.15; on = False; i = 0
    for line in open(path, errors='replace'):
        m = re.search(r'per-frame critical path .*vsync period ([0-9.]+) ms', line)
        if m: per = float(m.group(1)); on = True; continue
        if not on: continue
        if line.strip().startswith('t ms'): continue
        if not line.strip() or not re.match(r'\s*[0-9.]+\s+\d+\s+[0-9.]+\s+\|', line): 
            if out and i: on = False
            continue
        left, mid = line.split('|')[0], line.split('|')[1]
        t, swap, intv = left.split()
        cols = mid.split()
        def f(x):
            try: return round(float(x), 2)
            except ValueError: return None
        iv = float(intv)
        if iv <= 0: continue
        i += 1
        out.append(dict(key=f"{label}/{i}", label=label, i=i, t_ms=round(float(t), 1), interval_ms=round(iv, 2),
                        vsyncs=max(1, round(iv / per)), bb_ms=f(cols[3]), gpu_ms=f(cols[4])))
json.dump(out, open(sys.argv[1], 'w'), separators=(',', ':'))
print(len(out), 'rows ->', sys.argv[1])
