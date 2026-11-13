#!/usr/bin/env python3
"""Packs the test program as a decrypted 3DS game program (NCCH, .cxi)
for the PixelDS tests: an extended header, and an ExeFS whose .code is
packed with the backwards LZ77 scheme games use. Our own tool, written
from the file format's description.

usage: mkcxi.py code.bin bss_size out.cxi
"""
import struct
import sys

PAGE = 0x1000


def blz_compress(data):
    """Packs data with backwards LZ77 (copies of 3-18 bytes from up to
    0x1001 bytes further on). Returns the packed bytes with the footer."""
    n = len(data)
    out = n           # data[out:] is already produced
    stream = []       # bytes in the order the unpacker reads them
    while out > 0:
        flags_at = len(stream)
        stream.append(0)
        flags = 0
        for bit in range(8):
            if out == 0:
                break
            best_len, best_disp = 0, 0
            for disp in range(2, 0x1002):
                # the unpacker writes byte p from byte p+1+disp
                if out + disp >= n:
                    break
                ln = 0
                while ln < 18 and out - 1 - ln >= 0 and data[out - 1 - ln] == data[out - ln + disp]:
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, disp
                    if ln == 18:
                        break
            if best_len >= 3:
                v = (best_len - 3) << 12 | (best_disp - 2)
                stream += [v >> 8, v & 0xff]
                flags |= 0x80 >> bit
                out -= best_len
            else:
                out -= 1
                stream.append(data[out])
        stream[flags_at] = flags
    packed = bytes(reversed(stream))
    pad = (-len(packed)) % 4
    body = packed + b"\xff" * pad
    footer_len = 8 + pad
    total = len(body) + 8
    footer = struct.pack("<II", footer_len << 24 | total, n - total)
    return body + footer


def main():
    code = open(sys.argv[1], "rb").read()
    bss = int(sys.argv[2])
    out = sys.argv[3]
    text_pages = (len(code) + PAGE - 1) // PAGE
    code = code + b"\0" * (text_pages * PAGE - len(code))
    packed = blz_compress(code)

    exh = bytearray(0x800)
    exh[0:8] = b"ctrtest\0"
    exh[0x0d] = 1  # .code is packed
    struct.pack_into("<III", exh, 0x10, 0x00100000, text_pages, len(code))
    struct.pack_into("<I", exh, 0x1c, 0x4000)  # stack
    data_addr = 0x00100000 + text_pages * PAGE
    struct.pack_into("<III", exh, 0x20, data_addr, 0, 0)
    struct.pack_into("<III", exh, 0x30, data_addr, 0, 0)
    struct.pack_into("<I", exh, 0x3c, bss + PAGE)
    exh[0x20f] = 0x30  # main thread priority

    exefs = bytearray(0x200)
    exefs[0:8] = b".code\0\0\0"
    struct.pack_into("<II", exefs, 8, 0, len(packed))
    exefs += packed
    exefs += b"\0" * ((-len(exefs)) % 0x200)

    hdr = bytearray(0x200)
    hdr[0x100:0x104] = b"NCCH"
    exefs_off = 0x200 + len(exh)
    total = exefs_off + len(exefs)
    struct.pack_into("<I", hdr, 0x104, total // 0x200)
    struct.pack_into("<Q", hdr, 0x118, 0x000400000FF3D500)
    hdr[0x150:0x160] = b"CTR-P-PXDS".ljust(16, b"\0")
    struct.pack_into("<I", hdr, 0x180, 0x400)
    hdr[0x188 + 4] = 1     # platform: CTR
    hdr[0x188 + 5] = 3     # data and executable
    hdr[0x188 + 7] = 4     # not encrypted
    struct.pack_into("<II", hdr, 0x1a0, exefs_off // 0x200, len(exefs) // 0x200)
    open(out, "wb").write(bytes(hdr) + bytes(exh) + bytes(exefs))


if __name__ == "__main__":
    main()
