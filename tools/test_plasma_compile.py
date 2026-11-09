#!/usr/bin/env python3
"""Optional guest-hosted PLASMA compile/run test; no automatic downloads."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHA256 = "0cdbf8eb863d5e19262ca0fc2715b879cda747ff4fddb164cd8e1125ef1fbb1a"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="pristine PLEIADES-2.21.po")
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--frames", type=Path, required=True)
    args = parser.parse_args()
    if hashlib.sha256(args.image.read_bytes()).hexdigest() != SHA256:
        parser.error("image SHA-256 mismatch; use the pinned upstream image")
    build = args.build.resolve()
    corpus = ROOT / "tests/corpus"
    with tempfile.TemporaryDirectory(prefix="pom2-plasma-") as temp:
        work = Path(temp)
        image = work / "test.po"
        shutil.copyfile(args.image, image)
        # The upstream image has no POMTEST module: only source is inserted.
        # Compilation and module creation must happen inside the emulated IIe.
        subprocess.run([str(build / "pom2_prodos"), "-t", "TXT", str(image),
                        "put", str(corpus / "plasma-smoke.pla"),
                        "BLD/POMTEST.PLA"], check=True)
        manifest = work / "test.manifest"
        manifest.write_text('''title plasma-compile iie plasma
  hdd 1 test.po
  boot 7
  run 5
  type "P /PLEIADES/BLD\\r"
  run 2
  type "+PLASM POMTEST.PLA\\r"
  run 60
  check compiled
  type "+POMTEST\\r"
  run 5
  check result
''')
        return subprocess.run([
            str(build / "pom2_corpus"), "--manifest", str(manifest),
            "--root", str(work), "--expected", str(corpus / "plasma-compile.expected"),
            "--frames", str(args.frames.resolve()),
        ], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
