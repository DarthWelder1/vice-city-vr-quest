#!/usr/bin/env python3
"""Extracts named entries from an IMG v1 archive into a target directory.

Linux port of imgextract.ps1. The IMG v1 directory is a flat list of 32-byte
entries: [0:4] sector (u32 LE), [4:8] sector count (u32 LE), [8:32] name
(24 bytes, NUL-terminated). Sector size is 2048, so an entry lives at
offset = sector*2048 and spans count*2048 bytes.

Usage:
    imgextract.py --img gta3.img --out outdir --entries name1 name2 ...
"""
import argparse
import os
import sys

SECT = 2048


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--img", required=True, help="path to the .img archive")
    ap.add_argument("--out", required=True, help="target directory for extracted files")
    ap.add_argument("--entries", nargs="+", required=True,
                    help="entry names to extract (matched case-insensitively)")
    args = ap.parse_args()

    dir_path = os.path.splitext(args.img)[0] + ".dir"
    if not os.path.isfile(dir_path):
        sys.exit(f"directory file not found: {dir_path}")

    with open(dir_path, "rb") as f:
        db = f.read()
    if len(db) % 32 != 0:
        sys.exit(f"{dir_path} length {len(db)} is not a multiple of 32")
    n = len(db) // 32

    os.makedirs(args.out, exist_ok=True)
    want = {e.lower() for e in args.entries}

    with open(args.img, "rb") as fs:
        for i in range(n):
            b = i * 32
            # name starts at b+8; find the NUL terminator within the 24-byte field
            z = db.find(b"\x00", b + 8, b + 32) - (b + 8)
            if z < 0 or z > 24:
                z = 24
            name = db[b + 8:b + 8 + z].decode("ascii", "replace")
            if name.lower() not in want:
                continue
            off = int.from_bytes(db[b:b + 4], "little") * SECT
            ln = int.from_bytes(db[b + 4:b + 8], "little") * SECT
            fs.seek(off)
            buf = fs.read(ln)
            out_name = name
            with open(os.path.join(args.out, out_name), "wb") as o:
                o.write(buf)
            print(f"extracted {out_name} ({ln:,} bytes)")


if __name__ == "__main__":
    main()
