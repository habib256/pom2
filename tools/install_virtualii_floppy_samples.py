#!/usr/bin/env python3
"""Map Virtual ][ drive sounds onto POM2's FloppySoundDevice WAV names.

Virtual ][ (Gerth, commercial) ships a handful of AIFF/RAW clips inside the
.app bundle. POM2's loader wants MAME's ten-stem bank (`525_*.wav` /
`35_*.wav`). This script transcodes a locally installed Virtual ][ and
writes the result under the per-user data dir, which `findResource` searches
first — so a GUI session hears Gerth's samples without touching the
BSD-licensed MAME set in `roms/floppy_samples/`.

    tools/install_virtualii_floppy_samples.py
    tools/install_virtualii_floppy_samples.py --app '/path/to/Virtual ][.app'
    tools/install_virtualii_floppy_samples.py --out /tmp/v2-floppy

The samples stay on THIS machine. Do not commit or redistribute them.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SR = 44100

# Virtual ][ has two independent effects (its own checkboxes):
#   disk-drive  = Disk Rotation.raw (motor, looped) + Move arm.aiff (head)
#   lid         = Disk Insertion.aiff + Disk Removal.aiff
# Boot.raw / I-O Error.raw are one-shots for boot and read errors; they are
# not the spindle or the arm. POM2's ten stems are MAME's model, so the
# drive clips fill every stem and the lid clips stay out of it — a lid slam
# on spin_start or spin_end is the motor playing the door.


def default_app() -> Path:
    return Path("/Applications/Virtual ][.app")


def default_out() -> Path:
    home = os.environ.get("HOME", "")
    if home:
        return Path(home) / "Library" / "Application Support" / "POM2" / "roms" / "floppy_samples"
    return Path.cwd() / "roms" / "floppy_samples_virtualii"


def read_wav(path: Path) -> list[int]:
    import wave

    with wave.open(str(path), "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != SR:
            raise SystemExit(f"{path}: expected mono 16-bit {SR} Hz, got "
                             f"{w.getnchannels()}ch {w.getsampwidth()*8}bit {w.getframerate()}Hz")
        n = w.getnframes()
        return list(struct.unpack("<" + "h" * n, w.readframes(n)))


def write_wav(path: Path, samples: list[int]) -> None:
    import wave

    path.parent.mkdir(parents=True, exist_ok=True)
    clamped = [max(-32768, min(32767, int(v))) for v in samples]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack("<" + "h" * len(clamped), *clamped))


def fade(samples: list[int], fade_in: int = 0, fade_out: int = 0) -> list[int]:
    out = list(samples)
    n = len(out)
    fi = min(fade_in, n)
    fo = min(fade_out, n)
    for i in range(fi):
        out[i] = int(out[i] * (i / fi))
    for i in range(fo):
        idx = n - fo + i
        out[idx] = int(out[idx] * ((fo - i) / fo))
    return out


def crop_ms(samples: list[int], start_ms: float, dur_ms: float) -> list[int]:
    start = int(start_ms * SR / 1000.0)
    dur = int(dur_ms * SR / 1000.0)
    start = max(0, min(start, len(samples)))
    end = max(start + 1, min(start + dur, len(samples)))
    return samples[start:end]


def raw_s16le(path: Path) -> list[int]:
    data = path.read_bytes()
    n = len(data) // 2
    return list(struct.unpack("<" + "h" * n, data[: n * 2]))


def afconvert_mono(src: Path, dst: Path) -> None:
    subprocess.run(
        ["afconvert", "-f", "WAVE", "-d", f"LEI16@{SR}", "-c", "1",
         str(src), str(dst)],
        check=True,
    )


def install(app: Path, out: Path) -> int:
    res = app / "Contents" / "Resources"
    if not res.is_dir():
        print(f"error: no Resources in {app}", file=sys.stderr)
        return 1

    needed = [
        "Move arm.aiff",
        "Disk Rotation.raw",
    ]
    missing = [n for n in needed if not (res / n).is_file()]
    if missing:
        print("error: missing in Virtual ][ bundle:", ", ".join(missing),
              file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="pom2-v2-") as tmp:
        tmp_p = Path(tmp)
        afconvert_mono(res / "Move arm.aiff", tmp_p / "arm.wav")
        arm = read_wav(tmp_p / "arm.wav")

    # Disk Rotation.raw is headerless s16le. 9188 frames @ 44.1 kHz is
    # 208 ms — one Disk II revolution — and Virtual ][ loops it for the
    # whole time the spindle is on. Same recording for empty and loaded:
    # the bundle has one motor clip.
    rotation = raw_s16le(res / "Disk Rotation.raw")
    spin = fade(rotation, fade_in=32, fade_out=32)
    # POM2 plays spin_start as a one-shot, then the loop. Virtual ][ just
    # starts the loop, so the one-shot is a short fade-in of that same
    # recording, not the lid.
    spin_start = fade(crop_ms(rotation, 0, 80), fade_in=int(0.015 * SR), fade_out=0)
    spin_end = fade(crop_ms(rotation, 0, 150), fade_in=0, fade_out=int(0.080 * SR))

    # Move arm.aiff is the head. POM2 loops a seek stem for the whole
    # seek and pitch-scales it; a 60 ms crop of this sweep repeats as a
    # stutter. The full recording is what Virtual ][ plays while the arm
    # moves. All four cadence classes share it — the engine still picks
    # the class from the step rate, and at each class's nominal gap the
    # pitch is 1, so the arm plays at its recorded speed.
    arm_loop = fade(arm, fade_in=16, fade_out=16)
    # step_1_1 is the one-shot click (isolated step, and the landing tick
    # when a seek ends). One short grain of the arm, not the lid slam.
    step = fade(crop_ms(arm, 60, 45), fade_in=0, fade_out=int(0.008 * SR))

    seeks = {stem: arm_loop for stem in
             ("seek_2ms", "seek_6ms", "seek_12ms", "seek_20ms")}

    stems = {
        "step_1_1": step,
        "spin_empty": spin,
        "spin_loaded": spin,
        "spin_start_empty": spin_start,
        "spin_start_loaded": spin_start,
        "spin_end": spin_end,
        **seeks,
    }

    out.mkdir(parents=True, exist_ok=True)
    written = 0
    for prefix in ("525", "35"):
        for stem, samples in stems.items():
            write_wav(out / f"{prefix}_{stem}.wav", samples)
            written += 1

    readme = out / "SOURCE.txt"
    readme.write_text(
        "POM2 local override — Virtual ][ floppy samples\n"
        "================================================\n"
        "\n"
        f"Extracted from: {app}\n"
        "Mapped onto FloppySoundDevice names (525_* and 35_*; the 3.5\"\n"
        "bank is a copy — Virtual ][ ships one Disk II set):\n"
        "\n"
        "  Disk Rotation.raw   motor. spin_loaded, spin_empty (the loop),\n"
        "                      spin_start_* (short fade-in), spin_end\n"
        "                      (fade-out). Virtual ][ loops this while the\n"
        "                      spindle turns.\n"
        "  Move arm.aiff       head. All four seek_* stems are the whole\n"
        "                      recording; step_1_1 is one short grain of it.\n"
        "  Disk Insertion.aiff lid close — not a motor or head sample.\n"
        "  Disk Removal.aiff   lid open — same.\n"
        "\n"
        "These files are Gerth's commercial recordings. They live here so\n"
        "findResource() prefers them over roms/floppy_samples/ (MAME,\n"
        "BSD-3-Clause) for THIS user. Do not commit or redistribute.\n"
        "\n"
        "Delete this directory to go back to the MAME bank.\n",
        encoding="utf-8",
    )
    print(f"installed {written} WAVs → {out}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--app", type=Path, default=default_app(),
                   help="Virtual ][.app bundle (default: /Applications/Virtual ][.app)")
    p.add_argument("--out", type=Path, default=None,
                   help="destination directory (default: userDataDir/roms/floppy_samples)")
    args = p.parse_args()
    out = args.out if args.out is not None else default_out()
    if not args.app.is_dir():
        print(f"error: Virtual ][ not found at {args.app}", file=sys.stderr)
        return 1
    return install(args.app, out)


if __name__ == "__main__":
    sys.exit(main())
