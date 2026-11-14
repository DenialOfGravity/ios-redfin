#!/usr/bin/env python3
"""state.py: the redfin phone's desired state (state/redfin.toml, in git) against what is on the phone.

  state.py dump [OUT]                one phone dump (phone/tools/sweetstate.sh over ssh) -> OUT (default stdout summary)
  state.py diff [--dump F] [--json]  drift report: missing / different / unexpected files, flags, sysctls, processes,
                                     boot (kernel UUID, boot-args), retired paths still present, temp overrides
  state.py apply [--dry-run] [--retire] [--yes-restart]
                                     install exactly the drifted files and flags, set drifted sysctls, restart only the
                                     processes those entries name, re-dump and verify. Never reboots, never flashes,
                                     refuses paths outside [managed].roots and anything under [managed].firmware.
                                     --retire: also MOVE retired paths still present into /var/local/scratch/retired-DATE/
                                     (gone at the next boot once the scratch wipe is in sensd-probe.sh; ask the user first)
  state.py adopt [--dump F] [--out FILE] [--known-good KNOWN-GOOD.txt]
                                     write the phone's current state as a manifest (bootstrap / re-baseline)
  state.py snapshot [LABEL]          dated dump + diff under PF/state/snapshots/, prints the lines for the status doc
  state.py set KEY VALUE --temp      experiment: set a sysctl (kern.*) or a KEY=VALUE line of a flag file
                                     (/var/local/FILE:KEY) now, recorded in PF/state/temp-overrides.json
  state.py revert KEY|all            put a temp override back to the manifest value and drop it
  state.py promote KEY --manifest FILE   write a temp override's value into the manifest file, drop it
  state.py summary                   the manifest as a short text (what KNOWN-GOOD.txt used to say)
  state.py index                     (re)build PF/state/build-index.json: sha256 -> builds/... (SHA256SUMS, agent/stage)

Common options: --manifest SPEC (a file, or git:REF:PATH; default PF/state/manifest-ref.txt, else
git:origin/main:state/redfin.toml), --via agent|direct (default: direct inside the phone agent, which sets
PHONE_AGENT_INSIDE=1; agent from anywhere else: requests through PF/agent/inbox like any Claude request).
Environment: PF (project folder, default /mnt/d/ClaudeProjects/iOS_Pixel5), REPO (default ~/ios-redfin).
Nothing personal is stored: the dump never carries secret files' contents nor PIN/ICCID/IMSI-like lines.
"""
import argparse, base64, datetime, fnmatch, gzip, hashlib, json, os, plistlib, re, shlex, struct, subprocess, sys, time

try:
    import tomllib
except ImportError:  # python < 3.11
    tomllib = None

PF = os.environ.get('PF', '/mnt/d/ClaudeProjects/iOS_Pixel5')
REPO = os.path.expanduser(os.environ.get('REPO', '~/ios-redfin'))
STATE = os.path.join(PF, 'state')
TEMP = os.path.join(STATE, 'temp-overrides.json')
INDEX = os.path.join(STATE, 'build-index.json')
PHONE = os.environ.get('PHONE_HOST', '192.168.7.2')
PSSH = ['ssh', '-i', os.path.expanduser('~/.ssh/redfin-claude'), '-o', 'StrictHostKeyChecking=no', '-o',
        'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=10', '-o', 'LogLevel=ERROR', '-o', 'BatchMode=yes',
        f'root@{PHONE}']
T = '/var/local/bin/toybox'
FIRMWARE_ALWAYS = ['/dev/*', '/var/local/fw/*', '/var/local/fw', '*/rmtfs/*']  # apply never writes these


def die(msg, code=2):
    print(f'state: {msg}', file=sys.stderr)
    sys.exit(code)


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def gmatch(path, globs):
    return any(fnmatch.fnmatchcase(path, g) or fnmatch.fnmatchcase(os.path.basename(path), g) for g in globs)


def under(path, roots):
    return any(path == r or path.startswith(r.rstrip('/') + '/') for r in roots)


# ---------------------------------------------------------------- manifest: read (tomllib) and write (own, stable)

def default_spec():
    f = os.path.join(STATE, 'manifest-ref.txt')
    if os.path.exists(f):
        s = open(f).read().strip()
        if s:
            return s
    return 'git:origin/main:state/redfin.toml'


def git_show(ref, path):
    return subprocess.run(['git', '-C', REPO, 'show', f'{ref}:{path}'], capture_output=True, check=True).stdout


def load_manifest(spec):
    spec = spec or default_spec()
    if spec.startswith('git:'):
        _, ref, path = spec.split(':', 2)
        if ref.startswith('origin/') and not os.environ.get('STATE_NO_FETCH'):
            subprocess.run(['git', '-C', REPO, 'fetch', '-q', 'origin', ref[len('origin/'):]], capture_output=True, timeout=60)
        try:
            data = git_show(ref, path)
        except subprocess.CalledProcessError as e:
            die(f'cannot read {path} at {ref} in {REPO}: {e.stderr.decode().strip()}')
    else:
        data = open(spec, 'rb').read()
    if tomllib is None:
        die('needs python 3.11+ (tomllib)')
    m = tomllib.loads(data.decode())
    m.setdefault('_spec', spec)
    return m


def tq(s):
    """TOML basic string"""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif o < 0x20 or o == 0x7f:
            out.append(f'\\u{o:04x}')
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def tml(s):
    """TOML multi-line basic string (readable flag contents)"""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch in '\n\t':
            out.append(ch)
        elif o < 0x20 or o == 0x7f:
            out.append(f'\\u{o:04x}')
        else:
            out.append(ch)
    return '"""\n' + ''.join(out) + '"""'


def tkey(k):
    return k if re.fullmatch(r'[A-Za-z0-9_-]+', k) else tq(k)


def tval(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return tml(v) if '\n' in v else tq(v)
    if isinstance(v, list):
        if not v:
            return '[]'
        inner = [tval(x) for x in v]
        if sum(len(x) for x in inner) > 80:
            return '[\n' + ''.join(f'  {x},\n' for x in inner) + ']'
        return '[' + ', '.join(inner) + ']'
    if isinstance(v, dict):
        if not v:
            return '{}'
        return '{ ' + ', '.join(f'{tkey(k)} = {tval(x)}' for k, x in v.items()) + ' }'
    raise TypeError(type(v))


SECTION_ORDER = ['meta', 'boot', 'managed', 'sysctl', 'sysctl_expect', 'process', 'file', 'bundle', 'link', 'flag',
                 'retired']
COMMENTS = {
    'meta': 'Desired state of the redfin phone. Edit in git, then `state apply` (tools/state.py, state/README.md).',
    'boot': 'What runs: image/kernel/cmdline. Never applied by state.py (no reboots): `cycle` / `boot` do that; diff checks kern.uuid and kern.bootargs.',
    'managed': 'Paths state.py may write (roots) and how the dump is read. Outside roots or under firmware: apply refuses.',
    'sysctl': 'Applied at every boot: generated into /var/local/sweet-sysctl.conf (read by /var/local/bin/sweet-sysctl.sh, started from sensd-probe.sh = launchd job org.iosonpixel.sensdprobe). apply also sets the running value.',
    'sysctl_expect': 'Runtime values diff checks but that are NOT written to the conf (kernel defaults, boot-args); apply sets them with sctl.',
    'process': 'Processes expected up. restart = the command apply runs after installing something that names this process.',
    'file': 'Single files. source = PF-relative path of the build output (builds/<target>/<ref>-<sha10>/NAME), agent/stage/..., "stock" (Apple, untouched) or "unknown" (provenance not found: resolve or retire).',
    'bundle': 'Bundle directories: every file with its sha256; apply installs the whole bundle from source.',
    'link': 'Symlinks.',
    'flag': 'Small config/flag/env files with their content (content wins; sha256 only for secret or redacted files: never their content).',
    'retired': 'Must NOT exist. diff lists them while present; `apply --retire` moves them to /var/local/scratch/retired-DATE/.',
}


def dump_toml(m):
    out = []
    for sec in SECTION_ORDER + [k for k in m if k not in SECTION_ORDER and not k.startswith('_')]:
        if sec not in m:
            continue
        v = m[sec]
        if sec in COMMENTS:
            out.append('')
            out.append('# ' + COMMENTS[sec])
        if isinstance(v, list):
            for item in v:
                out.append(f'[[{sec}]]')
                for k, x in item.items():
                    out.append(f'{tkey(k)} = {tval(x)}')
                out.append('')
        else:
            out.append(f'[{sec}]')
            for k, x in v.items():
                out.append(f'{tkey(k)} = {tval(x)}')
    return '\n'.join(out).lstrip('\n') + '\n'


# ---------------------------------------------------------------- transport: the phone agent's inbox, or ssh directly

class Direct:
    name = 'direct'

    def sh(self, cmd, stdin=None, timeout=300):
        r = subprocess.run(PSSH + [cmd], input=stdin, capture_output=True, timeout=timeout)
        return r.returncode, r.stdout.decode(errors='replace') + r.stderr.decode(errors='replace')

    def dump(self, script, args):
        cmd = '/var/local/bin/dash -s -- ' + ' '.join(shlex.quote(a) for a in args)
        rc, out = self.sh(cmd, stdin=script.encode(), timeout=600)
        if rc:
            die(f'dump failed (ssh exit {rc}): {out[-400:]}')
        return out

    def script(self, text, timeout=300):
        return self.sh('/var/local/bin/dash -s', stdin=text.encode(), timeout=timeout)

    def put(self, local, remote, mode, owner):
        tmp = remote + '.st-new'
        cmd = (f"{T} mkdir -p {shlex.quote(os.path.dirname(remote))} && cat > {shlex.quote(tmp)} && "
               f"{T} chmod {mode} {shlex.quote(tmp)} && {T} chown {owner} {shlex.quote(tmp)} && "
               f"{T} mv -f {shlex.quote(tmp)} {shlex.quote(remote)} && {T} sha256sum {shlex.quote(remote)}")
        with open(local, 'rb') as f:
            return self.sh(cmd, stdin=f.read())

    def write(self, data, remote, mode='644', owner='0:0'):
        tmp = remote + '.st-new'
        cmd = (f"cat > {shlex.quote(tmp)} && {T} chmod {mode} {shlex.quote(tmp)} && {T} chown {owner} {shlex.quote(tmp)}"
               f" && {T} mv -f {shlex.quote(tmp)} {shlex.quote(remote)}")
        return self.sh(cmd, stdin=data)

    def pull(self, remote, local):
        os.makedirs(os.path.dirname(local), exist_ok=True)
        r = subprocess.run(PSSH + [f'cat {shlex.quote(remote)}'], capture_output=True, timeout=600)
        if r.returncode == 0:
            open(local, 'wb').write(r.stdout)
        return r.returncode


class Agent:
    """The request files of tools-claude/phone-agent.sh; one request at a time, unique names."""
    name = 'agent'

    def __init__(self):
        self.n = 0
        self.tag = now_utc().strftime('st%H%M%S')

    def req(self, line, timeout=600):
        self.n += 1
        name = f'{self.tag}-{os.getpid()}-{self.n}'
        inbox, outbox = os.path.join(PF, 'agent/inbox'), os.path.join(PF, 'agent/outbox')
        open(os.path.join(inbox, name + '.tmp'), 'w').write(line + '\n')
        os.replace(os.path.join(inbox, name + '.tmp'), os.path.join(inbox, name + '.req'))
        out = os.path.join(outbox, name + '.out')
        end = time.time() + timeout
        while not os.path.exists(out):
            if time.time() > end:
                die(f'no answer from the phone agent for {name} (is it running? busy with a cycle/bench?)')
            time.sleep(1)
        text = open(out, errors='replace').read()
        m = re.search(r'== exit (\d+) at', text)
        rc = int(m.group(1)) if m else 1
        body = text.split('\n', 1)[1] if '\n' in text else ''
        body = re.sub(r'\n?== exit \d+ at [0-9:]+\s*$', '', body)
        return rc, body

    def sh(self, cmd, stdin=None, timeout=300):
        if stdin is not None:
            raise RuntimeError('agent transport: no stdin')
        return self.req('phone ' + cmd, timeout)

    def dump(self, script, args):
        # dropbear refuses a command over ~9000 bytes: gzip + base64 the script, short sysctl names
        lean = '\n'.join(l.strip() for l in script.splitlines() if l.strip() and not l.strip().startswith('#')) + '\n'
        b = base64.b64encode(gzip.compress(lean.encode(), 9, mtime=0)).decode()
        cmd = (f'echo {b} | {T} base64 -d | {T} zcat | /var/local/bin/dash -s -- ' +
               ' '.join(shlex.quote(a) for a in args))
        if len(cmd) > 8800:
            die(f'dump command too long for dropbear ({len(cmd)} bytes): use the new agent (state-dump) or fewer -n')
        rc, out = self.req('phone ' + cmd, 600)
        if rc:
            die(f'dump failed (exit {rc}): {out[-400:]}')
        return out

    def script(self, text, timeout=300):
        b = base64.b64encode(gzip.compress(text.encode(), 9, mtime=0)).decode()
        cmd = f'echo {b} | {T} base64 -d | {T} zcat | /var/local/bin/dash -s'
        if len(cmd) > 8800:
            die(f'script too long for dropbear ({len(cmd)} bytes)')
        return self.req('phone ' + cmd, timeout)

    def put(self, local, remote, mode, owner):
        rc, out = self.req(f'files put {local} {remote} {mode}')
        if rc == 0 and owner != '0:0':
            rc, o2 = self.sh(f'{T} chown {owner} {shlex.quote(remote)}')
            out += o2
        if rc == 0:
            rc, o3 = self.sh(f'{T} sha256sum {shlex.quote(remote)}')
            out += o3
        return rc, out

    def write(self, data, remote, mode='644', owner='0:0'):
        tmpl = os.path.join(STATE, 'tmp', f'w-{os.getpid()}-{self.n}')
        os.makedirs(os.path.dirname(tmpl), exist_ok=True)
        open(tmpl, 'wb').write(data)
        rc, out = self.put(tmpl, remote, mode, owner)
        os.unlink(tmpl)
        return rc, out

    def pull(self, remote, local):
        rel = os.path.relpath(local, PF)
        rc, _ = self.req(f'pull {remote} {rel}')
        return rc


def transport(via):
    via = via or ('direct' if os.environ.get('PHONE_AGENT_INSIDE') else 'agent')
    return Direct() if via == 'direct' else Agent()


# ---------------------------------------------------------------- the dump

def sweetstate_script(m):
    spec = (m or {}).get('_spec', default_spec())
    if spec.startswith('git:'):
        ref = spec.split(':', 2)[1]
        try:
            return git_show(ref, 'phone/tools/sweetstate.sh').decode()
        except subprocess.CalledProcessError:
            pass
    for p in (os.path.join(os.path.dirname(os.path.abspath(spec)), '..', 'phone/tools/sweetstate.sh') if not spec.startswith('git:') else '',
              os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'phone/tools/sweetstate.sh'),
              os.path.join(PF, 'tools-claude/sweetstate.sh')):
        if os.path.exists(p):
            return open(p).read()
    die('no phone/tools/sweetstate.sh')


def sysctl_names(m):
    names = set((m or {}).get('sysctl', {})) | set((m or {}).get('sysctl_expect', {}))
    f = os.path.join(STATE, 'sysctl-names.txt')  # kern.sweet_* from xnu/patches (state.py names)
    if os.path.exists(f):
        names |= {x.strip() for x in open(f) if x.strip()}
    return sorted(names)


def dump_args(m, short=False):
    args = []
    mg = (m or {}).get('managed') or DEFAULT_MANAGED
    for g in mg.get('nohash', []):
        args += ['-x', g]
    for n in sysctl_names(m):
        args += ['-n', n[len('kern.sweet_'):] if short and n.startswith('kern.sweet_') else n]
    return args


def take_dump(m, tp):
    script = sweetstate_script(m)
    text = tp.dump(script, dump_args(m, short=tp.name == 'agent'))
    return text


class Dump:
    def __init__(self, text):
        self.h, self.files, self.sha, self.links, self.content, self.ignored = {}, {}, {}, {}, {}, {}
        self.sysctl, self.procs, self.maps, self.errors = {}, [], [], []
        root = None
        for line in text.splitlines():
            if not line or line.startswith('#') or line.startswith('$ '):
                continue
            f = line.split('\t')
            t = f[0]
            try:
                if t == 'H':
                    self.h[f[1]] = '\t'.join(f[2:])
                elif t == 'R':
                    root = f[1]
                elif t == 'F':
                    p = self.join(root, f[7])
                    self.files[p] = dict(type=f[1], mode=f[2], uid=int(f[3]), gid=int(f[4]), size=int(f[5]),
                                         mtime=int(f[6]))
                elif t == 'K':
                    self.sha[self.join(root, f[2])] = f[1]
                elif t == 'L':
                    self.links[self.join(root, f[1])] = f[2]
                elif t == 'C':
                    p = f[1]
                    p = '/var/local' + p[len('/private/var/local'):] if p.startswith('/private/var/local') else \
                        self.join('/var/local', p) if not p.startswith('/') else p
                    self.content[p] = (int(f[2]), base64.b64decode(f[3]) if len(f) > 3 else b'')
                elif t == 'I':
                    self.ignored[self.join(root, f[1])] = (int(f[2] or 0), int(f[3] or 0))
                elif t == 'S':
                    self.sysctl[f[1]] = f[2] if len(f) > 2 else ''
                elif t == 'P':
                    self.procs.append(dict(pid=int(f[1]), ppid=int(f[2]), uid=int(f[3]),
                                           start=None if f[4] == '-' else int(f[4]), comm=f[5]))
                elif t == 'M':
                    self.maps.append(dict(pid=int(f[1]), comm=f[2], path=f[3]))
                elif t == 'E':
                    self.errors.append('\t'.join(f[1:]))
            except (IndexError, ValueError) as e:
                self.errors.append(f'bad line ({e}): {line[:120]}')
        if 'end' not in self.h:
            self.errors.append('dump incomplete (no end record)')

    @staticmethod
    def join(root, rel):
        if rel.startswith('/'):
            return rel
        rel = rel[2:] if rel.startswith('./') else rel
        return root if rel in ('', '.') else root.rstrip('/') + '/' + rel

    def running(self):
        return {p['comm'] for p in self.procs}


def read_dump(path):
    if path:
        return Dump(open(path, errors='replace').read())
    return None


# ---------------------------------------------------------------- provenance index: sha256 -> builds/... and agent/stage

def build_index(refresh=False):
    old = {}
    if os.path.exists(INDEX) and not refresh:
        old = json.load(open(INDEX))
    idx = {'sums': old.get('sums', {}), 'stage': old.get('stage', {}), 'kernels': old.get('kernels', {})}
    bdir = os.path.join(PF, 'builds')
    for tgt in sorted(os.listdir(bdir)) if os.path.isdir(bdir) else []:
        td = os.path.join(bdir, tgt)
        if not os.path.isdir(td) or tgt == 'logs':
            continue
        for d in sorted(os.listdir(td)):
            dd = os.path.join(td, d)
            sf = os.path.join(dd, 'SHA256SUMS')
            if not os.path.isfile(sf):
                continue
            key = f'builds/{tgt}/{d}'
            mt = os.path.getmtime(sf)
            if key in idx['sums'] and idx['sums'][key]['mtime'] == mt:
                continue
            info = {'mtime': mt, 'files': {}, 'ref': '', 'source': '', 'target': tgt}
            bt = os.path.join(dd, 'BUILD.txt')
            if os.path.exists(bt):
                for line in open(bt, errors='replace'):
                    k, _, v = line.strip().partition(' ')
                    if k in ('ref', 'source', 'target'):
                        info[k] = v
            for line in open(sf, errors='replace'):
                parts = line.strip().split(None, 1)
                if len(parts) == 2 and re.fullmatch(r'[0-9a-f]{64}', parts[0]):
                    rel = parts[1].lstrip('*')
                    rel = rel[2:] if rel.startswith('./') else rel
                    info['files'][rel] = parts[0]
            idx['sums'][key] = info
            if tgt == 'xnu' and key not in idx['kernels']:
                u = kernel_uuid(dd)
                if u:
                    idx['kernels'][key] = u
    sd = os.path.join(PF, 'agent/stage')
    if os.path.isdir(sd):
        for n in sorted(os.listdir(sd)):
            p = os.path.join(sd, n)
            if os.path.isfile(p) and os.path.getsize(p) < (64 << 20):
                mt = os.path.getmtime(p)
                k = f'agent/stage/{n}'
                if k not in idx['stage'] or idx['stage'][k][0] != mt:
                    idx['stage'][k] = [mt, sha256_file(p)]
    os.makedirs(STATE, exist_ok=True)
    json.dump(idx, open(INDEX + '.tmp', 'w'))
    os.replace(INDEX + '.tmp', INDEX)
    return idx


def kernel_uuid(d):
    """LC_UUID of builds/xnu/X/kernel.development.sweet(.gz): the phone's kern.uuid"""
    for n in ('kernel.development.sweet', 'kernel.development.sweet.gz'):
        p = os.path.join(d, n)
        if not os.path.exists(p):
            continue
        try:
            op = gzip.open if n.endswith('.gz') else open
            with op(p, 'rb') as f:
                hdr = f.read(1 << 16)
            magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', hdr)
            if magic != 0xfeedfacf:
                return None
            off = 32
            for _ in range(ncmds):
                cmd, size = struct.unpack_from('<II', hdr, off)
                if cmd == 0x1b:
                    u = hdr[off + 8:off + 24].hex().upper()
                    return f'{u[:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:]}'
                off += size
        except Exception:
            return None
    return None


def by_sha(idx):
    """sha256 -> best source string (newest build dir first, then agent/stage)"""
    out = {}
    for key, info in sorted(idx['sums'].items(), key=lambda kv: -kv[1]['mtime']):
        for rel, h in info['files'].items():
            out.setdefault(h, f'{key}/{rel}')
    for k, (mt, h) in idx['stage'].items():
        out.setdefault(h, k)
    return out


def src_info(idx, source):
    """builds/<tgt>/<dir>/... -> (target, ref, sha10)"""
    m = re.match(r'builds/([^/]+)/([^/]+)', source or '')
    if not m:
        return None
    info = idx['sums'].get(f'builds/{m.group(1)}/{m.group(2)}', {})
    return info.get('target', m.group(1)), info.get('ref', ''), (info.get('source', '') or '')[:10]


# ---------------------------------------------------------------- diff

class Drift:
    def __init__(self):
        self.items = []

    def add(self, kind, path, detail='', fix=None):
        self.items.append(dict(kind=kind, path=path, detail=detail, fix=fix))

    def of(self, *kinds):
        return [i for i in self.items if i['kind'] in kinds]


DRIFT_KINDS = ['boot', 'missing', 'hash', 'mode', 'owner', 'link', 'flag', 'secret', 'unexpected', 'sysctl',
               'process', 'retired', 'temp', 'temp-lost', 'dump']
INFO_KINDS = {'retired', 'temp', 'dump'}  # reported, but not "drift" for the exit code


def manifest_paths(m):
    """every path the manifest names (files, bundle files, links, flags) -> kind"""
    out = {}
    for f in m.get('file', []):
        out[f['path']] = 'file'
    for b in m.get('bundle', []):
        out[b['path']] = 'bundle'
        for rel in b.get('files', {}):
            out[b['path'] + '/' + rel] = 'bundle'
    for l in m.get('link', []):
        out[l['path']] = 'link'
    for f in m.get('flag', []):
        out[f['path']] = 'flag'
    if m.get('sysctl'):
        out['/var/local/sweet-sysctl.conf'] = 'sysctl-conf'
    return out


def parse_sysctl_conf(text):
    out = {}
    for line in text.splitlines():
        parts = line.split('#', 1)[0].split()
        if len(parts) >= 2 and re.fullmatch(r'-?\d+', parts[1]):
            out[parts[0]] = int(parts[1])
    return out


def sysctl_conf_text(m):
    lines = ['# generated by tools/state.py from [sysctl] in state/redfin.toml: do not edit on the phone']
    lines += [f'{k} {v}' for k, v in m.get('sysctl', {}).items()]
    return '\n'.join(lines) + '\n'


def flag_desired(f):
    """(sha256, bytes or None); redacted flags: (None, template bytes with @PHONE@)"""
    if 'content' in f and f.get('redacted') and not f.get('secret'):
        return None, f['content'].encode()
    if 'content' in f and not f.get('redacted') and not f.get('secret'):
        b = f['content'].encode()
        return hashlib.sha256(b).hexdigest(), b
    return f.get('sha256'), None


FILL_SH = r'''T=/var/local/bin/toybox
f=%(path)s out=$f.st-new
[ -f "$f" ] || { echo "no $f: cannot fill @PHONE@ values"; exit 3; }
: > "$out" || exit 3
set -f
while IFS= read -r l || [ -n "$l" ]; do
	case $l in
	*@PHONE@*)
		r=
		for t in $l; do
			case $t in
			*=@PHONE@)
				k=${t%%%%=*} v=
				while IFS= read -r ol || [ -n "$ol" ]; do for ot in $ol; do [ "${ot%%%%=*}" = "$k" ] && v=${ot#*=}; done; done < "$f"
				[ -n "$v" ] || { echo "no $k= in $f: not written"; $T rm -f "$out"; exit 3; }
				t=$k=$v;;
			@PHONE@) echo "bare @PHONE@ in the template: not written"; $T rm -f "$out"; exit 3;;
			esac
			r="$r${r:+ }$t"
		done
		printf '%%s\n' "$r" >> "$out";;
	*) printf '%%s\n' "$l" >> "$out";;
	esac
done <<'ST_TEMPLATE_EOF'
%(template)s
ST_TEMPLATE_EOF
$T chmod %(mode)s "$out" && $T chown %(owner)s "$out" && $T mv -f "$out" "$f" && echo "filled $f"
'''


def write_flag(tp, path, text, mode='644', owner='0:0'):
    """write a flag file; @PHONE@ placeholders (redacted secrets) are filled on the phone from the file there"""
    if '@PHONE@' not in text:
        return tp.write(text.encode(), path, mode, owner)
    if 'ST_TEMPLATE_EOF' in text:
        return 1, 'bad template'
    body = text[:-1] if text.endswith('\n') else text
    return tp.script(FILL_SH % dict(path=shlex.quote(path), template=body, mode=mode, owner=owner))


def load_temp():
    return json.load(open(TEMP)) if os.path.exists(TEMP) else {}


def compute_diff(m, d):
    dr = Drift()
    mg = m.get('managed', {})
    roots, ignore = mg.get('roots', []), mg.get('ignore', [])
    volatile, stock = mg.get('volatile', []), mg.get('stock', [])
    for e in d.errors:
        dr.add('dump', '', e)
    # boot
    b = m.get('boot', {})
    if b.get('kernel_uuid') and d.h.get('kern.uuid') and b['kernel_uuid'] != d.h['kern.uuid']:
        dr.add('boot', 'kernel', f"running kern.uuid {d.h['kern.uuid']} != manifest {b['kernel_uuid']} "
                                 f"({b.get('kernel_dir', '?')}): boot the manifest's kernel with `cycle`, or adopt")
    if b.get('bootargs') and d.h.get('kern.bootargs') is not None and b['bootargs'].strip() != d.h['kern.bootargs'].strip():
        a, c = set(b['bootargs'].split()), set(d.h['kern.bootargs'].split())
        dr.add('boot', 'bootargs', f"only in manifest: {' '.join(sorted(a - c)) or '-'}; only running: {' '.join(sorted(c - a)) or '-'}")
    temp = load_temp()
    # files
    for f in m.get('file', []):
        p = f['path']
        cur = d.files.get(p)
        if not cur:
            dr.add('missing', p, f.get('source', ''), fix='install')
            continue
        if f.get('sha256') and d.sha.get(p) and d.sha[p] != f['sha256']:
            dr.add('hash', p, f"phone {d.sha[p][:12]} != {f['sha256'][:12]} ({f.get('source', '')})", fix='install')
        elif f.get('sha256') and p not in d.sha and cur['type'] == 'f' and not gmatch(p, mg.get('nohash', [])):
            dr.add('hash', p, 'no hash in the dump')
        if f.get('mode') and cur['mode'] != f['mode']:
            dr.add('mode', p, f"{cur['mode']} != {f['mode']}", fix='chmod')
        if f.get('owner') and f"{cur['uid']}:{cur['gid']}" != f['owner']:
            dr.add('owner', p, f"{cur['uid']}:{cur['gid']} != {f['owner']}", fix='chown')
    for bd in m.get('bundle', []):
        p = bd['path']
        if p not in d.files:
            dr.add('missing', p, bd.get('source', ''), fix='install')
            continue
        bad = []
        for rel, h in bd.get('files', {}).items():
            q = p + '/' + rel
            if q not in d.files:
                bad.append(f'{rel} missing')
            elif d.sha.get(q) and d.sha[q] != h:
                bad.append(f'{rel} {d.sha[q][:12]}!={h[:12]}')
        extra = [q[len(p) + 1:] for q, x in d.files.items() if q.startswith(p + '/') and x['type'] == 'f'
                 and q[len(p) + 1:] not in bd.get('files', {})]
        bad += [f'{e} unexpected' for e in extra]
        if bad:
            dr.add('hash', p, '; '.join(bad[:6]) + (f' (+{len(bad) - 6})' if len(bad) > 6 else ''), fix='install')
    for l in m.get('link', []):
        if d.links.get(l['path']) != l['target']:
            dr.add('link', l['path'], f"{d.links.get(l['path'], 'missing')} != {l['target']}", fix='link')
    tflag = {k.split(':', 1)[0] for k in temp if k.startswith('/')}
    for f in m.get('flag', []):
        p = f['path']
        want, data = flag_desired(f)
        if p not in d.files:
            if f.get('absent_ok'):
                continue
            dr.add('missing', p, 'flag', fix='write' if data is not None else None)
            continue
        have = d.sha.get(p)
        if f.get('redacted') and data is not None:
            got = d.content.get(p)
            if got is not None and got[1] != data and p not in tflag:
                dr.add('flag', p, short_delta(data.decode(errors='replace'), got[1].decode(errors='replace'))
                       + ' (secret values compared as @PHONE@)', fix='write')
            continue
        if want and have and want != have:
            if p in tflag:
                continue
            kind = 'secret' if f.get('secret') or f.get('redacted') else 'flag'
            got = d.content.get(p)
            det = ''
            if got and data is not None and not got[0]:
                det = short_delta(data.decode(errors='replace'), got[1].decode(errors='replace'))
            dr.add(kind, p, det or f'{have[:12]} != {want[:12]}', fix='write' if data is not None else None)
    if m.get('sysctl'):
        p = '/var/local/sweet-sysctl.conf'
        got = parse_sysctl_conf(d.content.get(p, (0, b''))[1].decode(errors='replace')) if p in d.files else None
        want = {k: int(v) for k, v in m['sysctl'].items()}
        if got != want:
            if got is None:
                det = 'missing'
            else:
                det = ', '.join([f'{k} {got.get(k, "-")}->{v}' for k, v in want.items() if got.get(k) != v] +
                                [f'{k} {v} not in [sysctl]' for k, v in got.items() if k not in want])
            dr.add('flag', p, det or 'differs from [sysctl]', fix='write-sysctl-conf')
    # sysctls (running values)
    for sec in ('sysctl', 'sysctl_expect'):
        for k, v in m.get(sec, {}).items():
            if k in temp:
                continue
            got = d.sysctl.get(k)
            if got is None:
                if d.sysctl:
                    dr.add('sysctl', k, f'not readable on the running kernel (manifest {v})')
                continue
            if str(got).strip() != str(v):
                dr.add('sysctl', k, f'running {got} != {v} ({sec})', fix='sctl')
    # temp overrides
    for k, t in temp.items():
        if k.startswith('kern.'):
            got = d.sysctl.get(k)
            if got is not None and str(got) != str(t['value']):
                dr.add('temp-lost', k, f"temp {t['value']} but running {got} (reboot?) - `state revert {k}`")
            else:
                dr.add('temp', k, f"{t['value']} (manifest {t.get('orig')}, by {t.get('by', '?')} {t.get('at', '')})")
        else:
            dr.add('temp', k, f"{t['value']} (manifest {t.get('orig')}, {t.get('at', '')})")
    # processes
    run = d.running()
    for pr in m.get('process', []):
        if pr['name'] not in run:
            dr.add('process', pr['name'], 'not running', fix='restart')
    # retired / unexpected
    named = manifest_paths(m)
    ret = {r['path']: r for r in m.get('retired', [])}
    for p in sorted(d.files):
        x = d.files[p]
        if p in ret:
            dr.add('retired', p, ret[p].get('reason', ''), fix='retire')
            continue
        if any(p.startswith(r + '/') for r in ret):
            continue
        if p in named or any(p.startswith(b + '/') for b, k in named.items() if k == 'bundle'):
            continue
        if x['type'] == 'd' or p in roots or gmatch(p, ignore) or gmatch(p, volatile) or gmatch(p, stock):
            continue
        dr.add('unexpected', p, f"{x['type']} {x['size']} B {datetime.datetime.fromtimestamp(x['mtime'], datetime.timezone.utc):%Y-%m-%d}")
    return dr


def short_delta(want, have):
    w, h = want.splitlines(), have.splitlines()
    plus = [l for l in h if l not in w][:3]
    minus = [l for l in w if l not in h][:3]
    return '; '.join([f'phone has "{l}"' for l in plus] + [f'manifest "{l}"' for l in minus])[:300]


def report(dr, d, m, as_json=False, out=sys.stdout):
    if as_json:
        json.dump(dr.items, out, indent=1)
        out.write('\n')
        return
    real = [i for i in dr.items if i['kind'] not in INFO_KINDS]
    hdr = (f"manifest {m.get('_spec')} | phone: kern.uuid {d.h.get('kern.uuid', '?')}, up "
           f"{fmt_dur(d.h.get('uptime'))}, {len(d.files)} paths, {len(d.sysctl)} sysctls, {len(d.procs)} processes")
    print(hdr, file=out)
    if not real:
        print(f"state diff: no drift ({len(m.get('file', []))} files, {len(m.get('bundle', []))} bundles, "
              f"{len(m.get('flag', []))} flags, {len(m.get('sysctl', {})) + len(m.get('sysctl_expect', {}))} sysctls, "
              f"{len(m.get('process', []))} processes)", file=out)
    for k in DRIFT_KINDS:
        its = dr.of(k)
        if not its:
            continue
        print(f'{k} ({len(its)}):', file=out)
        lim = 25 if k in ('retired', 'unexpected') else 200
        for i in its[:lim]:
            print(f"  {i['path']}  {i['detail']}".rstrip(), file=out)
        if len(its) > lim:
            print(f'  ... {len(its) - lim} more (--json for all)', file=out)
    if real:
        print(f'state diff: DRIFT {len(real)} item(s): ' +
              ', '.join(f'{k} {len(dr.of(k))}' for k in DRIFT_KINDS if dr.of(k) and k not in INFO_KINDS), file=out)


def fmt_dur(s):
    try:
        s = int(s)
    except (TypeError, ValueError):
        return '?'
    return f'{s // 3600}h{s % 3600 // 60:02d}m'


# ---------------------------------------------------------------- apply

def check_writable(m, path):
    mg = m.get('managed', {})
    if not under(path, mg.get('roots', [])):
        die(f'refused: {path} is outside [managed].roots')
    if gmatch(path, mg.get('firmware', []) + FIRMWARE_ALWAYS):
        die(f'refused: {path} is firmware ([managed].firmware): state.py never writes firmware')
    if '..' in path.split('/'):
        die(f'refused: {path}')


def restart_cmds(m, names):
    pr = {p['name']: p for p in m.get('process', [])}
    out = []
    for n in sorted(names):
        if n == 'reboot':
            continue
        c = pr.get(n, {}).get('restart') or f'/var/local/bin/psx -k {n}'
        out.append((n, c))
    return out


def cmd_apply(m, tp, a):
    d = Dump(take_dump(m, tp))
    dr = compute_diff(m, d)
    report(dr, d, m)
    files = {f['path']: f for f in m.get('file', [])}
    bundles = {b['path']: b for b in m.get('bundle', [])}
    flags = {f['path']: f for f in m.get('flag', [])}
    links = {l['path']: l for l in m.get('link', [])}
    plan, restart, manual = [], set(), []
    for i in dr.items:
        p, k = i['path'], i['kind']
        if k == 'boot':
            manual.append(f'boot: {i["detail"]}')
        elif k in ('missing', 'hash', 'mode', 'owner') and (p in files or p in bundles):
            e = files.get(p) or bundles[p]
            src = e.get('source', 'unknown')
            if e.get('firmware'):
                manual.append(f'{p}: firmware copy: never written by state.py')
                continue
            if src in ('unknown', 'stock', 'moved-aside', 'firmware-copy') or not src:
                manual.append(f'{p}: source "{src}": cannot install (resolve the source in the manifest)')
                continue
            plan.append(('chmod' if k == 'mode' else 'chown' if k == 'owner' else 'install', p, e))
            if k in ('missing', 'hash'):
                restart.update(as_list(e.get('restart')))
        elif k in ('missing', 'flag', 'secret') and p in flags:
            f = flags[p]
            want, data = flag_desired(f)
            if data is None and not f.get('source'):
                manual.append(f'{p}: secret flag (hash only) without a local source file: cannot write')
                continue
            plan.append(('flag', p, f))
            restart.update(as_list(f.get('restart')))
        elif k == 'flag' and i.get('fix') == 'write-sysctl-conf':
            plan.append(('sysctl-conf', p, None))
        elif k == 'link' and p in links:
            plan.append(('link', p, links[p]))
        elif k == 'sysctl' and i.get('fix') == 'sctl':
            sec = 'sysctl' if p in m.get('sysctl', {}) else 'sysctl_expect'
            plan.append(('sctl', p, m[sec][p]))
        elif k == 'process':
            restart.add(p)
        elif k == 'retired' and a.retire:
            plan.append(('retire', p, None))
        elif k == 'unexpected':
            manual.append(f'{p}: unexpected (not in the manifest): add it, retire it, or move it to /var/local/scratch')
    if 'reboot' in restart:
        manual.append('an installed entry needs a reboot to take effect (restart = "reboot"): not done by state.py')
    for p in [x[1] for x in plan if x[0] in ('install', 'flag', 'sysctl-conf', 'link', 'chmod', 'chown')]:
        check_writable(m, p)
    print('\nplan:')
    for op, p, e in plan:
        print(f'  {op:11} {p}' + (f"  <- {e.get('source')}" if isinstance(e, dict) and e.get('source') else
                                  f'  = {e}' if op == 'sctl' else ''))
    rc = restart_cmds(m, restart)
    for n, c in rc:
        print(f'  restart     {n}: {c}')
    for x in manual:
        print(f'  MANUAL      {x}')
    if a.dry_run or not plan and not rc:
        print('dry run: nothing changed' if a.dry_run else 'nothing to do')
        return 0 if not manual else 1
    # unknown-provenance content is pulled to the PC before it is overwritten
    attic = os.path.join(STATE, 'attic', now_utc().strftime('%Y%m%dT%H%M%SZ'))
    by = by_sha(build_index())
    failed = 0
    for op, p, e in plan:
        if op == 'install':
            old = d.sha.get(p)
            if old and old not in by and d.files.get(p, {}).get('type') == 'f':
                tp.pull(p, os.path.join(attic, p.lstrip('/')))
            if p in bundles:
                failed += install_bundle(tp, e)
            else:
                failed += install_file(tp, e)
        elif op == 'chmod':
            r, o = tp.sh(f"{T} chmod {e['mode']} {shlex.quote(p)}")
            failed += r != 0
        elif op == 'chown':
            r, o = tp.sh(f"{T} chown {e['owner']} {shlex.quote(p)}")
            failed += r != 0
        elif op == 'flag':
            want, data = flag_desired(e)
            if data is None:
                src = os.path.join(PF, e['source'])
                data = open(src, 'rb').read()
                if hashlib.sha256(data).hexdigest() != want:
                    print(f'  {p}: {e["source"]} does not match the manifest sha256: skipped')
                    failed += 1
                    continue
            if d.sha.get(p) and d.sha[p] not in by:
                tp.pull(p, os.path.join(attic, p.lstrip('/')))
            r, o = write_flag(tp, p, data.decode(errors='replace'), e.get('mode', '644'), e.get('owner', '0:0'))
            print(f'  flag {p}: {"ok" if r == 0 else "FAILED " + o[-200:]}')
            failed += r != 0
        elif op == 'sysctl-conf':
            r, o = tp.write(sysctl_conf_text(m).encode(), p)
            print(f'  {p}: {"ok" if r == 0 else "FAILED " + o[-200:]}')
            failed += r != 0
        elif op == 'link':
            r, o = tp.sh(f"{T} ln -sfn {shlex.quote(e['target'])} {shlex.quote(p)}")
            failed += r != 0
        elif op == 'sctl':
            if not re.fullmatch(r'kern\.[a-z0-9_.]+', p) or not re.fullmatch(r'-?(0x)?[0-9a-fA-F]+', str(e)):
                print(f'  sctl {p} {e}: bad name/value, skipped')
                continue
            r, o = tp.sh(f'/var/local/bin/sctl {p} {e}')
            print(f'  {o.strip()}')
            failed += r != 0
        elif op == 'retire':
            dst = f"/var/local/scratch/retired-{now_utc():%Y%m%d}/" + p.lstrip('/').replace('/', '__')
            r, o = tp.sh(f'{T} mkdir -p /var/local/scratch/retired-{now_utc():%Y%m%d} && {T} mv {shlex.quote(p)} {shlex.quote(dst)}')
            print(f'  retire {p} -> {dst}: {"ok" if r == 0 else "FAILED " + o[-200:]}')
            failed += r != 0
    for n, c in rc:
        r, o = tp.sh(c)
        print(f'  restart {n}: {o.strip()[-200:] or "ok"}')
    if os.path.isdir(attic):
        print(f'  unknown-provenance originals saved under {os.path.relpath(attic, PF)}')
    time.sleep(5 if rc else 0)
    d2 = Dump(take_dump(m, tp))
    dr2 = compute_diff(m, d2)
    print('\nafter apply:')
    report(dr2, d2, m)
    real = [i for i in dr2.items if i['kind'] not in INFO_KINDS]
    return 1 if failed or real else 0


def as_list(x):
    return [] if not x else [x] if isinstance(x, str) else list(x)


def install_file(tp, e):
    src = os.path.join(PF, e['source'])
    if not os.path.isfile(src):
        print(f"  {e['path']}: source {e['source']} not found")
        return 1
    if e.get('sha256') and sha256_file(src) != e['sha256']:
        print(f"  {e['path']}: {e['source']} sha256 differs from the manifest: not installed")
        return 1
    r, o = tp.put(src, e['path'], e.get('mode', '755'), e.get('owner', '0:0'))
    ok = r == 0 and (not e.get('sha256') or e['sha256'] in o)
    print(f"  install {e['path']}: {'ok' if ok else 'FAILED ' + o[-200:]}")
    return 0 if ok else 1


def install_bundle(tp, b):
    src = os.path.join(PF, b['source'])
    bad = 0
    for rel, h in b.get('files', {}).items():
        s = os.path.join(src, rel)
        if not os.path.isfile(s) or sha256_file(s) != h:
            print(f"  {b['path']}/{rel}: not in {b['source']} with the manifest sha256: bundle not installed")
            return 1
    for rel, h in b.get('files', {}).items():
        mode = b.get('modes', {}).get(rel, '755' if os.access(os.path.join(src, rel), os.X_OK) else '644')
        r, o = tp.put(os.path.join(src, rel), f"{b['path']}/{rel}", mode, b.get('owner', '0:0'))
        if r or h not in o:
            print(f"  {b['path']}/{rel}: FAILED {o[-200:]}")
            bad += 1
    print(f"  bundle {b['path']}: {'ok' if not bad else f'{bad} file(s) FAILED'}")
    return 1 if bad else 0


# ---------------------------------------------------------------- adopt (bootstrap a manifest from a dump)

RESTART = [  # (glob on the phone path, process names restarted when it changes)
    ('/var/local/bin/wland', 'wland'), ('/var/local/cell.on', 'wland'), ('/var/local/wland.conf', 'wland'),
    ('/var/local/abp-policy.conf', 'wland'),
    ('/var/local/bin/sensd', 'sensd'), ('/var/local/bin/fastrpcd', 'fastrpcd'), ('/var/local/bin/btd-bridge', 'btd-bridge'),
    ('/var/local/bin/sweetmon', 'sweetmon'),
    ('/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver', 'mediaserverd'), ('/var/local/redwood-call-route', 'mediaserverd'),
    ('/System/Library/Extensions/SweetMetal2.bundle', 'backboardd'), ('/var/local/hmetal/lib/*', 'backboardd'),
    ('/var/local/hmetal/ca/env', 'backboardd'), ('/var/local/hmetal/hm-only', 'backboardd'),
    ('/var/local/hmetal/m2-procs', 'backboardd'), ('/var/local/hmetal/m2-fallback', 'backboardd'),
    ('/var/local/mtl2-env', 'backboardd'), ('/var/local/mtl2-clients', 'backboardd'),
    ('/var/local/lib/SweetFPS.dylib', 'SpringBoard'), ('/var/local/sweethud', 'SpringBoard'),
    ('/var/local/sweetfps', 'SpringBoard'), ('/var/local/sweetcell.conf', 'SpringBoard'),
    ('/System/Library/xpc/launchd.plist', 'reboot'), ('/var/local/bin/sensd-probe.sh', 'reboot'),
    ('/var/local/bin/sweet-sysctl.sh', 'reboot'),
]
PROCESSES = {  # expected up; restart command (launchd restarts KeepAlive jobs after a kill)
    'launchd': None, 'SpringBoard': None, 'backboardd': None, 'mediaserverd': None, 'CommCenter': None,
    'wland': None, 'sensd': None, 'fastrpcd': None, 'btd-bridge': None, 'bluetoothd': None, 'wifid': None,
    'dropbearmulti': '-', 'sweetmon': '/var/local/bin/psx -k sweetmon; /var/local/bin/sweetmon -D </dev/null >/dev/null 2>&1',
    'configd': None, 'locationd': None,
}
RETIRE_RULES = [  # (regex on the path, reason); first match wins; a matching directory retires its whole subtree
    (r'/knowngood-r5au1s$', 'r5au1s fallback copy (2026-11-09); known-good tracking retired 2026-11-05: git manifest + builds/ replace it'),
    (r'/SweetMetal2\.bundle\.[^/]+$', 'old SweetMetal2 bundle copy (hand-kept backup); any version is rebuilt from builds/metal2'),
    (r'^/var/local/hmetal/ca/HakiosMetal\.bundle$', 'hmt11 copy (ws-hm-var11): the plug-in moved to hmetal/lib in hmapp1 (KNOWN-GOOD 2026-11-09 20:01); check `sweetstate -M` shows no process mapping it before --retire'),
    (r'/HakiosMetal\.bundle\.[^/]+$', 'old HakiosMetal variant (hmt/var runs 2026-11-06..09); active plug-in is hmetal/lib/HakiosMetal.bundle'),
    (r'/hmetal/ca/shim(\.[^/]+)?$', 'old Metal dispatcher shim copy (hmt runs); active = /System/Library/Extensions/SweetMetal2.bundle'),
    (r'/RedwoodAudio\.driver\.[^/]+$', 'old RedwoodAudio plug-in copy; any version is in builds/userland'),
    (r'/RedwoodAudio\.pre-[^/]+$', 'old RedwoodAudio plug-in copy; any version is in builds/userland'),
    (r'^/var/local/backup$', 'hand backups (MobileGestalt / Info.plist tars, 2026-11-04/05)'),
    (r'/lib/[^/]+\.dylib\.[^/]+$', 'old dylib copy kept by hand'),
    (r'\.(pre|before)[-_][^/]*$|\.before-[^/]*$|\.u-w-bak$', 'pre-change backup: the git manifest history is the backup now'),
    (r'\.knowngood[^/]*$', 'known-good copy: known-good tracking retired; the git manifest history replaces it'),
    (r'\.(bak|orig)$|-orig$', 'hand backup'),
    (r'/launchd\.plist\.[^/]+$', 'old launchd.plist copy (the live one is managed)'),
    (r'/bin/(wland|sensd|btd-bridge|venusctl|sctl|sensd2)\.[^/]+$|/bin/hakios-metal-probe-[^/]+$', 'old build copy of a daemon/tool kept by hand'),
    (r'/bin/hakios-metal-probe$', 'one-off Metal probe (2026-11-06)'),
    (r'/(wland|btd)\.log\.(?!prev$)[^/]+$', 'saved log copy from an old test run (copy to PF/logs if still wanted)'),
    (r'/abp-policy\.conf\.[^/]+$|/abp-policy\.r3\.conf$', 'old abp-policy variant (secret-class file: never copied off the phone)'),
    (r'/cell\.on\.[^/]+$', 'old cell.on variant'),
    (r'/mtl2-(env|live|clients)\.[^/]+$', 'old Metal env variant'),
    (r'/hmetal/ca/env\.[^/]+$|/hmetal/ca/bbd-hm\.starts\.[^/]+$', 'old hmetal env/start log variant'),
    (r'/sweet-sysctl\.conf\.[^/]+$', 'old sysctl conf variant'),
    (r'/var/local/[^/]+\.(text|nv12|tar|tgz|ips|data|str)$', 'one-off dump/test output (copy to PF/logs if still wanted)'),
    (r'/var/local/(bb8|cmcc|cst2?|fcss|fms|s1|s2|stubs|bks|l6|loc[0-9]|o[0-9]+m?|or10|oslog-s4|wfr2-[^/]+|cellnet|celestial)(\.[a-z]+)?$', 'one-off dump/test output (copy to PF/logs if still wanted)'),
    (r'/var/local/hmetal/[^/]+\.(log|out|bin|metallib)$|/var/local/hmetal/perf1(\.sh|\.log)?$|/var/local/hmetal-probe[^/]*\.log$|/var/local/hmetal/HakiosMetal\.bundle$', 'one-off hmetal probe/bench output (2026-11-06..09)'),
    (r'/hmetal/ca/(h[0-9][a-z]*(tail|user)?\.log|bbd-hm-[^/]+\.log|tstart[^/]*|h3b-stage-mtimes\.txt|shim[^/]*\.log)$', 'one-off hmetal test log'),
    (r'/var/local/(c30-[a-z]+\.sh|staged-[^/]+|camera-dump-still|sensd-probe\.out|mtl2-telemetry|av\.log|sweetsb\.[a-z]+)$', 'one-off script/marker from an old test'),
    (r'/var/local/bin/(n12b-rep|h1test|rgtest)\.sh$|/var/local/bin/(stubt|vktest-[a-z]+|pimg|mic_test-tcc|camera_avtest|camera_test|mg_test|sound_test|vt_test|hmbench)$', 'one-off test tool (rebuild from builds/userland when needed)'),
]
BIG_LOG = 64 << 20


def retire_reason(p, x):
    for rx, why in RETIRE_RULES:
        if re.search(rx, p):
            return why
    if x and x['type'] == 'f' and x['size'] >= BIG_LOG and re.search(r'\.log$', p):
        return f"{x['size'] >> 20} MB log: truncate/rotate (the writer recreates it)"
    return None


def restart_of(p):
    out = []
    for g, n in RESTART:
        if fnmatch.fnmatchcase(p, g) or p.startswith(g.rstrip('*') + '/') and not g.endswith('*'):
            out.append(n)
    return sorted(set(out))


DEFAULT_MANAGED = {
    'roots': ['/var/local', '/System/Library/Extensions/SweetMetal2.bundle',
              '/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver',
              '/System/Library/xpc', '/Applications/SweetControl.app',
              '/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches'],
    'ignore': ['/var/local/scratch/*', '/var/local/tmp/*', '/var/local/m2v-cache/*', '/var/local/hmetal/cache/*',
               '/var/local/hmetal/log/*', '/var/local/hmetal/dump/*', '/var/local/fw/rmtfs/*'],
    'volatile': ['*.log', '/var/local/wland.log.prev', '/var/local/cell.status', '/var/local/wland.boot',
                 '/var/local/wland.ipa', '/var/local/hmetal/ca/bbd-hm.starts', '/var/local/hmetal/ca/bbd-hm',
                 '/var/local/sweetctl/changes.log', '/var/local/modem-profiles-*.bak', '/var/local/ipa-ulflt.bin',
                 '/var/local/sensd-probe.log', '/var/local/sweetmon.key', '/var/local/sweethud',
                 '*.log.[0-9]', '/var/local/hmetal/m2-fallback',
                 '/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/*'],
    'stock': ['/System/Library/xpc/extensions.plist', '/System/Library/xpc/extensions.plist.sig',
              '/System/Library/xpc/launchd.plist.sig'],
    'firmware': ['/var/local/fw', '/var/local/fw/*', '/var/local/hexagon', '/var/local/hexagon/*', '/dev/*'],
    'secret': ['/var/local/abp-policy.conf*', '/var/local/modem-profiles-*', '*.key', '*MobileGestalt*',
               '/var/local/fw/rmtfs/*'],
    'nohash': ['./SweetMetal2.bundle.*', './RedwoodAudio.driver.*', './knowngood-r5au1s', './knowngood-r5au1s/*',
               './hmetal/ca/HakiosMetal.bundle.*', './hmetal/ca/shim.*', './hexagon.t*', './launchd.plist.*',
               './wland.log.*', './btd.log.*', './*.text', './*.nv12', './*.pre-*'],
    'scratch': '/var/local/scratch',
}


def cmd_adopt(d, a, base=None):
    idx = build_index()
    by = by_sha(idx)
    mg = json.loads(json.dumps(DEFAULT_MANAGED))
    m = {'meta': {'schema': 1, 'device': 'redfin', 'adopted': now_utc().strftime('%Y-%m-%dT%H:%M:%SZ'),
                  'from_dump': (os.path.relpath(a.dump, PF) if a.dump and os.path.abspath(a.dump).startswith(PF + '/') else a.dump or ''), 'note': 'bootstrapped by state.py adopt; source = "unknown" entries need a decision (resolve or retire)'},
         'boot': adopt_boot(d, idx), 'managed': mg}
    # sysctls: the boot conf on the phone, the rest of the readable kern.sweet_* as runtime expectations
    c = d.content.get('/var/local/sweet-sysctl.conf')
    conf = parse_sysctl_conf(c[1].decode(errors='replace')) if c else {}
    m['sysctl'] = conf
    exp = {}
    for k, v in sorted(d.sysctl.items()):
        if k in conf or not re.fullmatch(r'-?\d+', v.strip()) or re.search(
                r'stat|count|waits|errs|errors|drops|_n$|last|bytes|pkts|_seen|_total|_age|_ms_max|_hits|_miss|irq|state|_up$|_rx$|_tx$|bat|temp|therm|rssi|sig$|_cur|frames|xfers|resets|rearms|stuck|snum|serial|imei|iccid|imsi|vsid|bootstage|_msg$|bbmsg|chipid|_img|imglen|_mhz$', k):
            continue
        exp[k] = int(v.strip())
    m['sysctl_expect'] = exp
    run = d.running()
    m['process'] = [dict(name=n, **({'restart': r} if r else {})) for n, r in PROCESSES.items() if n in run and r != '-'] + \
                   [dict(name='dropbearmulti', restart='-', note='ssh: never restarted by state.py')] * ('dropbearmulti' in run)
    files, bundles, links, flags, retired = [], [], [], [], []
    seen_bundle = set()
    retired_dirs = []
    for fwroot in ('/var/local/fw', '/var/local/hexagon'):
        if fwroot in d.files:
            fb = adopt_bundle(d, fwroot, {}, idx, skip=mg['ignore'])
            fb['source'] = 'firmware-copy'
            fb['firmware'] = True
            fb['note'] = "copied from the phone's own vendor/firmware partitions: never in git, never written by state.py"
            bundles.append(fb)
            seen_bundle.add(fwroot)
    moved = {'/var/local/disabled/SyncBundles/AirFair.syncBundle': 'AirFair moved aside (FairPlay off; KNOWN-GOOD rk3a notes)',
             '/System/Library/xpc/com.apple.voicemail.vmd.job.plist.disabled': 'vmd disabled 2026-10-29 (user decision; revisit with telephony)'}
    for p in sorted(d.files):
        x = d.files[p]
        if gmatch(p, mg['ignore']) or any(p.startswith(r + '/') for r in retired_dirs):
            continue
        if p in mg['roots'] and not re.search(r'\.(bundle|driver|app)$', p):
            continue
        if any(p.startswith(b + '/') for b in seen_bundle):
            continue
        why = None if gmatch(p, mg['stock']) else retire_reason(p, x)
        if why and gmatch(p, mg['volatile']) and 'MB log' not in why:
            why = None  # written by something running (logs, modem-profiles-*.bak): not a leftover
        if why:
            retired.append(dict(path=p, reason=why))
            if x['type'] == 'd':
                retired_dirs.append(p)
            continue
        if x['type'] == 'd':
            if re.search(r'\.(bundle|driver|app|syncBundle)$', p):
                bb = adopt_bundle(d, p, by, idx)
                if p in moved:
                    bb['source'], bb['note'] = 'moved-aside', moved[p]
                bundles.append(bb)
                seen_bundle.add(p)
            continue
        if x['type'] == 'l':
            links.append(dict(path=p, target=d.links.get(p, '?')))
            continue
        if gmatch(p, mg['volatile']) or gmatch(p, mg['stock']):
            continue
        if p in moved:
            fe = adopt_file(d, p, x, by, idx)
            fe['source'], fe['note'] = 'moved-aside', moved[p]
            files.append(fe)
            continue
        cont = d.content.get(p)
        if gmatch(p, mg['secret']) or cont is not None:
            flags.append(adopt_flag(d, p, x, cont, gmatch(p, mg['secret']), by))
            continue
        files.append(adopt_file(d, p, x, by, idx))
    resolve_by_name(d, files + bundles, idx)
    m['file'], m['bundle'], m['link'], m['flag'], m['retired'] = files, bundles, links, flags, retired
    # the conf is generated from [sysctl]; it is not a flag of its own
    m['flag'] = [f for f in m['flag'] if f['path'] != '/var/local/sweet-sysctl.conf']
    if a.known_good:
        m['meta']['known_good_baseline'] = known_good_baseline(a.known_good)
    return m


def resolve_by_name(d, entries, idx):
    """source = "unknown" entries: look for a file with the same name and size anywhere in the project folder
    (CI artefacts, r*/ image folders, metal2/, push/, ...) and take it when its sha256 matches"""
    want = {}
    for e in entries:
        if e.get('source') != 'unknown':
            continue
        if 'files' in e:
            for rel, h in e['files'].items():
                q = e['path'] + '/' + rel
                if h and q in d.files:
                    want.setdefault(os.path.basename(rel), []).append((d.files[q]['size'], h, e, rel))
        elif e.get('sha256') and e['path'] in d.files:
            want.setdefault(os.path.basename(e['path']), []).append((d.files[e['path']]['size'], e['sha256'], e, None))
    if not want:
        return
    expr = []
    for n in want:
        expr += (['-o'] if expr else []) + ['-name', n]
    cmd = ['find', PF, '-maxdepth', '8', '(', '-path', f'{PF}/logs', '-o', '-path', f'{PF}/builds/logs', '-o', '-path',
           f'{PF}/state', '-o', '-path', f'{PF}/agent/inbox', '-o', '-path', f'{PF}/agent/outbox', '-o', '-name', '.git', ')',
           '-prune', '-o', '-type', 'f', '(', *expr, ')', '-print']
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=600).stdout.decode(errors='replace').splitlines()
    except subprocess.TimeoutExpired:
        return
    found = {}
    for path in out:
        n = os.path.basename(path)
        try:
            sz = os.path.getsize(path)
        except OSError:
            continue
        if any(sz == c[0] for c in want.get(n, [])):
            found.setdefault((n, sz), []).append(path)
    hits = {}
    for (n, sz), paths in found.items():
        for path in sorted(paths, key=lambda x: -os.path.getmtime(x)):
            h = sha256_file(path)
            for c in want[n]:
                if c[0] == sz and c[1] == h:
                    hits.setdefault(id(c[2]), {})[c[3]] = os.path.relpath(path, PF)
    for e in entries:
        hh = hits.get(id(e))
        if not hh:
            continue
        if 'files' in e:
            # the bundle's source = the folder that holds the most of its files
            dirs = {}
            for rel, path in hh.items():
                if rel and path.endswith(rel):
                    dirs[path[:-len(rel) - 1]] = dirs.get(path[:-len(rel) - 1], 0) + 1
            if dirs:
                best = max(dirs, key=dirs.get)
                e['source'] = best
                if dirs[best] < len(e['files']):
                    e['note'] = f"{dirs[best]} of {len(e['files'])} files found in {best}"
        else:
            e['source'] = hh[None]
        si = src_info(idx, e['source'])
        if si:
            e['target'], e['ref'], e['sha'] = si


def adopt_boot(d, idx):
    b = {'image': '', 'base': '', 'kernel_dir': '', 'kernel_branch': '', 'kernel_sha': '', 'kernel_uuid': d.h.get('kern.uuid', ''),
         'cmdline_file': '', 'bootargs': d.h.get('kern.bootargs', '')}
    for key, u in idx.get('kernels', {}).items():
        if u == b['kernel_uuid']:
            info = idx['sums'].get(key, {})
            b['kernel_dir'], b['kernel_branch'], b['kernel_sha'] = key, info.get('ref', ''), info.get('source', '')
    lb = os.path.join(PF, 'agent/last-boot.txt')
    if os.path.exists(lb):
        img = open(lb).read().strip()
        b['image'] = img
        rd = os.path.join(PF, img, 'README.txt')
        if os.path.exists(rd):
            t = open(rd, errors='replace').read()
            mm = re.search(r'^base\s+(\S+)', t, re.M)
            if mm:
                b['base'] = mm.group(1)
            mm = re.search(r'cmdline from (\S+?),', t)
            if mm:
                b['cmdline_file'] = mm.group(1)
            mm = re.search(r'^kernel\s+(builds/xnu/[^/\s]+)', t, re.M)
            if mm and not b['kernel_dir']:
                b['kernel_dir'] = mm.group(1)
                info = idx['sums'].get(mm.group(1), {})
                b['kernel_branch'], b['kernel_sha'] = info.get('ref', ''), info.get('source', '')
                b['note'] = 'kernel from the image README (kern.uuid not matched to a build: check after the next boot)'
    return b


def adopt_file(d, p, x, by, idx, firmware=False):
    h = d.sha.get(p, '')
    e = {'path': p, 'source': by.get(h, 'firmware-copy' if firmware else 'unknown'), 'sha256': h, 'mode': x['mode'],
         'owner': f"{x['uid']}:{x['gid']}"}
    si = src_info(idx, e['source'])
    if si:
        e['target'], e['ref'], e['sha'] = si
    r = restart_of(p)
    if r:
        e['restart'] = r[0] if len(r) == 1 else r
    if not h:
        e['note'] = 'not hashed in the bootstrap dump (nohash pattern)'
    return e


def adopt_bundle(d, p, by, idx, skip=()):
    files = {q[len(p) + 1:]: d.sha.get(q, '') for q, x in sorted(d.files.items())
             if q.startswith(p + '/') and x['type'] == 'f' and not gmatch(q, skip)}
    srcs = {}
    for rel, h in files.items():
        s = by.get(h)
        if s and s.endswith('/' + rel):
            srcs.setdefault(s[:-len(rel) - 1], 0)
            srcs[s[:-len(rel) - 1]] += 1
    src = max(srcs, key=srcs.get) if srcs else 'unknown'
    e = {'path': p, 'source': src}
    si = src_info(idx, src)
    if si:
        e['target'], e['ref'], e['sha'] = si
    if srcs and srcs[src] < len(files):
        e['note'] = f'{srcs[src]} of {len(files)} files match {src}'
    r = restart_of(p)
    if r:
        e['restart'] = r[0] if len(r) == 1 else r
    pl = d.content.get(p + '/Info.plist')
    if pl:
        try:
            info = plistlib.loads(pl[1])
            for k in ('CFBundleIdentifier', 'CFBundleShortVersionString', 'CFBundleVersion'):
                if k in info:
                    e[k] = str(info[k])
        except Exception:
            pass
    e['files'] = files
    return e


def adopt_flag(d, p, x, cont, secret, by):
    h = d.sha.get(p, '')
    e = {'path': p}
    if secret or cont is None:
        e['secret'] = True
        e['sha256'] = h
        if h in by:
            e['source'] = by[h]
    elif cont[0]:
        e['redacted'] = cont[0]
        e['sha256'] = h
        e['content'] = cont[1].decode(errors='replace')
    else:
        e['content'] = cont[1].decode(errors='replace')
        if h and hashlib.sha256(cont[1]).hexdigest() != h:
            e['note'] = 'dump content and hash disagree (file changed during the dump?)'
    e['mode'] = x['mode']
    r = restart_of(p)
    if r:
        e['restart'] = r[0] if len(r) == 1 else r
    return e


def known_good_baseline(path):
    t = open(path, errors='replace').read()
    i = t.rfind('=== BASELINE')
    if i < 0:
        return ''
    blk = t[i:].split('\n\n', 1)[0].splitlines()
    out = [blk[0]] + [l for l in blk[1:] if not re.match(r'^\d{4}-\d\d-\d\dT|^TEST|^===', l)]
    return '\n'.join(out).strip()[:1500]


# ---------------------------------------------------------------- temp overrides

def cmd_set(m, tp, key, value, by):
    temp = load_temp()
    if key.startswith('kern.'):
        if not re.fullmatch(r'kern\.[a-z0-9_.]+', key) or not re.fullmatch(r'-?(0x)?[0-9a-fA-F]+', value):
            die('KEY kern.NAME, VALUE an integer')
        orig = m.get('sysctl', {}).get(key, m.get('sysctl_expect', {}).get(key))
        r, o = tp.sh(f'/var/local/bin/sctl {key} {value}')
        print(o.strip())
        if r:
            return r
    else:
        path, _, var = key.partition(':')
        f = next((x for x in m.get('flag', []) if x['path'] == path), None)
        if not f or 'content' not in f or not var:
            die('KEY: kern.NAME or /var/local/FLAGFILE:VAR (a KEY=VALUE line of a flag file in the manifest)')
        check_writable(m, path)
        orig = None
        for tok in f['content'].split():
            if '=' in tok and tok.split('=', 1)[0] == var:
                orig = tok.split('=', 1)[1]
        if re.search(r'(?i)(^|[^a-z])(sim_?)?(pin|puk)|iccid|imsi|imei|msisdn', var):
            die(f'{var}: secret-class key: not set through state.py')
        cur = load_flag_with_temps(f, {**temp, key: {'value': value}})
        r, o = write_flag(tp, path, cur, f.get('mode', '644'))
        if r:
            print(o)
            return r
        print(f'{path}: {var}={value} (restart {", ".join(as_list(f.get("restart"))) or "nothing"} to take effect)')
    temp[key] = {'value': value, 'orig': orig, 'by': by or os.environ.get('STATE_BY', ''), 'at': now_utc().strftime('%Y-%m-%dT%H:%MZ')}
    save_temp(temp)
    return 0


def load_flag_with_temps(f, temp):
    """flag content with temp overrides: VAR=VALUE as a whole line (env files) or as a token (cell.on: k=v k=v ...)"""
    lines = f['content'].splitlines()
    for k, t in temp.items():
        path, _, var = k.partition(':')
        if path != f['path'] or not var:
            continue
        hit = False
        for i, l in enumerate(lines):
            toks = l.split(' ')
            for j, tok in enumerate(toks):
                if '=' in tok and tok.split('=', 1)[0] == var:
                    toks[j], hit = f"{var}={t['value']}", True
            lines[i] = ' '.join(toks)
        if not hit:
            lines.append(f"{var}={t['value']}")
    return '\n'.join(lines) + ('\n' if f['content'].endswith('\n') or not f['content'] else '')


def save_temp(temp):
    os.makedirs(STATE, exist_ok=True)
    json.dump(temp, open(TEMP + '.tmp', 'w'), indent=1)
    os.replace(TEMP + '.tmp', TEMP)


def cmd_revert(m, tp, keys):
    temp = load_temp()
    keys = list(temp) if keys == ['all'] else keys
    for k in keys:
        t = temp.pop(k, None)
        if t is None:
            print(f'{k}: no temp override')
            continue
        if k.startswith('kern.'):
            v = m.get('sysctl', {}).get(k, m.get('sysctl_expect', {}).get(k, t.get('orig')))
            if v is None:
                print(f'{k}: no manifest value and no recorded original: left at {t["value"]}')
            else:
                r, o = tp.sh(f'/var/local/bin/sctl {k} {v}')
                print(o.strip())
        else:
            path = k.split(':', 1)[0]
            f = next((x for x in m.get('flag', []) if x['path'] == path), None)
            if f:
                write_flag(tp, path, load_flag_with_temps(f, temp), f.get('mode', '644'))
                print(f'{path}: {k.split(":", 1)[1]} back to the manifest value (restart {", ".join(as_list(f.get("restart"))) or "nothing"})')
    save_temp(temp)
    return 0


def cmd_promote(spec, keys):
    if not spec or spec.startswith('git:'):
        die('promote edits a manifest FILE: --manifest ~/wt/BRANCH/state/redfin.toml (then commit + push it)')
    m = load_manifest(spec)
    temp = load_temp()
    for k in keys:
        t = temp.pop(k, None)
        if t is None:
            print(f'{k}: no temp override')
            continue
        if k.startswith('kern.'):
            sec = 'sysctl_expect' if k in m.get('sysctl_expect', {}) else 'sysctl'
            m.setdefault(sec, {})[k] = int(t['value'], 0)
        else:
            path = k.split(':', 1)[0]
            f = next(x for x in m['flag'] if x['path'] == path)
            f['content'] = load_flag_with_temps(f, {k: t})
        print(f'{k} = {t["value"]} -> {spec}')
    m.pop('_spec', None)
    open(spec, 'w').write(dump_toml(m))
    save_temp(temp)
    return 0


# ---------------------------------------------------------------- summary / snapshot

def summary(m):
    b = m.get('boot', {})
    out = [f"boot   image {b.get('image')} (base {b.get('base')}), kernel {b.get('kernel_branch')} {b.get('kernel_sha', '')[:10]} "
           f"({b.get('kernel_dir')}), cmdline {b.get('cmdline_file')}, kern.uuid {b.get('kernel_uuid')}"]
    for e in m.get('bundle', []) + [f for f in m.get('file', []) if f.get('source') not in ('unknown', 'firmware-copy')]:
        src = e.get('source', '')
        tag = f"{e.get('ref', '')} {e.get('sha', '')}".strip() or src
        h = e.get('sha256', '')[:12]
        out.append(f"{e['path']:58} {tag}{'  ' + h if h else ''}")
    if m.get('sysctl'):
        out.append('sysctl (boot) ' + ', '.join(f'{k.replace("kern.sweet_", "")} {v}' for k, v in m['sysctl'].items()))
    unk = [f['path'] for f in m.get('file', []) + m.get('bundle', []) if f.get('source') == 'unknown']
    if unk:
        out.append(f'unknown provenance ({len(unk)}): ' + ', '.join(unk))
    return '\n'.join(out)


def cmd_snapshot(m, tp, label):
    ts = now_utc().strftime('%Y-%m-%dT%H%MZ')
    d_dir = os.path.join(STATE, 'snapshots')
    os.makedirs(d_dir, exist_ok=True)
    base = os.path.join(d_dir, ts + (f'-{label}' if label else ''))
    text = take_dump(m, tp)
    open(base + '.dump', 'w').write(text)
    d = Dump(text)
    dr = compute_diff(m, d)
    import io
    buf = io.StringIO()
    report(dr, d, m, out=buf)
    open(base + '.diff.txt', 'w').write(buf.getvalue())
    open(base + '.summary.txt', 'w').write(summary(m) + '\n')
    rel = os.path.relpath(base, PF)
    last = buf.getvalue().strip().splitlines()[-1]
    print(f'snapshot {rel}.dump (+ .diff.txt, .summary.txt)')
    print('--- for the status doc ---')
    print(f'State: `state.py diff` at {ts} against {m.get("_spec")}: {last}  (snapshot {rel}.dump)')
    print(buf.getvalue())
    return 0


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser(prog='state.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd')
    ap.add_argument('args', nargs='*')
    ap.add_argument('--manifest')
    ap.add_argument('--via', choices=['agent', 'direct'])
    ap.add_argument('--dump', help='use this dump file instead of asking the phone')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--retire', action='store_true')
    ap.add_argument('--temp', action='store_true')
    ap.add_argument('--out')
    ap.add_argument('--known-good')
    ap.add_argument('--by')
    a = ap.parse_args(argv)
    c = a.cmd
    if c == 'index':
        idx = build_index(refresh='--refresh' in a.args)
        print(f"{len(idx['sums'])} build dirs, {len(idx['kernels'])} kernel UUIDs, {len(idx['stage'])} agent/stage files -> {os.path.relpath(INDEX, PF)}")
        return 0
    if c == 'adopt':
        d = read_dump(a.dump)
        if d is None:
            try:
                mm = load_manifest(a.manifest)
            except SystemExit:
                mm = None
            text = take_dump(mm, transport(a.via))
            os.makedirs(os.path.join(STATE, 'snapshots'), exist_ok=True)
            a.dump = os.path.join(STATE, 'snapshots', now_utc().strftime('%Y-%m-%dT%H%MZ') + '-adopt.dump')
            open(a.dump, 'w').write(text)
            d = Dump(text)
        m = cmd_adopt(d, a)
        text = dump_toml(m)
        if a.out:
            open(a.out, 'w').write(text)
            unk = [e['path'] for e in m['file'] + m['bundle'] if e.get('source') == 'unknown']
            print(f"{a.out}: {len(m['file'])} files, {len(m['bundle'])} bundles, {len(m['link'])} links, {len(m['flag'])} flags, "
                  f"{len(m['sysctl'])} boot sysctls, {len(m['sysctl_expect'])} expected sysctls, {len(m['process'])} processes, "
                  f"{len(m['retired'])} retired; unknown provenance {len(unk)}")
        else:
            sys.stdout.write(text)
        return 0
    if c == 'promote':
        return cmd_promote(a.manifest, a.args)
    m = load_manifest(a.manifest)
    if c == 'summary':
        print(summary(m))
        return 0
    tp = transport(a.via)
    if c == 'dump':
        text = take_dump(m, tp)
        if a.args:
            open(a.args[0], 'w').write(text)
            d = Dump(text)
            print(f'{a.args[0]}: {len(d.files)} paths, {len(d.sha)} hashed, {len(d.content)} contents, {len(d.sysctl)} sysctls, '
                  f'{len(d.procs)} processes, errors {len(d.errors)}')
        else:
            sys.stdout.write(text)
        return 0
    if c == 'diff':
        d = read_dump(a.dump) or Dump(take_dump(m, tp))
        dr = compute_diff(m, d)
        report(dr, d, m, as_json=a.json)
        return 1 if [i for i in dr.items if i['kind'] not in INFO_KINDS] else 0
    if c == 'apply':
        return cmd_apply(m, tp, a)
    if c == 'snapshot':
        return cmd_snapshot(m, tp, a.args[0] if a.args else '')
    if c == 'set':
        if len(a.args) != 2 or not a.temp:
            die('usage: set KEY VALUE --temp   (permanent changes go into the manifest in git)')
        return cmd_set(m, tp, a.args[0], a.args[1], a.by)
    if c == 'revert':
        return cmd_revert(m, tp, a.args or die('revert KEY... | all'))
    die(f'unknown command {c}')


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
