#!/usr/bin/env python3
"""ca-variants.py QC [options]: CoreAnimation's uber-shader variants as the hakios plug-in reads them, off the phone.

QC is iOS's QuartzCore.framework directory, or any directory above one (apple-ref/16.7/ with the shared cache's
System/Library/Frameworks/QuartzCore.framework under it). Nothing here is Apple's: the files are read where they lie
and nothing derived from them is written into this repository (--table refuses a path inside it).

What is read, the same way as HakiosMetalCA.m's hm_ca_load() (pinned hakios 2631f5d) reads it on the phone at run time:
  default.pipelinelib/pipelines.desc  u32 section count, u32 section offsets; section 1 = the functions' labels
                                      (u32 n, n u32 offsets relative to the section, each label a C string at +16 from
                                      its offset), section 7 = the pipelines (u32 n, n offsets; in each entry the
                                      strings com.apple.coreanimation.{draw,vert,frag}.*)
  default.pipelinelib/functions       the variants: MTLB libraries back to back, one function each, in section 1's order
  default.metallib                    (iOS 16.7 has it, hakios' iPhone X 16.5 image had not) the unspecialised functions;
                                      the plug-in's exact and own picks compile fixed_* from it with CoreAnimation's
                                      constants
Labels are keyed by their full string (com.apple.coreanimation.frag.TimgA2Xhf_Isrc); a run-time pipeline's fragment
label is its draw label after the first '_'; hm_ca_nearest() takes the frag label's form
T(sampled)B(blend)D(..)A(varyings)S(2nd sampler)L(..)X(flags)_inputs. Function constants are not in any label: the
variants are already specialised, and a pipeline's own constants come from Metal at run time.

Options:
  --table OUT       write OUT (TSV: kind, label, vert, frag) for diffing versions; OUT must be outside the repository
  --compare QC2     labels and pipelines only in one of the two (e.g. apple-ref/16.5 against apple-ref/16.7)
  --replay LOG      each draw label in a plug-in log (bbd-hm.log): what exact / own / nearest would compile for it
  --selftest        the readers and the nearest pick on a synthetic library (no Apple file needed)
"""
import os
import re
import struct
import sys
import tempfile

PREFIX = 'com.apple.coreanimation.'
FORM = re.compile(r'^(T[a-z0-9]{3})?(B[a-z]{3})?(D[a-z]{2})?(A[0-9])(S[0-9])?(L[a-z]{2})?X([a-z]*)((?:_[A-Za-z0-9]+)*)$')


def find_qc(path):
    if os.path.isdir(os.path.join(path, 'default.pipelinelib')) or os.path.isfile(os.path.join(path, 'default.metallib')):
        return path
    direct = os.path.join(path, 'System/Library/Frameworks/QuartzCore.framework')
    if os.path.isdir(direct):
        return direct
    for root, dirs, _files in os.walk(path):
        if os.path.basename(root) == 'QuartzCore.framework':
            return root
        dirs.sort()
    sys.exit('%s: no QuartzCore.framework (with default.pipelinelib or default.metallib) under it' % path)


def mtlb_functions(lib):
    """[(name, {tag: bytes})] of one thin MTLB library."""
    if lib[:4] != b'MTLB' or len(lib) < 0x58:
        return []
    funcs = struct.unpack_from('<Q', lib, 0x18)[0]
    n = struct.unpack_from('<I', lib, funcs)[0]
    out, q = [], funcs + 4
    for _ in range(n):
        size = struct.unpack_from('<I', lib, q)[0]
        tags, t, end = {}, q + 4, q + size
        while t + 4 <= end and lib[t:t + 4] != b'ENDT':
            ln = struct.unpack_from('<H', lib, t + 4)[0]
            tags[lib[t:t + 4].decode('latin-1')] = lib[t + 6:t + 6 + ln]
            t += 6 + ln
        out.append((tags.get('NAME', b'').split(b'\0')[0].decode('latin-1'), tags))
        q += size
    return out


def thin(data, what):
    """The air64 slice of a universal library, or the library itself."""
    if data[:4] == b'\xca\xfe\xba\xbe':
        n = struct.unpack_from('>I', data, 4)[0]
        for i in range(n):
            cpu, _sub, off, size, _al = struct.unpack_from('>iiIII', data, 8 + 20 * i)
            if cpu == 0x01000017:
                return data[off:off + size]
        sys.exit('%s: universal, no air64 slice' % what)
    return data


def read_pipelinelib(qc):
    """(labels, variants {label: function name}, pipelines {draw: (vert, frag|None)}, frag_vert {frag: vert}, multi {frag: set})"""
    d = open(os.path.join(qc, 'default.pipelinelib', 'pipelines.desc'), 'rb').read()
    f = open(os.path.join(qc, 'default.pipelinelib', 'functions'), 'rb').read()
    sections = struct.unpack_from('<I', d, 0)[0]
    if sections > 16 or 4 + 4 * sections > len(d):
        sys.exit('pipelines.desc: %d sections: not the layout the plug-in reads' % sections)
    off = struct.unpack_from('<%dI' % sections, d, 4)
    labels = []
    if sections > 7:
        n = struct.unpack_from('<I', d, off[1])[0]
        for i in range(n):
            r = struct.unpack_from('<I', d, off[1] + 4 + 4 * i)[0]
            s = d[off[1] + r + 16:off[1] + r + 16 + 200].split(b'\0')[0]
            labels.append(s.decode('latin-1'))
    variants, at = {}, 0
    for label in labels:
        m = f.find(b'MTLB', at)
        if m < 0:
            break
        total = struct.unpack_from('<Q', f, m + 0x10)[0]
        fns = mtlb_functions(f[m:m + total])
        variants[label] = fns[0][0] if fns else '?'
        at = m + total
    pipelines, frag_vert, multi = {}, {}, {}
    if sections > 8:
        n = struct.unpack_from('<I', d, off[7])[0]
        for i in range(n):
            r = struct.unpack_from('<I', d, off[7] + 4 + 4 * i)[0]
            nxt = struct.unpack_from('<I', d, off[7] + 4 + 4 * (i + 1))[0] if i + 1 < n else off[8] - off[7]
            draw = vert = frag = None
            q = r
            while q + 24 < nxt and off[7] + q + 24 < len(d):
                s = d[off[7] + q:]
                if not s.startswith(PREFIX.encode()):
                    q += 1
                    continue
                label = s.split(b'\0')[0].decode('latin-1')
                if label.startswith(PREFIX + 'draw.'):
                    draw = label
                elif label.startswith(PREFIX + 'vert.'):
                    vert = label
                elif label.startswith(PREFIX + 'frag.'):
                    frag = label
                q += len(label)
            if draw and vert:
                pipelines[draw] = (vert, frag)
                if frag:
                    frag_vert[frag] = vert          # (the plug-in keeps the last, as here)
                    multi.setdefault(frag, set()).add(vert)
    return labels, variants, pipelines, frag_vert, {k: v for k, v in multi.items() if len(v) > 1}


def nearest(frag, variants, frag_vert):
    """hm_ca_nearest(): (best label or None, cost)."""
    fp = PREFIX + 'frag.'
    if not frag.startswith(fp):
        return None, None
    want = frag[len(fp):]
    w = FORM.match(want)
    if not w:
        return None, None
    best, best_cost = None, None
    for cand in frag_vert:
        if not cand.startswith(fp) or cand not in variants:
            continue
        have = cand[len(fp):]
        h = FORM.match(have)
        if not h or any((w.group(i) or '') != (h.group(i) or '') for i in range(1, 7)):
            continue
        win, hin = (w.group(8) or '').split('_'), (h.group(8) or '').split('_')
        if len(win) != len(hin):
            continue
        cost = sum(0 if a == b else 100 + abs(len(b) - len(a)) for a, b in zip(win, hin))
        wf, hf = w.group(7) or '', h.group(7) or ''
        cost += sum(3 for c in wf if c not in hf) + sum(10 for c in hf if c not in wf)
        if best_cost is None or cost < best_cost or (cost == best_cost and cand < best):
            best, best_cost = cand, cost
    return best, best_cost


def pick(draw, mode, lib, have_metallib=True):
    """What the plug-in (bench/plugin.py) compiles for pipeline DRAW in MODE: (vertex, fragment) descriptions."""
    _labels, variants, pipelines, frag_vert, _multi = lib
    if mode != 'nearest' and not have_metallib:
        mode = 'nearest'
    if mode == 'own':
        return 'own', 'own'
    if draw in pipelines:
        v, fr = pipelines[draw]
        return 'exact-pipeline ' + v[len(PREFIX):], ('exact-pipeline ' + fr[len(PREFIX):]) if fr else 'none'
    if mode == 'exact':
        return 'own', 'own'
    spec = draw[len(PREFIX + 'draw.'):]
    if '_' not in spec:
        return 'own (no variant)', 'own (no variant)'
    frag = PREFIX + 'frag.' + spec[spec.index('_') + 1:]
    asked = frag
    cost = 0
    if frag not in variants:
        frag, cost = nearest(frag, variants, frag_vert)
    if frag is None:
        return 'own (no variant)', 'own (no variant)'
    vert = frag_vert.get(frag)
    how = 'nearest' if frag != asked else 'frag-label'
    return (('vert-of-' + ('nearest' if frag != asked else 'frag') + ' ' + vert[len(PREFIX):]) if vert in variants else 'own (no variant)',
            '%s %s%s' % (how, frag[len(PREFIX):], ' (cost %d)' % cost if frag != asked else ''))


def summary(qc):
    print('QuartzCore: %s' % qc)
    ml = os.path.join(qc, 'default.metallib')
    have_metallib = os.path.isfile(ml)
    if have_metallib:
        fns = mtlb_functions(thin(open(ml, 'rb').read(), ml))
        fixed = sorted(n for n, _t in fns if n.startswith('fixed_'))
        print('default.metallib: %d functions; uber shaders: %s' % (len(fns), ', '.join(fixed) or 'none'))
    else:
        print('default.metallib: absent (the plug-in falls back to the nearest pick)')
    if not os.path.isdir(os.path.join(qc, 'default.pipelinelib')):
        print('default.pipelinelib: absent (every fixed_* pipeline is compiled from the function: own)')
        return None, have_metallib
    lib = read_pipelinelib(qc)
    labels, variants, pipelines, frag_vert, multi = lib
    frags = [l for l in labels if l.startswith(PREFIX + 'frag.')]
    odd = [l for l in frags if not FORM.match(l[len(PREFIX + 'frag.'):])]
    names = {}
    for n in variants.values():
        names[n] = names.get(n, 0) + 1
    print('default.pipelinelib: %d labels, %d variants (%s), %d pipelines, %d fragment labels with one vertex label each'
          % (len(labels), len(variants), ', '.join('%s %d' % kv for kv in sorted(names.items())), len(pipelines), len(frag_vert)))
    print('  fragment labels not of hm_ca_nearest\'s form: %d%s' % (len(odd), (' (' + ', '.join(odd[:8]) + ')') if odd else ''))
    print('  fragment labels with more than one vertex label (the plug-in keeps the last): %d%s'
          % (len(multi), (' (' + ', '.join(sorted(multi)[:6]) + ')') if multi else ''))
    return lib, have_metallib


def inside_repo(path):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..', '..'))
    return os.path.realpath(os.path.abspath(path)).startswith(os.path.realpath(root) + os.sep)


def selftest():
    """A synthetic library in the layout above (no Apple bytes): the readers and the pick agree with the plug-in's."""
    def mtlb(name):
        tags = b'NAME' + struct.pack('<H', len(name) + 1) + name.encode() + b'\0' + b'ENDT'
        entry = struct.pack('<I', 4 + len(tags)) + tags
        funcs = 0x58
        body = struct.pack('<I', 1) + entry
        total = funcs + len(body)
        head = b'MTLB' + b'\0' * 12 + struct.pack('<QQQ', total, funcs, len(body))
        head += b'\0' * (funcs - len(head))
        return head + body

    frag = ['TimgA2Xhf_Isrc', 'TimgA2Xhfcx_Isrc', 'TcmaBpldA2LlnXhfcx', 'TimgBdsoA2Xhfcn_Isrc']
    vert = ['VfxXh', 'VfxU100Xh', 'VfxLlnXh']
    labels = [PREFIX + 'frag.' + x for x in frag] + [PREFIX + 'vert.' + x for x in vert]
    pipes = [('PBGRAXm_TimgA2Xhf_Isrc', 'VfxXh', 'TimgA2Xhf_Isrc'), ('PBGRAXm_TimgA2Xhfcx_Isrc', 'VfxU100Xh', 'TimgA2Xhfcx_Isrc'),
             ('PBGRAXm_TcmaBpldA2LlnXhfcx', 'VfxLlnXh', 'TcmaBpldA2LlnXhfcx'), ('PBGRABdsoXm_TimgBdsoA2Xhfcn_Isrc', 'VfxXh', 'TimgBdsoA2Xhfcn_Isrc')]
    s1 = struct.pack('<I', len(labels))
    strs = b''
    for l in labels:
        s1 += struct.pack('<I', 4 + 4 * len(labels) + len(strs))
        strs += b'\0' * 16 + l.encode() + b'\0'
    s1 += strs
    s7 = struct.pack('<I', len(pipes))
    ents = b''
    for d, v, fr in pipes:
        s7 += struct.pack('<I', 4 + 4 * len(pipes) + len(ents))
        ents += b'\0' * 8 + b'\0'.join((PREFIX + 'draw.' + d).encode() for _ in [0]) + b'\0' + (PREFIX + 'vert.' + v).encode() + b'\0' + \
            (PREFIX + 'frag.' + fr).encode() + b'\0' + b'\0' * 32
    s7 += ents
    nsec = 9
    hdr = 4 + 4 * nsec
    secs = [b'', s1, b'', b'', b'', b'', b'', s7, b'']
    offs, at = [], hdr
    for s in secs:
        offs.append(at)
        at += len(s)
    desc = struct.pack('<I', nsec) + struct.pack('<%dI' % nsec, *offs) + b''.join(secs)
    with tempfile.TemporaryDirectory() as t:
        qc = os.path.join(t, 'System/Library/Frameworks/QuartzCore.framework')
        os.makedirs(os.path.join(qc, 'default.pipelinelib'))
        open(os.path.join(qc, 'default.pipelinelib', 'pipelines.desc'), 'wb').write(desc)
        open(os.path.join(qc, 'default.pipelinelib', 'functions'), 'wb').write(
            b''.join(mtlb('fixed_frag_lph_cph' if l.startswith(PREFIX + 'frag.') else 'fixed_vert_lph_spc') for l in labels))
        lib = read_pipelinelib(find_qc(t))
        assert lib[0] == labels, lib[0]
        assert len(lib[1]) == 7 and lib[1][labels[0]] == 'fixed_frag_lph_cph', lib[1]
        assert len(lib[2]) == 4 and lib[2][PREFIX + 'draw.PBGRAXm_TimgA2Xhf_Isrc'] == (PREFIX + 'vert.VfxXh', PREFIX + 'frag.TimgA2Xhf_Isrc')
        # the bbd-hm-2.log cases: a flag left out stands in for one asked for, the extra flags added cost more
        assert nearest(PREFIX + 'frag.TcmaBpldA2LlnXhf', lib[1], lib[3])[0] == PREFIX + 'frag.TcmaBpldA2LlnXhfcx'
        assert nearest(PREFIX + 'frag.TimgBdsoA2Xhfn_Isrc', lib[1], lib[3])[0] == PREFIX + 'frag.TimgBdsoA2Xhfcn_Isrc'
        assert nearest(PREFIX + 'frag.TvdkA2Xhf_Isrc', lib[1], lib[3])[0] is None
        d = PREFIX + 'draw.PBGRABsovXm_TimgA2Xhf_Isrc'
        assert pick(d, 'nearest', lib) == ('vert-of-frag vert.VfxXh', 'frag-label frag.TimgA2Xhf_Isrc'), pick(d, 'nearest', lib)
        assert pick(d, 'exact', lib) == ('own', 'own')
        assert pick(PREFIX + 'draw.PBGRAXm_TimgA2Xhf_Isrc', 'exact', lib)[0] == 'exact-pipeline vert.VfxXh'
        assert pick(d, 'exact', lib, have_metallib=False)[1] == 'frag-label frag.TimgA2Xhf_Isrc'
        assert inside_repo(__file__) and not inside_repo(t)
    print('ca-variants.py selftest: ok')


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'):
        sys.exit(__doc__)
    if a[0] == '--selftest':
        return selftest()
    qc = find_qc(a[0])
    lib, have_metallib = summary(qc)
    opts = dict(zip(a[1::2], a[2::2]))
    if '--table' in opts:
        out = opts['--table']
        if inside_repo(out):
            sys.exit('%s: inside the repository; Apple-derived tables stay out of it' % out)
        with open(out, 'w') as f:
            for draw, (v, fr) in sorted(lib[2].items()):
                f.write('pipeline\t%s\t%s\t%s\n' % (draw, v, fr or ''))
            for label in lib[0]:
                f.write('variant\t%s\t%s\t\n' % (label, lib[1].get(label, '')))
        print('table: %s' % out)
    if '--compare' in opts:
        other, _m = summary(find_qc(opts['--compare']))
        for what, i in (('labels', 0), ('pipelines', 2)):
            x, y = set(lib[i]), set(other[i])
            print('%s only in %s: %d; only in %s: %d' % (what, a[0], len(x - y), opts['--compare'], len(y - x)))
            for l in sorted(x - y)[:10]:
                print('  < %s' % l)
            for l in sorted(y - x)[:10]:
                print('  > %s' % l)
    if '--replay' in opts:
        draws = []
        for line in open(opts['--replay'], errors='replace'):
            for m in re.finditer(r'(com\.apple\.coreanimation\.draw\.[A-Za-z0-9_]+)', line):
                if m.group(1) not in draws:
                    draws.append(m.group(1))
        print('replay: %d draw labels in %s' % (len(draws), opts['--replay']))
        for d in draws:
            print(d[len(PREFIX):])
            for mode in ('exact', 'own', 'nearest'):
                v, fr = pick(d, mode, lib, have_metallib)
                print('  %-8s vs %-40s fs %s' % (mode, v, fr))


if __name__ == '__main__':
    main()
