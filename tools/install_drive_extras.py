#!/usr/bin/env python3
"""Fetch three Disk II sounds POM2's sample set does not have.

MAME's floppy samples (roms/floppy_samples/, BSD) cover the motor, the seeks
and one step click. They have no recording of a 5.25" drive DOOR, and none of
the carriage hitting the TRACK-0 STOP — the rattle of every boot. OpenEmulator
has all three, recorded from a Shugart SA400 (the Disk II's mechanism):

    Shugart SA400 Open.ogg   -> 525_door_open.wav
    Shugart SA400 Close.ogg  -> 525_door_close.wav
    Shugart SA400 Stop.ogg   -> 525_head_stop.wav

POM2 does NOT ship them. OpenEmulator's COPYING says "the OpenEmulator
binaries and most of its source code are distributed under the GPL version 3
license" and says nothing about the recordings under res/sounds/, nor who made
them. Until the project confirms, they are fetched onto YOUR machine by this
script and stay there: roms/floppy_extras/ is ignored by git and is not in a
package (packaging/stage_data.sh --verify would refuse it).

    tools/install_drive_extras.py                 # into roms/floppy_extras/
    tools/install_drive_extras.py --out DIR

Each file is fetched at a pinned commit and checked against a SHA-256 before
anything is written. Needs `ffmpeg` on PATH: the sources are Ogg Vorbis and
POM2's loader reads 16-bit mono WAV.

FloppySoundDevice::loadExtras picks up whichever of the three is present; a
missing one leaves that event silent, as it was. Delete the folder to go back.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "openemulator/libemulation"
COMMIT = "8288db41ecc2e137f1e8c2ddb9d063b5e3e5599c"   # master, 2025-07-24
BASE = f"https://raw.githubusercontent.com/{REPO}/{COMMIT}/res/sounds/Shugart/"

# upstream name, POM2 name, SHA-256 of the upstream file
FILES = [
    ("Shugart SA400 Open.ogg", "525_door_open.wav",
     "64197916aa6d5bada4cdcceaae9fae8b15fb1622bbc7dfa3da0ff8cc50dc7f36"),
    ("Shugart SA400 Close.ogg", "525_door_close.wav",
     "9df48a2b5c3590e6d3c0d19eb093aed4b310270a0dd3c59eddeeb1faf692bf55"),
    ("Shugart SA400 Stop.ogg", "525_head_stop.wav",
     "9a5ed615cc3114b15e7555e1124557e3134896a1d08b69d51041f6e200d2b142"),
]

README = """These three recordings are NOT part of POM2.

Fetched by tools/install_drive_extras.py from
https://github.com/{repo} at commit {commit}
(res/sounds/Shugart/), transcoded from Ogg Vorbis to 16-bit mono WAV.

OpenEmulator's COPYING puts its binaries and "most of its source code" under
the GPL version 3 and does not name the recordings. Keep them on this machine:
do not commit or redistribute them until their licence is confirmed upstream.
"""


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=root / "roms" / "floppy_extras")
    args = ap.parse_args()

    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("install_drive_extras: ffmpeg not found on PATH (the sources are "
              "Ogg Vorbis)", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        staged = []
        for upstream, name, want in FILES:
            url = BASE + urllib.parse.quote(upstream)
            try:
                with urllib.request.urlopen(url, timeout=60) as r:
                    data = r.read()
            except OSError as e:
                print(f"install_drive_extras: cannot fetch {url}: {e}", file=sys.stderr)
                return 1
            got = hashlib.sha256(data).hexdigest()
            if got != want:
                print(f"install_drive_extras: {upstream}: SHA-256 {got}, expected {want}",
                      file=sys.stderr)
                return 1
            src = Path(tmp) / "in.ogg"
            dst = Path(tmp) / name
            src.write_bytes(data)
            done = subprocess.run(
                [ffmpeg, "-v", "error", "-y", "-i", str(src),
                 "-ac", "1", "-c:a", "pcm_s16le", str(dst)])
            if done.returncode != 0 or not dst.is_file():
                print(f"install_drive_extras: ffmpeg could not convert {upstream}",
                      file=sys.stderr)
                return 1
            staged.append(dst)

        # Nothing is written until all three are verified and converted.
        args.out.mkdir(parents=True, exist_ok=True)
        for f in staged:
            shutil.copyfile(f, args.out / f.name)
        (args.out / "README.txt").write_text(
            README.format(repo=REPO, commit=COMMIT), encoding="ascii")

    print(f"install_drive_extras: 3 recordings in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
