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
#                 cached file that is not a zip is replaced, and so is a
#                 cached zip that fails the manifest (downloaded once more).
#                 Every file is SHA-256-checked whatever the zip's origin.
#   --self-test   offline check of the cache logic (ctest fetch_test_media)
#   $POM2_TEST_MEDIA_URL overrides the default URL, as --url does.

import argparse
import hashlib
import io
import os
import sys
import time
import urllib.request
import zipfile
import zlib

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


def extract(blob, want, root):
    """Write every listed file the tree lacks. Returns (written, kept,
    problems); a problem that does not end in "(kept)" is the archive's."""
    written = kept = 0
    problems = []
    try:
        z = zipfile.ZipFile(io.BytesIO(blob))
    except zipfile.BadZipFile as e:
        return 0, 0, [f"the archive is not a readable zip ({e})"]
    with z:
        members = {i.filename: i for i in z.infolist() if not i.is_dir()}
        for rel, digest in sorted(want.items()):
            dst = os.path.join(root, *rel.split("/"))
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
            try:
                data = z.read(members[rel])
            except (zipfile.BadZipFile, zlib.error, EOFError) as e:
                problems.append(f"{rel}: unreadable in the archive ({e})")
                continue
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
    return written, kept, problems


def archive_problems(problems):
    return [p for p in problems if not p.endswith("(kept)")]


def store_cache(path, blob):
    # Kept for the next run; atomic, so a killed job never leaves a
    # half-written file under the cache's name.
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".fetchtmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, path)


def fetch(args, want, fetcher=download):
    blob = None
    from_cache = False
    if args.zip:
        with open(args.zip, "rb") as f:
            blob = f.read()
    elif args.cache and os.path.exists(args.cache):
        with open(args.cache, "rb") as f:
            blob = f.read()
        if zipfile.is_zipfile(io.BytesIO(blob)):
            print(f"fetch_test_media: using the cached archive {args.cache}")
            from_cache = True
        else:
            print(f"fetch_test_media: {args.cache} is not a zip — "
                  "downloading again", file=sys.stderr)
            blob = None
    if blob is None:
        print(f"fetch_test_media: downloading {args.url}")
        blob = fetcher(args.url)
        if args.cache:
            store_cache(args.cache, blob)

    written, kept, problems = extract(blob, want, args.root)
    # A cached zip is trusted no further than a download (PKG-4, bug hunt
    # 2026-11-01): one that fails the manifest — a truncated-but-valid zip,
    # a member that will not inflate — used to be restored by every later
    # job, all failing alike, while the log said "using the cached archive".
    # Download once more and replace it. A file the archive really lacks
    # fails again after the download, as it should.
    if from_cache and archive_problems(problems):
        print(f"fetch_test_media: the cached archive fails the manifest "
              f"({archive_problems(problems)[0]}) — downloading again",
              file=sys.stderr)
        blob = fetcher(args.url)
        store_cache(args.cache, blob)
        more, kept, problems = extract(blob, want, args.root)
        written += more
    return written, kept, problems


def self_test():
    """Offline: a poisoned cache is replaced, a good one is not re-fetched."""
    import tempfile
    import types
    good = io.BytesIO()
    with zipfile.ZipFile(good, "w") as z:
        z.writestr("a/x.bin", b"payload")
    bad = io.BytesIO()
    with zipfile.ZipFile(bad, "w") as z:
        z.writestr("a/y.bin", b"other")          # x.bin missing
    want = {"a/x.bin": sha256(b"payload")}
    calls = []

    def fetcher(url):
        calls.append(url)
        return good.getvalue()

    for cached, expect_calls in ((bad.getvalue(), 1), (good.getvalue(), 0)):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "cache.zip")
            with open(cache, "wb") as f:
                f.write(cached)
            calls.clear()
            args = types.SimpleNamespace(zip=None, cache=cache, url="u",
                                         root=os.path.join(d, "tree"))
            written, _, problems = fetch(args, want, fetcher)
            assert len(calls) == expect_calls, calls
            assert written == 1 and not problems, problems
            with open(cache, "rb") as f:
                assert f.read() == good.getvalue(), "cache not replaced"
    print("fetch_test_media: self-test OK")
    return 0


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
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    want = read_manifest(args.manifest)
    written, kept, problems = fetch(args, want)

    print(f"fetch_test_media: {written} written, {kept} already present, "
          f"{len(problems)} problem(s), {len(want)} listed")
    for p in problems:
        print(f"  {p}", file=sys.stderr)
    if problems and (args.strict or archive_problems(problems)):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
