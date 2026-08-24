#!/usr/bin/env python3
"""Rebuilds a GTA IMG version 1 archive (gta3.img + gta3.dir), replacing entries
whose name matches a file supplied on disk. Entries keep their dir order;
offsets and sizes are recomputed.

Linux port of imginject.ps1. The rebuild writes a full second copy of the
archive beside the original, then swaps it in atomically through a `.old`
backup so a failure never leaves the original deleted.

Usage:
    imginject.py --img gta3.img --from srcdir1 srcdir2 ... [--exclude name ...]
"""
import argparse
import os
import shutil
import sys

SECTOR = 2048
BUF = 8 * 1024 * 1024


def swap_in_place(new, target):
    """Atomically replace target with new, keeping a backup until success."""
    if not os.path.exists(new):
        sys.exit(f"'{new}' was written but is no longer there. Antivirus or a "
                 f"folder-sync client is the usual cause; build outside "
                 f"Downloads/Documents, or exclude the build folder from "
                 f"real-time scanning.")
    backup = target + ".old"
    if os.path.exists(backup):
        os.remove(backup)
    os.replace(target, backup)
    try:
        os.replace(new, target)
    except Exception as e:
        os.replace(backup, target)
        sys.exit(f"Could not put the rebuilt '{target}' in place, the original "
                 f"is back: {e}")
    if os.path.exists(backup):
        os.remove(backup)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--img", required=True, help="path to the .img archive")
    ap.add_argument("--from", dest="froms", nargs="+", required=True,
                    help="source directories whose .dff/.txd files replace entries")
    ap.add_argument("--exclude", nargs="*", default=[],
                    help="entry names to drop from the archive (case-insensitive)")
    args = ap.parse_args()

    img = args.img
    dir_path = os.path.splitext(img)[0] + ".dir"
    if not os.path.isfile(dir_path):
        sys.exit(f"directory file not found: {dir_path}")

    # collect replacements, later sources win. A source may be a directory
    # (all its .dff/.txd files are offered) or a single file (offered if its
    # extension matches) — mirroring the PowerShell Get-ChildItem -LiteralPath
    # semantics.
    repl = {}
    for src in args.froms:
        if not os.path.exists(src):
            print(f"  missing source: {src}")
            continue
        if os.path.isdir(src):
            for fn in sorted(os.listdir(src)):
                full = os.path.join(src, fn)
                if not os.path.isfile(full):
                    continue
                ext = os.path.splitext(fn)[1].lower()
                if ext not in (".dff", ".txd"):
                    continue
                repl[fn.lower()] = full
        else:
            fn = os.path.basename(src)
            ext = os.path.splitext(fn)[1].lower()
            if ext in (".dff", ".txd"):
                repl[fn.lower()] = src
    print(f"replacement files offered: {len(repl)}")

    with open(dir_path, "rb") as f:
        db = f.read()
    if len(db) % 32 != 0:
        sys.exit(f"{dir_path} length {len(db)} is not a multiple of 32")
    n = len(db) // 32

    entries = []
    for i in range(n):
        b = i * 32
        z = db.find(b"\x00", b + 8, b + 32) - (b + 8)
        if z < 0 or z > 24:
            z = 24
        name = db[b + 8:b + 8 + z].decode("ascii", "replace")
        off = int.from_bytes(db[b:b + 4], "little") * SECTOR
        size = int.from_bytes(db[b + 4:b + 8], "little") * SECTOR
        entries.append({"name": name, "offset": off, "size": size})
    print(f"archive entries: {len(entries)}")

    # Optional exact-name pruning (used to drop expensive vegetation geometry).
    exclude_set = {e.lower() for e in args.exclude if e}
    if exclude_set:
        before = len(entries)
        entries = [e for e in entries if e["name"].lower() not in exclude_set]
        print(f"archive entries excluded: {before - len(entries)}")

    out_img = img + ".new"

    # The rebuild needs room for a full second copy of the archive.
    need = os.path.getsize(img)
    try:
        free = shutil.disk_usage(os.path.abspath(img)).free
    except OSError:
        free = None
    if free is not None and free < need:
        sys.exit(f"Rebuilding '{img}' needs {need / 1e9:.1f} GB free on that "
                 f"drive; {free / 1e9:.1f} GB is available.")

    new_dir = bytearray(len(entries) * 32)
    replaced = 0
    used = {}
    in_f = out_f = None
    try:
        in_f = open(img, "rb")
        out_f = open(out_img, "wb")
        pad = bytearray(SECTOR)
        next_sector = 0
        for i, e in enumerate(entries):
            key = e["name"].lower()
            written = 0
            if key in repl:
                with open(repl[key], "rb") as r:
                    data = r.read()
                out_f.write(data)
                written = len(data)
                replaced += 1
                used[key] = True
            else:
                in_f.seek(e["offset"])
                left = e["size"]
                while left > 0:
                    take = min(BUF, left)
                    got = in_f.read(take)
                    if not got:
                        break
                    out_f.write(got)
                    written += len(got)
                    left -= len(got)
            tail = written % SECTOR
            if tail:
                out_f.write(pad[:SECTOR - tail])
                written += SECTOR - tail
            size_sectors = written // SECTOR
            b = i * 32
            new_dir[b:b + 4] = next_sector.to_bytes(4, "little")
            new_dir[b + 4:b + 8] = size_sectors.to_bytes(4, "little")
            name_bytes = e["name"].encode("ascii", "replace")
            new_dir[b + 8:b + 8 + len(name_bytes)] = name_bytes
            next_sector += size_sectors
    except Exception as e:
        if in_f:
            in_f.close()
        if out_f:
            out_f.close()
        if os.path.exists(out_img):
            os.remove(out_img)
        sys.exit(f"Rebuilding '{img}' failed and the original was left "
                 f"untouched: {e}")
    else:
        if in_f:
            in_f.close()
        if out_f:
            out_f.close()

    with open(dir_path + ".new", "wb") as f:
        f.write(new_dir)

    if os.path.getsize(out_img) < SECTOR:
        sys.exit(f"The rebuilt archive '{out_img}' is empty; the original was "
                 f"left untouched.")
    swap_in_place(out_img, img)
    swap_in_place(dir_path + ".new", dir_path)

    print(f"entries replaced in archive: {replaced}")
    unused = sorted(k for k in repl if k not in used)
    print(f"supplied files with no matching entry: {len(unused)}")
    for name in unused[:20]:
        print(f"    {name}")


if __name__ == "__main__":
    main()
