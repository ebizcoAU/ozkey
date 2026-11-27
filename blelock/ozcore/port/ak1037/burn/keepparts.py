#!/usr/bin/env python3
"""Cut ROMFS and OZKV out of a GUI-tool read-back (spiflash.bin) so a burn can write them back unchanged.

The Anyka burn tool refuses a config whose partition has no file ("Partition name and BIN file name are
different"), so config_ozkie_v2.txt writes ROMFS and OZKV with their own current contents.

    python3 -I keepparts.py <spiflash.bin> <out_dir>

Writes <out_dir>/romfs_keep.bin and <out_dir>/ozkv_keep.bin (mode 0600). ozkv_keep.bin holds the lock's
identity and bonds: never commit, share or print it. Prints sizes and short hashes only.
"""
import hashlib
import os
import struct
import sys

NAMES = ("OTA", "PROG", "LP", "ROMFS", "UDATA", "OZKV", "FPDAT")


def table(img):
    """Partition table written by the tool: 32-byte entries, name[6] kb:u16 pad:u16 off:u32 len:u32 ..."""
    # "OTA\0\0\0" also occurs before the table; take the match followed by PROG one entry later.
    t = img.find(b"OTA\0\0\0", 0, 0x4000)
    while t >= 0 and img[t + 32: t + 38] != b"PROG\0\0":
        t = img.find(b"OTA\0\0\0", t + 1, 0x4000)
    if t < 0:
        sys.exit("no partition table in the first 16 KB")
    parts = {}
    for k, want in enumerate(NAMES):
        e = img[t + 32 * k: t + 32 * k + 32]
        name = e[:6].rstrip(b"\0").decode("ascii", "replace")
        if name != want:
            sys.exit(f"entry {k}: {name!r}, expected {want!r}")
        kb = struct.unpack("<H", e[6:8])[0]
        off, ln = struct.unpack("<II", e[10:18])
        parts[name] = (off, kb, ln)
    for a, b in zip(NAMES, NAMES[1:]):  # contiguous, no overlap
        off, kb, _ = parts[a]
        if off + kb * 1024 != parts[b][0]:
            sys.exit(f"{a} does not end where {b} starts")
    return parts


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    parts = table(img)
    if parts["OZKV"][1] != 64:
        sys.exit("OZKV is not 64 KB; layout differs from config_ozkie*.txt")
    cuts = (("ROMFS", parts["ROMFS"][2], "romfs_keep.bin"),  # data length the tool recorded
            ("OZKV", parts["OZKV"][1] * 1024, "ozkv_keep.bin"))  # whole partition
    for name, ln, fn in cuts:
        off = parts[name][0]
        b = img[off: off + ln]
        if len(b) != ln:
            sys.exit(f"{name} runs past the image end")
        p = os.path.join(out, fn)
        fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(b)
        print(f"{fn}: {name} @0x{off:06x}, {ln} B, sha256 {hashlib.sha256(b).hexdigest()[:16]}…")


if __name__ == "__main__":
    main()
