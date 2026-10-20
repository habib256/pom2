#!/usr/bin/env python3
"""Create a separate Nox Archaist v1.37 HDV compatible with the //c + MB4c.

Usage: python3 tools/make_nox_mb4c_fix.py INPUT.hdv OUTPUT.hdv

The publisher documents that MB4c needs a game patch:
https://www.6502workshop.com/p/playing-nox-archaist-on-real-hardware.html
This is POM2's independently tested compatibility patch, not that official patch.

The title's LOGO program enables the //c VBL interrupt for polling while SEI
is set. MOCK.DRIVER then executes CLI without disabling VBL. Its handler only
acknowledges the VIA, leaving IRQ_SRC_VBL asserted: the player runs repeatedly
without returning to the loader. Neither the VIA nor the CPU is at fault.

Disable VBL immediately before the player's CLI. The loader's final VBL wait
must subsequently re-arm it (through its own existing $08B1 routine), before
its existing $086A-$0877 cleanup acknowledges/disables VBL and executes CLI.
The additional driver code occupies unused auxiliary $03D0-$03DE, beyond the
player's state at $03B3-$03C7, inside its existing 256-byte code copy.

Both complete input files are fingerprinted. Only their existing allocated
blocks and MOCK.DRIVER's EOF change; no game data or allocation map is changed.
The input is never written and an existing output is never overwritten.
"""

import argparse
import hashlib
from pathlib import Path
import sys

BLOCK = 512
DRIVER_SHA = "4ae0b6b048b3af977d959ae99c8d84bd7b2a46e3a394e68e3aed39c28dd26a04"
LOGO_SHA = "ccce12f1a4ef2cb6797c994c5590398ed7cadd7b4bde267b7a51168acb1dbf5e"


def block_offset(image, number):
    if not 0 < number < len(image) // BLOCK:
        raise ValueError(f"Invalid ProDOS block {number}")
    return number * BLOCK


def find_entry(image, directory, name):
    visited = set()
    while directory:
        if directory in visited:
            raise ValueError("Cyclic ProDOS directory")
        visited.add(directory)
        base = block_offset(image, directory)
        for i in range(13):
            offset = base + 4 + 39 * i
            entry = image[offset:offset + 39]
            if entry[0] >> 4 in (1, 2, 13) and entry[1:1 + (entry[0] & 15)] == name:
                return offset
        directory = int.from_bytes(image[base + 2:base + 4], "little")
    raise ValueError(f"ProDOS entry {name.decode()} not found")


def read_sapling(image, entry):
    if image[entry] >> 4 != 2:
        raise ValueError("Expected a ProDOS sapling file")
    eof = int.from_bytes(image[entry + 21:entry + 24], "little")
    if eof > 256 * BLOCK:
        raise ValueError("Sapling file too large")
    index = block_offset(image, int.from_bytes(image[entry + 17:entry + 19], "little"))
    blocks = []
    for i in range((eof + BLOCK - 1) // BLOCK):
        number = image[index + i] | image[index + 256 + i] << 8
        blocks.append(block_offset(image, number))
    if len(set(blocks)) != len(blocks) or index in blocks:
        raise ValueError("Overlapping ProDOS file blocks")
    return bytearray(b"".join(image[p:p + BLOCK] for p in blocks)[:eof]), blocks


def patch_image(original):
    if len(original) % BLOCK or len(original) < 3 * BLOCK:
        raise ValueError("Expected a raw ProDOS HDV image")
    image = bytearray(original)
    na = find_entry(image, 2, b"NA")
    directory = int.from_bytes(image[na + 17:na + 19], "little")
    driver_entry = find_entry(image, directory, b"MOCK.DRIVER")
    logo_entry = find_entry(image, directory, b"LOGO")
    driver, driver_blocks = read_sapling(image, driver_entry)
    logo, logo_blocks = read_sapling(image, logo_entry)
    for name, data, expected in (("MOCK.DRIVER", driver, DRIVER_SHA), ("LOGO", logo, LOGO_SHA)):
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Unrecognized {name}; expected the original v1.37 file")
    if set(driver_blocks) & set(logo_blocks):
        raise ValueError("Overlapping game files")

    # AUX $03A4: CLI / BNE $035B -> JMP $03D0.
    driver[0x26E:0x271] = bytes.fromhex("4C D0 03")
    driver.extend(bytes(0x29A - len(driver)))
    driver.extend(bytes.fromhex(
        "48 "          # PHA
        "8D 7F C0 "    # STA CLRIOUDIS
        "8D 5A C0 "    # STA DISVBL (also clears a pending VBL request)
        "8D 7E C0 "    # STA SETIOUDIS
        "68 58 "       # PLA / CLI (A=$26, same N/Z and carry as before)
        "4C 5B 03"     # JMP $035B (original taken BNE destination)
    ))
    # MAIN $0865: use the loader's own SEI/enable/poll routine, then its
    # original acknowledge/disable/CLI epilogue. No additional code space.
    logo[0x65:0x6A] = bytes.fromhex("20 B1 08 EA EA")

    for data, blocks in ((driver, driver_blocks), (logo, logo_blocks)):
        if len(data) > len(blocks) * BLOCK:
            raise ValueError("Patch would require another allocated block")
        for i, offset in enumerate(blocks):
            chunk = data[i * BLOCK:(i + 1) * BLOCK]
            image[offset:offset + len(chunk)] = chunk
    image[driver_entry + 21:driver_entry + 24] = len(driver).to_bytes(3, "little")
    return bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        patched = patch_image(args.input.read_bytes())
        with args.output.open("xb") as output:
            output.write(patched)
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    print(f"Created {args.output} (original preserved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
