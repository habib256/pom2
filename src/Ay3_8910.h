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

// Ay3_8910 — GI/Microchip AY-3-8910 / AY-3-8913 PSG register bank +
// VIA-side control bus decoder. The audio synthesis state (counters,
// LFSR, envelope step) lives on the audio thread inside each card's
// AudioSrc — NOT here. This struct is touched by both threads; the
// owning card serialises access with a mutex.
//
// Extracted 2026-05-27 from MockingboardCard's private nested struct so
// PhasorCard can drive four instances over the same bus contract.
//
// AY-3-8910 / 8912 / 8913: identical synthesis core; the 8912 has one
// I/O port, the 8913 none. The difference IS observable, on read-back —
// [A] mb-audit issue #22 (tomcw/mb-audit, opened 2024-09-15; measurements
// in the comments of 2026-06-06, 07-26 and 08-19, by Tom Charlesworth) with
// the sequence R7=$FF, R14=$55, R15=$69 then read back:
//   AY-3-8913 (ReactiveMicro Mockingboard; both Phasor AYs): 3F 0E 0F
//   AY-3-8910 (MB-K), two Microchip AY38912A/P pulled from a Mockingboard
//     4c, and a GI AY-3-8912 (date code 8048): FF 55 69, and FF FF with
//     both ports set as INPUTS (R7=$3F).
// [B] 1982 GI Microelectronics Data Catalog (bitsavers, components/gi/
// _dataBooks/), p. 5-19: "The I/O Ports are TTL-compatible and are provided
// with internal pull-ups on each pin. Both Ports are available on the
// AY-3-8910; only I/O Port A is available on the AY-3-8912; no ports are
// available on the AY-3-8913" — and p. 5-22 (R16/R17): "none are available
// on the AY-3-8913".
// [D] AppleWin d5db8e84 (2026-07-18) has the 8913 right (R7 & $3F, R14 →
// $0E, R15 → $0F) but gives the 8912 R7 & $7F and R15 → $0F, which both
// measured 8912s contradict: an 8912 reads back like an 8910. AMONT FAUX.
// Every POM2 slot card carries 8913s (Mockingboard A/C, Sound II, Phasor,
// Echo+); the Mockingboard 4c carries an 8912. `chip` says which. Until
// 2026-11-01 POM2 returned the stored byte for every chip — AUDIO-010 was
// "closed with no change, matching MAME": a grade-D alignment against a
// grade-A measurement, and mb-audit flagged every AY as an 8910.
// `docs/audits/parite-ay8910-2026-11-01.md`.
//
// PB → AY control bus map (Mockingboard A/C + Phasor):
//   PB0 → BC1
//   PB1 → BDIR
//   PB2 → /RESET (active LOW)
// Command encoding (BDIR, BC1):
//   00  INACTIVE   — no bus action
//   01  READ       — rare; music drivers don't read
//   10  WRITE      — write PA byte to latched register
//   11  LATCH ADDR — latch PA[3:0] as the next register address

#ifndef POM2_AY3_8910_H
#define POM2_AY3_8910_H

#include "ByteIO.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pom2 {

struct Ay3_8910
{
    static constexpr int kAyNumRegs = 16;

    /// Which part sits in the socket — see the header comment for what each
    /// one reads back. Configuration, not state: not in the snapshot.
    enum class Chip : uint8_t { Ay8910, Ay8912, Ay8913 };
    Chip chip = Chip::Ay8913;

    // PB bit positions for AY control bus.
    static constexpr uint8_t kPbBitBc1   = 0x01;
    static constexpr uint8_t kPbBitBdir  = 0x02;
    static constexpr uint8_t kPbBitReset = 0x04;

    uint8_t regs[kAyNumRegs] = {0};
    /// Low nibble: the register latch. Bit 7 (`kDeselected`): the last
    /// address latched had a non-zero high nibble, so the chip is not
    /// selected — the AY-3-8910's mask-programmed 4-bit code is 0000 (GI
    /// datasheet; MAME `ay8910_write_ym`, `m_active`). Kept in this byte so
    /// the snapshot layout does not move; every user masks with 0x0F.
    static constexpr uint8_t kDeselected = 0x80;
    uint8_t latchedAddr = kDeselected;

    // PB control state captured on the last VIA strobe — for transition
    // detection in applyControl.
    uint8_t prevCommand = 0;     // {BDIR, BC1} as a 2-bit command

    // Per-command counters (diagnostic — surfaced via card peek APIs).
    // Tick only on real {BDIR,BC1} edges so a held strobe reports once.
    uint32_t latchCount       = 0;
    uint32_t writeStrobeCount = 0;
    uint32_t readStrobeCount  = 0;
    uint32_t inactiveCount    = 0;

    void reset()
    {
        // MAME `ay8910.cpp ay8910_reset_ym` clears regs 0..AY_PORTA-1
        // (= 0..13) and leaves R14/R15 untouched. Mockingboard / Phasor
        // wire R14/R15 unused so this is academic, but `getAyRegister`
        // peeks would diverge from MAME if we wiped all 16.
        std::memset(regs, 0, 14);
        // MAME ay8910.cpp:1305-1308 (a2b6ba2d4be7): reset clears
        // m_active. No data-bus access is accepted until an address latch.
        latchedAddr = kDeselected;
        prevCommand = 0;
    }

    // ─── Snapshot (rewind) ───────────────────────────────────────────────
    // The whole audible state of this register-model AY is its 16 registers
    // plus the control-bus latch, so a snapshot restores the chip exactly.
    static constexpr std::size_t kSnapshotBytes = kAyNumRegs + 2 + 16;
    inline void appendSnapshot(std::vector<uint8_t>& o) const
    {
        for (int i = 0; i < kAyNumRegs; ++i) byteio::putU8(o, regs[i]);
        byteio::putU8(o, latchedAddr);
        byteio::putU8(o, prevCommand);
        byteio::putU32(o, latchCount);       byteio::putU32(o, writeStrobeCount);
        byteio::putU32(o, readStrobeCount);  byteio::putU32(o, inactiveCount);
    }
    inline void loadSnapshot(const uint8_t* d)   // caller ensures >= kSnapshotBytes
    {
        byteio::Reader r(d, kSnapshotBytes);
        for (int i = 0; i < kAyNumRegs; ++i) regs[i] = r.u8();
        latchedAddr      = r.u8();
        prevCommand      = r.u8();
        latchCount       = r.u32(); writeStrobeCount = r.u32();
        readStrobeCount  = r.u32(); inactiveCount    = r.u32();
    }

    enum ApplyResult { NoChange, ResetOnly, Wrote, Read };

    /// Value the chip drove onto the data bus on the last READ command
    /// (BDIR=0, BC1=1). The card latches it onto the VIA's port-A input,
    /// mirroring MAME's `m_porta` shadow.
    ///
    /// Deliberately NOT in the snapshot above, though every other field is:
    /// it is written and consumed inside one `applyControl` call (the card
    /// does `setPortAInput(ay->busOut)` the instant the call returns `Read`)
    /// and recomputed from `regs` on the next READ. What survives between
    /// calls is the VIA's `portAIn`, which the VIA's own v2 snapshot carries.
    /// Adding it here would grow `kSnapshotBytes` and shift every field of
    /// every existing Mockingboard / Phasor blob for no observable gain.
    uint8_t busOut = 0;

    /// React to a VIA Port B (and, on Latch/Write commands, also Port A)
    /// change. `pa` is the AY data bus (VIA Port A output bits driven by
    /// DDRA), `pb` is the VIA Port B output after DDRB masking.
    /// !RESET (PB2) is active-low: while held low the AY stays in reset
    /// and every register reads zero. Reported as `ResetOnly` so the
    /// diagnostic panel can separate "music driver clearing the chip"
    /// from "music driver delivered a register-store strobe".
    ApplyResult applyControl(uint8_t pa, uint8_t pb)
    {
        if ((pb & kPbBitReset) == 0) {
            reset();
            return ApplyResult::ResetOnly;
        }
        const uint8_t cmd = static_cast<uint8_t>(
            ((pb & kPbBitBdir) ? 0x02 : 0) |
            ((pb & kPbBitBc1)  ? 0x01 : 0));
        // [B] R6522 pin logic: the card calls this only on a PA or PB PIN
        //     edge (Via6522::write), so a same-value ORB store never gets
        //     here (audit parite-queue-audio, 2026-11-01).
        // [D] MAME a2mockingboard.cpp:391-410 @ a2b6ba2d — via_psg_ctrl runs
        //     on every ORB store (6522via.cpp:847-853); a PA store only
        //     parks m_porta1 (:436-439). AppleWin Mockingboard.cpp:366-437
        //     @ 6c579da5 — a function executes only from INACTIVE (GH#320).
        // [F] Unknown: what the AY does when PA changes under a HELD LATCH
        //     or WRITE. POM2 re-strobes the latched register with each new
        //     data byte (the level-sensitive reading); MAME parks the byte,
        //     AppleWin ignores it. Settled by: a logic-analyser capture of
        //     DA0-7 against BDIR/BC1 on a real AY-3-8913. The edge tracking
        //     below feeds the diagnostic counters only.
        ApplyResult result = ApplyResult::NoChange;
        const bool edge = (cmd != prevCommand);
        switch (cmd) {
        case 0b11:    // LATCH ADDR
            if (edge) ++latchCount;
            // A high nibble other than 0 deselects the chip and leaves the
            // register latch alone (MAME keeps m_register_latch). A stray
            // $1x latch used to select register x and the next data byte
            // landed in it as a note (bug hunt 2026-09-17).
            if ((pa >> 4) == 0)
                latchedAddr = static_cast<uint8_t>(pa & 0x0F);
            else
                latchedAddr = static_cast<uint8_t>((latchedAddr & 0x0F) | kDeselected);
            break;
        case 0b10:    // WRITE
            if (edge) ++writeStrobeCount;
            if (latchedAddr & kDeselected) break;
            regs[latchedAddr & 0x0F] = pa;
            result = ApplyResult::Wrote;
            break;
        case 0b01:    // READ
            // MAME `ay8910.cpp` drives the selected register onto the
            // data bus, and `mockingboard.cpp via_psg_ctrl` latches that
            // onto VIA port A. POM2 counted the strobe and did nothing
            // else, so a driver that probes the AY by writing then
            // READING a register back (a common presence check, and how
            // some Phasor mode detectors identify the board) always saw
            // the VIA's own stale port-A output instead.
            if (edge) ++readStrobeCount;
            // Unimplemented register bits read back as 0 — hardware-
            // confirmed per-register masks from MAME `ay8910.cpp
            // ay8910_read_ym` ("Tested and confirmed on hardware: AY-3-
            // 8910: inaccessible bits read back as 0"). Returning the raw
            // stored byte defeated the classic write-$FF-read-back probe:
            // a 4-bit register must answer $0F, or AY-vs-YM2149 detectors
            // (and Phasor mode sniffers) mis-identify the chip.
            {
                static constexpr uint8_t kReadMask[16] = {
                    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
                };
                const uint8_t r = latchedAddr & 0x0F;
                if (latchedAddr & kDeselected)
                    busOut = 0xFF;                      // deselected: bus floats high
                else if (chip == Chip::Ay8913 && r == 7)
                    busOut = regs[7] & 0x3F;            // [A] no port-direction bits to read
                else if (chip == Chip::Ay8913 && r >= 14)
                    busOut = r;                         // [A] $0E / $0F: no port behind it
                else if (r >= 14 && !(regs[7] & (r == 14 ? 0x40 : 0x80)))
                    busOut = 0xFF;                      // [A][B] input port: pull-ups
                else
                    busOut = regs[r] & kReadMask[r];
            }
            result = ApplyResult::Read;
            break;
        case 0b00:
        default:
            if (edge) ++inactiveCount;
            break;
        }
        prevCommand = cmd;
        return result;
    }
};

} // namespace pom2

#endif // POM2_AY3_8910_H
