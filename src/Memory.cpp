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

#include "Memory.h"

#include "CharRomDump.h"
#include "MemoryProfile_IIcClass.h"
#include "CassetteDevice.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "Logger.h"
#include "M6502.h"
#include "NoSlotClock.h"
#include "SpeakerDevice.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace {
// `POM2_TRACE_IIE_REBOOT=1` enables verbose tracing of IIe paging soft-
// switch writes ($C001-$C00F), the auto-INTCXROM flip-flop, and (in
// M6502.cpp) the PC-landing-on-$FA62 reset-entry hook. The three sites
// share the same env var so a single export captures the entire reboot
// path. Resolved once at startup via static init.
bool iieRebootTraceEnabled()
{
    static const bool e = []() {
        const char* env = std::getenv("POM2_TRACE_IIE_REBOOT");
        return env && env[0] != '\0' && env[0] != '0';
    }();
    return e;
}
}

Memory::Memory()
{
    mem.fill(0);
    writable.fill(true);
    lcBank1.fill(0);
    lcBank2.fill(0);
    lcHigh.fill(0);
    // ROM region. The Apple II //e bank-switched language card is NOT
    // modelled — $D000-$FFFF is plain ROM. The slot ROM range
    // $C100-$C7FF is NOT marked as ROM here: SlotBus owns that window
    // and dispatches reads to plugged cards (writes are dropped inside
    // Memory::memWrite).
    markRomRegion(0xD000, 0xFFFF);  // Applesoft + Monitor

    // Default reset vector points at $F800 (Monitor cold start) so a
    // fresh boot without ROM loaded still runs *something* (BRK loop)
    // instead of jumping into uninitialised memory.
    mem[0xFFFC] = 0x00;
    mem[0xFFFD] = 0xF8;
    // IRQ/BRK and NMI both land at $F800 by default.
    mem[0xFFFE] = 0x00;
    mem[0xFFFF] = 0xF8;
    mem[0xFFFA] = 0x00;
    mem[0xFFFB] = 0xF8;

    // POM2_TRACE_HANG implies the bank-mismatch detector too, so one env var
    // captures everything in a single run.
    if (std::getenv("POM2_TRACE_BANK") || std::getenv("POM2_TRACE_HANG")) {
        bankTrace_ = true;
        writeBank_.assign(0xC000, static_cast<int8_t>(-1));
        writeVal_.assign(0xC000, 0);
        std::fprintf(stderr, "[BANK] write/read bank-mismatch detector ARMED\n");
    }
    refreshReadFastFlags();
}

void Memory::noteBankWrite(uint16_t addr, bool toAux, uint8_t v)
{
    if (!bankTrace_ || addr >= 0xC000) return;
    writeBank_[addr] = toAux ? 1 : 0;
    writeVal_[addr]  = v;
}

void Memory::checkBankRead(uint16_t addr, bool fromAux, uint8_t v)
{
    if (!bankTrace_ || addr >= 0xC000) return;
    const int8_t wb = writeBank_[addr];
    if (wb < 0) return;                         // never written this session
    const int rb = fromAux ? 1 : 0;
    if (rb != wb && v != writeVal_[addr]) {
        static int n = 0;
        if (n++ < 300) {
            std::fprintf(stderr,
                "[BANK] MISMATCH $%04X: wrote bank%d=%02X, read bank%d=%02X "
                "(80STORE=%d RAMRD=%d RAMWRT=%d ALTZP=%d PAGE2=%d HIRES=%d) cyc=%llu\n",
                addr, wb, writeVal_[addr], rb, v,
                (iieMemMode & MF_80STORE) ? 1 : 0, (iieMemMode & MF_RAMRD) ? 1 : 0,
                (iieMemMode & MF_RAMWRT) ? 1 : 0, (iieMemMode & MF_ALTZP) ? 1 : 0,
                display.page2 ? 1 : 0, display.hiRes ? 1 : 0,
                static_cast<unsigned long long>(cycleCounter));
        }
    }
}

void Memory::setCpu(M6502* c)
{
    cpu = c;
    // Re-install the SlotBus IRQ router whenever the CPU is rewired. The
    // closure captures the CPU pointer by value so a later swap doesn't
    // dangle — re-issuing setCpu() re-installs against the new pointer.
    // An empty function on `c == nullptr` disconnects stray assertIrq()
    // calls from cards that haven't been unplugged yet.
    if (c) {
        slots.setIrqRouter([c](int slot, bool asserted) {
            c->setIrqLine(slot, asserted);
        });
    } else {
        slots.setIrqRouter({});
    }
}

std::string Memory::busStateSummary() const
{
    if (!lcReadRam && !lcWriteEnable) return " (LC: ROM)";
    std::string s = " (LC: ";
    s += lcReadRam ? "RAM" : "ROM";
    s += lcBank2Active ? " bank2" : " bank1";
    s += lcWriteEnable ? " writable)" : " write-protected)";
    return s;
}

void Memory::markRomRegion(uint16_t lo, uint16_t hi)
{
    for (int a = lo; a <= hi; ++a) writable[a] = false;
    // A watched address inside the range is already forced non-writable, so
    // the loop above changed nothing for it — but its SHADOW still claims the
    // old permission, and the shadow is what memWriteSlow honours. Update it,
    // or a ROM region marked while a watchpoint is armed (a profile switch
    // reloading ROMs) would leave that one address writable.
    if (writeWatch_.empty()) return;
    for (int a = lo; a <= hi; ++a)
        writeWatch_[a] = static_cast<uint8_t>(writeWatch_[a] & ~kWatchWasWritable);
}

bool Memory::loadRomBytes(const uint8_t* src, size_t length, uint16_t addr)
{
    if (!src || length == 0) return false;
    if (static_cast<size_t>(addr) + length > 0x10000) return false;
    std::memcpy(mem.data() + addr, src, length);
    return true;
}

int Memory::loadAppleIIRom(const char* filename, bool pickLower16KFor32K)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f) {
        lastError = std::string("Cannot open ROM: ") + filename;
        pom2::log().warn("ROM", lastError);
        return 0;
    }
    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    // Common image sizes:
    //   12 KB ($D000-$FFFF) = Apple II+ Autostart + Applesoft
    //   16 KB ($C000-$FFFF) = Apple //e — split into the internal I/O ROM
    //     ($C100-$CFFF, motherboard firmware) and the main ROM ($D000-$FFFF)
    //     when iieMode is on. On II+, the same 16 KB image just loads at
    //     $C000 and skips the I/O page (legacy behaviour).
    //   32 KB = TWO common layouts, indistinguishable by size alone:
    //     * Apple //e "system + video" combined dump. The 16 KB firmware
    //       lives at file offsets 0x4000-0x7FFF (reset vector at 0x7FFC
    //       maps to $FFFC); lower 16 KB carries video / char data we
    //       don't load through this path. → pickLower16KFor32K = false.
    //     * Apple //c / //c+ two-bank firmware dump. Bank 0 (cold-reset
    //       entry) at file offsets 0x0000-0x3FFF; bank 1 (alt firmware,
    //       AppleTalk / MouseText / SmartPort drivers) at 0x4000-0x7FFF.
    //       The two banks are swapped at runtime via the $C028 ROMBANK
    //       soft switch; bank 0 must be the one mapped at reset.
    //       → pickLower16KFor32K = true.
    uint16_t loadAddr = 0;
    bool iieFromUpper16K = false;
    size_t  skipBytes = 0;  // leading bytes to drop from the file
    if (size == 12 * 1024) {
        loadAddr = 0xD000;
    } else if (size == 16 * 1024) {
        loadAddr = 0xC000;
    } else if (size == 20 * 1024) {
        // Some Apple II+ dumps (notably the MAME "apple2_plus" combined
        // pack) prepend 4 KB of filler — typically zeros or an unused
        // alternate Integer BASIC bank — to the real 16 KB
        // $C000-$FFFF firmware. The high 16 KB is what every Apple II+
        // expects at $C000 onwards. Skip the first 4 KB so the loader
        // doesn't poke ROM bytes into user RAM at $B000-$BFFF (the old
        // "best effort" branch landed loadAddr there, which clobbered
        // the user-RAM region with whatever pad bytes the file carried).
        loadAddr  = 0xC000;
        skipBytes = 0x1000;
    } else if (size == 32 * 1024) {
        loadAddr = 0xC000;
        iieFromUpper16K = !pickLower16KFor32K;
    } else if (size >= 0x800 && size <= 0x10000) {
        // Best effort: fit at the high end so vectors land at $FFFA-$FFFF.
        loadAddr = static_cast<uint16_t>(0x10000 - size);
    } else {
        lastError = "Unexpected ROM size: " + std::to_string(size);
        pom2::log().warn("ROM", lastError);
        return 0;
    }

    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (!f) {
        lastError = "Short read";
        return 0;
    }

    // Slice out the firmware payload — for 32 KB dumps this drops the
    // unused half (upper for //c-style, lower for //e-style). After
    // this, `payload` is a 16 KB block that covers $C000-$FFFF (or
    // 12 KB for II+, or whatever the user gave us in the best-effort
    // branch).
    const uint8_t* payload      = buf.data() + skipBytes;
    size_t         payloadSize  = size - skipBytes;
    const uint8_t* altBankSrc   = nullptr;   // //c 32 KB bank 1, else null
    if (size == 32 * 1024) {
        if (iieFromUpper16K) {
            payload     = buf.data() + 0x4000;   // //e layout
        } else {
            payload     = buf.data();            // //c bank 0 (active at reset)
            // Bank 1 (upper 16 KB) feeds the $C028 ROMBANK toggle. Bytes
            // 0x000-0x0FF (the $C000-$C0FF soft-switch shadow) are never
            // returned — that range always routes through softSwitchAccess
            // regardless of the active bank. IIcClassProfile stashes the
            // rest (0x100-0xFFF mirror $C100-$CFFF; 0x1000-0x3FFF mirror
            // $D000-$FFFF).
            altBankSrc  = buf.data() + 0x4000;
        }
        payloadSize = 0x4000;
    }

    // Unified //c-class detection — runs for BOTH 16 KB and 32 KB iieMode
    // dumps (matches MAME `apple2e.cpp:1275-1299` which probes the ROM
    // regardless of size). 16 KB //c rev-255 needs this too — without it
    // INTCXROM isn't forced and $C100-$C7FF reads return slot-bus $FF
    // (D-1-1). On a //c-class ROM we install an IIcClassProfile (it does
    // the //c+ probe + alt-firmware stash); II/II+/IIe clear it.
    if (iieMode && payloadSize >= 0x3c00 && payload[0x3bc0] == 0x00) {
        iicProfile_ = std::make_unique<IIcClassProfile>(
            payload, payloadSize, altBankSrc,
            iwmDevice, smartPortHub, iwmAuthoritative);
        iicProfile_->setExternalSmartPort(externalSmartPort_);
            // //c boots with INTCXROM forced on. applyProfile calls
        // resetSoftSwitches BEFORE loadAppleIIRom, so its MF_INTCXROM
        // hook (now gated on iicProfile_) doesn't catch the just-detected
        // class — set it here too.
        iieMemMode |= MF_INTCXROM;
    } else {
        iicProfile_.reset();
        }
    refreshReadFastFlags();   // romFastRead_ follows iicProfile_

    if (iieMode && payloadSize == 16 * 1024) {
        // IIe split: bytes 0x0000-0x00FF map to $C000-$C0FF (I/O page,
        // ignored — those addresses are soft switches, not ROM). Bytes
        // 0x0100-0x0FFF go into the internal I/O ROM, callable via
        // INTCXROM=on or SLOTC3ROM=off (slot 3 only). Bytes 0x1000-0x3FFF
        // load into $D000-$FFFF as the main Applesoft + Monitor ROM.
        for (size_t i = 0x100; i < 0x1000; ++i) {
            internalIORom[i] = payload[i];
        }
        for (size_t i = 0x1000; i < payloadSize; ++i) {
            uint16_t addr = static_cast<uint16_t>(0xC000 + i);
            mem[addr] = payload[i];
        }
    } else {
        // II+ path (or non-16-KB): linear load, skipping the I/O page so
        // soft switches keep working when a 16 KB II+ image is provided.
        for (size_t i = 0; i < payloadSize; ++i) {
            uint16_t addr = static_cast<uint16_t>(loadAddr + i);
            if (addr >= 0xC000 && addr <= 0xC0FF) continue;
            mem[addr] = payload[i];
        }
    }
    pom2::log().info("ROM", std::string("Loaded ") + filename + " at $" +
                     [&]{ char b[8]; std::snprintf(b, 8, "%04X", loadAddr); return std::string(b); }() +
                     (iieMode ? " (IIe)" : ""));
    return 1;
}

int Memory::loadCharRom(const char* filename, int bank)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f) {
        lastError = std::string("Cannot open char ROM: ") + filename;
        pom2::log().warn("ROM", lastError);
        return 0;
    }
    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    // 2K (II/II+) and 4K (IIe-class) dumps are normalized below. An 8K dump is
    // an INTERNATIONAL //e video ROM: MAME's `gfx1` region is 8K = two 4K
    // banks, and the machine's charset switch picks one. The US apple2ee fills
    // both banks with the same 4K dump (342-0265-a.chr at offset 0 AND 0x1000);
    // the French apple2eefr ships one 8K part, 342-0274-a.e9, whose low bank is
    // the FR-CA set and whose high bank is the US set — verified by CRC against
    // POM2's own 4K apple2e_char_frca.rom / apple2e_char.rom. So an 8K dump is
    // handled by selecting `bank` and running the ordinary 4K path on it, which
    // keeps one normalization routine rather than two. Anything else is
    // rejected rather than stored raw (the renderer gates on size >= 2048 and
    // would happily draw garbage).
    if (size != 2048 && size != 4096 && size != 8192) {
        lastError = "Char ROM must be 2K, 4K or 8K, got " + std::to_string(size);
        return 0;
    }
    characterRom.resize(size);
    f.read(reinterpret_cast<char*>(characterRom.data()), size);
    charRomDualBank_ = false;
    if (size == 8192 && f && bank < 0) {
        // DUAL-BANK: keep both 4 KB sets; annunciator 2 selects the live one
        // at render time (charRomActiveData). This is the real localized-//e
        // wiring — the char ROM's A12 is driven by AN2 ($C05C/$C05D) — and it
        // is what lets a demo switch its whole font mid-run (French Touch
        // "Block ASCII Anthology": normal text ↔ block glyphs). Normalise each
        // 4 KB half in place, then skip the single-bank normalisation below.
        const pom2::CharRomFacts f0 =
            pom2::normaliseCharRom(characterRom.data(),        4096);
        (void)pom2::normaliseCharRom(characterRom.data() + 4096, 4096);
        charRomLowercase_ = f0.hasLowercase;
        charRomDualBank_  = true;
        pom2::log().info("ROM",
            std::string("Char ROM: 8K two-set dump, dual-bank (AN2 $C05C/$C05D "
                        "selects the live 4K set): ") + filename);
        return 1;
    }
    if (size == 8192 && f) {
        // Collapse to the selected 4K bank, then fall through to the ordinary
        // 4K normalization. Done before the short-read check below so a failed
        // read still lands in the same error path.
        const size_t off = (bank == 1) ? 4096u : 0u;
        characterRom.erase(characterRom.begin() + static_cast<long>(off) + 4096,
                           characterRom.end());
        characterRom.erase(characterRom.begin(),
                           characterRom.begin() + static_cast<long>(off));
        size = 4096;
        pom2::log().info("ROM",
            std::string("Char ROM: 8K international dump, bank ") +
            std::to_string(bank == 1 ? 1 : 0) + " of " + filename);
    }
    if (!f) {
        // Short read — don't leave a partial ROM the renderer would treat as
        // valid (it gates on size >= 2048 and would draw the garbage tail).
        characterRom.clear();
        lastError = std::string("Short read on char ROM: ") + filename;
        return 0;
    }

    // Convention normalisation + the lowercase question both live in
    // CharRomDump: they are facts about the DUMP, not about the memory
    // map, and two dumps of the same size can need opposite treatment.
    const pom2::CharRomFacts facts =
        pom2::normaliseCharRom(characterRom.data(), size);
    charRomLowercase_ = facts.hasLowercase;
    if (size == 2048 && !facts.bit7Marker) {
        pom2::log().info("ROM",
            "Char ROM: 2 KB dump with no bit-7 range marker (Videx-style), "
            "split by offset instead");
    }

    pom2::log().info("ROM",
        "Loaded char ROM (" + std::to_string(size) + " B" +
        (charRomLowercase_ ? ", lowercase" : ", uppercase only") + "): " +
        filename);
    return 1;
}

void Memory::cassetteAdvanceCycles(int cycles)
{
    cassette->advanceCycles(cycles);
}

void Memory::advanceCyclesVideo()
{
    // Scanline-accurate VBL transition detection. Apple II video timing:
    // 65 CPU cycles/line; 262 lines NTSC / 312 PAL. Visible: 0..191. VBL is
    // 192..261 (NTSC) / 192..311 (PAL) → the VBL/frame period must follow the
    // standard, or a French Touch demo that measures the VBL frame length to
    // detect PAL vs NTSC sees the wrong machine. The "long cycle" (1 extra
    // every 65) isn't modelled; nominal 65/line is close enough.
    constexpr uint64_t kCyclesPerScanline = 65;
    // Relaxed: the standard is a plain enum flipped by the UI under the
    // state lock; the derived `frameCycles` is re-checked every call below,
    // so no ordering is needed — and this runs once per emulated instruction.
    const uint64_t kScanlinesPerFrame =
        static_cast<uint64_t>(pom2VideoTiming(
            videoStandard_.load(std::memory_order_relaxed)).scanlinesPerFrame);
    constexpr uint64_t kVisibleScanlines  = 192;
    // Track the start-of-frame cycle incrementally instead of deriving the
    // scanline with `(cycleCounter / 65) % scanlinesPerFrame` every time.
    //
    // This runs once per EMULATED INSTRUCTION (~4 M times per 10 emulated
    // seconds), and `% kScanlinesPerFrame` is a true hardware division: the
    // divisor is a runtime value, so unlike `/ 65` — a compile-time constant the
    // compiler turns into a multiply-shift — it cannot be strength-reduced.
    // Removing it measured **15 % off the emulation core** (Callgrind +
    // wall-clock, 2026-07-30). Note Callgrind *understates* it: a `div` is one
    // instruction but 20-40 cycles.
    //
    // The subtraction below is deliberately unsigned. `cycleCounter` does not
    // only march forward — `setCycleCounter()` moves it arbitrarily on a
    // snapshot restore or a rewind, backwards included. A backwards jump wraps
    // the difference to a huge value, which fails the `< 2 * frameCycles` test
    // and lands in the resync branch, so the base self-heals in one division on
    // the next call. (An earlier draft reset the base to 0 and let the `while`
    // loop walk forward instead — that would have spun for millions of
    // iterations on any rewind with a large cycle counter.)
    //
    // The base is only meaningful for the frame period it was aligned to; the
    // invariant the incremental scheme rests on is
    // `vblFrameBase_ % frameCycles == 0`. The period is a LIVE input:
    // `setVideoStandard()` flips NTSC↔PAL on an already-running machine
    // (profile switch), and the rollover test below can never notice, because
    // the two periods sit within a factor of two of each other (17030 vs
    // 20280) — `sinceBase` always lands in the ordinary-rollover branch, which
    // carries the stale residue forward forever. A boot-in-NTSC then
    // switch-to-//c-PAL put the VBL edge on scanline 252 instead of 192, out
    // of step with `$C019`, `pushVideoEventLocked` and
    // `Apple2Display::frameCycleToPos` (all of which take a true modulo). So
    // re-derive the base whenever the period moves, comparing the DERIVED
    // `frameCycles` rather than the video standard: any future input feeding
    // into the period re-aligns the base automatically instead of silently
    // reintroducing the phase error.
    const uint64_t frameCycles = kCyclesPerScanline * kScanlinesPerFrame;
    if (frameCycles != vblFrameCycles_) {
        vblFrameCycles_ = frameCycles;
        vblFrameBase_   = cycleCounter - (cycleCounter % frameCycles);
    }
    const uint64_t sinceBase   = cycleCounter - vblFrameBase_;   // wraps if behind
    if (sinceBase >= frameCycles) {
        vblFrameBase_ = (sinceBase < 2 * frameCycles)
                            ? vblFrameBase_ + frameCycles          // ordinary rollover
                            : cycleCounter - (cycleCounter % frameCycles);  // resync
    }
    const uint64_t scanline = (cycleCounter - vblFrameBase_) / kCyclesPerScanline;
    const bool nowActive = scanline < kVisibleScanlines;

    // Line-boundary edge, same as the $C019 read path (see the long note
    // there: an intra-line phase was tried both ways against MAD EFFECT and
    // measured to be wrong). This path drives the //c-class VBLINT *latch*
    // and is evaluated per tick rather than per cycle, so it would be coarse
    // regardless.

    // Edge: active video → VBL. `$C05B` (EnVBL) arms the per-frame VBL
    // interrupt. The old blanket "never assert the CPU IRQ line" existed
    // because POM2 did not model IOUDIS: with IOUDIS disabled the same
    // address is the AN1 annunciator (legacy II/II+ behaviour), and the
    // many programs that poke $C05B for paddle / annunciator reasons
    // would have raised an IRQ with no handler installed (ProDOS crash).
    //
    // POM2 models IOUDIS now (2026-07-30), and $C05A/$C05B only reach
    // the VBL mask on a //c-class machine with IOUDIS clear — the real
    // mouse/VBL switch decode, gated in softSwitchAccess. So on //c-class
    // the arm is unambiguous and the line CAN be driven, which is what a
    // //c PAL demo (Le Chat Mauve / French Touch target) uses as its
    // 50 Hz frame sync: without it such a demo either spins on its
    // "wait for frame" flag forever or free-runs and tears. MAME raises
    // IRQ_VBL for //c-class the same way (`apple2e.cpp` m_isiic).
    //
    // IIe keeps the polling-only behaviour: there $C05A/$C05B really are
    // plain annunciators (POM2 overlays the mask for the vbl_smoke_test
    // contract), so asserting would resurrect the original crash.
    if (vblWasActive && !nowActive) {
        if (iieMode && vblIrqMask) {
            vblIrqPending = true;
            if (iicProfile_ && cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, true);
        }
    }
    vblWasActive = nowActive;

    // ── Per-video-frame publication of the beam-racing event log ────────
    // The recording frame closes at each video-frame boundary (65 × 262/312
    // cycles) and becomes the published frame the UI renders from. Decoupled
    // from both the worker's CPU budget tick (17045/20313 ≠ one video frame)
    // and the UI's vsync: a 60 Hz UI over 50 Hz PAL content re-renders the
    // same published frame instead of stealing a half-recorded one — the
    // old tick-bracket model dropped every event recorded between the UI's
    // take and the next tick (~1 empty take in 6 under PAL → mid-scanline
    // effects like French Touch *Mad Effect* flickered at ~10 Hz).
    // Legacy mode: tests bracket synchronously via beginVideoEventFrame().
    //
    // The frame boundary is `vblFrameBase_` moving — it IS
    // `cycleCounter - cycleCounter % frameCycles`, maintained incrementally
    // above, so compare it rather than divide again: `cycleCounter /
    // frameCycles` here was a second runtime-divisor 64-bit division per
    // emulated instruction (the first one was removed in 2026-07; this one
    // arrived with the per-video-frame publication and measured ~2 % of the
    // core on an M1, more on a Cortex-A72 where `udiv` is slower).
    if (!legacyEventBracket_ && vblFrameBase_ != lastVideoFrameStart_) {
        lastVideoFrameStart_ = vblFrameBase_;
        const uint64_t newFrameStart = vblFrameBase_;
        std::lock_guard<std::mutex> lk(stateMutex);
        publishedFrameStart_ = displayAtFrameStart_;
        // An instruction can straddle the frame boundary: its soft-switch
        // event is stamped `cycleCounter + currentInstructionCycles`, which
        // may land PAST the boundary (rawLine wrapped to ~0) while
        // publication only runs here, after the instruction. Publishing
        // such an event into the closing frame applied the switch one
        // frame early across all of frame N. Stamps are non-decreasing, so
        // boundary-crossers form the tail of the log — carry them into the
        // new recording frame instead.
        auto firstNew = videoEvents_.begin();
        while (firstNew != videoEvents_.end() &&
               firstNew->emuCycle < newFrameStart)
            ++firstNew;
        std::vector<VideoEvent> carry(firstNew, videoEvents_.end());
        videoEvents_.erase(firstNew, videoEvents_.end());
        publishedEvents_     = std::move(videoEvents_);
        videoEvents_         = std::move(carry);
        displayAtFrameStart_ = display;   // state at scanline 0 of the new frame
    }

    // Nothing above can change again before the VBL edge of this frame (if
    // it is still ahead) or, failing that, the frame boundary — so that is
    // the next cycle the inline gate lets this function run.
    const uint64_t edge = vblFrameBase_ + kVisibleScanlines * kCyclesPerScanline;
    vblNextEventCycle_ = (cycleCounter < edge) ? edge : vblFrameBase_ + frameCycles;
}

void Memory::beginVideoEventFrame()
{
    // Legacy synchronous bracket (tests): from here on, recording is gated
    // by the open flag and takeVideoEvents() moves the log out directly —
    // the per-video-frame publication in advanceCycles() stands down.
    std::lock_guard<std::mutex> lk(stateMutex);
    legacyEventBracket_  = true;
    displayAtFrameStart_ = display;
    videoEvents_.clear();
    videoEventFrameOpen_ = true;
}

std::vector<Memory::VideoEvent> Memory::takeVideoEvents()
{
    std::lock_guard<std::mutex> lk(stateMutex);
    if (legacyEventBracket_) {
        videoEventFrameOpen_ = false;
        return std::move(videoEvents_);
    }
    // Published mode: COPY, not move — the UI re-renders the same frame
    // when no new one has been published (60 Hz vsync over 50 Hz content).
    return publishedEvents_;
}

void Memory::recordVideoEvent(VideoEventKind kind, bool value)
{
    std::lock_guard<std::mutex> lk(stateMutex);
    if (!videoEventFrameOpen_) return;
    pushVideoEventLocked(kind, value);
}

void Memory::pushVideoEventLocked(VideoEventKind kind, bool value)
{
    if (!videoEventFrameOpen_) return;
    // PAL frames are 312 scanlines (vs 262 NTSC); the horizontal 65-cycle line
    // and 192 visible lines are the same. Using the active standard's geometry
    // keeps mid-frame soft-switch edges on the right scanline for beam-racing.
    const VideoTiming& t = pom2VideoTiming(videoStandard_.load());
    const uint64_t kCyclesPerScanline = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t kScanlinesPerFrame = static_cast<uint64_t>(t.scanlinesPerFrame);
    const uint64_t kVisibleScanlines  = static_cast<uint64_t>(t.visibleScanlines);
    const uint64_t now = cycleCounter +
        (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0);
    const uint64_t rawLine = (now / kCyclesPerScanline) % kScanlinesPerFrame;
    // A switch thrown during VBL (rawLine >= 192) must NOT be replayed
    // inside the visible frame: the beam already finished the picture in
    // the pre-switch state (throwing mode switches in VBL is the canonical
    // tear-free idiom). Stamp it as scanline 192 — Apple2Display's replay
    // loops only consume scanlines 0..191, so the event sorts after every
    // visible position and is skipped; its effect reaches the NEXT frame
    // through displayAtFrameStart_. (Clamping to 191, as previously done,
    // painted a spurious post-switch split on the last visible line.)
    const uint16_t scanline = static_cast<uint16_t>(
        rawLine < kVisibleScanlines ? rawLine : kVisibleScanlines);

    videoEvents_.push_back({now, scanline, kind, value});
}

void Memory::resetSoftSwitchesWarm()
{
    // II/II+ machine_reset (apple2.cpp:325-331) only clears the cnxx
    // tracker + kbd strobe — LC bank-select, display switches and the
    // expansion-ROM latch SURVIVE. IIe/IIc/IIc+ reset_w (apple2e.cpp:
    // 1453-1508) runs the full MMU/IOU/LC list — same as our cold reset.
    if (iieMode) {
        resetSoftSwitches();
        return;
    }
    keyboard_.reset();   // abandon any in-flight paste, drop the strobe
    // NB: cnxx-slot tracker analogue lives in SlotBus, which the caller
    // (EmulationController::softReset) drives via slotBus().reset();
    // nothing else needs touching here on II/II+.
}

bool Memory::chatMauveBlockedBySlot3() const
{
    SlotPeripheral* card = slots.peripheral(3);
    if (!card) return false;                 // empty slot: Eve window free
    // A Chat Mauve IS allowed to sit in slot 3 and answer there; only a
    // FOREIGN card (SSC, Mockingboard…) blocks the window. Compared by
    // name to avoid dragging the card's header into Memory.
    return card->name() != "Le Chat Mauve";
}

void Memory::resetSoftSwitches()
{
    std::lock_guard<std::mutex> lk(stateMutex);
    display = DisplayState{};
    // Beam-racing log: a reset wipes the soft-switch timeline. Replaying
    // pre-reset events (or a stale published frame) against the wiped state
    // would paint ghost segments for up to one frame — drop both and resync
    // the frame-start snapshots to the fresh state.
    videoEvents_.clear();
    publishedEvents_.clear();
    displayAtFrameStart_ = display;
    publishedFrameStart_ = display;
    lcReadRam     = false;
    // Sather "Understanding the Apple //e" Fig 5.13: post-reset LC state is
    // read ROM / write RAM enabled / bank 2 selected / no pre-write. MAME
    // `apple2e.cpp:1227-1232` + `:1492-1497` sets `m_lcwriteenable=true` on
    // both machine_reset and reset_w. The Language Card on II/II+ powers up
    // in the same state, so the rule applies universally.
    lcWriteEnable = true;
    lcBank2Active = true;
    lcPrewrite    = false;
    iieMemMode    = 0;
    intC8Rom      = false;   // //e expansion-window auto-INTCXROM flip-flop
    // VBL interrupt: disarm AND drop the line. Without this a //c that had
    // enabled it ($C05B) kept re-asserting IRQ_SRC_VBL on the very next
    // frame edge into a freshly reset machine with no handler — and the
    // guest could no longer turn it off, because $C05A (DisVBL) only
    // decodes while IOUDIS is CLEAR and the reset below forces it back
    // TRUE. (M6502::reset clears its own source mask, so only the
    // re-assertion mattered.)
    vblIrqMask    = false;
    vblIrqPending = false;
    if (cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, false);
    if (iicProfile_) iicProfile_->onResetSoftSwitches();  // ROMBANK → bank 0
    // //c boots with INTCXROM forced on (MAME `apple2e.cpp:1273`,
    // `apple2e.cpp:1467-1475`). Gate on `isIIcClass` so BOTH 16 KB
    // rev-255 //c dumps and 32 KB rev-0/3/4 + //c+ dumps re-force the
    // bit on every reset. (Pre-Theme-6 this was gated on iicHasAltBank
    // and missed the 16 KB case — D-1-1/D-3-1.) The internal ROM stays
    // mapped either way (see the isIIcClass gate in memRead). Setting
    // it here keeps $C015 (RDCXROM) consistent with what real //c
    // firmware sees when probing the switch.
    if (iicProfile_ && iicProfile_->forcesIntCxRom()) iieMemMode |= MF_INTCXROM;
    // IOUDIS resets to true on every reset (MAME `apple2e.cpp:1224`),
    // gating the IOU/mouse softswitches off until the firmware clears
    // it via $C07F. Shared by IIe, IIc, IIc+ even though IIe ignores
    // SET/CLR — the read at $C07E still returns the bit.
    ioudis = true;
    // RamWorks III — MAME `a2eramworks3.cpp:65-68 device_reset` snaps
    // `m_bank = 0` on every reset. Match that: swap the visible aux
    // back to bank 0 so Ctrl-Reset / F12 don't leave the user looking
    // at whatever bank software last selected. Data in all banks is
    // preserved (reset clears the bank selector, not the DRAM).
    if (iieMode && ramWorksBanks_ > 1) {
        ramWorksSwapToBank(0);
    }
    // Annunciators AN0-AN2. The 74LS259 addressable latch that drives them
    // has its /CLR tied to the reset line, so every reset drops all four
    // outputs — MAME `apple2e.cpp` machine_reset zeroes `m_an0..m_an3`
    // alongside the rest of the IOU. AN3 lives in `display` and was already
    // cleared by the `display = DisplayState{}` above; these three were not,
    // and AN2 is not decorative: on an 8 KB international character
    // generator it is wired to the ROM's A12, so a stale AN2 left a
    // freshly-reset machine rendering the second 4 KB font
    // (`charRomBankOffset`). Same reason they are in the snapshot trailer.
    an0 = false;
    an1 = false;
    an2 = false;
    keyboard_.reset();   // abandon any in-flight paste, drop the strobe
}

void Memory::clearRam()
{
    // MAME-faithful power-on RAM pattern: alternating `00 FF 00 FF…`
    // (apple2.cpp:294-298 + apple2e.cpp:1014-1035). Real silicon DRAM
    // settles into this pattern from the way the cell columns refresh;
    // some software (RAM diagnostics, demo RNG seeds) probes it
    // deliberately. The Language Card is RAM too, so a power-cycle
    // clears it even though its address window overlaps motherboard
    // ROM. Pre-Theme-11 POM2 zero-filled (F-1-2/B-1-1/C-1-1/D-1-3/E-1-1).
    auto fill00FF = [](auto& span, size_t bytes) {
        for (size_t i = 0; i < bytes; i += 2) {
            span[i]     = 0x00;
            if (i + 1 < bytes) span[i + 1] = 0xFF;
        }
    };
    fill00FF(mem,    0xC000);
    fill00FF(lcBank1, lcBank1.size());
    fill00FF(lcBank2, lcBank2.size());
    fill00FF(lcHigh,  lcHigh.size());
    if (iieMode) {
        fill00FF(aux,        aux.size());
        fill00FF(auxLcBank1, auxLcBank1.size());
        fill00FF(auxLcBank2, auxLcBank2.size());
        fill00FF(auxLcHigh,  auxLcHigh.size());
        // RamWorks III backing — wipe every bank slot, snap back to
        // bank 0 (MAME `device_reset`-equivalent, plus a cold RAM
        // wipe which `device_reset` itself doesn't do).
        if (ramWorksBanks_ > 1) {
            fill00FF(ramWorksBacking_, ramWorksBacking_.size());
            ramWorksBank_ = 0;
        }
    }
}

void Memory::setIIEMode(bool on)
{
    iieMode = on;
    refreshReadFastFlags();
    iieMemMode = 0;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        display.altChar     = false;
        display.eightyStore = false;
        display.eightyCol   = false;
        display.dhgr        = false;
    }
    // Drop any RamWorks backing when leaving IIe (the card is aux-slot
    // only). Re-enabling IIe leaves the user setting to MainWindow.
    if (!on) {
        ramWorksBanks_ = 1;
        ramWorksBank_  = 0;
        ramWorksBacking_.clear();
        ramWorksBacking_.shrink_to_fit();
    }
}

void Memory::setRamWorksBanks(uint32_t banks)
{
    if (banks < 1) banks = 1;
    if (banks > kRamWorksMaxBanks) banks = kRamWorksMaxBanks;
    ramWorksBanks_ = banks;
    ramWorksBank_  = 0;
    // Backing holds ONE slot per bank (including bank 0 — we snapshot
    // the visible buffers into it whenever leaving bank 0). When
    // banks == 1 the backing is empty — stock IIe path, no swap.
    if (banks > 1) {
        ramWorksBacking_.assign(
            static_cast<size_t>(banks) * kRamWorksBankStride, 0u);
    } else {
        ramWorksBacking_.clear();
        ramWorksBacking_.shrink_to_fit();
    }
}

// MAME `a2eramworks3.cpp:108-115 write_c07x`: bank index = data & 0x7F.
// One backing slot per bank (including bank 0). Swap is symmetric:
// snapshot visible → backing[prev], advance ramWorksBank_, load
// backing[curr] → visible. The visible buffers (`aux`, `auxLcBank1/2`,
// `auxLcHigh`) always reflect the active bank so the rest of Memory.cpp
// reads them directly without bank-aware indexing.
void Memory::ramWorksSwapToBank(uint8_t newBank)
{
    if (ramWorksBanks_ <= 1) return;
    // Clamp to populated banks via modulo. MAME does not clamp (it
    // allocates a fixed 8 MB array and reads garbage for unpopulated
    // slots — UB-adjacent in C++); we wrap. On a real RamWorks III with
    // fewer than 128 banks installed, the chip-select decoder aliases
    // higher-bank writes to populated banks anyway.
    newBank = static_cast<uint8_t>(newBank % ramWorksBanks_);
    if (newBank == ramWorksBank_) return;

    const size_t stride = kRamWorksBankStride;
    uint8_t* prev = ramWorksBacking_.data()
                  + static_cast<size_t>(ramWorksBank_) * stride;
    std::memcpy(prev,             aux.data(),         0x10000);
    std::memcpy(prev + 0x10000,   auxLcBank1.data(),  0x1000);
    std::memcpy(prev + 0x11000,   auxLcBank2.data(),  0x1000);
    std::memcpy(prev + 0x12000,   auxLcHigh.data(),   0x2000);

    ramWorksBank_ = newBank;

    const uint8_t* curr = ramWorksBacking_.data()
                        + static_cast<size_t>(newBank) * stride;
    std::memcpy(aux.data(),        curr,             0x10000);
    std::memcpy(auxLcBank1.data(), curr + 0x10000,   0x1000);
    std::memcpy(auxLcBank2.data(), curr + 0x11000,   0x1000);
    std::memcpy(auxLcHigh.data(),  curr + 0x12000,   0x2000);
}

// Extended-state blob layout version. Bump if the field order below changes.
static constexpr uint8_t kMemStateBlobVersion = 1;

void Memory::appendSnapshotState(std::vector<uint8_t>& out)
{
    auto putU8  = [&](uint8_t v) { out.push_back(v); };
    auto putU16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto putU32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putU64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putBytes = [&](const void* p, size_t k) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + k);
    };

    putU8(kMemStateBlobVersion);
    putU8(iieMode ? 1 : 0);
    putU16(iieMemMode);
    putU8(lcReadRam     ? 1 : 0);
    putU8(lcWriteEnable ? 1 : 0);
    putU8(lcBank2Active ? 1 : 0);
    putU8(lcPrewrite    ? 1 : 0);
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        putU8(display.textMode    ? 1 : 0);
        putU8(display.mixedMode   ? 1 : 0);
        putU8(display.page2       ? 1 : 0);
        putU8(display.hiRes       ? 1 : 0);
        putU8(display.eightyCol   ? 1 : 0);
        putU8(display.an3         ? 1 : 0);
        putU8(display.altChar     ? 1 : 0);
        putU8(display.dhgr        ? 1 : 0);
        putU8(display.eightyStore ? 1 : 0);
    }
    putU64(cycleCounter);
    putU64(paddles_.latchCycle());

    // Main Language-Card RAM (II/II+ and IIe main-bank LC live here; the
    // mem[] $D000-$FFFF region is always the ROM mirror).
    putBytes(lcBank1.data(), lcBank1.size());
    putBytes(lcBank2.data(), lcBank2.size());
    putBytes(lcHigh.data(),  lcHigh.size());

    putU32(ramWorksBanks_);
    putU8(ramWorksBank_);
    if (ramWorksBanks_ <= 1) {
        // Stock aux — serialize the visible aux + aux-LC arrays directly.
        putBytes(aux.data(),        aux.size());
        putBytes(auxLcBank1.data(), auxLcBank1.size());
        putBytes(auxLcBank2.data(), auxLcBank2.size());
        putBytes(auxLcHigh.data(),  auxLcHigh.size());
    } else {
        // RamWorks — flush the live visible bank back into the backing store
        // so the serialized backing is fully coherent (see ramWorksSwapToBank
        // for the per-bank layout), then dump the whole backing.
        uint8_t* slot = ramWorksBacking_.data()
                      + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
        std::memcpy(slot,           aux.data(),        0x10000);
        std::memcpy(slot + 0x10000, auxLcBank1.data(), 0x1000);
        std::memcpy(slot + 0x11000, auxLcBank2.data(), 0x1000);
        std::memcpy(slot + 0x12000, auxLcHigh.data(),  0x2000);
        putU32(static_cast<uint32_t>(ramWorksBacking_.size()));
        putBytes(ramWorksBacking_.data(), ramWorksBacking_.size());
    }

    // Trailer (appended, version stays 1 — older blobs simply lack it and
    // the loader falls back to the field's default): the //c-class on-board
    // SmartPort ROM-exposure gate. Without it, a rewind-ring entry captured
    // after a //c HDV/3.5" boot restored with armed=false, flipping
    // $C500-$C5FF back to the real //c firmware under a live ProDOS whose
    // device vector points into the stub — the next MLI call executed
    // unrelated ROM bytes.
    putU8(iicSmartPortArmed_ ? 1 : 0);

    // Second trailer: //c-class on-board device state, each section
    // self-identifying by magic so a blob may carry neither, either or
    // both. Both were previously absent, and both are cycle-sensitive:
    //   * IWMDevice holds eight absolute emuCycles stamps. A rewind rolls
    //     `cycleCounter` back while the IWM kept a larger `lastSync_`, so
    //     `sync()`'s walker stopped advancing and the controller froze
    //     until emulated time caught up to its pre-rewind position.
    //   * The //c+ MIG gate array's 2 KB RAM + page pointer came back
    //     zeroed, so the alt firmware read something other than what it
    //     had written.
    // Length-prefixed: the loader must be able to skip a section it does
    // not understand without losing the rest of the trailer.
    {
        std::vector<uint8_t> sect;
        if (iwmDevice) iwmDevice->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    {
        std::vector<uint8_t> sect;
        if (iicProfile_) iicProfile_->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Third section: paging/IOU flip-flops that were previously invisible
    // to snapshot/rewind — INTC8ROM (latched by $C3xx access, cleared by
    // $CFFF; a restored PC inside $C800-$CFFF slot firmware read the
    // wrong ROM without it), IOUDIS, and the //c VBL mask + pending
    // latch. Same length-prefixed convention: absent in older blobs →
    // live values kept.
    {
        std::vector<uint8_t> sect;
        sect.push_back(intC8Rom      ? 1 : 0);
        sect.push_back(ioudis        ? 1 : 0);
        sect.push_back(vblIrqMask    ? 1 : 0);
        sect.push_back(vblIrqPending ? 1 : 0);
        // Annunciators AN0-AN2, appended 2026-09-06. AN3 already travels
        // inside DisplayState; these three did not travel at all, and AN2
        // drives A12 of an 8 KB international character generator
        // (`charRomBankOffset`) — so a snapshot restore or a rewind of a
        // French Touch "Block ASCII" screen came back rendering the other
        // 4 KB font. The section is length-prefixed and grew at the END, so
        // a 4-byte blob from an older build still loads (the loader keeps
        // the live values for anything the section does not carry).
        sect.push_back(an0 ? 1 : 0);
        sect.push_back(an1 ? 1 : 0);
        sect.push_back(an2 ? 1 : 0);
        // Appended 2026-09-06, same grow-at-the-end rule:
        //  * `vblWasActive` is the EDGE detector behind the //c VBL IRQ.
        //    It defaults to true, so a restore taken while the beam was
        //    already inside the blanking interval re-armed the edge and
        //    fired one spurious VBL IRQ on the next boundary — the frame
        //    sync a //c PAL French Touch demo races against.
        //  * `iicCardWindow_` is the partner latch of `iicSmartPortArmed_`
        //    (already in the first trailer): it says whether the punched
        //    $C500 page has its $C800 window open. Restoring one without
        //    the other left the pair inconsistent.
        sect.push_back(vblWasActive   ? 1 : 0);
        sect.push_back(iicCardWindow_ ? 1 : 0);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Fourth section: the No-Slot Clock. It uses no slot, so it gets no
    // SLOTn section — but it is a bit-serial state machine walked over many
    // reads (see NoSlotClock.h), and it lives behind a Memory pointer, so
    // Memory's trailer is its home. Same length-prefixed, absent-tolerant
    // convention: a machine with no NSC writes a zero length.
    {
        std::vector<uint8_t> sect;
        if (noSlotClock_) noSlotClock_->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Fifth section: the two ON-BOARD Sony 3.5" mechanisms (//c+ internal
    // bay + the external port drive). They hang off the SmartPortHub, not
    // off a slot, so they get no SLOTn section — yet `iwmDevice` above
    // restores the controller that walks them. Restoring one without the
    // other left the IWM reading cells from the head position of the
    // abandoned future. Media is NOT captured (800 KB/frame); the ring is
    // cleared on a 3.5" write instead. Each drive is length-prefixed on its
    // own so a machine with one, both or neither round-trips.
    for (int which = 0; which < 2; ++which) {
        std::vector<uint8_t> sect;
        if (smartPortHub) {
            const pom2::Sony35Drive* d = (which == 0)
                ? smartPortHub->internal35() : smartPortHub->external35();
            if (d) d->appendSnapshotState(sect);
        }
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
}

void Memory::resetVideoEventLogForClockJump()
{
    videoEvents_.clear();
    publishedEvents_.clear();
    constexpr uint64_t kCyclesPerScanline = 65;   // as in advanceCycles
    const uint64_t kCyclesPerFrame =
        kCyclesPerScanline *
        static_cast<uint64_t>(
            pom2VideoTiming(videoStandard_.load()).scanlinesPerFrame);
    lastVideoFrameStart_ = cycleCounter - (cycleCounter % kCyclesPerFrame);
}

bool Memory::loadSnapshotState(const uint8_t* data, size_t n)
{
    size_t pos = 0;
    // `k <= n - pos`, NOT `pos + k <= n`: k comes straight out of the blob as
    // a uint32_t (the RamWorks backing size, among others), and on a 32-bit
    // size_t — the wasm32 target is one — `pos + 0xFFFFFFFF` wraps to
    // `pos - 1` and sails through, after which the memcpys below read tens of
    // megabytes past the payload. The subtraction form cannot overflow,
    // because pos <= n holds throughout.
    auto need  = [&](size_t k) { return pos <= n && k <= n - pos; };
    auto getU8 = [&]() -> uint8_t { return data[pos++]; };
    auto getU16 = [&]() -> uint16_t {
        uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2; return v;
    };
    auto getU32 = [&]() -> uint32_t {
        uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
        pos += 4; return v;
    };
    auto getU64 = [&]() -> uint64_t {
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        pos += 8; return v;
    };
    auto getBytes = [&](void* dst, size_t k) {
        std::memcpy(dst, data + pos, k); pos += k;
    };

    if (!need(1)) return false;
    if (getU8() != kMemStateBlobVersion) return false;
    // iieMode(1) + iieMemMode(2) + lc flags(4) + display(9) + 2×u64(16) = 32
    if (!need(32)) return false;
    (void)getU8();                       // iieMode — informational (mode is set by the profile)
    iieMemMode    = getU16();
    lcReadRam     = getU8() != 0;
    lcWriteEnable = getU8() != 0;
    lcBank2Active = getU8() != 0;
    lcPrewrite    = getU8() != 0;
    DisplayState ds;
    ds.textMode    = getU8() != 0; ds.mixedMode = getU8() != 0; ds.page2 = getU8() != 0;
    ds.hiRes       = getU8() != 0; ds.eightyCol = getU8() != 0; ds.an3   = getU8() != 0;
    ds.altChar     = getU8() != 0; ds.dhgr      = getU8() != 0; ds.eightyStore = getU8() != 0;
    cycleCounter     = getU64();
    vblNextEventCycle_ = 0;     // re-derive the VBL/frame gate from the new counter
    paddles_.setLatchCycle(getU64());
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        display = ds;
        // The restored clock invalidates the beam-racing event log:
        // events stamped with pre-restore (possibly FUTURE, on rewind)
        // emuCycles break the non-decreasing-stamp invariant the
        // publication carry loop in advanceCycles() relies on. After a
        // rewind every stale event sat past the new frame start, so
        // publishedEvents_ came out empty on every publication for the
        // whole rewound span (beam-raced effects froze at the frame-start
        // state) while videoEvents_ carried the stale tail forever and
        // grew without bound. Start the log fresh from the restored
        // display state and clock.
        displayAtFrameStart_ = ds;
        publishedFrameStart_ = ds;
        resetVideoEventLogForClockJump();
    }

    if (!need(lcBank1.size() + lcBank2.size() + lcHigh.size())) return false;
    getBytes(lcBank1.data(), lcBank1.size());
    getBytes(lcBank2.data(), lcBank2.size());
    getBytes(lcHigh.data(),  lcHigh.size());

    if (!need(5)) return false;
    const uint32_t savedBanks = getU32();
    const uint8_t  savedBank  = getU8();

    if (savedBanks <= 1) {
        if (!need(aux.size() + auxLcBank1.size() + auxLcBank2.size() + auxLcHigh.size()))
            return false;
        getBytes(aux.data(),        aux.size());
        getBytes(auxLcBank1.data(), auxLcBank1.size());
        getBytes(auxLcBank2.data(), auxLcBank2.size());
        getBytes(auxLcHigh.data(),  auxLcHigh.size());
        if (ramWorksBanks_ > 1) {
            // Live config has RamWorks; mirror the restored data into the
            // current backing slot so a later bank swap doesn't lose it.
            uint8_t* slot = ramWorksBacking_.data()
                          + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
            std::memcpy(slot,           aux.data(),        0x10000);
            std::memcpy(slot + 0x10000, auxLcBank1.data(), 0x1000);
            std::memcpy(slot + 0x11000, auxLcBank2.data(), 0x1000);
            std::memcpy(slot + 0x12000, auxLcHigh.data(),  0x2000);
        }
    } else {
        if (!need(4)) return false;
        const uint32_t backingSize = getU32();
        if (!need(backingSize)) return false;
        if (savedBanks == ramWorksBanks_ && backingSize == ramWorksBacking_.size()) {
            std::memcpy(ramWorksBacking_.data(), data + pos, backingSize);
            ramWorksBank_ = static_cast<uint8_t>(savedBank % ramWorksBanks_);
            const uint8_t* slot = ramWorksBacking_.data()
                                + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
            std::memcpy(aux.data(),        slot,           0x10000);
            std::memcpy(auxLcBank1.data(), slot + 0x10000, 0x1000);
            std::memcpy(auxLcBank2.data(), slot + 0x11000, 0x1000);
            std::memcpy(auxLcHigh.data(),  slot + 0x12000, 0x2000);
        } else if (static_cast<size_t>(savedBank) * kRamWorksBankStride
                       + kRamWorksBankStride <= backingSize) {
            // Bank-count mismatch — best effort: lift just the saved current
            // bank's visible slice into the live aux arrays.
            const uint8_t* slot = data + pos
                                + static_cast<size_t>(savedBank) * kRamWorksBankStride;
            std::memcpy(aux.data(),        slot,           0x10000);
            std::memcpy(auxLcBank1.data(), slot + 0x10000, 0x1000);
            std::memcpy(auxLcBank2.data(), slot + 0x11000, 0x1000);
            std::memcpy(auxLcHigh.data(),  slot + 0x12000, 0x2000);
        }
        pos += backingSize;
    }

    // Optional trailer (see appendSnapshotState): //c on-board SmartPort
    // arming gate. Absent in pre-trailer blobs → keep the live value.
    if (need(1)) iicSmartPortArmed_ = getU8() != 0;

    // Second trailer (see appendSnapshotState): length-prefixed //c-class
    // device sections. Absent in older blobs → live values kept, which is
    // exactly the pre-fix behaviour, so nothing regresses on an old save.
    // Once a section starts, both its framing and device payload must be
    // valid; failure propagates to MachineSnapshot's transactional rollback.
    auto readSection = [&](auto&& apply) -> bool {
        if (pos == n) return true;             // optional trailer absent
        if (!need(4)) return false;            // torn length prefix
        const uint32_t len = getU32();
        if (len == 0) return true;
        if (n - pos < len) return false;
        if (!apply(data + pos, static_cast<size_t>(len))) return false;
        pos += len;
        return true;
    };
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !iwmDevice || iwmDevice->loadSnapshotState(p, k);
        })) return false;
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !iicProfile_ || iicProfile_->loadSnapshotState(p, k) != 0;
        })) return false;
    // Paging/IOU flip-flops (see appendSnapshotState). Older blobs end
    // before this section and keep the live values — the documented
    // back-compat convention for this trailer.
    if (!readSection([&](const uint8_t* p, size_t k) {
            if (k < 4) return false;
            intC8Rom      = p[0] != 0;
            ioudis        = p[1] != 0;
            vblIrqMask    = p[2] != 0;
            vblIrqPending = p[3] != 0;
            // Re-drive the LINE to match the restored latch. A rewind that
            // lands `pending = false` while IRQ_SRC_VBL is still asserted
            // used to wedge the machine permanently: the $C070 ack is
            // gated on the latch, so it would be skipped forever and the
            // //c would spin in its IRQ vector.
            if (iicProfile_ && cpu)
                cpu->setIrqLine(M6502::IRQ_SRC_VBL, vblIrqPending);
            // Annunciators, appended after the four above. A blob written
            // before 2026-09-06 stops at k == 4 and keeps the live values,
            // which is the same back-compat rule the whole trailer follows.
            if (k >= 7) {
                an0 = p[4] != 0;
                an1 = p[5] != 0;
                an2 = p[6] != 0;
            }
            // VBL edge detector + the //c $C800 window latch (see
            // appendSnapshotState). Older blobs stop before these.
            if (k >= 9) {
                vblWasActive   = p[7] != 0;
                iicCardWindow_ = p[8] != 0;
            }
            return true;
        })) return false;
    // No-Slot Clock (see appendSnapshotState). A blob written by a build
    // without it, or by a machine with the chip disabled, ends here.
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !noSlotClock_ || noSlotClock_->loadSnapshotState(p, k);
        })) return false;
    // On-board Sony 3.5" mechanisms (see appendSnapshotState).
    for (int which = 0; which < 2; ++which) {
        if (!readSection([&](const uint8_t* p, size_t k) {
                pom2::Sony35Drive* d = smartPortHub
                    ? (which == 0 ? smartPortHub->internal35()
                                  : smartPortHub->external35())
                    : nullptr;
                return !d || d->loadSnapshotState(p, k);
            })) return false;
    }

    return true;
}

void Memory::restoreMainRam(const uint8_t* data, size_t n)
{
    const size_t lim = (n < mem.size()) ? n : mem.size();
    for (size_t i = 0; i < lim; ++i) {
        // ramWritable(), not writable[]: an address diverted by a write
        // watchpoint reads as non-writable there, and a snapshot or rewind
        // restore would silently skip that one byte for as long as the watch
        // stayed armed.
        if (ramWritable(static_cast<uint16_t>(i))) mem[i] = data[i];
    }
}

uint8_t Memory::softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal)
{
    // Soft-switch byte is in $C000-$C07F. Many switches respond to either
    // a read OR a write (both edges work as toggles). We snapshot the
    // current keyboard latch under kbMutex on every $C000/$C010 access.
    // Only the keyboard addresses ($C000-$C01F: latch, strobe, the //e
    // status reads) consume the latch — every other soft switch in this page
    // (speaker $C030, display $C050-$C05F, paddles $C06x/$C070) used to take
    // the lock too, for nothing. A speaker-driven tune toggles $C030 tens of
    // thousands of times a second, so the uncontended lock/unlock pair was
    // ~5 % of a banner profile on its own.
    const uint8_t low = static_cast<uint8_t>(addr & 0xFF);
    const uint8_t kbLatch =
        (low < 0x20) ? keyboard_.latchMirror() : uint8_t{0};

    // Keyboard latch + IIe paging soft switches at $C000-$C00F.
    //
    // MAME's `apple2.cpp:548` mirrors $C000 across $C001-$C00F via
    // `.mirror(0xf)`, and `apple2e.cpp:1825-1828` does the same with
    // `if((offset & 0xf0) == 0) return m_transchar | m_strobe;`. So on
    // either II+ or IIe, READS of $C000-$C00F return the keyboard
    // latch — they do NOT toggle the IIe paging soft switches. WRITES
    // to $C001-$C00F dispatch to the IIe handler (writes-only on real
    // hardware).
    //
    // BUT $C00C/$C00D drive the Le Chat Mauve / Video-7 RGB FIFO data
    // line — and those cards sniff the bus regardless of read/write
    // direction. On a IIe the broadcast is folded into
    // `iieHandleSoftSwitch`; on a II+ we run the broadcast directly
    // here because there's no IIe handler to do it for us.
    if (low <= 0x0F) {
        if (!iieMode && (low == 0x0C || low == 0x0D)) {
            {
                std::lock_guard<std::mutex> lk(stateMutex);
                display.eightyCol = (low == 0x0D);
            }
            recordVideoEvent(VideoEventKind::EightyCol, low == 0x0D);
            slots.broadcastVideoSwitch(addr);
        }
        if (isWrite && iieMode) {
            iieHandleSoftSwitch(addr);
        }
        return kbLatch;
    }
    // Keyboard strobe clear.
    //
    // II/II+: $C010 is mirrored across the whole $C010-$C01F page — MAME
    // `apple2.cpp:548` `map(0xc010,0xc010).mirror(0xf)` routes both reads
    // and writes of any $C01x to the strobe clear. The II decodes only
    // A4-A7 here; there are no IIe status registers to shadow. (POM2
    // previously answered $C011/$C012 with IIe-style LC status on II+ —
    // a deliberate convenience that diverged from hardware; software
    // acking a key via `STA $C01x` (x≠0) never saw its strobe clear.)
    if (!iieMode && low >= 0x10 && low <= 0x1F) {
        clearKeyStrobe();
        return kbLatch & 0x7F;
    }
    // IIe: any WRITE in $C010-$C01F clears the strobe (MAME `apple2e.cpp`
    // c000_w: `if ((offset & 0xf0) == 0x10) { m_strobe = 0; ... }` — only
    // $C010 additionally pings the keyboard MCU). Reads of $C011-$C01F
    // are status-only and fall through to the handlers below.
    if (iieMode && isWrite && low >= 0x10 && low <= 0x1F) {
        clearKeyStrobe();
        return kbLatch & 0x7F;   // store: return value unused
    }
    // IIe keyboard strobe read — clear + AKD approximation.
    if (low == 0x10) {
        // IIe: bit 7 reflects "any key down" (MAME `apple2e.cpp:1833`:
        // `m_transchar | (m_anykeydown ? 0x80 : 0x00)`). POM2 doesn't
        // model key-release events separately, so we approximate
        // "any-key-down" with the pre-clear strobe state — that's what
        // software typically polls $C010 for ("is the user still
        // holding a key?"). On II+ the strobe-clear semantic is
        // historical: bit 7 LOW after clear.
        // Reuse the bit captured under kbMutex into kbLatch above (bit 7 ==
        // keyReady) rather than re-reading the keyReady member here unlocked —
        // a bare read races the UI/HTTP threads that write it under kbMutex.
        const bool wasReady = (kbLatch & 0x80) != 0;
        clearKeyStrobe();
        if (iieMode) {
            return static_cast<uint8_t>(
                (kbLatch & 0x7F) | (wasReady ? 0x80 : 0x00));
        }
        return kbLatch & 0x7F;
    }
    // IIe Language Card status reads RDBNK2/RDLCRAM (only reachable in
    // iieMode: II+ $C01x is the strobe mirror handled above). Low 7 bits
    // carry m_transchar (last keyboard char) per MAME
    // `apple2e.cpp:1842-1871`.
    if (low == 0x11 || low == 0x12) {
        const bool on = (low == 0x11) ? lcBank2Active : lcReadRam;
        uint8_t low7 = 0;
        if (iieMode) {
            low7 = keyboard_.lastKey7();
        }
        return static_cast<uint8_t>((on ? 0x80 : 0x00) | low7);
    }

    // (The IIe paging dispatch for $C001-$C00F now lives inside the
    // `low <= 0x0F` block above — gated on `isWrite` so reads of those
    // addresses don't accidentally flip paging bits.)

    // IIe status reads at $C013-$C018 + $C01E-$C01F (high bit reflects
    // the matching MF_* / DisplayState bit). Out-of-band "handled" flag:
    // an in-band sentinel byte is unusable here because every status
    // value `0x80 | transchar` is legitimate — with lastKey = $7E ('~')
    // an ON switch reads back exactly $FE, and a sentinel collision sent
    // RDRAMRD-class polls to the floating bus (reporting OFF while ON).
    if (iieMode && low >= 0x13 && low <= 0x1F) {
        uint8_t s = 0;
        if (iieReadStatus(addr, s)) return s;
    }
    // VBL (vertical blank) strobe — IIe-only register. Scanline-accurate
    // Apple II frame: 262 scanlines × 65 cycles. Visible video =
    // 0..191, VBL = 192..261. Bit 7 of $C019 reflects the active-video
    // state per MAME `apple2e.cpp:2107` convention: HIGH during active,
    // LOW during VBL. II/II+ doesn't decode $C019 at all — the address
    // falls through to the floating bus per MAME `apple2.cpp` (no
    // $C019 case in `do_io`). Without the iieMode gate, software
    // running on II+ that probes $C019 (e.g. some ProDOS detection
    // routines) would see a deterministic scanline-derived value
    // instead of the random-ish video DMA byte real hardware returns.
    //
    // **$C019 read does NOT clear the VBL IRQ** — per Apple IIc
    // Technical Note #9 (and MAME `apple2e.cpp:2244` comment "does not
    // reset"). The clear path is the $C05A AN1 write (IIc/IIc+; POM2
    // overlays the same on IIe as documented in the $C058-$C05D block
    // below). Earlier POM2 versions cleared the IRQ on every $C019
    // read, contradicting Tech Note #9.
    if (iieMode && low == 0x19) {
        uint8_t low7 = 0;
        {
            low7 = keyboard_.lastKey7();
        }
        // //c-class: $C019 is VBLINT — the LATCHED "a VBL interrupt
        // occurred" flag, not the live beam state. MAME `c000_iic_r`
        // case 0x19 (apple2e.cpp:2256-2257): `(m_irqmask & (1<<IRQ_VBL))
        // ? 0x80 : 0` with the comment "does not reset, see Apple IIc
        // Technical Note #9". POM2 already maintains exactly that latch
        // (vblIrqPending) but returned the IIe beam state on every
        // iieMode machine, so //c software polling for its VBL interrupt
        // saw a free-running beam signal instead.
        if (iicProfile_) {
            return static_cast<uint8_t>((vblIrqPending ? 0x80 : 0x00) | low7);
        }
        // IIe: live VBLBAR beam state (MAME apple2e.cpp:2092-2093),
        // OR m_transchar into the low 7 bits.
        constexpr uint64_t kCyclesPerScanline = 65;
        const uint64_t kScanlinesPerFrame =
            static_cast<uint64_t>(pom2VideoTiming(videoStandard_.load()).scanlinesPerFrame);
        constexpr uint64_t kVisibleScanlines  = 192;
        // Sample at the ACTUAL data-fetch cycle: cycleCounter only
        // advances at end-of-instruction, so a bare read placed the beam
        // up to 7 cycles early and could report the wrong side of a
        // scanline/VBL boundary. floatingBus() and pushVideoEventLocked
        // already add the in-flight instruction progress — this is the
        // same stamp, and it matters for beam-racing code that polls
        // $C019 to find the VBL edge.
        const uint64_t now = cycleCounter +
            (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0);
        // NO intra-line phase shift is applied here, and that is a
        // measured conclusion rather than an omission.
        //
        // French Touch's MAD EFFECT (//e PAL + Mockingboard, GPLv3 sources
        // in disks_5.4/demo/madef/) syncs its whole frame off this edge and
        // its `Sources/main.a` says "DISPLAY detected from cycle #52 of last
        // line (#311) of VBL", which reads like a 13-cycle lead. Two
        // different anchorings of that sentence into POM2's beam coordinates
        // were implemented and BOTH were falsified by replaying the real
        // disk: a +13 lead and a -12 lead each moved the demo's 192
        // per-scanline lit-run starts FURTHER outside the visible window,
        // and the -12 variant also broke `pal_timing` / `vbl_smoke` (which
        // require line 192 to read VBL from its first cycle).
        //
        // The sentence pins a relation, not a position: it cannot say where
        // the demo's "cycle 0" sits relative to POM2's line boundary — one
        // equation, two unknowns. Sweeping all 65 phases against the real
        // disk shows the lit-run starts land wholly inside the 40-column
        // window for offsets 21..24 with NO shift here, i.e. within 1-4
        // cycles of `frameCycleToPos`'s existing 25. Any shift on this edge
        // moves it away. Pinned by `vbl_edge_phase`; see CHANGELOG.
        const uint64_t scanline = (now / kCyclesPerScanline) % kScanlinesPerFrame;
        const bool nowActive = scanline < kVisibleScanlines;
        return static_cast<uint8_t>((nowActive ? 0x80 : 0x00) | low7);
    }

    // Annunciators AN0 ($C058/9), AN1 ($C05A/B), AN2 ($C05C/D). Each
    // pair toggles a dedicated output line on the game I/O connector
    // (MAME `apple2e.cpp:1750-1773`). POM2 doesn't wire those lines
    // anywhere yet, but software still expects the soft switch to
    // *swallow* the access (return zero / floating bus, no side
    // effects on display). The IIe-specific note: MAME treats $C05A/B
    // as plain AN1 toggles on IIe — VBL IRQ masking lives on IIc/IIc+
    // only (`apple2e.cpp:2057-2065` `lower_irq` is `m_isiic ||
    // m_isace500`). POM2 historically wired the mask in IIe mode for
    // convenience (and `tests/vbl_smoke_test.cpp` pins it that way);
    // we keep that behaviour AS AN OVERLAY on top of the AN1 state
    // tracking so a future IIc port can drop the overlay without
    // disturbing the AN-state model.
    // //c-class with IOUDIS clear: $C058-$C05F are the IIc mouse/VBL
    // switches, NOT annunciators and NOT DHGR. MAME `apple2e.cpp do_io`
    // (the `(m_isiic || m_isace500) && !m_ioudis` branch, ~:1807-1852):
    // 58/59 DisXY/EnbXY, 5A/5B DisVBL/EnVBL, 5C-5F X0/Y0 edge selects —
    // and it RETURNS without touching AN0-AN3 or an3_w (DHGR). POM2
    // tracked `ioudis` (writable at $C07E/F, readable at $C07E) but never
    // gated anything on it, so a //c guest driving the mouse firmware's
    // switch protocol wrongly flipped AN3/DHGR and the display.
    if (low >= 0x58 && low <= 0x5F && iicProfile_ && !ioudis) {
        if (low == 0x5A) {              // DisVBL
            vblIrqMask = false;
            if (vblIrqPending) {
                vblIrqPending = false;
                if (cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, false);
            }
        } else if (low == 0x5B) {       // EnVBL
            vblIrqMask = true;
        }
        // DisXY/EnbXY and the X0/Y0 edge selects have no POM2 mouse-model
        // consumer yet (the MouseCard keeps its own state machine); the
        // access is swallowed exactly like MAME's tracked-bool cases.
        return isWrite ? 0 : floatingBus();
    }

    if (low >= 0x58 && low <= 0x5D) {
        const bool on = (low & 1) != 0;
        switch ((low - 0x58) >> 1) {
            case 0: an0 = on; break;
            case 1: an1 = on; break;
            case 2: an2 = on; break;
        }
        if (iieMode && !iicProfile_ && (low == 0x5A || low == 0x5B)) {
            // POM2 overlay: $C05A/B doubles as VBL IRQ mask in IIe.
            // Strictly speaking that's an IIc/IIc+ feature in MAME;
            // we keep it here so existing software that relies on the
            // overlay (and `vbl_smoke_test.cpp`) keeps working.
            //
            // //c-class is EXCLUDED (`!iicProfile_`): there the VBL mask
            // is reachable only through the real IOU decode above. MAME
            // `apple2e.cpp:1808-1876` splits the same way — the
            // `(m_isiic || m_isace500) && !m_ioudis` branch (:1811) owns
            // DisVBL/EnVBL (:1823-1830) and `return`s (:1848); the else
            // branch is commented "IIe does not have IOUDIS" (:1851),
            // handles only SETDHIRES/CLRDHIRES and falls through to the
            // plain AN0/AN1/AN2 cases (:1975-1983), which touch `m_an1`
            // and the gameio pin and NOTHING else. MAME quotes the IIc
            // Technical Reference in that fall-through (:1867-1870): "if
            // the IOUDis switch is on, both reading from and writing to
            // addresses C058 through C05D are reserved".
            //
            // IOUDIS resets to TRUE (`resetSoftSwitches`, MAME
            // `apple2e.cpp:1234`), so without this gate the reset default
            // on a //c sent a plain `LDA $C05B` (the legacy AN1 idiom)
            // into the overlay, arming vblIrqMask — and unlike IIe, the
            // //c-class edge in advanceCycles DOES drive the CPU IRQ
            // line, so the guest took an unhandled 50/60 Hz IRQ storm
            // through $FFFE. The mirror case was as bad: `LDA $C05A`
            // silently ACKed a VBL interrupt the guest had legitimately
            // armed with IOUDIS clear, costing a //c PAL demo its frame
            // sync.
            if (low == 0x5A) {
                vblIrqMask = false;
                if (vblIrqPending) {
                    vblIrqPending = false;
                    if (cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, false);
                }
            } else {
                vblIrqMask = true;
            }
        }
        // Annunciators don't drive the data bus: a READ returns the floating
        // bus (video scanner byte), like the paddle/catch-all paths below —
        // not a hard 0. RNG / copy-protection code samples these expecting
        // non-deterministic low bits.
        return isWrite ? 0 : floatingBus();
    }

    // $C040 utility strobe — MAME `apple2e.cpp:1711-1716` pulses the
    // game I/O connector's STRB pin (high → low → high) on every access.
    // No POM2 peripheral consumes the strobe, but the address is UNDRIVEN,
    // so a read returns the floating bus like every other undriven $C0xx —
    // matching real hardware (a vapor-lock loop polling $C040 must see the
    // scanner byte, not a hard 0). Writes don't drive the bus.
    if (low == 0x40) return isWrite ? 0 : floatingBus();

    // Display soft switches. They don't drive the data bus either: a READ
    // flips the mode AND returns the floating bus (video scanner byte) —
    // MAME `apple2.cpp do_io` returns `read_floatingbus()` for $C050-$C057.
    // DROL's cut-scene depends on this: it vapor-locks with
    //     LDX #$02 / LDA $C050 / CMP #$80 / BNE / DEX / BPL
    // (three consecutive scanner reads of $80) — a hard 0 here spins it
    // forever (the same hang LinApple had; AppleWin fixed it in 1.13.0 by
    // implementing the floating bus). NB: floatingBus() takes stateMutex,
    // so the switch work is scoped before the return.
    if (low >= 0x50 && low <= 0x57) {
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            switch (low) {
                case 0x50: display.textMode  = false; break;
                case 0x51: display.textMode  = true;  break;
                case 0x52: display.mixedMode = false; break;
                case 0x53: display.mixedMode = true;  break;
                case 0x54: display.page2     = false; break;
                case 0x55: display.page2     = true;  break;
                case 0x56: display.hiRes     = false; break;
                case 0x57: display.hiRes     = true;  break;
            }
            switch (low) {
                case 0x50: pushVideoEventLocked(VideoEventKind::TextMode,  false); break;
                case 0x51: pushVideoEventLocked(VideoEventKind::TextMode,  true);  break;
                case 0x52: pushVideoEventLocked(VideoEventKind::MixedMode, false); break;
                case 0x53: pushVideoEventLocked(VideoEventKind::MixedMode, true);  break;
                case 0x54: pushVideoEventLocked(VideoEventKind::Page2,     false); break;
                case 0x55: pushVideoEventLocked(VideoEventKind::Page2,     true);  break;
                case 0x56: pushVideoEventLocked(VideoEventKind::HiRes,      false); break;
                case 0x57: pushVideoEventLocked(VideoEventKind::HiRes,      true);  break;
            }
            if (iieRebootTraceEnabled()) {
                static const char* dnames[8] = {
                    "TEXT=off(gfx)", "TEXT=on", "MIXED=off", "MIXED=on",
                    "PAGE2=off",     "PAGE2=on","HIRES=off(lo)","HIRES=on"
                };
                std::ostringstream oss;
                oss << std::hex << std::uppercase << std::setfill('0');
                oss << "display $" << std::setw(4) << static_cast<int>(addr)
                    << " " << dnames[low - 0x50]
                    << " text=" << display.textMode
                    << " mixed=" << display.mixedMode
                    << " page2=" << display.page2
                    << " hires=" << display.hiRes
                    << " cyc=" << std::dec << cycleCounter;
                pom2::log().warn("IIE", oss.str());
            }
        }
        return isWrite ? 0 : floatingBus();
    }

    // (The II/II+ 80COL / Le Chat Mauve FIFO hook for $C00C/$C00D lives
    // in the `low <= 0x0F` block at the top of this function — that block
    // returns unconditionally, so no $C00x can reach this point.)

    // AN3 annunciator ($C05E off / $C05F on). Used as the FIFO clock by
    // Le Chat Mauve — every $C05E→$C05F rising edge pushes the current
    // 80COL bit into the card's mode register.
    //
    // On a IIe the same two addresses are DHIRESON ($C05E) / DHIRESOFF
    // ($C05F): they enable / disable double hi-res mode. The polarity is
    // OPPOSITE the AN3 line (AN3 high ↔ DHGR off), so we track DHGR as a
    // separate bit instead of inverting at the read site. The Le Chat
    // Mauve hook below still fires on every access so its FIFO continues
    // to clock on a IIe with the card plugged.
    if (low == 0x5E || low == 0x5F) {
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            display.an3 = (low == 0x5F);
            if (iieMode) display.dhgr = (low == 0x5E);
            pushVideoEventLocked(VideoEventKind::An3, low == 0x5F);
            if (iieMode) pushVideoEventLocked(VideoEventKind::Dhgr, low == 0x5E);
        }
        slots.broadcastVideoSwitch(addr);
        return isWrite ? 0 : floatingBus();
    }

    // Speaker click — toggles on every access ($C030-$C03F all alias).
    // The audio path uses cycleCounter + the CPU's current-instruction
    // cycle progress for sub-instruction timestamps; without this every
    // toggle inside a frame collapses to one cycle and the audio aliases.
    if (low >= 0x30 && low <= 0x3F) {
        speakerToggles.fetch_add(1, std::memory_order_relaxed);
        if (speakerCb) speakerCb(speakerUser);
        if (speaker) {
            const uint64_t now = cycleCounter +
                (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0);
            speaker->recordToggle(now);
        }
        // The speaker latch doesn't drive the bus — a READ clicks AND
        // returns the floating bus (same rule as $C040/$C05x).
        return isWrite ? 0 : floatingBus();
    }

    // Apple //c ROMBANK ($C020-$C02F): on every //c-class machine the
    // $C02x range toggles `iicRomBank` (matches MAME `apple2e.cpp:
    // 1907-1923` `if (m_isiic) m_romswitch = !m_romswitch`). Gating on
    // `isIIcClass` (not `iicHasAltBank`) keeps 16 KB rev-255 //c users
    // out of the cassette-toggle path — the //c has no cassette port.
    // On 16 KB dumps the alt-firmware read paths stay inert because
    // `iicHasAltBank` remains false, so the toggle is cosmetic but
    // MAME-faithful. The Apple IIc Tech Ref 2e lists the softswitch
    // at "$c02x" range, not just $C028. Both reads and writes trigger.
    if (low >= 0x20 && low <= 0x2F) {
        // //c-class: $C02x toggles ROMBANK — the profile flips its alt-
        // firmware bank and resets MIG state on the →bank-0 edge (MAME
        // `apple2e.cpp:1907-1923`). On II/II+/IIe it's the cassette
        // OUTPUT toggle: the Monitor WRITE routine ($FECD) loops on
        // BIT $C020 to drive the head with 770 Hz / 1 kHz / 2 kHz square
        // waves encoding sync, ones and zeroes.
        // $C02x doesn't drive the data bus on any machine (cassette OUT and
        // ROMBANK are toggles, not registers): a READ returns the floating
        // bus like $C040/$C050-$C05F above, not a hard 0 — RNG /
        // copy-protection entropy loops sample this range.
        if (iicProfile_ && iicProfile_->romBankToggle())
            return isWrite ? 0 : floatingBus();
        if (cassette) cassette->toggleOutput();
        return isWrite ? 0 : floatingBus();
    }

    // Cassette INPUT ($C060 only): bit-7 = sign of the audio comparator.
    // The Monitor's READ routine ($FEFD) tight-loops on $C060 measuring
    // sign-flip durations to recover bits from the tape. Note: $C061-$C067
    // are NOT cassette aliases on the II/II+ — they're the paddle buttons
    // and paddle inputs, handled below.
    if (low == 0x60) {
        if (cassette) return cassette->readTapeInput();
        return 0;
    }

    // Cassette input + push-buttons + paddle inputs at $C060-$C067,
    // mirrored across $C068-$C06F (MAME `apple2.cpp:554` `.mirror(0x8)`
    // and `apple2e.cpp:1889/1903/1909/1915/1919/1923/1927`) — $C068
    // reads the cassette comparator just like $C060. Real hardware ORs
    // the floating-bus byte into the low 7 bits (Beagle Bros and
    // demoscene RNGs depend on this). The IIgs STATEREG at $C068 stays
    // unexposed — POM2 is II/II+/IIe-class only.
    if (low >= 0x61 && low <= 0x6F) {
        const uint8_t mirrored = static_cast<uint8_t>(0x60 | (low & 0x07));
        const uint8_t bit7 = [&]() -> uint8_t {
            switch (mirrored) {
                case 0x61: return paddles_.button0() ? 0x80 : 0x00;
                case 0x62: return paddles_.button1() ? 0x80 : 0x00;
                case 0x63: return paddles_.button2(iieMode) ? 0x80 : 0x00;
                case 0x64: case 0x65: case 0x66: case 0x67: {
                    const int idx = mirrored - 0x64;
                    return paddles_.discharging(idx, cycleCounter) ? 0x80 : 0x00;
                }
                case 0x60:
                    // $C068 mirrors $C060 (cassette comparator) per the
                    // same `.mirror(0x8)` cited above — only a literal
                    // $C060 access takes the dedicated branch earlier.
                    // Returning a hard 0 here clamped bit 7 low, so a
                    // tape-read loop polling $C068 never saw the
                    // comparator flip and entropy loops keyed on N were
                    // deterministic.
                    return cassette
                        ? static_cast<uint8_t>(cassette->readTapeInput() & 0x80)
                        : uint8_t{0};
                default: return 0;  // unreachable: mirrored ∈ $60-$67
            }
        }();
        return static_cast<uint8_t>(bit7 | (floatingBus() & 0x7F));
    }

    // Paddle-trigger reset ($C070-$C07F mirrored). MAME `apple2.cpp:555`
    // `.mirror(0xf)`; the read returns floating bus (used as a poor
    // RNG seed by many games). Real silicon also implements a 558
    // monostable one-shot semantic (re-strobing during the count
    // doesn't restart the timer); POM2 reloads unconditionally — the
    // simpler model passes every game we've tested.
    //
    // RamWorks III (IIe aux-slot card) sniffs writes to $C071/3/5/7
    // on the same address window. MAME `a2eramworks3.cpp:108-115`
    // predicate `(offset & 0x9) == 1` over the low nibble selects
    // those four addresses; the data byte's low 7 bits (`data & 0x7F`,
    // line 113) is the new bank index. Bank switch and paddle reset
    // both fire on the same access — they share the bus, neither
    // shadows the other.
    if (low >= 0x70 && low <= 0x7F) {
        // An accelerator (TransWarp) lives entirely in this window: $C070
        // is its joystick-slowdown trigger, $C072 releases its ROM shadow
        // and $C074 is its speed register. $C074 is the one address it
        // takes OFF the bus — MAME `transwarp.cpp dma_w` returns there —
        // so a consumed access skips the paddle rearm and everything below
        // it. Null on any machine without one.
        if (SlotPeripheral* snoop = slots.busSnooper()) {
            if (snoop->busSnoop(addr, isWrite, isWrite ? writeVal : 0))
                return isWrite ? 0 : floatingBus();
        }
        paddles_.rearm(cycleCounter);
        // //c-class: ANY $C070-$C07F access acknowledges the VBL
        // interrupt — MAME `apple2e.cpp` c000_iic_r/w case 0x70-0x7F:
        // `if (m_isiic ...) lower_irq(IRQ_VBL);` (~:2014-2017). Pairs
        // with the latched $C019 VBLINT read above: poll $C019, then
        // strobe $C070 to re-arm for the next frame (Tech Note #9).
        // NOT gated on vblIrqPending: MAME's lower_irq is unconditional,
        // and gating it meant any state where the line was asserted while
        // the latch read false (a restored snapshot, a reset race) could
        // never be acknowledged — the //c would spin in its IRQ vector.
        if (iicProfile_) {
            vblIrqPending = false;
            if (cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, false);
        }
        if (isWrite && iieMode && ramWorksBanks_ > 1
            && (low & 0x09) == 0x01) {
            ramWorksSwapToBank(static_cast<uint8_t>(writeVal & 0x7F));
        }
        // IOUDIS SET/CLR — MAME `apple2e.cpp:2569-2587`. Only IIc-class
        // honours the write; IIe falls through (the softswitch exists
        // but is read-only). $C07E = SET (ioudis=true), $C07F = CLR
        // (ioudis=false). $C078/$C079 are //c mouse firmware mirrors
        // of the same SET/CLR pair. The paddle-latch side-effect above
        // still fires; the IOUDIS toggle is a parallel decode.
        if (isWrite && iicProfile_ && low >= 0x78) {
            // MAME apple2e.cpp: on //c EVERY even $C078/A/C/E is SETIOUDIS
            // and every odd $C079/B/D/F is CLRIOUDIS (not just $C078/E).
            ioudis = !(low & 1);
        }
        // $C07E read returns bit 7 = ioudis state (MAME `:2276-2278`).
        // Shared by IIe/IIc/IIc+. Other $C07x reads keep returning
        // floating bus.
        if (!isWrite && iieMode && low == 0x7E) {
            return ioudis ? 0x80 : 0x00;
        }
        return isWrite ? 0 : floatingBus();
    }

    // Unknown soft switch — Apple II hardware floats the bus. For reads,
    // return whatever the video DMA is currently fetching (matches real
    // hardware so software that uses $C0xx as a poor RNG, or that BMI/BPL
    // on a status read's low 7 bits, behaves correctly). Writes don't
    // care about the return value.
    return isWrite ? 0 : floatingBus();
}

void Memory::iieHandleSoftSwitch(uint16_t addr)
{
    // $C000-$C00F: 80STORE / RAMRD / RAMWRT / INTCXROM / ALTZP / SLOTC3ROM
    // / 80COL / ALTCHAR. Even byte = OFF (clear bit), odd byte = ON.
    const uint8_t low = static_cast<uint8_t>(addr & 0x0F);
    const bool   on  = (low & 1) != 0;
    uint16_t flag = 0;
    switch (low >> 1) {
        case 0: flag = MF_80STORE;   break;
        case 1: flag = MF_RAMRD;     break;
        case 2: flag = MF_RAMWRT;    break;
        case 3: flag = MF_INTCXROM;  break;
        case 4: flag = MF_ALTZP;     break;
        case 5: flag = MF_SLOTC3ROM; break;
        case 6: flag = MF_80COL;     break;
        case 7: flag = MF_ALTCHAR;   break;
    }
    if (on) iieMemMode |= flag;
    else    iieMemMode &= static_cast<uint16_t>(~flag);

    if (bankTrace_ && flag == MF_ALTZP) {
        std::fprintf(stderr, "[ALTZP] %s via $%04X  PC=$%04X cyc=%llu\n",
                     on ? "ON " : "OFF", static_cast<unsigned>(addr),
                     cpu ? static_cast<unsigned>(cpu->getProgramCounter()) : 0u,
                     static_cast<unsigned long long>(cycleCounter));
    }

    if (iieRebootTraceEnabled()) {
        static const char* names[8] = {
            "80STORE", "RAMRD", "RAMWRT", "INTCXROM",
            "ALTZP",   "SLOTC3ROM", "80COL",  "ALTCHAR"
        };
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        oss << "IIe paging $" << std::setw(4) << static_cast<int>(addr)
            << " " << names[low >> 1] << "=" << (on ? "ON" : "OFF")
            << " mode=" << std::setw(4) << static_cast<int>(iieMemMode)
            << " cyc=" << std::dec << cycleCounter;
        pom2::log().warn("IIE", oss.str());
    }

    // Mirror display-relevant bits into DisplayState so Apple2Display can
    // pick them up via its single getDisplayState() snapshot per frame.
    if (flag == MF_80STORE || flag == MF_80COL || flag == MF_ALTCHAR) {
        std::lock_guard<std::mutex> lk(stateMutex);
        display.eightyStore = (iieMemMode & MF_80STORE) != 0;
        display.eightyCol   = (iieMemMode & MF_80COL)   != 0;
        display.altChar     = (iieMemMode & MF_ALTCHAR) != 0;
        if (flag == MF_80COL)
            pushVideoEventLocked(VideoEventKind::EightyCol, on);
        if (flag == MF_80STORE)
            pushVideoEventLocked(VideoEventKind::EightyStore, on);
        if (flag == MF_ALTCHAR)
            pushVideoEventLocked(VideoEventKind::AltChar, on);
    }
    // Le Chat Mauve / Video-7 RGB FIFO clocking — for the 80COL pair
    // ($C00C/D) we need to forward the data-bit edge to plugged video
    // cards even in IIe mode. AN3 ($C05E/F) takes a separate path
    // outside this handler. Without this broadcast, software that uses
    // 80COL toggles on a IIe to drive the Le Chat Mauve mode FIFO
    // would silently stop clocking.
    if (flag == MF_80COL) {
        slots.broadcastVideoSwitch(addr);
    }
}

bool Memory::iieReadStatus(uint16_t addr, uint8_t& out) const
{
    const uint8_t low = static_cast<uint8_t>(addr & 0xFF);
    DisplayState ds;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        ds = display;
    }
    // MAME `apple2e.cpp:1842-1871` `c000_r`: every status read in
    // $C011-$C01F returns `(bit ? 0x80 : 0x00) | m_transchar`. The low
    // 7 bits carry the last latched keyboard character — software like
    // Beagle Bros' Pro-Byter, Print Shop, and the IIe Self-Test rely on
    // this to read "key + status flag" in one byte. POM2's
    // `lastKey` is the same latch as MAME's m_transchar.
    uint8_t transchar = 0;
    {
        transchar = keyboard_.lastKey7();
    }
    auto bit = [transchar](bool on) -> uint8_t {
        return static_cast<uint8_t>((on ? 0x80 : 0x00) | transchar);
    };
    switch (low) {
        case 0x13: out = bit((iieMemMode & MF_RAMRD)     != 0); return true; // RDRAMRD
        case 0x14: out = bit((iieMemMode & MF_RAMWRT)    != 0); return true; // RDRAMWRT
        case 0x15: out = bit((iieMemMode & MF_INTCXROM)  != 0); return true; // RDCXROM
        case 0x16: out = bit((iieMemMode & MF_ALTZP)     != 0); return true; // RDALTZP
        case 0x17: out = bit((iieMemMode & MF_SLOTC3ROM) != 0); return true; // RDC3ROM
        case 0x18: out = bit((iieMemMode & MF_80STORE)   != 0); return true; // RD80STORE
        case 0x1A: out = bit(ds.textMode);                      return true; // RDTEXT
        case 0x1B: out = bit(ds.mixedMode);                     return true; // RDMIXED
        case 0x1C: out = bit(ds.page2);                         return true; // RDPAGE2
        case 0x1D: out = bit(ds.hiRes);                         return true; // RDHIRES
        case 0x1E: out = bit((iieMemMode & MF_ALTCHAR)   != 0); return true; // RDALTCHAR
        case 0x1F: out = bit((iieMemMode & MF_80COL)     != 0); return true; // RD80COL
    }
    return false;  // not a status read ($C019 VBL etc.) — caller continues
}

uint8_t Memory::iieMemRead(uint16_t addr)
{
    // Routing rules and the mutex rationale both live on iieReadFromAux()
    // in the header — it is the single definition of the aux/main decision,
    // shared with memRead()'s inline fast path.
    const bool fromAux = iieReadFromAux(addr);
    const uint8_t v = fromAux ? aux[addr] : mem[addr];
    if (bankTrace_) checkBankRead(addr, fromAux, v);
    return v;
}

void Memory::iieMemWrite(uint16_t addr, uint8_t value)
{
    const bool toAux = iieWriteToAux(addr);
    if (toAux) aux[addr] = value;
    else       mem[addr] = value;
    if (bankTrace_) {
        noteBankWrite(addr, toAux, value);
        // Trace writes to the $0080-$00AB zero-page trampoline region (the
        // routine the Nox freeze jumps to) — shows which bank (main/aux) the
        // game copies it into vs the ALTZP state when later executed.
        if (addr >= 0x0080 && addr <= 0x00AB)
            std::fprintf(stderr,
                "[ZP] W $%04X=%02X -> %s (ALTZP=%d) cyc=%llu\n",
                addr, value, toAux ? "AUX" : "MAIN",
                (iieMemMode & MF_ALTZP) ? 1 : 0,
                static_cast<unsigned long long>(cycleCounter));
    }
}

uint8_t Memory::languageCardSwitchAccess(uint16_t addr, bool isWrite)
{
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);

    // $C080-$C087 select bank 2, $C088-$C08F select bank 1. Within each
    // half, the low two bits choose ROM/RAM read mode and whether the
    // prewrite latch is armed. $C084-$C087 mirror $C080-$C083.
    //
    // Write-enable is STICKY (MAME apple2e.cpp:1506-1564 `lc_update`):
    //   - any EVEN access ($C08{0,2,4,6,8,A,C,E}) clears prewrite AND
    //     write-enable;
    //   - any WRITE clears prewrite only — write-enable is left UNCHANGED
    //     (so flipping the bank with `STA $C08x` mid-write keeps writes on);
    //   - the first odd READ arms prewrite; a second consecutive odd READ
    //     commits write-enable.
    // The previous formula (`writeEnable = odd && prevPrewrite`, recomputed
    // every access) diverged from this — it dropped/re-armed write-enable on
    // repeated odd writes/reads, so a game that streams data into Language-
    // Card RAM while toggling banks (Nox Archaist's city decompressor, into
    // aux LC at $D000) had its LC writes silently dropped → corrupt $D000
    // code → crash. Pin: tests/iie_langcard_writeenable_smoke_test.cpp.
    if ((low4 & 1) == 0) {            // even access: disable prewrite + writing
        lcPrewrite    = false;
        lcWriteEnable = false;
    }
    if (isWrite) {                    // any write disables prewrite (WE unchanged)
        lcPrewrite = false;
    } else if ((low4 & 1) == 1) {     // odd read: arm, then commit
        if (!lcPrewrite) lcPrewrite = true;
        else             lcWriteEnable = true;
    }

    const uint8_t mode = low4 & 0x03;
    lcReadRam     = (mode == 0x00 || mode == 0x03);   // 0/3 = RAM, 1/2 = ROM
    lcBank2Active = (low4 & 0x08) == 0;                // bank2 when !(offset&8)

    // The card itself does not drive the data lines for $C08x — the byte
    // the CPU reads is whatever the video DMA last latched onto the bus.
    return floatingBus();
}

uint8_t Memory::floatingBus() const
{
    // CPU read path: the data fetch happens at the in-flight instruction's
    // access cycle (last cycle for LDA/CMP/BIT $C0xx), so sample the scanner
    // there — consistent with pushVideoEventLocked's timestamp.
    return floatingBus(cycleCounter +
        (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0));
}

uint8_t Memory::floatingBus(uint64_t absCycle) const
{
    // Verbatim port of MAME `apple2video.cpp:124-201 scanner_address`.
    // Input: h_clock [0..64] (active video from 25), v_clock [0..261]
    // (active video from 0). Output: 16-bit DRAM address the video
    // scanner is currently fetching. Used by reads of unimplemented
    // soft switches ($C040, $C050-$C05F mirrors during VBL, $C019 in
    // II/II+, etc.) which let the floating bus byte through. Software
    // using this as an RNG seed (Beagle Bros copy protection, some
    // demos) needs bit-exact replication of the scanner counter.
    //
    // The earlier POM2 implementation built the address from a
    // text/HGR row-interleave formula; that gave the same byte during
    // active video but diverged during HBL (where MAME's `addend0=0x0D
    // + h-carries` produces the "$1000 phantom row" effect) and on
    // page-2 HGR (m_hgr2 base differs).
    // Scanner geometry follows the active video standard: 262 lines NTSC,
    // 312 lines PAL (same 65-cycle line). A vapor-locking demo polls this
    // until the beam reaches its marker, so under PAL the scanner MUST sweep
    // a 312-line / 20280-cycle frame — a hardcoded 262 would make the lock
    // recur every 17030 cycles instead, drifting the per-frame sync of French
    // Touch / DIX. (cyclesPerScanline=65 both; only the line count differs.)
    constexpr uint64_t kCyclesPerLine  = 65;
    const uint64_t kLinesPerFrame =
        static_cast<uint64_t>(pom2VideoTiming(videoStandard_.load()).scanlinesPerFrame);
    const uint64_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;

    const uint64_t cyc     = absCycle % kCyclesPerFrame;
    const int      v_clock = static_cast<int>(cyc / kCyclesPerLine);  // 0..311
    const int      h_clock = static_cast<int>(cyc % kCyclesPerLine);  // 0..64

    DisplayState ds;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        ds = display;
    }
    int Hires = (ds.hiRes && !ds.textMode) ? 1 : 0;
    const int Mixed = ds.mixedMode ? 1 : 0;
    // The video scanner honours PAGE2 only when 80STORE is off. With
    // 80STORE on, PAGE2 redirects aux-bank selection rather than the
    // displayed page, so the scanner — and therefore the floating bus —
    // always reads page 1. MAME apple2video.cpp use_page_2() = m_page2 &&
    // !m_80store. Uses iieMemMode (same source as the iieMemRead routing)
    // so II/II+ (no 80STORE bit) are unaffected.
    const int Page2 = (ds.page2 && !(iieMemMode & MF_80STORE)) ? 1 : 0;

    // MAME `apple2video.cpp:140`: two 0-states ([0, 0..63]).
    const int h_state = h_clock - (h_clock > 0);
    const int h_0 = (h_state >> 0) & 1;
    const int h_1 = (h_state >> 1) & 1;
    const int h_2 = (h_state >> 2) & 1;
    const int h_3 = (h_state >> 3) & 1;
    const int h_4 = (h_state >> 4) & 1;
    const int h_5 = (h_state >> 5) & 1;

    // MAME `apple2video.cpp:149`: V[543210CBA] = 100000000 = 256+v.
    // The overflow compensation uses screen().height() in MAME; POM2's
    // frame is 262 lines so we subtract the screen height when v wraps.
    int v_state = 256 + v_clock;
    if (v_clock >= 256) v_state -= static_cast<int>(kLinesPerFrame);
    const int v_A = (v_state >> 0) & 1;
    const int v_B = (v_state >> 1) & 1;
    const int v_C = (v_state >> 2) & 1;
    const int v_0 = (v_state >> 3) & 1;
    const int v_1 = (v_state >> 4) & 1;
    const int v_2 = (v_state >> 5) & 1;
    const int v_3 = (v_state >> 6) & 1;
    const int v_4 = (v_state >> 7) & 1;

    // Mixed-mode bottom 4 text rows: HGR off when Mixed && v_4 && v_2.
    if (Hires && Mixed && v_4 && v_2) Hires = 0;

    const int addend0 = 0x0D;
    const int addend1 = (h_5 << 2) | (h_4 << 1) | (h_3 << 0);
    const int addend2 = (v_4 << 3) | (v_3 << 2) | (v_4 << 1) | (v_3 << 0);
    const int sum     = (addend0 + addend1 + addend2) & 0x0F;

    uint16_t address = 0;
    address |= static_cast<uint16_t>(h_0 << 0);
    address |= static_cast<uint16_t>(h_1 << 1);
    address |= static_cast<uint16_t>(h_2 << 2);
    address |= static_cast<uint16_t>(sum << 3);
    address |= static_cast<uint16_t>(v_0 << 7);
    address |= static_cast<uint16_t>(v_1 << 8);
    address |= static_cast<uint16_t>(v_2 << 9);
    if (Hires) {
        address |= static_cast<uint16_t>(v_A << 10);
        address |= static_cast<uint16_t>(v_B << 11);
        address |= static_cast<uint16_t>(v_C << 12);
        // HGR page base: $2000 (page 1) or $4000 (page 2). MAME's
        // `m_hgr2` is the page-2 base for IIe; on II/II+ it's the
        // same $4000.
        address |= static_cast<uint16_t>(Page2 ? 0x4000 : 0x2000);
    } else {
        // Text base. MAME also adds 0x1000 during HBL on II/II+ ("Apple
        // II HBL phantom row"); on IIe that bit is suppressed. POM2 is
        // a II/II+/IIe emulator — gate on !iieMode to match the model.
        address |= static_cast<uint16_t>(Page2 ? 0x0800 : 0x0400);
        if (!iieMode && h_clock < 25) {
            address |= 0x1000;
        }
    }
    return mem[address];
}

uint8_t Memory::languageCardRead(uint16_t addr) const
{
    if (!lcReadRam) {
        // //c ROMBANK alt firmware overrides motherboard ROM at $D000-$FFFF.
        // LC RAM path below is unaffected — banking only swaps the ROM side.
        uint8_t out;
        if (iicProfile_ && iicProfile_->languageCardRomRead(addr, out))
            return out;
        return mem[addr];
    }
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) return lcBank2Active ? auxLcBank2[off] : auxLcBank1[off];
        return lcBank2Active ? lcBank2[off] : lcBank1[off];
    }
    if (useAux) return auxLcHigh[addr - 0xE000];
    return lcHigh[addr - 0xE000];
}

void Memory::languageCardWrite(uint16_t addr, uint8_t value)
{
    if (!lcWriteEnable) return;
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) {
            if (lcBank2Active) auxLcBank2[off] = value;
            else               auxLcBank1[off] = value;
        } else {
            if (lcBank2Active) lcBank2[off] = value;
            else               lcBank1[off] = value;
        }
        return;
    }
    if (useAux) auxLcHigh[addr - 0xE000] = value;
    else        lcHigh[addr - 0xE000] = value;
}

std::string Memory::recentIoReadSummary() const
{
    const uint32_t n = (ioReadRingPos_ < kIoReadRing) ? ioReadRingPos_ : kIoReadRing;
    if (n == 0) return "(none captured)";
    uint16_t addrs[kIoReadRing];
    int      cnts [kIoReadRing];
    int t = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t a = ioReadRing_[(ioReadRingPos_ - 1 - i) % kIoReadRing];
        int j = 0;
        for (; j < t; ++j) if (addrs[j] == a) { ++cnts[j]; break; }
        if (j == t) { addrs[t] = a; cnts[t] = 1; ++t; }
    }
    // Sort distinct addresses by descending count (t is tiny).
    for (int i = 0; i < t; ++i)
        for (int j = i + 1; j < t; ++j)
            if (cnts[j] > cnts[i]) {
                int ct = cnts[i]; cnts[i] = cnts[j]; cnts[j] = ct;
                uint16_t at = addrs[i]; addrs[i] = addrs[j]; addrs[j] = at;
            }
    auto label = [](uint16_t a) -> const char* {
        switch (a) {
            case 0xC000: return "KBD";
            case 0xC010: return "KBDSTRB";
            case 0xC011: case 0xC012: return "LCSTATE";
            case 0xC019: return "RDVBL";
            case 0xC01F: return "RD80COL";
            case 0xC061: return "PB0/OpenApple";
            case 0xC062: return "PB1/SolidApple";
            case 0xC064: case 0xC065: return "PADDLE";
            default: break;
        }
        if (a >= 0xC0E0 && a <= 0xC0EF) return "DISKII/IWM";
        if (a >= 0xC0D0 && a <= 0xC0DF) return "HDV(slot5)";
        return "";
    };
    std::string out;
    const int show = (t < 8) ? t : 8;
    char buf[24];
    for (int i = 0; i < show; ++i) {
        if (i) out += "  ";
        std::snprintf(buf, sizeof(buf), "$%04X", addrs[i]);
        out += buf;
        const char* lab = label(addrs[i]);
        if (*lab) { out += '('; out += lab; out += ')'; }
        std::snprintf(buf, sizeof(buf), "x%d", cnts[i]);
        out += buf;
    }
    return out;
}

// The slow half of the bus read. memRead() in the header decides the two hot
// cases inline (main RAM below $C000, ROM at $D000+) and calls this for
// everything else — and for EVERYTHING while a read watchpoint is armed
// (`readDivert_`), which is the only time the test below can succeed. The
// report happens after the read so the hit carries the value the bus saw.
uint8_t Memory::memReadSlow(uint16_t addr)
{
    const uint8_t v = memReadSlowBody(addr);
    if (readDivert_ && readWatch_[addr] && watchSink_)
        watchSink_->noteAccess(addr, v, /*write=*/false);
    return v;
}

// memReadSlow's body, unchanged from before the fast-path split: any path
// that reaches it behaves exactly as it always did.
inline uint8_t Memory::memReadSlowBody(uint16_t addr)
{
    // Flat test RAM, or a card CPU's own bus. → Memory.h ForeignBus
    if (flatBus_) return foreignBus_ ? foreignBus_->read(addr) : mem[addr];

    // Fast path: RAM below $C000. In IIe mode the read may route to aux RAM
    // (RAMRD / ALTZP / 80STORE+PAGE2). In II+ mode, plain main bank.
    if (addr < 0xC000) {
        return iieMode ? iieMemRead(addr) : mem[addr];
    }
    if (addr >= 0xD000) {
        uint8_t v = languageCardRead(addr);
        // Dallas DS1216E "SmartWatch" on Apple II / II+ ONLY. AppleWin
        // parity (Memory.cpp:IO_F8xx — "NSC for Apple II/II+, GH#827"):
        // II/II+ have no internal slot-3/8 ROM, so the chip sits under
        // the Monitor ROM at $F800 instead. //e / //c-class hook the
        // NSC inside the INTCXROM/SLOTC3ROM branches at $C300 + $C800
        // (where ProDOS 2.4 + GS-OS actually scan). Gate on
        // !iieMode + LC-ROM-mapped so this exactly matches AppleWin's
        // `!SW_HIGHRAM && !SW_WRITERAM` check.
        if (noSlotClock_ && !iieMode && addr >= 0xF800 && !lcReadRam) {
            v = noSlotClock_->interceptRead(addr, v);
        }
        return v;
    }

    // Diagnostic: record $C000-$C0FF reads (soft switches + slot registers)
    // so the hang detector can show which register a frozen loop polls.
    if (addr <= 0xC0FF) noteIoRead(addr);

    // //c-class : tout acces $C0xx etranger au device-select du slot 5
    // referme la fenetre $C800 de la carte percee (voir iicCardWindow_ au
    // perçage). Le firmware interne tape les soft-switches en permanence,
    // le stub du SmartPort jamais — c'est ce qui les departage.
    if (iicCardWindow_ && addr <= 0xC0FF &&
        (addr < 0xC0D0 || addr > 0xC0DF))
        iicCardWindow_ = false;

    // $C000-$C07F — built-in I/O page (keyboard, speaker, cassette,
    // display soft switches, paddles).
    if (addr <= 0xC07F) return softSwitchAccess(addr, /*isWrite=*/false, 0);

    // $C080-$C0FF — slot device-select (16 bytes per slot, slot N at
    // $C080+N*16; slot 0 = language card, slots 1-7 = expansion cards).
    if (addr <= 0xC08F) return languageCardSwitchAccess(addr, /*isWrite=*/false);
    // //c (32 KB ROM rev 0/3/4) and //c+ on-board IWM: $C0E0-$C0EF.
    // MAME wires `A2BUS_IWM` at sl6 for both `apple2c_iwm` (apple2c0,
    // UniDisk 3.5) and `apple2c_mem` (apple2c3/c4, Memory Expansion)
    // — see `apple2e.cpp:5249-5254` + `5263-5272`. The original 16 KB
    // //c (rev 255) is the **only** //c-class that uses A2BUS_DISKIING
    // (`apple2e.cpp:5212`); ROM 0/3/4 ditched the LSS for the IWM
    // when Apple unified the //c motherboard around the IWM chip in
    // 1985-86. The slot-6 DiskIICard still observes the access so its
    // motor sound / disk-turbo / head-position tracking stay current
    // — but the **value returned to the CPU is the IWM's**. The IWM's
    // sync() walks DiskImage flux via DiskIICard's pushed-in
    // setFloppy() so both controllers see the same flux stream; only
    // the bit-cell window walker differs (LSS in DiskIICard vs
    // MAME-faithful IWM state machine here).
    //
    // `iicHasAltBank` is the right gate because POM2 sets it precisely
    // for 32 KB //c-class dumps (apple2c0/c3/c4 AND apple2cp) —
    // matching MAME's rom-→-machine-config mapping at
    // `apple2e.cpp:6281-6302`.
    if (addr >= 0xC0E0 && addr <= 0xC0EF && iicProfile_) {
        uint8_t v;
        if (iicProfile_->ioReadIWM(addr, cycleCounter, v)) {
            // Disk II side effects — not for the external port's own traffic.
            if (!iicProfile_->servesExternalSmartPort())
                (void)slots.deviceSelectRead(addr);
            return v;
        }
        // Shadow mode (or no IWM/media): IWM state advanced, but the
        // byte returned to the CPU comes from the slot-6 DiskIICard LSS.
    }
    // Eve-class Le Chat Mauve registers live in slot-3 device-select
    // space ($C0B8-$C0BB — $C0B8/9 Color TEXT, $C0BA/B HGR Duochrome).
    // Le Chat Mauve Eve switches, $C0B0-$C0BF — slot 3's window, forwarded
    // to the card only while slot 3 is free (Memory.h, chatMauveBlockedBySlot3).
    if (addr >= 0xC0B0 && addr <= 0xC0BF && !chatMauveBlockedBySlot3())
        slots.broadcastVideoSwitch(addr);
    if (addr <= 0xC0FF) return slots.deviceSelectRead(addr);

    // $C100-$CFFF — slot ROM dispatch.
    //
    // II+: $C100-$C7FF goes to slot bus, $C800-$CFFF is the shared expansion
    // window owned by the most-recently-selected slot.
    //
    // IIe: INTCXROM=on swallows the entire $C100-$CFFF range into the
    // motherboard internal I/O ROM. Even when INTCXROM=off, $C300-$C3FF
    // is owned by the internal ROM unless SLOTC3ROM=on (so PR#3 reads
    // the IIe 80-col firmware out of the box).
    //
    if (iieMode) {
        // //c-class (isIIcClass): no physical slots — internal ROM is
        // always mapped at $C100-$CFFF regardless of INTCXROM. MAME
        // `apple2e.cpp:1619-1631` (`update_slotrom_banks`) ORs `m_isiic`
        // into every internal-ROM gate; the softswitch is still
        // writable/readable via $C006/$C007/$C015 but has no effect on
        // what actually executes from $CnXX. The //c reset routine at
        // $FA62 immediately `JSR $CE4D` etc. — without this override
        // those addresses would fall through to slot bus (empty → $FF),
        // and the //c never boots. Pre-Theme-6 this was gated on
        // `iicHasAltBank`, missing the 16 KB rev-255 //c case (D-1-1).
        if ((iieMemMode & MF_INTCXROM) ||
            (iicProfile_ && iicProfile_->forcesIntCxRom())) {
            if (addr == 0xCFFF) {
                intC8Rom = false;
                slots.deactivateExpansion();
            }
            // INTC8ROM latches on ANY $C3xx access while SLOTC3ROM=off —
            // INTCXROM state does not gate it (UTAIIe 5-28; MAME
            // `apple2e.cpp` `c300_int_r`: the $C300 page of the
            // INTCXROM-on view still runs c300_int_r, whose only
            // condition is `!m_slotc3rom`). Missing this: read $C3xx
            // with INTCXROM on, drop INTCXROM, then JMP into
            // $C800-$CFFF — must still see internal ROM, not slot bus.
            if (addr >= 0xC300 && addr <= 0xC3FF &&
                !(iieMemMode & MF_SLOTC3ROM)) {
                intC8Rom = true;
            }
            // //c-class override: //c+ MIG windows ($CC00/$CE00 in bank 1)
            // or alt-firmware bank-1 bytes (plain //c rev-0/3/4 + //c+
            // outside the MIG windows). Bank 0 — and plain //e INTCXROM —
            // fall through to internalIORom. See IIcClassProfile.
            uint8_t out;
            if (iicProfile_ && iicProfile_->internalRomRead(addr, floatingBus(), out)) {
                return out;
            }
            // //c-class slot-ROM punch: a slot peripheral can override the
            // forced INTCXROM mask for its own $Cn00 firmware window by
            // returning true from exposesIicOnboardRom(). Bank 1 is handled
            // by internalRomRead() above, so this is bank-0 only. Device-
            // select I/O ($C0(8+s)0-$C0(8+s)F) is never masked — it reaches
            // the slot bus above. Used today by:
            //
            //   sl5 SmartPort: host-served stub, armed by bootFromSlot only.
            //   sl4 AppleWin HLE mouse: PR#4 needs the EPROM at $C400 to
            //     reach the slot card's PIA at $C0C0. The //c's internal
            //     mouse firmware talks to on-board IOU hardware POM2
            //     doesn't model, so without this punch the //c sees a
            //     dead mouse. No autostart probe at $C400, so unarmed.
            if (iicProfile_ && addr >= 0xC100 && addr <= 0xC7FF) {
                const int slot = (addr >> 8) & 0x07;
                const bool armOk = (slot != 5) ||
                    (iicSmartPortArmed_ && !iicProfile_->servesExternalSmartPort());
                if (armOk) {
                    if (SlotPeripheral* p = slots.peripheral(slot);
                        p && p->exposesIicOnboardRom()) {
                        // Le flux d'execution entre chez la carte : sa banque
                        // $C800 redevient visible (voir iicCardWindow_).
                        if (slot == 5) iicCardWindow_ = true;
                        return slots.slotRomRead(addr);
                    }
                }
            }
            // //c-class $C800-$CFFF : la banque d'EXTENSION de la carte
            // percee. Le driver du stub $C500 saute en $CD00/$CE00 — le
            // vrai firmware Liron fait le meme BIT $CFFF + JMP $CE00 — et
            // sans ce perçage le //c executait le ROM interne comme si
            // c'etait le gestionnaire SmartPort : le boot HDV pendait en
            // boucle firmware, bloc 0 jamais lu (2026-08-30).
            //
            // La garde fine est iicCardWindow_ : sur un vrai //c cette
            // region est TOUJOURS interne (le reset moteur fait JSR $CE4D
            // sans toucher $C3xx), donc le proprietaire SlotBus ne suffit
            // pas — apres un appel du pilote il restait au slot 5 et le
            // premier JMP $CExx du moniteur ($FB4F, trace du 2026-08-30)
            // executait la banque carte. La fenetre ne s'ouvre que par un
            // fetch dans la page $C5xx du stub et se referme au premier
            // acces $C0xx etranger au device-select du slot (le firmware
            // interne tape les soft-switches en permanence, le stub
            // jamais). intC8Rom garde en plus le firmware 80 colonnes.
            if (iicProfile_ && addr >= 0xC800 && addr <= 0xCFFE &&
                !intC8Rom && iicSmartPortArmed_ && iicCardWindow_ &&
                slots.getActiveExpansionSlot() == 5) {
                if (SlotPeripheral* p = slots.peripheral(5);
                    p && p->exposesIicOnboardRom())
                    return slots.expansionRomRead(addr);
            }
            uint8_t romVal = internalIORom[addr - 0xC000];
            // Dallas DS1216E "No-Slot Clock" — AppleWin parity. On //e /
            // //c-class (INTCXROM forced or SLOTC3ROM off) the NSC sits
            // under the internal ROM at $C300-$C3FF and the expansion
            // ROM page at $C800-$C8FF. ProDOS 2.4 + GS-OS walk the magic
            // key there, not at $F800. See AppleWin Memory.cpp
            // IsPotentialNoSlotClockAccess (UAIIe:5-28).
            if (noSlotClock_ &&
                ((addr >= 0xC300 && addr <= 0xC3FF) ||
                 (addr >= 0xC800 && addr <= 0xC8FF))) {
                romVal = noSlotClock_->interceptRead(addr, romVal);
            }
            return romVal;
        }
        // $C300-$C3FF with SLOTC3ROM=off: return internal 80-col
        // firmware AND auto-enable `intC8Rom` so the firmware's
        // continuation in $C800-$CFFF (JMP $C803/$C87C/$C9B4/...) reads
        // internal ROM instead of slot bus. MAME
        // `apple2e.cpp:c300_int_r`: `m_intc8rom = true; update_slotrom_banks()`.
        if (addr >= 0xC300 && addr <= 0xC3FF &&
            !(iieMemMode & MF_SLOTC3ROM)) {
            if (iieRebootTraceEnabled() && !intC8Rom) {
                const uint16_t rpc = cpu ? cpu->getProgramCounter() : 0;
                std::ostringstream oss;
                oss << std::hex << std::uppercase << std::setfill('0');
                oss << "auto-INTCXROM flip via read $" << std::setw(4)
                    << static_cast<int>(addr) << " intC8Rom=true pc=$"
                    << std::setw(4) << static_cast<int>(rpc)
                    << " cyc=" << std::dec << cycleCounter;
                pom2::log().warn("IIE", oss.str());
            }
            intC8Rom = true;
            uint8_t out;
            if (iicProfile_ && iicProfile_->internalRomRead(addr, floatingBus(), out))
                return out;
            uint8_t romVal = internalIORom[addr - 0xC000];
            // NSC at $C300 with SLOTC3ROM=off (//e default). Same hook
            // as the INTCXROM-forced branch above.
            if (noSlotClock_) {
                romVal = noSlotClock_->interceptRead(addr, romVal);
            }
            return romVal;
        }
        // $C800-$CFFF with `intC8Rom` set: shared expansion window
        // mapped to internal ROM. Reading $CFFF additionally clears
        // `intC8Rom` and releases the slot expansion-ROM owner —
        // MAME `apple2e.cpp:c800_int_r`: `if (offset == 0x7ff) {
        // m_cnxx_slot = CNXX_UNCLAIMED; m_intc8rom = false; ... }`.
        if (intC8Rom && addr >= 0xC800 && addr <= 0xCFFF) {
            uint8_t v = internalIORom[addr - 0xC000];
            // NSC hook at $C800-$C8FF (AppleWin parity: only page 8 is
            // watched, the rest of $C800-$CFFF is left alone).
            if (noSlotClock_ && addr >= 0xC800 && addr <= 0xC8FF) {
                v = noSlotClock_->interceptRead(addr, v);
            }
            if (addr == 0xCFFF) {
                intC8Rom = false;
                slots.deactivateExpansion();
            }
            return v;
        }
        // $CFFF without intC8Rom set: still release the slot expansion
        // owner — real //e wires the address decode directly to the
        // slot latch reset, bypassing the INTCXROM mux (MAME
        // `apple2e.cpp:2636-2645` `c800_r` always runs the deactivate).
        if (addr == 0xCFFF) {
            slots.deactivateExpansion();
        }
    }
    if (addr <= 0xC7FF) return slots.slotRomRead(addr);
    return slots.expansionRomRead(addr);
}

void Memory::memWriteSlow(uint16_t addr, uint8_t value)
{
    // Flat test RAM, or a card CPU's own bus — see memReadSlowBody.
    if (flatBus_) { if (foreignBus_) foreignBus_->write(addr, value);
                    else             mem[addr] = value;  return; }

    // Write watchpoints (Memory.h § Write watchpoints). Armed addresses below
    // $C000 are diverted here by having their `writable[]` byte cleared;
    // everything from $C000 up already comes through here, so one test covers
    // RAM, soft switches, slot I/O and the language card alike. Costs an empty
    // -vector test when nobody is debugging, on the SLOW path only.
    if (!writeWatch_.empty() && (writeWatch_[addr] & kWatchArmed) && watchSink_)
        watchSink_->noteAccess(addr, value, /*write=*/true);

    // Fast path: writable RAM and Language Card overlay for $D000-$FFFF.
    if (addr < 0xC000) {
        if (!ramWritable(addr)) return;
        // Diagnostic: log every write to $0400 (top-left text-screen cell)
        // while the IIe reboot trace is armed. The user reports an 'M'
        // landing there after Choplifter's title screen; we want to see
        // from which PC and at which cycle.
        if (addr == 0x0400 && value == 0xCD && iieRebootTraceEnabled()) {
            // The 'M' write to $0400 is the smoking gun. Dump 16 bytes
            // around the writing PC plus the M6502 PC trace ring buffer
            // so we can see how control got here.
            const uint16_t pc = cpu ? cpu->getProgramCounter() : 0;
            std::ostringstream oss;
            oss << std::hex << std::uppercase << std::setfill('0');
            oss << "write $0400 = $CD ('M') pc=$" << std::setw(4)
                << static_cast<int>(pc) << " cyc=" << std::dec << cycleCounter
                << std::hex << " ctx-16..+16:";
            for (int off = -16; off <= 16; ++off) {
                const uint16_t a = static_cast<uint16_t>(pc + off);
                oss << " " << std::setw(2) << static_cast<int>(mem[a]);
                if (off == 0) oss << "<";
            }
            pom2::log().warn("IIE", oss.str());
            if (cpu) cpu->dumpPcTrace("M-write pc-trace");
        }
        else if (addr >= 0x0400 && addr <= 0x0427 && iieRebootTraceEnabled()) {
            std::ostringstream oss;
            oss << std::hex << std::uppercase << std::setfill('0');
            oss << "write $" << std::setw(4) << static_cast<int>(addr)
                << " = $" << std::setw(2)
                << static_cast<int>(value) << " ('"
                << ((value & 0x7F) >= 0x20 && (value & 0x7F) < 0x7F
                    ? static_cast<char>(value & 0x7F) : '.')
                << "') pc=$"
                << (cpu ? std::setw(4) : std::setw(0))
                << (cpu ? static_cast<int>(cpu->getProgramCounter()) : 0)
                << " cyc=" << std::dec << cycleCounter;
            pom2::log().warn("IIE", oss.str());
        }
        if (iieMode) {
            iieMemWrite(addr, value);
            // Eve CPREG auto-write (Memory.h § Aux shadow): CPREG lands in AUX
            // at the address of a write that went to MAIN.
            if (auxShadowCovers(addr) && !iieWriteToAux(addr)) aux[addr] = auxShadowByte_;
        } else {
            mem[addr] = value;
        }
        return;
    }
    if (addr >= 0xD000) {
        // DS1216E No-Slot Clock, WRITE cycles: AppleWin's
        // `CNoSlotClock::Write(address)` drives the SAME state machine
        // as reads (the key bit rides on A0 of the ADDRESS; R/W is
        // irrelevant to the matcher) — some NSC drivers feed the 64-bit
        // key with STA and never unlocked the clock with only the read
        // hook wired. Same gating as the read site (II+ Monitor-ROM
        // window, LC ROM mapped).
        if (noSlotClock_ && !iieMode && addr >= 0xF800 && !lcReadRam) {
            noSlotClock_->interceptWrite(addr);
        }
        languageCardWrite(addr, value);
        return;
    }

    // //c-class : meme regle que la lecture (voir memReadSlowBody) — tout
    // acces $C0xx etranger au device-select du slot 5 referme la fenetre
    // $C800 de la carte percee. Doit rester AVANT le retour anticipe des
    // soft switches $C000-$C07F : le firmware interne les ECRIT autant
    // qu'il les lit (STA $C00D, STA $C051…), et ces ecritures ne
    // refermaient rien — la fenetre restait ouverte apres un appel du
    // pilote et le premier JMP $CExx du moniteur executait la banque carte,
    // exactement le bug que la garde existe pour empecher.
    if (iicCardWindow_ && addr <= 0xC0FF &&
        (addr < 0xC0D0 || addr > 0xC0DF))
        iicCardWindow_ = false;

    if (addr <= 0xC07F) {
        softSwitchAccess(addr, /*isWrite=*/true, value);
        return;
    }
    if (addr <= 0xC0FF) {
        if (addr <= 0xC08F) {
            languageCardSwitchAccess(addr, /*isWrite=*/true);
            return;
        }
        // //c (32 KB ROM) / //c+ IWM (see memRead for the read side
        // and MAME refs). Writes are dispatched to IWMDevice (mode
        // register, data write, etc.) AND forwarded to DiskIICard
        // so its slot-6 state stays in sync (phases, motor on/off,
        // sound + writeback gating).
        if (addr >= 0xC0E0 && addr <= 0xC0EF && iicProfile_) {
            // A write the external port claimed is not the Disk II's (flux!).
            if (iicProfile_->ioWriteIWM(addr, value, cycleCounter)) return;
        }
        // Le Chat Mauve Eve registers — mirror of the memRead path so the
        // toggle reacts to STA $C0B9 and friends, not just LDA. Same
        // slot-3 collision guard as the read path.
        if (addr >= 0xC0B0 && addr <= 0xC0BF && !chatMauveBlockedBySlot3())
            slots.broadcastVideoSwitchWrite(addr, value);
        slots.deviceSelectWrite(addr, value);
        return;
    }
    // Same $CFFF + INTCXROM deactivate handling as memRead (MAME
    // `apple2e.cpp:2636-2645`). The write goes through to the slot
    // bus regardless so cards that decode their own $C800-$CFFF
    // window (rare on a IIe) still see it.
    if (iieMode && addr == 0xCFFF) {
        intC8Rom = false;   // same flip-flop reset as the read side
        slots.deactivateExpansion();
    }
    // Mirror the memRead INTCXROM override for //c: when internal ROM
    // is mapped at $C100-$CFFF, writes are absorbed (real silicon: ROM
    // is read-only, slot bus is not reached). Without this, a //c
    // firmware write into $CnXX would forward to a (non-existent) slot
    // card and possibly latch activeExpansionSlot to a stale value.
    if (iieMode && ((iieMemMode & MF_INTCXROM) ||
                    (iicProfile_ && iicProfile_->forcesIntCxRom()))) {
        // NSC write-cycle hook — same $C300/$C800 windows as the
        // INTCXROM-forced read intercepts.
        if (noSlotClock_ &&
            ((addr >= 0xC300 && addr <= 0xC3FF) ||
             (addr >= 0xC800 && addr <= 0xC8FF))) {
            noSlotClock_->interceptWrite(addr);
        }
        // //c-class internal ROM is read-only: writes are absorbed,
        // except the //c+ MIG windows ($CC00/$CE00 in bank 1) which the
        // profile dispatches (drive enable/disable, IWM reset, MIG RAM —
        // MAME `apple2e.cpp:3186-3190`).
        if (iicProfile_) iicProfile_->internalRomWrite(addr, value);
        return;
    }
    // IIe $C300-$C3FF with SLOTC3ROM=off: the motherboard claims the
    // page — slot 3's I/O SELECT never asserts, so the write must NOT
    // reach the slot bus (slotRomWrite would both poke a deselected
    // card and latch slot 3 as the $C800 expansion-ROM owner, stealing
    // the window from the rightful card). The access still arms
    // INTC8ROM, mirroring the read side (UTAIIe 5-28: any $C3xx access;
    // MAME `apple2e.cpp` write view runs the same c300 internal handler).
    if (iieMode && addr >= 0xC300 && addr <= 0xC3FF &&
        !(iieMemMode & MF_SLOTC3ROM)) {
        // NSC write-cycle hook — same window as the read-side intercept.
        if (noSlotClock_) noSlotClock_->interceptWrite(addr);
        intC8Rom = true;
        return;
    }
    if (addr <= 0xC7FF) {
        // Slot ROM is read-only on most cards, but a handful (Mockingboard
        // in particular) decode 6522 VIA MMIO inside the $CnXX window.
        // SlotBus::slotRomWrite forwards to the card and latches the slot
        // as the active expansion-ROM owner (same as a read into the
        // window), so cards that genuinely have read-only ROM still see
        // their slot select on writes.
        slots.slotRomWrite(addr, value);
        return;
    }
    // $C800-$CFFF with INTC8ROM set: internal ROM owns the window —
    // the write is absorbed by ROM, never forwarded to a slot card
    // (the $CFFF deactivate already ran above). Mirrors the read side.
    if (iieMode && intC8Rom) {
        // NSC write-cycle hook — same $C800-$C8FF window as reads.
        if (noSlotClock_ && addr >= 0xC800 && addr <= 0xC8FF)
            noSlotClock_->interceptWrite(addr);
        return;
    }
    // $C800-$CFFF — expansion ROM, conventionally read-only. Forward
    // through SlotBus so $CFFF disable still works on writes (the
    // address bus is what matters, not the direction).
    slots.expansionRomWrite(addr, value);
}
