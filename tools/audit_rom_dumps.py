#!/usr/bin/env python3
# POM2 Apple II Emulator — GPL-3.0-or-later
"""Compare every dump in roms/ with the CRC-32s MAME records for the chips.

    tools/audit_rom_dumps.py                 # fetch MAME at the pin, audit roms/
    tools/audit_rom_dumps.py --mame-src DIR  # use MAME sources already on disk

Why: the ][+ dump POM2 pins carried $FF59 at $FFFE where Apple's 341-0020-00
has $FA40 — every IRQ and BRK fell into the Monitor — and nobody saw it until
the two were compared page by page (2026-11-07, RomErrata.h). RomCatalog.h
asserts a CRC on fewer than two entries in three, and an asserted CRC is only
the one the file had when it was catalogued.

For each file the tool reports:
  MATCH    the whole file is one MAME ROM_LOAD
  TILED    every slice of one MAME chip size matches some ROM_LOAD
  PARTIAL  some slices match: the others are named, to be explained
  NONE     nothing matches (POM2's own ROMs, dumps MAME does not have)

A PARTIAL or NONE is a question, not a verdict: a combined dump carries
filler MAME does not catalogue, and MAME lacks several cards POM2 runs. The
answers of the 2026-11-07 run are in docs/audits/rom-dumps-vs-mame-2026-11-07.md.
Evidence grade of a match: [D] (MAME's catalogue, at the pinned revision).
"""

import argparse
import collections
import glob
import json
import os
import re
import sys
import urllib.request
import zlib

MAME_REV = "b2a182f0b7"      # the revision POM2's citations pin (ZipChip.h)
MAME_DIRS = ["src/mame/apple", "src/devices/bus/a2bus", "src/devices/bus/a2gameio",
             "src/devices/machine"]
ROM_LOAD = re.compile(
    r'ROMX?_LOAD\w*\s*\(\s*"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*'
    r'(0x[0-9a-fA-F]+|\d+)\s*,\s*CRC\(([0-9a-fA-F]{8})\)')
SKIP_EXT = (".txt", ".md", ".wav", ".flac", ".zip")


def fetch_mame(cache):
    os.makedirs(cache, exist_ok=True)
    for d in MAME_DIRS:
        url = f"https://api.github.com/repos/mamedev/mame/contents/{d}?ref={MAME_REV}"
        with urllib.request.urlopen(url) as r:
            entries = json.load(r)
        for e in entries:
            if not e["name"].endswith(".cpp"):
                continue
            out = os.path.join(cache, e["path"].replace("/", "_"))
            if os.path.exists(out):
                continue
            raw = (f"https://raw.githubusercontent.com/mamedev/mame/{MAME_REV}/"
                   f"{e['path']}")
            with urllib.request.urlopen(raw) as r, open(out, "wb") as f:
                f.write(r.read())
    return cache


def load_catalogue(src):
    crcs = collections.defaultdict(set)
    sizes = set()
    for path in glob.glob(os.path.join(src, "*.cpp")):
        with open(path, errors="ignore") as f:
            for m in ROM_LOAD.finditer(f.read()):
                size = int(m.group(3), 0)
                crcs[int(m.group(4), 16)].add(m.group(1))
                if size >= 0x100:
                    sizes.add(size)
    return crcs, sorted(sizes, reverse=True)


def name_of(names):
    """Apple's part number first when clones share the CRC (1331403.g5...)."""
    return min(names, key=lambda n: (not n.startswith(("341", "342", "344")), n))


def audit(path, crcs, sizes):
    data = open(path, "rb").read()
    whole = zlib.crc32(data)
    if whole in crcs:
        return "MATCH", name_of(crcs[whole])
    best = None
    for size in sizes:
        if size > len(data) or len(data) % size:
            continue
        slices = [zlib.crc32(data[i:i + size]) for i in range(0, len(data), size)]
        hits = sum(c in crcs for c in slices)
        if hits and (best is None or hits / len(slices) > best[1] / len(best[2])):
            best = (size, hits, slices)
    if best is None:
        return "NONE", f"crc {whole:08x}"
    size, hits, slices = best
    if hits == len(slices):
        return "TILED", f"{len(slices)} x {size:#x}"
    detail = ", ".join(
        f"+{i * size:#x}={name_of(crcs[c]) if c in crcs else '?'}"
        for i, c in enumerate(slices))
    return "PARTIAL", f"{hits}/{len(slices)} x {size:#x}: {detail}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mame-src", help="directory of MAME .cpp files (flat)")
    ap.add_argument("--cache", default=os.path.join(
        os.environ.get("TMPDIR", "/tmp"), f"pom2_mame_{MAME_REV}"))
    ap.add_argument("--roms", default="roms")
    args = ap.parse_args()
    src = args.mame_src or fetch_mame(args.cache)
    crcs, sizes = load_catalogue(src)
    if not crcs:
        sys.exit(f"no ROM_LOAD lines found under {src}")
    print(f"MAME {MAME_REV}: {sum(len(v) for v in crcs.values())} ROM_LOAD entries")
    files = sorted(p for p in glob.glob(os.path.join(args.roms, "*"))
                   if os.path.isfile(p) and not p.lower().endswith(SKIP_EXT))
    counts = collections.Counter()
    for p in files:
        verdict, detail = audit(p, crcs, sizes)
        counts[verdict] += 1
        print(f"{verdict:8s} {p}  {detail}")
    print(", ".join(f"{k} {v}" for k, v in sorted(counts.items())))


if __name__ == "__main__":
    main()
