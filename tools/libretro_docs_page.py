#!/usr/bin/env python3
"""Write POM2's page for docs.libretro.com (libretro/docs, docs/library/pom2.md).

Everything on the page comes from the core itself, so it cannot drift: the
core options with their categories, labels and defaults as the core declares
them to RetroArch (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL), the
controller types, the input descriptors, the geometry and timing, and the
firmware list of libretro/pom2_libretro.info (md5 from the dumps in roms/
when present). The layout follows libretro's docs/meta/core-template.md.

  python3 tools/libretro_docs_page.py build/pom2_libretro.dylib roms > pom2.md
"""

import ctypes as C
import hashlib
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from libretro_frontend import Frontend, prepare_system  # noqa: E402

MAX_VALUES = 128


class Value(C.Structure):
    _fields_ = [("value", C.c_char_p), ("label", C.c_char_p)]


class Definition(C.Structure):
    _fields_ = [("key", C.c_char_p), ("desc", C.c_char_p), ("desc_categorized", C.c_char_p),
                ("info", C.c_char_p), ("info_categorized", C.c_char_p), ("category_key", C.c_char_p),
                ("values", Value * MAX_VALUES), ("default_value", C.c_char_p)]


class Category(C.Structure):
    _fields_ = [("key", C.c_char_p), ("desc", C.c_char_p), ("info", C.c_char_p)]


class OptionsV2(C.Structure):
    _fields_ = [("categories", C.POINTER(Category)), ("definitions", C.POINTER(Definition))]


class OptionsV2Intl(C.Structure):
    _fields_ = [("us", C.POINTER(OptionsV2)), ("local", C.POINTER(OptionsV2))]


class ControllerDescription(C.Structure):
    _fields_ = [("desc", C.c_char_p), ("id", C.c_uint)]


class ControllerInfo(C.Structure):
    _fields_ = [("types", C.POINTER(ControllerDescription)), ("num_types", C.c_uint)]


class InputDescriptor(C.Structure):
    _fields_ = [("port", C.c_uint), ("device", C.c_uint), ("index", C.c_uint), ("id", C.c_uint),
                ("description", C.c_char_p)]


def text(raw):
    return raw.decode("utf-8") if raw else ""


class DocsFrontend(Frontend):
    """Answers as a v2 core-options frontend and records what the core declares."""

    def __init__(self, *args, **kwargs):
        self.categories, self.definitions, self.ports, self.descriptors = [], [], [], []
        super().__init__(*args, **kwargs)

    def environment(self, command, data):
        if command == 52:  # GET_CORE_OPTIONS_VERSION
            C.cast(data, C.POINTER(C.c_uint))[0] = 2
            return True
        if command in (67, 68):  # SET_CORE_OPTIONS_V2[_INTL]
            options = (C.cast(data, C.POINTER(OptionsV2Intl))[0].us if command == 68
                       else C.cast(data, C.POINTER(OptionsV2)))[0]
            self.categories, self.definitions = [], []
            i = 0
            while options.categories and options.categories[i].key:
                c = options.categories[i]
                self.categories.append((text(c.key), text(c.desc), text(c.info)))
                i += 1
            i = 0
            while options.definitions[i].key:
                d = options.definitions[i]
                values = []
                for v in d.values:
                    if not v.value:
                        break
                    values.append((text(v.value), text(v.label) or text(v.value)))
                self.definitions.append({"key": text(d.key), "desc": text(d.desc_categorized) or text(d.desc),
                                         "info": text(d.info_categorized) or text(d.info),
                                         "category": text(d.category_key), "values": values,
                                         "default": text(d.default_value)})
                i += 1
            return True
        if command == 35:  # SET_CONTROLLER_INFO
            infos = C.cast(data, C.POINTER(ControllerInfo))
            i = 0
            while infos[i].types:
                self.ports.append([text(infos[i].types[j].desc) for j in range(infos[i].num_types)])
                i += 1
            return True
        if command == 11:  # SET_INPUT_DESCRIPTORS
            items = C.cast(data, C.POINTER(InputDescriptor))
            i = 0
            self.descriptors = []
            while items[i].description:
                d = items[i]
                self.descriptors.append((d.port, d.device, d.index, d.id, text(d.description)))
                i += 1
            return True
        return super().environment(command, data)


def info_fields(path):
    fields = {}
    for line in path.read_text().splitlines():
        m = re.match(r'^(\w+)\s*=\s*(?:"(.*)"|(\S+))\s*$', line)
        if m:
            fields[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
    return fields


PAD = {0: "B", 1: "Y", 2: "Select", 3: "Start", 4: "D-Pad Up", 5: "D-Pad Down", 6: "D-Pad Left",
       7: "D-Pad Right", 8: "A", 9: "X", 10: "L", 11: "R", 12: "L2", 13: "R2", 14: "L3", 15: "R3"}


def main():
    core, roms = Path(sys.argv[1]), Path(sys.argv[2])
    info = info_fields(ROOT / "libretro" / "pom2_libretro.info")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        prepare_system(work / "system", roms)
        host = DocsFrontend(core, work / "system", work / "saves", {})
        try:
            av = host.load(None)
        finally:
            host.close()

    name = info["display_name"]
    out = []
    w = out.append
    w('---')
    w(f'title: "POM2 – Apple II core for RetroArch"')
    w(f'description: "RetroArch POM2 core (Apple II): firmware, core options and controls. {info["description"].split(".")[0]}."')
    w('---')
    w('')
    w(f'# {name}')
    w('')
    w('## Background')
    w('')
    w(info["description"])
    w('')
    w('POM2 aims at the real machine: every hardware claim in its source carries the grade of the evidence '
      'behind it (a measurement on the iron, a data sheet, a ROM disassembly, another emulator). On 4am\'s '
      'woz-a-day collection of copy-protected disks (2245 titles) the core boots 99.3 % to a picture '
      '([audit](https://github.com/habib256/pom2/blob/main/docs/audits/woz-a-day-2026-11-24.md)).')
    w('')
    w('The POM2 core has been authored by')
    w('')
    w(f'- {info["authors"]}')
    w('')
    w('The POM2 core is licensed under')
    w('')
    w(f'- {info["license"]}')
    w('')
    w('A summary of the licenses behind RetroArch and its cores can be found [here](../development/licenses.md).')
    w('')
    w('## BIOS')
    w('')
    w('[Required or optional firmware files](https://docs.libretro.com/library/bios/) go in the frontend\'s '
      'system directory, in a `pom2` folder. Each machine needs its own system ROM; the Disk II PROMs are '
      'optional (POM2 has its own 16- and 13-sector boot PROMs and its own disk sequencer).')
    w('')
    w('| Filename | Description | md5sum |')
    w('|---|---|---|')
    for i in range(int(info.get("firmware_count", "0"))):
        path = info[f"firmware{i}_path"]
        local = roms / Path(path).name
        md5 = hashlib.md5(local.read_bytes()).hexdigest() if local.is_file() else ""
        optional = "Optional" if info.get(f"firmware{i}_opt") == "true" else "Required"
        w(f'| {path} | {info[f"firmware{i}_desc"]} - {optional} | {md5} |')
    w('')
    w('## Extensions')
    w('')
    w('Content that can be loaded by the POM2 core have the following file extensions:')
    w('')
    for ext in info["supported_extensions"].split("|"):
        w(f'- .{ext}')
    w('')
    w('The POM2 core can also be started without content: the machine starts at its BASIC prompt.')
    w('')
    w('RetroArch database(s) that are associated with the POM2 core:')
    w('')
    w(f'- [{info["database"]}](https://github.com/libretro/libretro-database/blob/master/rdb/Apple%20-%20II.rdb)')
    w('')
    w('## Features')
    w('')
    w('Frontend-level settings or features that the POM2 core respects.')
    w('')
    w('| Feature           | Supported |')
    w('|-------------------|:---------:|')
    features = [("Restart", True), ("Saves", True), ("States", True), ("Rewind", True), ("Netplay", True),
                ("Core Options", True), ("RetroAchievements", True), ("RetroArch Cheats", True),
                ("Native Cheats", False), ("Controls", True), ("Remapping", True), ("Multi-Mouse", False),
                ("Rumble", False), ("Sensors", False), ("Camera", False), ("Location", False),
                ("Subsystem", False), ("[Softpatching](../guides/softpatching.md)", False),
                ("Disk Control", True), ("Username", False), ("Language", True), ("Crop Overscan", False),
                ("LEDs", True)]
    for feature, ok in features:
        w(f'| {feature:<17} | {"✔" if ok else "✕"}         |')
    w('')
    w('Netplay is checked frame by frame in POM2\'s CI: the same state on both machines, exact rollbacks, '
      'a joining player in step. For games played at the keyboard, set Port 3 to "Keyboard (for netplay)": '
      'RetroArch\'s netplay carries a keyboard only as a port\'s device.')
    w('')
    w('### Directories')
    w('')
    w(f'The POM2 core\'s library name is \'{info["corename"]}\'')
    w('')
    w('The POM2 core saves/loads to/from these directories.')
    w('')
    w('**Frontend\'s Save directory**')
    w('')
    w('| File | Description |')
    w('|---|---|')
    w('| pom2/disks/... | A private writable copy of each disk the game writes to; the original image is never written |')
    w('')
    w('**Frontend\'s State directory**')
    w('')
    w('| File | Description |')
    w('|---|---|')
    w('| *.state# | State |')
    w('')
    w('## Geometry and timing')
    w('')
    w(f'- The POM2 core\'s core provided FPS is {av["fps"]:.3f} for NTSC machines (50.080 for PAL)')
    w(f'- The POM2 core\'s core provided sample rate is {av["sample_rate"]:.0f} Hz')
    w(f'- The POM2 core\'s base width is {av["base_size"][0]}')
    w(f'- The POM2 core\'s base height is {av["base_size"][1]}')
    w('')
    w('## Core options')
    w('')
    w('The POM2 core has the following option(s) that can be tweaked from the core options menu. '
      'The default setting is bolded. Options marked (restart) take effect at the next Load Content or Restart.')
    w('')
    for key, desc, info_text in host.categories:
        w(f'#### {desc}')
        w('')
        if info_text:
            w(info_text)
            w('')
        for d in (d for d in host.definitions if d["category"] == key):
            values = "|".join(f'**{label}**' if value == d["default"] else label for value, label in d["values"])
            w(f'- **{d["desc"]}** [{d["key"]}] ({values})')
            w('')
            if d["info"]:
                w(f'    {d["info"]}')
                w('')
    w('## Controllers')
    w('')
    w('The POM2 core supports the following device type(s) in the controls menu, bolded device types are '
      'the default for the specified user(s).')
    w('')
    for port, types in enumerate(host.ports, 1):
        w(f'### User {port} device types')
        w('')
        for i, t in enumerate(types):
            w(f'- {"**" + t + "**" if i == 0 else t}')
        w('')
    w('The keyboard types into the machine directly (the keyboard callback), on the host\'s layout. Port 3\'s '
      '"Keyboard (for netplay)" reads it as a device instead, on a US layout.')
    w('')
    w('## Joypad')
    w('')
    w('| RetroPad Inputs | User 1 input descriptors |')
    w('|---|---|')
    for port, device, index, ident, desc in host.descriptors:
        if port == 0 and device == 1:
            w(f'| {PAD.get(ident, ident)} | {desc} |')
    w('')
    w('## External Links')
    w('')
    w('- [POM2 Repository](https://github.com/habib256/pom2)')
    w('- [Core installation, firmware and controls](https://github.com/habib256/pom2/blob/main/libretro/README.md)')
    w('- [Report POM2 Core Issues Here](https://github.com/habib256/pom2/issues)')
    w('')
    w('## See also')
    w('')
    w('### Apple II')
    w('')
    w('- [Apple II (AppleWin)](applewin.md)')
    w('- [Arcade (MAME - Current)](mame.md)')
    print("\n".join(out))


if __name__ == "__main__":
    main()
