#!/usr/bin/env python3
"""Makes a 3DSX from the test program's flat binary: everything in the code
segment (linked at 0x00100000, so no relocations), BSS in the data segment.
Usage: mk3dsx.py <program.bin> <bss size> <out.3dsx>"""
import struct, sys
code = open(sys.argv[1], 'rb').read()
bss = int(sys.argv[2], 0)
code += b'\0' * (-len(code) % 4)
hdr = struct.pack('<4sHHIIIIII', b'3DSX', 0x20, 8, 0, 0, len(code), 0, bss, bss)
rel = struct.pack('<II', 0, 0) * 3
open(sys.argv[3], 'wb').write(hdr + rel + code)
