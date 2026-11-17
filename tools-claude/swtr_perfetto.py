#!/usr/bin/env python3
"""swtr_perfetto.py TRACE.swtr OUT.json [--procs NAME,...] [--from MS] [--to MS]

One timeline of a sweettrace recording for ui.perfetto.dev (or chrome://tracing): open OUT.json there.
Also reachable as `sweet-trace.py TRACE --perfetto OUT.json`.

Tracks:
  - every thread (grouped by process): when it was on a CPU ("running"); our driver calls with START/END
    (IOMFB selectors, swaps, ...) as nested slices; Core Animation kdebug (0x31CA....) as slices or instants
    named "CA 0xNNN"; hakios-metal shader events (HM_SHADER) as instants with their stats;
  - process "GPU": one slice per kernel submit (GPU_DONE start/end, placed on mach time with the GPU_CAL pairs),
    named by the submitting process; hakios-metal command buffers (HM_CB) and encoders (HM_ENC, with the HM_PASS
    tile decisions and HM_CTR counter deltas as args) on their own tracks per process;
  - process "Display": VSYNC, SWAP, SCAN_FLIP/LATCH, SWAPDONE and TOUCH instants;
  - counters: per-encoder HM_CTR deltas as counter tracks.
hakios-metal events (class 0x21 subclass 0xA6, HM_PERF) appear only when the trace was recorded with them
(sweettrace default classes from ws-gpuperf on, or `-c 0x21/0xa6`).
"""
import argparse, collections, importlib.util, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('sweet_trace', os.path.join(HERE, 'sweet-trace.py'))
st = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(st)

HM_BASE = (0x21 << 24) | (0xA6 << 16)
HM_NAMES = {1: 'HM_ENC', 2: 'HM_LABEL', 3: 'HM_CB', 4: 'HM_PASS', 5: 'HM_SHADER', 6: 'HM_CTR', 7: 'HM_CAL'}
STAGES = {0: 'vertex', 1: 'fragment', 2: 'compute'}
ENC_KIND = {1: 'render', 2: 'compute', 3: 'blit'}
CA_BASE = (0x31 << 24) | (0xCA << 16)
GPU_PID, DISP_PID = 1000001, 1000002
DISPLAY_EVENTS = {'VSYNC', 'SWAP', 'SCAN_SWAP', 'SCAN_FLIP', 'SCAN_LATCH', 'SWAPDONE', 'TOUCH', 'PRESENT'}


def export(T, out, procs=None, t_from=None, t_to=None):
    us = lambda a: (a - T.start_abs) * T.numer / T.denom / 1e3
    lo = T.start_abs + T.abs_of_ns(t_from * 1e6) if t_from is not None else 0
    hi = T.start_abs + T.abs_of_ns(t_to * 1e6) if t_to is not None else 1 << 64
    threads = T.threads
    keep_pid = None
    if procs:
        want = set(procs.split(','))
        keep_pid = {v[0] for v in threads.values() if v[1] in want}
    ev = []

    def pid_of(tid):
        return threads.get(tid, [-1])[0]

    def ok(tid):
        return keep_pid is None or pid_of(tid) in keep_pid

    # metadata: process and thread names
    seen_p = set()
    for tid, (pid, comm, name) in threads.items():
        if keep_pid is not None and pid not in keep_pid:
            continue
        if pid not in seen_p:
            seen_p.add(pid)
            ev.append({'ph': 'M', 'name': 'process_name', 'pid': pid, 'args': {'name': f'{comm} [{pid}]'}})
        ev.append({'ph': 'M', 'name': 'thread_name', 'pid': pid, 'tid': tid, 'args': {'name': name or comm}})
    for pid, nm in ((GPU_PID, 'GPU (Adreno 620)'), (DISP_PID, 'Display')):
        ev.append({'ph': 'M', 'name': 'process_name', 'pid': pid, 'args': {'name': nm}})

    cpu_cur, run_start = {}, {}
    open_calls = collections.defaultdict(list)
    ca_open = collections.defaultdict(list)
    sw = collections.defaultdict(list)
    hm = collections.defaultdict(list)
    labels = {}

    def run_end(tid, ts):
        s = run_start.pop(tid, None)
        if s is not None and ok(tid) and tid in threads:
            ev.append({'ph': 'X', 'name': 'running', 'pid': pid_of(tid), 'tid': tid, 'ts': us(s), 'dur': us(ts) - us(s),
                       'cat': 'sched'})

    for e in T.events:
        ts, a1, a2, a3, a4, tid, dbg, cpu, _ = e
        if ts < lo or ts > hi:
            continue
        cid, fn = dbg & ~3, dbg & 3
        if cid == st.MACH_SCHED or cid == st.MACH_STACK_HANDOFF:
            new, old = a2, cpu_cur.get(cpu, tid)
            if old != new:
                run_end(old, ts)
                run_start[new] = ts
            cpu_cur[cpu] = new
        elif (dbg & 0xffff0000) == st.SW_BASE:
            c = (dbg >> 2) & 0x3fff
            name = st.SW.get(c, f'0x{c:x}')
            sw[name].append((ts, tid, fn, a1, a2, a3, a4))
            if c in st.CALL_CODES and fn in (1, 2) and ok(tid):
                if fn == 1:
                    open_calls[tid].append((c, ts, a1))
                else:
                    st_ = open_calls[tid]
                    for i in range(len(st_) - 1, -1, -1):
                        if st_[i][0] == c:
                            c0, t0, x1 = st_.pop(i)
                            nm = name
                            if name == 'IOMFB_SEL':
                                nm = 'IOMFB ' + st.IOMFB_SEL_NAMES.get(x1, str(x1))
                            elif name == 'ACCEL_SEL':
                                nm = 'Accel ' + st.ACCEL_NAMES.get(x1, str(x1))
                            ev.append({'ph': 'X', 'name': nm, 'pid': pid_of(tid), 'tid': tid, 'ts': us(t0),
                                       'dur': us(ts) - us(t0), 'cat': 'driver'})
                            break
            if name in DISPLAY_EVENTS:
                ev.append({'ph': 'i', 's': 'p', 'name': name, 'pid': DISP_PID, 'tid': 1, 'ts': us(ts),
                           'cat': 'display', 'args': {'a1': a1, 'a2': a2}})
        elif (dbg & 0xffff0000) == CA_BASE and ok(tid):
            code = (dbg >> 2) & 0x3fff
            nm = f'CA 0x{code:03x}'
            if fn == 1:
                ca_open[(tid, code)].append(ts)
            elif fn == 2 and ca_open[(tid, code)]:
                t0 = ca_open[(tid, code)].pop()
                ev.append({'ph': 'X', 'name': nm, 'pid': pid_of(tid), 'tid': tid, 'ts': us(t0), 'dur': us(ts) - us(t0),
                           'cat': 'coreanimation', 'args': {'a1': a1, 'a2': a2, 'a3': a3}})
            else:
                ev.append({'ph': 'i', 's': 't', 'name': nm, 'pid': pid_of(tid), 'tid': tid, 'ts': us(ts),
                           'cat': 'coreanimation', 'args': {'a1': a1, 'a2': a2, 'a3': a3, 'a4': a4}})
        elif (dbg & 0xffff0000) == HM_BASE:
            code = (dbg >> 2) & 0x3fff
            if code == 2:
                labels[a1 & 0xffffffff] = st.cstr(struct.pack('<QQQ', a2, a3, a4))
            hm[HM_NAMES.get(code, f'HM_{code}')].append((ts, tid, a1, a2, a3, a4))
    for tid in list(run_start):
        run_end(tid, min(T.end_abs, hi))

    # GPU clock -> mach time (same calibration as sweet-trace.py)
    cal = [(e[3], (e[4] + e[5]) / 2) for e in sw['GPU_CAL'] if e[3]]
    if not cal:     # no kernel GPU_CAL pairs: use hakios-metal's own (HM_CAL: GPU ticks, mach_absolute_time)
        cal = [(e[2], e[3]) for e in hm['HM_CAL'] if e[2] and e[3]]
    if len(cal) >= 2 and cal[-1][0] != cal[0][0]:
        k = (cal[-1][0] - cal[0][0]) / (cal[-1][1] - cal[0][1])
    else:
        k = 19.2e6 / (1e9 * T.denom / T.numer)

    def gabs(t):
        return cal[0][1] + (t - cal[0][0]) / k if cal and t else None

    def proc_name(pid):
        for v in threads.values():
            if v[0] == pid:
                return v[1]
        return str(pid)

    # kernel submits on the GPU
    n_gpu = 0
    for ts, tid, fn, fence, s, en, pid in sw['GPU_DONE']:
        sa, ea = gabs(s), gabs(en)
        if sa is None or ea is None or ea < sa:
            continue
        ev.append({'ph': 'X', 'name': f'submit {proc_name(pid)}', 'pid': GPU_PID, 'tid': 1, 'ts': us(sa),
                   'dur': us(ea) - us(sa), 'cat': 'gpu', 'args': {'fence': fence, 'pid': pid}})
        n_gpu += 1
    ev.append({'ph': 'M', 'name': 'thread_name', 'pid': GPU_PID, 'tid': 1, 'args': {'name': 'kernel submits'}})
    ev.append({'ph': 'M', 'name': 'thread_name', 'pid': DISP_PID, 'tid': 1, 'args': {'name': 'display events'}})

    # hakios-metal: command buffers, encoders (+ pass decisions, counters), shaders
    passes = {e[2]: e for e in hm['HM_PASS']}
    ctrs = collections.defaultdict(dict)
    for ts, tid, ref, *vals in hm['HM_CTR']:
        for v in vals:
            if v and (v >> 56) != 0xff:
                ctrs[ref][v >> 56] = v & ((1 << 56) - 1)
    hm_tracks = {}

    def hm_track(pid, kind):
        key = (pid, kind)
        if key not in hm_tracks:
            hm_tracks[key] = 100 + len(hm_tracks)
            ev.append({'ph': 'M', 'name': 'thread_name', 'pid': GPU_PID, 'tid': hm_tracks[key],
                       'args': {'name': f'{proc_name(pid)} {kind}'}})
        return hm_tracks[key]

    for ts, tid, s, en, seq, fence in hm['HM_CB']:
        sa, ea = gabs(s), gabs(en)
        if sa is not None and ea is not None and ea >= sa:
            ev.append({'ph': 'X', 'name': f'CB {seq}', 'pid': GPU_PID, 'tid': hm_track(pid_of(tid), 'command buffers'),
                       'ts': us(sa), 'dur': us(ea) - us(sa), 'cat': 'hmetal', 'args': {'fence': fence}})
    for ts, tid, s, en, info, h in hm['HM_ENC']:
        sa, ea = gabs(s), gabs(en)
        if sa is None or ea is None or ea < sa:
            continue
        kind, ref = ENC_KIND.get(info >> 56, str(info >> 56)), info & ((1 << 56) - 1)
        lab = labels.get(h & 0xffffffff, '') if h else ''
        args = {'cb': ref >> 16, 'index': ref & 0xffff}
        p = passes.get(ref)
        if p:
            m2, wh, att = p[3], p[4], p[5]
            args.update({'mode': 'gmem' if m2 & 0xff else 'sysmem', 'bins': (m2 >> 8) & 0xffff,
                         'lrz': (m2 >> 24) & 0xf, 'samples': (m2 >> 28) & 0xf, 'size': f'{wh >> 16}x{wh & 0xffff}',
                         'attachments': att & 0xff, 'depth': (att >> 8) & 1})
        for i, v in ctrs.get(ref, {}).items():
            args[f'ctr{i}'] = v
            ev.append({'ph': 'C', 'name': f'counter {i}', 'pid': GPU_PID, 'ts': us(ea), 'args': {'delta': v}})
        ev.append({'ph': 'X', 'name': f'{kind} {lab}'.strip(), 'pid': GPU_PID, 'tid': hm_track(pid_of(tid), 'encoders'),
                   'ts': us(sa), 'dur': us(ea) - us(sa), 'cat': 'hmetal', 'args': args})
    for ts, tid, h, a2, a3, a4 in hm['HM_SHADER']:
        if ok(tid):
            ev.append({'ph': 'i', 's': 't', 'name': 'shader compile', 'pid': pid_of(tid), 'tid': tid, 'ts': us(ts),
                       'cat': 'hmetal', 'args': {'hash': hex(h), 'instr': a2 & 0xffffffff, 'alu': a2 >> 32,
                                                 'full_regs': a3 & 0xffff, 'half_regs': (a3 >> 16) & 0xffff,
                                                 'max_waves': (a3 >> 32) & 0xffff,
                                                 'stage': STAGES.get((a3 >> 48) & 0x7fff, (a3 >> 48) & 0x7fff),
                                                 'from_ir3': bool(a3 >> 63), 'spills': a4 & 0xffff,
                                                 'fills': (a4 >> 16) & 0xffff, 'compile_us': a4 >> 32}})
    json.dump({'traceEvents': ev, 'displayTimeUnit': 'ms',
               'metadata': {'source': os.path.basename(T.path) if hasattr(T, 'path') else '', 'args': T.args}},
              open(out, 'w'), separators=(',', ':'))
    hmn = ', '.join(f'{k} {len(v)}' for k, v in hm.items() if v) or 'none (record with HM_PERF and class 0x21/0xa6)'
    print(f'{out}: {len(ev)} events; GPU submits {n_gpu} ({len(cal)} clock pairs); hakios-metal events: {hmn}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('out')
    ap.add_argument('--procs', help='only these processes\' threads (comma list of names); GPU/Display tracks always')
    ap.add_argument('--from', dest='t_from', type=float)
    ap.add_argument('--to', dest='t_to', type=float)
    a = ap.parse_args()
    export(st.Trace(a.trace), a.out, a.procs, a.t_from, a.t_to)


if __name__ == '__main__':
    main()
