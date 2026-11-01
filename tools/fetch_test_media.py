#!/usr/bin/env python3
# fetch_test_media.py — put the ROMs and third-party disk images the test
# suite needs back into the tree, from POM2's test-media archive on
# archive.org.
#
# The repository carries no copyrighted firmware and no third-party disk
# image (2026-10-29): roms/ keeps only the MAME floppy samples, the disk
# folders keep the author's own GPL software and French Touch's GPL demos.
# Everything else ctest opens lives in one zip, listed file by file in
# tools/test_media.sha256 — a path the list does not name is never written,
# and a file whose SHA-256 differs from the list is refused.
#
# A file already in the tree is left alone: a developer's working copy may
# hold disks the emulator has written to. --strict (CI) turns a mismatch into
# an error instead of a warning.
#
# Usage: tools/fetch_test_media.py [--strict] [--url URL] [--zip FILE]
#                                  [--cache FILE] [--manifest FILE] [--root DIR]
#   --zip FILE    use a local copy of the archive instead of downloading it
#   --cache FILE  use FILE as the archive when it is one; otherwise download
#                 and keep the download there for the next run (CI keeps it
#                 in an Actions cache keyed on the manifest's hash, so
#                 archive.org — which answers 500/502 for a minute now and
#                 then — is asked once per manifest, not once per job). A
#                 cached file that is not a zip is replaced. Every file is
#                 SHA-256-checked whatever the zip's origin.
#   $POM2_TEST_MEDIA_URL overrides the default URL, as --url does.

import argparse
import hashlib
import io
import os
import sys
import time
import urllib.request
import zipfile

# Item pom2_media_202611 since 2026-11-01: pom2_testmedia_20261029 went dark.
DEFAULT_URL = ("https://archive.org/download/pom2_media_202611/"
               "pom2_media.zip")


def read_manifest(path):
    entries = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            digest, rel = line.split("  ", 1)
            entries[rel] = digest.lower()
    return entries


def download(url):
    last = None
    # Six tries, 5 s more between each: ~2 minutes of patience. The outages
    # seen on 2026-11-01 lasted about a minute and the other jobs of the same
    # run downloaded fine seconds later — four tries (50 s) sat inside them.
    for attempt in range(1, 7):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "pom2-ci"})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except Exception as e:  # network errors, HTTP 5xx from archive.org
            last = e
            print(f"fetch_test_media: attempt {attempt} failed: {e}",
                  file=sys.stderr)
            time.sleep(5 * attempt)
    raise SystemExit(f"fetch_test_media: cannot download {url}: {last}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("POM2_TEST_MEDIA_URL",
                                                    DEFAULT_URL))
    ap.add_argument("--zip")
    ap.add_argument("--cache")
    ap.add_argument("--manifest", default=os.path.join(here, "test_media.sha256"))
    ap.add_argument("--root", default=os.path.dirname(here))
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    want = read_manifest(args.manifest)
    blob = None
    if args.zip:
        with open(args.zip, "rb") as f:
            blob = f.read()
    elif args.cache and os.path.exists(args.cache):
        with open(args.cache, "rb") as f:
            blob = f.read()
        if zipfile.is_zipfile(io.BytesIO(blob)):
            print(f"fetch_test_media: using the cached archive {args.cache}")
        else:
            print(f"fetch_test_media: {args.cache} is not a zip — "
                  "downloading again", file=sys.stderr)
            blob = None
    if blob is None:
        print(f"fetch_test_media: downloading {args.url}")
        blob = download(args.url)
        if args.cache:
            # Kept for the next run; atomic, so a killed job never leaves a
            # half-written file under the cache's name.
            os.makedirs(os.path.dirname(os.path.abspath(args.cache)),
                        exist_ok=True)
            tmp = args.cache + ".fetchtmp"
            with open(tmp, "wb") as f:
                f.write(blob)
            os.replace(tmp, args.cache)

    written = kept = 0
    problems = []
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        members = {i.filename: i for i in z.infolist() if not i.is_dir()}
        for rel, digest in sorted(want.items()):
            dst = os.path.join(args.root, *rel.split("/"))
            if os.path.exists(dst):
                with open(dst, "rb") as f:
                    if sha256(f.read()) != digest:
                        problems.append(f"{rel}: the tree's copy differs from "
                                        "the archive (kept)")
                kept += 1
                continue
            if rel not in members:
                problems.append(f"{rel}: missing from the archive")
                continue
            data = z.read(members[rel])
            if sha256(data) != digest:
                problems.append(f"{rel}: SHA-256 mismatch in the archive "
                                "(not written)")
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + ".fetchtmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, dst)
            written += 1

    print(f"fetch_test_media: {written} written, {kept} already present, "
          f"{len(problems)} problem(s), {len(want)} listed")
    for p in problems:
        print(f"  {p}", file=sys.stderr)
    if problems and (args.strict or any("kept" not in p for p in problems)):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
