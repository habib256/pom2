#!/usr/bin/env python3
"""Compare Apple II cores through a common, headless Libretro frontend."""

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageChops

from libretro_frontend import PAD, Frontend, prepare_system

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def revision(source):
    try:
        result = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                                check=False, capture_output=True, text=True)
    except OSError:
        return None
    return result.stdout.strip() or None


def firmware_info(roms, applewin):
    info = {"pom2_files": {leaf: sha256(roms / leaf) for leaf in
                         ("apple2e.rom", "apple2e_char.rom", "disk2.rom", "diskii_p6.rom")
                         if (roms / leaf).is_file()}}
    rom = roms / "apple2e.rom"
    if rom.is_file():
        info["pom2_effective_system_rom"] = hashlib.sha256(rom.read_bytes()[-16384:]).hexdigest()
    if applewin:
        applewin_roms = {leaf: sha256(applewin / "resource" / leaf) for leaf in
                         ("Apple2e_Enhanced.rom", "Apple2e_Enhanced_Video.rom", "DISK2.rom")}
        info["applewin_resources"] = applewin_roms
        info["applewin_source_revision"] = revision(applewin)
        info["matching_roms"] = {
            "system": info.get("pom2_effective_system_rom") == applewin_roms["Apple2e_Enhanced.rom"],
            "characters": info["pom2_files"].get("apple2e_char.rom") == applewin_roms["Apple2e_Enhanced_Video.rom"],
            "disk_ii_boot": info["pom2_files"].get("disk2.rom") == applewin_roms["DISK2.rom"],
        }
    return info


def default_options(core):
    if core.startswith("pom2"):
        return {"pom2_machine_profile": "ntsc",
                "pom2_fast_disk_loading": "enabled" if core == "pom2-fast" else "disabled"}
    return {"applewin_machine": "Apple //e (enhanced)", "applewin_video_refresh_rate": "60Hz",
            "applewin_video_style": "280 x 192", "applewin_video_mode": "Color (RGB Card/Monitor)",
            "applewin_slot3": "Empty", "applewin_slot4": "Empty", "applewin_slot5": "Empty",
            "applewin_slot7": "Empty", "applewin_keyboard_type": "ASCII",
            "applewin_disk_control_drive": "Drive 1", "applewin_floppy_multidrive": "disabled",
            "applewin_floppy_write_protect": "enabled"}


# Witness coordinates are in the Apple II active area at 280 x 192, not in
# the core's output resolution. A witness is about the guest screen layout,
# so cores can disagree on color and subpixel rendering without failing it.
class Observation:
    def __init__(self, rgb):
        self.mask = rgb.resize((280, 192), Image.Resampling.BOX).convert("L").point(
            [0] * 64 + [255] * 192)

    def lit(self, box):
        return self.mask.crop(box).histogram()[255]

    def glyph(self, column, row):
        bits = self.mask.crop((column * 7, row * 8, column * 7 + 7, row * 8 + 8)).tobytes()
        return tuple(sum((bits[line * 7 + bit] != 0) << bit for bit in range(7))
                     for line in range(8))

    def difference(self, other, box=(0, 0, 280, 192)):
        return ImageChops.difference(self.mask.crop(box), other.mask.crop(box)).histogram()[255]


PROMPT = (62, 48, 48, 48, 48, 48, 62, 0)
ANSWER = ((28, 34, 32, 24, 4, 2, 62, 0), (62, 32, 16, 24, 32, 34, 28, 0),
          (16, 24, 20, 18, 62, 16, 16, 0), (62, 2, 30, 32, 32, 34, 28, 0))
SOKO_BOARD = (80, 92, 180, 147)


def witnesses():
    return {
        "applesoft-prompt": lambda im: im.glyph(0, 2) == PROMPT,
        "answer-2345": lambda im: any(
            tuple(im.glyph(column, row) for column in range(4)) == ANSWER for row in range(3, 10)),
        "dirt-title": lambda im: (4900 < im.lit((10, 12, 270, 45)) < 5700 and
                                  1700 < im.lit((155, 70, 245, 145)) < 2300),
        "dirt-menu": lambda im: (im.lit((0, 32, 110, 144)) > 2000 and
                                 im.lit((140, 32, 245, 144)) > 1900 and
                                 im.lit((0, 165, 280, 175)) < 700),
        "dirt-start": lambda im: (im.lit((0, 165, 280, 175)) > 2500 and
                                  im.lit((35, 115, 82, 150)) > 35 and
                                  im.lit((85, 115, 150, 150)) < 10),
        "dirt-moved": lambda im: (im.lit((0, 165, 280, 175)) > 2500 and
                                  im.lit((85, 115, 150, 150)) > 35),
        "soko-title": lambda im: (im.lit((0, 0, 16, 192)) > 1600 and
                                  im.lit((70, 10, 207, 30)) > 1100 and
                                  im.lit((70, 77, 205, 155)) > 2200),
        "soko-help": lambda im: (im.lit((0, 0, 16, 192)) > 1600 and
                                 270 < im.lit((70, 10, 207, 30)) < 470 and
                                 1000 < im.lit((70, 77, 205, 155)) < 1350),
        "soko-tutorial": lambda im: (im.lit((0, 0, 16, 192)) < 550 and
                                     im.lit(SOKO_BOARD) > 1300 and
                                     im.lit((0, 0, 60, 16)) > 150),
        "chroma-title": lambda im: (im.lit((0, 0, 280, 60)) > 5500 and
                                    3500 < im.lit((0, 26, 280, 100)) < 4600),
        "chroma-level": lambda im: (im.lit((0, 26, 280, 100)) > 5800 and
                                    im.lit((50, 177, 160, 191)) > 100 and
                                    im.lit((192, 177, 270, 191)) < 40),
        "chroma-paddle-right": lambda im: (im.lit((0, 26, 280, 100)) > 5400 and
                                           im.lit((192, 177, 270, 191)) > 100),
        "file-list": lambda im: (im.lit((0, 0, 280, 8)) > 900 and
                                 im.lit((0, 8, 280, 56)) > 1600 and
                                 im.lit((0, 60, 280, 150)) < 100 and
                                 im.lit((0, 174, 280, 192)) > 950),
        "file-help": lambda im: (im.lit((0, 0, 280, 8)) > 1500 and
                                 im.lit((0, 60, 280, 150)) > 2500 and
                                 im.lit((0, 174, 280, 192)) < 550),
        "hectic-loader": lambda im: (580 < im.lit((0, 20, 105, 170)) < 950 and
                                      im.lit((185, 20, 280, 170)) < 180 and
                                      1200 < im.lit((0, 0, 280, 192)) < 2200),
        "hectic-raster": lambda im: (im.lit((0, 20, 105, 170)) > 9000 and
                                      im.lit((185, 20, 280, 170)) > 200),
        "midline-loader": lambda im: (1200 < im.lit((0, 0, 280, 192)) < 1900 and
                                       im.lit((0, 135, 280, 185)) < 100),
        "midline-filled-screen": lambda im: (im.lit((0, 135, 280, 185)) > 1800 and
                                              im.lit((0, 0, 280, 192)) > 7500),
        "lemm-intro": lambda im: (1000 < im.lit((0, 0, 280, 42)) < 2200 and
                                   600 < im.lit((0, 60, 280, 135)) < 1500 and
                                   190 < im.lit((0, 148, 280, 186)) < 600),
        "lemm-level": lambda im: (im.lit((0, 0, 280, 42)) > 3600 and
                                   im.lit((0, 60, 280, 135)) > 5500 and
                                   im.lit((0, 148, 280, 186)) > 2000),
        "monster-loader": lambda im: (430 < im.lit((0, 12, 140, 70)) < 700 and
                                       410 < im.lit((140, 12, 280, 70)) < 660 and
                                       50 < im.lit((80, 96, 200, 111)) < 200 and
                                       800 < im.lit((0, 160, 280, 184)) < 1300),
        "monster-logo": lambda im: (im.lit((0, 12, 280, 70)) > 5500 and
                                     im.lit((70, 85, 215, 170)) > 2200 and
                                     im.lit((0, 160, 280, 184)) > 2100 and
                                     im.lit((0, 70, 280, 90)) < 50),
        "heartbeat-loader": lambda im: (60 < im.lit((0, 125, 280, 190)) < 220 and
                                         260 < im.lit((95, 45, 185, 145)) < 550 and
                                         1700 < im.lit((0, 0, 280, 192)) < 3500),
        "heartbeat-moving-raster": lambda im: (im.lit((0, 125, 280, 190)) > 4000 and
                                                im.lit((95, 45, 185, 145)) > 2600),
    }


DIRT = "packaging/disks/DIRTBIKE.dsk"
SOKO = "packaging/disks/MICRO-SOKOBAN.dsk"
DEATER = "disks_5.4/demo/deater/"
DIRT_MENU = [("wait", "dirt-title", 2200), ("key", "RETURN"), ("wait", "dirt-menu", 240)]
# Every row waits for a named guest screen; key actions alone cannot pass.
# wait windows are expressed in host frames, even when a core accelerates disk I/O.
SCENARIOS = {
    "applesoft-calculate": (None, [("wait", "applesoft-prompt", 720),
                                  ("type", "PRINT 1111+1234"), ("wait", "answer-2345", 120)]),
    "dirtbike-track-menu": (DIRT, DIRT_MENU),
    "dirtbike-race-throttle": (DIRT, DIRT_MENU + [("key", "RETURN"), ("wait", "dirt-start", 540),
                                                ("frames", 140), ("expect", "dirt-start"),
                                                ("repeat-key", "a", 25), ("wait", "dirt-moved", 120)]),
    "sokoban-help": (SOKO, [("wait", "soko-title", 1800), ("key", "h"),
                            ("wait", "soko-help", 420)]),
    "sokoban-move-undo": (SOKO, [("wait", "soko-title", 1800), ("key", "RETURN"),
                                 ("wait", "soko-tutorial", 420), ("save", "board-before-move"),
                                 ("key", "RIGHT"), ("frames", 30),
                                 ("diff", "board-before-move", SOKO_BOARD, 20, 500),
                                 ("key", "u"), ("frames", 30), ("expect", "soko-tutorial"),
                                 ("diff", "board-before-move", SOKO_BOARD, 0, 4)]),
    "chromabreak-paddle": ("packaging/disks/CHROMABREAK.po",
                            [("wait", "chroma-title", 2200), ("key", "SPACE"),
                             ("wait", "chroma-level", 400), ("repeat-key", "RIGHT", 12),
                             ("wait", "chroma-paddle-right", 240)]),
    "a2filecmd-help": ("disks_5.4/A2FILECMD-DOS3.3-1.0.dsk",
                       [("wait", "file-list", 3000), ("key", "?"), ("wait", "file-help", 400),
                        ("key", "ESC"), ("wait", "file-list", 2200)]),
    "hectic-raster": (DEATER + "hectic.dsk", [("wait", "hectic-loader", 800), ("key", "SPACE"),
                                             ("wait", "hectic-raster", 600), ("save", "first-raster"),
                                             ("frames", 90), ("diff", "first-raster", (0, 0, 280, 192), 1000, 53760)]),
    "midline-intro": (DEATER + "midline.dsk", [("wait", "midline-loader", 800), ("key", "SPACE"),
                                               ("wait", "midline-filled-screen", 600)]),
    "lemm-level-one": (DEATER + "lemm.dsk", [("wait", "lemm-intro", 2400), ("key", "RETURN"),
                                             ("wait", "lemm-level", 1000)]),
    "monster-splash-logo": (DEATER + "monster_splash.dsk", [("wait", "monster-loader", 1300),
                                                           ("wait", "monster-logo", 3000)]),
    "heartbeat-animation": (DEATER + "heartbeat.dsk", [("wait", "heartbeat-loader", 800), ("key", "SPACE"),
                                                       ("wait", "heartbeat-moving-raster", 700),
                                                       ("save", "first-heart"), ("frames", 120),
                                                       ("diff", "first-heart", (0, 0, 280, 192), 600, 53760)]),
}


def run_steps(host, steps, result, captures):
    checks = witnesses()
    saved = {}

    def mark(label):
        at = host.frames
        result["checks"].append({"witness": label, "host_frame": at, "seconds": round(at / result["fps"], 4)})
        if captures:
            host.image().save(captures / f"{len(result['checks']):02d}-{label}.png")

    for operation, *args in steps:
        if operation == "wait":
            label, budget = args
            start = host.frames
            matched = False
            while host.frames < start + budget:
                host.frame(min(8, start + budget - host.frames))
                # Require two successive samples and reject startup noise.
                if host.frames >= 48 and checks[label](Observation(host.image())):
                    if matched:
                        mark(label)
                        break
                    matched = True
                else:
                    matched = False
            else:
                raise AssertionError(f"{label} not seen within {host.frames - start} host frames")
        elif operation == "expect":
            label, = args
            if not checks[label](Observation(host.image())):
                raise AssertionError(f"expected screen {label}")
            mark(label)
        elif operation == "save":
            label, = args
            saved[label] = Observation(host.image())
            mark(label)
        elif operation == "diff":
            label, box, minimum, maximum = args
            count = Observation(host.image()).difference(saved[label], box)
            if not minimum <= count <= maximum:
                raise AssertionError(f"difference from {label}: {count} pixels, expected {minimum}–{maximum}")
            mark(f"difference-from-{label}-{count}px")
        elif operation == "frames":
            host.frame(*args)
        elif operation == "key":
            host.key(*args)
        elif operation == "repeat-key":
            key, repeat = args
            for _ in range(repeat):
                host.key(key)
                host.frame(2)
        elif operation == "type":
            command, = args
            for key in command:
                host.key(key)
            host.key("RETURN")
        else:
            raise ValueError(f"unknown benchmark operation: {operation}")


def benchmark(profile, library, names, roms, capture_root):
    options = default_options(profile)
    results = []
    with tempfile.TemporaryDirectory(prefix="pom2-libretro-bench-") as directory:
        root = Path(directory)
        prepare_system(root / "system", roms)
        for name in names:
            source, steps = SCENARIOS[name]
            result = {"id": name, "media": source, "checks": []}
            results.append(result)
            if source and not (ROOT / source).is_file():
                result.update(status="skip", reason="media missing")
                continue
            if any(not (roms / leaf).is_file() for leaf in ("apple2e.rom", "apple2e_char.rom")):
                result.update(status="skip", reason="Apple IIe comparison firmware missing")
                continue
            saves = root / name
            saves.mkdir()
            media = None
            if source:
                media = saves / Path(source).name
                shutil.copyfile(ROOT / source, media)
                result["media_sha256"] = sha256(media)
            captures = capture_root / profile / name if capture_root else None
            if captures:
                captures.mkdir(parents=True, exist_ok=True)
            host = Frontend(library, root / "system", saves, options)
            try:
                result.update(host.load(media))
                result["registered_options"] = sorted(host.option_names & options.keys())
                expected_name = "AppleWin" if profile == "applewin" else "POM2"
                if result["name"] != expected_name:
                    raise RuntimeError(f"{profile} requires {expected_name}, got {result['name']}")
                missing_options = sorted(options.keys() - host.option_names)
                if missing_options:
                    raise RuntimeError("the module did not register requested options: " + ", ".join(missing_options))
                run_steps(host, steps, result, captures)
                result["status"] = "pass"
            except (RuntimeError, AssertionError) as error:
                result.update(status="fail", reason=str(error))
                if captures and host.video:
                    host.image().save(captures / "failure.png")
            finally:
                host.close()
            if media and sha256(media) != result["media_sha256"]:
                result.update(status="fail", reason="core modified the copied input image")
            result.update(total_host_frames=host.frames, audio_frames=host.audio_frames, input_polls=host.polls)
    return {"profile": profile, "module": library.name, "module_sha256": sha256(library),
            "options": options, "scenarios": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", action="append", required=True, metavar="PROFILE=MODULE",
                        help="pom2-original, pom2-fast or applewin (repeat to compare)")
    parser.add_argument("--scenario", action="append", choices=SCENARIOS, help="limit run to named scenarios")
    parser.add_argument("--roms", type=Path, default=ROOT / "roms")
    parser.add_argument("--applewin-source", type=Path, help="verify the comparison build's bundled ROM resources")
    parser.add_argument("--captures", type=Path, help="write ready/verified and failed screen captures as PNG")
    parser.add_argument("--json", type=Path, help="write machine-readable measurements")
    args = parser.parse_args()
    profiles = []
    for target in args.core:
        profile, separator, module = target.partition("=")
        if not separator or profile not in {"pom2-original", "pom2-fast", "applewin"}:
            parser.error("--core expects pom2-original, pom2-fast or applewin followed by =MODULE")
        path = Path(module).resolve()
        if not path.is_file():
            parser.error(f"module not found: {path}")
        if any(used == profile for used, _ in profiles):
            parser.error(f"duplicate profile: {profile}")
        profiles.append((profile, path))
    try:
        firmware = firmware_info(args.roms, args.applewin_source)
    except OSError as error:
        parser.error(str(error))
    data = {"schema": 1, "sample_interval_frames": 8, "pom2_checkout_revision": revision(ROOT),
            "platform": platform.platform(), "firmware": firmware,
            "runs": [benchmark(mode, path, args.scenario or SCENARIOS, args.roms, args.captures)
                     for mode, path in profiles]}
    if args.json:
        args.json.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    for run in data["runs"]:
        print(run["profile"])
        for row in run["scenarios"]:
            timing = ", ".join(f"{check['witness']}={check['host_frame']}f/{check['seconds']:.2f}s"
                               for check in row["checks"])
            print(f"  {row['status'].upper():4} {row['id']}: {row.get('reason', timing)}")
    return 1 if any(row["status"] == "fail" for run in data["runs"] for row in run["scenarios"]) else 0


if __name__ == "__main__":
    sys.exit(main())
