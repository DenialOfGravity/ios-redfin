#!/usr/bin/env python3
"""session-analyse.py DIR [options]: slow-moment finder for a free-form UI session (pa session) or any sweettrace.

  session-analyse.py logs/sessions/NAME-MMDD-HHMMSS          (manifest.json, chunks/*.swtr, klog.txt, shots/)
  session-analyse.py logs/traces/NAME-MMDD-HHMMSS [--klog F]  (one pa trace: trace.swtr; klog optional; the output
                                                              goes to its session/ subfolder)
  options: --gap-ms 50  --resp-ms 100  --launch-ms 600  --worst 15  --no-html  --out DIR (default: DIR)

Writes DIR/report.txt, DIR/session.json and DIR/report.html (timeline + screenshot thumbnails).

What it does (sweettrace events read with sweet-trace.py's Trace; chunks are read one after the other):
  - interactions: TOUCH events (kernel 0994 HID samples; fallback: klog "SWEET HID: touch N down/up" lines) grouped
    into gestures: tap, long press, swipe/scroll (direction), edge swipes (home bar, top edge), multi-touch;
  - context: the touched app (the process backboardd sends messages to during the gesture = HID delivery), the
    frontmost committer (most messages to backboardd = CA commits), app launches (klog "exec NAME" after a tap, then
    that process commits), nearest screenshot;
  - per gesture: touch -> first new frame on screen (scanout latch), frames during the gesture + its settle animation
    (fps, intervals in vsync periods 1/2/3/4+), launch time (tap up -> first frame after the app's first commit);
  - slow moments: a frame gap > --gap-ms inside a running animation (or while a finger drags), and a touch -> first
    frame > --resp-ms from a still screen; each with a cause from the evidence in its window: app main thread state
    (on CPU per cluster / waiting for a CPU / blocked + wakers), app commits, backboardd display thread state, GPU
    busy and per-frame GPU ms (fence stamps), SwapWait / GPU waits (back-pressure), shader compiles (HM_SHADER
    events, klog HakiosMetal "compiled"/"pipeline" lines), CPU load per cluster, SWEET GOV / THERMAL lines, CRASH
    and exec lines. Low-cadence animations (the app commits only every 2-4 vsyncs, e.g. Apple's 20-30 fps fades)
    are labelled and not counted as slow;
  - worst N list, per-app summary, timeline.
Clusters: CPU 0-5 little (A55), 6 gold, 7 prime. Times: "t" = seconds from the session start; klog [s] = mach s.
Cost (measured on this PC): 1.6 GB / 25 M events in 22 s, ~1 GB RAM: a 180 s session takes ~25-35 s.
"""
import argparse
import bisect
import collections
import glob
import html
import importlib.util
import json
import math
import os
import re
import sys
import time
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('sweet_trace', os.path.join(HERE, 'sweet-trace.py'))
st = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(st)

SWB = st.SW_BASE
HM_BASE = (0x21 << 24) | (0xA6 << 16)
LITTLE = set(range(6))
CLUSTER = {6: 'gold', 7: 'prime'}
DAEMONS = {'kernel_task', 'launchd', 'backboardd', 'sweettrace', 'sweetmon', 'dropbear', 'dash', 'toybox', 'sctl',
           'CommCenter', 'locationd', 'wifid', 'mediaserverd', 'thermalmonitord', 'runningboardd', 'assertiond',
           'powerd', 'UserEventAgent', 'syslogd', 'notifyd', 'cfprefsd', 'iconservicesagent', 'configd',
           'mDNSResponder', 'bluetoothd', 'audiomxd', 'logd', 'symptomsd', 'aggregated', 'nfcd', 'SpringBoardHome',
           'AccessibilityUIServer', 'healthd', 'dasd', 'xpcproxy', 'MTLCompilerService', 'lsd', 'sharingd',
           'ReportCrash', 'fseventsd', 'watchdogd', 'sweetctl', 'PosterBoard', 'callservicesd', 'accessoryd',
           'locationpushd', 'coreduetd', 'contextstored', 'useractivityd', 'chronod', 'searchpartyd'}
HOME = 'SpringBoard'


def clus(cpu):
    return 'little' if cpu in LITTLE else CLUSTER.get(cpu, f'cpu{cpu}')


class G:
    """everything kept from all chunks (times in mach seconds = the klog clock)"""

    def __init__(self):
        self.threads = {}                   # tid -> [pid, comm, name]
        self.cpu = collections.defaultdict(lambda: array('d'))     # cpu -> (start, end, tid)*
        self.waits = collections.defaultdict(lambda: array('d'))   # tid -> (off, wake or -1, on, waker or -1)*
        self.to_bb = []                     # (t, sender tid, sender pid, bytes)
        self.from_bb = []                   # (t, dst pid)
        self.sw = collections.defaultdict(list)    # name -> (t, tid, fn, a1, a2, a3, a4)
        self.calls = []                     # (start, end, tid, code, a_start, a_end)
        self.fences = {}                    # fence -> (start, end, pid)
        self.submits = []                   # (t, tid, fence, pid)
        self.shaders = []                   # (t, tid, compile_ms)
        self.chunks = []                    # (start_s, end_s, wall_s at start, file, events, wrapped, dropped)
        self.bb = set()


def scan_chunk(path, g, log):
    t0 = time.time()
    T = st.Trace(path)
    k = T.numer / T.denom / 1e9
    for tid, v in T.threads.items():
        old = g.threads.get(tid)
        if old is None or v[0] >= 0:
            g.threads[tid] = [v[0], v[1], v[2] or (old[2] if old else '')]
    bb = {v[0] for v in T.threads.values() if v[1] == 'backboardd'}
    g.bb |= bb
    g.chunks.append((T.start_abs * k, T.end_abs * k, T.wall_ns / 1e9, path, len(T.events), bool(T.kd_flags & 8),
                     T.dropped))
    MACH_SCHED, HANDOFF, RUNNABLE, INTR = st.MACH_SCHED, st.MACH_STACK_HANDOFF, st.MACH_MAKERUNNABLE, st.INTR
    KINFO, NEWT, TSTR, TNAME = st.IPC_KMSG_INFO, st.TRACE_NEWTHREAD, st.TRACE_STRING_GLOBAL, st.TRACE_THREADNAME
    SW, CALLS = st.SW, st.CALL_CODES
    cpu_cur, cpu_on, cpu_intr = {}, {}, collections.defaultdict(int)
    off_start, pend = {}, {}
    gcpu, gwaits, threads = g.cpu, g.waits, g.threads
    to_bb, from_bb, sw = g.to_bb, g.from_bb, g.sw
    stack = collections.defaultdict(list)
    names_pending = {}
    cal, done, subs = [], [], []
    for ts, a1, a2, a3, a4, tid, dbg, cpu, _ in T.events:
        cid = dbg & ~3
        if cid == MACH_SCHED or cid == HANDOFF:
            new = a2
            old = cpu_cur.get(cpu)
            if old != new:
                t = ts * k
                if old is not None:
                    s = cpu_on.get(cpu)
                    if s is not None:
                        gcpu[cpu].extend((s, t, old))
                    off_start[old] = t
                o = off_start.pop(new, None)
                w = pend.pop(new, None)
                if o is not None:
                    if w is not None and w[0] >= o:
                        gwaits[new].extend((o, w[0], t, w[1]))
                    else:
                        gwaits[new].extend((o, -1.0, t, -1.0))
                cpu_cur[cpu] = new
                cpu_on[cpu] = t
        elif cid == RUNNABLE:
            pend[a1] = (ts * k, tid if not cpu_intr[cpu] else -2.0 - tid)    # < -1: woken from an interrupt on tid
        elif cid == INTR:
            fn = dbg & 3
            if fn == 1:
                cpu_intr[cpu] += 1
            elif fn == 2 and cpu_intr[cpu] > 0:
                cpu_intr[cpu] -= 1
        elif cid == KINFO:
            if dbg & 3 == 2:
                if a2 in bb:
                    to_bb.append((ts * k, tid, a1, a3))
                elif a1 in bb:
                    from_bb.append((ts * k, a2))
        elif (dbg & 0xffff0000) == SWB:
            c = (dbg >> 2) & 0x3fff
            fn = dbg & 3
            name = SW.get(c, f'0x{c:x}')
            t = ts * k
            if c in CALLS and fn in (1, 2):
                if fn == 1:
                    stack[tid].append((c, t, (a1, a2, a3, a4)))
                else:
                    sl = stack[tid]
                    for i in range(len(sl) - 1, -1, -1):
                        if sl[i][0] == c:
                            s = sl.pop(i)
                            g.calls.append((s[1], t, tid, c, s[2], (a1, a2, a3, a4)))
                            break
            if name == 'GPU_CAL':
                if a1:
                    cal.append((a1, (a2 + a3) / 2))
            elif name == 'GPU_DONE':
                done.append((a1, a2, a3, a4))
            elif name == 'GPU_SUBMIT':
                if fn == 2 and a1:
                    subs.append((t, tid, a1, a3))
            else:
                sw[name].append((t, tid, fn, a1, a2, a3, a4))
        elif (dbg & 0xffff0000) == HM_BASE:
            if ((dbg >> 2) & 0x3fff) == 5:     # HM_SHADER: a4 >> 32 = compile us
                g.shaders.append((ts * k, tid, (a4 >> 32) / 1000.0))
        elif cid == NEWT:
            threads.setdefault(a1, [a2, threads.get(tid, [0, '?', ''])[1], ''])
        elif cid == TSTR:
            fn = dbg & 3
            if fn & 1:
                names_pending[tid] = st.struct.pack('<QQ', a3, a4) if (a1 & ~3) == TNAME else None
            elif names_pending.get(tid) is not None:
                names_pending[tid] += st.struct.pack('<QQQQ', a1, a2, a3, a4)
            if fn & 2 and names_pending.get(tid) is not None:
                nm = st.cstr(names_pending.pop(tid))
                if nm:
                    threads.setdefault(tid, [-1, '?', ''])[2] = nm
    tend = T.end_abs * k
    for c, tid in cpu_cur.items():
        s = cpu_on.get(c)
        if s is not None:
            gcpu[c].extend((s, tend, tid))
    # GPU clock -> mach seconds (as sweet-trace.py), per chunk
    if cal:
        if len(cal) >= 2 and cal[-1][0] != cal[0][0]:
            tpa = (cal[-1][0] - cal[0][0]) / (cal[-1][1] - cal[0][1])
        else:
            tpa = 19.2e6 / (1e9 * T.denom / T.numer)
        g0, a0 = cal[0]
        prev_end = None
        for f, s, e, pid in sorted(done):
            if prev_end and prev_end > s and prev_end < e:
                s = prev_end
            prev_end = e
            if s and e:
                sa, ea = (a0 + (s - g0) / tpa) * k, (a0 + (e - g0) / tpa) * k
                if ea >= sa:
                    g.fences[f] = (sa, ea, pid)
    g.submits.extend(subs)
    n = len(T.events)
    del T
    log(f'  {os.path.basename(path)}: {n} events, {time.time() - t0:.1f} s')


# ------------------------------------------------------------------------------------------------ klog
KRX = re.compile(r'^\[ *([0-9]+\.[0-9]+)\] (.*)$')


def read_klog(path, lo, hi):
    out = []
    if not path or not os.path.exists(path):
        return out
    last = None
    with open(path, 'rb') as f:
        for raw in f:
            line = raw.decode('utf-8', 'replace').rstrip('\n')
            m = KRX.match(line)
            if m:
                last = float(m.group(1))
                txt = m.group(2)
            else:
                txt = line
            if last is not None and lo - 6 <= last <= hi + 6:
                out.append((last, txt))
    return out


# ------------------------------------------------------------------------------------------------ analysis
class Ana:
    def __init__(self, g, klog, a):
        self.g, self.a = g, a
        self.k = klog
        self.t0 = min(c[0] for c in g.chunks)
        self.t1 = max(c[1] for c in g.chunks)
        self.gaps = [(x[1], y[0]) for x, y in zip(sorted(g.chunks), sorted(g.chunks)[1:]) if y[0] - x[1] > 0.001]
        th = g.threads
        self.pname = {}
        for tid, (pid, comm, nm) in th.items():
            if pid >= 0 and comm and comm != '?':
                self.pname.setdefault(pid, comm)
        self.cpu_idx = {c: arr[0::3] for c, arr in g.cpu.items()}      # starts, for bisect
        self.idle = self.find_idle()
        # the kernel's timer thread: the kernel_task thread that wakes the most different threads
        wk = collections.defaultdict(set)
        for tid, w in g.waits.items():
            for by in w[3::4]:
                if by >= 0:
                    wk[by].add(tid)
        kt = [(len(v), by) for by, v in wk.items() if (g.threads.get(by) or [0])[0] == 0]
        self.timer_tid = max(kt)[1] if kt else None
        vs = sorted(e[0] for e in g.sw['VSYNC'])
        d = sorted(y - x for x, y in zip(vs, vs[1:]) if 0.005 < y - x < 0.05)
        self.period = d[len(d) // 2] if d else 1 / 60
        lat = sorted((e[0], e[3]) for e in g.sw['SCAN_LATCH'])
        if not lat:     # no direct scanout: presents
            lat = sorted((c[1], c[4][0]) for c in g.calls if c[3] == 0x06 and not c[5][1])
        self.frames = [x[0] for x in lat]
        self.frame_swap = [x[1] for x in lat]
        se = [(c[0], c[2], c[5][3]) for c in g.calls if c[3] == 0x04 and c[4][0] == 5]
        cnt = collections.Counter(x[1] for x in se)
        self.disp = cnt.most_common(1)[0][0] if cnt else None
        self.swapend = {}
        for t, tid, sw in sorted(se):
            self.swapend.setdefault(sw, t)
        # the display thread can change (render server restart): the SwapEnd caller in force at a time
        self.se_all = sorted((t, tid) for t, tid, _ in se)
        self.se_t = [x[0] for x in self.se_all]
        dtids = set(cnt)
        self.se_times = sorted(t for t, tid, _ in se)
        self.waitcalls = sorted((c[0], c[1]) for c in g.calls if c[2] in dtids and
                                ((c[3] == 0x04 and c[4][0] == 6) or c[3] in (0x12, 0x0c, 0x14)))
        self.fl = sorted(g.fences.values())
        self.fl_s = [f[0] for f in self.fl]
        self.subs = sorted(g.submits)
        g.to_bb.sort()
        g.from_bb.sort()
        self.to_bb_t = [m[0] for m in g.to_bb]
        self.from_bb_t = [m[0] for m in g.from_bb]
        # main thread of a pid = its thread that sends the most messages to backboardd (CA commits run there)
        per = collections.Counter((m[2], m[1]) for m in g.to_bb if m[3] >= 500)
        per.update((m[2], m[1]) for m in g.to_bb)
        self.main = {}
        for (pid, tid), n in per.most_common():
            self.main.setdefault(pid, tid)
        # commits (as sweet-trace.py --chain): bursts of main-thread messages to backboardd (< 0.5 ms apart) with
        # >= 3 messages or one >= 500 bytes; the time is the burst's first message
        self.commits = collections.defaultdict(list)
        cur = {}
        for t, tid, pid, sz in g.to_bb:
            if self.main.get(pid) != tid:
                continue
            b = cur.get(pid)
            if b and t - b[1] < 0.0005:
                b[1] = t
                b[2] += 1
                b[3] = max(b[3], sz)
            else:
                if b and (b[2] >= 3 or b[3] >= 500):
                    self.commits[pid].append(b[0])
                cur[pid] = [t, t, 1, sz]
        for pid, b in cur.items():
            if b[2] >= 3 or b[3] >= 500:
                self.commits[pid].append(b[0])
        self.kl_exec = [(t, x[5:].strip()) for t, x in klog if x.startswith('exec ')]
        self.kl_crash = [(t, x) for t, x in klog if 'CRASH' in x or 'panic(' in x]
        self.kl_shader = [(t, x) for t, x in klog if x.startswith('HakiosMetal') and
                          re.search(r'compiled|CA pipeline|pipeline state|shader', x)]
        self.kl_gov = [(t, x) for t, x in klog if x.startswith('SWEET GOV: 5s')]
        self.kl_therm = [(t, x) for t, x in klog if x.startswith('SWEET THERMAL:')]
        self.kl_ui = [(t, x) for t, x in klog if x.startswith('SWEET UI:')]

    # ---- helpers
    def ms(self, s):
        return s * 1000.0

    def rel(self, t):
        return t - self.t0

    def name(self, pid):
        return self.pname.get(pid, f'pid{pid}')

    def tname(self, tid):
        v = self.g.threads.get(tid)
        if v is None:
            return f'tid {int(tid)}'
        return f'{v[1]}/{v[2] or int(tid)}'

    def disp_at(self, t):
        if not self.se_all:
            return None
        i = bisect.bisect_right(self.se_t, t)
        return self.se_all[i if i < len(self.se_all) else -1][1]

    def in_gap(self, a, b):
        return any(x < b and y > a for x, y in self.gaps)

    def find_idle(self):
        """idle threads: kernel_task threads that only ever run on one CPU and run there a lot"""
        tot = collections.defaultdict(float)
        cpus = collections.defaultdict(set)
        for c, arr in self.g.cpu.items():
            for i in range(0, len(arr), 3):
                tid = arr[i + 2]
                tot[tid] += arr[i + 1] - arr[i]
                cpus[tid].add(c)
        span = sum(c[1] - c[0] for c in self.g.chunks)
        idle = set()
        for tid, t in tot.items():
            v = self.g.threads.get(tid)
            if (v is None or v[0] == 0) and len(cpus[tid]) == 1 and t > 0.05 * span:
                idle.add(tid)
        return idle

    def thread_state(self, tid, a, b):
        """on-CPU ms per cluster, runnable ms, blocked ms and the wakers, for one thread in [a, b]"""
        oncpu = collections.defaultdict(float)
        for c, arr in self.g.cpu.items():
            idx = self.cpu_idx[c]
            i = max(0, bisect.bisect_left(idx, a) - 1)
            n = len(idx)
            while i < n and idx[i] < b:
                if arr[3 * i + 2] == tid:
                    s, e = max(a, arr[3 * i]), min(b, arr[3 * i + 1])
                    if e > s:
                        oncpu[clus(c)] += e - s
                i += 1
        run = blk = 0.0
        wakers = collections.Counter()
        w = self.g.waits.get(tid)
        if w is not None:
            # waits are appended in time order per thread: scan from the first that can overlap
            offs = w[0::4]
            i = max(0, bisect.bisect_left(offs, a - 2.0))
            for j in range(i, len(offs)):
                o, wk, on, by = w[4 * j], w[4 * j + 1], w[4 * j + 2], w[4 * j + 3]
                if o >= b:
                    break
                if on <= a:
                    continue
                if wk < 0:
                    run += max(0.0, min(b, on) - max(a, o))
                else:
                    bl = max(0.0, min(b, wk) - max(a, o))
                    blk += bl
                    run += max(0.0, min(b, on) - max(a, wk))
                    if bl > 0 or a <= wk <= b:
                        who = (('kernel timer ' if by == self.timer_tid else '') + self.tname(by)) if by >= 0 \
                            else ('irq on ' + self.tname(-2.0 - by))
                        wakers[who] += bl
        return {'cpu_ms': {k2: v * 1000 for k2, v in oncpu.items()}, 'on_ms': sum(oncpu.values()) * 1000,
                'runnable_ms': run * 1000, 'blocked_ms': blk * 1000,
                'wakers': [(k2, round(v * 1000, 1)) for k2, v in wakers.most_common(3)]}

    def cpu_load(self, a, b):
        """busy fraction per cluster in [a, b] and the top non-idle threads on the big cores"""
        busy = collections.defaultdict(float)
        big = collections.Counter()
        for c, arr in self.g.cpu.items():
            idx = self.cpu_idx[c]
            i = max(0, bisect.bisect_left(idx, a) - 1)
            while i < len(idx) and idx[i] < b:
                tid = arr[3 * i + 2]
                if tid not in self.idle:
                    s, e = max(a, arr[3 * i]), min(b, arr[3 * i + 1])
                    if e > s:
                        busy[clus(c)] += e - s
                        if c not in LITTLE:
                            big[self.tname(tid)] += e - s
                i += 1
        d = max(b - a, 1e-6)
        return {'little': busy['little'] / (6 * d), 'gold': busy['gold'] / d, 'prime': busy['prime'] / d,
                'big_top': [(k2, round(v * 1000, 1)) for k2, v in big.most_common(3)]}

    def gpu(self, a, b):
        """GPU busy ms (union of fences) in [a, b] and the longest fence overlapping it"""
        i = max(0, bisect.bisect_left(self.fl_s, a - 0.2))
        segs = []
        longest = 0.0
        lpid = None
        for s, e, pid in self.fl[i:]:
            if s >= b:
                break
            if e <= a:
                continue
            segs.append((max(a, s), min(b, e)))
            if e - s > longest:
                longest, lpid = e - s, pid
        segs.sort()
        tot, cur = 0.0, None
        for s, e in segs:
            if cur is None or s > cur[1]:
                if cur:
                    tot += cur[1] - cur[0]
                cur = [s, e]
            else:
                cur[1] = max(cur[1], e)
        if cur:
            tot += cur[1] - cur[0]
        return tot * 1000, longest * 1000, lpid

    def frame_gpu(self, fi):
        """GPU ms of on-screen frame fi: the display thread's submits since its previous SwapEnd"""
        se = self.swapend.get(self.frame_swap[fi])
        if se is None:
            return None
        j = bisect.bisect_left(self.se_times, se)
        lo = self.se_times[j - 1] if j > 0 else se - 0.1
        i = bisect.bisect_right(self.subs, (lo, float('inf')))
        tot = 0.0
        n = 0
        while i < len(self.subs) and self.subs[i][0] <= se:
            s = self.subs[i]
            if (s[1] == self.disp_at(se) or s[3] in self.g.bb) and s[2] in self.g.fences:
                f = self.g.fences[s[2]]
                tot += f[1] - f[0]
                n += 1
            i += 1
        return tot * 1000 if n else None

    def wait_ms(self, a, b):
        tot = 0.0
        for s, e in self.waitcalls[max(0, bisect.bisect_left(self.waitcalls, (a - 0.5, 0))):]:
            if s >= b:
                break
            tot += max(0.0, min(b, e) - max(a, s))
        return tot * 1000

    def between(self, lst, a, b, key=lambda x: x[0]):
        return [x for x in lst if a <= key(x) <= b]

    def near(self, lst, t):
        if not lst:
            return None
        i = bisect.bisect_left([x[0] for x in lst], t)
        c = [lst[j] for j in (i - 1, i) if 0 <= j < len(lst)]
        return min(c, key=lambda x: abs(x[0] - t))

    def front(self, a, b):
        """the process committing most (main-thread messages to backboardd) in [a, b], not a daemon"""
        i, j = bisect.bisect_left(self.to_bb_t, a), bisect.bisect_right(self.to_bb_t, b)
        c = collections.Counter()
        for t, tid, pid, _ in self.g.to_bb[i:j]:
            if self.main.get(pid) == tid and self.name(pid) not in DAEMONS:
                c[pid] += 1
        return c.most_common(1)[0][0] if c else None

    def touched(self, a, b):
        """the process backboardd sends messages to during [a, b] (HID event delivery), not a daemon"""
        i, j = bisect.bisect_left(self.from_bb_t, a), bisect.bisect_right(self.from_bb_t, b)
        c = collections.Counter(pid for _, pid in self.g.from_bb[i:j] if self.name(pid) not in DAEMONS and pid > 0
                                and self.commits_in(pid, a - 0.5, b + 0.5))
        if not c:
            return self.front(a - 0.3, b + 0.5)
        top = c.most_common(2)
        # SpringBoard gets most system gestures' events; an app that gets a fair share is the target
        if len(top) > 1 and self.name(top[0][0]) == HOME and top[1][1] >= 0.5 * top[0][1]:
            return top[1][0]
        return top[0][0]

    def commits_in(self, pid, a, b):
        c = self.commits.get(pid, [])
        return c[bisect.bisect_left(c, a):bisect.bisect_right(c, b)]

    def cadence(self, pid, t):
        """median spacing of pid's commit bursts in the 0.6 s before t, in vsync periods (None: too few)"""
        c = self.commits_in(pid, t - 0.6, t)
        bursts = list(c)
        if len(bursts) < 4:
            return None
        d = sorted(y - x for x, y in zip(bursts, bursts[1:]))
        return d[len(d) // 2] / self.period

    def frame_after(self, t):
        i = bisect.bisect_right(self.frames, t)
        return i if i < len(self.frames) else None

    # ---- gestures
    def gestures(self):
        ev = sorted(self.g.sw['TOUCH'])
        src = 'trace'
        if not ev:
            src = 'klog'
            rx = re.compile(r'SWEET HID: touch (\d+) (down|up) at (\d+),(\d+)')
            for t, x in self.k:
                m = rx.search(x)
                if m and self.t0 - 0.5 <= t <= self.t1 + 0.5:
                    ev.append((t, 0, 0, int(m.group(1)), 1 if m.group(2) == 'down' else 0, int(m.group(3)),
                               int(m.group(4))))
        open_g = {}
        out = []
        for t, tid, fn, tch, down, x, y in ev:
            gg = open_g.get(tch)
            if down:
                if gg is None:
                    gg = {'id': tch, 'down': t, 'pts': [], 'up': None}
                    open_g[tch] = gg
                gg['pts'].append((t, x, y))
            else:
                if gg is None:
                    continue
                gg['pts'].append((t, x, y))
                gg['up'] = t
                out.append(open_g.pop(tch))
        for gg in open_g.values():
            gg['up'] = gg['pts'][-1][0]
            gg['open'] = True
            out.append(gg)
        out.sort(key=lambda z: z['down'])
        # multi-touch: overlapping gestures of different ids merge into the first
        merged = []
        for gg in out:
            if merged and gg['down'] < merged[-1]['up'] and gg['id'] != merged[-1]['id']:
                merged[-1]['fingers'] = merged[-1].get('fingers', 1) + 1
                merged[-1]['up'] = max(merged[-1]['up'], gg['up'])
                continue
            merged.append(gg)
        for gg in merged:
            p = gg['pts']
            (t0, x0, y0), (t1, x1, y1) = p[0], p[-1]
            dx, dy = x1 - x0, y1 - y0
            dist = math.hypot(dx, dy)
            dur = gg['up'] - gg['down']
            if gg.get('fingers', 1) > 1:
                kind = 'multi-touch'
            elif dist < 40:
                kind = 'long press' if dur >= 0.5 else 'tap'
            else:
                d = ('down' if dy > 0 else 'up') if abs(dy) >= abs(dx) else ('right' if dx > 0 else 'left')
                if y0 >= 2240 and d == 'up':
                    kind = 'home-bar swipe up'
                elif y0 <= 80 and d == 'down':
                    kind = 'top-edge pull down' + (' (Control Center)' if x0 > 700 else ' (notifications)')
                else:
                    sp = dist / max(dur, 0.02)
                    kind = f'{"fling" if sp > 2500 else "swipe"} {d}'
            gg.update(kind=kind, x0=x0, y0=y0, x1=x1, y1=y1, dist=dist, dur=dur, samples=len(p), src=src)
        return merged

    # ---- cause attribution for a window [a, b] that ends with on-screen frame fb (or None)
    def attribute(self, a, b, pid, fb=None, kind='gap'):
        ev = {}
        main = self.main.get(pid) if pid is not None else None
        cm = self.commits_in(pid, a, b) if pid is not None else []
        first_c = cm[0] if cm else None
        ev['app'] = self.name(pid) if pid is not None else '?'
        ev['app_commits'] = len(cm)
        ev['first_commit_ms'] = round((first_c - a) * 1000, 1) if first_c else None
        ms_win = (b - a) * 1000
        app_end = first_c if first_c and (first_c - a) * 1000 > 0.4 * ms_win else b
        if main is not None:
            ev['app_main'] = self.thread_state(main, a, app_end)
        dt = self.disp_at(b)
        if dt is not None:
            ev['bb_display'] = self.thread_state(dt, a, b)
        g_busy, g_long, g_pid = self.gpu(a, b)
        ev['gpu_busy_ms'] = round(g_busy, 1)
        ev['gpu_longest_ms'] = round(g_long, 1)
        ev['gpu_longest_by'] = self.name(g_pid) if g_pid is not None else None
        if fb is not None:
            fg = self.frame_gpu(fb)
            ev['frame_gpu_ms'] = round(fg, 1) if fg is not None else None
        ev['bb_wait_ms'] = round(self.wait_ms(a, b), 1)
        ev['cpu_load'] = self.cpu_load(a, b)
        sh = [s for s in self.g.shaders if a - 0.05 <= s[0] <= b]
        ksh = self.between(self.kl_shader, a - 0.1, b + 0.02)
        ev['shader_compiles'] = len(sh) + len(ksh)
        if sh:
            ev['shader_compile_ms'] = round(sum(s[2] for s in sh), 1)
        if ksh:
            ev['shader_lines'] = [x[:140] for _, x in ksh[:3]]
        cr = self.between(self.kl_crash, a - 2, b + 1)
        if cr:
            ev['crash'] = [x[:140] for _, x in cr[:3]]
        ex = self.between(self.kl_exec, a - 0.5, b)
        if ex:
            ev['exec'] = [x for _, x in ex[:6]]
        gv = self.near(self.kl_gov, b + 2.5)
        if gv and abs(gv[0] - b) < 6:
            m = re.search(r'capped (\d+)', gv[1])
            ev['gov'] = gv[1][15:160]
            if m and int(m.group(1)) > 0:
                ev['gov_capped'] = int(m.group(1))
        th = self.near(self.kl_therm, b)
        if th and abs(th[0] - b) < 30:
            temps = [float(x) for x in re.findall(r' ([0-9]+\.[0-9]) ', th[1] + ' ')]
            ev['temp_max_c'] = max(temps) if temps else None
        cad = self.cadence(pid, b) if pid is not None else None
        ev['app_cadence_vsync'] = round(cad, 2) if cad else None

        # ---- the decision
        per = self.period * 1000
        am = ev.get('app_main')
        bbd = ev.get('bb_display')
        win_app = (app_end - a) * 1000 or 1e-3
        causes = []
        app_late = first_c is None or (first_c - a) * 1000 > 0.4 * ms_win
        if self.in_gap(a, b):
            causes.append(('trace gap (between chunks): not measured', 0))
        if app_late:
            if am:
                on, rn, bl = am['on_ms'], am['runnable_ms'], am['blocked_ms']
                little = am['cpu_ms'].get('little', 0)
                if on >= 0.45 * win_app:
                    where = f'{little:.0f} ms of it on little cores' if little > 0.5 * on else 'big cores'
                    causes.append((f'app main thread CPU-bound: {on:.0f} ms on CPU of {win_app:.0f} ms ({where})', on))
                elif rn >= 0.3 * win_app:
                    load = ev['cpu_load']
                    causes.append((f'app main thread waiting for a CPU {rn:.0f} ms (load little {load["little"]:.0%} '
                                   f'gold {load["gold"]:.0%} prime {load["prime"]:.0%})', rn))
                elif bl >= 0.5 * win_app and (ev['shader_compiles'] or ev.get('exec')):
                    causes.append((f'app main thread blocked {bl:.0f} ms: shader/pipeline build or launch '
                                   f'(wakers {am["wakers"][:2]})', bl))
                elif bl >= 0.5 * win_app and am['wakers'] and am['wakers'][0][0].startswith('kernel timer'):
                    causes.append((f'app idle until a timer fired: main thread blocked {bl:.0f} ms, no commit '
                                   f'(app-paced / delayed animation step, not a CPU or IO stall)', 0.5 * win_app))
                elif bl >= 0.5 * win_app and am['wakers'] and am['wakers'][0][0].startswith('kernel_task'):
                    causes.append((f'app main thread blocked {bl:.0f} ms, woken by the kernel (IO / page-in / '
                                   f'timer?: {am["wakers"][0][0]})', bl))
                elif bl >= 0.5 * win_app and am['wakers'] and am['wakers'][0][1] > 0.3 * win_app:
                    causes.append((f'app main thread blocked {bl:.0f} ms waiting for {am["wakers"][0][0]}', bl))
                else:
                    lbl = 'app not committing'
                    if cad and cad >= 1.7:
                        lbl += f' (app-paced animation, commits every {cad:.1f} vsyncs: e.g. an Apple 20-30 fps fade)'
                    causes.append((f'{lbl}: main thread idle {bl:.0f} ms', 0.5 * win_app))
            else:
                causes.append(('no commits from the app in the window (main thread unknown)', 0.3 * ms_win))
        else:
            fg = ev.get('frame_gpu_ms') or 0
            if fg >= 1.5 * per or ev['gpu_busy_ms'] >= 0.6 * ms_win:
                causes.append((f'GPU-bound: frame GPU {fg:.1f} ms, GPU busy {ev["gpu_busy_ms"]:.0f} of {ms_win:.0f} ms'
                               f' (longest {ev["gpu_longest_ms"]:.0f} ms, {ev["gpu_longest_by"]})',
                               max(fg, ev['gpu_busy_ms'])))
            if ev['bb_wait_ms'] >= 0.3 * ms_win:
                causes.append((f'backboardd waiting in SwapWait / GPU fence (back-pressure) {ev["bb_wait_ms"]:.0f} ms',
                               ev['bb_wait_ms']))
            if bbd and bbd['on_ms'] >= 0.4 * ms_win:
                lt = bbd['cpu_ms'].get('little', 0)
                causes.append((f'backboardd render thread CPU {bbd["on_ms"]:.0f} ms'
                               + (f' ({lt:.0f} ms on little cores)' if lt > 0.3 * bbd['on_ms'] else ''),
                               bbd['on_ms']))
            if bbd and bbd['runnable_ms'] >= 0.25 * ms_win:
                causes.append((f'backboardd render thread waiting for a CPU {bbd["runnable_ms"]:.0f} ms',
                               bbd['runnable_ms']))
            if not causes:
                causes.append((f'backboardd late after the commit (display thread on CPU {bbd["on_ms"] if bbd else 0:.0f}'
                               f' ms, blocked {bbd["blocked_ms"] if bbd else 0:.0f} ms, GPU {ev["gpu_busy_ms"]:.0f} ms)',
                               0.1))
        if ev['shader_compiles'] and not any('shader' in c[0] for c in causes):
            causes.append((f'shader/pipeline builds in the window: {ev["shader_compiles"]}', 1))
        if ev.get('crash'):
            causes.append(('a crash right before: ' + ev['crash'][0], 1))
        if ev.get('gov_capped'):
            causes.append((f'CPU clocks capped (SWEET GOV capped {ev["gov_capped"]})', 1))
        if ev.get('temp_max_c') and ev['temp_max_c'] >= 70:
            causes.append((f'hot: {ev["temp_max_c"]:.0f} C', 1))
        causes.sort(key=lambda c: -c[1])
        ev['cause'] = causes[0][0]
        ev['other_causes'] = [c[0] for c in causes[1:4]]
        # not counted: the app itself chose not to draw (idle until a timer, or a low-rate animation)
        ev['fade'] = bool(app_late and not ev['shader_compiles'] and
                          (ev['cause'].startswith('app idle until a timer') or
                           (ev['cause'].startswith('app not committing') and cad and cad >= 1.7)))
        return ev

    # ---- the whole report
    def run(self, shots):
        a = self.a
        per = self.period
        F = self.frames
        gest = self.gestures()
        res = {'span_s': self.t1 - self.t0, 'period_ms': per * 1000, 'frames': len(F), 'gestures': [],
               'slow': [], 'gaps': self.gaps}
        shot_t = sorted((s['t'], s) for s in shots if s.get('t') is not None)

        def shot_near(t):
            if not shot_t:
                return None
            i = bisect.bisect_left([x[0] for x in shot_t], t)
            c = [shot_t[j] for j in (i - 1, i) if 0 <= j < len(shot_t)]
            s = min(c, key=lambda x: abs(x[0] - t))
            return {'png': s[1].get('png'), 'thumb': s[1].get('thumb'), 'dt_s': round(s[0] - t, 2)}

        used_frames = set()
        gpid = {}
        for n, gg in enumerate(gest, 1):
            d, u = gg['down'], gg['up']
            pid = self.touched(d, u + 0.05)
            fpid = self.front(d - 0.3, u + 0.5)
            tgt = pid if pid is not None else fpid
            gpid[n] = tgt
            # end of the settle animation: frames keep coming with gaps < 150 ms, at most 3 s after the up
            i = bisect.bisect_left(F, d)
            end = u
            j = bisect.bisect_right(F, u) - 1 if F else -1
            j = max(j, i - 1)
            k2 = j + 1
            # the first frame after the up may come late (a slow response): up to 1 s
            while k2 < len(F) and F[k2] < u + 3.0 and (F[k2] - max(end, F[k2 - 1] if k2 > 0 else end) < 0.15 or
                                                       (end == u and F[k2] - u < 1.0)):
                end = F[k2]
                k2 += 1
            fr = F[i:k2]
            iv = [(y - x) * 1000 for x, y in zip(fr, fr[1:])]
            hist = collections.Counter(min(4, max(1, round(v / (per * 1000)))) for v in iv)
            rec = {'n': n, 'kind': gg['kind'], 't': round(self.rel(d), 3), 'clock': None,
                   'pos': f'{gg["x0"]},{gg["y0"]}' + ('' if gg['dist'] < 40 else f' -> {gg["x1"]},{gg["y1"]}'),
                   'dur_ms': round(gg['dur'] * 1000), 'samples': gg['samples'], 'app': self.name(tgt) if tgt else '?',
                   'front': self.name(fpid) if fpid else '?', 'frames': len(fr),
                   'anim_ms': round((end - d) * 1000), 'fps': round(len(fr) / max(end - d, 1e-3), 1) if fr else 0,
                   'vsync_hist': {('4+' if k == 4 else str(k)): hist[k] for k in sorted(hist)},
                   'worst_interval_ms': round(max(iv), 1) if iv else None, 'shot': shot_near(d)}
            if iv and tgt is not None:
                med = sorted(iv)[len(iv) // 2] / (per * 1000)
                cad = self.cadence(tgt, end) or self.cadence(tgt, u + 0.3)
                if med >= 1.7:
                    rec['note'] = (f'app-paced: commits every {cad:.1f} vsyncs' if cad and cad >= 1.7 else
                                   f'frames every {med:.1f} vsyncs while the app commits every '
                                   f'{cad:.1f}' if cad else f'frames every {med:.1f} vsyncs')
            # touch -> first frame (from a still screen)
            # a tap without a frame while the finger is down (no highlight): measured from the up
            ref = d
            if gg['kind'] == 'tap' and bisect.bisect_left(F, u) == bisect.bisect_left(F, d):
                ref = u
            rec['resp_from'] = 'up' if ref == u and u != d else 'down'
            pre = bisect.bisect_left(F, ref) - 1
            still = pre < 0 or ref - F[pre] > 0.1
            fi = self.frame_after(ref)
            if fi is not None and still:
                rec['resp_ms'] = round((F[fi] - ref) * 1000, 1)
            elif fi is not None:
                rec['resp_ms'] = None
                rec['resp_note'] = 'screen already animating'
            if gg['kind'] in ('tap', 'long press'):
                fu = self.frame_after(u)
                if fu is not None:
                    rec['up_resp_ms'] = round((F[fu] - u) * 1000, 1)
            # launch: an exec within 2 s after the up of a process that then commits
            for t, nm in self.kl_exec:
                if u - 0.05 <= t <= u + 2.0 and nm not in DAEMONS:
                    lp = [p for p, c in self.pname.items() if c[:15] == nm[:15] and self.commits.get(p)]
                    if lp:
                        c0 = min(self.commits[lp[0]][0], 1e18)
                        ff = self.frame_after(c0)
                        rec['launch'] = {'app': nm, 'exec_ms': round((t - u) * 1000), 'first_commit_ms':
                                         round((c0 - u) * 1000), 'first_frame_ms':
                                         round((F[ff] - u) * 1000) if ff is not None else None}
                        break
            res['gestures'].append(rec)
            # slow: response
            if rec.get('resp_ms') is not None and rec['resp_ms'] > a.resp_ms and fi is not None:
                ev = self.attribute(ref, F[fi], tgt, fi, 'response')
                res['slow'].append({'type': 'touch -> first frame', 't': round(self.rel(ref), 3), 'ms': rec['resp_ms'],
                                    'gesture': n, 'kind': gg['kind'], 'app': rec['app'], 'ev': ev,
                                    'shot': rec['shot']})
                used_frames.add(fi)
            if rec.get('launch') and rec['launch']['first_frame_ms'] and rec['launch']['first_frame_ms'] > a.launch_ms:
                la = rec['launch']
                lp = [p for p, c in self.pname.items() if c[:15] == la['app'][:15] and self.commits.get(p)]
                ff = self.frame_after(u + la['first_commit_ms'] / 1000)
                ev = self.attribute(u, F[ff] if ff is not None else u + la['first_frame_ms'] / 1000,
                                    lp[0] if lp else tgt, ff, 'launch')
                res['slow'].append({'type': 'app launch (tap up -> first app frame)', 't': round(self.rel(u), 3),
                                    'ms': la['first_frame_ms'], 'gesture': n, 'kind': gg['kind'], 'app': la['app'],
                                    'ev': ev, 'shot': shot_near(u + 1)})
        # gaps in running animations (anywhere in the session)
        gap = a.gap_ms / 1000
        anim = 3.6 * per
        touch_iv = [(gg['down'], gg['up']) for gg in gest]
        for i in range(1, len(F)):
            dt = F[i] - F[i - 1]
            if dt <= gap or i in used_frames:
                continue
            prev_ok = i >= 2 and F[i - 1] - F[i - 2] <= anim
            next_ok = i + 1 < len(F) and F[i + 1] - F[i] <= anim
            drag = any(x <= F[i - 1] and y >= F[i] - 0.02 for x, y in touch_iv)
            if not ((prev_ok and next_ok) or (drag and (prev_ok or next_ok))):
                continue
            a0, b0 = F[i - 1], F[i]
            gn = max((k + 1 for k, (x, y) in enumerate(touch_iv) if x - 0.05 <= b0 and y + 3 >= a0), default=None)
            pid = gpid.get(gn) if gn else None
            if pid is None:
                pid = self.front(a0 - 0.5, b0 + 0.2)
            ev = self.attribute(a0, b0, pid, i, 'gap')
            res['slow'].append({'type': 'frame gap in animation' + (' (finger down)' if drag else ''),
                                't': round(self.rel(a0), 3), 'ms': round(dt * 1000, 1), 'gesture': gn,
                                'kind': gest[gn - 1]['kind'] if gn else None,
                                'app': ev['app'], 'ev': ev, 'shot': shot_near(a0)})
        for s in res['slow']:
            s['counted'] = not s['ev'].get('fade') and not s['ev']['cause'].startswith('trace gap')
        res['slow'].sort(key=lambda s: -s['ms'])
        # cadence labels for gestures (median interval >= 2 vsync while the app commits at that rate)
        for rec in res['gestures']:
            pass
        return res


def shots_from_manifest(m, d, chunks):
    out = []
    off = (m.get('clock') or {}).get('phone_minus_pc_s')
    for s in m.get('shots', []):
        mid = (s['pc_t0'] + s['pc_t1']) / 2
        pw = mid + (off or 0.0)
        # phone wall -> mach seconds via the chunk whose wall span holds it (else the nearest)
        best = None
        for c in chunks:
            t = c[0] + (pw - c[2])
            if c[0] - 1 <= t <= c[1] + 1:
                best = t
                break
        if best is None and chunks:
            c = min(chunks, key=lambda c: abs(c[2] - pw))
            best = c[0] + (pw - c[2])
        thumb = s['png'][:-4] + '.thumb.jpg' if s.get('png') else None
        out.append({'t': best, 'png': s.get('png'), 'thumb': thumb if thumb and os.path.exists(os.path.join(d, thumb))
                    else None, 'read_ms': round((s['pc_t1'] - s['pc_t0']) * 1000), 'kind': s['kind']})
    return out


def fmt_ev(ev):
    out = []
    am = ev.get('app_main')
    if am:
        out.append(f'app {ev["app"]} main: on CPU {am["on_ms"]:.0f} ms '
                   f'({", ".join(f"{k} {v:.0f}" for k, v in sorted(am["cpu_ms"].items()))}), runnable '
                   f'{am["runnable_ms"]:.0f}, blocked {am["blocked_ms"]:.0f}'
                   + (f' (wakers {", ".join(f"{w} {v}" for w, v in am["wakers"][:2])})' if am['wakers'] else '')
                   + f'; commits {ev["app_commits"]}'
                   + (f', first after {ev["first_commit_ms"]} ms' if ev['first_commit_ms'] is not None else '')
                   + (f'; cadence {ev["app_cadence_vsync"]} vsync' if ev.get('app_cadence_vsync') else ''))
    bb = ev.get('bb_display')
    if bb:
        out.append(f'backboardd display thread: on CPU {bb["on_ms"]:.0f} ms '
                   f'({", ".join(f"{k} {v:.0f}" for k, v in sorted(bb["cpu_ms"].items()))}), runnable '
                   f'{bb["runnable_ms"]:.0f}, blocked {bb["blocked_ms"]:.0f}'
                   + (f' (wakers {", ".join(f"{w} {v}" for w, v in bb["wakers"][:2])})' if bb['wakers'] else '')
                   + f'; SwapWait/fence waits {ev["bb_wait_ms"]} ms')
    out.append(f'GPU busy {ev["gpu_busy_ms"]} ms, longest fence {ev["gpu_longest_ms"]} ms ({ev["gpu_longest_by"]})'
               + (f', late frame GPU {ev["frame_gpu_ms"]} ms' if ev.get('frame_gpu_ms') is not None else ''))
    cl = ev['cpu_load']
    out.append(f'CPU load little {cl["little"]:.0%} gold {cl["gold"]:.0%} prime {cl["prime"]:.0%}; big cores: '
               + ', '.join(f'{k} {v}' for k, v in cl['big_top']))
    if ev.get('shader_compiles'):
        out.append(f'shader/pipeline builds {ev["shader_compiles"]}'
                   + (f' ({ev["shader_compile_ms"]} ms compile)' if ev.get('shader_compile_ms') else '')
                   + (f': {ev["shader_lines"][0]}' if ev.get('shader_lines') else ''))
    for k in ('exec', 'crash'):
        if ev.get(k):
            out.append(f'{k}: {"; ".join(ev[k])}')
    if ev.get('gov'):
        out.append(f'gov: {ev["gov"]}' + (f'; max temp {ev["temp_max_c"]} C' if ev.get('temp_max_c') else ''))
    return out


def write_html(path, d, res, ana, shots):
    t0 = ana.t0
    span = max(res['span_s'], 1)
    W = 1100
    rows = math.ceil(span / 30)
    F = ana.frames
    per = ana.period * 1000
    parts = ['<!DOCTYPE html><html><head><meta charset="utf-8"><title>UI session</title><style>'
             ':root{--bg:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--s1:#2a78d6;--bad:#e34948;--grid:#e4e3df}'
             '@media (prefers-color-scheme: dark){:root{--bg:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--s1:#3987e5;'
             '--bad:#e66767;--grid:#3a3a38}}'
             'body{background:var(--bg);color:var(--ink);font:13px system-ui,sans-serif;margin:16px}'
             'h1,h2{font-weight:600}table{border-collapse:collapse}td,th{padding:3px 8px;border-bottom:1px solid '
             'var(--grid);text-align:left;vertical-align:top}th{color:var(--ink2)}img.th{height:110px;border-radius:4px}'
             '.ax{fill:var(--ink2);font-size:10px}.bar{fill:var(--s1)}.bad{fill:var(--bad)}.tch{fill:var(--ink2);'
             'opacity:.15}.slow{color:var(--bad);font-weight:600}svg{display:block;margin:6px 0 2px}</style></head><body>']
    parts.append(f'<h1>UI session {html.escape(os.path.basename(os.path.abspath(d)))}</h1>'
                 f'<p>{span:.0f} s, {len(F)} frames on screen, vsync {per:.2f} ms, {len(res["gestures"])} gestures, '
                 f'{sum(1 for s in res["slow"] if s["counted"])} slow moments. Bars: interval between frames on screen '
                 f'(ms, capped at 200); red: slow moments; grey bands: finger down. Hover a mark for details.</p>')
    slow_t = [(s['t'], s) for s in res['slow'] if s['counted']]
    for r in range(rows):
        lo, hi = r * 30, min(span, (r + 1) * 30)
        H = 120
        sx = lambda t: 40 + (t - lo) / 30 * (W - 50)
        sy = lambda v: H - 20 - min(v, 200) / 200 * (H - 30)
        s = [f'<svg width="{W}" height="{H}" role="img" aria-label="frame intervals {lo}-{hi:.0f} s">']
        for v in (50, 100, 200):
            s.append(f'<line x1="40" x2="{W - 10}" y1="{sy(v):.1f}" y2="{sy(v):.1f}" stroke="var(--grid)"/>'
                     f'<text class="ax" x="4" y="{sy(v) + 3:.1f}">{v} ms</text>')
        for gr in res['gestures']:
            if lo <= gr['t'] < hi:
                x = sx(gr['t'])
                w = max(2, gr['dur_ms'] / 1000 / 30 * (W - 50))
                s.append(f'<rect class="tch" x="{x:.1f}" y="4" width="{w:.1f}" height="{H - 24}"><title>#{gr["n"]} '
                         f'{html.escape(gr["kind"])} at {gr["t"]:.2f} s, {html.escape(gr["app"])}</title></rect>')
        for i in range(1, len(F)):
            t = F[i] - t0
            if lo <= t < hi:
                v = (F[i] - F[i - 1]) * 1000
                x = sx(t)
                s.append(f'<rect class="bar" x="{x - 1:.1f}" y="{sy(v):.1f}" width="2" height="{H - 20 - sy(v):.1f}" '
                         f'rx="1"><title>{t:.3f} s: {v:.1f} ms ({v / per:.1f} vsync)</title></rect>')
        for t, sl in slow_t:
            if lo <= t < hi:
                x = sx(t)
                s.append(f'<circle class="bad" cx="{x:.1f}" cy="10" r="5"><title>{html.escape(sl["type"])} '
                         f'{sl["ms"]:.0f} ms: {html.escape(sl["ev"]["cause"])}</title></circle>')
        for tt in range(int(lo), int(hi) + 1, 5):
            s.append(f'<text class="ax" x="{sx(tt):.1f}" y="{H - 5}">{tt} s</text>')
        s.append('</svg>')
        parts.append(''.join(s))
        th = [x for x in shots if x.get('thumb') and x['t'] is not None and lo <= x['t'] - t0 < hi]
        if th:
            parts.append('<div>' + ''.join(f'<img class="th" src="{html.escape(x["thumb"])}" title="{x["t"] - t0:.1f} s">'
                                          for x in th) + '</div>')

    def img(sh):
        if sh and sh.get('thumb'):
            return f'<img class="th" src="{html.escape(sh["thumb"])}">'
        return ''
    parts.append('<h2>Worst moments</h2><table><tr><th>t s</th><th>ms</th><th>what</th><th>app</th><th>cause</th>'
                 '<th>screen</th></tr>')
    for sl in [x for x in res['slow'] if x['counted']][:ana.a.worst]:
        parts.append(f'<tr><td>{sl["t"]:.2f}</td><td class="slow">{sl["ms"]:.0f}</td><td>{html.escape(sl["type"])}'
                     f'{" (#" + str(sl["gesture"]) + " " + html.escape(sl["kind"] or "") + ")" if sl["gesture"] else ""}'
                     f'</td><td>{html.escape(sl["app"])}</td><td>{html.escape(sl["ev"]["cause"])}<br><small>'
                     + '<br>'.join(html.escape(x) for x in fmt_ev(sl['ev'])) + f'</small></td><td>{img(sl["shot"])}</td></tr>')
    parts.append('</table><h2>Gestures</h2><table><tr><th>#</th><th>t s</th><th>gesture</th><th>app</th><th>touch->frame'
                 '</th><th>frames / fps</th><th>vsyncs per frame</th><th>screen</th></tr>')
    for gr in res['gestures']:
        parts.append(f'<tr><td>{gr["n"]}</td><td>{gr["t"]:.2f}</td><td>{html.escape(gr["kind"])} {gr["pos"]} '
                     f'{gr["dur_ms"]} ms</td><td>{html.escape(gr["app"])}</td><td>{gr.get("resp_ms") or "-"}</td>'
                     f'<td>{gr["frames"]} / {gr["fps"]}</td><td>{gr["vsync_hist"]}</td><td>{img(gr["shot"])}</td></tr>')
    parts.append('</table></body></html>')
    open(path, 'w').write('\n'.join(parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--klog')
    ap.add_argument('--gap-ms', type=float, default=50)
    ap.add_argument('--resp-ms', type=float, default=100)
    ap.add_argument('--launch-ms', type=float, default=600)
    ap.add_argument('--worst', type=int, default=15)
    ap.add_argument('--no-html', action='store_true')
    ap.add_argument('--out')
    a = ap.parse_args()
    d = a.dir
    # a pa trace folder has its own report.txt: write next to it in session/ instead
    out = a.out or (d if os.path.exists(os.path.join(d, 'manifest.json')) else os.path.join(d, 'session'))
    os.makedirs(out, exist_ok=True)
    tstart = time.time()
    logl = []

    def log(s):
        logl.append(s)
        print(s, file=sys.stderr)
    m = {}
    if os.path.exists(os.path.join(d, 'manifest.json')):
        m = json.load(open(os.path.join(d, 'manifest.json')))
        files = [os.path.join(d, c['file']) for c in m['chunks']]
    else:
        files = sorted(glob.glob(os.path.join(d, 'chunks', 'c*.swtr'))) or sorted(glob.glob(os.path.join(d, '*.swtr')))
    if not files:
        sys.exit(f'{d}: no chunks/*.swtr or *.swtr')
    g = G()
    log(f'reading {len(files)} trace file(s)')
    for f in files:
        scan_chunk(f, g, log)
    t_read = time.time() - tstart
    lo = min(c[0] for c in g.chunks)
    hi = max(c[1] for c in g.chunks)
    kpath = a.klog or (os.path.join(d, 'klog.txt') if os.path.exists(os.path.join(d, 'klog.txt')) else None)
    klog = read_klog(kpath, lo, hi)
    # a klog of another boot has the same mach times: check the wall clock of hakios-metal lines against the trace's
    hm = [(t, x) for t, x in klog if x.startswith('HakiosMetal[')]
    offs = []
    for t, x in hm[:200]:
        mm = re.match(r'HakiosMetal\[\S+ \d+ (\d\d):(\d\d):(\d\d\.\d+)\]', x)
        if mm:
            offs.append((int(mm.group(1)) * 3600 + int(mm.group(2)) * 60 + float(mm.group(3)) - t) % 86400)
    if offs:
        c0 = g.chunks[0]
        lt = time.localtime(c0[2])
        tr = (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + c0[2] % 1 - c0[0]) % 86400
        ko = sorted(offs)[len(offs) // 2]
        dd = min(abs(ko - tr), 86400 - abs(ko - tr))
        if dd > 5:
            log(f'klog {kpath}: its wall clock is {dd:.0f} s off the trace (another boot?): not used')
            klog = []
    ana = Ana(g, klog, a)
    shots = shots_from_manifest(m, d, g.chunks) if m else []
    res = ana.run(shots)
    t_all = time.time() - tstart
    per = ana.period * 1000
    # ---------------- report.txt
    L = []
    pr = L.append
    wall0 = g.chunks[0][2] if g.chunks else 0
    pr(f'UI session {os.path.abspath(d)}')
    pr(f'{len(files)} trace file(s), {sum(c[4] for c in g.chunks)} events, {res["span_s"]:.1f} s from '
       f'{time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(wall0))} (mach {lo:.3f} s); chunk gaps '
       f'{[round((y - x) * 1000) for x, y in res["gaps"]]} ms; wrapped {sum(c[5] for c in g.chunks)}, dropped '
       f'{sum(c[6] for c in g.chunks)}; klog {len(klog)} lines ({os.path.basename(kpath) if kpath else "none"}); '
       f'{len(shots)} shots')
    pr(f'vsync {per:.2f} ms; {len(ana.frames)} frames on screen; display thread {ana.tname(ana.disp) if ana.disp else "?"}'
       f'; touches from {res["gestures"][0]["samples"] and (g.sw["TOUCH"] and "trace" or "klog") if res["gestures"] else "-"}')
    pr(f'thresholds: frame gap > {a.gap_ms:.0f} ms in a running animation, touch -> first frame > {a.resp_ms:.0f} ms, '
       f'launch > {a.launch_ms:.0f} ms. Times "t" = s from the start. analysis {t_all:.0f} s (reading {t_read:.0f} s)')
    pr('')
    counted = [s for s in res['slow'] if s['counted']]
    pr(f'== worst {min(a.worst, len(counted))} of {len(counted)} slow moments '
       f'({len(res["slow"]) - len(counted)} more labelled app-paced fade / trace gap, not counted)')
    for i, s in enumerate(counted[:a.worst], 1):
        pr(f'{i:2d}. t {s["t"]:8.3f}  {s["ms"]:6.0f} ms  {s["type"]}'
           + (f' [#{s["gesture"]} {s["kind"]}]' if s['gesture'] else '') + f'  app {s["app"]}')
        pr(f'      cause: {s["ev"]["cause"]}')
        for c in s['ev']['other_causes']:
            pr(f'      also:  {c}')
        for x in fmt_ev(s['ev']):
            pr(f'        {x}')
        if s.get('shot') and s['shot'].get('png'):
            pr(f'        screen: {s["shot"]["png"]} ({s["shot"]["dt_s"]:+.1f} s)')
    nf = [s for s in res['slow'] if not s['counted']]
    if nf:
        pr('')
        pr('== not counted (app-paced animations: the app commits every 2+ vsyncs and its main thread is idle; trace gaps)')
        for s in nf[:10]:
            pr(f'    t {s["t"]:8.3f} {s["ms"]:6.0f} ms {s["type"]} app {s["app"]}: {s["ev"]["cause"]}')
    pr('')
    pr('== gestures (resp = touch -> first frame from a still screen: from the down, or for a tap with no frame while '
       'the finger was down from the up (marked u); up = tap up -> next frame)')
    pr(f'{"#":>3} {"t s":>8} {"gesture":24} {"pos":22} {"ms":>5} {"app":16} {"resp":>6} {"up":>6} {"frames":>6} '
       f'{"fps":>5} {"anim ms":>7}  vsyncs/frame 1/2/3/4+   worst')
    for gr in res['gestures']:
        h = gr['vsync_hist']
        pr(f'{gr["n"]:3d} {gr["t"]:8.3f} {gr["kind"][:24]:24} {gr["pos"][:22]:22} {gr["dur_ms"]:5d} {gr["app"][:16]:16} '
           f'{(str(gr["resp_ms"]) + ("u" if gr.get("resp_from") == "up" else "")) if gr.get("resp_ms") is not None else "-":>6} '
           f'{gr.get("up_resp_ms", "-"):>6} '
           f'{gr["frames"]:6d} {gr["fps"]:5.1f} {gr["anim_ms"]:7d}  {h.get("1", 0):3d}/{h.get("2", 0)}/{h.get("3", 0)}'
           f'/{h.get("4+", 0)}'.ljust(22) + f'   {gr["worst_interval_ms"] or "-"}'
           + (f'  LAUNCH {gr["launch"]["app"]}: exec +{gr["launch"]["exec_ms"]} ms, first commit '
              f'+{gr["launch"]["first_commit_ms"]} ms, first frame +{gr["launch"]["first_frame_ms"]} ms'
              if gr.get('launch') else '') + (f'  [{gr["note"]}]' if gr.get('note') else ''))
    # per app
    pr('')
    pr('== per app')
    apps = collections.defaultdict(lambda: {'gestures': 0, 'frames': 0, 'anim_s': 0.0, 'resp': [], 'slow': 0,
                                            'causes': collections.Counter(), 'hist': collections.Counter()})
    for gr in res['gestures']:
        x = apps[gr['app']]
        x['gestures'] += 1
        x['frames'] += gr['frames']
        x['anim_s'] += gr['anim_ms'] / 1000
        if gr.get('resp_ms') is not None:
            x['resp'].append(gr['resp_ms'])
        for k, v in gr['vsync_hist'].items():
            x['hist'][k] += v
    for s in counted:
        x = apps[s['app']]
        x['slow'] += 1
        x['causes'][re.sub(r'[0-9.]+', 'N', s['ev']['cause'].split(':')[0])[:60]] += 1
    for app, x in sorted(apps.items(), key=lambda kv: -kv[1]['gestures']):
        r = sorted(x['resp'])
        pr(f'  {app:18} gestures {x["gestures"]:3d}, fps in gestures {x["frames"] / max(x["anim_s"], 1e-3):5.1f}, '
           f'vsyncs/frame 1/2/3/4+ {x["hist"]["1"]}/{x["hist"]["2"]}/{x["hist"]["3"]}/{x["hist"]["4+"]}, '
           f'resp median {r[len(r) // 2] if r else "-"} ms, slow {x["slow"]}'
           + (f' ({", ".join(f"{k} {n}" for k, n in x["causes"].most_common(3))})' if x['causes'] else ''))
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(L) + '\n')
    res['log'] = logl
    res['analysis_s'] = t_all
    json.dump(res, open(os.path.join(out, 'session.json'), 'w'), indent=1, default=str)
    if not a.no_html:
        write_html(os.path.join(out, 'report.html'), out, res, ana, shots)
    print('\n'.join(L[:3 + min(a.worst, len(counted)) * 6 + 4]))
    print(f'-> {os.path.join(out, "report.txt")}, session.json' + ('' if a.no_html else ', report.html'))


if __name__ == '__main__':
    main()
