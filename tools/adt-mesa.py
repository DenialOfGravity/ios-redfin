#!/usr/bin/env python3
"""Add (or remove) IODeviceTree:/arm-io/mesa in an existing flat ADT (SweetMesa / Touch ID persona, opt-in).

    adt-mesa.py IN OUT          add /arm-io/mesa (mkadt.mesa_node; idempotent: an existing node is kept)
    adt-mesa.py IN OUT --remove take it out again
    adt-mesa.py IN --check      print whether IN has the node (exit 0 yes, 1 no)

Used by tools-claude/mkimage.py when MKIMAGE_ADT_MESA=1 (the base image's ADT gets the node), so a test image
needs no new ADT build. Phandle = highest AAPL,phandle in the tree + 1. Nothing else in the tree changes.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402
import mkadt  # noqa: E402


def find(node, name):
    for c in node.children:
        if c.name == name:
            return c
    return None


def max_phandle(node):
    m = 0
    v = node.prop('AAPL,phandle')
    if v is not None and len(v) >= 4:
        m = struct.unpack_from('<I', v)[0]
    for c in node.children:
        m = max(m, max_phandle(c))
    return m


def edit(buf, remove=False):
    """Return (new_buf, what) for ADT bytes buf."""
    root, end = adt.parse(buf)
    arm_io = find(root, 'arm-io')
    if arm_io is None:
        raise SystemExit('no /arm-io in the ADT')
    have = find(arm_io, 'mesa')
    if remove:
        if have is None:
            return buf, 'no /arm-io/mesa (unchanged)'
        arm_io.children = [c for c in arm_io.children if c is not have]
        return adt.serialize(root) + buf[end:], 'removed /arm-io/mesa'
    if have is not None:
        return buf, '/arm-io/mesa already there (unchanged)'
    ph = max_phandle(root) + 1
    arm_io.children.append(mkadt.mesa_node(lambda: ('AAPL,phandle', mkadt.u32(ph))))
    return adt.serialize(root) + buf[end:], f'added /arm-io/mesa (phandle {ph:#x}, mesaType 1)'


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[1] == '--check':
        root, _ = adt.parse(open(a[0], 'rb').read())
        io = find(root, 'arm-io')
        yes = io is not None and find(io, 'mesa') is not None
        print('yes' if yes else 'no')
        sys.exit(0 if yes else 1)
    if len(a) not in (2, 3) or (len(a) == 3 and a[2] != '--remove'):
        print(__doc__)
        sys.exit(2)
    buf, what = edit(open(a[0], 'rb').read(), remove=len(a) == 3)
    open(a[1], 'wb').write(buf)
    print(f'{a[1]}: {len(buf)} bytes, {what}', file=sys.stderr)


if __name__ == '__main__':
    main()
