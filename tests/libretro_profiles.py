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
    """Archives and programs as content: a zip, a two-disk zip, a gzip, a 7z, an
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
    # Hostile and awkward archives (bug hunt 2026-11-25): names in CP437 with
    # the UTF-8 flag clear (Windows' own zipper), a program beside the disk,
    # disks named "#1 ..." (a playlist line starting with '#'), a member
    # named with a drive letter (outside the cache folder on Windows).
    def cp437_zip(path, members):
        # zipfile writes non-ASCII names as UTF-8 with the flag set: write an
        # ASCII stand-in of the same length, then put the CP437 bytes in.
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for raw, data in members:
                z.writestr(raw.decode("cp437").encode("ascii", "replace").decode().replace("?", "_"), data)
        blob = path.read_bytes()
        for raw, _ in members:
            stand_in = raw.decode("cp437").encode("ascii", "replace").replace(b"?", b"_")
            blob = blob.replace(stand_in, raw)
        path.write_bytes(blob)
    cp437_zip(content / "mixed.zip", [(b"Jeu \x82t\x82.dsk", disk.read_bytes()),
                                      (b"Notice R\x8agles.txt", b"not a disk"),
                                      (b"GAME.BIN", bytes([0x60]))])
    with zipfile.ZipFile(content / "numbered.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(disk, "#1 Boot.dsk")
        z.write(disk, "#2 Data.dsk")
    with zipfile.ZipFile(content / "drive.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(disk, "D:x.dsk")
    # A zip with its own playlist (disks in a subfolder, labels), and a zip of
    # long TOSEC-style names: the same disk list at every load, in order.
    with zipfile.ZipFile(content / "own.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(disk, "disks/Side A.dsk")
        z.write(disk, "disks/Side B.dsk")
        z.writestr("Game.m3u", "disks/Side B.dsk|B\ndisks/Side A.dsk|A\n")
    tosec = "Ultima V - Warriors of Destiny (1988)(Origin Systems, Inc.)(US)(Disk %d of 3)(Side A).dsk"
    with zipfile.ZipFile(content / "tosec.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for n in (3, 1, 2):
            z.write(disk, tosec % n)
    (content / "game.dsk.gz").write_bytes(gzip.compress(disk.read_bytes()))
    # A .7z, as the zips above: one disk in a folder, and two sides with a
    # readme between them (one solid block for all three).
    (content / "game.7z").write_bytes(seven_zip([("Games/DIRTBIKE.dsk", disk.read_bytes())]))
    (content / "two disks.7z").write_bytes(seven_zip([("Game (Disk 2).dsk", disk.read_bytes()),
                                                      ("readme.txt", b"not a disk"),
                                                      ("Game (Disk 1).dsk", disk.read_bytes())]))
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

    for name, images in (("game.zip", 1), ("two disks.zip", 2), ("game.dsk.gz", 1),
                         ("mixed.zip", 1), ("numbered.zip", 2), ("drive.zip", 1),
                         ("game.7z", 1), ("two disks.7z", 2)):
        count, _, text = boot(name, 300)
        require(count == images, f"{name} should hold {images} disk(s), not {count}")
        require("DIRTBIKE" in text, f"{name} did not boot Dirtbike")
    def unpacked(name):
        found = sorted((root / "saves" / "pom2" / "archives").glob("*/" + name))
        require(len(found) == 1, f"{name} was not unpacked once: {found}")
        return found[0].read_text()
    for attempt in range(2):
        count, _, _ = boot("own.zip", 120)
        require(count == 2 and unpacked("Game.m3u") == "./Side B.dsk|B\n./Side A.dsk|A\n",
                f"a zip's own playlist changed at load {attempt + 1}: {unpacked('Game.m3u')!r}")
        count, _, _ = boot("tosec.zip", 120)
        require(count == 3 and unpacked("tosec.m3u") == "".join("./" + tosec % n + "\n" for n in (1, 2, 3)),
                f"long names lost their order or their text at load {attempt + 1}: {unpacked('tosec.m3u')!r}")
    _, _, text = boot("hello.bas", 240)
    require("POM2 BAS OK" in text, "the Applesoft listing did not RUN")
    _, marker, text = boot("MARK#060300.bin", 240)
    require(marker == 0xA5 and "CALL 768" in text, "the binary was not loaded at $0300 and called")
    count, _, text = boot("dirtbike.sdk", 300)
    require(count == 1 and "DIRTBIKE" in text, "the ShrinkIt disk archive did not boot Dirtbike")
    _, _, text = boot("hello.aci", 60 * 45)
    require("POM2 TAPE OK" in text and "LOAD" in text, "the Applesoft tape was not LOADed and RUN")
    # The Passport MIDI card (pom2_midi): a note-on through its ACIA in slot
    # 2 reaches the frontend's MIDI interface, after the first byte timed in
    # microseconds (a byte is 320 us at 31 250 baud).
    note = bytes([0xA9, 0x03, 0x8D, 0xA8, 0xC0,   # LDA #$03 / STA $C0A8  master reset
                  0xA9, 0x15, 0x8D, 0xA8, 0xC0,   # LDA #$15 / STA $C0A8  8N1, /16
                  0xA2, 0x00,                     # LDX #0
                  0xAD, 0xA8, 0xC0, 0x29, 0x02,   # LDA $C0A8 / AND #2    transmit empty?
                  0xF0, 0xF9,                     # BEQ back
                  0xBD, 0x1F, 0x03, 0x8D, 0xA9, 0xC0,   # LDA $031F,X / STA $C0A9
                  0xE8, 0xE0, 0x03, 0xD0, 0xEE,   # INX / CPX #3 / BNE back
                  0x60,                           # RTS
                  0x90, 0x3C, 0x64])              # note on, middle C, velocity 100
    (content / "MIDI#060300.bin").write_bytes(note)
    host = Host(module, system, root / "saves", {"pom2_midi": "passport"})
    try:
        host.load(content / "MIDI#060300.bin")
        host.frame(240)
        sent = [byte for byte, _ in host.midi]
        require(sent == [0x90, 0x3C, 0x64], f"the MIDI card's note did not reach the frontend: {sent}")
        require(all(250 <= delta <= 2000 for _, delta in host.midi[1:]),
                f"MIDI bytes are not timed a byte apart: {host.midi}")
    finally:
        host.close()
    # Android's Storage Access Framework: RetroArch hands saf://<tree>/<path>,
    # which only its VFS opens. The core copies the content through the VFS
    # (a disk, then an M3U and the disks it names) and boots the copy.
    saf = root / "saf"
    (saf / "Games").mkdir(parents=True)
    shutil.copyfile(disk, saf / "Games" / "DIRTBIKE.dsk")
    shutil.copyfile(disk, saf / "Games" / "Side B.dsk")
    (saf / "Games" / "dirtbike.m3u").write_text("DIRTBIKE.dsk|Side A\nSide B.dsk|Side B\n")
    class SafHost(Host):
        vfs_root = str(saf)
    for name, images in (("saf://tree/Games/DIRTBIKE.dsk", 1), ("saf://tree/Games/dirtbike.m3u", 2)):
        host = SafHost(module, system, root / "saves", {})
        try:
            host.load(name)
            host.frame(300)
            _, text = text_page(host)
            count = host.disk.get_num_images()
            require(count == images, f"{name} should hold {images} disk(s), not {count}")
            require("DIRTBIKE" in text, f"{name} did not boot Dirtbike through the VFS")
        finally:
            host.close()
    # A per-game pad profile, found by the content's name: "Snake Byte" is a
    # keyboard game, so its D-pad types I/M/J/K. A BASIC program under that
    # name prints the keys it gets; the controls menu names the keys.
    (content / "Snake Byte.bas").write_text('10 GET A$: PRINT "KEY=";A$: GOTO 10\n')
    class ProfileHost(Host):
        descriptors = []
        def environment(self, command, data):
            if command == 11:   # SET_INPUT_DESCRIPTORS
                class Descriptor(ctypes.Structure):
                    _fields_ = [("port", ctypes.c_uint), ("device", ctypes.c_uint), ("index", ctypes.c_uint),
                                ("id", ctypes.c_uint), ("description", ctypes.c_char_p)]
                items = ctypes.cast(data, ctypes.POINTER(Descriptor))
                self.descriptors = []
                i = 0
                while items[i].description:
                    self.descriptors.append(items[i].description.decode())
                    i += 1
                return True
            return super().environment(command, data)
    for options, expect in (({}, True), ({"pom2_pad_profile": "disabled"}, False)):
        host = ProfileHost(module, system, root / "saves", options)
        try:
            host.load(content / "Snake Byte.bas")
            host.frame(240)
            host.pad.add(4)       # D-pad up
            host.frame(4)
            host.pad.discard(4)
            host.frame(30)
            _, text = text_page(host)
            labelled = "Key I: Up" in host.descriptors
            if expect:
                require(labelled, f"the Snake Byte profile does not name its keys: {host.descriptors}")
                require("KEY=I" in text, "the D-pad did not type Snake Byte's up key")
            else:
                require(not labelled and "KEY=I" not in text, "the pad profile applied while switched off")
        finally:
            host.close()
    # The profile travels with the state: a netplay client keeping the same
    # program under another name (no profile of its own) takes the host's.
    (content / "SB.bas").write_text((content / "Snake Byte.bas").read_text())
    def capture(host):
        host.core.retro_serialize_size.restype = ctypes.c_size_t
        host.core.retro_serialize.argtypes = host.core.retro_unserialize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        host.core.retro_serialize.restype = host.core.retro_unserialize.restype = ctypes.c_bool
        return host.core.retro_serialize_size()
    host = ProfileHost(module, system, root / "saves", {})
    try:
        host.load(content / "Snake Byte.bas")
        host.frame(240)
        size = capture(host)
        state = ctypes.create_string_buffer(size)
        require(host.core.retro_serialize(state, size), "could not capture the profiled machine")
    finally:
        host.close()
    host = ProfileHost(module, system, root / "saves", {})
    try:
        host.load(content / "SB.bas")
        host.frame(10)
        require("Key I: Up" not in host.descriptors, "SB.bas found a profile by itself")
        size = capture(host)
        require(host.core.retro_unserialize(state, size), "the client refused the host's state")
        require("Key I: Up" in host.descriptors, "the host's profile did not come with its state")
        host.pad.add(4)
        host.frame(4)
        host.pad.discard(4)
        host.frame(30)
        _, text = text_page(host)
        require("KEY=I" in text, "the client's D-pad did not type the host's profile key")
    finally:
        host.close()
    # The netplay keyboard (port 3) repeats a held key while port 1 is the
    # joystick: the joystick path used to reset the repeat every frame, so a
    # held key typed once (bug hunt 2026-11-25).
    (content / "echo.bas").write_text('10 GET A$: PRINT A$;: GOTO 10\n')
    class KeyboardHost(Host):
        held = set()
        def input_state(self, port, device, index, button):
            if port == 2 and device == 3:   # RETRO_DEVICE_KEYBOARD
                return int(button in self.held)
            return super().input_state(port, device, index, button)
    host = KeyboardHost(module, system, root / "saves", {})
    try:
        host.load(content / "echo.bas")
        host.core.retro_set_controller_port_device(2, 3)
        host.frame(240)
        host.held.add(120)   # RETROK_x
        host.frame(90)
        host.held.clear()
        host.frame(10)
        _, text = text_page(host)
        require(text.lower().count("x") >= 5, f"a held netplay-keyboard key did not repeat: {text!r}")
    finally:
        host.close()
    # A restore of the machine's own state changes nothing in it, with the
    # default ports (typed keyboard, port 3 on None) as with any: save, load,
    # save gives the same bytes. Netplay and run-ahead compare them (bug hunt
    # 2026-11-25: a restore rebuilt the held keys and flipped keyboard flags).
    for device, pad in ((1, ()), (0x101, (4,))):   # joystick; "Keyboard: arrows" with Up held
        host = Host(module, system, root / "saves", {})
        try:
            host.load(None)
            host.core.retro_set_controller_port_device(0, device)
            host.frame(120)
            for button in pad:
                host.pad.add(button)
            host.frame(5)
            host.core.retro_serialize_size.restype = ctypes.c_size_t
            host.core.retro_serialize.argtypes = host.core.retro_unserialize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            host.core.retro_serialize.restype = host.core.retro_unserialize.restype = ctypes.c_bool
            size = host.core.retro_serialize_size()
            first, second = ctypes.create_string_buffer(size), ctypes.create_string_buffer(size)
            require(host.core.retro_serialize(first, size) and host.core.retro_unserialize(first, size)
                    and host.core.retro_serialize(second, size), "could not save, load and save")
            require(first.raw == second.raw, f"a restore changed the state it restored (port 1 device {device:#x})")
        finally:
            host.close()
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


def seven_zip(members):
    """A solid .7z of `members` [(name, bytes)] in one LZMA2 folder, written
    from 7-Zip's DOC/7zFormat.txt with the standard library only (Python's
    liblzma packs, the LZMA SDK in the core unpacks: two code bases)."""
    import lzma
    import struct
    import zlib

    def num(v):   # 7zFormat.txt "NUMBER": the leading 1 bits count the extra bytes
        for n in range(8):
            if v < 1 << (8 * n + 7 - n):
                return (bytes([((0xFF << (8 - n)) & 0xFF) | (v >> (8 * n))])
                        + (v & ((1 << 8 * n) - 1)).to_bytes(n, "little"))
        return b"\xff" + v.to_bytes(8, "little")
    dictionary = 1 << 20
    prop = next(p for p in range(41) if dictionary <= (2 | (p & 1)) << (p // 2 + 11))
    data = b"".join(body for _, body in members)
    packed = lzma.compress(data, format=lzma.FORMAT_RAW,
                           filters=[{"id": lzma.FILTER_LZMA2, "dict_size": dictionary}])
    names = b"".join(name.encode("utf-16-le") + b"\0\0" for name, _ in members)
    header = (b"\x01\x04"                                                     # Header, MainStreamsInfo
              + b"\x06" + num(0) + num(1) + b"\x09" + num(len(packed)) + b"\x00"  # PackInfo
              + b"\x07\x0b" + num(1) + b"\x00"                                # UnPackInfo, one folder
              + num(1) + b"\x21\x21" + num(1) + bytes([prop])                   # one coder: LZMA2
              + b"\x0c" + num(len(data)) + b"\x00"
              + b"\x08\x0d" + num(len(members))                                 # SubStreamsInfo
              + b"\x09" + b"".join(num(len(body)) for _, body in members[:-1])
              + b"\x0a\x01" + b"".join(struct.pack("<I", zlib.crc32(body)) for _, body in members)
              + b"\x00\x00"
              + b"\x05" + num(len(members))                                      # FilesInfo
              + b"\x11" + num(len(names) + 1) + b"\x00" + names + b"\x00"
              + b"\x00")
    start = struct.pack("<QQI", len(packed), len(header), zlib.crc32(header))
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + packed + header


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
