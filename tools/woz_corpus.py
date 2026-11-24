#!/usr/bin/env python3
# POM2 Apple II Emulator
# Copyright (C) 2026 VERHILLE Arnaud
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot a WOZ corpus in POM2, AppleWin and MAME, and publish the pass rates.

The wide counterpart of tools/libretro_corpus.py: every disk of a corpus
fetched by tools/woz_corpus_fetch.py (4am's woz-a-day boot disks) is booted
in each emulator on the same machine, for the same emulated time, and judged
by the same witness:

  error    the 40-column text page holds a boot or I/O failure message
  crash    the Monitor's register line is on the text page (a BRK)
  noboot   the only text is the power-on "Apple //e" / "Apple ][" banner,
           and the picture holds nothing below it
  blank    the picture is black
  screen   anything else: a title, a menu, a program running
  reject   the emulator refused the disk or failed to run
  skip     out of scope: a 3.5-inch or IIgs-only disk (not counted)

"screen" is the pass. It is a coarse witness, the same for all three: what
it measures is agreement, and a title that passes in one emulator and not in
another is the signal to look at.

The machine is the one the WOZ asks for (INFO "compatible hardware"):
enhanced //e when it lists it or says nothing, else unenhanced //e, ][+, ][.
POM2 runs its libretro core with original drive timing, so all three are
judged at the same emulated second,
AppleWin its libretro core (audetto), MAME its apple2ee/apple2e/apple2p/apple2
driver; no card beside the Disk II in any of them. Each run is its own
process, so one emulator's crash costs one cell.

  python3 tools/woz_corpus.py --corpus ~/.cache/pom2-wozaday --out /tmp/woz \\
      --pom2 build/pom2_libretro.dylib --applewin .../applewin_libretro.dylib \\
      --mame /opt/homebrew/bin/mame --mame-roms ~/.cache/pom2-mame/roms
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ERRORS = ("I/O ERROR", "UNABLE TO", "CHECK DISK", "NO DEVICE", "ERR #", "DISK ERROR",
          "INSERT SYSTEM DISK", "RELOCATION/CONFIGURATION ERROR", "NOT A STARTUP DISK",
          "LANGUAGE NOT AVAILABLE")
MONITOR = re.compile(r"A=[0-9A-F]{2} X=[0-9A-F]{2} Y=[0-9A-F]{2} P=[0-9A-F]{2} S=[0-9A-F]{2}")
BANNER = re.compile(r"^\s*APPLE\s*(//E|\]\[|II|//C)\s*$")
MACHINES = {   # name: (POM2 option, AppleWin option, MAME driver)
    "iie-enhanced": ("iie-enhanced", "Apple //e (enhanced)", "apple2ee"),
    "iie": ("iie", "Apple //e (original)", "apple2e"),
    "iiplus": ("iiplus", "Apple II Plus", "apple2p"),
    "ii": ("ii", "Apple II (original)", "apple2"),
}


def out_of_scope(woz):
    """[B] WOZ INFO +1 disk type 2 is a 3.5" disk; compatible hardware with
    only bit 5 set is a IIgs title. Neither runs on the 8-bit machines
    compared here."""
    data = woz.read_bytes()[:80]
    if data[12:16] != b"INFO":
        return ""
    if data[21] == 2:
        return "3.5-inch disk"
    if data[:4] == b"WOZ2" and data[20] >= 2 and (data[60] | (data[61] << 8)) == 0x20:
        return "Apple IIgs only"
    return ""


# [B] WOZ 2.1 reference, META chunk: tab-separated key / value lines;
# requires_machine lists 2|2+|2e|2c|2e+|2gs|2c+|3|3+.
META_MACHINE_BITS = {"2": 0x01, "2+": 0x02, "2e": 0x04, "2c": 0x08, "2e+": 0x10,
                     "2gs": 0x20, "2c+": 0x40, "3": 0x80, "3+": 0x100}


def woz_meta(woz):
    """The META chunk's fields, {} when there is none."""
    data = woz.read_bytes()
    at, meta = 12, {}
    while at + 8 <= len(data):
        size = int.from_bytes(data[at + 4:at + 8], "little")
        if data[at:at + 4] == b"META":
            for line in data[at + 8:at + 8 + size].decode("utf-8", "replace").splitlines():
                key, _, value = line.partition("\t")
                meta[key] = value
            break
        at += 8 + size
    return meta


def compatible_hardware(woz):
    """INFO +40 (WOZ 2, INFO version 2), else META requires_machine: a WOZ
    written with INFO version 1 names its machines only there (Genetic
    Drift, Pest Patrol: ][ and ][+)."""
    data = woz.read_bytes()[:80]
    if data[:4] == b"WOZ2" and data[12:16] == b"INFO" and data[20] >= 2:
        bits = data[60] | (data[61] << 8)
        if bits:
            return bits
    bits = 0
    for name in woz_meta(woz).get("requires_machine", "").split("|"):
        bits |= META_MACHINE_BITS.get(name.strip(), 0)
    return bits


def machine_for(woz):
    """[B] WOZ 2 INFO +40 (or META): compatible hardware, bit 0 ][, 1 ][+,
    2 //e, 3 //c, 4 //e Enhanced. Nothing named: the enhanced //e."""
    data = woz.read_bytes()[:80]
    bits = compatible_hardware(woz)
    # INFO +38, the boot sector format: 2 = 13-sector, 3 = both. A DOS 3.2
    # disk that runs on the original ][ wants it: its programs are mostly
    # Integer BASIC, which a ][+ only has on a language card.
    if bits & 1 and data[20] >= 2 and data[58] in (2, 3):
        return "ii"
    for bit, name in ((4, "iie-enhanced"), (2, "iie"), (1, "iiplus"), (0, "ii")):
        if bits & (1 << bit):
            return name
    return "iie-enhanced"


def is13Sector(woz):
    """[B] WOZ 2 INFO +38 (boot sector format) is 2: 13-sector only."""
    data = woz.read_bytes()[:80]
    return data[:4] == b"WOZ2" and data[12:16] == b"INFO" and data[20] >= 2 and data[58] == 2


def text_rows(page):
    """The 40-column page ($0400-$07FF) as 24 rows of ASCII."""
    rows = []
    for row in range(24):
        base = (row % 8) * 0x80 + (row // 8) * 0x28
        rows.append("".join(chr((b & 0x7F) + (0x40 if (b & 0x7F) < 0x20 else 0))
                            for b in page[base:base + 40]))
    return rows


def text_picture(image):
    """True when something is lit and under 5 % of it is coloured: a text
    screen. A black picture is not one, however stale the text page."""
    small = image.convert("RGB").resize((280, 192))
    lit = coloured = 0
    for r, g, b in small.getdata():
        if max(r, g, b) < 48:
            continue
        lit += 1
        if max(r, g, b) - min(r, g, b) > 64:
            coloured += 1
    return lit > 0 and coloured * 20 < lit   # black is not a message: "blank"


def judge(page, image):
    """The verdict from the text page (1 KiB) and the picture (PIL image)."""
    rows = text_rows(page)
    upper = "\n".join(rows).upper()
    # The text page is read from memory whatever is on screen: a game in
    # graphics mode can leave a loader's old message there (Lock 'N' Chase
    # plays over a stale "UNABLE TO"). A message counts only when the
    # picture is a text screen, light on black with no colour to speak of.
    if text_picture(image):
        for needle in ERRORS:
            if needle in upper:
                return "error", needle
        if MONITOR.search(upper):
            return "crash", MONITOR.search(upper).group(0)
    grey = image.convert("L").resize((280, 192))
    if grey.getextrema()[1] < 16:
        return "blank", ""
    lines = [r.strip() for r in rows if r.strip()]
    below = grey.crop((0, 16, 280, 192)).getextrema()[1]
    if len(lines) == 1 and BANNER.match(lines[0].upper()) and below < 16:
        return "noboot", lines[0].strip()
    return "screen", ""


# ── One run, in its own process ──────────────────────────────────────────

def run_libretro(core, library, woz, machine, seconds, work, roms):
    sys.path.insert(0, str(ROOT / "tools"))
    from libretro_frontend import Frontend, prepare_system
    from libretro_bench import default_options
    import ctypes as C
    system = work / "system"
    prepare_system(system, roms)
    for name in ("apple2p.rom", "apple2o.rom", "apple2_char.rom", "apple2e_unenh.rom", "apple2e_char_2k.rom"):
        if (roms / name).is_file():
            shutil.copyfile(roms / name, system / "pom2" / name)
    pom2_option, applewin_option, _ = MACHINES[machine]
    if core == "pom2":
        # Original drive timing: fast loading runs more emulated time per host
        # frame, and the witness must look at the same emulated second in
        # every emulator (an attract loop that blanks between scenes caught
        # POM2 at another moment: Cavern Creatures, 2026-11-24).
        options = dict(default_options("pom2-original"), pom2_machine=pom2_option)
    else:
        options = dict(default_options("applewin"), applewin_machine=applewin_option)
    host = Frontend(library, system, work, options)
    try:
        info = host.load(woz)
        host.frame(int(seconds * info["fps"]))
        get = host.core.retro_get_memory_data
        get.restype, get.argtypes = C.c_void_p, [C.c_uint]
        address = get(2)
        page = bytes((C.c_uint8 * 0x800).from_address(address)[0x400:0x800]) if address else bytes(0x400)
        image = host.image()
    finally:
        host.close()
    return page, image


MAME_LUA = """
local out = os.getenv("POM2_TEXT_OUT")
-- The original ][ has no Autostart: its Monitor waits at '*' and the disk
-- is started by hand, as on the real machine.
if os.getenv("POM2_TYPE_BOOT") then
  local typed = false
  pom2_typer = emu.register_frame_done(function()
    if not typed and manager.machine.time.seconds > 2 then
      manager.machine.natkeyboard:post("C600G\\n"); typed = true
    end
  end)
end
-- Both subscriptions are kept: one left unreferenced is collected, and the
-- page was then never written once the frame callback made garbage.
pom2_stop = emu.add_machine_stop_notifier(function()
  local space = manager.machine.devices[":maincpu"].spaces["program"]
  local f = io.open(out, "wb")
  for a = 0x400, 0x7FF do f:write(string.char(space:read_u8(a))) end
  f:close()
end)
"""


def run_mame(binary, rompath, woz, machine, seconds, work):
    from PIL import Image
    driver = MACHINES[machine][2]
    (work / "probe.lua").write_text(MAME_LUA)
    page_file, snaps = work / "page.bin", work / "snap"
    command = [binary, driver, "-rompath", str(rompath), "-flop1", str(woz),
               "-seconds_to_run", str(int(seconds)), "-nothrottle", "-sound", "none", "-video", "none",
               "-snapshot_directory", str(snaps), "-snapname", "shot", "-autoboot_script", str(work / "probe.lua"),
               "-skip_gameinfo", "-nomouse", "-cfg_directory", str(work / "cfg"), "-nvram_directory", str(work / "nv")]
    # No card beside the Disk II, as in the two libretro profiles.
    for slot in ("sl1", "sl2", "sl3", "sl4", "sl5", "sl7"):
        command += [f"-{slot}", ""]
    env = dict(os.environ, POM2_TEXT_OUT=str(page_file))
    if is13Sector(woz):
        # The 13-sector controller, as the two libretro cores switch to by
        # themselves. MAME's diskii13 set lacks the 341-0009 dump: the corpus
        # rompath must carry it as diskii13/341-0009.bin.
        command[2:2] = ["-sl6", "diskiing13"]   # before -flop1, which it serves
    if driver == "apple2":
        env["POM2_TYPE_BOOT"] = "1"
    subprocess.run(command, cwd=work, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, check=False)
    shots = list(snaps.rglob("*.png"))
    if not page_file.exists() or not shots:
        raise RuntimeError("MAME produced no snapshot")
    return page_file.read_bytes(), Image.open(shots[0]).convert("RGB")


def worker(args):
    woz = Path(args.woz)
    with tempfile.TemporaryDirectory(prefix="pom2-woz-") as directory:
        work = Path(directory)
        copy = work / woz.name
        shutil.copyfile(woz, copy)
        copy.chmod(0o444)   # a read-only copy: no emulator writes the corpus
        try:
            if args.emulator == "mame":
                page, image = run_mame(args.binary, Path(args.mame_roms), copy, args.machine, args.seconds, work)
            else:
                page, image = run_libretro(args.emulator, Path(args.binary), copy, args.machine,
                                           args.seconds, work, Path(args.roms))
            verdict, detail = judge(page, image)
            if args.capture:
                Path(args.capture).parent.mkdir(parents=True, exist_ok=True)
                image.save(args.capture)
        except Exception as error:   # a refusal is a result, not a crash of the run
            verdict, detail = "reject", str(error)[:200]
    print(json.dumps({"verdict": verdict, "detail": detail}))


# ── The whole corpus ─────────────────────────────────────────────────────

def one(args, emulator, binary, entry, corpus, out):
    woz = corpus / entry["file"]
    scope = out_of_scope(woz)
    if scope:
        return {"verdict": "skip", "detail": scope, "identifier": entry["identifier"],
                "title": entry.get("title", ""), "emulator": emulator, "machine": "", "sha256": entry["sha256"]}
    machine = machine_for(woz)
    command = [sys.executable, "-B", __file__, "--worker", "--emulator", emulator, "--binary", str(binary),
               "--woz", str(woz), "--machine", machine, "--seconds", str(args.seconds),
               "--roms", str(args.roms), "--mame-roms", str(args.mame_roms or "")]
    if args.captures:
        command += ["--capture", str(out / "captures" / emulator / (entry["identifier"] + ".png"))]
    def run(seconds):
        try:
            seconds_at = command.index("--seconds") + 1
            command[seconds_at] = str(seconds)
            done = subprocess.run(command, capture_output=True, text=True, timeout=900)
            return json.loads(done.stdout.strip().splitlines()[-1])
        except Exception as error:
            return {"verdict": "reject", "detail": f"runner: {error}"[:200]}

    result = run(args.seconds)
    if result["verdict"] in ("blank", "noboot"):
        # One instant can fall in a title's fade or between two screens
        # (Dawn Patrol, Carmen USA: a picture at 30 s and 60 s, black at
        # 45 s), and a long load at the real drive's speed is not over at
        # 45 s (Thexder, Maniac Mansion, Street Sports). Every emulator gets
        # the same looks, 5 s later and at 100 s; only a screen that stays
        # empty is a failure.
        first = result["verdict"]
        for later in (args.seconds + 5, 100):
            if later <= args.seconds:
                continue
            result = run(later)
            if result["verdict"] == "screen":
                result["detail"] = f"{first} at {args.seconds:g} s, a picture at {later:g} s"
                break
    return dict(result, identifier=entry["identifier"], title=entry.get("title", ""), emulator=emulator,
                machine=machine, sha256=entry["sha256"])


def report(results, emulators, out, corpus=None, files=None):
    verdicts = ("screen", "noboot", "blank", "error", "crash", "reject")
    table = {}
    for r in results:
        table.setdefault(r["identifier"], {})[r["emulator"]] = r
    skipped = sorted(ident for ident, row in table.items()
                     if any(r["verdict"] == "skip" for r in row.values()))
    for ident in skipped:
        del table[ident]
    lines = [f"Titles: {len(table)} compared, {len(skipped)} out of scope (3.5-inch or IIgs only). "
             "Pass = \"screen\" (see tools/woz_corpus.py).", "",
             "| Emulator | Pass rate | " + " | ".join(verdicts) + " |",
             "|---|---:|" + "---:|" * len(verdicts)]
    for emulator in emulators:
        counts = {v: 0 for v in verdicts}
        for row in table.values():
            if emulator in row:
                counts[row[emulator]["verdict"]] = counts.get(row[emulator]["verdict"], 0) + 1
        total = sum(counts.values())
        rate = 100.0 * counts["screen"] / total if total else 0.0
        lines.append(f"| {emulator} | {rate:.1f} % ({counts['screen']}/{total}) | "
                     + " | ".join(str(counts[v]) for v in verdicts) + " |")
    complete = [row for row in table.values() if all(e in row for e in emulators)]
    for emulator in emulators:
        others = [e for e in emulators if e != emulator]
        behind = sorted((row[emulator]["title"] or ident, row[emulator]["verdict"], row[emulator]["detail"])
                        for ident, row in table.items() if row in complete
                        and row[emulator]["verdict"] != "screen"
                        and any(row[o]["verdict"] == "screen" for o in others))
        lines += ["", f"## {emulator}: fails where another passes ({len(behind)})", ""]
        lines += [f"- {title}: {verdict}" + (f" ({detail})" if detail else "") for title, verdict, detail in behind]
    # What no emulator boots, with what the disk's own META says about it
    # (a data disk for another program, a card it needs).
    none = sorted((row[emulators[0]]["title"] or ident, ident) for ident, row in table.items()
                  if row in complete and not any(row[e]["verdict"] == "screen" for e in emulators))
    lines += ["", f"## No emulator boots it ({len(none)})", ""]
    for title, ident in none:
        note = ""
        if corpus and files and ident in files and (corpus / files[ident]).exists():
            note = woz_meta(corpus / files[ident]).get("notes", "")
        lines.append(f"- {title}" + (f": {note}" if note else ""))
    (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return "\n".join(lines[:4 + len(emulators)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--emulator"), parser.add_argument("--binary"), parser.add_argument("--woz")
    parser.add_argument("--machine"), parser.add_argument("--capture")
    parser.add_argument("--corpus", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--pom2", type=Path)
    parser.add_argument("--applewin", type=Path)
    parser.add_argument("--mame", type=Path)
    parser.add_argument("--mame-roms", type=Path)
    parser.add_argument("--roms", type=Path, default=ROOT / "roms")
    parser.add_argument("--seconds", type=float, default=45.0)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--captures", action="store_true")
    args = parser.parse_args()
    if args.worker:
        worker(args)
        return 0
    emulators = [(name, path) for name, path in (("pom2", args.pom2), ("applewin", args.applewin),
                                                ("mame", args.mame)) if path]
    manifest = args.corpus / "manifest.json"
    if manifest.exists():
        entries = [e for e in json.loads(manifest.read_text()) if "sha256" in e]
    else:
        # A fetch still running: the disks already there, hashed now. A later
        # run with the manifest resumes, keeping the cells judged on the same bytes.
        import hashlib
        entries = [{"identifier": w.stem, "title": w.stem, "file": w.name,
                    "sha256": hashlib.sha256(w.read_bytes()).hexdigest()}
                   for w in sorted(args.corpus.glob("*.woz"))]
    if args.limit:
        entries = entries[:args.limit]
    args.out.mkdir(parents=True, exist_ok=True)
    log = args.out / "results.jsonl"
    done = {}
    if log.exists():   # resume: a cell already judged on the same bytes is kept
        for line in log.read_text().splitlines():
            r = json.loads(line)
            done[(r["emulator"], r["identifier"], r["sha256"])] = r
    todo = [(e, b, entry) for entry in entries for e, b in emulators
            if (e, entry["identifier"], entry["sha256"]) not in done]
    print(f"{len(entries)} titles x {len(emulators)} emulators: {len(todo)} runs to do", flush=True)
    results = list(done.values())
    with open(log, "a") as sink, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(one, args, e, b, entry, args.corpus, args.out) for e, b, entry in todo]
        for n, future in enumerate(concurrent.futures.as_completed(futures), 1):
            r = future.result()
            results.append(r)
            sink.write(json.dumps(r) + "\n")
            sink.flush()
            if n % 50 == 0:
                print(f"{n}/{len(todo)}", flush=True)
    files = {e["identifier"]: e["file"] for e in entries if e.get("file")}
    print(report(results, [e for e, _ in emulators], args.out, args.corpus, files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
