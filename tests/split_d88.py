#!/usr/bin/env python3
"""Split a multi-image D88 into one file per bundled disk image
(QUASI88 wants one image per drive). Each image carries its own
header; its total size is the little-endian uint32 at +0x1C.

Usage: split_d88.py MULTI.D88 OUTDIR -> OUTDIR/DISK_A.D88, DISK_B.D88, ...
"""
import os
import struct
import sys


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    os.makedirs(outdir, exist_ok=True)
    # Image 0 starts at offset 0: a D88 file IS image 0 (header
    # included), and further images follow contiguously.
    base = 0
    names = "ABCDEFGH"
    i = 0
    while base + 0x1C < len(d) and i < len(names):
        size = struct.unpack("<I", d[base + 0x1C:base + 0x20])[0]
        if size <= 0 or base + size > len(d):
            break
        out = os.path.join(outdir, "DISK_%s.D88" % names[i])
        open(out, "wb").write(d[base:base + size])
        print(out, size, d[base:base + 16])
        base += size
        i += 1
    if i == 0:
        print("no images found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
