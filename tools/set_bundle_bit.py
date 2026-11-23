#!/usr/bin/env python3
"""Sets the Finder's "hasBundle" flag in MacBinary files, so the Finder uses the
file's BNDL (its icons). Retro68's Rez doesn't set it. Updates the MacBinary II
header CRC to match.

Usage: set_bundle_bit.py file.bin [...]
"""
import sys

HAS_BUNDLE = 0x2000
INITED = 0x0100


def crc16_xmodem(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


for path in sys.argv[1:]:
    with open(path, 'r+b') as f:
        header = bytearray(f.read(128))
        if len(header) < 128 or header[0] != 0 or not 1 <= header[1] <= 63:
            sys.exit(f'{path}: not a MacBinary file')
        flags = (header[73] << 8) | header[101]
        flags = (flags | HAS_BUNDLE) & ~INITED        # use the BNDL; let the Finder place the icon
        header[73], header[101] = flags >> 8, flags & 0xFF
        crc = crc16_xmodem(header[:124])
        header[124], header[125] = crc >> 8, crc & 0xFF
        f.seek(0)
        f.write(header)
    print(f'{path}: Finder flags now 0x{flags:04X} (hasBundle)')
