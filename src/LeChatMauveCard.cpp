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

#include "LeChatMauveCard.h"

#include "Logger.h"
#include "Memory.h"

#include <cstring>

// ─── Variant ─────────────────────────────────────────────────────────────

const char* LeChatMauveCard::variantKey(Variant v)
{
    switch (v) {
        case Variant::Feline:     return "feline";
        case Variant::IIcAdapter: return "iic";
        case Variant::Eve:        return "eve";
        case Variant::Video7:     return "video7";
        case Variant::RvbGraph:   return "rvb";
    }
    return "feline";
}

const char* LeChatMauveCard::variantLabel(Variant v)
{
    switch (v) {
        case Variant::Feline:     return "Feline (//e aux slot)";
        case Variant::IIcAdapter: return "Adaptateur //c (DB-15)";
        case Variant::Eve:        return "Eve (//e aux slot, $C0B0-$C0BF)";
        case Variant::Video7:     return "Video-7 AppleColor RGB (US)";
        case Variant::RvbGraph:   return "RVB Graph (II/II+, slot 7)";
    }
    return "?";
}

bool LeChatMauveCard::parseVariant(std::string_view key, Variant& out)
{
    for (int i = 0; i < kVariantCount; ++i) {
        const auto v = static_cast<Variant>(i);
        if (key == variantKey(v)) { out = v; return true; }
    }
    return false;
}

void LeChatMauveCard::setVariant(Variant v)
{
    if (v == variant_) return;
    variant_ = v;
    // Another model is another board, powered up: its own latch state.
    armPowerOnLatch();
    // A card that is not an Eve has no switches to be in any state; one that
    // becomes an Eve powers up with all of them off, like the real board.
    eveSwitches_ = 0;
    cpreg_       = 0;
    rvb_         = RvbGraphState{};
    ++rvbGeneration_;
    mixedWarned_ = false;
    syncAuxShadow();
}

void LeChatMauveCard::setMemory(Memory* mem)
{
    if (mem_ && mem_ != mem) mem_->setAuxShadow(false, false, 0);
    mem_ = mem;
    syncAuxShadow();
}

// ─── Bus ─────────────────────────────────────────────────────────────────

const char* LeChatMauveCard::video7TextColourKey(uint8_t loresIndex)
{
    switch (loresIndex) {
        case 12: return "green";
        case 9:  return "amber";
        case 6:  return "blue";
        default: return "white";
    }
}

uint8_t LeChatMauveCard::parseVideo7TextColour(std::string_view key)
{
    if (key == "green") return 12;
    if (key == "amber") return 9;
    if (key == "blue")  return 6;
    return 15;
}

void LeChatMauveCard::armPowerOnLatch()
{
    // [B] US 4,631,692: «This initialization procedure powers-on the hardware
    // in the 560×192 mode» — the Video-7. [B] Féline manual: programs that
    // never switch «seront toujours en couleur» — the Féline, and the //c
    // adapter (a Féline without its RAM). The Eve and the RVB Graph: no
    // source, COL140 [F] (the Eve's own table IX-1 shows colour at HR 000).
    // [D] AppleWin @ 7fd7fd7c and MAME `device_reset` @ e1479b7a
    // (m_rgbmode = 3) power every card up in colour.
    if (variant_ == Variant::Video7) { fifo = 0b00; mode = RenderMode::BW560; }
    else                             { fifo = 0b11; mode = RenderMode::COL140; }
}

void LeChatMauveCard::onReset()
{
    // The latch is NOT reset by Ctrl-Reset: [B] the patent gives it a
    // power-on circuit and no reset input; [A] nicolho, a real //c adapter
    // (AppleWin #633, 2019-04-28): Renegade's title «in color after a cold
    // start … after one reboot or more, it always shows up in B&W» — the
    // latch kept its state, and the reset's AN3 rise clocked it once with
    // 80COL off. POM2 reproduces that edge as the hardware makes it: the
    // RESET clears 80COL (eightyColLatched) and the ROM's `$C05F` (Memory.cpp,
    // the reset throws it) is a rising edge if AN3 was low, so an3Prev is
    // left as it was. [D] AppleWin re-arms colour here (RGB_ResetState @
    // 7fd7fd7c) — a convenience the hardware does not have (until
    // 2026-11-28 POM2 copied it).
    eightyColLatched = false;

    // [B] Eve manual ch. IX p. 119: Ctrl-Reset clears the eight switches
    // unless LOCKRES is on. [E] CPREG is a data register, not a switch — it
    // keeps its byte (the manual does not say).
    if (isEve() && !eveSwitch(LOCKRES)) {
        eveSwitches_ = 0;
        syncAuxShadow();
    }

    // RVB Graph manual p. 13, K: with the registers open, "si vous pressez
    // la touche RESET au clavier vous revenez au mode de visualisation
    // usuel (dans ce cas RESET est équivalent à un «POKE -16 144, 0»)";
    // with K = 8, "la touche RESET n'a plus d'action sur la carte". The
    // colour registers are not touched either way — RESET is a control
    // write, and they only matter again once a program sets U.
    if (variant_ == Variant::RvbGraph && !rvb_.locked) {
        deviceSelectWrite(0x0, 0x00);
    }
}

void LeChatMauveCard::onPowerCycle()
{
    // LOCKRES / K = 8 survived a cold boot through onReset: the Eve kept
    // depositing CPREG into aux text behind the next program, the RVB Graph
    // kept its colour registers and its lock.
    eveSwitches_ = 0;
    cpreg_       = 0;
    rvb_         = {};
    ++rvbGeneration_;
    // The latch's own power-on (onReset leaves it alone), and AN3 powers up
    // HIGH (DHIRES off — [D] MAME `apple2video.cpp` `device_reset` @
    // e1479b7a, m_dhires = false), so a bare $C05F right after power-on is
    // NOT a rising edge.
    armPowerOnLatch();
    an3Prev = true;
    // The beam-replay ring holds cycle-stamped edges; a cold boot rewinds
    // cycleCounter to 0, so surviving edges would sit "in the future" and
    // replay a mode this timeline never entered. Start the history fresh —
    // Memory resets its own video event log for the same reason.
    latchRingHead_ = 0;
    latchRingSize_ = 0;
    onReset();
    syncAuxShadow();
}

uint8_t LeChatMauveCard::deviceSelectRead(uint8_t low4)
{
    // Nothing on this board DRIVES the data bus in $C0(8+n)X: the read sees
    // the floating bus (a card answering $FF hides it — CLAUDE.md § Memory
    // map). But [B] FR 2 509 560 Fig. 4: the RVB Graph's control register is
    // a 74LS273 clocked by DEVICE SELECT with no R/W gate, so a READ latches
    // it too — the address's S/U/K and, as the text colour, whatever the
    // floating bus holds. The manual only ever POKEs it.
    const uint8_t v = openBus();
    if (variant_ == Variant::RvbGraph) deviceSelectWrite(low4, v);
    return v;
}

uint8_t LeChatMauveCard::slotRomRead(uint8_t low8)
{
    // No ROM: the floating bus. And [B] FR 2 509 560 Fig. 3: the colour
    // registers are written by I/O SELECT with no R/W gate, so a READ of
    // $C7xx writes them too (with the bus's byte as Cn), unless K = 8 — the
    // autostart's slot scan does it at every boot, harmless while U = 0.
    // The debugger's views use peekSlotRom, which has no such effect.
    const uint8_t v = openBus();
    if (variant_ == Variant::RvbGraph) slotRomWrite(low8, v);
    return v;
}

void LeChatMauveCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (variant_ != Variant::RvbGraph) return;
    // RVB Graph manual pp. 12-13: POKE −16144 + S + U + K, 16·(15 − CT).
    // −16144 = $C0F0, so the address's low nibble IS the option set —
    // S = bits 0-1 (0..3), U = bit 2 (0 or 4), K = bit 3 (0 or 8) — and the
    // data's high nibble is the text colour, inverted. The manual's own
    // recipes: $C0F4 then $C0FC (p. 9), $C0F5/$C0FD (p. 11), $C0F9 (p. 14).
    rvb_.s          = static_cast<uint8_t>(low4 & 0x03);
    rvb_.useRegs    = (low4 & 0x04) != 0;
    rvb_.locked     = (low4 & 0x08) != 0;
    rvb_.textColour = static_cast<uint8_t>(15 - (v >> 4));
    ++rvbGeneration_;
}

void LeChatMauveCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    if (variant_ != Variant::RvbGraph || rvb_.locked) return;
    // RVB Graph manual pp. 9 and 15-16: POKE −14577 − Bn, 15 − Cn for n = 0
    // and −14561 / −14545 / −14529 for n = 1..3, i.e. $C70F, $C71F, $C72F,
    // $C73F minus Bn. [B] FR 2 509 560 Fig. 3 (the card's patent, two
    // independent readings 2026-11-28): two 74LS170 4×4 register files, the
    // write strobe I/O SELECT itself — $C700-$C7FF, A6/A7 NOT decoded, so the
    // page mirrors every $40 — address A4/A5 the entry, the C file taking
    // D0-D3 and the B file A0-A3 in the same access, writes blocked by K
    // through diode D1 («registres fermés», manual p. 13).
    const int n = (low8 >> 4) & 0x03;
    rvb_.b[n] = static_cast<uint8_t>(15 - (low8 & 0x0F));
    rvb_.c[n] = static_cast<uint8_t>(15 - (v & 0x0F));
    ++rvbGeneration_;
}

void LeChatMauveCard::onUnplug()
{
    if (mem_) mem_->setAuxShadow(false, false, 0);
    mem_ = nullptr;
}

void LeChatMauveCard::onVideoSoftSwitch(uint16_t addr)
{
    // Data line first — the FIFO samples 80COL on the AN3 rising edge, so
    // the data bit must be up to date before we look for the clock.
    if (addr == 0xC00C) { eightyColLatched = false; return; }
    if (addr == 0xC00D) { eightyColLatched = true;  return; }

    // Clock line. AN3 going LOW just records the level; nothing shifts.
    if (addr == 0xC05E) { an3Prev = false; return; }

    // AN3 going HIGH: rising edge → push the current data bit into the
    // FIFO. Only a real transition shifts; software that hammers $C05F
    // without an intervening $C05E gets one shift, not many — the Arlequin
    // reference sequence alternates STA $C05E ; STA $C05F for every bit.
    if (addr == 0xC05F) {
        // 80COL is clocked DIRECTLY ([D] MAME `an3_w` @ e1479b7a); [D]
        // AppleWin @ 7fd7fd7c clocks /80COL, as [B] the patent's hardware
        // does (only ~80COL is available), so its mode numbering is the
        // bit-inverse of this one for the same STA sequence — expected, not
        // a bug.
        if (!an3Prev) clockFifo(eightyColLatched);
        an3Prev = true;
        return;
    }

    // The Eve's eight switches at sixteen addresses. [F] Any access decodes
    // the address — [B] the manual has them set by writes (p. 117), but the
    // board sees the address bus and POM2 has always let a `LDA $C0B9`
    // strobe through, which is what the Apple II's own SET/CLR soft
    // switches do. Settled by: a `LDA $C0B9` on a real Eve. Other variants
    // have no registers there.
    if (addr >= 0xC0B0 && addr <= 0xC0BF && isEve()) eveDecode(addr);
}

void LeChatMauveCard::onVideoSoftSwitchWrite(uint16_t addr, uint8_t value)
{
    if (addr >= 0xC0B0 && addr <= 0xC0BF && isEve()) {
        // [B] Manual p. 117: «Il faudra écrire à ces adresses car, à chaque
        // fois la donnée sera mémorisée dans … CPREG»: the data byte is
        // latched on EVERY write to the window, whichever switch it names.
        cpreg_ = value;
        eveDecode(addr);
        syncAuxShadow();
        return;
    }
    onVideoSoftSwitch(addr);
}

void LeChatMauveCard::eveDecode(uint16_t addr)
{
    const uint8_t bit = static_cast<uint8_t>((addr - 0xC0B0u) >> 1);
    const uint8_t mask = static_cast<uint8_t>(1u << bit);
    const uint8_t next = (addr & 1u) ? static_cast<uint8_t>(eveSwitches_ | mask)
                                     : static_cast<uint8_t>(eveSwitches_ & ~mask);
    if (next == eveSwitches_) return;
    eveSwitches_ = next;
    syncAuxShadow();
}

void LeChatMauveCard::setEveSwitch(EveSwitch s, bool on)
{
    if (!isEve()) return;
    eveDecode(static_cast<uint16_t>(0xC0B0 + 2 * s + (on ? 1 : 0)));
}

void LeChatMauveCard::setCpreg(uint8_t v)
{
    if (!isEve() || v == cpreg_) return;
    cpreg_ = v;
    syncAuxShadow();
}

void LeChatMauveCard::syncAuxShadow()
{
    if (!mem_) return;
    // [B] Eve manual ch. IX (pp. 117-119): TXT16 makes the card deposit
    // CPREG in aux for every character sent to the text page; ENHRCPREG
    // extends it to the HGR page (CP280's HPLOT). LOCKCPREG freezes both.
    // Off on every other variant.
    const bool live = isEve() && !eveSwitch(LOCKCPREG);
    mem_->setAuxShadow(live && eveSwitch(TXT16),
                       live && eveSwitch(ENHRCPREG),
                       cpreg_);
}

void LeChatMauveCard::clockFifo(bool dataBit)
{
    const uint8_t before = fifo;
    fifo = static_cast<uint8_t>(((fifo << 1) | (dataBit ? 1u : 0u)) & 0b11);
    mode = static_cast<RenderMode>(fifo);
    // A program clocking the FÉLINE mixed mode on a card that does not have
    // it ([B] Manuel Arlequin, as quoted by Extasie Chat Mauve Reloaded:
    // "la carte Eve n'est pas compatible avec ce mode") gets the fallback — and one loud line saying why
    // the picture looks wrong and what to switch to. Extasie and Arlequin
    // are the canonical cases.
    if (mode == RenderMode::Mixed && !mixedWarned_ &&
        (variant_ == Variant::Eve || variant_ == Variant::RvbGraph)) {
        mixedWarned_ = true;
        pom2::log().info("Chat Mauve",
            std::string("this program uses the Féline/IIc MIXED DHGR mode; the ") +
            variantLabel(variant_) +
            " does not have it (falls back to COL140). Extasie/Arlequin need"
            " the Féline — Slot Config → model.");
    }
    // Timestamped history for the beam-raced replay, on the SAME clock the
    // video-event log uses (Memory::videoAccessCycle): latchBefore() is compared
    // against a VideoEvent's emuCycle, and an instruction-START stamp is a
    // few cycles too early — a frame whose first event IS this $C05F then
    // got the POST-clock latch as its seed and forEachBeamSegment clocked
    // it a second time. Without a Memory there is no clock to stamp with —
    // the ring stays empty and latchBefore() degrades to the current value.
    if (mem_) {
        latchRing_[latchRingHead_] = { mem_->videoAccessCycle(), before, fifo };
        latchRingHead_ = (latchRingHead_ + 1) % kLatchRing;
        if (latchRingSize_ < kLatchRing) ++latchRingSize_;
    }
}

LeChatMauveCard::RenderMode LeChatMauveCard::latchBefore(uint64_t cycle) const
{
    // Newest-first scan for the last edge STRICTLY before `cycle`; the ring
    // is small and this is called once per rendered frame. An edge stores
    // the fifo on BOTH sides, so a cycle older than every recorded edge
    // still gets the right answer (the oldest edge's pre-clock value).
    for (int k = 0; k < latchRingSize_; ++k) {
        const int idx = (latchRingHead_ - 1 - k + 2 * kLatchRing) % kLatchRing;
        if (latchRing_[idx].cycle < cycle)
            return static_cast<RenderMode>(latchRing_[idx].after & 0b11);
        if (k == latchRingSize_ - 1)
            return static_cast<RenderMode>(latchRing_[idx].before & 0b11);
    }
    // Empty ring: the latch never clocked (or no Memory to timestamp with).
    return mode;
}

// ─── What the renderers ask ──────────────────────────────────────────────

LeChatMauveCard::DhgrMode LeChatMauveCard::dhgrModeFor(RenderMode latch) const
{
    switch (variant_) {
    case Variant::Video7:
        // [B] The four patent modes (US 4,631,692), as [D] MAME's
        // `dhgr_update` rgbmode 0-3 @ e1479b7a.
        switch (latch) {
            case RenderMode::BW560:     return DhgrMode::BW560;
            case RenderMode::Mixed:     return DhgrMode::Mixed;
            case RenderMode::Chunky160: return DhgrMode::Chunky160;
            case RenderMode::COL140:    return DhgrMode::COL140;
        }
        return DhgrMode::COL140;

    case Variant::Feline:
    case Variant::IIcAdapter:
        // No 160-wide mode on the Chat Mauve boards: [D] AppleWin's
        // RGB_Is140Mode() @ 7fd7fd7c folds it into COL140 for the Féline,
        // and [B] the Manuel Arlequin lists only 140 / 560 / mixed.
        switch (latch) {
            case RenderMode::BW560:     return DhgrMode::BW560;
            case RenderMode::Mixed:     return DhgrMode::Mixed;
            case RenderMode::Chunky160: return DhgrMode::COL140;
            case RenderMode::COL140:    return DhgrMode::COL140;
        }
        return DhgrMode::COL140;

    case Variant::RvbGraph:
        // [F] A ][ / ][+ card ([B] its manual: TEXT 40, GR, HGR only — no
        // 80COL, no AN3, no DHGR), so this has NO source: on a //e that hosts
        // it anyway POM2 shows COL140 / BW560 from the latch, no mixed, no 160.
        // Settled by: an RVB Graph in a //e — or nobody, if none was ever
        // run there (docs/chatmauve_plan.md § 6).
        return latch == RenderMode::BW560 ? DhgrMode::BW560 : DhgrMode::COL140;

    case Variant::Eve: {
        // [B] Table IX-1 (Eve manual p. 118), AN3 off rows (all but CP280
        // need 80COL on — CP280 has Note 2 only — and 80COL on is the DHGR
        // condition that brought us here). HR = (HR1, HR2, HR3).
        // [C] Purplesoft's own `& GR 6..10` switch tables (PURPLESOFT* rev B,
        // $E06F-$E0A0: AN3, ENHRCPREG, HR1, HR2, HR3 per mode) are the
        // authority where the scanned table is ambiguous:
        //   6 COL140  = 000    7 COL280A = 100    8 COL280B = 010
        //   9 CP280   = 111   10 BW560   = 011  (HR2+HR3 — the same pair
        //   that is HRBW with AN3 on; [B] the scan prints the same, p. 118 —
        //   the "HR3 alone" was our misread of it)
        // The patent latch, at HR 000: [A] second-hand, fenarinarsa (AppleWin
        // #764, «tested on EVE with the Apple RGB demo disk»): `C00C+C00C` =
        // DHGR black and white «(AppleColor, EVE, Feline)», `C00D+C00D` =
        // colour, the unsupported codes colour; [A] jvernet's //e + Le Chat
        // Mauve card boots Apple II DeskTop in BW560 through the latch (#631);
        // and fenarinarsa's *Modes vidéo chelous*: the Arlequin routine
        // programs the latch «y compris sur la EVE». So BW560 at latch 00,
        // COL140 otherwise ([B] "la carte Eve n'est pas compatible avec ce
        // mode", Manuel Arlequin as quoted by Extasie Chat Mauve Reloaded:
        // mixed → 140; no 160). Until 2026-11-28 POM2 ignored
        // the latch on the Eve, from [C] Purplesoft's `& GR 6` expecting
        // colour. Both stand if the Eve follows the latch only while its own
        // switches are out of the way [F]: the AppleColor demo ran with every
        // Eve switch off (power-on), while Purplesoft turns ENHRCPREG on for
        // every AN3-off mode ([C] `$E06F-$E0A0`) — and its `& GR` itself
        // clocks the latch to 00 during start-up ([C] `STA $C000,Y` at
        // `$E053`, AN3 rising with 80COL off, twice), so `& GR 6` would be
        // black and white if the latch ruled there. So: ENHRCPREG off → the
        // latch; on → table IX-1. Settled by: an owner — Purplesoft `& GR 6`
        // after `& GR 1` twice, then the same with `POKE -16208,0` (ENHRCPREG
        // off). Whether the other HR rows follow the latch is [F]: table IX-1
        // alone decides them here.
        const bool hr1 = eveSwitch(HR1), hr2 = eveSwitch(HR2), hr3 = eveSwitch(HR3);
        if (!hr1 && !hr2 && !hr3)
            return (!eveSwitch(ENHRCPREG) && latch == RenderMode::BW560) ? DhgrMode::BW560
                                                                          : DhgrMode::COL140;
        if (!hr1 &&  hr2 &&  hr3) return DhgrMode::BW560;
        if ( hr1 && !hr2 && !hr3) return DhgrMode::COL280A;
        if (!hr1 &&  hr2 && !hr3) return DhgrMode::COL280B;
        if ( hr1 &&  hr2 && !hr3) return DhgrMode::Blank;
        if ( hr1 &&  hr2 &&  hr3) return DhgrMode::CP280;
        // 001 (HR3 alone) and 101 are not in table IX-1 ([B] p. 118) and
        // used by nobody we know of ([C] not by Purplesoft). [F] 001 is kept
        // on BW560 and 101 falls back to COL140 — settled by: an owner.
        if (!hr1 && !hr2 &&  hr3) return DhgrMode::BW560;
        return DhgrMode::COL140;
    }
    }
    return DhgrMode::COL140;
}

LeChatMauveCard::HgrMode LeChatMauveCard::hgrMode(bool an3On) const
{
    switch (variant_) {
    case Variant::Feline:
    case Variant::IIcAdapter:
        // [B] Féline manual p. 38: `POKE -16290,0` (AN3 off) shows HGR in
        // monochrome; `POKE -16289,0` or Ctrl-Reset (AN3 back on) restores
        // the colours. [A] The //c adapter does the same (fenarinarsa,
        // AppleWin #764).
        return an3On ? HgrMode::LcmColor : HgrMode::Mono;

    case Variant::Video7:
        // [D] MAME `hgr_update` @ e1479b7a: rgb_monitor && m_dhires (HGR,
        // so !m_80col) → the foreground/background mode with the colours in
        // aux. [B] Bob S-C, Apple Assembly Line 1986-08: «F/B mode is active
        // any time the 80 column switch is off and annunciator 3 is off».
        return an3On ? HgrMode::LcmColor : HgrMode::FgBg;

    case Variant::RvbGraph:
        // Its own decoder whatever the switches: monochrome is not a mode
        // of this card but a choice of colours (manual p. 11: every
        // register at 15 gives "une image identique à celle obtenue sur un
        // moniteur noir et blanc", every one at 12 a green monitor).
        return HgrMode::RvbGraph;

    case Variant::Eve: {
        // [B] Table IX-1 (p. 118), AN3 on rows, confirmed by [C]
        // Purplesoft's `& GR 1..5` tables ($E06F-$E0A0): 1 HRAPPLEII = 000,
        // 2 HRSPEC1 = 100, 3 HRSPEC2 = 110 (SPEC2 builds on SPEC1), 4 HRDASH
        // = 001, 5 HRBW = 011. [F] Where the rows' don't-cares overlap, the
        // order below is POM2's: HRBW wins over the DASH row (011), SPEC2
        // over SPEC1 (110; 111 with AN3 on), SPEC1 over DASH (101), HR2
        // alone reads as SPEC2 — settled by: an owner. AN3 off with 80COL off: [C] Purplesoft's `& GR 9` runs CP280
        // exactly so (its 80COL table at $E317-$E32A turns 80COL on for
        // modes 6, 7, 8, 10 and leaves it for 9 — off after `& TEXT 1`; [B]
        // p. 118: CP280 has Note 2 only, no 80COL requirement — the Eve IS
        // the aux memory and has the attribute byte whatever 80COL says).
        // [F] The other HR values with AN3 off and 80COL off are not in the
        // table: POM2 keeps the HGR decoder running — settled by: an Eve
        // owner's capture in that state.
        const bool hr1 = eveSwitch(HR1), hr2 = eveSwitch(HR2), hr3 = eveSwitch(HR3);
        if (!an3On && hr1 && hr2 && hr3) return HgrMode::Cp280;
        if (!hr1 &&  hr2 &&  hr3) return HgrMode::Mono;
        if ( hr2)                 return HgrMode::Spec2;
        if ( hr1)                 return HgrMode::Spec1;
        if ( hr3)                 return HgrMode::Dash;
        return HgrMode::LcmColor;
    }
    }
    return HgrMode::LcmColor;
}

LeChatMauveCard::TextMode LeChatMauveCard::textMode(bool eightyCol, bool an3On) const
{
    switch (variant_) {
    case Variant::Feline:
    case Variant::IIcAdapter:
        // "TEXT colour: —" for both (plan § 1). The motherboard's text.
        return TextMode::Plain;

    case Variant::Video7:
        // [D] MAME `text_update` @ e1479b7a (:788): rgb_monitor && m_dhires
        // && !m_80col → colours from aux, hi nibble = foreground; [B] AAL
        // 1986-08, F/B = 80COL off and AN3 off.
        if (!eightyCol && !an3On) return TextMode::Color;
        // Outside F/B the text takes the board's DIP-switch colour ([D]
        // AppleWin `RGB_SetVideocard` @ 7fd7fd7c).
        return video7TextColour_ == 15 ? TextMode::Plain : TextMode::Tinted;

    case Variant::RvbGraph:
        // "Les lignes de texte apparaîtront dans la couleur CT" (p. 10):
        // one colour for the whole text screen, from the control register.
        return rvb_.textColour == 15 ? TextMode::Plain : TextMode::Tinted;

    case Variant::Eve:
        // [B] Eve manual IV-2.2, III-2: TXT16 on + 80COL off → 40-col colour
        // text, hi nibble = background. No AN3 condition (the manual's BASIC
        // recipe, `POKE -16199,16*F+C`, never touches it). [B] TXTGREEN
        // (ch. IX): white → green, 40 and 80 col. [E] When both are on, POM2
        // shows the colour text — it has no white to turn green.
        if (eveSwitch(TXT16) && !eightyCol) return TextMode::Color;
        if (eveSwitch(TXTGREEN))            return TextMode::Green;
        return TextMode::Plain;
    }
    return TextMode::Plain;
}

uint32_t LeChatMauveCard::renderStateKey() const
{
    return static_cast<uint32_t>(variant_)
         | (static_cast<uint32_t>(mode)          << 4)
         | (static_cast<uint32_t>(eveSwitches_)  << 8)
         | (invertBit7_ ? (1u << 16) : 0u)
         | (static_cast<uint32_t>(rvbGeneration_ & 0x7FFF) << 17);
}

// ─── Snapshot ────────────────────────────────────────────────────────────

void LeChatMauveCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // v3: the Eve's switch byte + CPREG replace v2's two toggles (which
    // were the $C0B8/$C0BA pair under their old, wrong labels). Both are
    // written by the guest through $C0Bx, so a rewind past a `STA $C0B9`
    // has to put them back.
    // v5: the RVB Graph's register file replaces v4's mode strobe.
    out.push_back('C'); out.push_back('M'); out.push_back(5);
    out.push_back(fifo);
    out.push_back(static_cast<uint8_t>(mode));
    out.push_back(an3Prev ? 1 : 0);
    out.push_back(eightyColLatched ? 1 : 0);
    out.push_back(eveSwitches_);
    out.push_back(cpreg_);
    out.push_back(static_cast<uint8_t>(rvb_.s | (rvb_.useRegs ? 0x04 : 0) |
                                       (rvb_.locked ? 0x08 : 0)));
    out.push_back(rvb_.textColour);
    for (int n = 0; n < 4; ++n) out.push_back(rvb_.c[n]);
    for (int n = 0; n < 4; ++n) out.push_back(rvb_.b[n]);
}

void LeChatMauveCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 7 || data[0] != 'C' || data[1] != 'M' ||
        data[2] < 1 || data[2] > 5) return;
    fifo             = static_cast<uint8_t>(data[3] & 0b11);
    mode             = static_cast<RenderMode>(data[4] & 0b11);
    an3Prev          = data[5] != 0;
    eightyColLatched = data[6] != 0;
    // Every field a version does not carry goes back to its power-on value
    // (the SlotPeripheral.h contract): a v1 blob kept the live Eve switches,
    // CPREG and RVB Graph registers, and v2 the live CPREG and registers, so
    // an old session came back painted with the current one's colours.
    eveSwitches_ = 0;
    cpreg_       = 0;
    rvb_         = RvbGraphState{};
    if (data[2] == 2 && len >= 9) {
        // v2 carried "colour text enable" ($C0B8/9 = TXT16) and "HGR
        // duochrome" ($C0BA/B = TXTGREEN, mislabelled). Map them onto the
        // switches they really were.
        eveSwitches_ = static_cast<uint8_t>((data[7] ? (1u << TXT16)    : 0u) |
                                            (data[8] ? (1u << TXTGREEN) : 0u));
    } else if (data[2] >= 3 && len >= 9) {
        eveSwitches_ = data[7];
        cpreg_       = data[8];
        // v4's byte 9 was the old four-strobe model, which the manual
        // disproved; a v4 blob restores the power-on register file.
        if (data[2] >= 5 && len >= 19) {
            rvb_.s          = static_cast<uint8_t>(data[9] & 0x03);
            rvb_.useRegs    = (data[9] & 0x04) != 0;
            rvb_.locked     = (data[9] & 0x08) != 0;
            rvb_.textColour = static_cast<uint8_t>(data[10] & 0x0F);
            for (int n = 0; n < 4; ++n) {
                rvb_.c[n] = static_cast<uint8_t>(data[11 + n] & 0x0F);
                rvb_.b[n] = static_cast<uint8_t>(data[15 + n] & 0x0F);
            }
        }
    }
    if (!isEve()) eveSwitches_ = 0;
    // Snapshot-load and rewind move cycleCounter backwards; edges recorded
    // on the abandoned timeline are stamped in the FUTURE and would win
    // every latchBefore() scan, replaying a mode the restored machine never
    // entered (MachineSnapshot resets Memory's video event log for exactly
    // this clock-jump reason — this ring is the card's parallel log).
    latchRingHead_ = 0;
    latchRingSize_ = 0;
    ++rvbGeneration_;
    syncAuxShadow();
}
