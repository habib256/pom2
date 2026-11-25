#!/usr/bin/env python3
# POM2 Apple II Emulator
# Copyright (C) 2026 VERHILLE Arnaud
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a clrmamepro DAT of 4am's woz-a-day collection for libretro-database.

RetroArch names a scanned disk from libretro-database; its "Apple - II" DAT
is TOSEC's, which knows 594 of woz-a-day's boot disks (2026-11-25). This
reads every disk zip of the collection (tools/woz_corpus_fetch.py's metadata
and the zips cached by --fetch) and writes one entry per disk, TOSEC-style:
"Title (Year)(Publisher)(Disk 1)(Side A)". One entry per disk because
RetroArch's c_converter indexes only the last rom of a multi-rom game (a
game's first disk then never matched a scan, 2026-11-25). One thumbnail
still serves every disk: RetroArch falls back to the label up to its first
" (" (gfx_thumbnail_fill_content_img), the same for all of a title's disks.

The name is "Title (Year)(Publisher)" from the WOZ's own META chunk (the
WOZ 2 specification's title, publisher and copyright fields), falling back
to archive.org's item metadata. Left out: IIgs-only titles (META
requires_machine = 2gs: POM2 is an 8-bit Apple II), and any disk whose CRC
TOSEC's DAT already lists, so the DAT adds names and never renames one.

  wozaday_dat.py --fetch            download the disk zips (754 MB, MD5-checked)
  wozaday_dat.py --tosec TOSEC.dat --out "Apple - II.dat"
"""
import argparse
import concurrent.futures
import datetime
import glob
import hashlib
import io
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request
import zipfile
import zlib

CACHE = os.path.expanduser("~/.cache")
META_DIR = os.path.join(CACHE, "pom2-wozaday-meta")
ZIP_DIR = os.path.join(CACHE, "pom2-wozaday-zips")
MANIFEST = os.path.join(CACHE, "pom2-wozaday", "manifest.json")
AGENT = {"User-Agent": "pom2-dat-builder/1.0"}


def disk_zips(meta):
    return [f for f in meta.get("files", [])
            if f["name"].endswith("(woz-a-day collection).zip") and " extras " not in f["name"]]


def fetch_metadata():
    os.makedirs(META_DIR, exist_ok=True)
    ids = [e["identifier"] for e in json.load(open(MANIFEST))]

    def get(ident):
        path = os.path.join(META_DIR, ident + ".json")
        if os.path.exists(path) and os.path.getsize(path) > 100:
            return
        for attempt in range(4):
            try:
                data = urllib.request.urlopen(urllib.request.Request(
                    f"https://archive.org/metadata/{ident}", headers=AGENT), timeout=60).read()
                json.loads(data)
                open(path, "wb").write(data)
                return
            except Exception:
                time.sleep(2 + 3 * attempt)
        raise RuntimeError(f"no metadata for {ident}")

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        list(pool.map(get, ids))


def fetch_zips():
    os.makedirs(ZIP_DIR, exist_ok=True)
    jobs = []
    for path in sorted(glob.glob(os.path.join(META_DIR, "*.json"))):
        ident = os.path.basename(path)[:-5]
        for f in disk_zips(json.load(open(path))):
            jobs.append((ident, f["name"], f.get("md5")))

    def get(job):
        ident, name, md5 = job
        dst = os.path.join(ZIP_DIR, ident + "__" + name.replace("/", "_"))
        if os.path.exists(dst) and hashlib.md5(open(dst, "rb").read()).hexdigest() == md5:
            return
        url = f"https://archive.org/download/{ident}/" + urllib.parse.quote(name)
        for attempt in range(5):
            try:
                data = urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=120).read()
                if hashlib.md5(data).hexdigest() == md5:
                    open(dst, "wb").write(data)
                    return
            except Exception:
                pass
            time.sleep(3 + 5 * attempt)
        raise RuntimeError(f"could not fetch {url}")

    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        list(pool.map(get, jobs))


def woz_meta(data):
    """The META chunk of a WOZ 1/2 image as a dict (WOZ 2 spec, META chunk)."""
    if data[:4] not in (b"WOZ1", b"WOZ2"):
        return {}
    pos = 12
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if cid == b"META":
            text = data[pos + 8:pos + 8 + size].decode("utf-8", "replace")
            meta = {}
            for line in text.split("\n"):
                if "\t" in line:
                    key, value = line.split("\t", 1)
                    meta[key.strip()] = value.strip()
            return meta
        pos += 8 + size
    return {}


def woz_disk_type(data):
    """INFO chunk's disk type (WOZ 2 spec): 1 = 5.25", 2 = 3.5"."""
    if data[:4] not in (b"WOZ1", b"WOZ2") or data[12:16] != b"INFO":
        return 1
    return data[21]


def clean(text):
    text = re.sub(r"\s+", " ", text or "").strip()
    return text.replace('"', "'")


def item_title(meta):
    return re.sub(r"\s*\(woz-a-day collection\)\s*$", "", meta.get("metadata", {}).get("title", "")).strip()


def item_year(meta):
    m = re.search(r"\bis an? (\d{4})\b", meta.get("metadata", {}).get("description", ""))
    return m.group(1) if m else ""


def item_publisher(meta):
    creator = meta.get("metadata", {}).get("creator", "")
    if isinstance(creator, list):
        creator = creator[0] if creator else ""
    m = re.search(r"\(([^()]*)\)\s*$", creator)
    return (m.group(1) if m else creator).strip()


def disk_suffix(rom, side, title):
    """(Disk 1)(Side A) from the WOZ META side, else what the file name adds."""
    if side:
        return "".join(f"({part.strip()})" for part in side.split(",") if part.strip())
    stem = os.path.splitext(rom)[0]
    rest = stem[len(title):] if stem.lower().startswith(title.lower()) else stem
    rest = rest.strip(" -_.")
    return f"({rest or stem})"


def tosec_crcs(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    return {m.upper() for m in re.findall(r" crc ([0-9A-Fa-f]{8}) ", text)}


def tosec_names(path):
    """CRC -> TOSEC's game name, for a playable copy of a title TOSEC lists."""
    text = open(path, encoding="utf-8", errors="replace").read()
    names = {}
    for game in re.finditer(r'game \(\s*name "(.*?)"(.*?)\n\)', text, re.S):
        for crc in re.findall(r" crc ([0-9A-Fa-f]{8}) ", game.group(2)):
            names.setdefault(crc.upper(), game.group(1))
    return names


def build(tosec, out):
    known = tosec_crcs(tosec)
    tosec_name = tosec_names(tosec)
    # Every disk of every zip, first: a playable copy never takes the CRC
    # of a real disk of another title.
    zipped = set()
    for path in sorted(glob.glob(os.path.join(META_DIR, "*.json"))):
        ident = os.path.basename(path)[:-5]
        for f in disk_zips(json.load(open(path))):
            with zipfile.ZipFile(os.path.join(ZIP_DIR, ident + "__" + f["name"].replace("/", "_"))) as z:
                zipped |= {"%08X" % i.CRC for i in z.infolist() if i.filename.lower().endswith(".woz")}
    games, seen, alternates = [], set(), []
    skipped_tosec = skipped_gs = 0
    for path in sorted(glob.glob(os.path.join(META_DIR, "*.json"))):
        ident = os.path.basename(path)[:-5]
        meta = json.load(open(path))
        roms, metas, zip_crcs, gs_only = [], [], [], False
        for f in disk_zips(meta):
            zpath = os.path.join(ZIP_DIR, ident + "__" + f["name"].replace("/", "_"))
            if not os.path.exists(zpath):
                raise SystemExit(f"missing {zpath}: run --fetch first")
            with zipfile.ZipFile(zpath) as z:
                for info in z.infolist():
                    if not info.filename.lower().endswith(".woz"):
                        continue
                    data = z.read(info)
                    crc = "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)
                    zip_crcs.append(crc)
                    wm = woz_meta(data)
                    machines = set(filter(None, wm.get("requires_machine", "").split("|")))
                    if machines == {"2gs"}:
                        gs_only = True
                        skipped_gs += 1
                        continue
                    if crc in known:
                        skipped_tosec += 1
                        continue
                    if crc in seen:
                        continue
                    seen.add(crc)
                    wm["_disk_type"] = woz_disk_type(data)
                    metas.append(wm)
                    roms.append((os.path.basename(info.filename), len(data), crc,
                                 hashlib.md5(data).hexdigest().upper(), hashlib.sha1(data).hexdigest().upper(),
                                 clean(wm.get("side", "")), ""))
        if not roms:
            # Every disk is TOSEC's: a playable copy that differs still gets
            # an entry, as an alternate of TOSEC's name for the first disk.
            first_disk = next((tosec_name[c] for c in zip_crcs if c in tosec_name), None)
            for f in meta.get("files", []):
                crc = (f.get("crc32") or "").upper().zfill(8)
                if (first_disk and f["name"] == "00playable.woz" and f.get("crc32") and not gs_only
                        and crc not in seen and crc not in known and crc not in zipped):
                    seen.add(crc)
                    alternates.append((first_disk + "[a]", "", "", "",
                                       ("00playable.woz", int(f["size"]), crc, f["md5"].upper(), f["sha1"].upper())))
            continue
        first = next((m for m in metas if m.get("title")), {})
        title = clean(first.get("title") or item_title(meta))
        subtitle = clean(first.get("subtitle", ""))
        if subtitle and subtitle.lower() not in title.lower():
            title += " - " + subtitle
        version = clean(first.get("version", ""))
        if version and version.lower() not in title.lower():
            title += " v" + version.lstrip("vV")
        year = (re.search(r"\b(19|20)\d\d\b", first.get("copyright", "")) or [None])[0] or item_year(meta)
        publisher = clean(first.get("publisher", "")) or clean(item_publisher(meta))
        developer = ", ".join(clean(d) for d in first.get("developer", "").split("|") if d.strip())
        name = title + (f" ({year})" if year else "") + (f"({publisher})" if publisher else "")
        # A title published on 5.25" and on 3.5" is two items; archive.org
        # tells them apart as "800K", and so does the name.
        if all(m.get("_disk_type") == 2 for m in metas) and "800k" not in name.lower():
            name += "(800K)"
        roms.sort()
        # archive.org's "00playable.woz", the copy its page offers on its own,
        # is sometimes not byte-identical to any disk of the zip: list it as
        # an alternate ("[a]", TOSEC's flag) of the title's first disk.
        for f in meta.get("files", []):
            crc = (f.get("crc32") or "").upper().zfill(8)
            if f["name"] != "00playable.woz" or not f.get("crc32") or crc in seen or crc in known or crc in zipped:
                continue
            seen.add(crc)
            roms.append(("00playable.woz", int(f["size"]), crc, f["md5"].upper(), f["sha1"].upper(), "", "[a]"))
        games.append((name, year, publisher, developer, ident, roms, title))

    # Any name still shared by two items gets the item's own title.
    counts = {}
    for g in games:
        counts[g[0]] = counts.get(g[0], 0) + 1
    games = [(f"{g[0]} [{g[4].removeprefix('wozaday_')}]" if counts[g[0]] > 1 else g[0],) + g[1:]
             for g in games]

    entries = []
    for name, year, publisher, developer, ident, roms, title in games:
        disks = [r for r in roms if not r[6]]
        suffixes = [disk_suffix(r[0], r[5], title) if len(disks) > 1 else "" for r in disks]
        for i, sfx in enumerate(suffixes):
            if sfx and suffixes.count(sfx) > 1:
                suffixes[i] = f"{sfx}(#{i + 1})"
        for rom, sfx in zip(disks, suffixes):
            entries.append((name + sfx, year, publisher, developer, rom[:5]))
        for rom in roms:
            if rom[6]:
                entries.append((name + (suffixes[0] if suffixes else "") + rom[6], year, publisher, developer, rom[:5]))
    entries += alternates
    entries.sort(key=lambda e: e[0].lower())
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("clrmamepro (\n\tname \"Apple - II\"\n\tdescription \"Apple - II - woz-a-day\"\n")
        f.write(f"\tversion \"{datetime.date.today():%Y.%m.%d}\"\n")
        f.write("\tcomment \"4am's woz-a-day collection, the disks TOSEC does not list\"\n")
        f.write("\thomepage \"https://archive.org/details/wozaday\"\n)\n\n")
        for name, year, publisher, developer, (rom, size, crc, md5, sha1) in entries:
            f.write(f"game (\n\tname \"{name}\"\n\tdescription \"{name}\"\n")
            if year:
                f.write(f"\treleaseyear \"{year}\"\n")
            if publisher:
                f.write(f"\tpublisher \"{publisher}\"\n")
            if developer:
                f.write(f"\tdeveloper \"{developer}\"\n")
            f.write(f"\trom ( name \"{clean(rom)}\" size {size} crc {crc} md5 {md5} sha1 {sha1} )\n")
            f.write(")\n\n")
    print(f"{out}: {len(games)} titles, {len(entries)} disks; left out {skipped_tosec} TOSEC disks, "
          f"{skipped_gs} IIgs-only disks", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fetch", action="store_true", help="download item metadata and disk zips")
    ap.add_argument("--tosec", help="libretro-database's metadat/tosec/Apple - II.dat")
    ap.add_argument("--out", default="Apple - II.dat")
    args = ap.parse_args()
    if args.fetch:
        fetch_metadata()
        fetch_zips()
    if args.tosec:
        build(args.tosec, args.out)
    elif not args.fetch:
        ap.error("give --fetch and/or --tosec")


if __name__ == "__main__":
    main()
