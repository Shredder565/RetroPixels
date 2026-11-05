#!/usr/bin/env python3
"""Packs the self-test's two programs into an .nds file (a DS card image with
just the header fields direct boot reads). Usage: mknds.py arm9.bin arm7.bin out.nds"""
import struct, sys
a9 = open(sys.argv[1], 'rb').read(); a7 = open(sys.argv[2], 'rb').read()
pad = lambda b: b + b'\0' * (-len(b) % 0x200)
a9, a7 = pad(a9), pad(a7)
h = bytearray(0x200)
h[0:12] = b'PIXELDSTEST\0'; h[12:16] = b'PXDT'; h[16:18] = b'01'
o9 = 0x4000; o7 = o9 + len(a9)
struct.pack_into('<IIII', h, 0x20, o9, 0x02000000, 0x02000000, len(a9))
struct.pack_into('<IIII', h, 0x30, o7, 0x03800000, 0x03800000, len(a7))
struct.pack_into('<I', h, 0x80, o7 + len(a7))
rom = bytes(h) + b'\0' * (o9 - 0x200) + a9 + a7
rom += b'\xff' * (-len(rom) % 0x20000)
open(sys.argv[3], 'wb').write(rom)
