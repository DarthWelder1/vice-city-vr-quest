#!/usr/bin/env python3
"""Merges TXD dictionaries: keeps every texture from --base, and appends only
the textures from --extra whose names are not already in the base. Used to
pull a few missing textures out of a pack without inheriting its worse
versions of the ones we already have.

Linux port of txdmerge.ps1.

Usage:
    txdmerge.py --base original.txd --extra pack.txd --out merged.txd
"""
import argparse
import os
import struct
import sys


def read_txd(path):
    with open(path, "rb") as f:
        b = f.read()
    # outer chunk: type, size, version
    outer_type, outer_size, version = struct.unpack_from("<III", b, 0)
    if outer_type != 0x16:
        raise ValueError(f"{path} is not a texture dictionary (type {outer_type})")
    pos = 12
    # struct chunk
    s_type, s_size, s_ver = struct.unpack_from("<III", b, pos)
    if s_type != 0x01:
        raise ValueError(f"{path} has no struct chunk")
    num_tex, device_id = struct.unpack_from("<HH", b, pos + 12)
    pos += 12 + s_size
    end = 12 + outer_size

    tex = []
    trailing = []
    while pos + 12 <= end:
        c_type, c_size = struct.unpack_from("<II", b, pos)
        total = 12 + c_size
        if c_type == 0x15:
            # name lives at struct payload +8 (after platformId and filter/addr)
            name_off = pos + 12 + 12 + 8
            z = b.find(b"\x00", name_off) - name_off
            if z < 0 or z > 32:
                z = 32
            name = b[name_off:name_off + z].decode("ascii", "replace")
            tex.append((name, b[pos:pos + total]))
        else:
            trailing.append(b[pos:pos + total])
        pos += total

    return {
        "version": version,
        "s_ver": s_ver,
        "num_tex": num_tex,
        "device_id": device_id,
        "tex": tex,
        "trailing": trailing,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", required=True)
    ap.add_argument("--extra", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    bd = read_txd(args.base)
    ed = read_txd(args.extra)
    print(f"base : {len(bd['tex'])} textures (declared {bd['num_tex']})")
    print(f"extra: {len(ed['tex'])} textures (declared {ed['num_tex']})")

    have = {t[0].lower() for t in bd["tex"]}
    add = [t for t in ed["tex"] if t[0].lower() not in have]
    print(f"adding {len(add)} textures absent from base:")
    for name, _ in add:
        print(f"    {name}")

    all_tex = bd["tex"] + add

    # struct chunk
    body = bytearray()
    body += struct.pack("<III", 0x01, 4, bd["s_ver"])
    body += struct.pack("<HH", len(all_tex), bd["device_id"])
    for _, chunk in all_tex:
        body += chunk
    for chunk in bd["trailing"]:
        body += chunk

    with open(args.out, "wb") as f:
        f.write(struct.pack("<III", 0x16, len(body), bd["version"]))
        f.write(bytes(body))
    size_mb = os.path.getsize(args.out) / (1024 * 1024)
    print(f"wrote {args.out} ({size_mb:.1f} MB, {len(all_tex)} textures)")


if __name__ == "__main__":
    main()
