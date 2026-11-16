#!/usr/bin/env python3
"""rd-summary.py: one submission's command stream (an hm HM_RD_DUMP or Turnip FD_RD_DUMP .rd / .rd.gz), summarised per
render pass, so that hm and Turnip drawing the same frame can be compared line by line (ws-hm-csdiff, 2026-11-15).

  rd-summary.py FILE.rd [...]                 cffdump each file and print its passes
  rd-summary.py --diff HM.rd TU.rd            the two side by side: per pass kind, the counts and the key states
  rd-summary.py --json FILE.rd                the summary as JSON
  options: --cffdump PATH (default: $CFFDUMP or ~/hmvt-tools/mesa-decode-build/src/freedreno/decode/cffdump),
           --decoded (the inputs are cffdump --no-color text already, not .rd)

Per pass (a CP_SET_MARKER render mode starts one; what comes before the first is "pre"): its mode, target (RB_MRT[0]
buffer info, base, pitch; RB_RENDER_CNTL.FLAG_MRTS = UBWC), draws, CP_BLITs, WFIs, waits (CP_WAIT_REG_MEM /
CP_WAIT_MEM_GTE / CP_WAIT_MEM_WRITES), every CP_EVENT_WRITE by event (CCU flushes and invalidates, CACHE_*, LRZ_*, ...),
CP_SET_DRAW_STATE / CP_LOAD_STATE6 counts, register writes, and for the draws the values seen of the states that cost
GPU time: GRAS_SC_CNTL (single_prim_mode), RB_MRT[0].CONTROL / BLEND_CONTROL and RB_BLEND_CNTL (blending, reads of the
destination), SP_PS_CNTL_0 / SP_FS_CTRL_REG0 (threadsize, register footprint), SP_PS_INITIAL_TEX_LOAD_CNTL /
SP_FS_PREFETCH_CNTL (texture prefetch), RB_CCU_CNTL, GRAS_LRZ_CNTL, RB_DEPTH_CNTL, and the sampled textures' tile modes
and UBWC flag (TEX_CONST). Register names are both Mesa generations' (old SP_FS_* and new SP_PS_*).
"""
import collections, gzip, json, os, re, subprocess, sys

KEYREGS = ['GRAS_SC_CNTL', 'RB_MRT[0].CONTROL', 'RB_MRT[0].BLEND_CONTROL', 'RB_BLEND_CNTL', 'SP_PS_CNTL_0', 'SP_FS_CTRL_REG0',
           'SP_PS_INITIAL_TEX_LOAD_CNTL', 'SP_FS_PREFETCH_CNTL', 'RB_CCU_CNTL', 'GRAS_LRZ_CNTL', 'RB_DEPTH_CNTL',
           'RB_RENDER_CNTL', 'RB_MRT[0].BUF_INFO', 'SP_PS_WAVE_CNTL', 'SP_FS_OUTPUT_CNTL0', 'RB_FS_OUTPUT_CNTL0',
           'HLSQ_CONTROL_1_REG', 'SP_PS_CONFIG', 'SP_FS_CONFIG', 'SP_PS_INSTR_SIZE', 'SP_FS_INSTRLEN', 'GRAS_SU_CNTL',
           'RB_DITHER_CNTL', 'RB_SRGB_CNTL', 'PC_CNTL', 'PC_PRIMITIVE_CNTL_0', 'SP_PS_OUTPUT_MASK', 'RB_PS_OUTPUT_MASK']
TARGET = ['RB_MRT[0].BUF_INFO', 'RB_MRT[0].BASE', 'RB_MRT[0].PITCH', 'RB_RENDER_CNTL', 'GRAS_SC_WINDOW_SCISSOR_BR',
          'GRAS_SC_SCREEN_SCISSOR[0].BR', 'RB_CCU_CNTL']
REG = re.compile(r'^\t{2,}([A-Z][A-Z0-9_]*(?:\[[^\]]*\])?(?:\.[A-Z0-9_]+(?:\[[^\]]*\])?)*): (.*)$')
OPC = re.compile(r'opcode: (CP_[A-Z0-9_]+) ')
EVT = re.compile(r'^\t+event ([A-Z0-9_]+)')
MARK = re.compile(r'MODE = (RM6_[A-Z0-9_]+)')
TEXT = re.compile(r'(TILE_MODE|TILE6?_MODE) = (TILE6_[A-Z0-9_]+)')
FLAG = re.compile(r'\bFLAG\b')
WRITE = re.compile(r'^\t+write ([A-Z][A-Z0-9_\[\]\.]*) ')


def cffdump(path, tool):
    r = subprocess.run([tool, '--no-color', '--allregs', path], capture_output=True, text=True, errors='replace')
    if r.returncode != 0 and not r.stdout:
        sys.exit('%s: cffdump failed: %s' % (path, r.stderr.strip()[:300]))
    return r.stdout


def new_pass(mode, idx):
    return {'n': idx, 'mode': mode, 'draws': 0, 'blits': 0, 'wfi': 0, 'waits': 0, 'events': collections.Counter(),
            'opcodes': collections.Counter(), 'regwrites': 0, 'target': {}, 'states': {k: collections.Counter() for k in KEYREGS},
            'tex_tile': collections.Counter(), 'tex_flag': 0, 'tex_descs': 0, 'draw_seq': []}


def summarise(text):
    state = {}
    passes = [new_pass('pre', 0)]
    p = passes[0]
    last_op = None
    since = collections.Counter()   # events/WFIs since the previous draw (the per-draw barrier signature)
    for line in text.splitlines():
        m = OPC.search(line)
        if m:
            op = m[1]
            last_op = op
            p['opcodes'][op] += 1
            if op == 'CP_WAIT_FOR_IDLE':
                p['wfi'] += 1; since['WFI'] += 1
            elif op in ('CP_WAIT_REG_MEM', 'CP_WAIT_MEM_GTE', 'CP_WAIT_MEM_WRITES'):
                p['waits'] += 1; since[op[3:]] += 1
            elif op == 'CP_BLIT':
                p['blits'] += 1
            elif op.startswith('CP_DRAW'):
                p['draws'] += 1
                for k in KEYREGS:
                    if k in state:
                        p['states'][k][state[k]] += 1
                p['draw_seq'].append(dict(since))
                since.clear()
                if p['draws'] == 1:
                    p['target'] = {k: state.get(k) for k in TARGET}
            continue
        if last_op == 'CP_SET_MARKER':
            mm = MARK.search(line)
            if mm:
                p = new_pass(mm[1], len(passes))
                passes.append(p)
                last_op = None
                continue
        e = EVT.match(line)
        if e:
            p['events'][e[1]] += 1; since[e[1]] += 1
            continue
        if WRITE.match(line):
            p['regwrites'] += 1
            continue
        r = REG.match(line)
        if r:
            name, val = r[1], r[2].strip()
            state[name] = val
            if name.startswith('TEX_CONST') or 'TEX_CONST' in name:
                pass
            continue
        t = TEXT.search(line)
        if t and ('TEX_CONST' in line or 'TILE_MODE' in line) and 'RB_MRT' not in line and 'BUF_INFO' not in line:
            p['tex_tile'][t[2]] += 1
            p['tex_descs'] += 1
            if FLAG.search(line):
                p['tex_flag'] += 1
    return passes


def compact(passes):
    out = []
    for p in passes:
        if p['mode'] == 'pre' and not p['draws'] and not sum(p['events'].values()) and not p['wfi']:
            continue
        sig = collections.Counter(tuple(sorted(d.items())) for d in p['draw_seq'])
        out.append({
            'n': p['n'], 'mode': p['mode'], 'draws': p['draws'], 'blits': p['blits'], 'wfi': p['wfi'], 'waits': p['waits'],
            'events': dict(p['events']), 'regwrites': p['regwrites'],
            'set_draw_state': p['opcodes'].get('CP_SET_DRAW_STATE', 0),
            'load_state6': sum(v for k, v in p['opcodes'].items() if k.startswith('CP_LOAD_STATE6')),
            'target': p['target'],
            'states': {k: dict(v) for k, v in p['states'].items() if v},
            'tex_tile': dict(p['tex_tile']), 'tex_flag': p['tex_flag'],
            'barriers_before_draws': {' '.join('%s%s' % (k, '' if n == 1 else 'x%d' % n) for k, n in s) or '(none)': c
                                      for s, c in sig.most_common(8)},
        })
    return out


def short(v, n=90):
    v = str(v)
    return v if len(v) <= n else v[:n - 3] + '...'


def show(name, ps):
    tot = collections.Counter()
    for p in ps:
        tot['passes'] += 1; tot['draws'] += p['draws']; tot['wfi'] += p['wfi']; tot['waits'] += p['waits']
        for k, v in p['events'].items():
            tot['ev:' + k] += v
    print('== %s: %s' % (name, ', '.join('%s %d' % kv for kv in tot.items())))
    for p in ps:
        print('-- pass %d %s: %d draws, %d blits, %d WFI, %d waits, events %s, set_draw_state %d, load_state6 %d, reg writes %d'
              % (p['n'], p['mode'], p['draws'], p['blits'], p['wfi'], p['waits'],
                 ' '.join('%s:%d' % kv for kv in sorted(p['events'].items())) or '-', p['set_draw_state'], p['load_state6'], p['regwrites']))
        for k, v in p['target'].items():
            if v:
                print('     target %-28s %s' % (k, short(v, 140)))
        for k, v in p['states'].items():
            vs = sorted(v.items(), key=lambda x: -x[1])
            print('     %-28s %s' % (k, ' || '.join('%dx %s' % (c, short(s)) for s, c in vs[:4]) + (' || +%d more' % (len(vs) - 4) if len(vs) > 4 else '')))
        if p['tex_tile']:
            print('     sampled textures: %s, with a UBWC flag %d' % (' '.join('%s:%d' % kv for kv in p['tex_tile'].items()), p['tex_flag']))
        if p['draws']:
            print('     barriers before draws: %s' % '; '.join('%dx %s' % (c, s) for s, c in p['barriers_before_draws'].items()))


def main(argv):
    tool = os.environ.get('CFFDUMP', os.path.expanduser('~/hmvt-tools/mesa-decode-build/src/freedreno/decode/cffdump'))
    as_json = diff = decoded = False
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--cffdump':
            tool = argv[i + 1]; i += 1
        elif a == '--json':
            as_json = True
        elif a == '--diff':
            diff = True
        elif a == '--decoded':
            decoded = True
        else:
            files.append(a)
        i += 1
    if not files or (diff and len(files) != 2):
        sys.exit(__doc__)
    res = {}
    for f in files:
        text = open(f, errors='replace').read() if decoded else cffdump(f, tool)
        res[f] = compact(summarise(text))
    if as_json:
        json.dump(res, sys.stdout, indent=1)
        return
    if not diff:
        for f, ps in res.items():
            show(f, ps)
        return
    a, b = files
    show('A ' + a, res[a])
    print()
    show('B ' + b, res[b])
    print('\n== differences in the passes that draw (A vs B, in order)')
    pa = [p for p in res[a] if p['draws']]
    pb = [p for p in res[b] if p['draws']]
    for k in range(max(len(pa), len(pb))):
        x = pa[k] if k < len(pa) else None
        y = pb[k] if k < len(pb) else None
        print('-- draw pass %d: A %s | B %s' % (k, '%d draws %d WFI %d waits %s' % (x['draws'], x['wfi'], x['waits'], x['events']) if x else '-',
                                              '%d draws %d WFI %d waits %s' % (y['draws'], y['wfi'], y['waits'], y['events']) if y else '-'))
        if x and y:
            for key in sorted(set(x['states']) | set(y['states'])):
                vx, vy = set(x['states'].get(key, {})), set(y['states'].get(key, {}))
                if vx != vy:
                    print('     %-28s A %s\n     %-28s B %s' % (key, short(' || '.join(sorted(vx)), 160), '', short(' || '.join(sorted(vy)), 160)))


if __name__ == '__main__':
    main(sys.argv[1:])
