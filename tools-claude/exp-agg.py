#!/usr/bin/env python3
"""exp-agg.py OUT.json EXPDIR... -> one row per (experiment, variant, scene): medians over valid rounds
(frames > 0). GPU = gpu_p50 for settings, gpu_swipe_p50 for pages."""
import csv, json, statistics, sys, os
out, dirs = sys.argv[1], sys.argv[2:]
def f(x):
    try: return float(x)
    except: return None
def med(v):
    v = [x for x in v if x is not None]
    return round(statistics.median(v), 2) if v else None
rows = []
for d in dirs:
    exp = os.path.basename(d.rstrip('/')).rsplit('-', 2)[0].replace('exp-', '')
    with open(os.path.join(d, 'summary.tsv')) as fh: r = list(csv.DictReader(fh, delimiter='\t'))
    order = []
    for x in r:
        if x['variant'] not in order: order.append(x['variant'])
    for v in order:
        for s in ('settings', 'pages'):
            g = [x for x in r if x['variant'] == v and x['scene'] == s and (f(x.get('frames')) or 0) > 0]
            if not g: continue
            gk = 'gpu_p50' if s == 'settings' else 'gpu_swipe_p50'
            rows.append(dict(key=f"{exp}/{v}/{s}", experiment=exp, variant=v, var_order=order.index(v), scene=s, n=len(g),
                gpu_ms=med([f(x[gk]) for x in g]), fps=med([f(x['fps']) for x in g]), int_mean_ms=med([f(x['int_mean']) for x in g]),
                pct1v_swipe=med([f(x['pct1v_swipe']) for x in g]), disp_cpu_ms=med([f(x['disp_oncpu_ms']) for x in g]),
                wake_submit_ms=med([f(x['bbwake_to_submit_p50']) for x in g]), temp_c=med([f(x['temp_c']) for x in g]),
                stalled=sum(1 for x in g if (f(x['int_p50']) or 0) >= 40)))
skip = set(a.split('=',1)[1] for a in [] )
rows = [x for x in rows if not (x['experiment'] == 'combo1' and x['scene'] == 'settings')]  # superseded by combo1set
json.dump(rows, open(out, 'w'), indent=0)
print(len(rows), 'rows')
