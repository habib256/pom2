// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
// Game-port bit 7, including the cassette-input mirror at $C068.
// Called by softSwitchAccess with the low address resolved to $60-$67;
// the caller adds the floating bus on the remaining seven bits.
#include "Memory.h"
#include "CassetteDevice.h"

uint8_t Memory::gamePortReadBit7(uint8_t mirrored)
{
    if (joyportActive()) {
        if (mirrored >= 0x61 && mirrored <= 0x63)
            return joyport_.readButton(mirrored - 0x61, display.an0, an1);
        // No potentiometers in this Atari-only configuration.
        if (mirrored >= 0x64 && mirrored <= 0x67) return 0x80;
    }
    switch (mirrored) {
        case 0x61: return paddles_.button0(!iieMode) ? 0x80 : 0x00;
        case 0x62: return paddles_.button1(!iieMode) ? 0x80 : 0x00;
        // Shift-key mods: //e (not //c) and ][/][+ wirings, PaddleInputs.h.
        case 0x63: return paddles_.button2(iieMode && !iicProfile_, !iieMode) ? 0x80 : 0x00;
        case 0x64: case 0x65: case 0x66: case 0x67: {
            const int idx = mirrored - 0x64;
            if (const int w = paddles_.wiredPaddle(idx, !iieMode); w >= 0) return w ? 0x80 : 0x00;
            return paddles_.discharging(idx, stockSincePaddleStrobe(idx)) ? 0x80 : 0x00;
        }
        case 0x60:
            // $C068 mirrors $C060 (cassette comparator) per the
            // same `.mirror(0x8)` cited above — only a literal
            // $C060 access takes the dedicated branch earlier.
            // Returning a hard 0 here clamped bit 7 low, so a
            // tape-read loop polling $C068 never saw the
            // comparator flip and entropy loops keyed on N were
            // deterministic.
            // Fenced off the //c the same way the literal $C060
            // branch above is — the mirror must not be the back
            // door into a deck the machine does not have.
            return (cassette && !iicProfile_)
                ? static_cast<uint8_t>(cassette->readTapeInput() & 0x80)
                : static_cast<uint8_t>(iicProfile_ ? 0x00 : 0x80);
        default: return 0;  // unreachable: mirrored ∈ $60-$67
    }
}
