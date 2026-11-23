#!/usr/bin/env python3
"""Boot every local disk image in one or more Apple II libretro cores.

The wide corpus beside tools/libretro_bench.py: where the benchmark scripts
twelve titles to a verified screen, this one boots every image it finds and
records what the guest shows after a fixed time, the same way for every core:

  error    the 40-column text page holds a boot or I/O failure message
  blank    the picture is black
  screen   anything else (a title, a menu, a program running)

A coarse witness, applied identically to each core; a title that boots on one
and fails on the other is the signal. 5.25-inch images run on every core;
3.5-inch and hard disk images only on cores whose profile mounts them.

  python3 tools/libretro_corpus.py --core pom2=build/pom2_libretro.dylib \\
      --core applewin=/path/applewin_libretro.dylib --roots disks_5.4 disks_3.5 hdv \\
      --report /tmp/corpus.md --captures /tmp/corpus
"""

import argparse
import ctypes as C
import hashlib
import shutil
import sys
import tempfile
from pathlib import Path

from libretro_frontend import Frontend, prepare_system
from libretro_bench import default_options

ROOT = Path(__file__).resolve().parents[1]
FLOPPY = {".dsk", ".do", ".nib", ".woz", ".d13"}
ERRORS = ("I/O ERROR", "UNABLE TO", "CHECK DISK", "NO DEVICE", "ERR #", "DISK ERROR",
          "INSERT SYSTEM DISK", "RELOCATION/CONFIGURATION ERROR", "NOT A STARTUP DISK")


def kind(path):
    suffix = path.suffix.lower()
    if suffix in FLOPPY:
        return "5.25"
    size = path.stat().st_size
    if suffix == ".po" and size == 143360:
        return "5.25"
    if suffix == ".hdv" or size > 820000:
        return "hdv"
    return "3.5"


def options_for(profile, extra):
    options = dict(default_options("pom2-fast" if profile.startswith("pom2") else "applewin"))
    if profile.startswith("pom2"):
        options.update(pom2_mockingboard="ac", pom2_mockingboard_slot="4")
    else:
        options.update(applewin_slot4="Mockingboard")
    options.update(extra.get(profile, {}))
    return options


def text_page(host):
    """The 40-column text page as ASCII, from main RAM ($0400-$07FF)."""
    get = host.core.retro_get_memory_data
    get.restype, get.argtypes = C.c_void_p, [C.c_uint]
    address = get(2)   # RETRO_MEMORY_SYSTEM_RAM
    if not address:
        return ""
    ram = (C.c_uint8 * 0x800).from_address(address)
    rows = []
    for row in range(24):
        base = 0x400 + (row % 8) * 0x80 + (row // 8) * 0x28
        line = ""
        for column in range(40):
            code = ram[base + column] & 0x7F
            line += chr(code + 0x40 if code < 0x20 else code)
        rows.append(line.rstrip())
    return "\n".join(rows)


def classify(host):
    text = text_page(host)
    upper = text.upper()
    for needle in ERRORS:
        if needle in upper:
            return "error", needle
    image = host.image().convert("L")
    if image.getextrema()[1] < 16:
        return "blank", ""
    return "screen", ""


def run(profile, library, images, roms, seconds, captures, extra):
    results = {}
    with tempfile.TemporaryDirectory(prefix="pom2-libretro-corpus-") as directory:
        root = Path(directory)
        prepare_system(root / "system", roms)
        for name in ("apple2c-32Kv0.rom", "apple2cp.rom", "apple2p.rom", "apple2_char.rom"):
            if (roms / name).is_file():   # the other machines' firmware, when present
                shutil.copyfile(roms / name, root / "system" / "pom2" / name)
        for image in images:
            if profile == "applewin" and kind(image) != "5.25":
                results[image] = ("skip", "this AppleWin profile mounts 5.25-inch disks only")
                continue
            work = root / hashlib.sha256(str(image).encode()).hexdigest()[:16]
            work.mkdir()
            copy = work / image.name
            shutil.copyfile(image, copy)
            copy.chmod(0o444)
            before = hashlib.sha256(copy.read_bytes()).hexdigest()
            host = Frontend(library, root / "system", work if profile == "applewin" else None,
                            options_for(profile, extra))
            try:
                info = host.load(copy)
                host.frame(int(seconds * info["fps"]))
                results[image] = classify(host)
                if captures:
                    folder = captures / profile
                    folder.mkdir(parents=True, exist_ok=True)
                    host.image().save(folder / (image.stem + ".png"))
            except Exception as error:   # a rejected load is a result, not a crash
                results[image] = ("reject", str(error))
            finally:
                host.close()
            if hashlib.sha256(copy.read_bytes()).hexdigest() != before:
                results[image] = ("fail", "the core modified the read-only copy")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--core", action="append", required=True, metavar="NAME=MODULE")
    parser.add_argument("--roots", nargs="+", default=["disks_5.4", "disks_3.5", "hdv"])
    parser.add_argument("--roms", type=Path, default=ROOT / "roms")
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--captures", type=Path)
    parser.add_argument("--set", action="append", default=[], metavar="NAME:KEY=VALUE",
                        help="a core option for one profile, e.g. pom2-iic:pom2_machine=iic")
    args = parser.parse_args()
    extra = {}
    for item in args.set:
        name, assignment = item.split(":", 1)
        key, value = assignment.split("=", 1)
        extra.setdefault(name, {})[key] = value
    images = sorted(p for r in args.roots for p in (ROOT / r).rglob("*")
                    if p.is_file() and p.suffix.lower() in FLOPPY | {".po", ".2mg", ".hdv"})
    profiles = [item.split("=", 1) for item in args.core]
    table = {name: run(name, Path(module).resolve(), images, args.roms, args.seconds, args.captures, extra)
             for name, module in profiles}
    names = [name for name, _ in profiles]
    lines = [f"| Image | Kind | {' | '.join(names)} |", "|---|---|" + "---|" * len(names)]
    for image in images:
        cells = []
        for name in names:
            verdict, detail = table[name][image]
            cells.append(verdict + (f" ({detail})" if detail and verdict in ("error", "reject", "fail") else ""))
        lines.append(f"| {image.relative_to(ROOT)} | {kind(image)} | {' | '.join(cells)} |")
    lines.append("")
    for name in names:
        counts = {}
        for verdict, _ in table[name].values():
            counts[verdict] = counts.get(verdict, 0) + 1
        lines.append(f"- **{name}**: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    report = "\n".join(lines)
    print(report)
    if args.report:
        args.report.write_text(report + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
