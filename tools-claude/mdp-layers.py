#!/usr/bin/env python3
"""mdp-layers.py: the frame the display engine (MDP/DPU at 0x0ae00000) composes, from its own registers (read-only).

  mdp-layers.py regs-cmd                 the phone command that dumps the registers this script reads (only blocks
                                         `p mdp dump` reads too: CTL0/1, VIG0/1, DMA0-2, LM0/1; `pmem r`, read-only)
  mdp-layers.py plan REGS                one line per staged layer, then "read ADDR LEN PIPE STRIDE" per surface to fetch
  mdp-layers.py compose REGS DIR OUT     DIR/surf-0xADDR.bin (the reads) -> OUT: the composed panel frame, raw BGRA
                                         (W x H, 4*W-byte rows; prints "frame WxH:STRIDE" then the layer lines)

How the panel is fed (redfin, ABL's topology; the kernel never rewrites CTL_LAYER): CTL0 stages VIG0 on LM0 and VIG1 on
LM1 (source split: each pipe fetches one half of the same surface, OUT_XY in full-panel coordinates), DMA0-2 unused.
Kernel multi-layer presents (xnu 1262/1263, kern.sweet_present_layers) are composited by the GPU's 2D engine into a
panel buffer, which the VIG pipes then scan: a shot of what VIG0/VIG1 fetch is the composed frame. This script still
stages every pipe the CTL names (any stage order, the mixer's blend op per stage), so a frame the mixer blends from
several pipes comes out composed too.

Register layout (msm DPU, sm8250-class; offsets the kernel uses in 0948/1111): SSPP +0x00 SRC_SIZE, +0x04 SRC_IMG,
+0x08 SRC_XY, +0x0c OUT_SIZE, +0x10 OUT_XY, +0x14 SRC0, +0x1c SRC2 (UBWC meta), +0x24 YSTRIDE0, +0x30 FORMAT (bits
31:30 fetch mode: 2 = UBWC; 10:9 bytes per pixel - 1; 8 alpha enable), +0x34 UNPACK (byte i = element: 0 G, 1 B, 2 R,
3 A), +0x38 OP_MODE (13 flip LR, 14 flip UD). CTL +0x00/+0x04 LAYER(LM0/LM1), +0x40/+0x44 LAYER_EXT, +0x70/+0x74
LAYER_EXT2 (stage + 1 per pipe). LM +0x04 OUT_SIZE; per blend stage s >= 1: +0x20 + 0x18 * (s - 1) BLEND_OP,
+0x24 + .. CONST_ALPHA (fg 23:16, bg 7:0).
A UBWC-compressed layer (FORMAT fetch mode 2) is not rows of pixels: it is drawn hatched and reported, not decoded.
"""
import os, re, sys

MDP = 0x0ae00000
CTL = [0x2000, 0x2200]
PIPES = [('vig0', 0x5000), ('vig1', 0x7000), ('dma0', 0x25000), ('dma1', 0x27000), ('dma2', 0x29000)]
LMS = [0x45000, 0x46000]
# pipe -> (CTL_LAYER bit, CTL_LAYER_EXT bit) for 3+1-bit stages; dma2: 4 bits in LAYER_EXT2
STAGE_BITS = {'vig0': (0, 0), 'vig1': (3, 2), 'dma0': (18, 16), 'dma1': (21, 18)}
RAM = (0x80000000, 0x100000000)	# surfaces the tools already read (panel buffers 0xa0..., pool slots 0xc...)
MAX_LEN = 16 << 20
ELEM = {0: 'G', 1: 'B', 2: 'R', 3: 'A'}


def regs_cmd():
    blocks = [(c, 0x80 // 4 + 2) for c in CTL] + [(b, 0x50 // 4) for _, b in PIPES] + [(l, 0xc0 // 4) for l in LMS]
    return '; '.join('/var/local/bin/pmem r %#010x %d' % (MDP + b, n) for b, n in blocks)


def load(path):
    r = {}
    for line in open(path, errors='replace'):
        m = re.match(r'\s*([0-9a-fA-F]{8}):\s*([0-9a-fA-F]{8})\s*$', line)
        if m:
            r[int(m[1], 16) - MDP] = int(m[2], 16)
    return r


def need(r, off):
    if off not in r:
        raise KeyError('register %#x not in the dump' % (MDP + off))
    return r[off]


def wh(v):
    return v & 0xffff, v >> 16


def layers(r):
    """the staged pipes, lowest stage first: dicts with the fetch and the placement"""
    out = []
    for ci, c in enumerate(CTL):
        for lm in range(2):
            lay, ext = r.get(c + 4 * lm, 0), r.get(c + 0x40 + 4 * lm, 0)
            ext2 = r.get(c + 0x70 + 4 * lm, 0)
            for name, base in PIPES:
                if name in STAGE_BITS:
                    lb, eb = STAGE_BITS[name]
                    v = (lay >> lb & 7) | (ext >> eb & 1) << 3
                elif name == 'dma2':
                    v = ext2 & 0xf
                else:
                    v = 0
                if not v:
                    continue
                if any(o['pipe'] == name for o in out):	# one pipe staged on both mixers: once
                    continue
                L = {'pipe': name, 'ctl': ci, 'lm': lm, 'stage': v - 1, 'base': base}
                sw, sh = wh(need(r, base + 0x00))
                sx, sy = wh(need(r, base + 0x08))
                dw, dh = wh(need(r, base + 0x0c))
                dx, dy = wh(need(r, base + 0x10))
                fmt, unp, op = need(r, base + 0x30), need(r, base + 0x34), need(r, base + 0x38)
                L.update(src=(sx, sy, sw, sh), dst=(dx, dy, dw, dh), addr=need(r, base + 0x14),
                         stride=need(r, base + 0x24) & 0xffff, fmt=fmt, unpack=unp, op=op,
                         fetch=fmt >> 30, cpp=(fmt >> 9 & 3) + 1, alpha=bool(fmt >> 8 & 1),
                         flip=('h' if op >> 13 & 1 else '') + ('v' if op >> 14 & 1 else ''))
                order = ''.join(ELEM[unp >> (8 * i) & 3] for i in range(4))
                if not L['alpha']:
                    order = order.replace('A', 'X')
                L['order'] = order
                s = L['stage']
                lmb = LMS[lm]
                if s >= 1:
                    L['blend'] = (r.get(lmb + 0x20 + 0x18 * (s - 1), 0), r.get(lmb + 0x24 + 0x18 * (s - 1), 0))
                else:
                    L['blend'] = None	# base stage: replaces
                L['rows'] = sy + sh
                L['len'] = L['stride'] * L['rows']
                why = ''
                if L['fetch'] == 2:
                    why = 'UBWC-compressed: not readable raw (the kernel has no decompress/blit verb for it)'
                elif L['fetch'] != 0:
                    why = 'tiled fetch mode %d: not readable raw' % L['fetch']
                elif L['cpp'] != 4:
                    why = '%d-byte pixels: format %#x not decoded' % (L['cpp'], fmt)
                elif not (RAM[0] <= L['addr'] and L['addr'] + L['len'] <= RAM[1]) or L['len'] > MAX_LEN or not sw or not dw:
                    why = 'address %#x + %d outside the RAM window read before: not read' % (L['addr'], L['len'])
                elif (sx + sw) * 4 > L['stride']:
                    why = 'source rect wider than the stride'
                L['skip'] = why
                out.append(L)
    out.sort(key=lambda o: (o['stage'], o['pipe']))
    return out


def panel(r, ls):
    w = h = 0
    for lm in sorted({o['lm'] for o in ls} | {0}):
        lw, lh = wh(r.get(LMS[lm] + 0x04, 0))
        w, h = w + lw, max(h, lh)
    if not w or not h:
        w, h = 1080, 2340
    return w, h


def blend_desc(b):
    if b is None:
        return 'base'
    op, ca = b
    fa = {0: 'fgK', 1: 'bgK', 2: 'fgA', 3: 'bgA'}[op & 3] + ('~' if op & 4 else '')
    ba = {0: 'fgK', 1: 'bgK', 2: 'fgA', 3: 'bgA'}[op >> 8 & 3] + ('~' if op & 0x400 else '')
    k = 'opaque' if op == 0x400 and (ca >> 16 & 0xff) == 0xff else \
        'premultiplied over' if (op & 0x3bfb) == 0x200 and op & 0x400 and (ca >> 16 & 0xff) == 0xff else \
        'coverage over' if (op & 0x3bfb) == 0x202 and op & 0x400 else 'op %#x' % op
    return '%s (fg %s bg %s, const fg %02x bg %02x)' % (k, fa, ba, ca >> 16 & 0xff, ca & 0xff)


def line(o):
    s, d = o['src'], o['dst']
    t = '%s lm%d stage %d: %#x stride %d fmt %#x %s %s src %d,%d %dx%d -> dst %d,%d %dx%d%s | %s' % (
        o['pipe'], o['lm'], o['stage'], o['addr'], o['stride'], o['fmt'], o['order'],
        {0: 'linear', 1: 'tiled', 2: 'UBWC'}.get(o['fetch'], 'fetch%d' % o['fetch']),
        s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], (' flip ' + o['flip']) if o['flip'] else '', blend_desc(o['blend']))
    if o['skip']:
        t += ' | NOT READ: ' + o['skip']
    return t


def plan(r):
    ls = layers(r)
    if not ls:
        print('layers none (no pipe staged in CTL0/1)')
        return
    for o in ls:
        print('layer ' + line(o))
    reads = {}
    for o in ls:
        if not o['skip']:
            a = o['addr']
            if a not in reads or reads[a][0] < o['len']:
                reads[a] = (o['len'], o['pipe'], o['stride'])
    for a, (n, p, st) in sorted(reads.items(), key=lambda kv: kv[1][1] != 'vig0'):
        print('read %#x %d %s %d' % (a, n, p, st))


def hatch(size, label):
    from PIL import Image, ImageDraw
    im = Image.new('RGBA', size, (40, 40, 40, 255))
    d = ImageDraw.Draw(im)
    for k in range(-size[1], size[0], 48):
        d.line([(k, 0), (k + size[1], size[1])], fill=(255, 0, 255, 255), width=6)
    d.rectangle([0, 0, size[0] - 1, size[1] - 1], outline=(255, 0, 255, 255), width=8)
    for y in range(40, size[1], 300):
        d.text((24, y), label, fill=(255, 255, 255, 255))
    return im


def surface(o, data):
    from PIL import Image
    rows = len(data) // o['stride']
    im = Image.frombuffer('RGBA', (o['stride'] // 4, rows), data[:rows * o['stride']], 'raw', 'RGBA', o['stride'], 1)
    b = dict(zip(o['order'], im.split()))
    a = b.get('A') or Image.new('L', im.size, 255)
    im = Image.merge('RGBA', (b['R'], b['G'], b['B'], a))
    sx, sy, sw, sh = o['src']
    im = im.crop((sx, sy, sx + sw, sy + sh))
    if 'h' in o['flip']:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if 'v' in o['flip']:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    dw, dh = o['dst'][2:]
    if (dw, dh) != (sw, sh):
        im = im.resize((dw, dh), Image.NEAREST)
    return im


def blend_onto(canvas, fg, o):
    """the mixer's stage: out = fg * wf + bg * wb (colour as stored: CA's surfaces are premultiplied, the mixer
    does not multiply the colour by alpha itself; the op says which alpha weighs what)"""
    from PIL import Image, ImageChops
    x, y = o['dst'][:2]
    box = (x, y, x + fg.width, y + fg.height)
    if o['blend'] is None:
        canvas.paste(fg.convert('RGB'), box)
        return
    op, ca = o['blend']
    bg = canvas.crop(box)
    fga, bga = fg.split()[3], Image.new('L', fg.size, 255)	# the mixer's bg is opaque (border colour below)

    def sel(s, inv):
        a = {0: Image.new('L', fg.size, ca >> 16 & 0xff), 1: Image.new('L', fg.size, ca & 0xff), 2: fga, 3: bga}[s]
        return ImageChops.invert(a) if inv else a
    wf, wb = sel(op & 3, op & 4), sel(op >> 8 & 3, op & 0x400)
    m = lambda im, w: ImageChops.multiply(im, Image.merge('RGB', (w, w, w)))
    canvas.paste(ImageChops.add(m(fg.convert('RGB'), wf), m(bg, wb)), box)


def compose(r, d, outp):
    from PIL import Image
    ls = layers(r)
    w, h = panel(r, ls)
    canvas = Image.new('RGB', (w, h), (0, 0, 0))
    for o in ls:
        f = os.path.join(d, 'surf-%#x.bin' % o['addr'])
        if not o['skip'] and not (os.path.exists(f) and os.path.getsize(f) >= o['len']):
            o['skip'] = 'surface read failed'
        if o['skip']:
            fg = hatch(o['dst'][2:], '%s %s' % (o['pipe'], o['skip'][:60]))
            o2 = dict(o, blend=None)
            blend_onto(canvas, fg, o2)
        else:
            blend_onto(canvas, surface(o, open(f, 'rb').read()), o)
    b, g, rr = canvas.split()[2], canvas.split()[1], canvas.split()[0]
    open(outp, 'wb').write(Image.merge('RGBA', (b, g, rr, Image.new('L', (w, h), 255))).tobytes())
    print('frame %dx%d:%d' % (w, h, 4 * w))
    for o in ls:
        print('layer ' + line(o))


def main():
    a = sys.argv[1:]
    if a[:1] == ['regs-cmd']:
        print(regs_cmd())
    elif a[:1] == ['plan'] and len(a) == 2:
        plan(load(a[1]))
    elif a[:1] == ['compose'] and len(a) == 4:
        compose(load(a[1]), a[2], a[3])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
