#!/usr/bin/env python3
"""token-ledger.py: tokens the phone agent hands back to Claude, per kernel cycle.

Reads agent/outbox/*.out (first line "$ REQUEST", body = what Claude read).
A cycle = one `cycle NAME ...` request plus every cycle-status / cycle-wait /
cycle-stop / klog request after it, until the next `cycle`. Tokens are
estimated as bytes / 4 (what Claude's tool call returned, before any context
re-reads). Writes logs/token-ledger-cycles.tsv and prints a before/after summary.

usage: token-ledger.py PROJECT_DIR [--since YYYY-MM-DD] [--split YYYY-MM-DDTHH:MM]
  --split  start of the "after" period (UTC, file mtime); default: none
Agent-level totals (subagent tokens per cycle) are added by hand to
logs/token-ledger-agents.tsv from task notifications.
"""
import sys, os, re, statistics
from datetime import datetime, timezone

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); return 1
    proj = a[0]; since = split = None
    if '--since' in a: since = datetime.fromisoformat(a[a.index('--since') + 1]).replace(tzinfo=timezone.utc)
    if '--split' in a: split = datetime.fromisoformat(a[a.index('--split') + 1]).replace(tzinfo=timezone.utc)
    ob = os.path.join(proj, 'agent', 'outbox')
    rows = []
    for f in os.listdir(ob):
        if not f.endswith('.out'): continue
        p = os.path.join(ob, f)
        st = os.stat(p)
        t = datetime.fromtimestamp(st.st_mtime, timezone.utc)
        if since and t < since: continue
        with open(p, 'rb') as fh:
            first = fh.readline().decode('utf-8', 'replace').strip()
        if not first.startswith('$ '): continue
        req = first[2:].split()
        if not req: continue
        rows.append((t, f[:-4], req[0], req[1] if len(req) > 1 else '', st.st_size))
    rows.sort()
    cycles = []; cur = None
    follow = {'cycle-status', 'cycle-wait', 'cycle-stop', 'cycle-kill', 'klog', 'cycle-result'}
    for t, name, act, arg, size in rows:
        if act == 'cycle':
            cur = {'start': t, 'cycle': arg, 'reqs': 1, 'polls': 0, 'bytes': size, 'end': t}
            cycles.append(cur)
        elif cur and act in follow:
            cur['reqs'] += 1; cur['bytes'] += size; cur['end'] = t
            if act.startswith('cycle-status') or act == 'cycle-wait': cur['polls'] += 1
    out = os.path.join(proj, 'logs', 'token-ledger-cycles.tsv')
    with open(out, 'w') as fh:
        fh.write('start_utc\tperiod\tcycle\trequests\tpolls\tbytes\test_tokens\tminutes\n')
        for c in cycles:
            per = 'after' if split and c['start'] >= split else 'before'
            c['per'] = per
            fh.write(f"{c['start']:%Y-%m-%d %H:%M}\t{per}\t{c['cycle']}\t{c['reqs']}\t{c['polls']}\t"
                     f"{c['bytes']}\t{c['bytes'] // 4}\t{(c['end'] - c['start']).total_seconds() / 60:.1f}\n")
    print(f'{len(cycles)} cycles -> {out}')
    for per in ('before', 'after'):
        cs = [c for c in cycles if c.get('per') == per]
        if not cs: continue
        tok = [c['bytes'] // 4 for c in cs]; req = [c['reqs'] for c in cs]
        print(f'{per:6}: n={len(cs):3}  tokens/cycle median {statistics.median(tok):6.0f} mean {statistics.mean(tok):6.0f}'
              f'  requests/cycle median {statistics.median(req):4.1f}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
