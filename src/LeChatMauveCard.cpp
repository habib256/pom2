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
        case Variant::RvbGraph:   return "RVB Graph (II/II+, partial)";
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
    // A card that is not an Eve has no switches to be in any state; one that
    // becomes an Eve powers up with all of them off, like the real board.
    eveSwitches_ = 0;
    cpreg_       = 0;
    rvbMode_     = 0;
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

void LeChatMauveCard::onReset()
{
    // The latch comes up in COL140 — the state AppleWin's RGB_ResetState and
    // the Féline manual describe (the boot ROM never reconfigures it, so a
    // program that never talks to AN3+80COL gets the RGB rendering for
    // free). AN3 powers up HIGH (DHIRES off — MAME `apple2video
    // device_reset` m_dhires=false), so an3Prev starts TRUE: a bare $C05F
    // right after reset is NOT a rising edge and must not clock the FIFO.
    fifo             = 0b11;
    mode             = RenderMode::COL140;
    an3Prev          = true;
    eightyColLatched = false;
    // The beam-replay ring holds cycle-stamped edges; a cold boot rewinds
    // cycleCounter to 0, so surviving edges would sit "in the future" and
    // replay a mode this timeline never entered. Start the history fresh —
    // Memory resets its own video event log for the same reason.
    latchRingHead_ = 0;
    latchRingSize_ = 0;

    // Eve manual IX: Ctrl-Reset clears all sixteen switches unless LOCKRES
    // is on. CPREG is a data register, not a switch — it keeps its byte.
    if (isEve() && !eveSwitch(LOCKRES)) {
        eveSwitches_ = 0;
        syncAuxShadow();
    }
}

uint8_t LeChatMauveCard::deviceSelectRead(uint8_t low4)
{
    // RVB Graph mode strobes — POKE −16144…−16141 in the sources, i.e. any
    // access decodes the address, read or write.
    if (variant_ == Variant::RvbGraph && low4 <= 3) rvbMode_ = low4;
    // Nothing on this board DRIVES the data bus in $C0(8+n)X — the strobes
    // are address decodes, and the other four variants decode nothing here
    // at all. A card that answers $FF where it holds no driver hides the
    // floating bus (CLAUDE.md § Memory map), and `chatmauve` is the
    // fresh-install slot-7 default, so this covered $C0F0-$C0FF by default.
    return openBus();
}

void LeChatMauveCard::deviceSelectWrite(uint8_t low4, uint8_t)
{
    if (variant_ == Variant::RvbGraph && low4 <= 3) rvbMode_ = low4;
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
        // 80COL is clocked DIRECTLY (MAME's convention); AppleWin clocks
        // /80COL, so its mode numbering is the bit-inverse of this one for
        // the same STA sequence — expected, not a bug.
        if (!an3Prev) clockFifo(eightyColLatched);
        an3Prev = true;
        return;
    }

    // The Eve's sixteen switches. Any access decodes the address — the
    // manual's recipes are POKEs, but the board sees the address bus and
    // POM2 has always let a `LDA $C0B9` strobe through, which is what the
    // Apple II's own SET/CLR soft switches do. Other variants have no
    // registers there.
    if (addr >= 0xC0B0 && addr <= 0xC0BF && isEve()) eveDecode(addr);
}

void LeChatMauveCard::onVideoSoftSwitchWrite(uint16_t addr, uint8_t value)
{
    if (addr >= 0xC0B0 && addr <= 0xC0BF && isEve()) {
        // "Toute écriture dans l'un de ces registres charge aussi CPREG"
        // (manual IX): the data byte is latched on EVERY write to the
        // window, whichever switch the address names.
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
    // Manual IX: TXT16 makes the card deposit CPREG in aux for every
    // character sent to the text page; ENHRCPREG extends it to the HGR page
    // (CP280's HPLOT). LOCKCPREG freezes both. Off on every other variant.
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
    // it (Manuel Arlequin: "la carte Eve n'est pas compatible avec ce
    // mode") gets the hardware's fallback — and one loud line saying why
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
    // video-event log uses (Memory::accessCycle): latchBefore() is compared
    // against a VideoEvent's emuCycle, and an instruction-START stamp is a
    // few cycles too early — a frame whose first event IS this $C05F then
    // got the POST-clock latch as its seed and forEachBeamSegment clocked
    // it a second time. Without a Memory there is no clock to stamp with —
    // the ring stays empty and latchBefore() degrades to the current value.
    if (mem_) {
        latchRing_[latchRingHead_] = { mem_->accessCycle(), before, fifo };
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
        // The four patent modes, as MAME's dhgr_update rgbmode 0-3.
        switch (latch) {
            case RenderMode::BW560:     return DhgrMode::BW560;
            case RenderMode::Mixed:     return DhgrMode::Mixed;
            case RenderMode::Chunky160: return DhgrMode::Chunky160;
            case RenderMode::COL140:    return DhgrMode::COL140;
        }
        return DhgrMode::COL140;

    case Variant::Feline:
    case Variant::IIcAdapter:
        // No 160-wide mode on the Chat Mauve boards: AppleWin's
        // RGB_Is140Mode() folds it into COL140 for the Féline, and the
        // Manuel Arlequin lists only 140 / 560 / mixed.
        switch (latch) {
            case RenderMode::BW560:     return DhgrMode::BW560;
            case RenderMode::Mixed:     return DhgrMode::Mixed;
            case RenderMode::Chunky160: return DhgrMode::COL140;
            case RenderMode::COL140:    return DhgrMode::COL140;
        }
        return DhgrMode::COL140;

    case Variant::RvbGraph:
        // The plan's family table: COL140 and BW560 only — no mixed, no
        // 160. The two mono strobes force the 560-dot monochrome.
        if (rvbMode_ >= 2) return DhgrMode::BW560;
        return latch == RenderMode::BW560 ? DhgrMode::BW560 : DhgrMode::COL140;

    case Variant::Eve: {
        // Table IX-1, AN3 off rows (all need 80COL on, which is the DHGR
        // condition that brought us here). HR = (HR1, HR2, HR3).
        // Purplesoft's own `& GR 6..10` switch tables (PURPLESOFT* rev B,
        // $E06F-$E0A0: AN3, ENHRCPREG, HR1, HR2, HR3 per mode) are the
        // authority where the scanned table is ambiguous:
        //   6 COL140  = 000    7 COL280A = 100    8 COL280B = 010
        //   9 CP280   = 111   10 BW560   = 011  (HR2+HR3 — the same pair
        //   that is HRBW with AN3 on; the scan's "HR3 alone" was a misread)
        // The patent latch plays NO part on the Eve: Purplesoft never
        // clocks it on purpose, its `& TEXT` / `& GR` switching leaves it at
        // 00 (BW560 on a Féline), and `& GR 6` (HR 000) must show COL140 —
        // which is what the manual says too. BW560 is HR2+HR3, nothing else.
        // "La carte Eve n'est pas compatible avec ce mode" (Manuel Arlequin):
        // mixed → 140; 160 → 140.
        const bool hr1 = eveSwitch(HR1), hr2 = eveSwitch(HR2), hr3 = eveSwitch(HR3);
        if (!hr1 && !hr2 && !hr3) return DhgrMode::COL140;
        if (!hr1 &&  hr2 &&  hr3) return DhgrMode::BW560;
        if ( hr1 && !hr2 && !hr3) return DhgrMode::COL280A;
        if (!hr1 &&  hr2 && !hr3) return DhgrMode::COL280B;
        if ( hr1 &&  hr2 && !hr3) return DhgrMode::Blank;
        if ( hr1 &&  hr2 &&  hr3) return DhgrMode::CP280;
        // 001 (HR3 alone, the scan's reading of the BW560 row) and 101 are
        // used by nobody we know of. HR3 alone is kept on BW560 in case the
        // scan was right after all; 101 falls back to COL140.
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
        // Féline manual: `POKE -16290,0` (AN3 off) shows HGR in
        // monochrome; Ctrl-Reset (AN3 back on) restores the colours.
        return an3On ? HgrMode::LcmColor : HgrMode::Mono;

    case Variant::Video7:
        // MAME hgr_update: rgb_monitor && m_dhires && !m_80col → the
        // foreground/background mode with the colours in aux.
        return an3On ? HgrMode::LcmColor : HgrMode::FgBg;

    case Variant::RvbGraph:
        // $C0F2/$C0F3 are whole-screen monochrome; the HGR colour
        // registers (6 programmable of 16) are not modelled — standard LCM
        // colours until the manual surfaces.
        return rvbMode_ >= 2 ? HgrMode::Mono : HgrMode::LcmColor;

    case Variant::Eve: {
        // Table IX-1, AN3 on rows, confirmed by Purplesoft's `& GR 1..5`
        // tables: 1 HRAPPLEII = 000, 2 HRSPEC1 = 100, 3 HRSPEC2 = 110
        // (SPEC2 builds on SPEC1), 4 HRDASH = 001, 5 HRBW = 011. HRBW wins
        // over the don't-cares of the DASH row; HR2 alone reads as SPEC2.
        // AN3 off with 80COL off: Purplesoft's `& GR 9` runs CP280 exactly
        // so (its 80COL table turns 80COL on for modes 6, 7, 8, 10 and OFF
        // for 9 — the Eve IS the aux memory and has the attribute byte
        // whatever 80COL says; the 560-dot modes need 80COL for the doubled
        // shift rate, a 280-dot mode does not). The other HR values with
        // AN3 off and 80COL off are not in the table: assumption — the HGR
        // decoder keeps running.
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
        // MAME text_update: rgb_monitor && m_dhires && !m_80col → colours
        // from aux, hi nibble = foreground.
        return (!eightyCol && !an3On) ? TextMode::Color : TextMode::Plain;

    case Variant::RvbGraph:
        // $C0F1/$C0F3 turn the text green (white otherwise). The
        // whole-screen text COLOUR register is not modelled (no address on
        // record) — colour text renders white.
        return (rvbMode_ & 1) ? TextMode::Green : TextMode::Plain;

    case Variant::Eve:
        // Manual IV-2.2: TXT16 on + 80COL off → 40-col colour text, hi
        // nibble = background. No AN3 condition (the BASIC recipe never
        // touches it). TXTGREEN: white → green, 40 and 80 col; when both
        // are on the coloured text has no white to turn green.
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
         | (static_cast<uint32_t>(rvbMode_)      << 17);
}

// ─── Snapshot ────────────────────────────────────────────────────────────

void LeChatMauveCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // v3: the Eve's switch byte + CPREG replace v2's two toggles (which
    // were the $C0B8/$C0BA pair under their old, wrong labels). Both are
    // written by the guest through $C0Bx, so a rewind past a `STA $C0B9`
    // has to put them back.
    out.push_back('C'); out.push_back('M'); out.push_back(4);
    out.push_back(fifo);
    out.push_back(static_cast<uint8_t>(mode));
    out.push_back(an3Prev ? 1 : 0);
    out.push_back(eightyColLatched ? 1 : 0);
    out.push_back(eveSwitches_);
    out.push_back(cpreg_);
    out.push_back(rvbMode_);          // v4: the RVB Graph mode strobe
}

void LeChatMauveCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 7 || data[0] != 'C' || data[1] != 'M' ||
        data[2] < 1 || data[2] > 4) return;
    fifo             = static_cast<uint8_t>(data[3] & 0b11);
    mode             = static_cast<RenderMode>(data[4] & 0b11);
    an3Prev          = data[5] != 0;
    eightyColLatched = data[6] != 0;
    if (data[2] == 2 && len >= 9) {
        // v2 carried "colour text enable" ($C0B8/9 = TXT16) and "HGR
        // duochrome" ($C0BA/B = TXTGREEN, mislabelled). Map them onto the
        // switches they really were.
        eveSwitches_ = static_cast<uint8_t>((data[7] ? (1u << TXT16)    : 0u) |
                                            (data[8] ? (1u << TXTGREEN) : 0u));
    } else if (data[2] >= 3 && len >= 9) {
        eveSwitches_ = data[7];
        cpreg_       = data[8];
        if (data[2] >= 4 && len >= 10) rvbMode_ = data[9] & 0x03;
    }
    if (!isEve()) eveSwitches_ = 0;
    // Snapshot-load and rewind move cycleCounter backwards; edges recorded
    // on the abandoned timeline are stamped in the FUTURE and would win
    // every latchBefore() scan, replaying a mode the restored machine never
    // entered (MachineSnapshot resets Memory's video event log for exactly
    // this clock-jump reason — this ring is the card's parallel log).
    latchRingHead_ = 0;
    latchRingSize_ = 0;
    syncAuxShadow();
}
