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
//   Feline      Le Chat Mauve Féline (//e slot 3 + aux slot at once, [B]
//               Féline manual p. 8). Mode latch, COL140 / BW560 / mixed DHGR,
//               LCM HGR colour, HGR mono when AN3 is off ([B] p. 38). No
//               registers, no colour text. 160 → COL140 fallback.
//   IIcAdapter  Apple's "Adaptateur IIc" on the //c DB-15 — [E] a Féline
//               without RAM (fenarinarsa, AppleWin #764; no adapter manual
//               found). Same behaviour today; the adapter's inferred-80COL
//               quirk (plan § P5) is where the two will part.
//   Eve         Le Chat Mauve Eve (//e slot 3 + aux slot). Everything the
//               Féline does EXCEPT mixed DHGR (→ COL140; [B] Manuel Arlequin,
//               as quoted by Extasie Chat Mauve Reloaded), plus [B] eight
//               switches at the sixteen addresses $C0B0-$C0BF and the CPREG
//               colour register the card writes into aux memory behind the
//               CPU (Eve manual ch. IX, pp. 117-119), table IX-1's graphics
//               modes (COL280A/B, CP280, HRBW, SPEC1/2, DASH; p. 118), TXT16
//               colour text with the aux nibbles the OTHER way round from
//               Video-7's ([B] manual IV-2.2, III-2), TXTGREEN, LOCKRES.
//   RvbGraph    Le Chat Mauve RVB Graph (II / II+, slot 7 only). Everything
//               its "Manuel de Référence" (22 pp., chapters III-IV) gives a
//               program: the control register at $C0F0+S+U+K (data
//               16·(15−CT): the whole-screen text colour), dotted or
//               continuous colour lines (S bit 0), the four-colour system
//               (S bit 1), the HGR colour registers (U) and their lock (K),
//               which also decides whether RESET puts the card back to
//               normal. Eight write-only colour registers at $C70F-Bn,
//               $C71F-Bn, $C72F-Bn, $C73F-Bn (data 15−Cn): Cn colours an
//               isolated dot, Bn a run of 2+ by its leftmost dot ([B] the
//               manual). The silicon is its patent, FR 2 509 560 ([B],
//               the card's whole TTL schematic): it decodes the video
//               stream, reads of $C0Fx / $C7xx latch like writes (no R/W
//               gate), $C700-$C7FF mirrored every $40, its own resistor-DAC
//               palette. Rules, grades and what stays [F]:
//               docs/chatmauve_plan.md § 3.6.
//   Video7      Video-7 AppleColor RGB / Apple Extended 80-col RGB card: the
//               four patent modes including 160-wide chunky, F/B colour text
//               (aux hi nibble = foreground) and F/B HGR when AN3 is off —
//               [D] MAME `apple2video.cpp` @ e1479b7a rgb_monitor semantics.
//
// One catalog key ("chatmauve") covers the five: the variant is a card
// setting (`chatmauve_variant`), because the card's home is decided by the
// profile (//c-class → IIcAdapter, else Féline by default) and a dozen
// call sites reason about "the RGB card" by that one key.
//
// ── The mode latch (all variants) ────────────────────────────────────────
//
// [B] US 4,631,692 FIG. 1: a two-bit shift register clocked by AN3, data =
// 80COL (the next sentence: only ~80COL is available). Each $C05E→$C05F edge
// pushes the current 80COL level (POM2 clocks 80COL itself, as [D] MAME
// `an3_w` @ e1479b7a; [D] AppleWin @ 7fd7fd7c clocks /80COL — same latch,
// inverse numbering):
//
//     $C00C,$C00C → 00 BW560      $C00D,$C00D → 11 COL140 (power-on, not Video-7)
//     $C00C,$C00D → 01 Mixed      $C00D,$C00C → 10 Chunky160
//
// No precondition on TEXT/MIXED/HIRES — [B] the Video-7 patent states none;
// [A] fenarinarsa, //c adapter (AppleWin #764, #850): MIXED on still clocks,
// and the latch shifts the current ~80COL on each edge (mostly — some runs
// of the same random sequences differed; one unit). [D] "80COL must have
// been written since the last edge" was AppleWin's heuristic (2463aae545),
// withdrawn in 1.29.16.0 (fb0871672c) — never a measurement; POM2, AppleWin
// @ 7fd7fd7c and MAME @ e1479b7a clock on every edge (docs/chatmauve_plan.md
// § 3.1). Power-on: [B] the patent's is 560 mono (POM2's Video-7), [B] the
// Féline manual's colour (POM2's Féline and //c adapter); the Eve and the
// RVB Graph come up in COL140 [F] (plan § 6; armPowerOnLatch). The enum
// value IS the latch value; `dhgrMode()` says what the variant makes of it.
//
// ── The Eve's registers ($C0B0-$C0BF, Eve only) ──────────────────────────
//
// [B] Eve manual ch. IX: sixteen addresses = eight switches × {off (even),
// on (odd)}; a WRITE sets one and latches the data byte into CPREG (low
// nibble = dot colour, high nibble = background; p. 117). [F] POM2 also
// decodes the address on a read — the manual has them set by writes;
// settled by: a `LDA $C0B9` on a real Eve. [B] All off at power-on
// (p. 117); Ctrl-Reset clears them unless LOCKRES is on (p. 119). Memory
// forwards the window through SlotBus::broadcastVideoSwitch{,Write} only
// while slot 3 is empty or holds the Chat Mauve itself (Memory.cpp, the
// slot-3 guard) — an SSC there drives its ACIA at the same addresses.
//
// CPREG's auto-write — [B] Eve manual ch. IX (pp. 117-119): a CPU write to
// MAIN text page ($0400-$07FF, TXT16 on) or HGR page ($2000-$3FFF,
// ENHRCPREG on) deposits CPREG in AUX at the same address, LOCKCPREG off —
// is a Memory write hook (`Memory::setAuxShadow`) the card programs
// whenever its switches or CPREG move. That is how Purplesoft's `PRINT` in
// colour and HPLOT in CP280 get their colours without the program ever
// touching aux ([C] though every Purplesoft `& GR` ends with LOCKCPREG on,
// `STA $C0BD` at $E04D, so the CP280 path is unverified past that point).

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
        Blank,      // Eve: HR1+HR2 — screen black, CPREG keeps working ([B] table IX-1, p. 118)
    };

    /// What the decoder does in single HGR (AN3 on, or AN3 off with 80COL
    /// off).
    enum class HgrMode : uint8_t {
        LcmColor,   // 2-bit cell colour, 3-bit window (010/101 coloured)
        Mono,       // 280 dots black and white
        FgBg,       // Video-7 F/B: colours from aux at the same address (hi = fg)
        Spec1,      // Eve HRSPEC1: LCM minus "colour dot on white" (101 → own bit; [F] 5-dot rule)
        Spec2,      // Eve HRSPEC2: SPEC1 minus "colour dot on black" (010 → own bit; [F] 5-dot rule)
        Dash,       // Eve HRDASH: colour runs drawn dotted (P3 — rendered as LcmColor)
        Cp280,      // Eve CP280: AN3 off, 80COL OFF, HR1+HR2+HR3 — fg/bg from aux (hi = bg)
        RvbGraph,   // RVB Graph: C0-C3 / B0-B3 by dot position, S options (rvbGraph())
    };

    /// What the decoder does with TEXT.
    enum class TextMode : uint8_t {
        Plain,      // the motherboard's black and white
        Color,      // 40-col colour text, per-cell colours in aux (see auxHiNibbleIsForeground)
        Green,      // Eve TXTGREEN: white → green, 40 and 80 col
        Tinted,     // white → one colour: the RVB Graph's CT, the Video-7's DIP switches (textTintColour())
    };

    /// RVB Graph register file. Colours are indices into the lo-res table
    /// (the manual's "table des couleurs", chapter II); the card stores them
    /// inverted (15 − colour) and these are decoded. n = 0..3 is the dot
    /// position: 0 even column bit 7 = 0 (violet), 1 odd/0 (green), 2 even/1
    /// (blue), 3 odd/1 (orange).
    struct RvbGraphState {
        uint8_t s          = 0;      // 0 continuous, 1 dotted, 2 four-colour, 3 = 2 dotted
        bool    useRegs    = false;  // U = 4: HGR from C/B (else usual colours, white = CT)
        bool    locked     = false;  // K = 8: registers closed, RESET leaves the card alone
        uint8_t textColour = 15;     // CT
        uint8_t c[4]       = { 3, 12, 6, 9 };      // isolated dot (usual colours)
        uint8_t b[4]       = { 15, 15, 15, 15 };   // run of 2+, by its leftmost dot
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

    /// The Féline, the //c adapter, the Eve and the Video-7 have no slot ROM
    /// and no device-select space of their own (they sniff the video soft
    /// switches through the broadcast, and the Eve's window is slot 3's):
    /// for them the slot number is informational. The RVB Graph is the
    /// exception — its control register and colour registers are its own
    /// slot's $C0n0 and $Cn0F-$Cn3F (deviceSelectWrite / slotRomWrite); its
    /// manual says slot 7 only, POM2 follows whatever slot it is plugged in.
    explicit LeChatMauveCard(int slot = kDefaultSlot, Variant v = Variant::Feline)
        : slot_(slot), variant_(v) { armPowerOnLatch(); }

    int getSlot() const { return slot_; }

    /// Constant across variants: Memory::chatMauveBlockedBySlot3 and the
    /// slot-configuration coordinator identify the card by this name.
    std::string_view name() const override { return "Le Chat Mauve"; }

    // ── Variant ──────────────────────────────────────────────────────────
    Variant variant() const { return variant_; }
    void    setVariant(Variant v);
    static const char* variantKey  (Variant v);   // settings token: feline | iic | eve | video7 | rvb
    static const char* variantLabel(Variant v);   // UI label
    static bool        parseVariant(std::string_view key, Variant& out);

    /// The Eve writes aux memory behind the CPU; that hook lives in Memory.
    /// Optional — a card without a Memory simply has no auto-write.
    void setMemory(Memory* mem);

    // ── Bus ──────────────────────────────────────────────────────────────
    /// Apple II RESET: does NOT re-arm the latch — [B] the patent gives it a
    /// power-on circuit and no reset input; [A] nicolho, a real //c adapter
    /// keeps its latch across a reboot (AppleWin #633); [D] AppleWin
    /// `RGB_ResetState` @ 7fd7fd7c re-arms colour, which POM2 no longer
    /// copies. On the Eve clears the eight switches unless LOCKRES is on
    /// ([B] manual p. 119). CPREG is not a switch and keeps its value [E].
    void onReset() override;
    /// A power-off is not Ctrl-Reset: LOCKRES and the RVB Graph's K hold
    /// state against the RESET key, not against the supply (bug hunt
    /// 2026-11-18). Power-on: [B] the Eve's switches all off (manual p. 117);
    /// [B] the RVB Graph's registers «indéterminé» (manual p. 9), so POM2's
    /// usual colours and CPREG = 0 are [F] — settled by: a board.
    void onPowerCycle() override;
    /// The machine's clock jumped (a .pom2snap load by the CLI or the AI
    /// server restores CPU and memory only, never this card): every latch
    /// edge in the ring is then stamped in the FUTURE and would win each
    /// latchBefore() scan, painting beam-raced frames in a stale mode. The
    /// same clear onReset and loadSnapshotState make (bug hunt 2026-11-07).
    void onTimeJump() override { latchRingHead_ = latchRingSize_ = 0; }
    /// Leaving the bus disarms the aux shadow it may have programmed into
    /// Memory (a slot rebuild must not leave two RAM pages diverted).
    void onUnplug() override;

    /// System soft-switch broadcast from Memory::softSwitchAccess() and the
    /// $C0B0-$C0BF forward. Reads and writes both land here; a write also
    /// arrives through onVideoSoftSwitchWrite with its data byte.
    void onVideoSoftSwitch(uint16_t addr) override;
    void onVideoSoftSwitchWrite(uint16_t addr, uint8_t value) override;
    /// The card drives nothing in its device-select window: reads keep the
    /// floating bus on every variant.
    uint8_t deviceSelectRead (uint8_t low4) override;
    /// No slot EPROM: $Cn00-$CnFF must keep reading the floating bus.
    uint8_t slotRomRead(uint8_t low8) override;
    /// RVB Graph only: the control register ($C0F0-$C0FF at slot 7).
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    /// RVB Graph only: the colour registers ($C700-$C73F at slot 7).
    void    slotRomWrite(uint8_t low8, uint8_t v) override;
    const RvbGraphState& rvbGraph() const { return rvb_; }

    // Rewind/snapshot: the guest-visible state. v3 = v2 + the Eve's switch
    // byte and CPREG (guest-written through $C0Bx, so guest-volatile); v5
    // adds the RVB Graph's control byte, CT and its eight colour registers.
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
    /// the high nibble ([D] MAME `render_line_color_array` @ e1479b7a), the
    /// Eve the background ([B] Eve manual IV-2.2, III-2).
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
    /// The latch as the variant powers up (docs/chatmauve_plan.md § 3.1):
    /// [B] the Video-7 patent — 560 mono; [B] the Féline manual — colour
    /// (the //c adapter is a Féline); the Eve and the RVB Graph — COL140 [F].
    void armPowerOnLatch();
    /// UI / tests: poke an Eve switch as a `STA $C0Bx` would (Eve only).
    void setEveSwitch(EveSwitch s, bool on);
    void setCpreg(uint8_t v);

    // Dragon Wars compatibility: a handful of titles encode their DHGR
    // mixed-mode bit 7 the other way round from the patent (colour where the
    // hardware expects mono, and vice-versa). XORs bit 7 at decode time in
    // the mixed mode; matches [D] AppleWin's `-rgb-card-invert-bit7`
    // (`RGB_SetInvertBit7`, RGBMonitor.cpp @ 7fd7fd7c) — a convenience, not
    // the hardware: [A] on real //c adapters Dragon Wars' title shows in B&W
    // (nicolho, AppleWin #633; fenarinarsa, #764). Off = strict patent
    // semantics (default). Not applied to the HGR bank bit.
    /// The Video-7's text colour outside F/B — DIP switches on the board, so
    /// a user setting (`chatmauve_video7_text`), not machine state. [D]
    /// AppleWin `RGB_SetVideocard` @ 7fd7fd7c: blue, amber, green or white =
    /// lo-res 6, 9, 12, 15 on black. Any other value is taken as white.
    void    setVideo7TextColour(uint8_t loresIndex) {
        video7TextColour_ = (loresIndex == 6 || loresIndex == 9 || loresIndex == 12) ? loresIndex : 15;
    }
    uint8_t video7TextColour() const { return video7TextColour_; }
    static const char* video7TextColourKey(uint8_t loresIndex);   // white | green | amber | blue
    static uint8_t     parseVideo7TextColour(std::string_view key);   // → 15 / 12 / 9 / 6 (white if unknown)
    /// The colour a Tinted text is drawn in, and whether inverse / flashing
    /// characters keep black and white: [B] the Video-7 SL7 manual, as quoted
    /// by fenarinarsa (AppleWin PR #837): «Inverse text always show in black
    /// and white»; [D] AppleWin UpdateText40/80ColorCell @ 7fd7fd7c — every
    /// character below $80 (inverse and flashing), in 40 columns outside F/B
    /// and always in 80. The RVB Graph tints every character [F].
    uint8_t textTintColour() const { return variant_ == Variant::Video7 ? video7TextColour_ : rvb_.textColour; }
    bool    tintKeepsInverseWhite() const { return variant_ == Variant::Video7; }
    void setInvertBit7(bool v) { invertBit7_ = v; }
    bool invertBit7() const    { return invertBit7_; }

private:
    int        slot_;
    Variant    variant_;
    Memory*    mem_             = nullptr;
    bool       an3Prev          = true;     // AN3 powers up HIGH (DHIRES off) — [D] MAME device_reset @ e1479b7a
    bool       eightyColLatched = false;    // last seen 80COL level
    uint8_t    fifo             = 0b11;     // 2 bits, MSB shifted out
    RenderMode mode             = RenderMode::COL140;
    uint8_t    video7TextColour_ = 15;   // setVideo7TextColour (DIP switches)
    bool       invertBit7_      = false;
    uint8_t    eveSwitches_     = 0;        // all off at power-on ([B] Eve manual p. 117)
    uint8_t    cpreg_           = 0;
    RvbGraphState rvb_;                     // RVB Graph registers
    uint16_t   rvbGeneration_   = 0;        // bumped on every RVB write (frame key)
    // Rolling latch history for beam-raced replay (latchBefore). A mode set
    // is TWO AN3 clocks, a PAL frame has 312 lines, and the CPU is already
    // in the next frame when this one is painted: 2 x 2 x 312 = 1248 edges.
    // 512 overflowed on a per-line mode switcher and painted the top band in
    // the wrong DHGR mode (bug hunt 2026-10-27).
    struct LatchEdge { uint64_t cycle; uint8_t before; uint8_t after; };
    bool       mixedWarned_ = false;   // one log per plug/variant change
    static constexpr int kLatchRing = 2048;
    LatchEdge  latchRing_[kLatchRing] {};
    int        latchRingHead_ = 0;          // next write slot
    int        latchRingSize_ = 0;

    bool isEve() const { return variant_ == Variant::Eve; }
    void clockFifo(bool dataBit);
    void eveDecode(uint16_t addr);
    void syncAuxShadow();
};

#endif // POM2_LE_CHAT_MAUVE_CARD_H
