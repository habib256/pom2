#!/usr/bin/env python3
"""Stage pinned, official FujiNet-PC Apple runtimes beside POM2's assets.

POM2 pins the FujiNet-PC Apple binaries from FujiNet v1.5.2.
The platform archives are fetched at package time, so the repository does not
carry opaque executables. Their SHA-256 values are GitHub release digests.
"""

import argparse
import hashlib
import io
from pathlib import Path
import shutil
import tarfile
import tempfile
from urllib.request import urlopen
import zipfile


VERSION = "v1.5.2"
BASE = "https://github.com/FujiNetWIFI/fujinet-firmware/releases/download/" + VERSION
ARCHIVES = {
    "macos-arm64": ("fujinet-pc-APPLE_v1.5.2_macos-14-arm64.tar.gz",
                    "4af4c1577886a4b841c1479a4ef9742ed74116838453e9327f51a8499599a0ca"),
    "macos-x64": ("fujinet-pc-APPLE_v1.5.2_macos-15-x64.tar.gz",
                  "20a3d414a5316b70a6fb49976c44a50d1d7efb55aac19d160402860a40d72cd2"),
    "windows-x64": ("fujinet-pc-APPLE_v1.5.2_windows-x64.zip",
                    "0bc96fc537218a3ea2385d8465e005a4620221c2841271963faf7195c664cda5"),
    "linux-x64": ("fujinet-pc-APPLE_v1.5.2_ubuntu-22.04-amd64.tar.gz",
                  "7351f0e8e8ab334d49aa17863c53fc446118055412a69133ecfa3b673ba0ddbb"),
}
TARGETS = {
    "macos": ("macos-arm64", "macos-x64"),
    "windows": ("windows-x64",),
    "linux-x64": ("linux-x64",),
}


def sanitize_apple_media(data_dir):
    """Remove Apple ROM material that the Apple II CONFIG path never reads.

    Method follows FujiNet Go Apple II's sanitize-apple-media.py: A3A2EMU is
    an Apple /// emulator ROM in the ProDOS boot volumes; blank.do has DOS 3.3
    boot tracks. POM2 uses neither. Keep the FujiNet menu and data intact.
    """
    block = 512
    for name in ("autorun.po", "mount-and-boot.po"):
        path = data_dir / name
        if not path.is_file():
            continue
        image = bytearray(path.read_bytes())
        for directory_block in range(2, 6):
            for slot in range(13):
                offset = directory_block * block + 4 + slot * 0x27
                entry = image[offset:offset + 0x27]
                if not entry or not entry[0]:
                    continue
                length = entry[0] & 0x0F
                storage = entry[0] >> 4
                if entry[1:1 + length] != b"A3A2EMU":
                    continue
                key = entry[0x11] | (entry[0x12] << 8)
                blocks = [key]
                if storage == 2:
                    index = image[key * block:(key + 1) * block]
                    blocks += [index[i] | (index[256 + i] << 8)
                               for i in range(256) if index[i] or index[256 + i]]
                elif storage != 1:
                    raise ValueError("unsupported A3A2EMU ProDOS storage type")
                for n in blocks:
                    image[n * block:(n + 1) * block] = bytes(block)
                image[offset:offset + 0x27] = bytes(0x27)
                header = 2 * block + 4
                count = image[header + 0x21] | (image[header + 0x22] << 8)
                if count:
                    count -= 1
                    image[header + 0x21] = count & 0xFF
                    image[header + 0x22] = count >> 8
                path.write_bytes(image)
                print("Removed Apple /// ROM from", path, flush=True)
                break
    blank = data_dir / "blank.do"
    if blank.is_file():
        image = bytearray(blank.read_bytes())
        image[:3 * 16 * 256] = bytes(3 * 16 * 256)
        blank.write_bytes(image)


def contains_prodos_file(path, victim):
    image = path.read_bytes()
    for directory_block in range(2, 6):
        for slot in range(13):
            offset = directory_block * 512 + 4 + slot * 0x27
            entry = image[offset:offset + 0x27]
            if entry and entry[0] and entry[1:1 + (entry[0] & 0x0F)] == victim:
                return True
    return False


def safe_files(blob, name):
    """Yield (relative name, bytes, mode) without trusting archive paths."""
    if name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            for item in archive.infolist():
                if item.is_dir():
                    continue
                path = Path(item.filename)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError("unsafe FujiNet archive path: " + item.filename)
                if path.parts[0] != "fujinet-pc-APPLE":
                    raise ValueError("unexpected FujiNet archive root: " + item.filename)
                yield Path(*path.parts[1:]), archive.read(item), 0o755 if path.name.endswith(".exe") else 0o644
    else:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
            for item in archive:
                if item.isdir():
                    continue
                path = Path(item.name)
                if not item.isfile() or path.is_absolute() or ".." in path.parts:
                    raise ValueError("unsafe FujiNet archive member: " + item.name)
                if path.parts[0] != "fujinet-pc-APPLE":
                    raise ValueError("unexpected FujiNet archive root: " + item.name)
                yield Path(*path.parts[1:]), archive.extractfile(item).read(), item.mode & 0o777


def stage_one(tag, root, archive_dir=None):
    name, digest = ARCHIVES[tag]
    source = BASE + "/" + name
    local = archive_dir / name if archive_dir else None
    if local and local.is_file():
        print("Using", local, flush=True)
        blob = local.read_bytes()
    else:
        print("Fetching", source, flush=True)
        with urlopen(source, timeout=90) as response:
            blob = response.read()
    actual = hashlib.sha256(blob).hexdigest()
    if actual != digest:
        raise ValueError("FujiNet archive SHA-256 mismatch for " + name + ": " + actual)
    if local and not local.exists():
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(blob)
    dest = root / "fujinet-pc-APPLE" / tag
    with tempfile.TemporaryDirectory(prefix="pom2-fujinet-") as temp:
        staged = Path(temp) / tag
        for rel, data, mode in safe_files(blob, name):
            output = staged / rel
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
            output.chmod(mode)
        exe = staged / ("fujinet.exe" if tag == "windows-x64" else "fujinet")
        for required in (exe, staged / "fnconfig.ini", staged / "data" / "autorun.po"):
            if not required.is_file():
                raise ValueError("FujiNet archive missing " + str(required))
        sanitize_apple_media(staged / "data")
        if dest.exists():
            shutil.rmtree(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(staged, dest)
    print("Staged", dest, flush=True)


def verify_one(tag, root):
    dest = root / "fujinet-pc-APPLE" / tag
    exe = dest / ("fujinet.exe" if tag == "windows-x64" else "fujinet")
    for required in (exe, dest / "fnconfig.ini", dest / "data" / "autorun.po"):
        if not required.is_file() or not required.stat().st_size:
            raise ValueError("FujiNet package missing " + str(required))
    for name in ("autorun.po", "mount-and-boot.po"):
        image = dest / "data" / name
        if image.is_file() and contains_prodos_file(image, b"A3A2EMU"):
            raise ValueError("unsanitized Apple /// ROM in " + str(image))
    blank = dest / "data" / "blank.do"
    if blank.is_file() and any(blank.read_bytes()[:3 * 16 * 256]):
        raise ValueError("unsanitized DOS boot tracks in " + str(blank))
    print("Verified", dest, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS, required=True)
    parser.add_argument("--dest", type=Path, required=True)
    parser.add_argument("--archive-dir", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    for tag in TARGETS[args.target]:
        if args.verify:
            verify_one(tag, args.dest)
        else:
            stage_one(tag, args.dest, args.archive_dir)
    notice = args.dest / "fujinet-pc-APPLE" / "NOTICE.txt"
    license_file = args.dest / "fujinet-pc-APPLE" / "LICENSE-GPL3.txt"
    if args.verify:
        if not notice.is_file() or not license_file.is_file():
            raise ValueError("FujiNet source and license notices are missing")
    else:
        notice.write_text(
            "FujiNet-PC Apple " + VERSION + " by FujiNetWIFI.\n"
            "Source: https://github.com/FujiNetWIFI/fujinet-firmware/tree/" + VERSION + "\n"
            "Release: " + BASE + "\n"
            "License: GPL-3.0; see LICENSE-GPL3.txt.\n"
            "The Apple /// A3A2EMU ROM and DOS 3.3 boot tracks were removed "
            "from bundled media.\n", encoding="utf-8")
        shutil.copyfile(Path(__file__).resolve().parents[2] / "LICENSE", license_file)


if __name__ == "__main__":
    main()
