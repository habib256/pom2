#!/usr/bin/env python3
"""Package a built POM2 core for installation into a RetroArch directory."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("output", type=Path, help="destination ZIP, named for its platform")
    args = parser.parse_args()
    module = args.module
    if module.name not in ("pom2_libretro.dll", "pom2_libretro.so", "pom2_libretro.dylib",
                           "pom2_libretro_android.so"):
        parser.error("expected a POM2 libretro module")
    files = {f"cores/{module.name}": module, "info/pom2_libretro.info": module.parent / "pom2_libretro.info",
             "doc/pom2/README.md": ROOT / "libretro/README.md", "doc/pom2/LICENSE": ROOT / "LICENSE",
             "doc/pom2/compatibility.md": ROOT / "docs/audits/libretro-compatibility-2026-11-23.md",
             "doc/pom2/libretro.h": ROOT / "src/third_party/libretro/libretro.h"}
    # MAME's floppy samples (BSD) for the "Disk drive sounds" option: the
    # only firmware-folder files a package may carry, as no ROM may ship.
    for sample in sorted((ROOT / "roms/floppy_samples").iterdir()):
        if sample.suffix in (".wav", ".txt"):
            files[f"system/pom2/floppy_samples/{sample.name}"] = sample
    args.output.parent.mkdir(exist_ok=True, parents=True)
    checksums = []
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, source in files.items():
            data = source.read_bytes()
            if source.name == "README.md":
                revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                          capture_output=True, text=True).stdout.strip() or "main"
                base = f"](https://github.com/habib256/pom2/blob/{revision}/"
                guide = data.decode().replace("](../docs/audits/libretro-compatibility-2026-11-23.md)",
                                              "](compatibility.md)")
                data = guide.replace("](../", base).encode()
            checksums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
            archive.writestr(name, data)
        archive.writestr("doc/pom2/SHA256SUMS", "".join(checksums))
    print(hashlib.sha256(args.output.read_bytes()).hexdigest(), args.output)


if __name__ == "__main__":
    main()
