#!/usr/bin/env python3
"""Check that POM2's libretro core is deterministic enough for netplay.

RetroArch's netplay runs one copy of the core on each machine, feeds both
the same inputs, and checks the CRC of a serialized state now and then; on
a late input it loads an older state and replays the frames (rollback). A
client that joins is sent the host's state. So the core must give, from the
same state and the same inputs, the same state again — byte for byte, in
another process, in another directory, at another time.

For each scenario this runs:

  frameskip a third process whose frontend reports a nearly empty audio
           buffer, so the core skips drawing three frames in four: same CRCs;
  twin     two processes from a cold load, the same scripted inputs: the
           state's CRC must agree on every frame (with the clock off: a
           cold load takes the host's date, and netplay syncs that by
           sending the host's state, which `join` checks);
  rollback in one process, every 97 frames: save, run 31 frames, load,
           run them again with the same inputs: same CRCs;
  join     a third process loads the first one's state at frame N/2, as a
           netplay client does, and must reach the same CRCs to the end.

The inputs are a seeded script: the joystick (port 1), the second joystick
(port 2) and the keyboard as a netplay device (port 3), plus typed lines.

  python3 tools/libretro_netplay.py build/pom2_libretro.dylib roms \\
      [--disk packaging/disks/DIRTBIKE.dsk] [--frames 1800]
"""

import argparse
import ctypes as C
import json
import random
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from libretro_frontend import Frontend, prepare_system  # noqa: E402

RETRO_DEVICE_JOYPAD, RETRO_DEVICE_KEYBOARD, RETRO_DEVICE_ANALOG = 1, 3, 5
RETROK_RETURN, RETROK_LSHIFT = 13, 304

# (disk relative to the checkout, machine, frames): the keyboard and BASIC;
# a Disk II .dsk; a WOZ through the flux path; a 3.5-inch disk on the //c
# Plus, whose 4 MHz accelerator and SmartPort media each had a netplay bug.
SUITE = [(None, "iie-enhanced", 600),
         ("packaging/disks/DIRTBIKE.dsk", "iie-enhanced", 900),
         ("disks_5.4/demo/fastloader/fastloader.woz", "iie-enhanced", 900),
         ("packaging/disks/DIX-fix.po", "iic-plus", 900)]

# The US keyboard a netplay keyboard device is read as: character -> (RETROK, shift).
US_KEYS = {"\r": (RETROK_RETURN, False), " ": (32, False), ";": (59, False), ":": (59, True),
           "(": (57, True), ")": (48, True)}
US_KEYS.update({c: (ord(c), False) for c in "0123456789"})
US_KEYS.update({c: (ord(c.lower()), True) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"})


class NetplayFrontend(Frontend):
    """The test frontend, plus serialization and the scripted inputs."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.core.retro_serialize_size.restype = C.c_size_t
        self.core.retro_serialize.argtypes = [C.c_void_p, C.c_size_t]
        self.core.retro_serialize.restype = C.c_bool
        self.core.retro_unserialize.argtypes = [C.c_void_p, C.c_size_t]
        self.core.retro_unserialize.restype = C.c_bool
        self.inputs = {}   # (port, device, id) -> value, this frame

    def input_state(self, port, device, index, button):
        if device == RETRO_DEVICE_ANALOG:
            return self.inputs.get((port, RETRO_DEVICE_ANALOG, index * 2 + button), 0)
        return self.inputs.get((port, device, button), 0)

    def save(self):
        size = self.core.retro_serialize_size()
        buffer = C.create_string_buffer(size)
        if not self.core.retro_serialize(buffer, size):
            raise RuntimeError("retro_serialize failed")
        return buffer.raw

    def restore(self, state):
        buffer = C.create_string_buffer(state, len(state))
        if not self.core.retro_unserialize(buffer, len(state)):
            raise RuntimeError("retro_unserialize failed")


def script(seed, frames):
    """Frame -> the inputs held on it: buttons, sticks, keys, typed lines."""
    rng = random.Random(seed)
    held = [dict() for _ in range(frames)]
    def hold(start, length, key, value=1):
        for f in range(start, min(frames, start + length)):
            held[f][key] = value
    # A BASIC line typed early, then RUN: RND reads the keyboard wait loop's
    # counter, so it is as deterministic as the frames the keys land on.
    text = "10 PRINT RND(1);:GOTO 10\rRUN\r"
    frame = 90
    for ch in text:
        key, shift = US_KEYS[ch]
        if shift:
            hold(frame - 1, 5, (2, RETRO_DEVICE_KEYBOARD, RETROK_LSHIFT))
        hold(frame, 3, (2, RETRO_DEVICE_KEYBOARD, key))
        frame += 6
    typed_until = frame + 30
    # Then noise on every device: buttons, the stick, the second stick, keys.
    for _ in range(frames // 6):
        start, length = rng.randrange(typed_until, max(typed_until + 1, frames)), rng.randrange(1, 20)
        kind = rng.randrange(4)
        if kind == 0:
            hold(start, length, (0, RETRO_DEVICE_JOYPAD, rng.choice([0, 4, 5, 6, 7, 8])))
        elif kind == 1:
            hold(start, length, (0, RETRO_DEVICE_ANALOG, rng.randrange(2)), rng.randrange(-32768, 32767))
        elif kind == 2:
            hold(start, length, (1, RETRO_DEVICE_JOYPAD, rng.choice([0, 4, 5, 6, 7])))
        else:
            hold(start, length, (2, RETRO_DEVICE_KEYBOARD, rng.choice(list(range(97, 123)) + [32, 273, 274])))
    return held


def worker(args):
    work = Path(tempfile.mkdtemp(prefix="pom2-netplay-"))
    system = work / "system"
    prepare_system(system, Path(args.roms))
    for name in ("apple2c-32Kv0.rom", "apple2cp.rom", "apple2p.rom", "apple2o.rom", "apple2_char.rom",
                 "apple2e_unenh.rom", "apple2e_char_2k.rom", "mouse_341-0270-c.bin"):
        if (Path(args.roms) / name).is_file():
            shutil.copyfile(Path(args.roms) / name, system / "pom2" / name)
    options = {"pom2_machine": args.machine, "pom2_video_standard": "ntsc",
               "pom2_dix_preset": "disabled", "pom2_mockingboard": args.mockingboard,
               "pom2_clock": args.clock, "pom2_fast_disk_loading": "disabled",
               "pom2_frameskip": "auto" if args.frameskip else "disabled"}
    host = NetplayFrontend(Path(args.core), system, work / "saves", options)
    try:
        host.load(Path(args.disk) if args.disk else None)
        host.core.retro_set_controller_port_device(2, RETRO_DEVICE_KEYBOARD)
        held = script(args.seed, args.frames)
        crcs = {}
        start = 0
        if args.join:
            state = Path(args.join).read_bytes()
            host.restore(state)
            start = args.join_frame
        def step(f):
            host.inputs = held[f]
            if args.frameskip and host.audio_status:
                host.audio_status(True, 0, True)   # RetroArch: the audio buffer is about to run dry
            host.frame()
        mismatches = []
        for f in range(start, args.frames):
            if args.rollback and f % 97 == 0 and f + 31 < args.frames:
                saved = host.save()
                first = []
                for g in range(f, f + 31):
                    step(g)
                    first.append(zlib.crc32(host.save()))
                host.restore(saved)
                for i, g in enumerate(range(f, f + 31)):
                    step(g)
                    if zlib.crc32(host.save()) != first[i]:
                        mismatches.append(g)
                        break
                host.restore(saved)
            step(f)
            state = host.save()
            crcs[f] = zlib.crc32(state)
            if args.dump_at is not None and f == args.dump_at:
                Path(args.dump).write_bytes(state)
        print(json.dumps({"crcs": crcs, "rollback_mismatches": mismatches,
                          "size": len(host.save())}))
    finally:
        host.close()


def run(args, **extra):
    command = [sys.executable, "-B", __file__, "--worker", args.core, args.roms,
               "--frames", str(args.frames), "--seed", str(args.seed),
               "--clock", extra.get("clock", args.clock), "--mockingboard", args.mockingboard,
               "--machine", args.machine]
    if args.disk:
        command += ["--disk", args.disk]
    for flag in ("rollback", "frameskip"):
        if extra.get(flag):
            command.append("--" + flag)
    for key in ("join", "join_frame", "dump", "dump_at"):
        if extra.get(key) is not None:
            command += ["--" + key.replace("_", "-"), str(extra[key])]
    done = subprocess.run(command, capture_output=True, text=True)
    lines = [l for l in done.stdout.splitlines() if l.startswith("{")]
    if done.returncode != 0 or not lines:
        raise RuntimeError(f"worker failed:\n{done.stderr[-2000:]}")
    result = json.loads(lines[-1])
    result["crcs"] = {int(k): v for k, v in result["crcs"].items()}
    return result


def first_difference(a, b):
    for frame in sorted(set(a) & set(b)):
        if a[frame] != b[frame]:
            return frame
    return None


def check(args):
    """The three checks for one scenario; returns the number of failures."""
    failures = 0
    def report(ok, what):
        nonlocal failures
        print(f"  [{'PASS' if ok else 'FAIL'}] {what}")
        failures += not ok

    name = Path(args.disk).name if args.disk else "no disk (BASIC)"
    print(f"== netplay determinism: {name} on {args.machine}, {args.frames} frames ==")
    # Twin: two cold loads, clock off (a cold load takes the host's date).
    a = run(args, clock="disabled")
    b = run(args, clock="disabled")
    diff = first_difference(a["crcs"], b["crcs"])
    report(diff is None, "two processes, same inputs: same state on every frame"
           + ("" if diff is None else f" (first difference at frame {diff})"))
    report(a["size"] == b["size"], f"same state size ({a['size']} bytes)")
    # A peer whose frontend skips drawing (frameskip on a slow device).
    c = run(args, clock="disabled", frameskip=True)
    diff = first_difference(a["crcs"], c["crcs"])
    report(diff is None, "a peer that skips drawing frames keeps the same state"
           + ("" if diff is None else f" (first difference at frame {diff})"))

    # Rollback, then join, with the clock as configured.
    with tempfile.TemporaryDirectory() as tmp:
        middle = args.frames // 2
        dump = Path(tmp) / "host.state"
        host = run(args, rollback=True, dump=dump, dump_at=middle - 1)
        report(not host["rollback_mismatches"], "load a state and replay 31 frames: same states"
               + ("" if not host["rollback_mismatches"] else f" (first at frame {host['rollback_mismatches'][0]})"))
        client = run(args, join=dump, join_frame=middle)
        tail = {f: c for f, c in host["crcs"].items() if f >= middle}
        diff = first_difference(tail, client["crcs"])
        report(diff is None and len(client["crcs"]) == len(tail),
               f"a client joining at frame {middle} with the host's state stays in step"
               + ("" if diff is None else f" (first difference at frame {diff})"))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("core")
    parser.add_argument("roms")
    parser.add_argument("--disk")
    parser.add_argument("--frames", type=int, default=1800)
    parser.add_argument("--seed", type=int, default=1977)
    parser.add_argument("--clock", default="enabled")
    parser.add_argument("--mockingboard", default="mockingboard")
    parser.add_argument("--machine", default="iie-enhanced")
    parser.add_argument("--suite", action="store_true",
                        help="the ctest scenarios: BASIC, a .dsk, a WOZ, a //c Plus 3.5-inch disk")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--rollback", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--frameskip", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--join", help=argparse.SUPPRESS)
    parser.add_argument("--join-frame", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--dump", help=argparse.SUPPRESS)
    parser.add_argument("--dump-at", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args)
        return 0
    if not (Path(args.roms) / "apple2e.rom").is_file():
        print("SKIP: roms/apple2e.rom unavailable")
        return 77

    if args.suite:
        root = Path(__file__).resolve().parent.parent
        failures = 0
        for disk, machine, frames in SUITE:
            if disk and not (root / disk).is_file():
                print(f"-- {disk}: not fetched (tools/fetch_test_media.py), not run")
                continue
            if machine == "iic-plus" and not (Path(args.roms) / "apple2cp.rom").is_file():
                print(f"-- //c Plus: roms/apple2cp.rom unavailable, not run")
                continue
            scenario = argparse.Namespace(**vars(args))
            scenario.disk = str(root / disk) if disk else None
            scenario.machine, scenario.frames = machine, frames
            failures += check(scenario)
        print("OK" if not failures else f"FAILED ({failures})")
        return 1 if failures else 0
    failures = check(args)
    print("OK" if not failures else f"FAILED ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
