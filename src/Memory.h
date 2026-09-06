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

// Apple II / II+ / IIe memory.
//
// II+: 48 KB RAM ($0000-$BFFF), I/O page ($C000-$C0FF), slot ROM area
// ($C100-$C7FF), 12 KB Monitor + Applesoft ROM ($D000-$FFFF), 16 KB
// Language Card overlay. Soft switches at $C050-$C057 drive display modes.
//
// IIe (when isIIE() is true): adds a 64 KB auxiliary bank, a 4 KB internal
// I/O ROM at $C100-$CFFF (motherboard firmware including the slot-3
// 80-column driver), an aux Language Card overlay, and the IIe paging
// soft switches at $C000-$C00F (80STORE, RAMRD, RAMWRT, INTCXROM, ALTZP,
// SLOTC3ROM, 80COL, ALTCHAR) with status reads at $C013-$C018, $C01E,
// $C01F. RAM routing per address range:
//   $0000-$01FF      ALTZP        → aux else main
//   $0200-$03FF      RAMRD/RAMWRT → aux else main
//   $0400-$07FF      80STORE on   → PAGE2 picks aux/main; else RAMRD/WRT
//   $0800-$1FFF      RAMRD/RAMWRT → aux else main
//   $2000-$3FFF      80STORE+HIRES on → PAGE2 picks aux/main; else RAMRD/WRT
//   $4000-$BFFF      RAMRD/RAMWRT → aux else main
//   $C100-$CFFF      INTCXROM     → internal IO ROM else slot bus
//                    SLOTC3ROM off → $C300-$C3FF reads internal ROM even
//                                    when INTCXROM is off
//   $D000-$FFFF      ALTZP picks the aux Language Card bank trio

#ifndef POM2_MEMORY_H
#define POM2_MEMORY_H

#include "SlotBus.h"
#include "MemoryProfile.h"
#include "CpuClock.h"
#include "Keyboard.h"
#include "MemoryWatchSink.h"
#include "PaddleInputs.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class CassetteDevice;
class M6502;
class SpeakerDevice;
namespace pom2 { class NoSlotClock; }

namespace pom2 { class IWMDevice; class SmartPortHub; class SmartPortBusPort; }

class Memory
{
public:
    Memory();

    // Built-in cassette interface — Apple II routes $C020 (output toggle)
    // and $C060 (input bit-7) directly to the cassette without a card.
    // Pointer is non-owning; lifetime managed by EmulationController.
    void setCassetteDevice(CassetteDevice* dev) { cassette = dev; }

    /// Built-in speaker (1-bit flip-flop at $C030-$C03F). Non-owning
    /// pointer set by EmulationController. The CPU pointer is needed
    /// by the $C030 handler to timestamp toggles with sub-instruction
    /// precision (`cycleCounter + cpu->getCurrentInstructionCycles()`).
    void setSpeakerDevice(SpeakerDevice* s) { speaker = s; }

    /// Dallas DS1216E "No-Slot Clock" — sits under the Monitor ROM
    /// ($F800-$FFFF). Memory hooks reads to that range through the
    /// chip's pattern-matcher; when 64 magic-key bits match, the next
    /// 64 reads return clock bits on D0. Non-owning pointer set by
    /// EmulationController; nullptr disables the intercept.
    void setNoSlotClock(pom2::NoSlotClock* nsc) { noSlotClock_ = nsc; }
    pom2::NoSlotClock* getNoSlotClock() const { return noSlotClock_; }
    /// Wire the host CPU. Also installs a SlotBus IRQ router so cards
    /// can raise IRQ via `SlotPeripheral::assertIrq()` without each
    /// holding their own M6502*. Pass nullptr to disconnect (the router
    /// is replaced with an empty function so stray assertIrq() calls
    /// from teardown don't dereference a dangling pointer).
    void setCpu(M6502* c);

    /// Apple //c / //c+ on-board IWM controller. Non-owning pointer
    /// set by EmulationController. When iicHasAltBank is on, $C0E0-
    /// $C0EF accesses always mirror to this device so its state
    /// machine (MAME `iwm.cpp` port — see `IWMDevice.{h,cpp}`)
    /// evolves in lock-step with the slot-6 DiskIICard's lightweight
    /// IWM-mode shadow.
    void setIWM(pom2::IWMDevice* iwm) {
        iwmDevice = iwm;
        if (iicProfile_) iicProfile_->setIwm(iwm);
    }

    /// When true (default), `$C0E0-$C0EF` reads on iicHasAltBank
    /// profiles return the IWMDevice's value rather than the slot-6
    /// DiskIICard's. Writes are dispatched to both either way.
    /// Setting false reverts to "shadow mode" — IWMDevice still
    /// advances on every access (timer drain, mode/status registers
    /// stay coherent with what the //c+ alt firmware expects), but
    /// the byte the CPU sees comes from DiskIICard's LSS path. Used
    /// during the SmartPort port to A/B-compare the two paths; the
    /// env var `POM2_IWM_LEGACY_DATA_PATH` lets the user flip this
    /// without rebuilding.
    void setIWMAuthoritative(bool on) {
        iwmAuthoritative = on;
        if (iicProfile_) iicProfile_->setIwmAuthoritative(on);
    }
    bool isIWMAuthoritative() const   { return iwmAuthoritative; }

    /// //c+ SmartPort hub — owned by EmulationController, wired here so
    /// MIG state changes ($C0CC/$C0CE windows) can call
    /// `recalc_active_device` per MAME `apple2e.cpp:638-679`. Non-owning.
    void setSmartPortHub(pom2::SmartPortHub* hub) {
        smartPortHub = hub;
        if (iicProfile_) iicProfile_->setSmartPortHub(hub);
    }
    pom2::SmartPortHub* getSmartPortHub() const   { return smartPortHub; }

    /// Plain //c external disk port (SmartPortBusPort.h). Owned by
    /// EmulationController; forwarded to the //c-class profile like the hub.
    void setExternalSmartPort(pom2::SmartPortBusPort* port) {
        externalSmartPort_ = port;
        if (iicProfile_) iicProfile_->setExternalSmartPort(port);
    }
    pom2::SmartPortBusPort* getExternalSmartPort() const { return externalSmartPort_; }
    /// True on a //c+ whose own firmware is serving the external SmartPort
    /// device: an explicit boot of slot 5 must go through the ROM's reset
    /// scan ($F223), because its $C500 page entered directly never probes
    /// the port, and the host-served stub is withheld while the port is live.
    bool iicPlusBootsSlot5ByReset() const {
        return iicProfile_ && iicProfile_->isIIcPlus() &&
               iicProfile_->servesExternalSmartPort();
    }

    /// //c-class on-board SmartPort "armed" gate. The slot-5 SmartPort
    /// firmware stub is punched through the INTCXROM mask at $C500-$C5FF
    /// ONLY while this is set (EmulationController::bootFromSlot sets it;
    /// every reset/cold-boot clears it). Rationale: the //c ROM's own
    /// autostart + a booted ProDOS expect the REAL //c SmartPort firmware
    /// at $C500 for device enumeration — substituting the block stub there
    /// during a normal reboot corrupts a multi-device boot (Disk II + the
    /// on-board SmartPort). Exposing the stub only for an explicit GUI
    /// "Boot" (bootFromSlot) avoids that. See project_iic_smartport_boot.
    void setIicSmartPortArmed(bool on) { iicSmartPortArmed_ = on;
                                         iicCardWindow_ = false; }
    bool iicSmartPortArmed() const     { return iicSmartPortArmed_; }

    /// Apple II expansion bus — slots 0-7. Cards plug directly via the
    /// SlotBus. Memory routes $C080-$CFFF accesses through it.
    SlotBus&       slotBus()       { return slots; }
    const SlotBus& slotBus() const { return slots; }

    /// Flat 64 KB RAM mode — bypasses soft switches, slot bus and ROM
    /// write protection. Every access is plain mem[addr]. Used **only**
    /// by the Klaus Dormann functional test, which expects the whole
    /// address space to behave as RAM. Must NOT be enabled in normal
    /// emulation; no safety checks remain.
    void setTestMode(bool enabled) { testMode = enabled; refreshBusFlags(); }
    bool isTestMode() const        { return testMode; }

    // ── Foreign bus (a coprocessor card's private address space) ─────────
    // POM2 has exactly one 6502 core, and `M6502` reaches memory through
    // this class. An intelligent card — the Apple II Workstation Card is the
    // first — runs the same core over a completely different map: its own
    // RAM, its own I/O selects, its own banked ROM, none of it Apple II.
    //
    // The rule this obeys is PERFORMANCE § 8.2/8.5: a branch added to the
    // bus path costs +13-16 %, and merely TESTING a flag there costs +7.2 %.
    // So nothing is added. `flatBus_` replaces the `testMode` test that both
    // slow paths and `memWrite`'s fast path already made — one byte load for
    // another — and `foreignBus_` is folded into the three derived read
    // gates the same way `readDivert_` is, so `memRead` is untouched. A
    // Memory with a foreign bus takes the slow path for everything, which is
    // free: nothing else in POM2 shares that instance.
    struct ForeignBus {
        virtual ~ForeignBus() = default;
        virtual uint8_t read(uint16_t addr) = 0;
        virtual void    write(uint16_t addr, uint8_t value) = 0;
    };

    /// Route every access on this Memory to `bus` instead of the Apple II
    /// map. Pass nullptr to detach. Mutually exclusive with test mode: both
    /// claim the same bypass, and a card that wanted flat RAM would not need
    /// a bus in the first place.
    void setForeignBus(ForeignBus* bus) {
        assert(!(bus && testMode) && "foreign bus and test mode are exclusive");
        foreignBus_ = bus;
        refreshBusFlags();
        // A card's CPU has no beam. Park the video event threshold at the end
        // of time so advanceCycles() never runs Apple II scanline bookkeeping
        // — which it would otherwise do on every emulated instruction of a
        // machine that has no display.
        if (bus) vblNextEventCycle_ = ~uint64_t{0};
    }
    ForeignBus* foreignBus() const { return foreignBus_; }

    /// Test/debug accessor for the current floating-bus byte (the value an
    /// undriven soft-switch read returns) at EXACTLY `cycleCounter` — no
    /// in-instruction offset, so a test that sets the cycle gets a
    /// deterministic byte. (The CPU read path uses the access-cycle variant.)
    uint8_t peekFloatingBus() const { return floatingBus(cycleCounter); }

    /// Read a byte from MAIN-bank RAM directly, bypassing IIe aux paging
    /// (80STORE/RAMRD/PAGE2/ALTZP) and the soft-switch / slot-bus / ROM
    /// machinery. The AppleMouse firmware always maintains its position +
    /// status screen holes ($0478+s / $0578+s / $04F8+s / $05F8+s /
    /// $07F8+s) in main memory, so the host-side mouse-sync feedback loop
    /// (`MainWindow::onMouseMove`) must read them from here — `memRead`
    /// would route to aux whenever RAMRD/PAGE2 happen to be set when the
    /// host cursor callback fires. Do NOT use for CPU bus emulation.
    uint8_t peekMainRam(uint16_t addr) const { return mem[addr]; }

    // CPU bus interface (called from M6502).
    //
    // memRead() is the single most-executed function in POM2 — once or twice
    // per emulated CPU cycle, ~1.02 M cycles per emulated second. It used to
    // live entirely in Memory.cpp, where callgrind put it (plus the
    // languageCardRead it tail-calls) at **35 % of a ROM-banner profile**,
    // most of that being the out-of-line call itself around what is, in the
    // common case, one array index.
    //
    // So the two hot cases are decided HERE, in the header, where they inline
    // into M6502's accessors:
    //
    //   * $0000-$BFFF on a non-//e machine → plain main RAM;
    //   * $D000-$FFFF with the language card reading ROM → plain ROM.
    //     That second one is not an edge case: with no LC RAM mapped, EVERY
    //     opcode fetch of Applesoft and the Monitor goes through it. (The
    //     equivalent trap in NeoST's bus was worth −4 % vs −20 % depending on
    //     whether the ROM window was in the fast path — see docs/PERFORMANCE.md.)
    //
    // Everything else — soft switches, slot ROM/IO, //e aux paging with bank
    // tracing on, LC RAM, the //c alt-firmware banks, the NoSlotClock window
    // — falls through to memReadSlow(), which is the ORIGINAL function,
    // unchanged. The fast path's conditions are the exact negation of that
    // function's own guards, so behaviour is identical by construction.
    uint8_t memRead(uint16_t addr)
    {
        // Read watchpoints live in this function WITHOUT a test of their
        // own: a flag test added here measured +7.2 % / +4.2 % (PERFORMANCE
        // § 8.5), so instead the three derived bytes `plainRead_`,
        // `iieFastRead_` and `romFastRead_` each fold `readDivert_` into a
        // test that was already being made. Arming a read watch clears all
        // three; the fast path then falls through to memReadSlow on the
        // branches it already had. See refreshReadFastFlags().
        if (addr < 0xC000) {
            // testMode (Klaus harness) = flat RAM over the whole space, and
            // it is checked first in memReadSlow too — keep that order.
            // (`plainRead_` = `!iieMode || testMode`, minus any read watch.)
            if (plainRead_) return mem[addr];
            // //e aux routing. `bankTrace_` is a debug-only diagnostic; when
            // it is armed the slow path takes over so the tracing lives in
            // exactly one place. (`iieFastRead_` = //e, `!bankTrace_`, no
            // read watch.)
            if (iieFastRead_) return iieReadFromAux(addr) ? aux[addr] : mem[addr];
            return memReadSlow(addr);
        }
        if (testMode) return mem[addr];
        // ROM window. `!lcReadRam` → the LC maps ROM; `romFastRead_` → no //c
        // alt-firmware bank can override it (`!iicProfile_`) and no read
        // watch is armed; the last clause is the exact NoSlotClock intercept
        // condition ($F800+ on a II/II+ with a chip fitted), which is the
        // only other reader of this range.
        // (Tried 2026-08-20: caching these three tests in one bool measured
        // 4 % SLOWER on an M1 — they are adjacent loads the compiler already
        // schedules well. Leave the condition written out.)
        if (addr >= 0xD000 && !lcReadRam && romFastRead_
            && !(noSlotClock_ && !iieMode && addr >= 0xF800))
            return mem[addr];
        // //e internal $C100-$CFFF I/O ROM. The //e executes its keyboard
        // input loop, the 80-column firmware and much of its Monitor glue
        // from here (INTCXROM on, or $C3xx/$C8xx via INTC8ROM), so on a //e
        // this window carries the opcode-fetch traffic the ROM window
        // carries on a ][+ — measured at a third of a //e banner profile
        // when it went through memReadSlow(). The conditions below are
        // exactly the cases where memReadSlow() returns
        // `internalIORom[addr - 0xC000]` with NO side effect:
        //   * no //c-class profile (alt-firmware banks, slot-ROM punches)
        //     and no NoSlotClock (its $C3xx/$C8xx intercept is stateful);
        //   * not $CFFF (releases the expansion-ROM owner / INTC8ROM);
        //   * INTCXROM on: whole window, except a $C3xx read that would
        //     LATCH intC8Rom (only when it is still clear and SLOTC3ROM off);
        //   * INTCXROM off: $C3xx with SLOTC3ROM off, or $C800-$CFFE,
        //     both only once intC8Rom is already latched.
        // Anything else — including the latch edge itself — still goes
        // through memReadSlow(), which is the one place those side effects
        // live. Pinned by tests/bus_fastpath_test.cpp, which walks the whole
        // 64 K under every paging state and requires memRead == memReadSlow
        // value AND side effects. (This used to name a
        // `tests/iie_internal_rom_fastpath_test.cpp` that has never existed.)
        // (`addr < 0xD000` matters: a $D000+ read that was NOT the ROM window
        // above — the language card mapping RAM — must not land here.)
        if (iieMode && addr >= 0xC100 && addr < 0xD000 && addr != 0xCFFF
            && romFastRead_ && !noSlotClock_) {
            const bool c3 = (addr & 0xFF00u) == 0xC300u;
            const bool slotC3 = (iieMemMode & MF_SLOTC3ROM) != 0;
            if (iieMemMode & MF_INTCXROM) {
                if (!c3 || intC8Rom || slotC3)
                    return internalIORom[addr - 0xC000];
            } else if (intC8Rom && (addr >= 0xC800 || (c3 && !slotC3))) {
                return internalIORom[addr - 0xC000];
            }
        }
        return memReadSlow(addr);
    }

    // memWrite() mirrors memRead(): the hot case — a write into writable
    // RAM below $C000 — is decided here, everything else (soft switches,
    // slot I/O, the language card, the //e write-trace diagnostics) falls
    // through to memWriteSlow(), the original body. On a //e the aux/main
    // routing is the shared inline helper iieWriteToAux(), the write-side
    // twin of iieReadFromAux(); iieMemWrite() uses the same one.
    //
    // The one fast-path exclusion that is not a routing rule: writes to
    // $0400-$0427 (text row 0) take the slow path unconditionally, because
    // memWriteSlow() carries an opt-in reboot-trace hook on exactly that
    // range. Forty addresses out of 48 K — cheaper to exclude than to test
    // the env flag here.
    void memWrite(uint16_t addr, uint8_t value)
    {
        if (addr < 0xC000 && !flatBus_ && writable[addr]
            && static_cast<uint16_t>(addr - 0x0400u) > 0x27u) {
            if (!iieMode) { mem[addr] = value; return; }
            if (!bankTrace_) {
                if (iieWriteToAux(addr)) aux[addr] = value;
                else                     mem[addr] = value;
                return;
            }
        }
        memWriteSlow(addr, value);
    }

    // ── Write watchpoints ────────────────────────────────────────────────
    // A watchpoint that cost the fast path a branch was measured at +13-16 %
    // on pom2_bench and thrown away (PERFORMANCE § 8.2). This is the design
    // that costs nothing: arming a watch **clears the address's `writable[]`
    // byte**, so `memWrite`'s existing test fails and the write falls into
    // `memWriteSlow` on its own — no new branch, no new load, not one
    // instruction added to the hot path. The slow path then reports the
    // access and performs the write using the REAL permission, shadowed in
    // `writeWatch_` (bit 1) beside the armed bit (bit 0).
    //
    // Consequences worth knowing:
    //   * $C000 and above needs no diversion at all — those writes already
    //     go through `memWriteSlow`, so soft switches, slot I/O and the
    //     language card are watchable for free.
    //   * The watch is on the ADDRESS, not the bank: on a //e it fires
    //     whichever of main/aux the paging routes the write to.
    //   * It fires on the ACCESS, including a write the machine then drops
    //     (write-protected RAM). "Somebody wrote here" is the question a
    //     watchpoint is asked; whether the byte stuck is the next one.
    // `markRomRegion` and `restoreMainRam` are the two other readers of
    // `writable[]`, and both consult `ramWritable()` so a diverted address
    // does not read as ROM to them.
    void setWriteWatch(uint16_t addr, bool on);
    void clearWriteWatches();
    bool hasWriteWatch(uint16_t addr) const {
        return !writeWatch_.empty() && (writeWatch_[addr] & kWatchArmed) != 0;
    }
    std::size_t writeWatchCount() const { return writeWatchCount_; }
    /// Where a watched write is reported. Set once, at wiring time.
    void setWatchSink(pom2::MemoryWatchSink* sink) { watchSink_ = sink; }
    /// The address's REAL write permission — what `writable[]` would say if
    /// no watchpoint had diverted it.
    bool ramWritable(uint16_t addr) const {
        if (!writeWatch_.empty() && (writeWatch_[addr] & kWatchArmed))
            return (writeWatch_[addr] & kWatchWasWritable) != 0;
        if (auxShadowCovers(addr)) return true;   // RAM pages by construction
        return writable[addr];
    }

    // ── Aux shadow (Le Chat Mauve Eve CPREG auto-write) ──────────────────
    // docs/chatmauve_plan.md § 3.4: while "en fonction", the Eve deposits its
    // CPREG byte into AUX at the address of every CPU write that lands in
    // MAIN text page ($0400-$07FF, TXT16) or HGR page ($2000-$3FFF,
    // ENHRCPREG). Same zero-cost design as the write watchpoints: arming
    // clears `writable[]` over the page, memWrite's own test then fails and
    // the write falls into memWriteSlow, which performs it and adds the aux
    // byte. Nothing is tested for on the hot path. `ramWritable()` reports
    // an armed page as writable — those two pages are RAM on every profile.
    void setAuxShadow(bool textPage, bool hgrPage, uint8_t byte);
    bool auxShadowCovers(uint16_t addr) const {
        return (auxShadowText_ && addr >= 0x0400 && addr <= 0x07FF) ||
               (auxShadowHgr_  && addr >= 0x2000 && addr <= 0x3FFF);
    }
    bool    auxShadowText() const { return auxShadowText_; }
    bool    auxShadowHgr()  const { return auxShadowHgr_; }
    uint8_t auxShadowByte() const { return auxShadowByte_; }

    // ── Read watchpoints ─────────────────────────────────────────────────
    // Reads have no per-address table on their fast path to hide a watch in
    // (that is why the write half above was free and this half is not), so
    // the design is one level coarser: `readDivert_` is true while ANY read
    // watch is armed, and `memRead` then sends every read to `memReadSlow`,
    // which reports the watched ones to the sink after performing the read.
    //   * Un-armed cost: one byte load and a predictable branch on each of
    //     memRead's two halves — measured at the noise floor on all three
    //     pom2_bench workloads (PERFORMANCE § 8.5), not assumed.
    //   * Armed cost: every bus read goes out of line — roughly the
    //     pre-2026-08 profile (§ 3.2). Paid only by the session that armed a
    //     read watch, and only while it is armed.
    //   * Fires on the bus ACCESS, after the read, with the value read:
    //     opcode fetches included (a watch on $FBB3 sees the ROM-ID check),
    //     soft-switch reads included (with their side effects, as on the
    //     real bus). The UI's memory viewer peeks `mem[]` and never fires.
    //   * The watch is on the ADDRESS, not the bank (same rule as writes).
    void setReadWatch(uint16_t addr, bool on);
    void clearReadWatches();
    bool hasReadWatch(uint16_t addr) const {
        return !readWatch_.empty() && readWatch_[addr] != 0;
    }
    std::size_t readWatchCount() const { return readWatchCount_; }

    // Diagnostic — used by M6502's BRK trace.
    std::string busStateSummary() const;

    // ROM loading. Apple II distributions ship as a single 12 KB image
    // covering $D000-$FFFF. Returns 1 on success, 0 on failure (last
    // error in `lastError`).
    //
    // 32 KB dump disambiguation: //e "system + video combined" dumps
    // carry the firmware in the UPPER 16 KB (file offsets 0x4000-0x7FFF)
    // with charset data in the lower half. //c / //c+ dumps instead
    // carry TWO 16 KB firmware banks side-by-side — bank 0 in the LOWER
    // half (the cold-reset entry), bank 1 in the upper half (alt
    // firmware reached via $C028 ROMBANK). The two layouts are
    // indistinguishable from file size alone — the caller passes
    // `pickLower16KFor32K=true` when loading a //c-style dump.
    int loadAppleIIRom(const char* filename, bool pickLower16KFor32K = false);
    /// 2 KB (II/II+) or 4 KB (IIe-class) dump, or an 8 KB INTERNATIONAL //e
    /// video ROM — two 4 KB banks, `bank` selects which (0 = low). See the
    /// comment in the definition for why an 8 KB part exists at all.
    int loadCharRom(const char* filename, int bank = 0);

    /// True when the loaded character generator actually carries lowercase
    /// glyphs. A 4 KB IIe-class ROM always does; a 2 KB one usually does
    /// NOT — except the Videx LOWER CASE CHIP, which is the whole reason
    /// this is a fact about the dump rather than about its size. The
    /// renderer folds a-z to A-Z only when this is false.
    bool charRomHasLowercase() const { return charRomLowercase_; }

    const std::string& getLastError() const { return lastError; }

    // Direct access for the display / debugger / snapshot.
    const uint8_t* data() const { return mem.data(); }

    /// Test/debug write into MAIN-bank RAM, bypassing IIe aux paging
    /// (80STORE/RAMRD/PAGE2/ALTZP) and ignoring the writable[] bitmap.
    /// Asserts the target lies inside RAM (`addr < 0xC000`) so a stray
    /// debugger poke can't silently rewrite slot ROM or the Monitor —
    /// loadRomBytes()/loadAppleIIRom() are the only legitimate ways to
    /// touch the ROM mirror.
    void writeRamUnchecked(uint16_t addr, uint8_t value) {
        assert(addr < 0xC000 && "writeRamUnchecked must target RAM, not ROM/I-O");
        mem[addr] = value;
    }

    /// Flat-RAM bulk load for the Klaus Dormann functional tests, which
    /// stuff a 64 KB image (including ROM-mirror bytes) into Memory and
    /// then expect every byte to behave as plain RAM. Asserts that
    /// `setTestMode(true)` has been called, so this can't be misused as
    /// a back door from production code.
    void loadFlatTestImage(const uint8_t* src, size_t length) {
        assert(testMode && "loadFlatTestImage requires setTestMode(true)");
        assert(length <= 0x10000 && "image larger than 64 KB");
        if (src && length) std::memcpy(mem.data(), src, length);
    }

    /// Bulk ROM load — bypasses the writable[] bitmap. Used by the ROM-flashing paths
    /// (and by future cards) to flash their ROM image into protected
    /// regions without having to flip ROM-protect manually. `addr +
    /// length` must be ≤ $10000.
    bool loadRomBytes(const uint8_t* src, size_t length, uint16_t addr);

    /// Mark `[lo, hi]` as ROM (writes silently dropped). Public so cards
    /// can declare their ROM windows after loading. Idempotent.
    void markRomRange(uint16_t lo, uint16_t hi) { markRomRegion(lo, hi); }

    // Character ROM access (2 KB, 8 bytes/glyph). May be empty if the
    // user hasn't loaded a charset — we then fall back to a built-in
    // ASCII table at render time.
    const std::vector<uint8_t>& charRom() const { return characterRom; }

    // ── International 8 KB char ROM: runtime bank select via annunciator 2 ──
    // A localized //e (French, Japanese katakana, …) fits a 2364-class 8 KB
    // character generator holding TWO 4 KB sets and wires the char ROM's A12
    // to annunciator 2 ($C05C = AN2 off, $C05D = AN2 on) — so software flips
    // the whole font by poking AN2 (apple2history.org ch.12; the Japanese
    // j-Plus katakana toggle is the canonical example). POM2 loads such a ROM
    // dual-bank (`loadCharRom` with bank < 0) and selects the live 4 KB set
    // here; a plain 4 KB ROM leaves AN2 a no-op, exactly as on a US machine.
    // French Touch "Block ASCII Anthology" uses this to switch between its
    // normal-text set (the intro screen) and its block-glyph set (the art).
    bool charRomIsDualBank() const { return charRomDualBank_; }
    std::size_t charRomBankOffset() const {
        return (charRomDualBank_ && an2) ? 4096u : 0u;
    }
    const uint8_t* charRomActiveData() const {
        return characterRom.empty() ? nullptr
                                     : characterRom.data() + charRomBankOffset();
    }
    std::size_t charRomActiveSize() const {
        return charRomDualBank_ ? 4096u : characterRom.size();
    }

    // Soft-switch state (read by the display / speaker / paddle code).
    struct DisplayState {
        bool textMode  = true;   // $C050 clear, $C051 set
        bool mixedMode = false;  // $C052 clear, $C053 set
        bool page2     = false;  // $C054 page 1, $C055 page 2
        bool hiRes     = false;  // $C056 lo-res, $C057 hi-res
        // 80COL ($C00C off / $C00D on) and AN3 ($C05E off / $C05F on)
        // are tracked here so the UI can show them. Le Chat Mauve / Video-7
        // also subscribe to per-access edges via SlotBus::broadcastVideoSwitch
        // — a cleared/set boolean is not enough on its own (the FIFO needs
        // every rising edge), but the snapshot is useful for diagnostics.
        bool eightyCol = false;
        bool an3       = false;
        // IIe-only: ALTCHAR ($C00E off / $C00F on) selects between the
        // standard charset (with flashing inverse) and the alternate set
        // (mousetext + non-flashing inverse). Ignored on II+.
        bool altChar   = false;
        // IIe-only: DHIRESON ($C05E) / DHIRESOFF ($C05F). When 80COL is
        // also on, DHGR mode reads aux + main HGR pages interleaved and
        // doubles the horizontal resolution to 560. Ignored on II+
        // (where the same soft switches are pure AN3 annunciator).
        bool dhgr      = false;
        // IIe-only: 80STORE ($C000 off / $C001 on) makes PAGE2 swap text
        // page 1 (and HGR page 1 when HIRES is on) to aux RAM rather than
        // selecting page 2. The display needs this to know whether to read
        // the text page from aux when 80STORE+PAGE2 are both on.
        bool eightyStore = false;
    };
    DisplayState getDisplayState() const {
        std::lock_guard<std::mutex> lk(stateMutex);
        return display;
    }

    /// Beam-racing: video soft-switch edges logged with CPU-cycle timestamps
    /// during each emulated frame. Apple2Display replays them per scanline
    /// when non-empty; otherwise the fast single-snapshot path is used.
    enum class VideoEventKind : uint8_t {
        TextMode,
        MixedMode,
        Page2,
        HiRes,
        EightyCol,
        Dhgr,
        An3,
        EightyStore,
        AltChar,
    };
    struct VideoEvent {
        uint64_t       emuCycle = 0;
        uint16_t       scanline = 0;   // 0..191 visible band
        VideoEventKind kind     = VideoEventKind::TextMode;
        bool           value    = false;
    };

    /// Video standard (NTSC 262 lines / PAL 312 lines). Set on profile load.
    /// Used by pushVideoEventLocked to stamp each soft-switch edge with the
    /// correct scanline geometry, so beam-racing positions PAL effects right.
    void          setVideoStandard(VideoStandard s) { videoStandard_.store(s); vblNextEventCycle_ = 0; }
    VideoStandard videoStandard() const { return videoStandard_.load(); }

    /// LEGACY synchronous bracket (tests only): snapshot display state and
    /// clear the event log; the matching takeVideoEvents() closes the bracket
    /// and moves the log out. The app does NOT use this — recording is
    /// continuous and advanceCycles() publishes the completed frame at each
    /// video-frame boundary (65 × 262/312 cycles), so a 60 Hz UI consuming
    /// 50 Hz PAL content never steals a half-recorded frame nor drops the
    /// events recorded between its take and the next worker tick.
    void beginVideoEventFrame();

    /// Display soft-switch state at the start of the last published frame
    /// (or of the legacy bracket when one is open — tests).
    DisplayState getDisplayStateAtFrameStart() const {
        std::lock_guard<std::mutex> lk(stateMutex);
        return legacyEventBracket_ ? displayAtFrameStart_ : publishedFrameStart_;
    }

    /// Video events of the last published frame, as a COPY — the UI may
    /// re-render the same frame several times (60 Hz vsync over 50 Hz PAL
    /// content). In legacy-bracket mode: closes the bracket and moves the
    /// recording out (the pre-publication contract the tests pin).
    std::vector<VideoEvent> takeVideoEvents();

    // Keyboard bridge — UI thread enqueues keys, CPU thread reads them
    // via $C000 / clears the strobe via $C010. State + FIFO live in
    // pom2::Keyboard (`keyboard_`); these forward to it, and the softswitch
    // read path calls `keyboard_.latchMirror()` / `lastKey7()` directly.
    void queueKey(uint8_t ascii)  { keyboard_.queueKey(ascii); }
    void clearKeyStrobe()         { keyboard_.clearStrobe(); }

    /// Paste a block of text. Line-endings normalised to CR ($0D) — `\r\n`,
    /// `\r`, `\n` all collapse to one CR. Non-printable controls below
    /// `$20` other than CR / HT are dropped silently. Capped at
    /// `kPasteMaxChars` so a runaway clipboard can't overwhelm the queue.
    /// Bytes are appended to an internal FIFO that drains one byte per
    /// `clearKeyStrobe()` — so the Apple II ROM's strobe-and-poll loop
    /// pulls them out at exactly its own pace, no timing tricks needed.
    /// Returns the number of bytes actually queued (after filtering).
    /// The ][/][+ case-fold is applied iff not in IIe mode.
    static constexpr size_t kPasteMaxChars = pom2::Keyboard::kPasteMaxChars;
    size_t pasteText(const char* data, size_t length) {
        return keyboard_.pasteText(data, length, /*foldToUpper=*/!iieMode);
    }
    size_t pasteText(const std::string& s) { return pasteText(s.data(), s.size()); }

    /// Like pasteText but does NOT filter control bytes — used by tests
    /// that need to drive a launcher's arrow keys (Ctrl-J / Ctrl-K) or
    /// ESC. Bytes are stripped to 7 bits but otherwise pass through
    /// verbatim. Same FIFO + strobe drain as pasteText.
    size_t pasteRawKeys(const char* data, size_t length) {
        return keyboard_.pasteRawKeys(data, length);
    }

    /// How many bytes are still waiting in the paste queue. UI shows this
    /// in the Edit menu so the user knows the paste is
    /// in flight.
    size_t pendingPasteSize() const { return keyboard_.pendingPasteSize(); }
    void   cancelPaste()            { keyboard_.cancelPaste(); }

    // Speaker — the CPU toggles a flip-flop by reading $C030. We expose
    // a counter the audio backend can sample. UI may also subscribe via
    // setSpeakerToggleCallback().
    uint64_t getSpeakerToggleCount() const { return speakerToggles.load(); }
    using SpeakerCallback = void (*)(void* user);
    void setSpeakerCallback(SpeakerCallback cb, void* user) {
        speakerCb = cb; speakerUser = user;
    }

    // Paddle inputs — $C064-$C067 read the 4 paddles, $C061-$C063 read
    // the 3 push-button switches. Game-port reset latch armed by $C070.
    // State + read logic live in pom2::PaddleInputs (`paddles_`); these are
    // thin forwarders kept for the existing call sites.
    void setPaddle(int idx, uint8_t value)   { paddles_.setPaddle(idx, value); }
    void setPaddleButton(int idx, bool down) { paddles_.setPaddleButton(idx, down); }

    // IIe modifier keys. Real //e wires Open Apple to PB0 ($C061),
    // Solid Apple to PB1 ($C062), and Shift (with the SHK jumper set,
    // which is the factory default on enhanced //e) to PB2 ($C063).
    // MAME `apple2e.cpp:1908-1913` honours this. The host keyboard
    // handler can call these from a GLFW key callback when the IIe
    // ROM is loaded; they're OR'd into the joystick button states at
    // read time.
    void setOpenAppleKey  (bool down) { paddles_.setOpenAppleKey(down); }
    void setSolidAppleKey (bool down) { paddles_.setSolidAppleKey(down); }
    void setShiftKey      (bool down) { paddles_.setShiftKey(down); }

    // Reset — returns LC/MMU/IOU/video switches to cold-boot defaults.
    // Does NOT touch RAM. Used by power-on (`coldBoot`), profile apply,
    // and IIe-class warm reset. II/II+ warm/hard reset use
    // `resetSoftSwitchesWarm()` instead so LC + display switches survive
    // (MAME `apple2.cpp:325-331` machine_reset).
    void resetSoftSwitches();

    // Warm reset (Ctrl-Reset / F11). Mirrors MAME's split between
    // `apple2_state::machine_reset` (minimal — only kbd strobe + cnxx
    // tracker; LC/display/MMU SURVIVE) and `apple2e_state::reset_w`
    // (full MMU/IOU/LC reset list). On IIe/IIc/IIc+ this is identical
    // to `resetSoftSwitches()` above; on II/II+ it deliberately leaves
    // the Language Card mode and display switches intact so software
    // that depends on Ctrl-Reset NOT wiping LC RAM-mode (B-3-1) keeps
    // working. (Pre-Theme-7 POM2 ran the full reset on every soft
    // reset, breaking some hot-loaded ProDOS setups on II/II+.)
    void resetSoftSwitchesWarm();

    // Power-cycle helper: wipe user RAM ($0000-$BFFF). Leaves the I/O page,
    // slot ROM area, and main ROM ($D000-$FFFF) intact. In IIe mode also
    // wipes the auxiliary 64 KB bank and the aux LC banks (but never the
    // internal I/O ROM, which is ROM).
    void clearRam();

    // Apple IIe extension. Off by default — call setIIEMode(true) BEFORE
    // loadAppleIIRom() to switch the loader/dispatcher to IIe behaviour.
    // When false, every IIe-specific code path is gated off and the class
    // behaves as a plain II+.
    void setIIEMode(bool on);
    bool isIIE() const                       { return iieMode; }
    uint16_t iieModeFlags() const            { return iieMemMode; }
    const uint8_t* auxData() const           { return aux.data(); }
    uint8_t*       auxDataMutable()          { return aux.data(); }
    const uint8_t* internalIORomData() const { return internalIORom.data(); }

    /// Applied Engineering RamWorks III — IIe aux-slot RAM expansion up
    /// to 8 MB (128 × 64 KB banks). Verbatim port of MAME
    /// src/devices/bus/a2bus/a2eramworks3.cpp. `banks == 1` = stock IIe
    /// 64 KB aux (no RamWorks). Standard tiers: 1 (stock), 4 (256K),
    /// 8 (512K), 16 (1M), 48 (3M), 128 (8M). Clamped to [1, 128].
    /// Only meaningful in IIe mode — call after setIIEMode(true) and
    /// BEFORE loadAppleIIRom. Wipes aux contents and resets current
    /// bank to 0 (MAME `device_reset` line 67).
    void setRamWorksBanks(uint32_t banks);
    uint32_t ramWorksBanks() const { return ramWorksBanks_; }
    uint8_t  ramWorksBank()  const { return ramWorksBank_; }

    // IIe memory mode flags. Bit positions are arbitrary (we don't need to
    // match AppleWin's MF_* layout); they only have to be stable for the
    // life of the process. Tests pin the routing behaviour, not the flag
    // values themselves.
    static constexpr uint16_t MF_80STORE   = 0x0001;  // $C000/01
    static constexpr uint16_t MF_RAMRD     = 0x0002;  // $C002/03
    static constexpr uint16_t MF_RAMWRT    = 0x0004;  // $C004/05
    static constexpr uint16_t MF_INTCXROM  = 0x0008;  // $C006/07
    static constexpr uint16_t MF_ALTZP     = 0x0010;  // $C008/09
    static constexpr uint16_t MF_SLOTC3ROM = 0x0020;  // $C00A/0B
    static constexpr uint16_t MF_80COL     = 0x0040;  // $C00C/0D
    static constexpr uint16_t MF_ALTCHAR   = 0x0080;  // $C00E/0F

    // CPU pacing hook — EmulationController calls this with the cycle
    // count returned by M6502::run() so the paddle RC discharge timer
    // ticks against the real CPU clock instead of wallclock. Forwards to
    // the cassette device too (so its pulse advance stays cycle-aligned).
    // Once per emulated instruction (M6502::step). Only the bookkeeping that
    // genuinely happens every instruction lives here — the cycle counter, the
    // cassette and slot-card fan-outs — and the video-timing part
    // (VBL edge, frame rollover, per-video-frame event publication) is
    // skipped until `vblNextEventCycle_`, the first cycle at which it can
    // have anything to do. advanceCyclesVideo() is the former body,
    // unchanged; it recomputes that threshold on every run. Anyone moving
    // `cycleCounter` or the frame period behind its back must zero the
    // threshold (setCycleCounter / setVideoStandard / snapshot restore do)
    // so the next call re-derives everything from scratch — the slow path
    // already self-heals from an arbitrary jump, the gate must just let it
    // run. Measured at ~15 % of a ][+ banner profile before the split.
    void advanceCycles(int cycles)
    {
        if (cycles <= 0) return;
        cycleCounter += cycles;
        if (cassette) cassetteAdvanceCycles(cycles);
        if (slots.hasActiveCards()) slots.advanceCycles(cycles);
        if (cycleCounter >= vblNextEventCycle_) advanceCyclesVideo();
    }
    uint64_t getCycleCounter() const { return cycleCounter; }
    /// Set the clock. Deliberately NOT invalidating the beam-race event log:
    /// the display tests use this as a plain "put the beam here" primitive and
    /// push events around it. A restore that MOVES the clock has to invalidate
    /// separately — see resetVideoEventLogForClockJump().
    void     setCycleCounter(uint64_t c) { cycleCounter = c; vblNextEventCycle_ = 0; }

    /// Throw away the beam-race event log and re-derive the frame start from
    /// the clock. Every event carries an emuCycle stamp, so moving the clock
    /// BACKWARDS leaves stamps in the future and breaks the non-decreasing
    /// invariant advanceCycles() publishes on: the log then publishes empty
    /// every frame for the whole rewound span — beam-raced effects freeze at
    /// the frame-start state — while the stale tail is carried forward for
    /// ever and grows without bound. loadSnapshotState() has always done this;
    /// a snapshot whose sections happened to put CPU last (section order comes
    /// from the FILE and was never constrained) bypassed it.
    void     resetVideoEventLogForClockJump();

    // ── Snapshot state (de)serialization ────────────────────────────────
    // The main 64 KB (mem) is the caller's "MEM" section; these cover
    // everything else that defines the machine's memory state: aux RAM,
    // Language-Card RAM (main + aux), RamWorks banks, the IIe paging
    // soft-switches (iieMemMode), the LC latch flags, and DisplayState.
    /// Append a self-describing, versioned blob of the extended state.
    void appendSnapshotState(std::vector<uint8_t>& out);
    /// Restore extended state from a blob produced by appendSnapshotState.
    /// Parses defensively (returns false on a malformed/short buffer) and
    /// best-effort on a RamWorks bank-count mismatch.
    bool loadSnapshotState(const uint8_t* data, size_t n);
    /// Restore the main 64 KB honouring writable[] so ROM/I-O regions are
    /// not clobbered (the snapshot records the full 64 KB incl. the ROM
    /// mirror, but only RAM cells should be written back).
    void restoreMainRam(const uint8_t* data, size_t n);

    // $C0xx I/O read tracer (POM2_TRACE_HANG diagnostics). Records recent
    // soft-switch / slot-register read addresses so a frozen poll loop can be
    // identified by WHICH register it spins on — independent of where the code
    // lives or how aux/main RAM is paged at dump time. No-op unless enabled.
    void setIoReadTrace(bool on) { ioReadTrace_ = on; }
    std::string recentIoReadSummary() const;

private:
    void noteIoRead(uint16_t a) {
        if (!ioReadTrace_) return;
        ioReadRing_[ioReadRingPos_ % kIoReadRing] = a;
        ++ioReadRingPos_;
    }
    static constexpr uint32_t kIoReadRing = 128;
    bool     ioReadTrace_   = false;
    uint16_t ioReadRing_[kIoReadRing] = {};
    uint32_t ioReadRingPos_ = 0;

    // Bank-mismatch detector (POM2_TRACE_BANK=1): per-address shadow of the
    // bank the last write went to (+ the value), so a read returning data from
    // a DIFFERENT bank than it was written to is flagged — the signature of
    // the Nox decompressor output landing in the wrong aux/main bank.
    bool                 bankTrace_ = false;
    std::vector<int8_t>  writeBank_;   // -1 none / 0 main / 1 aux, sized $C000
    std::vector<uint8_t> writeVal_;
    void noteBankWrite(uint16_t addr, bool toAux, uint8_t v);
    void checkBankRead(uint16_t addr, bool fromAux, uint8_t v);


    std::array<uint8_t, 0x10000> mem{};       // flat 64 KB RAM/ROM mirror
    std::array<bool,    0x10000> writable{};  // false in ROM regions
    // Write-watchpoint table: one byte per address, empty until the first
    // watch is armed (a session that never debugs allocates nothing).
    static constexpr uint8_t kWatchArmed       = 1u;
    static constexpr uint8_t kWatchWasWritable = 2u;   // shadowed permission
    std::vector<uint8_t> writeWatch_;
    std::size_t          writeWatchCount_ = 0;
    pom2::MemoryWatchSink* watchSink_ = nullptr;
    // Aux shadow (see setAuxShadow): which pages divert, and the byte.
    bool    auxShadowText_ = false;
    bool    auxShadowHgr_  = false;
    uint8_t auxShadowByte_ = 0;
    // Read-watchpoint table, same lifetime rule. `readDivert_` is
    // `readWatchCount_ != 0`; memRead never tests it directly — it is folded
    // into the three derived fast-path bytes below by refreshReadFastFlags().
    std::vector<uint8_t> readWatch_;
    std::size_t          readWatchCount_ = 0;
    bool                 readDivert_ = false;
    // memRead's fast-path gates, each a test the function already made with
    // `readDivert_` folded in (so a read watch costs the hot path nothing):
    //   plainRead_   = (!iieMode || testMode) && !readDivert_
    //   iieFastRead_ = iieMode && !testMode && !bankTrace_ && !readDivert_
    //   romFastRead_ = !iicProfile_ && !readDivert_
    // Every writer of iieMode / testMode / bankTrace_ / iicProfile_ /
    // readDivert_ calls refreshReadFastFlags(); forgetting one does not
    // crash, it silently takes the slow path (or skips a watch) — pinned by
    // `debugger` case 10 and the fast-path parity tests.
    bool                 plainRead_   = true;
    bool                 iieFastRead_ = false;
    bool                 romFastRead_ = true;
    // A card's private address space, or null for the Apple II map. It is
    // folded into the three gates below exactly the way `readDivert_` is, so
    // a foreign bus closes them all and every read falls through to
    // memReadSlow without memRead testing anything new.
    ForeignBus*          foreignBus_ = nullptr;
    // The test BOTH slow paths and memWrite's fast path already made, widened
    // from `testMode` to "testMode or a foreign bus" — the two cases where
    // none of the Apple II decode applies. Same one test, not one more.
    bool                 flatBus_     = false;
    void refreshReadFastFlags() {
        plainRead_   = (!iieMode || testMode) && !readDivert_ && !foreignBus_;
        iieFastRead_ = iieMode && !testMode && !bankTrace_ && !readDivert_
                       && !foreignBus_;
        romFastRead_ = !iicProfile_ && !readDivert_ && !foreignBus_;
    }
    void refreshBusFlags() {
        flatBus_ = testMode || foreignBus_ != nullptr;
        refreshReadFastFlags();
    }
    // Apple II/II+ 16 KB Language Card. $D000-$DFFF has two 4 KB banks;
    // $E000-$FFFF is one shared 8 KB bank. Together with base 48 KB RAM
    // this gives the II+ its ProDOS-required 64 KB.
    std::array<uint8_t, 0x1000> lcBank1{};
    std::array<uint8_t, 0x1000> lcBank2{};
    std::array<uint8_t, 0x2000> lcHigh{};
    // IIe extension. Allocated unconditionally (small) but only consulted
    // by the dispatcher when iieMode is true.
    std::array<uint8_t, 0x10000> aux{};       // auxiliary 64 KB (= RamWorks bank 0)
    std::array<uint8_t, 0x1000>  internalIORom{}; // motherboard $C000-$CFFF
    std::array<uint8_t, 0x1000>  auxLcBank1{};
    std::array<uint8_t, 0x1000>  auxLcBank2{};
    std::array<uint8_t, 0x2000>  auxLcHigh{};

    // RamWorks III backing store. The four `aux*` arrays above are the
    // "currently visible" bank — kept at fixed addresses so Apple2Display
    // can cache the auxData() pointer once. Switching banks memcpys the
    // visible buffers into `ramWorksBacking_[prev]` and the new bank out
    // of `ramWorksBacking_[curr]`. Stride per bank = 64K + 4K + 4K + 8K =
    // 80K. `ramWorksBanks_ == 1` disables the swap path entirely (stock
    // IIe). Layout per slot in backing: [0..0xFFFF]=aux,
    // [0x10000..0x10FFF]=auxLcBank1, [0x11000..0x11FFF]=auxLcBank2,
    // [0x12000..0x13FFF]=auxLcHigh. Total: ramWorksBanks_ × 0x14000.
    static constexpr uint32_t kRamWorksMaxBanks  = 128;        // MAME 8 MB cap
    static constexpr size_t   kRamWorksBankStride = 0x14000;   // 80 KB per bank
    std::vector<uint8_t> ramWorksBacking_;
    uint32_t ramWorksBanks_ = 1;   // 1 = stock 64 KB aux (no RamWorks)
    uint8_t  ramWorksBank_  = 0;   // current bank (MAME m_bank / 0x10000)
    void ramWorksSwapToBank(uint8_t newBank);  // memcpy in/out
    std::vector<uint8_t> characterRom;
    bool charRomLowercase_ = false;   // see charRomHasLowercase()
    // True when `characterRom` holds a full 8 KB two-set part whose active
    // 4 KB half is chosen at runtime by annunciator 2 (see charRomActiveData).
    bool charRomDualBank_  = false;
    std::string lastError;

    // Soft-switch state, guarded by stateMutex. Reads from the UI thread
    // are infrequent (once per frame, in render()).
    mutable std::mutex stateMutex;
    DisplayState display;
    // Beam-racing event log. Recording is continuous (videoEventFrameOpen_
    // starts true); advanceCycles() publishes {frame-start state, events}
    // into the published_* pair at each video-frame boundary. The legacy
    // synchronous bracket (tests) flips legacyEventBracket_ and gates
    // recording with videoEventFrameOpen_ exactly as before publication.
    DisplayState displayAtFrameStart_;            // recording frame
    std::vector<VideoEvent> videoEvents_;         // recording frame
    DisplayState publishedFrameStart_;            // last completed frame
    std::vector<VideoEvent> publishedEvents_;     // last completed frame
    bool videoEventFrameOpen_ = true;
    bool legacyEventBracket_  = false;
    // Start cycle of the video frame whose events were last published by
    // advanceCycles(). Compared against `vblFrameBase_` (same quantity,
    // tracked incrementally) — NOT derived with a division per call.
    uint64_t lastVideoFrameStart_ = 0;
    std::atomic<VideoStandard> videoStandard_{VideoStandard::NTSC};

    void recordVideoEvent(VideoEventKind kind, bool value);
    /// Same as recordVideoEvent but caller already holds stateMutex.
    void pushVideoEventLocked(VideoEventKind kind, bool value);

    // Keyboard latch + host paste FIFO — its own concern now (Keyboard.h).
    // The $C000 hot read is `keyboard_.latchMirror()` (a lock-free atomic
    // republished under Keyboard's own mutex); the cold IIe status reads use
    // `keyboard_.lastKey7()`.
    pom2::Keyboard keyboard_;

    // Speaker.
    std::atomic<uint64_t> speakerToggles{0};
    SpeakerCallback speakerCb = nullptr;
    void*           speakerUser = nullptr;

    // Game port — paddles, buttons and the Open/Solid-Apple + Shift mods,
    // plus the $C070 RC-discharge latch. All of it lives in PaddleInputs now
    // (one concern per file); the $C061-$C067 read path and $C070 forward to
    // `paddles_`, and the snapshot serialises `paddles_.latchCycle()`.
    pom2::PaddleInputs paddles_;
    // Cycle at which the current video frame started. Derived state, kept
    // incrementally by advanceCycles() so the per-instruction VBL check needs
    // no runtime-divisor modulo — see the comment there. Deliberately NOT
    // snapshotted: it is a pure function of cycleCounter and the video
    // standard, and advanceCycles resyncs it in one division whenever it finds
    // the two out of step (which is exactly what a snapshot restore causes).
    uint64_t vblFrameBase_    = 0;
    // Frame period (65 × scanlines) `vblFrameBase_` is currently aligned to.
    // 0 = unaligned, which forces the first advanceCycles() call to derive it.
    // Also NOT snapshotted, for the same reason as the base.
    uint64_t vblFrameCycles_  = 0;
    // First cycle at which advanceCyclesVideo() has work (the VBL edge of the
    // current frame, then its end). 0 = run it on the next call.
    uint64_t vblNextEventCycle_ = 0;
    uint64_t cycleCounter     = 0;       // hand-rolled, see advanceCycles()

    // Cassette: non-owning pointer set by EmulationController.
    CassetteDevice* cassette = nullptr;

    // Dallas DS1216E No-Slot Clock. Non-owning; lives in
    // EmulationController so it survives profile switches (battery-
    // backed RTC keeps state across resets on real hardware).
    pom2::NoSlotClock* noSlotClock_ = nullptr;

    // Speaker + CPU back-pointers for $C030 sub-instruction timestamping.
    SpeakerDevice* speaker = nullptr;
    M6502*         cpu     = nullptr;

    // //c / //c+ on-board IWM controller (non-owning). Mirrors $C0E0-
    // $C0EF accesses for the state machine; see `setIWM`. Lives in
    // EmulationController, attached/detached around profile switches.
    pom2::IWMDevice*    iwmDevice     = nullptr;
    pom2::SmartPortHub* smartPortHub  = nullptr;
    pom2::SmartPortBusPort* externalSmartPort_ = nullptr;
    // Default true: the IWM is authoritative on iicHasAltBank
    // profiles — `$C0EC/ED/EE/EF` reads return what the MAME-faithful
    // state machine produced from POM2's DiskImage flux stream (scaled
    // from LSS-cycle space at `DiskIICard::lssCycle = cpuCycleTotal*2`,
    // see `IWMDevice::nextTransition`). DiskIICard's LSS still
    // observes every access so motor sound / disk-turbo / head-step
    // tracking stay correct. Flip off via `setIWMAuthoritative(false)`
    // or `POM2_IWM_AUTHORITATIVE=0` env var to A/B compare against
    // the slot-bus path during regression bisect.
    bool             iwmAuthoritative = true;

    // Expansion bus — owns plugged cards.
    SlotBus slots;

    // Klaus harness flat-RAM mode. See setTestMode().
    bool testMode = false;

    // Language Card latch state. Reset default is ROM visible, writes
    // protected. Write-enable follows the real prewrite rule: an odd
    // $C08x switch must be accessed twice consecutively before RAM writes
    // to $D000-$FFFF are accepted.
    bool lcReadRam      = false;
    bool lcWriteEnable  = false;
    bool lcBank2Active  = true;
    bool lcPrewrite     = false;

    bool     iieMode      = false;
    uint16_t iieMemMode   = 0;       // OR of MF_* flags

    // //e auto-INTCXROM for the $C800-$CFFF shared expansion window.
    // Separate from the MF_INTCXROM softswitch ($C006/$C007/$C015): real
    // //e hardware sets this flip-flop when a read hits $C300-$C3FF with
    // SLOTC3ROM=off (handing the //e 80-col firmware control over the
    // 2 KB expansion ROM page so its $C800+ continuation routines run),
    // and clears it when a read hits $CFFF (same mechanism that releases
    // the slot expansion-ROM owner). Without this, JSR $C300 lands in the
    // 80-col firmware OK, but the firmware's JMP $C803 / $C87C / $C9B4 /
    // JSR $CD5B reach the slot bus (= $FF empty) and the CPU walks
    // forward through $FF (= BBS7 zp,rel, 3-byte) until it hits ROM data
    // it decodes as JMP indirect through a stale user-RAM vector — BRK
    // in zero RAM, monitor `*` prompt. Verbatim port of MAME
    // `apple2e.cpp:apple2e_state::c300_int_r` / `c800_int_r`. Pinned by
    // `iie_c8xx_smoke_test.cpp`.
    bool intC8Rom = false;

    // Machine-profile strategy for all //c-class memory behaviour: alt
    // firmware $C028 ROMBANK, on-board IWM $C0E0-$C0EF, //c+ MIG windows
    // $CC00/$CE00, forced INTCXROM (no slots), and the alt-firmware
    // override of $C100-$FFFF. Non-null ONLY on //c / //c+ profiles —
    // created/destroyed in loadAppleIIRom from the ROM probes. II/II+/IIe
    // leave it null: a single `if (iicProfile_)` branch on the hot path,
    // zero virtual calls. See `MemoryProfile_IIcClass` + DEV.md § Memory.
    std::unique_ptr<MemoryProfile> iicProfile_;

    // //c-class on-board SmartPort ROM-exposure gate (see setIicSmartPortArmed).
    bool iicSmartPortArmed_ = false;
    /// //c-classe : le flux d'execution est-il "chez" la carte percee ?
    /// Ouvert par un fetch dans sa page $C5xx, referme par tout acces
    /// $C0xx hors de son device-select. Gouverne le perçage $C800-$CFFE.
    bool iicCardWindow_ = false;

    // VBL (vertical-blank) state. Apple II frame = 262 NTSC scanlines
    // × 65 CPU cycles = 17030 cycles (the long-cycle stretch is not
    // modelled here; nominal 17046 cycles/frame is close enough).
    // Visible video = scanlines 0..191; vertical blank = 192..261.
    // `vblIrqMask` (IIe only) gates the IRQ. The pending flag is set
    // when entering VBL with mask on; cleared on $C019 read or $C05A
    // (disable) write. Asserted on the CPU IRQ line via cpu->setIRQ.
    bool vblIrqMask    = false;
    bool vblIrqPending = false;
    bool vblWasActive  = true;       // tracks transition into VBL window

    // IOUDIS — MAME `apple2e.cpp:1224` initialises to `true`. Only IIc
    // and IIc+ honour SET/CLR writes ($C07E/$C07F per MAME `:2569-2587`,
    // plus //c mouse firmware mirrors at $C078/$C079). Read of $C07E
    // returns bit 7 = `ioudis ? 0x80 : 0` (MAME `:2276-2278`). On IIe
    // the writes are no-ops (MAME falls through), but the reset state
    // is shared so $C07E reads are consistent. POM2 used to leave this
    // unmodelled (C-1-3/D-1-2/D-3-2/D-4-1/E-4-3).
    bool ioudis = true;

    // Annunciator output state. AN0..AN3 are toggled by paired soft
    // switches ($C058/9 = AN0, $C05A/B = AN1, $C05C/D = AN2,
    // $C05E/F = AN3 — AN3 lives in `display.an3` because it also
    // drives Le Chat Mauve's FIFO clock). AN2 is NOT decorative: on an
    // 8 KB international character generator it is wired to the ROM's
    // A12, so it selects the live 4 KB font (`charRomBankOffset`).
    // AN0/AN1 have no external sink yet; the state is tracked so a
    // future GameI/O-style pin model can pick it up without
    // restructuring the soft-switch handler.
    // All three are cleared by `resetSoftSwitches` (the 74LS259's /CLR
    // rides the reset line) and carried in the snapshot trailer.
    bool an0 = false;
    bool an1 = false;
    bool an2 = false;

    void markRomRegion(uint16_t lo, uint16_t hi);
    uint8_t softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal);
    uint8_t languageCardSwitchAccess(uint16_t addr, bool isWrite);
    uint8_t languageCardRead(uint16_t addr) const;
    void    languageCardWrite(uint16_t addr, uint8_t value);

    /// "Floating bus" — the byte the video DMA is currently fetching.
    /// On a real Apple II, reading any soft switch that doesn't actively
    /// drive the data lines returns whichever byte the video circuit just
    /// latched off the DRAM. ProDOS / firmware code uses this as a poor-
    /// man's RNG and as the implicit return value of LC bank-select
    /// triggers ($C080-$C08F) and IIe paging triggers ($C001-$C00F that
    /// don't otherwise return state). Approximates the address by
    /// converting `cycleCounter` into a (line, column) pair and applying
    /// the standard text/HGR row-interleave formulas.
    /// Floating bus at the CURRENT CPU read access cycle: `cycleCounter +
    /// the in-flight instruction's cycle count`. For an LDA/CMP/BIT $C0xx the
    /// data fetch is the instruction's last cycle, so this lands the scanner
    /// on the byte the read actually samples — and matches the event log's
    /// `cycleCounter + getCurrentInstructionCycles()` stamp (vs the old
    /// instruction-START byte, ±a few cycles off).
    /// True when a card OTHER than a Le Chat Mauve occupies slot 3, whose
    /// device-select window is $C0B8-$C0BB — the same four addresses the
    /// Eve decodes. An SSC there drives its ACIA control register at
    /// $C0BB, so a serial driver's baud setup would flip the Eve's HGR
    /// Duochrome bit. A Chat Mauve plugged INTO slot 3 must still see its
    /// own registers, hence the type check rather than a bare occupancy
    /// test.
    /// `$C0B0-$C0BF` is slot 3's device-select window AND the Le Chat Mauve
    /// Eve's sixteen switches (the Eve sits in the AUX slot; on a //c there is
    /// no slot 3 at all). On a //e the user can plug both, and an SSC in slot
    /// 3 drives its ACIA registers at exactly these addresses — a serial
    /// driver's `STA $C0BB` would flip the Eve's TXTGREEN and turn the picture
    /// green; a slot-3 Mockingboard hits the same range ($C0BB = VIA #1 ACR).
    /// So the window is forwarded to the card only while slot 3 holds no
    /// FOREIGN card (a Chat Mauve in slot 3 is allowed to answer there).
    bool chatMauveBlockedBySlot3() const;

    uint8_t floatingBus() const;
    /// Floating bus at an explicit absolute cycle (the scanner address math).
    uint8_t floatingBus(uint64_t absCycle) const;

    /// tests/bus_fastpath_test.cpp — differential check of the inline
    /// memRead()/memWrite() fast paths against the slow paths over every
    /// address and paging state. The only reason the slow paths are reachable
    /// from outside.
    friend struct MemoryFastPathProbe;

    /// Everything memRead()'s inline fast path does not handle. This IS the
    /// former body of memRead(), moved wholesale — see the comment on
    /// memRead() for why the split exists.
    uint8_t memReadSlow(uint16_t addr);
    /// The original body of memReadSlow(); memReadSlow() itself is the
    /// read-watch report wrapped around it (§ Read watchpoints). Forced
    /// inline: left to itself the compiler kept this large body out of line
    /// and the extra call measured +1.0 % on the ][+ banner, whose keyboard
    /// poll lives on the slow path (PERFORMANCE § 8.5).
#if defined(_MSC_VER)
    __forceinline uint8_t memReadSlowBody(uint16_t addr);
#else
    __attribute__((always_inline)) uint8_t memReadSlowBody(uint16_t addr);
#endif
    /// Video-timing half of advanceCycles() — see the inline part.
    void    advanceCyclesVideo();
    /// Out-of-line so Memory.h needs only the CassetteDevice forward decl.
    void    cassetteAdvanceCycles(int cycles);
    /// Everything memWrite()'s inline fast path does not handle — the
    /// former body of memWrite(), moved wholesale.
    void    memWriteSlow(uint16_t addr, uint8_t value);

    /// Aux-vs-main routing for $0000-$BFFF reads under //e paging. ONE
    /// definition, shared by memRead()'s inline fast path and iieMemRead()'s
    /// traced path: two copies of this table would be a divergence waiting to
    /// happen, and it decides which 64 KB bank every //e read lands in.
    ///
    ///   $0000-$01FF  ALTZP            → aux else main
    ///   $0200-$03FF  RAMRD            → aux else main
    ///   $0400-$07FF  80STORE on       → PAGE2 picks aux/main; else RAMRD
    ///   $0800-$1FFF  RAMRD            → aux else main
    ///   $2000-$3FFF  80STORE+HIRES on → PAGE2 picks aux/main; else RAMRD
    ///   $4000-$BFFF  RAMRD            → aux else main
    ///
    /// Mutex note (unchanged from iieMemRead): `display.page2` / `display.
    /// hiRes` are read without holding `stateMutex`. Safe in this threading
    /// model — writers (softSwitchAccess, iieHandleSoftSwitch,
    /// resetSoftSwitches) and this reader all run on the CPU worker thread.
    /// TSAN may flag it formally because the writers DO take the mutex; there
    /// is no actual race, and taking a lock per emulated bus cycle would be
    /// ruinous.
    bool iieReadFromAux(uint16_t addr) const
    {
        const bool ramrd = (iieMemMode & MF_RAMRD) != 0;
        if (addr < 0x0200) return (iieMemMode & MF_ALTZP) != 0;
        if (addr >= 0x0400 && addr <= 0x07FF)
            return (iieMemMode & MF_80STORE) ? display.page2 : ramrd;
        if (addr >= 0x2000 && addr <= 0x3FFF)
            return ((iieMemMode & MF_80STORE) && display.hiRes) ? display.page2
                                                                : ramrd;
        return ramrd;
    }

    /// Write-side twin of iieReadFromAux(): same table with RAMWRT in
    /// place of RAMRD (UTAIIe 4-22; MAME `apple2e.cpp` auxbank_update).
    /// Shared by memWrite()'s inline fast path and iieMemWrite().
    bool iieWriteToAux(uint16_t addr) const
    {
        const bool ramwrt = (iieMemMode & MF_RAMWRT) != 0;
        if (addr < 0x0200) return (iieMemMode & MF_ALTZP) != 0;
        if (addr >= 0x0400 && addr <= 0x07FF)
            return (iieMemMode & MF_80STORE) ? display.page2 : ramwrt;
        if (addr >= 0x2000 && addr <= 0x3FFF)
            return ((iieMemMode & MF_80STORE) && display.hiRes) ? display.page2
                                                                : ramwrt;
        return ramwrt;
    }

    // IIe-only routing helpers. Selected per address range based on the
    // current iieMemMode + DisplayState; see the table at the top of the
    // header for the rules.
    uint8_t iieMemRead(uint16_t addr);
    void    iieMemWrite(uint16_t addr, uint8_t value);
    void    iieHandleSoftSwitch(uint16_t addr);
    /// IIe $C013-$C01F status reads. Returns true and fills `out` when
    /// `addr` is a status register; false otherwise (out-of-band on
    /// purpose — every byte value incl. $FE is a legitimate status, so
    /// an in-band sentinel collided with `0x80 | transchar($7E)`).
    bool    iieReadStatus(uint16_t addr, uint8_t& out) const;
};

#endif // POM2_MEMORY_H
