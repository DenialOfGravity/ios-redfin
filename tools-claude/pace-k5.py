#!/usr/bin/env python3
"""pace-k5.py KLOG [RUN]: xnu 1114 pacing ring (SWEET PACE: f lines from kern.sweet_pace_dump=1) per fpsbench phase.

Each frame line carries its latch time; phases come from the SWEET PHASE lines (fpsbench "p phase NAME"), touches
from SWEET HID: touch N down/up. Per phase: slow (3+ vsync) frames split into
  cadence  no touch down and lifted > 300 ms before SwapBegin, GPU < 1 ms, 7+ vsyncs (the ~89 ms idle bursts)
  quiet    other slow frames outside a touch (deceleration tails etc. are inside the 300 ms window)
  motion   touch down or lifted <= 300 ms before SwapBegin
and the motion ones by why CA did not swap at an earlier tick (first match):
  ring     a tick in the gap where CA polled SwapWait and every answer was Busy (tribuf 2 swap ring full)
  thread   a 'none' tick and the swapping run waited runnable > 2 ms, or started > 3 ms after the tick, or the CA
           display thread waited runnable > 3 ms in the gap (scheduler / core placement)
  client   a 'none' tick with the CA display thread on core promptly: CA had nothing new (SpringBoard / the app
           did not commit in time; see sb run/wait)
  render   no skipped tick: the frame's own encode + GPU + latch made it 3+ vsyncs
RUN: only the part between fpsbench-start RUN and fpsbench-end RUN (dump lines after the end are kept).
"""
import collections
import re
import statistics as stx
import sys

F = re.compile(r'SWEET PACE: f ([\d.]+) id (\d+) v (\d+) \| idle ([\d.]+) sub ([\d.]+) gate ([\d.]+) gpu ([\d.]+) '
               r'done->latch ([\d.]+) \(wait ([\d.]+) flip->latch ([\d.]+)\) \| ticks (\d+) swap-gap busy (\d+) '
               r'none (\d+) ok (\d+) pend (\d+) off (\d+) polls (\d+)/(\d+) busy \| tick->begin ([\d.]+) on-core '
               r'(\S+) wait ([\d.]+) cpu (\d+) cls (\d+) pend-at-tick (\d+) \| disp run ([\d.]+)/([\d.]+) wait '
               r'([\d.]+)/([\d.]+) \| rs run ([\d.]+)/([\d.]+) wait ([\d.]+)/([\d.]+) \| sb run ([\d.]+)/([\d.]+) '
               r'wait ([\d.]+)/([\d.]+) \| touch down (\d+) up (\d+) ev (\d+) n (\d+) \| ahead (\d+) subs (\d+)')
K = ['t', 'id', 'v', 'idle', 'sub', 'gate', 'gpu', 'dl', 'wait', 'fl', 'ticks', 'busy', 'none', 'ok', 'pend', 'off',
     'polls', 'pbusy', 'tb', 'oc', 'cw', 'cpu', 'cls', 'pat', 'drb', 'drl', 'dwb', 'dwl', 'rrb', 'rrl', 'rwb', 'rwl',
     'srb', 'srl', 'swb', 'swl', 'down', 'up', 'ev', 'evn', 'ahead', 'subs']


def main():
    k = sys.argv[1]
    run = sys.argv[2] if len(sys.argv) > 2 else None
    L = open(k, errors='replace').read().replace('\0', '').splitlines()
    if run:
        s = [i for i, l in enumerate(L) if 'fpsbench-start ' + run in l]
        e = [i for i, l in enumerate(L) if 'fpsbench-end ' + run in l]
        if s and e:
            t0 = re.match(r'\[\s*([\d.]+)\]', L[s[-1]])
            t1 = re.match(r'\[\s*([\d.]+)\]', L[e[-1]])
            lo, hi = float(t0.group(1)), float(t1.group(1))
        else:
            sys.exit('run markers not found')
    else:
        lo, hi = 0.0, 1e12
    ph, fr, five = [], [], []
    for l in L:
        m = re.match(r'\[\s*([\d.]+)\]', l)
        t = float(m.group(1)) if m else None
        m = re.search(r'SWEET PHASE: (\w+)', l)
        if m and t is not None and lo <= t <= hi:
            ph.append((t, m.group(1)))
        m = F.search(l)
        if m:
            d = {}
            for i, name in enumerate(K):
                v = m.group(i + 1)
                d[name] = v if name == 'oc' else float(v)
            if lo <= d['t'] <= hi:
                fr.append(d)
        if 'SWEET PACE: 5s ticks' in l and t is not None and lo <= t <= hi:
            five.append((t, l.split('SWEET PACE: ')[1]))

    def phase(t):
        p = 'pre'
        for a, b in ph:
            if a <= t:
                p = b
        return p
    seen = set()
    by = collections.defaultdict(list)
    for d in fr:
        if (d['id'], d['t']) in seen:
            continue
        seen.add((d['id'], d['t']))
        by[phase(d['t'])].append(d)
    for p in ['unlock', 'pages', 'launch', 'settings', 'home', 'cc', 'idle']:
        fs = [d for d in by[p] if d['v'] >= 3]
        if not fs:
            continue
        cls = collections.Counter()
        why = collections.Counter()
        mot = []
        for d in fs:
            active = d['down'] == 1 or d['up'] <= 300
            if not active and d['gpu'] < 1.0 and d['v'] >= 7:
                cls['cadence'] += 1
                continue
            if not active:
                cls['quiet'] += 1
                continue
            cls['motion'] += 1
            mot.append(d)
            oc = 1e9 if d['oc'] == 'pre' else float(d['oc'])
            if d['busy'] > 0:
                why['ring'] += 1
            elif d['none'] > 0 and (d['cw'] > 2 or (d['oc'] != 'pre' and oc > 3) or d['dwb'] + d['dwl'] > 3):
                why['thread'] += 1
            elif d['none'] > 0:
                why['client'] += 1
            else:
                why['render'] += 1
        print(f"{p:9s} slow {len(fs)}: {dict(cls)} | motion by cause {dict(why)}")
        if mot:
            def m(key):
                xs = [x[key] for x in mot]
                return f"{stx.mean(xs):.1f}"
            little = sum(1 for x in mot if x['cpu'] < 6)
            print(f"          motion means: v {m('v')} idle {m('idle')} gpu {m('gpu')} done->latch {m('dl')} | ticks "
                  f"{m('ticks')} busy {m('busy')} none {m('none')} ok {m('ok')} pend {m('pend')} | tick->begin "
                  f"{m('tb')} wait {m('cw')} swapper on little {little}/{len(mot)} | disp run b/l {m('drb')}/"
                  f"{m('drl')} wait {m('dwb')}/{m('dwl')} | sb run {m('srb')}/{m('srl')} wait {m('swb')}/"
                  f"{m('swl')} | touch events in gap {m('evn')}")
    for t, l in five:
        print(f"[{t:9.3f}] {phase(t):8s} {l[:230]}")


if __name__ == '__main__':
    main()
