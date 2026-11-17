#!/usr/bin/env python3
"""pstk.py OUT NAME... : pstack every phone process whose path contains NAME, symbolicate with ipsw + the
iOS 16.7 shared cache in ~/redfin-work/dsc, write OUT. Run in WSL. (2026-10-28, storage/Files investigation)"""
import os, re, subprocess, sys
HOME = os.path.expanduser('~')
IPSW = HOME + '/redfin-build/toolchain/bin/ipsw'; DSC = HOME + '/redfin-work/dsc/dyld_shared_cache_arm64'
SSH = ['ssh', '-i', HOME + '/.ssh/redfin-claude', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
       '-o', 'ConnectTimeout=10', '-o', 'LogLevel=ERROR', 'root@192.168.7.2']
out = open(sys.argv[1], 'w')
def ph(cmd): return subprocess.run(SSH + [cmd], capture_output=True, text=True, timeout=60).stdout
ps = ph('/bin/ps -Ao pid,comm')
memo = {}
def sym(a, slide):
    if a < 0x180000000 + slide: return '(main binary)'
    if a not in memo:
        r = subprocess.run([IPSW, 'dyld', 'a2s', DSC, hex(a), '-s', hex(slide)],
                           capture_output=True, text=True, timeout=120)
        o = (r.stdout.strip() or r.stderr.strip()).split('\n')[-1]
        o = re.sub(r'^0x[0-9a-f]+: ', '', o); o = re.sub(r'\x1b\[[0-9;]*m', '', o)
        memo[a] = o
    return memo[a]
for name in sys.argv[2:]:
    for line in ps.splitlines()[1:]:
        pid, comm = line.split(None, 1)
        if name.lower() not in comm.lower(): continue
        st = ph('/var/local/bin/pstack ' + pid)
        m = re.search(r'slide 0x([0-9a-f]+)', st)
        print('==== %s %s' % (pid, comm), file=out, flush=True)
        if not m: print(st, file=out); continue
        slide = int(m.group(1), 16)
        for l in st.splitlines():
            t = re.match(r'thread (\d+): pc (0x[0-9a-f]+) lr (0x[0-9a-f]+)', l)
            if t:
                print('  thread %s  pc %s' % (t.group(1), sym(int(t.group(2), 16), slide)), file=out); continue
            if l.strip().startswith('bt'):
                for x in l.split()[1:]:
                    print('      %s' % sym(int(x, 16), slide), file=out)
        out.flush()
print('DONE', file=out)
