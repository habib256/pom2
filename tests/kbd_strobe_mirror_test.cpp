// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Keyboard-strobe mirror decode + IIe status-read sentinel regression.
//
// II/II+: the whole $C010-$C01F page is the strobe-clear mirror — MAME
// `apple2.cpp:548` maps $C010 with `.mirror(0xf)`, read OR write. The II
// decodes only A4-A7 in this page; there are no IIe status registers.
// POM2 used to answer $C011/$C012 with IIe-style LC status on II+ and
// left the strobe set — software acking a key via `STA $C01x` (x != 0)
// hung in its key-poll loop.
//
// IIe: WRITES to any of $C010-$C01F clear the strobe (MAME `apple2e.cpp`
// c000_w: `if ((offset & 0xf0) == 0x10) m_strobe = 0;`); READS of
// $C011-$C01F are status-only and must NOT clear it.
//
// Sentinel regression: iieReadStatus used in-band 0xFE for "not a status
// register". But 0x80 | transchar($7E '~') == 0xFE is a legitimate ON
// reading — the collision sent RDRAMRD-class polls to the floating bus,
// reporting the switch OFF while it was ON. The flag is out-of-band now.

#include "Memory.h"
#include "KeyChord.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

bool strobeHigh(Memory& mem) { return (mem.memRead(0xC000) & 0x80) != 0; }

}  // namespace

int main()
{
    // Bagman reads $C000 then writes $C010 every frame. A held direction
    // must remain high on subsequent $C010 reads until its release event.
    {
        Memory mem;
        mem.setIIEMode(true);
        mem.setKeyHeld('L', true);
        mem.queueKey('L');
        if (pom2::keychord::routeHostKey(true, false, true))
            mem.setKeyHeld('L', false);
        assert(!(mem.memRead(0xC010) & 0x80) && "UI focus must not swallow a guest key release");
        mem.setKeyHeld('L', true);
        mem.queueKey('L');
        assert(mem.memRead(0xC000) == ('L' | 0x80));
        mem.memWrite(0xC010, 0);
        for (int i = 0; i < 20; ++i) {
            assert(!strobeHigh(mem));
            assert(mem.memRead(0xC010) == ('L' | 0x80));
        }
        mem.setKeyHeld('I', true);
        mem.setKeyHeld('L', false);
        assert(mem.memRead(0xC010) & 0x80);
        mem.setKeyHeld('I', false);
        assert(!(mem.memRead(0xC010) & 0x80));
        mem.setKeyHeld(' ', true);
        mem.queueKey(' ');
        assert(mem.memRead(0xC010) & 0x80);
        mem.releaseHeldKeys();
        assert(!(mem.memRead(0xC010) & 0x80));
        mem.setIIEMode(false);
        mem.setKeyHeld('L', true);
        mem.queueKey('L');
        assert(!(mem.memRead(0xC010) & 0x80));
    }

    // ── II/II+ mirror: any $C01x access clears the strobe ────────────────
    {
        Memory mem;                       // default = II/II+ mode
        assert(mem.pasteText("A") == 1);
        assert(strobeHigh(mem));
        (void)mem.memRead(0xC015);        // read a mirror, not $C010
        assert(!strobeHigh(mem) && "II+ read $C015 must clear the strobe");

        assert(mem.pasteText("B") == 1);
        assert(strobeHigh(mem));
        mem.memWrite(0xC011, 0x00);       // STA $C011 — the RWTS/game idiom
        assert(!strobeHigh(mem) && "II+ write $C011 must clear the strobe");
    }

    // ── IIe: status reads do NOT clear; writes anywhere in $C01x do ──────
    {
        Memory mem;
        mem.setIIEMode(true);
        assert(mem.pasteText("A") == 1);
        assert(strobeHigh(mem));
        (void)mem.memRead(0xC013);        // RDRAMRD — status-only
        (void)mem.memRead(0xC01F);        // RD80COL — status-only
        assert(strobeHigh(mem) && "IIe status reads must not clear the strobe");
        mem.memWrite(0xC018, 0x00);       // any $C01x write clears
        assert(!strobeHigh(mem) && "IIe write $C018 must clear the strobe");
    }

    // ── IIe sentinel collision: lastKey '~' + switch ON reads exactly $FE ─
    {
        Memory mem;
        mem.setIIEMode(true);
        assert(mem.pasteText("~") == 1);  // transchar = $7E
        mem.memWrite(0xC003, 0x00);       // RAMRD on → RDRAMRD bit 7 = 1
        const uint8_t s = mem.memRead(0xC013);
        assert(s == 0xFE &&
               "RDRAMRD with transchar=$7E must read $FE, not floating bus");
        mem.memWrite(0xC002, 0x00);       // RAMRD off
        const uint8_t s2 = mem.memRead(0xC013);
        assert(s2 == 0x7E && "RDRAMRD off keeps transchar in the low 7 bits");
    }

    std::printf("kbd_strobe_mirror OK\n");
    return 0;
}
