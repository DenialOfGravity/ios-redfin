#!/usr/bin/env python3
"""Read the running kernel's log ring (r9 kernel 30ba401b only) through the file server's /dev/pmem."""
import struct, subprocess, sys
OUT = '/mnt/d/ClaudeProjects/iOS_Pixel5/logs/klog-r9.txt'
TOTAL_PA, RING_PA, SIZE = 0x101cee710, 0x101cee718, 4 << 20
subprocess.run(['python3', '-I', 'tools/flame-files.py', 'read', '/dev/pmem', hex(TOTAL_PA), str(8 + SIZE), '/tmp/klog.bin'], check=True)
d = open('/tmp/klog.bin', 'rb').read()
total = struct.unpack_from('<Q', d, 0)[0]
ring = d[8:8 + SIZE]
if total <= SIZE:
    text = ring[:total]
else:
    p = total % SIZE
    text = ring[p:] + ring[:p]
open(OUT, 'wb').write(text)
print('log bytes written', total, '->', OUT)
