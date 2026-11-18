#!/usr/bin/env python3
"""decache.py - turn a DyldExtractor-extracted dyld-shared-cache dylib back into a standalone,
loadable MH_DYLIB (arm64, LC_DYLD_INFO_ONLY rebase/bind opcodes), and verify the result.

Written for libBKDM1 (iPhone 8 20H392 cache -> /usr/lib/libBKDM1.dylib on the redfin X 20H392 cache),
see tools/decache/README.md. Run with `python3 -I` from a venv that has capstone (4.x or 5.x).

  decache.py relink  --dx IN.dx.dylib --cache DIR --syms SYMS.json -o OUT.dylib [--report R.json]
  decache.py verify  OUT.dylib --xsyms X.json --xcache X_DSC [--cache DIR] [--report R.json]

--syms / --xsyms: `ipsw dyld symaddr <cache> --in patterns.json -o syms.json` for the image's undefined
symbols (patterns `^name$`), run on the source (--syms) and target (--xsyms) cache.
"""
import argparse, collections, glob, json, mmap, os, re, struct, subprocess, sys, uuid

PAGE = 0x4000
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_UUID = 0x19, 0x2, 0xb, 0x1b
LC_DYLD_INFO_ONLY, LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS = 0x80000022, 0x80000033, 0x80000034
LC_FUNCTION_STARTS, LC_DATA_IN_CODE, LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO = 0x26, 0x29, 0x1d, 0x1e
LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB = 0xc, 0x80000018, 0x8000001f, 0x80000023
MH_DYLIB_IN_CACHE = 0x80000000
S_ZEROFILL, S_NON_LAZY_SYMBOL_POINTERS = 0x1, 0x6
INDIRECT_SYMBOL_ABS = 0x40000000
N_TYPE, N_SECT, N_WEAK_REF = 0x0e, 0x0e, 0x0040


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        done = (v == 0 and not b & 0x40) or (v == -1 and b & 0x40)
        out.append(b | (0 if done else 0x80))
        if done:
            return bytes(out)


def read_uleb(d, p):
    r = s = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            return r, p


def read_sleb(d, p):
    r = s = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, p


# ----------------------------------------------------------------------------------------------- Mach-O
class MachO:
    def __init__(self, path):
        self.path = path
        self.d = d = bytearray(open(path, 'rb').read())
        if struct.unpack_from('<I', d, 0)[0] != 0xfeedfacf:
            raise SystemExit('%s: not a 64-bit Mach-O' % path)
        self.cputype, self.cpusub, self.ftype, self.ncmds, self.sizeofcmds, self.flags = struct.unpack_from('<IIIIII', d, 4)
        self.segs, self.sects, self.cmds, self.dylibs = [], [], [], []
        self.symoff = self.nsyms = self.stroff = self.strsize = 0
        self.dys = None
        off = 32
        for _ in range(self.ncmds):
            cmd, sz = struct.unpack_from('<II', d, off)
            self.cmds.append((cmd, off, sz))
            if cmd == LC_SEGMENT_64:
                name = d[off+8:off+24].rstrip(b'\0').decode()
                vm, vms, fo, fs, mp, ip, ns, fl = struct.unpack_from('<QQQQiiII', d, off+24)
                seg = dict(name=name, vm=vm, vms=vms, fo=fo, fs=fs, mp=mp, ip=ip, fl=fl, sects=[])
                self.segs.append(seg)
                so = off + 72
                for _j in range(ns):
                    sn = d[so:so+16].rstrip(b'\0').decode()
                    sa, ssz, sfo, al, ro, nr, sfl, r1, r2, r3 = struct.unpack_from('<QQIIIIIIII', d, so+32)
                    s = dict(seg=name, name=sn, addr=sa, size=ssz, fo=sfo, align=al, flags=sfl, r1=r1, r2=r2,
                             index=len(self.sects) + 1)
                    self.sects.append(s); seg['sects'].append(s)
                    so += 80
            elif cmd == LC_SYMTAB:
                self.symoff, self.nsyms, self.stroff, self.strsize = struct.unpack_from('<IIII', d, off + 8)
            elif cmd == LC_DYSYMTAB:
                self.dys = list(struct.unpack_from('<18I', d, off + 8))
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB):
                no = struct.unpack_from('<I', d, off + 8)[0]
                self.dylibs.append(d[off+no:off+sz].split(b'\0')[0].decode())
            off += sz

    def cmd(self, c):
        for (cmd, off, sz) in self.cmds:
            if cmd == c:
                return off, sz

    def seg(self, name):
        for s in self.segs:
            if s['name'] == name:
                return s

    def sect(self, seg, name):
        for s in self.sects:
            if s['seg'] == seg and s['name'] == name:
                return s

    def seg_of(self, a):
        for s in self.segs:
            if s['name'] != '__LINKEDIT' and s['vm'] <= a < s['vm'] + s['vms']:
                return s

    def sect_of(self, a):
        for s in self.sects:
            if s['addr'] <= a < s['addr'] + s['size']:
                return s

    def a2o(self, a):
        s = self.seg_of(a)
        if s is None or a - s['vm'] >= s['fs']:
            return None
        return s['fo'] + a - s['vm']

    def u64(self, a):
        o = self.a2o(a)
        return None if o is None else struct.unpack_from('<Q', self.d, o)[0]

    def u32(self, a):
        o = self.a2o(a)
        return None if o is None else struct.unpack_from('<I', self.d, o)[0]

    def s32(self, a):
        o = self.a2o(a)
        return None if o is None else struct.unpack_from('<i', self.d, o)[0]

    def put64(self, a, v):
        struct.pack_into('<Q', self.d, self.a2o(a), v)

    def put32(self, a, v):
        struct.pack_into('<I', self.d, self.a2o(a), v)

    def cstr(self, a, n=512):
        o = self.a2o(a)
        if o is None:
            return None
        e = self.d.find(b'\0', o, o + n)
        return bytes(self.d[o:e]).decode('latin1') if e >= 0 else None

    def sym(self, i):
        sx, ty, sc, de, va = struct.unpack_from('<IBBHQ', self.d, self.symoff + 16 * i)
        e = self.d.index(b'\0', self.stroff + sx)
        return dict(name=bytes(self.d[self.stroff + sx:e]).decode(), type=ty, sect=sc, desc=de, value=va, strx=sx)

    def indirect(self, i):
        return struct.unpack_from('<I', self.d, self.dys[12] + 4 * i)[0]

    def undefined(self):
        """name -> (ordinal, weak) of the undefined symbols."""
        out = {}
        for i in range(self.dys[4], self.dys[4] + self.dys[5]):
            s = self.sym(i)
            out[s['name']] = ((s['desc'] >> 8) & 0xff, bool(s['desc'] & N_WEAK_REF))
        return out

    def stubs(self):
        """[(stub addr, slot addr, symbol name)] for __TEXT.__stubs (adrp x16; ldr x16,[x16,#o]; br x16)."""
        out = []
        for s in self.sects:
            if s['name'] != '__stubs':
                continue
            esz = s['r2'] or 12
            for k in range(s['size'] // esz):
                a = s['addr'] + esz * k
                i1, i2 = struct.unpack_from('<II', self.d, s['fo'] + esz * k)
                page = adrp_target(a, i1)
                if page is None or (i2 & 0xffc003ff) != 0xf9400210:
                    raise SystemExit('unexpected stub at 0x%x' % a)
                isym = self.indirect(s['r1'] + k)
                out.append((a, page + ((i2 >> 10) & 0xfff) * 8, self.sym(isym)['name']))
        return out


def adrp_target(pc, ins):
    if (ins & 0x9f000000) != 0x90000000:
        return None
    immlo = (ins >> 29) & 3; immhi = (ins >> 5) & 0x7ffff
    imm = ((immhi << 2) | immlo) << 12
    if imm & (1 << 32):
        imm -= 1 << 33
    return (pc & ~0xfff) + imm


# ----------------------------------------------------------------------------------------------- cache
class Cache:
    """Split dyld shared cache reader: unslid addresses, slide info v2 (arm64)."""
    def __init__(self, cdir):
        self.maps = []
        for f in sorted(glob.glob(os.path.join(cdir, 'dyld_shared_cache_arm64*'))):
            if f.endswith(('.symbols', '.map', '.a2s')):
                continue
            fh = open(f, 'rb')
            mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
            if mm[:4] != b'dyld':
                continue
            mso, msc = struct.unpack_from('<II', mm, 0x138)
            for i in range(msc):
                a, s, fo, sio, sis, fl, mx, ip = struct.unpack_from('<QQQQQQII', mm, mso + i * 56)
                si = None
                if sis:
                    ver, psz, pso, psc, peo, pec, dm, va = struct.unpack_from('<IIIIIIQQ', mm, sio)
                    if ver != 2:
                        raise SystemExit('slide info v%d not supported (arm64 caches use v2)' % ver)
                    si = dict(psz=psz, dm=dm, va=va,
                              starts=struct.unpack_from('<%dH' % psc, mm, sio + pso),
                              extras=struct.unpack_from('<%dH' % pec, mm, sio + peo))
                self.maps.append(dict(addr=a, size=s, fo=fo, mm=mm, si=si, file=os.path.basename(f)))
        if not self.maps:
            raise SystemExit('no dyld cache in ' + cdir)
        self.lo = min(m['addr'] for m in self.maps)
        self.hi = max(m['addr'] + m['size'] for m in self.maps)

    def find(self, a):
        for m in self.maps:
            if m['addr'] <= a < m['addr'] + m['size']:
                return m

    def raw(self, a, fmt='<Q'):
        m = self.find(a)
        return None if m is None else struct.unpack_from(fmt, m['mm'], m['fo'] + a - m['addr'])[0]

    def ptr(self, a):
        m = self.find(a)
        if m is None:
            return None
        v = self.raw(a)
        if m['si']:
            v &= ~m['si']['dm'] & 0xffffffffffffffff
            if v:
                v += m['si']['va']
        return v

    def rebase_locs(self, lo, hi):
        out = set()
        for m in self.maps:
            si = m['si']
            if not si or hi <= m['addr'] or lo >= m['addr'] + m['size']:
                continue
            psz, dm = si['psz'], si['dm']
            shift = (dm & -dm).bit_length() - 1 - 2
            p0 = max(0, (lo - m['addr']) // psz)
            p1 = min(len(si['starts']), (hi - m['addr'] + psz - 1) // psz)
            for p in range(p0, p1):
                st = si['starts'][p]
                if st == 0x4000:
                    continue
                chains = []
                if st & 0x8000:
                    j = st & 0x3fff
                    while True:
                        e = si['extras'][j]
                        chains.append(e & 0x3fff)
                        if e & 0x8000:
                            break
                        j += 1
                else:
                    chains.append(st)
                base = m['addr'] + p * psz
                for ch in chains:
                    off = ch * 4
                    while True:
                        a = base + off
                        raw = struct.unpack_from('<Q', m['mm'], m['fo'] + a - m['addr'])[0]
                        if lo <= a < hi:
                            out.add(a)
                        delta = (raw & dm) >> shift
                        if not delta:
                            break
                        off += delta
        return sorted(out)

    def follow_stub(self, t):
        """Target of a cache stub (adrp x16; ldr x16,[x16]; br x16 or adrp; add; br)."""
        i1, i2 = self.raw(t, '<I'), self.raw(t + 4, '<I')
        page = adrp_target(t, i1 or 0)
        if page is None:
            return None
        if (i2 & 0xffc003ff) == 0xf9400210:
            return self.ptr(page + ((i2 >> 10) & 0xfff) * 8)
        if (i2 & 0xffc003ff) == 0x91000210:
            return page + ((i2 >> 10) & 0xfff)
        return None


def load_syms(path):
    """ipsw `dyld symaddr -o` JSON -> (addr -> set(names), name -> set(images))."""
    a2n, n2i = collections.defaultdict(set), collections.defaultdict(set)
    for e in json.load(open(path)):
        if 'address' in e and 'undefined' not in e.get('type', ''):
            a2n[e['address']].add(e['name'])
            n2i[e['name']].add(e['image'])
    return a2n, n2i


# ----------------------------------------------------------------------------------------------- text scan
def scan_text(m):
    """adrp-based references and branches from executable sections: returns (outside ldr targets,
    other outside refs, branches outside, refs per own segment)."""
    import capstone as cs
    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM)
    md.detail = True
    ldr_out = collections.Counter(); other_out = []; bl_out = []; own = collections.Counter()
    adrp_out = set(); consumed = set()
    for s in m.sects:
        if not (s['flags'] & 0x80000400):
            continue
        regs = {}; origin = {}
        code = bytes(m.d[s['fo']:s['fo'] + s['size']])
        pc = s['addr']
        while pc < s['addr'] + s['size']:
            got = False
            for ins in md.disasm(code[pc - s['addr']:], pc):
                got = True
                pc = ins.address + 4
                mn, ops = ins.mnemonic, ins.operands
                if mn == 'adrp':
                    regs[ops[0].reg] = ops[1].imm
                    origin[ops[0].reg] = ins.address
                    if m.seg_of(ops[1].imm) is None and m.seg_of(ops[1].imm + 0xfff) is None:
                        adrp_out.add(ins.address)
                    continue
                if mn in ('bl', 'b'):
                    if m.seg_of(ops[0].imm) is None:
                        bl_out.append(ins.address)
                    if mn == 'b':
                        regs.clear()
                    continue
                base = None
                if mn == 'add' and len(ops) == 3 and ops[1].type == cs.arm64.ARM64_OP_REG and ops[1].reg in regs \
                        and ops[2].type == cs.arm64.ARM64_OP_IMM:
                    base = regs[ops[1].reg] + ops[2].imm
                    consumed.add(origin.get(ops[1].reg))
                elif mn[:3] in ('ldr', 'str', 'ldp', 'stp'):
                    for o in ops:
                        if o.type == cs.arm64.ARM64_OP_MEM and o.mem.base in regs:
                            base = regs[o.mem.base] + o.mem.disp
                            consumed.add(origin.get(o.mem.base))
                try:
                    for r in ins.regs_access()[1]:
                        regs.pop(r, None)
                        nm = ins.reg_name(r)
                        if nm and nm[0] == 'w':
                            for k in list(regs):
                                if ins.reg_name(k) == 'x' + nm[1:]:
                                    regs.pop(k, None)
                except Exception:
                    pass
                if base is not None:
                    sg = m.seg_of(base)
                    if sg is None:
                        if mn == 'ldr':
                            ldr_out[base] += 1
                        else:
                            other_out.append((ins.address, mn, base))
                    else:
                        own[sg['name']] += 1
            if not got:
                pc += 4
    return ldr_out, other_out, bl_out, own, sorted(adrp_out - consumed)


# ----------------------------------------------------------------------------------------------- relink
class Relink:
    def __init__(self, a):
        self.a = a
        self.m = m = MachO(a.dx)
        self.c = Cache(a.cache)
        self.a2n, _ = load_syms(a.syms)
        self.undef = m.undefined()
        self.rep = collections.OrderedDict()
        self.err = []
        self.rebases = {}     # old loc -> old target
        self.binds = {}       # old loc -> (name, addend)
        if m.cputype != 0x0100000c:
            raise SystemExit('not arm64')
        if m.cpusub & 0xff == 2:
            raise SystemExit('arm64e input: PAC fixups are not implemented')

    def E(self, msg):
        self.err.append(msg)

    def name_at(self, v):
        names = self.a2n.get(v)
        if not names:
            return None
        cand = sorted(n for n in names if n in self.undef)
        if not cand:
            self.E('address 0x%x resolves to %s, not an undefined symbol of this image' % (v, sorted(names)))
            return None
        return cand[0]

    def run(self):
        m, c, a = self.m, self.c, self.a
        T = m.seg('__TEXT'); EX = m.seg('__EXTRA_OBJC')
        self.exlo, self.exhi = (EX['vm'], EX['vm'] + EX['vms']) if EX else (0, 0)
        self.delta = -T['vm']                      # new image base 0 (all 4K-multiple shift: adrp stays valid)
        assert T['vm'] % 0x1000 == 0
        self.rep['input'] = dict(file=a.dx, flags=hex(m.flags), segments={s['name']: hex(s['vm']) for s in m.segs})

        # 1. text: which cache-GOT-pool slots are used; nothing else may leave the image
        ldr_out, other_out, bl_out, own, lost = scan_text(m)
        if lost:
            self.E('%d adrp to outside pages without a tracked consumer: %s' % (len(lost), [hex(x) for x in lost[:10]]))
        if other_out:
            self.E('non-ldr text refs outside the image: %s' % [(hex(x), y, hex(z)) for x, y, z in other_out[:10]])
        if bl_out:
            self.E('%d branches leave the image' % len(bl_out))
        if own.get('__EXTRA_OBJC'):
            self.E('code references __EXTRA_OBJC directly (%d sites)' % own['__EXTRA_OBJC'])
        self.slots = sorted(ldr_out)
        self.rep['text'] = dict(pool_ldr_sites=sum(ldr_out.values()), pool_slots=len(self.slots),
                                own_refs=dict(own), branches_out=len(bl_out), untracked_outside_adrp=len(lost))
        stubname = {}
        for (sa, slot, nm) in m.stubs():
            stubname.setdefault(slot, set()).add(nm)
        for s in self.slots:
            if s % 8:
                self.E('unaligned pool slot 0x%x' % s)
            v = c.ptr(s)
            byval = self.name_at(v) if v else None
            if not byval and v:
                t = c.follow_stub(v)
                byval = self.name_at(t) if t else None
            sn = stubname.get(s)
            if sn and len(sn) > 1:
                self.E('slot 0x%x has several stub names %s' % (s, sn))
            nm = sorted(sn)[0] if sn else byval
            if sn and byval and byval != nm and self.a2n.get(v) and nm not in self.a2n.get(v, ()):
                self.rep.setdefault('slot_name_mismatch', []).append((hex(s), nm, byval))
            if not nm:
                self.E('pool slot 0x%x (value 0x%x) has no name' % (s, v or 0))
                continue
            self.binds[s] = (nm, 0)
        self.pool_lo, self.pool_hi = self.slots[0], self.slots[-1] + 8
        DC = m.seg('__DATA_CONST')
        for sg in m.segs:
            if sg is not DC and sg['vm'] < self.pool_hi and self.pool_lo < sg['vm'] + sg['vms']:
                self.E('GOT pool overlaps %s' % sg['name'])
        if not (DC['vm'] + DC['vms'] <= self.pool_lo):
            self.E('GOT pool not after __DATA_CONST')

        # 2. image-local __got (ld's original, unused by the cache-rewritten code): bind it as ld would
        g = m.sect('__DATA_CONST', '__got')
        self.own_got = 0
        if g:
            for k in range(g['size'] // 8):
                isym = m.indirect(g['r1'] + k)
                if isym & (INDIRECT_SYMBOL_ABS | 0x80000000):
                    continue
                self.binds[g['addr'] + 8 * k] = (m.sym(isym)['name'], 0)
                self.own_got += 1

        # 3. ObjC: protocols (EXTRA copies -> own definitions by name), classes
        self.objc_prepare()

        # 4. data pointers, from the source cache's slide info for this image's ranges
        locs = []
        for sg in m.segs:
            if sg['name'] in ('__DATA_CONST', '__DATA', '__DATA_DIRTY', '__DATA_DIRTY', '__AUTH', '__AUTH_CONST'):
                locs += c.rebase_locs(sg['vm'], sg['vm'] + sg['vms'])
        kinds = collections.Counter()
        for loc in locs:
            if loc in self.binds:
                continue
            v = m.u64(loc); v8 = c.ptr(loc)
            if v is None:                                 # zerofill
                if v8:
                    self.E('zerofill pointer 0x%x had 0x%x' % (loc, v8))
                continue
            if v and m.seg_of(v) and not self.in_ex(v):
                self.rebases[loc] = v; kinds['rebase-own'] += 1
            elif v and self.in_ex(v):
                if v in self.ex_proto:
                    self.rebases[loc] = self.ex_proto[v]; kinds['rebase-proto-to-own'] += 1
                elif self.a2n.get(v8):
                    nm = self.name_at(v8)
                    if nm:
                        self.binds[loc] = (nm, 0); kinds['bind-foreign-class-copy'] += 1
                else:
                    self.rebases[loc] = v; kinds['rebase-extra'] += 1
            elif v == 0:
                nm = self.name_at(v8) if v8 else None
                if nm:
                    self.binds[loc] = (nm, 0); kinds['bind-zeroed-by-extractor'] += 1
                elif loc in self.class_preopt:
                    kinds['zero-preopt-cache-field'] += 1      # stays 0, as ld emits it
                else:
                    self.E('zero pointer 0x%x (cache had 0x%x) unexplained' % (loc, v8 or 0))
            else:
                nm = self.name_at(v)
                if nm:
                    self.binds[loc] = (nm, 0); kinds['bind-external'] += 1
                else:
                    self.E('pointer 0x%x -> 0x%x outside image, unresolved' % (loc, v))
        self.rep['data'] = dict(slide_locations=len(locs), kinds=dict(kinds))

        # 5. __EXTRA_OBJC: keep only what is referenced (strings, selector slots), zero the rest
        if EX:
            self.extra_liveness()

        # 6. method lists, image info
        self.objc_fix()

        if self.err:
            return False
        self.write()
        return True

    def in_ex(self, v):
        return self.exlo <= v < self.exhi

    # ---- objc
    def objc_prepare(self):
        m = self.m
        self.classes = []
        cl = m.sect('__DATA_CONST', '__objc_classlist') or m.sect('__DATA', '__objc_classlist')
        for i in range(cl['size'] // 8 if cl else 0):
            k = m.u64(cl['addr'] + 8 * i)
            self.classes += [k, m.u64(k)]
        # class+0x18 is the shared-cache preopt-cache pointer (vtable slot for ld output: 0)
        self.class_preopt = set(k + 0x18 for k in self.classes)
        own = {}
        pl = m.sect('__DATA_CONST', '__objc_protolist') or m.sect('__DATA', '__objc_protolist')
        for i in range(pl['size'] // 8 if pl else 0):
            p = m.u64(pl['addr'] + 8 * i)
            own[m.cstr(m.u64(p + 8))] = p
        self.own_protos = own
        # EXTRA protocol copies reachable from class protocol lists / protocol sub-lists
        self.ex_proto = {}
        todo = []
        for k in self.classes:
            ro = m.u64(k + 32) & ~7
            bp = m.u64(ro + 40)
            if bp:
                todo += [m.u64(bp + 8 + 8 * j) for j in range(m.u64(bp))]
        for p in own.values():
            sub = m.u64(p + 16)
            if sub:
                todo += [m.u64(sub + 8 + 8 * j) for j in range(m.u64(sub))]
        for p in todo:
            if self.in_ex(p):
                nm = m.cstr(m.u64(p + 8))
                if nm not in own:
                    self.E('protocol %s only exists as a cache copy' % nm)
                else:
                    self.ex_proto[p] = own[nm]
        self.rep['objc'] = dict(classes=len(self.classes) // 2, own_protocols=sorted(own),
                                extra_protocol_copies_redirected=len(self.ex_proto))

    def method_lists(self):
        m = self.m; lists = set()
        for k in self.classes:
            ro = m.u64(k + 32) & ~7
            bm = m.u64(ro + 32)
            if bm:
                if bm & 1:
                    self.E('class 0x%x has a list-of-lists method pointer' % k)
                lists.add(bm & ~3)
        for p in self.own_protos.values():
            for o in (24, 32, 40, 48):
                ml = m.u64(p + o)
                if ml:
                    lists.add(ml)
        return sorted(lists)

    def extra_liveness(self):
        m = self.m
        live = bytearray(self.exhi - self.exlo)
        slots = set(); strings = set()
        for loc, t in self.rebases.items():
            if self.in_ex(t):
                strings.add(t)
        for ml in self.method_lists():
            ef, cnt = m.u32(ml), m.u32(ml + 4)
            if ef & 0x80000000:
                for i in range(cnt):
                    e = ml + 8 + 12 * i
                    n = e + m.s32(e)
                    if self.in_ex(n):
                        slots.add(n)
                    t = e + 4 + m.s32(e + 4)
                    if self.in_ex(t):
                        strings.add(t)
        for s in slots:
            v = m.u64(s)
            if not (v and (self.in_ex(v) or m.seg_of(v))):
                self.E('selector slot 0x%x has bad value 0x%x' % (s, v or 0))
                continue
            self.rebases[s] = v
            if self.in_ex(v):
                strings.add(v)
            live[s - self.exlo:s - self.exlo + 8] = b'\1' * 8
        for s in strings & slots:
            self.E('EXTRA address 0x%x used as both string and slot' % s)
        for s in strings:
            if s in slots:
                continue
            st = m.cstr(s, 4096)
            if st is None or not all(0x20 <= ord(ch) < 0x7f for ch in st):
                self.E('EXTRA target 0x%x is not a printable C string: %r' % (s, st))
                continue
            live[s - self.exlo:s - self.exlo + len(st) + 1] = b'\1' * (len(st) + 1)
        o = m.a2o(self.exlo)
        zeroed = 0
        for i in range(len(live)):
            if not live[i] and m.d[o + i]:
                m.d[o + i] = 0; zeroed += 1
        self.rep['extra_objc'] = dict(size=hex(self.exhi - self.exlo), selector_slots=len(slots),
                                      strings=len(strings - slots), live_bytes=sum(live), dead_bytes_zeroed=zeroed)

    def objc_fix(self):
        m = self.m
        fl = collections.Counter()
        for ml in self.method_lists():
            ef = m.u32(ml)
            fl[hex(ef)] += 1
            m.put32(ml, ef & ~3)                       # not uniqued/sorted: the runtime fixes them up
            if ef & 0x40000000:
                self.E('method list 0x%x uses cache selector offsets' % ml)
            if ef & 0x80000000:                        # relative: check offsets stay inside the image
                for i in range(m.u32(ml + 4)):
                    e = ml + 8 + 12 * i
                    for j in range(3):
                        t = e + 4 * j + m.s32(e + 4 * j)
                        if (j < 2 or m.s32(e + 8)) and not m.seg_of(t):
                            self.E('relative method entry 0x%x field %d leaves the image' % (e, j))
        for p in self.own_protos.values():
            pf = m.u32(p + 68)
            if pf & 0xe0000000:
                m.put32(p + 68, pf & ~0xe0000000)
        ii = m.sect('__DATA_CONST', '__objc_imageinfo') or m.sect('__DATA', '__objc_imageinfo')
        f0 = m.u32(ii['addr'] + 4)
        m.put32(ii['addr'] + 4, f0 & ~0x8)            # clear OptimizedByDyld
        self.rep['objc'].update(method_list_flags_before=dict(fl), imageinfo=[hex(f0), hex(f0 & ~8)])

    # ---- output
    def write(self):
        m, D = self.m, self.delta
        segs = []
        T = m.seg('__TEXT'); DC = m.seg('__DATA_CONST'); DA = m.seg('__DATA'); DD = m.seg('__DATA_DIRTY')
        EX = m.seg('__EXTRA_OBJC')
        # new segment ranges (old addresses + D, page aligned outward)
        def rng(lo, hi):
            return (lo + D) & ~(PAGE - 1), align(hi + D)
        plan = [('__TEXT', T['vm'], T['vm'] + T['vms'], [T]),
                ('__DATA_CONST', DC['vm'], self.pool_hi, [DC]),
                ('__DATA', DA['vm'], DA['vm'] + DA['vms'], [DA])]
        if DD and EX:
            plan.append(('__DATA_DIRTY', DD['vm'], EX['vm'] + EX['vms'], [DD, EX]))
        elif DD:
            plan.append(('__DATA_DIRTY', DD['vm'], DD['vm'] + DD['vms'], [DD]))
        for s in m.segs:
            if s['name'] not in ('__TEXT', '__DATA_CONST', '__DATA', '__DATA_DIRTY', '__EXTRA_OBJC', '__LINKEDIT'):
                self.E('unhandled segment ' + s['name'])
        if (T['vm'] + D) % PAGE:
            self.E('text not page aligned after shift')
        fo = 0
        prev_end = 0
        for (name, lo, hi, olds) in plan:
            vlo, vhi = rng(lo, hi)
            if vlo < prev_end:
                self.E('segment %s overlaps previous after alignment' % name)
            prev_end = vhi
            seg = dict(name=name, vm=vlo, vms=vhi - vlo, fo=fo, olds=olds, sects=[])
            # file-backed size: up to the end of the last non-zerofill byte
            fend = max(o['vm'] + o['fs'] for o in olds) + D
            if name == '__DATA_CONST':
                fend = max(fend, self.pool_hi + D)
            seg['fs'] = align(fend - vlo) if name != '__DATA' else align(fend - vlo)
            seg['fs'] = min(seg['fs'], seg['vms'])
            fo += seg['fs']
            segs.append(seg)
        # sections
        newsects = []
        for seg in segs:
            for o in seg['olds']:
                for s in o['sects']:
                    ns = dict(s); ns['addr'] = s['addr'] + D
                    if seg['name'] == '__DATA_DIRTY' and (s['flags'] & 0xff) == S_ZEROFILL:
                        ns['flags'] = s['flags'] & ~0xff        # inside the file-backed range now
                    if (ns['flags'] & 0xff) == S_ZEROFILL:
                        ns['fo'] = 0
                    else:
                        ns['fo'] = seg['fo'] + ns['addr'] - seg['vm']
                    ns['old_index'] = s['index']
                    seg['sects'].append(ns)
            if seg['name'] == '__DATA_CONST':
                seg['sects'].append(dict(seg='__DATA_CONST', name='__pool_got', addr=self.pool_lo + D,
                                         size=self.pool_hi - self.pool_lo, fo=seg['fo'] + self.pool_lo + D - seg['vm'],
                                         align=3, flags=S_NON_LAZY_SYMBOL_POINTERS, r1=0, r2=0, old_index=None))
            if seg['name'] == '__DATA_DIRTY' and EX:
                seg['sects'].append(dict(seg='__DATA_DIRTY', name='__extra_objc', addr=EX['vm'] + D, size=EX['vms'],
                                         fo=seg['fo'] + EX['vm'] + D - seg['vm'], align=4, flags=0, r1=0, r2=0,
                                         old_index=None))
            first = min(x['addr'] for x in seg['sects'])
            if seg['name'] != '__TEXT' and first > seg['vm']:
                # alignment padding as a section, so tools that take a segment's start from its first
                # section (llvm-objdump's bind/rebase decoder) agree with dyld, which uses vmaddr
                seg['sects'].insert(0, dict(seg=seg['name'], name='__decache_pad', addr=seg['vm'], size=first - seg['vm'],
                                            fo=seg['fo'], align=0, flags=0, r1=0, r2=0, old_index=None))
            newsects += seg['sects']
        secmap = {}
        for i, s in enumerate(newsects):
            if s['old_index']:
                secmap[s['old_index']] = i + 1
        # body
        le_fo = fo
        body = bytearray(le_fo)
        for seg in segs:
            for o in seg['olds']:
                n = o['fs']
                dst = seg['fo'] + o['vm'] + D - seg['vm']
                body[dst:dst + n] = m.d[o['fo']:o['fo'] + n]
        segidx = {s['name']: i for i, s in enumerate(segs)}

        def where(old):
            new = old + D
            for i, s in enumerate(segs):
                if s['vm'] <= new < s['vm'] + s['vms']:
                    return i, new - s['vm'], s
            raise SystemExit('address 0x%x outside new segments' % old)

        def put(old, v):
            i, off, s = where(old)
            if off >= s['fs']:
                raise SystemExit('write into zerofill at 0x%x' % old)
            struct.pack_into('<Q', body, s['fo'] + off, v)

        # rebases: store new unslid target; binds: store 0 (dyld writes the value)
        for loc, t in self.rebases.items():
            if m.seg_of(t) is None:
                raise SystemExit('rebase target 0x%x outside image' % t)
            put(loc, t + D)
        for loc in self.binds:
            put(loc, 0)
        # pool got indirect entries
        undef_names = {}
        for i in range(m.dys[4], m.dys[4] + m.dys[5]):
            undef_names[m.sym(i)['name']] = i
        ind = bytearray(m.d[m.dys[12]:m.dys[12] + 4 * m.dys[13]])
        pool_r1 = m.dys[13]
        for a in range(self.pool_lo, self.pool_hi, 8):
            b = self.binds.get(a)
            ind += struct.pack('<I', undef_names[b[0]] if b else INDIRECT_SYMBOL_ABS)
        for s in newsects:
            if s['name'] == '__pool_got':
                s['r1'] = pool_r1
        # opcodes
        rebase = bytearray([0x11])                      # REBASE_OPCODE_SET_TYPE_IMM | POINTER
        cur = None
        pend = 0
        for loc in sorted(self.rebases):
            i, off, s = where(loc)
            if cur and cur[0] == i and cur[1] == off and pend < 15:
                pend += 1; cur = (i, off + 8); continue
            if pend:
                rebase.append(0x50 | pend); pend = 0
            if cur and cur[0] == i and off > cur[1]:
                rebase += bytes([0x30]) + uleb(off - cur[1])
            elif not (cur and cur[0] == i and off == cur[1]):
                rebase += bytes([0x20 | i]) + uleb(off)
            pend = 1; cur = (i, off + 8)
        if pend:
            rebase.append(0x50 | pend)
        rebase.append(0)
        bind = bytearray()
        byname = collections.defaultdict(list)
        for loc, (nm, add) in self.binds.items():
            byname[(nm, add)].append(loc)
        nbind = 0
        for (nm, add) in sorted(byname):
            ordn, weak = self.undef.get(nm, (None, False))
            if ordn is None or ordn == 0 or ordn > len(m.dylibs):
                self.E('bind %s has no library ordinal' % nm)
                continue
            bind += bytes([0x10 | ordn]) if ordn < 16 else bytes([0x20]) + uleb(ordn)
            bind += bytes([0x40 | (1 if weak else 0)]) + nm.encode() + b'\0'
            bind += bytes([0x51])                       # BIND_TYPE_POINTER
            bind += bytes([0x60]) + sleb(add)
            for loc in sorted(byname[(nm, add)]):
                i, off, s = where(loc)
                bind += bytes([0x70 | i]) + uleb(off) + bytes([0x90])
                nbind += 1
        bind.append(0)
        if self.err:
            raise SystemExit('\n'.join(self.err))
        # linkedit pieces
        def blob(cmd):
            r = m.cmd(cmd)
            if not r:
                return b''
            o, sz = struct.unpack_from('<II', m.d, r[0] + 8)
            return bytes(m.d[o:o + sz])
        exports = blob(LC_DYLD_EXPORTS_TRIE)
        if not exports:
            raise SystemExit('input has no LC_DYLD_EXPORTS_TRIE')
        fstarts = blob(LC_FUNCTION_STARTS)
        dic = blob(LC_DATA_IN_CODE)
        syms = bytearray()
        for i in range(m.nsyms):
            sx, ty, sc, de, va = struct.unpack_from('<IBBHQ', m.d, m.symoff + 16 * i)
            if (ty & N_TYPE) == N_SECT and sc:
                if sc not in secmap:
                    raise SystemExit('symbol %d in unknown section %d' % (i, sc))
                sc = secmap[sc]; va += D
            syms += struct.pack('<IBBHQ', sx, ty, sc, de, va)
        strs = bytes(m.d[m.stroff:m.stroff + m.strsize])

        le = bytearray()
        def add(b, al=8):
            while len(le) % al:
                le.append(0)
            o = le_fo + len(le); le.extend(b); return o, len(b)
        rb_o, rb_s = add(rebase); bd_o, bd_s = add(bind); ex_o, ex_s = add(exports)
        fs_o, fs_s = add(fstarts); dc_o, dc_s = add(dic); sy_o, _ = add(syms); in_o, _ = add(ind)
        st_o, st_s = add(strs)
        while len(le) % 16:
            le.append(0)
        # load commands
        cmds = []
        for seg in segs + [dict(name='__LINKEDIT', vm=segs[-1]['vm'] + segs[-1]['vms'], vms=align(len(le)),
                                fo=le_fo, fs=len(le), sects=[])]:
            prot = 5 if seg['name'] == '__TEXT' else (1 if seg['name'] == '__LINKEDIT' else 3)
            c = struct.pack('<II16sQQQQiiII', LC_SEGMENT_64, 72 + 80 * len(seg['sects']), seg['name'].encode(),
                            seg['vm'], seg['vms'], seg['fo'], seg['fs'], prot, prot, len(seg['sects']), 0)
            for s in seg['sects']:
                c += struct.pack('<16s16sQQIIIIIIII', s['name'].encode(), s['seg'].encode(), s['addr'], s['size'],
                                 s['fo'], s['align'], 0, 0, s['flags'], s['r1'], s['r2'], 0)
            cmds.append(c)
        new_uuid = uuid.uuid5(uuid.NAMESPACE_OID, 'decache:' + self.old_uuid() + ':' + os.path.basename(self.a.dx))
        for (cmd, off, sz) in m.cmds:
            if cmd in (LC_SEGMENT_64, LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS, LC_SEGMENT_SPLIT_INFO,
                       LC_CODE_SIGNATURE):
                continue
            raw = bytearray(m.d[off:off + sz])
            if cmd == LC_SYMTAB:
                struct.pack_into('<IIII', raw, 8, sy_o, m.nsyms, st_o, st_s)
            elif cmd == LC_DYSYMTAB:
                dy = list(m.dys)
                dy[12], dy[13] = in_o, len(ind) // 4
                for k in (6, 7, 8, 9, 10, 11, 14, 15, 16, 17):
                    dy[k] = 0
                struct.pack_into('<18I', raw, 8, *dy)
            elif cmd == LC_UUID:
                raw[8:24] = new_uuid.bytes
            elif cmd == LC_FUNCTION_STARTS:
                struct.pack_into('<II', raw, 8, fs_o, fs_s)
            elif cmd == LC_DATA_IN_CODE:
                struct.pack_into('<II', raw, 8, dc_o, dc_s)
            cmds.append(bytes(raw))
            if cmd == LC_DYSYMTAB:
                pass
        # dyld info right after the segments (ld64 order)
        info = struct.pack('<12I', LC_DYLD_INFO_ONLY, 48, rb_o, rb_s, bd_o, bd_s, 0, 0, 0, 0, ex_o, ex_s)
        cmds.insert(len(segs) + 1, info)
        lc = b''.join(cmds)
        first = min(s['fo'] for s in newsects if s['seg'] == '__TEXT' and s['fo'])
        if 32 + len(lc) + 32 > first:
            raise SystemExit('load commands do not fit before __text (%d > %d)' % (32 + len(lc), first))
        hdr = struct.pack('<IIIIIIII', 0xfeedfacf, m.cputype, m.cpusub, 6, len(cmds), len(lc),
                          m.flags & ~MH_DYLIB_IN_CACHE, 0)
        body[0:32 + len(lc)] = hdr + lc
        body[32 + len(lc):first] = bytes(first - 32 - len(lc))
        out = bytes(body) + bytes(le)
        open(self.a.out, 'wb').write(out)
        self.rep['output'] = dict(file=self.a.out, size=len(out), uuid=str(new_uuid).upper(), old_uuid=self.old_uuid(),
                                  flags=hex(m.flags & ~MH_DYLIB_IN_CACHE), fixups='LC_DYLD_INFO_ONLY',
                                  rebases=len(self.rebases), binds=nbind, bind_symbols=len(byname),
                                  own_got_binds=self.own_got,
                                  segments=[(s['name'], hex(s['vm']), hex(s['vms']), hex(s['fo']), hex(s['fs']))
                                            for s in segs] + [('__LINKEDIT', hex(le_fo), hex(len(le)))])

    def old_uuid(self):
        r = self.m.cmd(LC_UUID)
        return str(uuid.UUID(bytes=bytes(self.m.d[r[0] + 8:r[0] + 24]))).upper()


# ----------------------------------------------------------------------------------------------- verify
def decode_fixups(m):
    """Decode LC_DYLD_INFO(_ONLY) rebase + bind (non-lazy) opcodes -> (rebases [addr], binds [(addr, ord, name, add, flags)])."""
    r = m.cmd(LC_DYLD_INFO_ONLY) or m.cmd(0x22)
    if not r:
        return None, None
    rb_o, rb_s, bd_o, bd_s, wb_o, wb_s, lb_o, lb_s = struct.unpack_from('<8I', m.d, r[0] + 8)
    d = m.d
    segs = [s for s in m.segs]
    rebases = []
    p, end = rb_o, rb_o + rb_s
    seg = off = 0
    while p < end:
        b = d[p]; p += 1
        op, imm = b & 0xf0, b & 0x0f
        if op == 0x00:
            break
        elif op == 0x10:
            if imm != 1:
                raise ValueError('rebase type %d' % imm)
        elif op == 0x20:
            seg = imm; off, p = read_uleb(d, p)
        elif op == 0x30:
            v, p = read_uleb(d, p); off += v
        elif op == 0x40:
            off += imm * 8
        elif op == 0x50:
            for _ in range(imm):
                rebases.append(segs[seg]['vm'] + off); off += 8
        elif op == 0x60:
            n, p = read_uleb(d, p)
            for _ in range(n):
                rebases.append(segs[seg]['vm'] + off); off += 8
        elif op == 0x70:
            rebases.append(segs[seg]['vm'] + off); v, p = read_uleb(d, p); off += 8 + v
        elif op == 0x80:
            n, p = read_uleb(d, p); sk, p = read_uleb(d, p)
            for _ in range(n):
                rebases.append(segs[seg]['vm'] + off); off += 8 + sk
        else:
            raise ValueError('rebase opcode 0x%x' % b)
    binds = []
    p, end = bd_o, bd_o + bd_s
    ordn = 0; name = None; flags = 0; add = 0; seg = off = 0
    while p < end:
        b = d[p]; p += 1
        op, imm = b & 0xf0, b & 0x0f
        if op == 0x00:
            break
        elif op == 0x10:
            ordn = imm
        elif op == 0x20:
            ordn, p = read_uleb(d, p)
        elif op == 0x30:
            ordn = -imm if imm else 0
        elif op == 0x40:
            flags = imm; e = d.index(b'\0', p); name = bytes(d[p:e]).decode(); p = e + 1
        elif op == 0x50:
            if imm != 1:
                raise ValueError('bind type %d' % imm)
        elif op == 0x60:
            add, p = read_sleb(d, p)
        elif op == 0x70:
            seg = imm; off, p = read_uleb(d, p)
        elif op == 0x80:
            v, p = read_uleb(d, p); off += v
        elif op == 0x90:
            binds.append((segs[seg]['vm'] + off, ordn, name, add, flags)); off += 8
        elif op == 0xa0:
            binds.append((segs[seg]['vm'] + off, ordn, name, add, flags)); v, p = read_uleb(d, p); off += 8 + v
        elif op == 0xb0:
            binds.append((segs[seg]['vm'] + off, ordn, name, add, flags)); off += 8 + imm * 8
        elif op == 0xc0:
            n, p = read_uleb(d, p); sk, p = read_uleb(d, p)
            for _ in range(n):
                binds.append((segs[seg]['vm'] + off, ordn, name, add, flags)); off += 8 + sk
        else:
            raise ValueError('bind opcode 0x%x' % b)
    return rebases, binds


def reexport_closure(xcache, dylibs):
    """path -> set of image basenames reachable via LC_REEXPORT_DYLIB in the target cache (uses ipsw)."""
    memo = {}
    def lcs(path):
        if path not in memo:
            out = subprocess.run(['ipsw', 'dyld', 'macho', xcache, path], capture_output=True, text=True).stdout
            memo[path] = re.findall(r'LC_REEXPORT_DYLIB\s+(\S+)', out), bool(re.search(r'LC_ID_DYLIB\s+' + re.escape(path), out))
        return memo[path]
    res = {}
    for p in dylibs:
        seen, todo = set(), [p]
        while todo:
            q = todo.pop()
            if q in seen:
                continue
            seen.add(q)
            todo += lcs(q)[0]
        res[p] = dict(images=sorted(seen), present=lcs(p)[1])
    return res


def verify(a):
    m = MachO(a.dylib)
    rep = collections.OrderedDict(); bad = []
    def chk(ok, what):
        rep.setdefault('checks', []).append(('PASS' if ok else 'FAIL') + ': ' + what)
        if not ok:
            bad.append(what)
    chk(m.ftype == 6, 'MH_DYLIB')
    chk(m.cputype == 0x0100000c and m.cpusub & 0xff == 0, 'cpu arm64 (subtype %d)' % (m.cpusub & 0xff))
    chk(not m.flags & MH_DYLIB_IN_CACHE, 'MH_DYLIB_IN_CACHE clear (flags 0x%x)' % m.flags)
    idr = m.cmd(0xd)
    chk(idr and m.d[idr[0] + 24:idr[0] + 24 + 23] == b'/usr/lib/libBKDM1.dylib', 'LC_ID_DYLIB /usr/lib/libBKDM1.dylib')
    chk(len(m.dylibs) == 17, '%d LC_LOAD_DYLIBs' % len(m.dylibs))
    prev = -1
    for s in m.segs:
        chk(s['vm'] % PAGE == 0 and s['fo'] % PAGE == 0, 'segment %s vm 0x%x fo 0x%x page aligned' % (s['name'], s['vm'], s['fo']))
        chk(s['vm'] >= prev, 'segment %s ordered, no overlap' % s['name'])
        prev = s['vm'] + s['vms']
        for x in s['sects']:
            chk(s['vm'] <= x['addr'] and x['addr'] + x['size'] <= s['vm'] + s['vms'], 'section %s,%s inside segment' % (s['name'], x['name']))
    chk(m.cmd(LC_DYLD_CHAINED_FIXUPS) is None and m.cmd(LC_DYLD_INFO_ONLY) is not None, 'LC_DYLD_INFO_ONLY, no chained fixups')
    rebases, binds = decode_fixups(m)
    chk(rebases is not None, 'fixups decode')
    datasegs = ('__DATA_CONST', '__DATA', '__DATA_DIRTY')
    for r in rebases:
        sg = m.seg_of(r)
        if not (sg and sg['name'] in datasegs and r % 8 == 0):
            bad.append('rebase location 0x%x' % r); break
        t = m.u64(r)
        if not (t is not None and m.seg_of(t)):
            bad.append('rebase 0x%x target 0x%x outside image' % (r, t or 0)); break
    else:
        chk(True, '%d rebases: all locations in data segments, all targets inside the image' % len(rebases))
    bl = [b[0] for b in binds]
    chk(len(bl) == len(set(bl)) and not set(bl) & set(rebases), '%d binds, no location bound twice or both rebased and bound' % len(binds))
    # binds vs target cache exports
    _, n2i = load_syms(a.xsyms)
    rx = reexport_closure(a.xcache, m.dylibs)
    rep['dylibs'] = {p: v['images'] for p, v in rx.items()}
    for p, v in rx.items():
        chk(v['present'], 'dependency in target cache: ' + p)
    unres = []
    for (loc, ordn, name, add, fl) in binds:
        dep = m.dylibs[ordn - 1]
        imgs = set(os.path.basename(i) for i in rx[dep]['images'])
        if not (n2i.get(name, set()) & imgs):
            unres.append((hex(loc), name, os.path.basename(dep), sorted(n2i.get(name, ()))))
    chk(not unres, 'every bind resolves by name in its dylib (or a re-export) in the target cache: %d symbols, %d unresolved %s'
        % (len(set(b[2] for b in binds)), len(unres), unres[:5]))
    # pointers into the source cache
    boundset, rbset = set(bl), set(rebases)
    if a.cache:
        c = Cache(a.cache)
        lo, hi = c.lo, c.hi
    else:
        lo, hi = 0x180000000, 0x280000000
    sus = []
    for s in m.sects:
        if s['seg'] in datasegs and (s['flags'] & 0xff) != S_ZEROFILL and s['fo']:
            for x in range(s['addr'] & ~7, s['addr'] + s['size'] - 7, 8):
                v = m.u64(x)
                if v and lo <= v < hi:
                    sus.append((x, v, s['name']))
    real = [x for x in sus if x[0] in boundset or x[0] in rbset]
    chk(not real, 'no rebase/bind location holds a source-cache address (%d)' % len(real))
    nonptr = [x for x in sus if (x[1] & 0xffffffff) < 0x10000]
    chk(len(nonptr) == len(sus), '8-byte values in __DATA* that fall in the source cache range [0x%x,0x%x): %d, all of them '
        'int32 pairs (list headers: entsize/flags < 0x10000, count in the high word), not pointers: %s'
        % (lo, hi, len(sus), sorted(set('%s=0x%x' % (n, v) for _, v, n in sus))))
    rep['source_cache_range_values'] = [(hex(x), hex(v), n) for x, v, n in sus]
    pg = m.sect('__DATA_CONST', '__pool_got')
    g = m.sect('__DATA_CONST', '__got')
    for sct in (pg, g):
        if sct:
            n = sum(1 for x in range(sct['addr'], sct['addr'] + sct['size'], 8) if m.u64(x))
            nb = sum(1 for x in bl if sct['addr'] <= x < sct['addr'] + sct['size'])
            chk(n == 0, '%s: %d slots bound, %d non-zero slot values' % (sct['name'], nb, n))
    # objc
    ii = m.sect('__DATA_CONST', '__objc_imageinfo')
    chk(not m.u32(ii['addr'] + 4) & 8, 'objc imageinfo flags 0x%x (OptimizedByDyld clear)' % m.u32(ii['addr'] + 4))
    objc_check(m, binds, chk, rep)
    rep['verdict'] = 'PASS' if not bad else 'FAIL'
    rep['failures'] = bad
    return rep


def objc_check(m, binds, chk, rep):
    """Walk the ObjC metadata of the output the way the runtime will see it."""
    bmap = {b[0]: b[2] for b in binds}
    def printable(a):
        t = m.cstr(a, 4096) if a else None
        return t is not None and len(t) > 0 and all(0x20 <= ord(ch) < 0x7f for ch in t)
    text = m.sect('__TEXT', '__text')
    cl = m.sect('__DATA_CONST', '__objc_classlist')
    st = collections.Counter(); probs = []
    names = []
    for i in range(cl['size'] // 8):
        k = m.u64(cl['addr'] + 8 * i)
        for kind, c in (('class', k), ('meta', m.u64(k))):
            if not c or not m.seg_of(c):
                probs.append('class ptr %d' % i); continue
            ro = m.u64(c + 32) & ~7
            nm = m.cstr(m.u64(ro + 24))
            if kind == 'class':
                names.append(nm)
            for fld, off in (('isa', 0), ('superclass', 8), ('cache', 16)):
                loc = c + off
                if loc in bmap:
                    want = {'isa': '_OBJC_METACLASS_$_', 'superclass': '_OBJC_METACLASS_$_' if kind == 'meta' else '_OBJC_CLASS_$_',
                            'cache': '__objc_empty_cache'}[fld]
                    if not bmap[loc].startswith(want):
                        probs.append('%s.%s %s bound to %s' % (nm, kind, fld, bmap[loc]))
                    st[fld + ':bind'] += 1
                elif fld == 'cache':
                    probs.append('%s.%s cache not bound' % (nm, kind))
                elif m.u64(loc) and m.seg_of(m.u64(loc)):
                    st[fld + ':own'] += 1
                elif not (kind == 'class' and fld == 'superclass' and m.u64(loc) == 0):
                    probs.append('%s.%s %s = 0x%x' % (nm, kind, fld, m.u64(loc) or 0))
            if m.u64(c + 24):
                probs.append('%s vtable/preopt field not 0' % nm)
            bm = m.u64(ro + 32)
            if bm:
                ef, cnt = m.u32(bm), m.u32(bm + 4)
                if ef & 3:
                    probs.append('%s method list flags 0x%x' % (nm, ef))
                for j in range(cnt):
                    if ef & 0x80000000:
                        e = bm + 8 + 12 * j
                        slot = e + m.s32(e)
                        sel = m.u64(slot)
                        ok = printable(sel) and printable(e + 4 + m.s32(e + 4)) and \
                            text['addr'] <= e + 8 + m.s32(e + 8) < text['addr'] + text['size']
                    else:
                        e = bm + 8 + (ef & 0xfffc) * j
                        ok = printable(m.u64(e)) and printable(m.u64(e + 8))
                    st['methods'] += 1
                    if not ok:
                        probs.append('%s method %d unreadable' % (nm, j))
    pl = m.sect('__DATA_CONST', '__objc_protolist')
    for i in range(pl['size'] // 8):
        p = m.u64(pl['addr'] + 8 * i)
        if m.u32(p + 68) & 0xe0000000:
            probs.append('protocol %s flags 0x%x' % (m.cstr(m.u64(p + 8)), m.u32(p + 68)))
        for off in (24, 32, 40, 48):
            ml = m.u64(p + off)
            if ml and m.u32(ml) & 3:
                probs.append('protocol method list flags')
            if ml:
                for j in range(m.u32(ml + 4)):
                    e = ml + 8 + (m.u32(ml) & 0xfffc) * j
                    st['protocol_methods'] += 1
                    if not (printable(m.u64(e)) and printable(m.u64(e + 8))):
                        probs.append('protocol method unreadable')
    for sec in ('__objc_selrefs',):
        x = m.sect('__DATA_CONST', sec)
        bad = sum(1 for a in range(x['addr'], x['addr'] + x['size'], 8) if not printable(m.u64(a)))
        st['selrefs'] = x['size'] // 8
        if bad:
            probs.append('%d selrefs unreadable' % bad)
    rep['objc'] = dict(classes=names, stats=dict(st))
    chk(not probs, 'ObjC metadata: %d classes, %d methods, %d protocol methods, %d selrefs readable; isa/superclass/cache '
        'binds %s; problems %s' % (len(names), st['methods'], st['protocol_methods'], st['selrefs'],
                                   {k: v for k, v in st.items() if ':' in k}, probs[:8]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    r = sp.add_parser('relink')
    r.add_argument('--dx', required=True); r.add_argument('--cache', required=True)
    r.add_argument('--syms', required=True); r.add_argument('-o', '--out', required=True)
    r.add_argument('--report')
    v = sp.add_parser('verify')
    v.add_argument('dylib'); v.add_argument('--xsyms', required=True); v.add_argument('--xcache', required=True)
    v.add_argument('--cache'); v.add_argument('--report')
    a = ap.parse_args()
    if a.cmd == 'relink':
        rl = Relink(a)
        ok = rl.run()
        rl.rep['errors'] = rl.err
        txt = json.dumps(rl.rep, indent=1)
        if a.report:
            open(a.report, 'w').write(txt)
        print(txt)
        sys.exit(0 if ok else 1)
    rep = verify(a)
    txt = json.dumps(rep, indent=1)
    if a.report:
        open(a.report, 'w').write(txt)
    print(txt)
    sys.exit(0 if rep['verdict'] == 'PASS' else 1)


if __name__ == '__main__':
    main()
