#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Writes the WiiConnect24 files of a fresh Wii NAND into pixelu/wii/Data/Sys/Wii/shared2/wc24.

PixelU-Wii copies Sys/Wii into the emulated NAND before a Wii game starts, for every file the
NAND does not have yet (Core/WiiRoot.cpp, InstallSystemMenuFiles). On a real console the System
Menu makes these files; games are booted directly here, and some of them expect the files to be
there from the first boot. Every file is built from its documented layout (WiiBrew's
/shared2/wc24 and /dev/net/kd pages, and the structures in Core/IOS/Network/KD), in the state of
a console that has never been online: no WiiConnect24 account, no download tasks, no friends,
no mail.

  python3 pixelu/wii/Data/make_wc24.py           write the files
  python3 pixelu/wii/Data/make_wc24.py --check   compare with the files in the tree

Files (all values big endian):

  nwc24msg.cfg, nwc24msg.cbk   account settings and their backup copy (1 KiB). Creation stage 0
                               ("initial"): no Wii Number, no password, no mail check ID. These
                               are the values KD itself writes when the file is missing
                               (NWC24Config::ResetConfig); the KD device makes a Wii Number and
                               registers it when a game asks for that.
  nwc24dl.bin                  the download task list: header and 120 unused entries.
  nwc24fl.bin                  the friend list: header and 100 empty slots.
  mbox/wc24send.ctl,           the outgoing and incoming mail lists: header and empty entries
  mbox/wc24recv.ctl            (127 and 255).
  mbox/wc24send.mbx,           the virtual FAT volumes (VFF) for mail bodies, empty. Only the
  mbox/wc24recv.mbx            32-byte header and the start of the first FAT are stored; the
                               volume grows when something is written to it.

misc.bin and nwc24fls.bin are not written: only Nintendo's own KD module reads them, and
PixelU-Wii's KD device (Core/IOS/Network/KD) replaces that module and never opens them.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "Sys", "Wii", "shared2", "wc24")


def be32(*values):
    return struct.pack(">%dI" % len(values), *values)


def fixed_text(text, size):
    raw = text.encode("ascii")
    assert len(raw) <= size
    return raw + bytes(size - len(raw))


# ---------------------------------------------------------------- nwc24msg.cfg

CFG_MAGIC = b"WcCf"
CFG_VERSION = 8
SERVER_URLS = [
    "https://amw.wc24.wii.com/cgi-bin/account.cgi",
    "http://rcw.wc24.wii.com/cgi-bin/check.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/receive.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/delete.cgi",
    "http://mtw.wc24.wii.com/cgi-bin/send.cgi",
]


def account_config():
    body = bytearray()
    body += CFG_MAGIC
    body += be32(CFG_VERSION)
    body += struct.pack(">Q", 0)  # Wii Number: none yet
    body += be32(0)  # ID generation counter
    body += be32(0)  # creation stage: initial
    body += fixed_text("@wii.com", 0x40)  # e-mail address (domain only until registered)
    body += bytes(0x20)  # password: none
    body += bytes(0x24)  # mail check ID: none
    for url in SERVER_URLS:
        body += fixed_text(url, 0x80)
    body += bytes(0xDC)  # reserved
    body += be32(0)  # do not let mail start the console
    assert len(body) == 1020
    # The checksum is the sum of the first 255 words.
    total = sum(struct.unpack(">255I", bytes(body))) & 0xFFFFFFFF
    body += be32(total)
    return bytes(body)


# ---------------------------------------------------------------- nwc24dl.bin

DL_MAX_ENTRIES = 120
DL_MAX_SUBTASKS = 32
DL_RESERVED_MAIL = 8
DL_ENTRY_UNUSED = 0xFF


def download_list():
    header = b"WcDl" + be32(1, 0, 0) + struct.pack(">HHH", DL_MAX_SUBTASKS, DL_RESERVED_MAIL,
                                                    DL_MAX_ENTRIES)
    header += bytes(128 - len(header))
    records = bytes(16 * DL_MAX_ENTRIES)  # scheduling records, all empty
    entries = bytearray()
    for index in range(DL_MAX_ENTRIES):
        entry = struct.pack(">HB", index, DL_ENTRY_UNUSED)
        entries += entry + bytes(512 - len(entry))
    data = header + records + bytes(entries)
    assert len(data) == 0xF800
    return data


# ---------------------------------------------------------------- nwc24fl.bin

FL_MAX_FRIENDS = 100


def friend_list():
    header = b"WcFl" + be32(2, FL_MAX_FRIENDS, 0)
    header += bytes(64 - len(header))
    codes = bytes(8 * FL_MAX_FRIENDS)
    entries = bytes(320 * FL_MAX_FRIENDS)
    data = header + codes + entries
    assert len(data) == 32864
    return data


# ---------------------------------------------------------------- mail lists and boxes

MAIL_ENTRY_SIZE = 128
MAIL_FLAG_LENGTH = 40


def mail_list(entries, file_size, box_size):
    """A mail list (.ctl) with no mail. Unused entries form a chain: each one's fourth word is
    the file offset of the next unused entry, and the last one's is 0."""
    header = b"WcTf" + be32(
        4,          # version (System Menu 4.x)
        0,          # messages
        entries,    # entries in the file
        0,          # bytes of mail
        file_size,  # size of this file
        1,          # next message ID
        MAIL_ENTRY_SIZE,  # offset of the next free entry (the first)
        0,
        box_size,   # free space in the matching .mbx volume
    )
    header += bytes(48)
    header += b"0" * (MAIL_FLAG_LENGTH - 1) + b"\0"  # mail flag: nothing new
    assert len(header) == MAIL_ENTRY_SIZE
    data = bytearray(header)
    for index in range(entries):
        entry = bytearray(MAIL_ENTRY_SIZE)
        last = index == entries - 1
        entry[12:16] = be32(0 if last else (index + 2) * MAIL_ENTRY_SIZE)
        data += entry
    assert len(data) == file_size
    return bytes(data)


def empty_vff(volume_size, fat16):
    """The header of an empty VFF volume (512-byte clusters) and the start of its first FAT:
    the media entry, the end-of-chain entry and cluster 2 marked end of chain, as a new volume
    starts."""
    header = b"VFF " + struct.pack(">HH", 0xFEFF, 0x0100) + be32(volume_size)
    header += struct.pack(">H", 512 // 16)  # cluster size in 16-byte units
    header += bytes(32 - len(header))
    if fat16:
        fat = struct.pack("<HHH", 0xFFF0, 0xFFFF, 0xFFFF)
    else:
        # FAT12: three 12-bit entries, 0xFF0, 0xFFF and 0xFFF, two per three bytes.
        e = (0xFF0, 0xFFF, 0xFFF, 0x000)
        fat = bytes([e[0] & 0xFF, (e[0] >> 8) | ((e[1] & 0xF) << 4), e[1] >> 4,
                     e[2] & 0xFF, (e[2] >> 8) | ((e[3] & 0xF) << 4), e[3] >> 4])
    return header + fat + bytes(16 - len(fat))


SEND_BOX_SIZE = 0x200000
RECEIVE_BOX_SIZE = 0x700000


def all_outputs():
    config = account_config()
    return {
        "nwc24msg.cfg": config,
        "nwc24msg.cbk": config,
        "nwc24dl.bin": download_list(),
        "nwc24fl.bin": friend_list(),
        "mbox/wc24send.ctl": mail_list(127, 0x4000, SEND_BOX_SIZE),
        "mbox/wc24recv.ctl": mail_list(255, 0x8000, RECEIVE_BOX_SIZE),
        # The send box is small enough for FAT12, the receive box needs FAT16.
        "mbox/wc24send.mbx": empty_vff(SEND_BOX_SIZE, fat16=False),
        "mbox/wc24recv.mbx": empty_vff(RECEIVE_BOX_SIZE, fat16=True),
    }


def main():
    check = "--check" in sys.argv[1:]
    bad = 0
    files = all_outputs()
    for rel, data in sorted(files.items()):
        path = os.path.join(OUT_DIR, rel)
        if check:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                print("differs or missing:", os.path.relpath(path, HERE))
                bad += 1
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    if check:
        print("%d files checked, %d differ or are missing" % (len(files), bad))
        return 1 if bad else 0
    print("wrote %d files to %s" % (len(files), os.path.relpath(OUT_DIR, HERE)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
