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

// LeChatMauveCard — the French "Le Chat Mauve" RGB video adapters (Péritel /
// SCART output) and their US-licensed cousin, the Video-7 AppleColor RGB.
//
// The card is combinational logic on the motherboard's video stream plus a
// little sequential state, and that state is ALL this class holds; the pixel
// rules live in Apple2Display's Chat Mauve renderers, which ask the card
// what to do through the three `*Mode()` queries below. What the hardware
// really does, rule by rule and with its sources, is `docs/chatmauve_plan.md`;
// this header only says what is modelled.
//
// ── Variants ─────────────────────────────────────────────────────────────
//
//   Feline      Le Chat Mauve Féline (//e aux slot). Mode latch, COL140 /
//               BW560 / mixed DHGR, LCM HGR colour, HGR mono when AN3 is
//               off. No registers, no colour text. 160 → COL140 fallback.
//   IIcAdapter  Apple's "Adaptateur IIc" on the //c DB-15 — a Féline without
//               RAM. Same behaviour today; the adapter's inferred-80COL quirk
//               (plan § P5) is where the two will part.
//   Eve         Le Chat Mauve Eve (//e aux slot). Everything the Féline does
//               EXCEPT mixed DHGR (→ COL140), plus sixteen write-only switches
//               at $C0B0-$C0BF, the CPREG colour register the card writes
//               into aux memory behind the CPU, table IX-1's graphics modes
//               (COL280A/B, CP280, HRBW, SPEC1/2, DASH), TXT16 colour text
//               with the aux nibbles the OTHER way round from Video-7's,
//               TXTGREEN, LOCKRES.
//   RvbGraph    Le Chat Mauve RVB Graph (II / II+ slot card) — PARTIAL: the
//               four documented mode strobes at its slot's device select
//               ($C0F0-$C0F3 in slot 7: colour + white text, colour + green
//               text, mono white, mono green — forum.system-cfg t=9395, via
//               a Sonotec clone). The whole-screen text colour register, the
//               HGR colour registers and the dotted-line option stay
//               unmodelled: its manual has never surfaced (plan § 3.6/P4).
//   Video7      Video-7 AppleColor RGB / Apple Extended 80-col RGB card: the
//               four patent modes including 160-wide chunky, F/B colour text
//               (aux hi nibble = foreground) and F/B HGR when AN3 is off —
//               MAME's `apple2video.cpp` rgb_monitor semantics.
//
// One catalog key ("chatmauve") covers the four: the variant is a card
// setting (`chatmauve_variant`), because the card's home is decided by the
// profile (//c-class → IIcAdapter, else Féline by default) and a dozen
// call sites reason about "the RGB card" by that one key.
//
// ── The mode latch (all variants) ────────────────────────────────────────
//
// US 4,631,692 FIG. 1: a two-bit shift register clocked by AN3, data = 80COL.
// Each $C05E→$C05F edge pushes the current 80COL level (POM2 clocks 80COL
// itself where AppleWin clocks /80COL — same hardware, inverse numbering):
//
//     $C00C,$C00C → 00 BW560      $C00D,$C00D → 11 COL140 (power-on)
//     $C00C,$C00D → 01 Mixed      $C00D,$C00C → 10 Chunky160
//
// No precondition on TEXT/MIXED/HIRES (measured on the //c adapter). The
// enum value IS the latch value; `dhgrMode()` says what the variant makes
// of it.
//
// ── The Eve's registers ($C0B0-$C0BF, Eve only) ──────────────────────────
//
// Sixteen addresses = eight switches × {off (even), on (odd)}; any access
// decodes the address, a WRITE additionally latches the data byte into
// CPREG (low nibble = dot colour, high nibble = background). All off at
// power-on; Ctrl-Reset clears them unless LOCKRES is on. Memory forwards the
// window through SlotBus::broadcastVideoSwitch{,Write} only while slot 3 is
// free (an SSC there drives its ACIA at the same addresses).
//
// CPREG's auto-write — a CPU write to MAIN text page ($0400-$07FF, TXT16 on)
// or HGR page ($2000-$3FFF, ENHRCPREG on) deposits CPREG in AUX at the same
// address, LOCKCPREG off — is a Memory write hook (`Memory::setAuxShadow`)
// the card programs whenever its switches or CPREG move. That is how
// Purplesoft's `PRINT` in colour and HPLOT in CP280 get their colours
// without the program ever touching aux.

#ifndef POM2_LE_CHAT_MAUVE_CARD_H
#define POM2_LE_CHAT_MAUVE_CARD_H

#include "SlotPeripheral.h"

#include <cstdint>
#include <string_view>

class Memory;

class LeChatMauveCard : public SlotPeripheral
{
public:
    enum class Variant : uint8_t {
        Feline     = 0,
        IIcAdapter = 1,
        Eve        = 2,
        Video7     = 3,
        RvbGraph   = 4,
    };
    static constexpr int kVariantCount = 5;

    /// The raw two-bit mode latch (value == F2 F1 as POM2 clocks 80COL).
    enum class RenderMode : uint8_t {
        BW560     = 0b00,
        Mixed     = 0b01,
        Chunky160 = 0b10,
        COL140    = 0b11,
    };

    /// What the decoder does in DHGR (AN3 off + 80COL on), after the
    /// variant's fallbacks and, on the Eve, table IX-1.
    enum class DhgrMode : uint8_t {
        BW560,      // 560 dots, black and white
        COL140,     // 140 cells of 4 dots, 16 colours
        Mixed,      // per-byte 560/140 mux over a free-running 4-dot cell latch
        Chunky160,  // Video-7 160 × 4-bit, three dots each
        COL280A,    // Eve: the 560 stream in 2-dot cells, black/orange/green/white
        COL280B,    // Eve: the 560 stream in 2-dot cells, black/lt-blue/pink/yellow
        CP280,      // Eve: 280 dots fg/bg per 7-dot byte, colours in aux (hi = bg) — with
                    // 80COL on; Purplesoft runs it with 80COL OFF (HgrMode::Cp280)
        Blank,      // Eve: HR1+HR2 — screen black, CPREG keeps working
    };

    /// What the decoder does in single HGR (AN3 on, or AN3 off with 80COL
    /// off).
    enum class HgrMode : uint8_t {
        LcmColor,   // 2-bit cell colour, 3-bit window (010/101 coloured)
        Mono,       // 280 dots black and white
        FgBg,       // Video-7 F/B: colours from aux at the same address (hi = fg)
        Spec1,      // Eve HRSPEC1: LCM minus "colour dot on white" (101 → own bit)
        Spec2,      // Eve HRSPEC2: SPEC1 minus "colour dot on black" (010 → own bit)
        Dash,       // Eve HRDASH: colour runs drawn dotted (P3 — rendered as LcmColor)
        Cp280,      // Eve CP280: AN3 off, 80COL OFF, HR1+HR2+HR3 — fg/bg from aux (hi = bg)
    };

    /// What the decoder does with TEXT.
    enum class TextMode : uint8_t {
        Plain,      // the motherboard's black and white
        Color,      // 40-col colour text, per-cell colours in aux (see auxHiNibbleIsForeground)
        Green,      // Eve TXTGREEN: white → green, 40 and 80 col
    };

    /// Eve switches, bit i of `eveSwitches()`; address = $C0B0 + 2·i (+1 = on).
    enum EveSwitch : uint8_t {
        ENHRCPREG = 0,  // CPREG also mirrors HGR writes (must be off while AN3 on)
        HR1       = 1,
        HR2       = 2,
        HR3       = 3,
        TXT16     = 4,  // 40-col colour text, hi nibble = background
        TXTGREEN  = 5,  // green monochrome text
        LOCKCPREG = 6,  // CPREG frozen — no auto-write
        LOCKRES   = 7,  // switches survive Ctrl-Reset
    };

    static constexpr int kDefaultSlot = 7;

    /// The card has no slot ROM and no device-select space of its own (it
    /// sniffs the video soft switches through the broadcast, and the Eve's
    /// window is slot 3's), so the slot number is informational — held for
    /// the UI / diagnostics panel.
    explicit LeChatMauveCard(int slot = kDefaultSlot, Variant v = Variant::Feline)
        : slot_(slot), variant_(v) {}

    int getSlot() const { return slot_; }

    /// Constant across variants: Memory::chatMauveBlockedBySlot3 and the
    /// slot-configuration coordinator identify the card by this name.
    std::string_view name() const override { return "Le Chat Mauve"; }

    // ── Variant ──────────────────────────────────────────────────────────
    Variant variant() const { return variant_; }
    void    setVariant(Variant v);
    static const char* variantKey  (Variant v);   // settings token: feline | iic | eve | video7
    static const char* variantLabel(Variant v);   // UI label
    static bool        parseVariant(std::string_view key, Variant& out);

    /// The Eve writes aux memory behind the CPU; that hook lives in Memory.
    /// Optional — a card without a Memory simply has no auto-write.
    void setMemory(Memory* mem);

    // ── Bus ──────────────────────────────────────────────────────────────
    /// Apple II RESET: re-arms the latch to COL140; on the Eve clears the
    /// sixteen switches unless LOCKRES is on. CPREG is not a switch and
    /// keeps its value.
    void onReset() override;
    /// Leaving the bus disarms the aux shadow it may have programmed into
    /// Memory (a slot rebuild must not leave two RAM pages diverted).
    void onUnplug() override;

    /// System soft-switch broadcast from Memory::softSwitchAccess() and the
    /// $C0B0-$C0BF forward. Reads and writes both land here; a write also
    /// arrives through onVideoSoftSwitchWrite with its data byte.
    void onVideoSoftSwitch(uint16_t addr) override;
    void onVideoSoftSwitchWrite(uint16_t addr, uint8_t value) override;
    /// RVB Graph only: the four mode strobes live in the card's own
    /// device-select window (any access decodes, like the Apple soft
    /// switches). Other variants keep the base no-op.
    uint8_t deviceSelectRead (uint8_t low4) override;
    /// No slot EPROM: $Cn00-$CnFF must keep reading the floating bus.
    uint8_t slotRomRead(uint8_t /*low8*/) override { return openBus(); }
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    /// RVB Graph mode register: 0 colour + white text, 1 colour + green
    /// text, 2 mono white, 3 mono green ($C0F0-$C0F3 at slot 7).
    uint8_t rvbMode() const { return rvbMode_; }

    // Rewind/snapshot: the guest-visible state. v3 = v2 + the Eve's switch
    // byte and CPREG (guest-written through $C0Bx, so guest-volatile).
    // The variant and invertBit7 are user settings and stay out.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ── What the renderers ask ───────────────────────────────────────────
    RenderMode currentMode() const { return mode; }          // raw latch
    DhgrMode   dhgrMode() const { return dhgrModeFor(mode); }
    /// The same decode for an EXPLICIT latch value — the beam-raced replay
    /// paints each band with the latch of ITS moment (latchBefore), not the
    /// end-of-frame value.
    DhgrMode   dhgrModeFor(RenderMode latch) const;
    HgrMode    hgrMode(bool an3On) const;                    // 80COL off, or AN3 on
    TextMode   textMode(bool eightyCol, bool an3On) const;
    /// Colour-text / CP280 / F/B nibble order: Video-7 puts the foreground in
    /// the high nibble, the Eve the background (manual IV-2.2, III-2).
    bool auxHiNibbleIsForeground() const { return variant_ == Variant::Video7; }
    /// Everything that changes pixels without a video event, folded into one
    /// integer for Apple2Display's frame key.
    uint32_t renderStateKey() const;

    // Read-only accessors used by the UI panel.
    uint8_t fifoBits()    const { return fifo; }
    bool    eightyCol()   const { return eightyColLatched; }
    bool    an3High()     const { return an3Prev; }
    uint8_t eveSwitches() const { return eveSwitches_; }
    bool    eveSwitch(EveSwitch s) const { return (eveSwitches_ >> s) & 1u; }
    uint8_t cpreg()       const { return cpreg_; }

    /// The latch value in force just BEFORE `cycle` — served from a small
    /// ring of (cycle, fifo) pairs appended at every clock edge (timestamped
    /// through `mem_`; without a Memory the ring stays empty and the current
    /// latch is returned, which is the pre-P6 behaviour). This is what lets
    /// a mid-frame `$C05E/$C05F` land on the right scanline: the replay asks
    /// for the frame-start value and walks the same logged edges forward.
    RenderMode latchBefore(uint64_t cycle) const;

    /// UI override: force a latch value independent of the bus. Resets back
    /// to the bus-driven value on the next AN3 rising edge.
    void overrideMode(RenderMode m) { mode = m; fifo = static_cast<uint8_t>(m); }
    /// UI / tests: poke an Eve switch as a `STA $C0Bx` would (Eve only).
    void setEveSwitch(EveSwitch s, bool on);
    void setCpreg(uint8_t v);

    // Dragon Wars compatibility: a handful of titles encode their DHGR
    // mixed-mode bit 7 the other way round from the patent (colour where the
    // hardware expects mono, and vice-versa). XORs bit 7 at decode time in
    // the mixed mode; matches AppleWin's `-rgb-card-invert-bit7`. Off =
    // strict patent semantics (default). Not applied to the HGR bank bit.
    void setInvertBit7(bool v) { invertBit7_ = v; }
    bool invertBit7() const    { return invertBit7_; }

private:
    int        slot_;
    Variant    variant_;
    Memory*    mem_             = nullptr;
    bool       an3Prev          = true;     // AN3 powers up HIGH (DHIRES off)
    bool       eightyColLatched = false;    // last seen 80COL level
    uint8_t    fifo             = 0b11;     // 2 bits, MSB shifted out
    RenderMode mode             = RenderMode::COL140;
    bool       invertBit7_      = false;
    uint8_t    eveSwitches_     = 0;        // all off at power-on
    uint8_t    cpreg_           = 0;
    uint8_t    rvbMode_         = 0;        // RVB Graph: colour + white text
    // Rolling latch history for beam-raced replay (latchBefore). 512 edges
    // is > one PAL frame of per-scanline clocking.
    struct LatchEdge { uint64_t cycle; uint8_t before; uint8_t after; };
    bool       mixedWarned_ = false;   // one log per plug/variant change
    static constexpr int kLatchRing = 512;
    LatchEdge  latchRing_[kLatchRing] {};
    int        latchRingHead_ = 0;          // next write slot
    int        latchRingSize_ = 0;

    bool isEve() const { return variant_ == Variant::Eve; }
    void clockFifo(bool dataBit);
    void eveDecode(uint16_t addr);
    void syncAuxShadow();
};

#endif // POM2_LE_CHAT_MAUVE_CARD_H
