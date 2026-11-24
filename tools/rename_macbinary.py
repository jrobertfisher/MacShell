#!/usr/bin/env python3
"""Sets the Mac file name stored in a MacBinary file's header (and its CRC).
Usage: rename_macbinary.py file.bin "New Name" """
import sys
path, name = sys.argv[1], sys.argv[2].encode('mac_roman')
if not 1 <= len(name) <= 31:
    sys.exit('the name must be 1 to 31 characters')
b = bytearray(open(path, 'rb').read())
b[1] = len(name)
b[2:65] = name.ljust(63, b'\0')
crc = 0
for x in b[:124]:
    crc ^= x << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
b[124], b[125] = crc >> 8, crc & 0xFF
open(path, 'wb').write(b)
print('%s: named "%s"' % (path, sys.argv[2]))
