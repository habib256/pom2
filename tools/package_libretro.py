#!/usr/bin/env python3
"""Package a built POM2 core for installation into a RetroArch directory."""
import argparse
import hashlib
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("output", type=Path, help="destination ZIP, named for its platform")
    args = parser.parse_args()
    module = args.module
    if module.name not in ("pom2_libretro.dll", "pom2_libretro.so", "pom2_libretro.dylib"):
        parser.error("expected a POM2 libretro module")
    files = {f"cores/{module.name}": module, "info/pom2_libretro.info": module.with_suffix(".info"),
             "doc/pom2/README.md": ROOT / "libretro/README.md", "doc/pom2/LICENSE": ROOT / "LICENSE",
             "doc/pom2/libretro.h": ROOT / "src/third_party/libretro/libretro.h"}
    args.output.parent.mkdir(exist_ok=True, parents=True)
    checksums = []
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, source in files.items():
            data = source.read_bytes()
            checksums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            archive.writestr(name, data)
        archive.writestr("doc/pom2/SHA256SUMS", "".join(checksums))
    print(hashlib.sha256(args.output.read_bytes()).hexdigest(), args.output)


if __name__ == "__main__":
    main()
