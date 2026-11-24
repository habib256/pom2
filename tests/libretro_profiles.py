"""Hardware and controller scenarios exercised through the public libretro ABI."""
import argparse
import ctypes
import hashlib
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from libretro_frontend import Frontend


class Host(Frontend):
    x = y = 0
    left = False
    stereo = False
    capture_audio = False

    def input_state(self, port, device, index, button):
        if port == 0 and device == 2:
            return {0: self.x, 1: self.y, 2: self.left}.get(button, 0)
        return super().input_state(port, device, index, button)

    def batch_audio(self, data, frames):
        if self.capture_audio:
            self.audio_digest = hashlib.sha256(ctypes.string_at(data, frames * 4)).digest()
        if self.frames % 30 == 0 and not self.stereo:
            self.stereo = any(data[2*i] != data[2*i+1] for i in range(0, frames, 5))
        return super().batch_audio(data, frames)

    def tap(self, button):
        self.pad.add(button)
        self.frame()
        self.pad.remove(button)
        self.frame()

    def motion(self, dx, dy, count):
        self.x, self.y = dx, dy
        self.frame(count)
        self.x = self.y = 0
        self.frame(12)

    def crop(self, left, top, width, height):
        data, w, h, pitch = self.video
        assert w >= left + width and h >= top + height and self.format == 1
        return b"".join(data[y*pitch+4*left:y*pitch+4*(left+width)] for y in range(top, top+height))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_replay(host, frames, inputs):
    host.core.retro_serialize_size.restype = ctypes.c_size_t
    save, restore = host.core.retro_serialize, host.core.retro_unserialize
    save.argtypes = restore.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    save.restype = restore.restype = ctypes.c_bool
    size = host.core.retro_serialize_size()
    require(size > 0, "configuration did not offer save states")
    state = ctypes.create_string_buffer(size)
    require(save(state, size), "could not capture the machine state")

    def replay():
        for frame in range(frames):
            inputs(frame)
            host.frame()
            yield hashlib.sha256(host.video[0]).digest(), host.audio_digest

    host.capture_audio = True
    expected = list(replay())
    require(restore(state, size), "could not restore the machine state")
    for index, (reference, output) in enumerate(zip(expected, replay())):
        require(reference == output, f"replay diverged at frame {index}")
    host.capture_audio = False


def check_dix(host, disk, full):
    av = host.load(disk)
    require(host.core.retro_get_region() == 1 and abs(av["fps"] - 1015625/20280) < 0.00001,
            "DIX did not select PAL timing")
    # FNV over the upper 40 lines of the actual RGB monitor. Each reference
    # is a distinct scene reached during a complete automatic tour.
    def scene(frame, expected):
        host.frame(frame - host.frames)
        raw, w, _, _ = host.video
        value = 2166136261
        for pixel in memoryview(raw)[:40*w*4].cast("I"):
            value = ((value ^ (pixel & 0xffffff)) * 16777619) & 0xffffffff
        require(value == expected, f"DIX frame {frame}: expected {expected:08x}, got {value:08x}")

    scene(1250, 0xa416f539)  # FRENCH TOUCH / USE ARROWS TO SELECT DEMO
    host.key("LEFT")
    host.frame(48)
    host.key("RETURN")
    scene(2800, 0x022548b3)  # UNLIMITED BOBS
    require(host.stereo, "Mockingboard did not produce distinct left/right channels")
    if full:
        for frame, expected in [(8709, 0x2f967d5d), (12165, 0xdf370291), (18375, 0xe6a090e5),
                                (22181, 0x85098545), (27339, 0x6484c0c5), (33299, 0x926f9479),
                                (46270, 0xa224e9dd), (51228, 0x48ff5447), (57338, 0x907f4721),
                                (62947, 0x91ecf163), (69007, 0x9e9b976d), (71211, 0x6454dd31),
                                (76670, 0xd8047de5), (79625, 0x12bb402b)]:
            scene(frame, expected)
    require(abs(host.audio_frames - host.frames * av["sample_rate"] / av["fps"]) < 1.01,
            "PAL audio drifted relative to the beam")
    check_replay(host, 600, lambda _: None)


def check_mouse(host, disk):
    host.load(disk)
    host.frame(1200)
    host.x = host.y = -6
    host.frame(30)
    host.x = host.y = 0
    host.frame(5)
    old = (478, 18, 75, 26)
    dest = (205, 82, 85, 30)
    old_icon = host.crop(*old)
    background = host.crop(*dest)
    require(len(set(background)) < len(set(old_icon)), "DeskTop volume icon did not appear")
    host.motion(8, 0, 100)
    host.motion(-3, 0, 10)
    host.motion(0, 3, 13)
    host.left = True
    host.frame(12)
    host.motion(-4, 2, 35)
    host.left = False
    host.frame(45)
    host.motion(-6, -6, 60)  # move pointer away from both reference regions
    require(host.crop(*old) != old_icon and host.crop(*dest) != background
            and len(set(host.crop(*old))) == len(set(background)),
            "DeskTop did not move the volume icon when dragged and released")
    def replay_motion(frame):
        host.x = (frame % 6) - 3
        host.y = 2 if frame % 8 < 4 else -2
        host.left = 40 <= frame < 60

    check_replay(host, 180, replay_motion)


def check_keyboard(host):
    host.load(None)
    host.frame(120)
    # Enter Q with the physical keyboard, then compare the guest's glyph
    # against one entered with just the RetroPad after a fresh boot.
    host.key("Q")
    expected = host.crop(7, 16, 7, 8)
    host.core.retro_unload_game()
    host.loaded = False
    host.load(None)
    host.frame(120)
    require(host.crop(7, 16, 7, 8) != expected, "BASIC did not start with a blank input cell")
    host.tap(14)  # L3 opens the keyboard
    panel = host.crop(48, 130, 176, 58)
    host.tap(5)   # down to Q
    host.tap(0)   # B types Q
    host.tap(14)
    require(host.crop(48, 130, 176, 58) != panel and host.crop(7, 16, 7, 8) == expected,
            "virtual keyboard did not type the selected key into BASIC")


def text_page(host):
    """The 40-column text page as ASCII, read through the memory map."""
    get = host.core.retro_get_memory_data
    get.restype, get.argtypes = ctypes.c_void_p, [ctypes.c_uint]
    ram = (ctypes.c_uint8 * 0xC000).from_address(get(2))
    rows = []
    for row in range(24):
        base = 0x400 + (row % 8) * 0x80 + (row // 8) * 0x28
        rows.append("".join(chr((ram[base + c] & 0x7F) + (0x40 if (ram[base + c] & 0x7F) < 0x20 else 0))
                            for c in range(40)))
    return ram, "\n".join(rows)


def check_formats(module, system, root, disk):
    """Archives and programs as content: a zip, a two-disk zip, a gzip, an
    Applesoft listing and a tagged binary, each through retro_load_game."""
    import gzip
    import zipfile
    content = root / "content"
    content.mkdir()
    with zipfile.ZipFile(content / "game.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(disk, "Games/DIRTBIKE.dsk")
    with zipfile.ZipFile(content / "two disks.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(disk, "Game (Disk 1).dsk")
        z.write(disk, "Game (Disk 2).dsk")
        z.writestr("readme.txt", "not a disk")
    (content / "game.dsk.gz").write_bytes(gzip.compress(disk.read_bytes()))
    (content / "hello.bas").write_text('10 HOME\n20 PRINT "POM2 BAS OK"\n30 END\n')
    # LDA #$A5 / STA $0350 / RTS at $0300, the address in its name.
    (content / "MARK#060300.bin").write_bytes(bytes([0xA9, 0xA5, 0x8D, 0x50, 0x03, 0x60]))
    (content / "dirtbike.sdk").write_bytes(shrinkit_disk(disk.read_bytes()))
    (content / "hello.aci").write_bytes(applesoft_tape())

    def boot(name, frames):
        host = Host(module, system, root / "saves", {})
        try:
            host.load(content / name)
            host.frame(frames)
            ram, text = text_page(host)
            return host.disk.get_num_images(), ram[0x350], text
        finally:
            host.close()

    for name, images in (("game.zip", 1), ("two disks.zip", 2), ("game.dsk.gz", 1)):
        count, _, text = boot(name, 300)
        require(count == images, f"{name} should hold {images} disk(s), not {count}")
        require("DIRTBIKE" in text, f"{name} did not boot Dirtbike")
    _, _, text = boot("hello.bas", 240)
    require("POM2 BAS OK" in text, "the Applesoft listing did not RUN")
    _, marker, text = boot("MARK#060300.bin", 240)
    require(marker == 0xA5 and "CALL 768" in text, "the binary was not loaded at $0300 and called")
    count, _, text = boot("dirtbike.sdk", 300)
    require(count == 1 and "DIRTBIKE" in text, "the ShrinkIt disk archive did not boot Dirtbike")
    _, _, text = boot("hello.aci", 60 * 45)
    require("POM2 TAPE OK" in text and "LOAD" in text, "the Applesoft tape was not LOADed and RUN")
    # A 13-sector disk with no Apple 13-sector PROM in system/: POM2's own
    # boots DOS 3.2's System Master to its greeting (its Integer BASIC
    # HELLO is not available under Applesoft, and DOS says so).
    master = Path(__file__).resolve().parent.parent / "disks_5.4" / "dsk" / "DOS32STD.d13"
    if master.is_file():
        shutil.copyfile(master, content / "DOS32STD.d13")
        _, _, text = boot("DOS32STD.d13", 60 * 20)
        require("LANGUAGE NOT AVAILABLE" in text, "DOS 3.2 did not boot through POM2's 13-sector PROM")
    else:
        print("SKIP 13-sector: DOS32STD.d13 not fetched")


def crc16(data, crc=0):
    """CRC-16/XMODEM, the NuFX CRC."""
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def shrinkit_disk(dsk):
    """A ShrinkIt archive holding a 140 KB disk, stored ([B] File Type Note
    $E0/$8002). ShrinkIt keeps disks in ProDOS block order: a DOS-ordered
    .dsk is reordered first ([B] Beneath Apple ProDOS: block n of track t is
    DOS sectors 0/E, D/C, B/A, 9/8, 7/6, 5/4, 3/2, 1/F for n = 0..7)."""
    import struct
    order = [0x0, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0xF]
    blocks = b"".join(dsk[(t * 16 + order[s]) * 256:(t * 16 + order[s] + 1) * 256]
                      for t in range(35) for s in range(16))
    name = b"DIRTBIKE"
    header = (b"NuFX\0\0" + struct.pack("<HHIHHIIIH", 58, 0, 1, 1, ord("/"), 0xE3, 0, 280, 512)
              + bytes(24) + struct.pack("<H", len(name)) + name
              + struct.pack("<HHHHII", 2, 0, 1, 0, 0, len(blocks)))
    crc = crc16(header[6:])
    header = header[:4] + struct.pack("<H", crc) + header[6:]
    master = b"N\xf5F\xe9l\xe5" + b"\0\0" + struct.pack("<I", 1) + bytes(36)
    header = b"N\xf5F\xd8" + header[4:]
    return master + header + blocks


def applesoft_tape():
    """`10 PRINT "POM2 TAPE OK"` as the Applesoft SAVE writes it, in POM2's
    .aci pulse format: a 3-byte header record (length, then $55), then the
    program ([C] Monitor WRITE: 770 Hz header, a 200/250 us sync cycle, bits
    as two 250 us or two 500 us halves, MSB first, an $FF-seeded XOR
    checksum)."""
    import struct
    line = b"\x0a\x00\xba\"POM2 TAPE OK\"\x00"   # line 10, PRINT, the string
    link = 0x0801 + 2 + len(line)
    program = struct.pack("<H", link) + line + b"\x00\x00"
    records = [struct.pack("<HB", len(program) + 1, 0x55), program + b"\x00\x00"]
    hz = 1022727.0
    halves = []
    def half(us):
        halves.append(max(1, round(us * hz / 1e6)))
    for record in records:
        for _ in range(16000):   # ~10 s: READ waits ~3.5 s after the first edge ([C] HEADR)
            half(649.0)
        half(200.0)
        half(250.0)
        check = 0xFF
        for byte in list(record) + [None]:
            value = check if byte is None else byte
            if byte is not None:
                check ^= byte
            for bit in range(7, -1, -1):
                us = 500.0 if (value >> bit) & 1 else 250.0
                half(us)
                half(us)
        half(500.0)
        half(400000.0)
    return b"POM1ACI1" + bytes([1, 0, 0, 0]) + struct.pack("<I", len(halves)) + struct.pack(f"<{len(halves)}I", *halves)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("roms", type=Path)
    parser.add_argument("scenario", choices=("dix", "desktop", "keyboard", "formats"))
    parser.add_argument("disk", type=Path, nargs="?")
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    roms = ["apple2e.rom", "apple2e_char.rom"]
    if args.scenario == "desktop":
        roms += ["mouse_341-0270-c.bin", "mouse_341-0269.bin"]
    if any(not (args.roms / rom).is_file() for rom in roms) or (args.disk and not args.disk.is_file()):
        print("SKIP: required firmware or scenario disk unavailable")
        return 77
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = root / "system" / "pom2"
        folder.mkdir(parents=True)
        for rom in roms:
            shutil.copyfile(args.roms / rom, folder / rom)
        if args.scenario == "formats":
            check_formats(args.module.resolve(), folder.parent, root, args.disk.resolve())
            print("PASS formats")
            return 0
        # The clock off: DeskTop draws the time, and a clock started at the
        # host's date made the scripted drag depend on when the test ran.
        host = Host(args.module.resolve(), folder.parent, root / "saves",
                    {"pom2_mouse": "enabled" if args.scenario == "desktop" else "disabled",
                     "pom2_clock": "disabled"})
        try:
            if args.scenario == "dix":
                check_dix(host, args.disk.resolve(), args.full)
            elif args.scenario == "desktop":
                check_mouse(host, args.disk.resolve())
            else:
                check_keyboard(host)
        finally:
            host.close()
    print(f"PASS {args.scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
