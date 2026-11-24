#!/usr/bin/env python3
"""Writes the data and resource forks of a MacBinary file to separate files,
so they can be embedded in the installer with Rez's $$read.

Usage: extract_fork.py file.bin out_prefix   ->  out_prefix.data, out_prefix.rsrc
"""
import struct
import sys

src, prefix = sys.argv[1], sys.argv[2]
b = open(src, 'rb').read()
if b[0] != 0 or not 1 <= b[1] <= 63:
    sys.exit(f'{src}: not a MacBinary file')
dlen, rlen = struct.unpack('>II', b[83:91])
data = b[128:128 + dlen]
r0 = 128 + ((dlen + 127) // 128) * 128
rsrc = b[r0:r0 + rlen]
open(prefix + '.data', 'wb').write(data)
open(prefix + '.rsrc', 'wb').write(rsrc)
print(f'{src}: data {len(data)} bytes, resource fork {len(rsrc)} bytes')
