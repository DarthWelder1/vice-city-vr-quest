#!/usr/bin/env python3
"""Audits every TXD inside an IMG v1 archive and reports which entries contain
textures that stream badly: uncompressed 24/32-bit, or any texture at 64px+
with a single mip level. CandidateList is configurable so release builds can
keep generated state in their private staging directory instead of modifying
the read-only tools folder or colliding with another build.

Linux port of imgtexaudit.ps1.

Usage:
    imgtexaudit.py --img gta3.img [--candidate-list txd_candidates.txt]
"""
import argparse
import os
import sys

SECT = 2048
MAX_TXD = 80 * 1024 * 1024


def read_dir_entries(dir_path):
    with open(dir_path, "rb") as f:
        db = f.read()
    if len(db) % 32 != 0:
        sys.exit(f"{dir_path} length {len(db)} is not a multiple of 32")
    entries = []
    for i in range(len(db) // 32):
        b = i * 32
        z = db.find(b"\x00", b + 8, b + 32) - (b + 8)
        if z < 0 or z > 24:
            z = 24
        name = db[b + 8:b + 8 + z].decode("ascii", "replace")
        off = int.from_bytes(db[b:b + 4], "little") * SECT
        ln = int.from_bytes(db[b + 4:b + 8], "little") * SECT
        entries.append((name, off, ln))
    return entries


def scan_chunks(data, start, end, parent, stats):
    """Walk RenderWare chunks; recurse into 0x16/0x15, count texture natives
    (0x01 under a 0x15 parent)."""
    p = start
    while p + 12 <= end:
        t = int.from_bytes(data[p:p + 4], "little")
        s = int.from_bytes(data[p + 4:p + 8], "little")
        de = p + 12 + s
        if de > end or s == 0:
            break
        if t == 0x01 and parent == 0x15:
            w = int.from_bytes(data[p + 12 + 80:p + 12 + 82], "little")
            h = int.from_bytes(data[p + 12 + 82:p + 12 + 84], "little")
            depth = data[p + 12 + 84]
            lev = data[p + 12 + 85]
            comp = data[p + 12 + 87]
            stats["texs"] += 1
            dim = max(w, h)
            if comp == 0 and depth >= 24:
                stats["uncmp"] += 1
                stats["wasted"] += w * h * (depth // 8)
            elif lev <= 1 and dim >= 64:
                stats["nomip"] += 1
        elif t in (0x16, 0x15):
            scan_chunks(data, p + 12, de, t, stats)
        p = de


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--img", required=True, help="path to the .img archive")
    ap.add_argument("--candidate-list",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "txd_candidates.txt"),
                    help="where to write the candidate list")
    args = ap.parse_args()

    dir_path = os.path.splitext(args.img)[0] + ".dir"
    entries = read_dir_entries(dir_path)

    bad = []
    with open(args.img, "rb") as fs:
        for name, off, ln in entries:
            if not name.lower().endswith(".txd"):
                continue
            if ln <= 0 or ln > MAX_TXD:
                continue
            fs.seek(off)
            buf = fs.read(ln)
            stats = {"texs": 0, "uncmp": 0, "nomip": 0, "wasted": 0}
            scan_chunks(buf, 0, len(buf), 0, stats)
            if stats["uncmp"] > 0 or stats["nomip"] > 0:
                bad.append({
                    "name": name,
                    "kb": round(ln / 1024),
                    "texs": stats["texs"],
                    "uncompressed": stats["uncmp"],
                    "nomip": stats["nomip"],
                    "uncmp_mb": round(stats["wasted"] / (1024 * 1024), 1),
                })

    print(f"txd entries with problems: {len(bad)}")
    for row in sorted(bad, key=lambda r: r["uncmp_mb"], reverse=True)[:25]:
        print(f"    {row['name']:32} {row['kb']:>9} KB  {row['texs']:>4} texs  "
              f"{row['uncompressed']:>4} uncmp  {row['nomip']:>4} no-mip  "
              f"{row['uncmp_mb']:>8} MB uncmp")
    total_uncmp = sum(r["uncmp_mb"] for r in bad)
    total_nomip = sum(r["nomip"] for r in bad)
    print(f"total uncompressed pixel data: {total_uncmp:,.0f} MB")
    print(f"total no-mip textures: {total_nomip}")

    # dump the full list for the extraction step
    with open(args.candidate_list, "w", encoding="ascii") as f:
        for name in sorted({r["name"] for r in bad}):
            f.write(name + "\n")
    print(f"candidate list written to {args.candidate_list}")


if __name__ == "__main__":
    main()
