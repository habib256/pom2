#!/usr/bin/env python3
"""Fetch the boot disk of every title in 4am's woz-a-day collection.

The collection is archive.org's `wozaday`: one item per title, its playable
boot disk named `00playable.woz`, or for a few only in the item's zip, where
the first WOZ by name is taken (a few titles also have `00playable2.woz`,
and for some `00playable.woz` is 4am's Anti-M loader rather than the game:
docs/audits/protected-woz-three-way-2026-11-23.md). Only `00playable.woz` is
fetched, one per title, into DEST with a manifest (identifier, title, size,
SHA-256), so a run can be reproduced against the same bytes. Nothing lands
in the checkout: these disks are not POM2's to ship (CLAUDE.md).

Usage: tools/woz_corpus_fetch.py DEST [--limit N] [--jobs 4]
Standard library only; already-fetched files are kept and re-hashed.
"""
import argparse
import concurrent.futures
import hashlib
import io
import json
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

AGENT = {"User-Agent": "pom2-woz-corpus/1.0"}
SEARCH = "https://archive.org/advancedsearch.php"


def get(url, attempts=4):
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=60) as r:
                return r.read()
        except Exception:
            if attempt + 1 == attempts:
                raise
            time.sleep(2 ** attempt)


def identifiers():
    items, page = [], 1
    while True:
        query = urllib.parse.urlencode({"q": "collection:wozaday", "fl[]": ["identifier", "title"],
                                        "rows": 1000, "page": page, "output": "json",
                                        "sort[]": "identifier asc"}, doseq=True)
        docs = json.loads(get(f"{SEARCH}?{query}"))["response"]["docs"]
        if not docs:
            return items
        items += [(d["identifier"], d.get("title", d["identifier"])) for d in docs]
        page += 1


def fetch(dest, identifier, title):
    target = dest / f"{identifier}.woz"
    if not target.exists():
        files = json.loads(get(f"https://archive.org/metadata/{identifier}/files")).get("result", [])
        names = [f["name"] for f in files]
        if "00playable.woz" in names:
            data = get(f"https://archive.org/download/{identifier}/00playable.woz")
        else:
            # Some titles keep their disks only in the item's own zip (not the
            # "extras" one): its first WOZ by name, side A as a rule.
            zips = sorted(n for n in names if n.lower().endswith(".zip") and "extras" not in n.lower())
            data = b""
            if zips:
                with zipfile.ZipFile(io.BytesIO(get(
                        f"https://archive.org/download/{identifier}/{urllib.parse.quote(zips[0])}"))) as z:
                    wozzes = sorted(n for n in z.namelist() if n.lower().endswith(".woz"))
                    if wozzes:
                        data = z.read(wozzes[0])
            if not data:
                return {"identifier": identifier, "title": title, "error": "no WOZ in the item"}
        if data[:4] not in (b"WOZ1", b"WOZ2"):
            return {"identifier": identifier, "title": title, "error": "not a WOZ"}
        tmp = target.with_suffix(".part")
        tmp.write_bytes(data)
        tmp.rename(target)
    data = target.read_bytes()
    return {"identifier": identifier, "title": title, "file": target.name, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dest", type=Path)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    args.dest.mkdir(parents=True, exist_ok=True)
    items = identifiers()
    if args.limit:
        items = items[:args.limit]
    print(f"{len(items)} titles in wozaday", flush=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(fetch, args.dest, i, t): i for i, t in items}
        for n, future in enumerate(concurrent.futures.as_completed(futures), 1):
            try:
                results.append(future.result())
            except Exception as error:   # one title's failure is recorded, not fatal
                results.append({"identifier": futures[future], "error": str(error)})
            if n % 100 == 0:
                print(f"{n}/{len(items)}", flush=True)
    results.sort(key=lambda r: r["identifier"])
    (args.dest / "manifest.json").write_text(json.dumps(results, indent=1) + "\n")
    ok = sum(1 for r in results if "sha256" in r)
    print(f"{ok} disks, {len(results) - ok} without one; manifest in {args.dest / 'manifest.json'}")


if __name__ == "__main__":
    main()
