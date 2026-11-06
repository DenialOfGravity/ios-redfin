#!/usr/bin/env python3
"""mkmetallib.py OUT.metallib NAME:TYPE:FILE.bc ... : a .metallib (MTLB container) of our own AIR bitcode, no Apple tool.

TYPE: vertex | fragment | kernel. FILE.bc: llvm-as output of a hand-written AIR .ll (hakios-metal tools/kernels style).
The layout is the one metal2vulkan's container parser and its corpus writer use (crates/metal2vulkan-air/src/container/
mtlb.rs, corpus-harvest.rs metallib()) and hakios-metal's MetalLibrary reads (src/metallib.cpp):
  0x00 'MTLB'; u16 platform 0x8001; u16 u16 container version; u8 file type (0 executable); u8 os; u16 u16 os version;
  0x10 u64 file size; then (offset, size) u64 pairs: function list entries, public metadata, private metadata, bitcode.
  0x58 u32 function count, entries (u32 size counting itself, tags NAME TYPE HASH MDSZ OFFT VERS, ENDT), the header
  extension (ENDT alone), per function an empty public and private metadata record (u32 4 + ENDT), the bitcode, each
  module in LLVM's 0x0B17C0DE wrapper (offset 0x14, cputype -1) as Apple's toolchain stores AIR.
Ours, MIT. Env MTLB_OS=0x82 (iOS), MTLB_OS_VERSION=16.0, MTLB_VERSION=2.6 change the header's values.
"""
import hashlib
import os
import struct
import sys

TYPES = {'vertex': 0, 'fragment': 1, 'kernel': 2}


def tag(fourcc, data):
    return fourcc + struct.pack('<H', len(data)) + data


def wrap(stream):
    # llvm-as already wraps a module whose triple is Apple's (air64-apple-*): offset 0x14, cputype -1, as here
    if stream[:4] == b'\xde\xc0\x17\x0b':
        assert struct.unpack_from('<I', stream, 8)[0] == 0x14 and stream[0x14:0x18] == b'BC\xc0\xde', 'odd wrapper'
        return stream
    assert stream[:4] == b'BC\xc0\xde', 'not LLVM bitcode'
    return struct.pack('<IIIII', 0x0B17C0DE, 0, 0x14, len(stream), 0xFFFFFFFF) + stream


def air_version(stream):
    # !air.version / !air.language_version are in the module; the VERS tag repeats them (AIR 2.5, Metal 3.0 for ours)
    return bytes([2, 0, 5, 0, 3, 0, 0, 0])


def build(functions):
    def ver(env, default):
        a, b = os.environ.get(env, default).split('.')
        return int(a), int(b)
    os_byte = int(os.environ.get('MTLB_OS', '0x82'), 0)
    osv = ver('MTLB_OS_VERSION', '16.0')
    cv = ver('MTLB_VERSION', '2.6')
    empty = struct.pack('<I', 4) + b'ENDT'
    public = private = bitcode = b''
    entries = []
    for name, kind, stream in functions:
        w = wrap(stream)
        offt = struct.pack('<QQQ', len(public), len(private), len(bitcode))
        public += empty
        private += empty
        bitcode += w
        body = (tag(b'NAME', name.encode() + b'\0') + tag(b'TYPE', bytes([kind])) +
                tag(b'HASH', hashlib.sha256(w).digest()) + tag(b'MDSZ', struct.pack('<Q', len(w))) +
                tag(b'OFFT', offt) + tag(b'VERS', air_version(stream)) + b'ENDT')
        entries.append(struct.pack('<I', 4 + len(body)) + body)
    elen = sum(map(len, entries))
    list_off = 0x58
    pub_off = list_off + 4 + elen + 4
    prv_off = pub_off + len(public)
    bc_off = prv_off + len(private)
    total = bc_off + len(bitcode)
    head = b'MTLB' + struct.pack('<HHHBBHH', 0x8001, cv[0], cv[1], 0, os_byte, osv[0], osv[1])
    head += struct.pack('<9Q', total, list_off, elen, pub_off, len(public), prv_off, len(private), bc_off, len(bitcode))
    out = head + struct.pack('<I', len(functions)) + b''.join(entries) + b'ENDT' + public + private + bitcode
    assert len(head) == 0x58 and len(out) == total
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    funcs = []
    for spec in sys.argv[2:]:
        name, kind, path = spec.split(':', 2)
        funcs.append((name, TYPES[kind], open(path, 'rb').read()))
    data = build(funcs)
    open(sys.argv[1], 'wb').write(data)
    print('%s: %d functions, %d bytes' % (sys.argv[1], len(funcs), len(data)))


if __name__ == '__main__':
    main()
