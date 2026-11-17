#!/usr/bin/env python3
"""fatx.py IMAGE SRCDIR OUTDIR [ls]: read-only FAT12/16 extractor (stdlib only).
Copies the files in SRCDIR (e.g. image/modem_pr/so) of a FAT image to OUTDIR."""
import os, struct, sys

img, src, out = sys.argv[1], sys.argv[2].strip('/'), sys.argv[3]
lsonly = len(sys.argv) > 4
f = open(img, 'rb')
bs = f.read(512)
bps, spc, rsv, nfat, nroot = struct.unpack_from('<HBHBH', bs, 11)
tot16, = struct.unpack_from('<H', bs, 19)
spf, = struct.unpack_from('<H', bs, 22)
tot32, = struct.unpack_from('<I', bs, 32)
fat_off = rsv * bps
root_off = fat_off + nfat * spf * bps
root_len = nroot * 32
data_off = root_off + ((root_len + bps - 1) // bps) * bps
csz = spc * bps
f.seek(fat_off); fat = f.read(spf * bps)
ncl = ((tot16 or tot32) * bps - data_off) // csz + 2
fat16 = ncl >= 4085

def nxt(c):
    if fat16:
        return struct.unpack_from('<H', fat, c * 2)[0]
    v = struct.unpack_from('<H', fat, c * 3 // 2)[0]
    return (v >> 4) if c & 1 else (v & 0xfff)

def chain(c):
    end = 0xfff8 if fat16 else 0xff8
    while 2 <= c < end:
        yield c
        c = nxt(c)

def read_chain(c, size=None):
    b = bytearray()
    for cl in chain(c):
        f.seek(data_off + (cl - 2) * csz); b += f.read(csz)
        if size is not None and len(b) >= size:
            break
    return bytes(b[:size]) if size is not None else bytes(b)

def entries(raw):
    lfn = {}
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xe5: lfn = {}; continue
        if e[11] == 0x0f:
            seq = e[0] & 0x1f
            lfn[seq] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'replace')
            continue
        if e[11] & 0x08: lfn = {}; continue
        if lfn:
            name = ''.join(lfn[k] for k in sorted(lfn)).split('\x00')[0]
        else:
            b, x = e[0:8].decode('latin1').rstrip(), e[8:11].decode('latin1').rstrip()
            if e[12] & 0x08: b = b.lower()
            if e[12] & 0x10: x = x.lower()
            name = b + ('.' + x if x else '')
        lfn = {}
        cl = struct.unpack_from('<H', e, 26)[0]
        size = struct.unpack_from('<I', e, 28)[0]
        yield name, bool(e[11] & 0x10), cl, size

f.seek(root_off); raw = f.read(root_len)
for part in [p for p in src.split('/') if p]:
    for name, d, cl, size in entries(raw):
        if d and name.lower() == part.lower():
            raw = read_chain(cl); break
    else:
        sys.exit('not found: ' + part)
os.makedirs(out, exist_ok=True)
n = tot = 0
for name, d, cl, size in entries(raw):
    if d or name in ('.', '..'): continue
    print(f'{size:10d} {name}')
    if not lsonly:
        with open(os.path.join(out, name), 'wb') as o:
            o.write(read_chain(cl, size))
    n += 1; tot += size
print(f'{n} files, {tot} bytes', file=sys.stderr)
