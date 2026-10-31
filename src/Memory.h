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
#include <functional>
#include <atomic>
#include <cassert>
#include <cmath>
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
#include "CpuSocketAccelerator.h"

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
    /// An accelerator in the 6502 SOCKET — the Zip Chip (ZipChip.h).
    /// Non-owning, set by EmulationController; nullptr = stock CPU. It is
    /// consulted only on //e-class machines with slots and on the //c Plus
    /// (activeAccel); EmulationController::installSocketAccelerator picks
    /// which chip that is after a profile switch.
    void setCpuSocketAccelerator(pom2::CpuSocketAccelerator* a) { accel_ = a; }
    pom2::CpuSocketAccelerator* cpuSocketAccelerator() const { return accel_; }
    /// The 6502 clock multiplier the frame loop burns at. Sampled per chunk.
    /// A slot accelerator (TransWarp) DMA-halts the socket CPU and runs its
    /// own, so while it is fast its speed is THE speed; the socket one (a
    /// Zip, the //c+ CGGA) counts only when it is not. The two used to be
    /// multiplied — 3.6 x 4 = ~14x with both fitted.
    double cpuSpeedMultiplier() const
    {
        const double m = slots.cpuSpeedMultiplier();
        if (m > 1.0) return m;
        if (const auto* a = activeAccel()) return m * a->cpuSpeedMultiplier(cycleCounter);
        return m;
    }
    /// Wire the host CPU. Also installs a SlotBus IRQ router so cards
    /// can raise IRQ via `SlotPeripheral::assertIrq()` without each
    /// holding their own M6502*. Pass nullptr to disconnect (the router
    /// is replaced with an empty function so stray assertIrq() calls
    /// from teardown don't dereference a dangling pointer).
    void setCpu(M6502* c);
    /// The wired host CPU (nullptr before `setCpu`). For a card that must
    /// change what that CPU is — the TransWarp's 65C02 substitute.
    M6502* hostCpu() const { return cpu; }

    /// Apple //c / //c+ on-board IWM controller. Non-owning pointer
    /// set by EmulationController. When iicHasAltBank is on, $C0E0-
    /// $C0EF accesses always mirror to this device so its state
    /// machine (MAME `iwm.cpp` port — see `IWMDevice.{h,cpp}`)
    /// evolves in lock-step with the slot-6 DiskIICard's lightweight
    /// IWM-mode shadow.
    /// Also hands it stockCyclesBetween: its drive-disable delay runs on the
    /// crystal, not on the //c+ 4 MHz (defined in MemoryProfile_IIcClass.cpp).
    void setIWM(pom2::IWMDevice* iwm);

    /// When true (default), `$C0E0-$C0EF` reads on iicHasAltBank
    /// profiles return the IWMDevice's value for a selected Sony rather
    /// than the slot-6 DiskIICard's. Its switches are exclusive to that drive.
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
    bool isIIcPlus() const { return iicProfile_ && iicProfile_->isIIcPlus(); }
    bool isIIcClass() const { return iicProfile_ != nullptr; }

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
                                         iicCardWindow_ = false;
                                         irqWindowDepth_ = 0; }
    /// The CPU took an interrupt (IRQ, NMI, BRK) / ran an RTI. A //c's
    /// interrupt handler is internal-ROM code that touches the soft switches
    /// and $C3xx, which closes the armed stub's $C800 window; an interrupt
    /// that lands inside the stub's $C800 code then returned into internal
    /// ROM bytes. The window open at entry is open again at the matching
    /// RTI (bug hunt 2026-10-25). //c-class only; never on the bus path.
    void noteInterruptEntry()
    {
        if (!iicProfile_) return;
        if (irqWindowDepth_ < irqWindowStack_.size())
            irqWindowStack_[irqWindowDepth_] = iicCardWindow_ && !intC8Rom;
        if (irqWindowDepth_ < 0xFF) ++irqWindowDepth_;
    }
    void noteInterruptReturn()
    {
        if (!iicProfile_ || irqWindowDepth_ == 0) return;
        --irqWindowDepth_;
        if (irqWindowDepth_ < irqWindowStack_.size() &&
            irqWindowStack_[irqWindowDepth_] && iicSmartPortArmed_) {
            iicCardWindow_ = true;
            intC8Rom = false;
        }
    }
    bool iicSmartPortArmed() const     { return iicSmartPortArmed_; }
    /// Whether a card on the //c internal expansion connector has been woken
    /// by a write into its page (see iicExpansionAwake_).
    bool iicExpansionAwake() const     { return iicExpansionAwake_; }

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
        // …and DETACHING must un-park it, or the Memory keeps skipping video
        // bookkeeping forever. 0 is the "re-derive on the next advanceCycles"
        // sentinel every other reseat uses (setCycleCounter, setVideoStandard).
        vblNextEventCycle_ = bus ? ~uint64_t{0} : 0;
    }
    ForeignBus* foreignBus() const { return foreignBus_; }

    // ── Bus INH: a slot card that replaces $D000-$FFFF ───────────────────
    // A RAM card in a peripheral slot (the Saturn 128K) pulls the bus's INH
    // line to take $D000-$FFFF from the motherboard ROM and the built-in
    // language card, separately for reads and writes (MAME a2bus
    // `inh_type`: INH_READ / INH_WRITE). One such card at a time. Same rule
    // as the foreign bus: the read side folds into `romFastRead_`, so the
    // ROM fast path tests nothing new, and writes above $D000 are already
    // on the slow path.
    struct LcInhibitor {
        virtual ~LcInhibitor() = default;
        virtual bool    inhibitsRead() const = 0;
        virtual bool    inhibitsWrite() const = 0;
        virtual uint8_t inhRead(uint16_t addr) const = 0;
        virtual void    inhWrite(uint16_t addr, uint8_t value) = 0;
    };
    /// Seat (or, with nullptr, remove) the card. It calls refreshLcInhibit()
    /// whenever its INH state changes.
    void setLcInhibitor(LcInhibitor* card) { lcInhibitor_ = card; refreshLcInhibit(); }
    LcInhibitor* lcInhibitor() const { return lcInhibitor_; }
    void refreshLcInhibit() {
        inhRead_ = lcInhibitor_ && lcInhibitor_->inhibitsRead();
        refreshReadFastFlags();
    }

    // ── A card in the //e auxiliary slot that sits on the aux RAM ───────
    // The Premium SoftCard //e (PremiumSoftCard.h) is the aux memory, and a
    // Z80 shares it. It answers a register at $C07E, and while it says so
    // the 6502's aux accesses are not plain: A15 inverted (to reach the Z80's
    // top 16 KB, which the 6502 sees as $C000-$FFFF), or a status byte read
    // at $2000. Folded like the foreign bus: `auxDivert_` closes
    // `iieFastRead_`, `auxFlip_` joins `bankTrace_` in the one test memWrite's
    // //e fast path already made (`iieWriteSlow_`) — so a //e without the
    // card runs the same instructions as before.
    struct AuxSlotPort {
        virtual ~AuxSlotPort() = default;
        virtual void c07eWrite(uint8_t value) = 0;
        /// 6502 aux accesses see A15 inverted.
        virtual bool flipsA15() const = 0;
        /// A RAMRD read of aux $2000 is the card's (status), not RAM's.
        virtual bool answersStatus(uint8_t& value) const = 0;
        /// Either of the two above may hold — keep the slow path.
        virtual bool divertsAux() const = 0;
        /// A physical cell the card keeps itself instead of the aux array
        /// (nullptr: the aux array's). Slow paths only.
        virtual uint8_t* ownCell(uint16_t /*phys*/) { return nullptr; }
    };
    void setAuxSlotPort(AuxSlotPort* port) { auxPort_ = port; refreshAuxSlotPort(); }
    AuxSlotPort* auxSlotPort() const { return auxPort_; }
    void refreshAuxSlotPort() {
        auxDivert_ = auxPort_ && auxPort_->divertsAux();
        auxFlip_   = auxPort_ && auxPort_->flipsA15();
        iieWriteSlow_ = bankTrace_ || auxFlip_;
        refreshReadFastFlags();
    }
    /// The aux card's RAM by physical address, as its chips hold it: $0000-
    /// $BFFF the aux bank, $C000-$CFFF the aux language card's $D000 bank 1,
    /// $D000-$DFFF its bank 2, $E000-$FFFF its $E000-$FFFF (the //e MMU's
    /// language-card layout). The CPU's current bank (no RamWorks with the
    /// Premium SoftCard: it IS the aux card).
    uint8_t& auxPhysical(uint16_t p) {
        if (auxPort_) if (uint8_t* own = auxPort_->ownCell(p)) return *own;
        if (p < 0xC000) return aux[p];
        if (p < 0xD000) return auxLcBank1[p - 0xC000];
        if (p < 0xE000) return auxLcBank2[p - 0xD000];
        return auxLcHigh[p - 0xE000];
    }

    /// Test/debug accessor for the current floating-bus byte (the value an
    /// undriven soft-switch read returns) at EXACTLY `cycleCounter` — no
    /// in-instruction offset, so a test that sets the cycle gets a
    /// deterministic byte. (The CPU read path uses the access-cycle variant.)
    uint8_t peekFloatingBus() const { return floatingBus(videoNow()); }

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
        if (testMode && plainRead_) return mem[addr];
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
            if (!iieWriteSlow_) {
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
        if (videoWriteLogCovers(addr)) return true;
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

    // ── Video write log: beam-time RAM for the display ──────────────────
    // The scanner fetches RAM DURING the scan (Sather, Understanding the
    // Apple II ch. 3 / IIe ch. 5): a byte written after the beam passed its
    // line shows next frame. The display paints a published frame later,
    // from live RAM, so a double-buffered game that flips mid-frame and
    // starts redrawing the page it just hid showed the band above the flip
    // half-erased (pom2games DIRTBIKE, 2026-10-21). While armed, every CPU
    // write to $0400-$0BFF / $2000-$5FFF (main, and the aux bank the scanner
    // reads) is logged with its cycle, and the painter rewinds RAM to each
    // band's scan time. Same zero-cost diversion as the write watch: arming
    // clears `writable[]` there. Armed while published frames carry
    // soft-switch events, dropped after kVideoLogQuietFrames without one.
    // MemoryVideoLog.cpp.
    bool videoWriteLogCovers(uint16_t addr) const {
        return videoLogArmed_ && ((addr >= 0x0400 && addr < 0x0C00) ||
                                  (addr >= 0x2000 && addr < 0x6000));
    }
    void setVideoWriteLog(bool on);
    bool videoWriteLogArmed() const { return videoLogArmed_; }
    /// Display side, the caller holding the state lock for the whole paint:
    /// begin, then `videoRewindTo(line)` before painting each band (either
    /// direction), then end, which puts RAM back exactly. begin() returns
    /// false and changes nothing when the log does not describe the
    /// published frame (armed too late, overflowed, or RAM moved under it).
    /// `cutFilter` (optional): only writes it accepts get a band of their
    /// own — the display passes "lands in a page this frame shows" for a
    /// frame with no soft-switch event. Every write is still rewound.
    bool videoRewindBegin(const std::function<bool(uint16_t addr, bool aux)>* cutFilter = nullptr);
    void videoRewindTo(int line);
    void videoRewindEnd();
    /// Lines 1..191 at which a logged write changes what the beam fetches.
    const std::vector<uint16_t>& videoRewindCuts() const { return vrCuts_; }
    /// Start cycle of the last published video frame.
    uint64_t videoPublishedFrameStart() const { return videoPublishedStart_; }

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
    struct AppleIIRomImage {
        std::string path;
        std::vector<uint8_t> bytes;
    };
    // Stage once before a profile teardown; installation never rereads a
    // file that may have changed while outgoing media were being saved.
    static bool prepareAppleIIRom(const char* filename, AppleIIRomImage& image,
                                 std::string& error);
    int loadAppleIIRom(const AppleIIRomImage& image, bool pickLower16KFor32K = false);
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

    /// Side-effect-free view of what the CPU would FETCH at `addr`, i.e. the
    /// flat mirror resolved through the live paging state:
    ///
    ///   * `$0000-$BFFF`  IIe aux routing (RAMRD / ALTZP / 80STORE+PAGE2),
    ///                    which is what `data()` gets wrong for anything the
    ///                    guest put in aux — including a RamWorks bank;
    ///   * `$C000-$C0FF`  left as the flat mirror ON PURPOSE. Reading a soft
    ///                    switch is an ACTION (it toggles paging, clears the
    ///                    keyboard strobe, steps the Disk II phases), and a
    ///                    debugger repaint must never perform it;
    ///   * `$C100-$CFFF`  the motherboard internal I/O ROM when INTCXROM /
    ///                    SLOTC3ROM / INTC8ROM (or a //c-class profile) map it
    ///                    there. A window that resolves to a SLOT card falls
    ///                    back to the flat mirror: a card's `slotRomRead` is
    ///                    MMIO for several of them (Mockingboard VIA at
    ///                    $Cs00-$Cs0F, CFFA's IDE task file), so peeking it
    ///                    would have exactly the side effect this API exists
    ///                    to avoid;
    ///   * `$D000-$FFFF`  the language card — bank 1/2, main vs ALTZP aux,
    ///                    or motherboard/`//c` alt-firmware ROM. The
    ///                    NoSlotClock intercept is skipped (it is a stateful
    ///                    magic-sequence detector).
    ///
    /// Callers: the Debugger panel's disassembly, the Memory Viewer's hex
    /// grid, and anything else that wants "what the CPU sees" rather than
    /// "what is in the main bank".
    uint8_t peekCpuView(uint16_t addr) const;
    pom2::DebugBank debugBank(uint16_t addr, bool write = false) const;

    /// Bulk form of peekCpuView() — fills all 64 KiB. `out` must have room
    /// for 0x10000 bytes.
    void snapshotCpuView(uint8_t* out) const;

    /// Side-effect-free peek at the byte a `memWrite(addr, …)` would REPLACE.
    /// Differs from peekCpuView() only in the //e aux table it consults
    /// (RAMWRT rather than RAMRD), which is what makes an undo record taken
    /// from the read view push a main-RAM byte into aux under 80STORE. Above
    /// `$BFFF` it is the read view (the language card writes where it reads,
    /// and I/O has no meaningful "previous value").
    uint8_t peekCpuWriteTarget(uint16_t addr) const;

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
        // Annunciator 0 ($C058 off / $C059 on). Nothing on the motherboard
        // displays it, but a Videx Videoterm's soft video switch does: the
        // card's picture replaces the Apple's while TEXT and AN0 are both
        // on (VidexVideotermCard.h). It lives HERE, not beside AN1/AN2, so
        // the display reads it from the same published per-frame snapshot
        // as every other switch — and so the static-text skip key, which
        // compares this struct wholesale, sees an AN0 flip.
        bool an0 = false;
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
        /// Display-side only, never recorded by Memory: a band boundary the
        /// renderer inserts where a logged video-RAM write changes what the
        /// beam fetches (see videoRewindBegin). Changes no state.
        Cut,
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
    // read path calls `keyboard_.readLatch()` / `lastKey7()` directly.
    /// A live key. The ][ / ][+ keyboard cannot emit $61-$7A, so a-z folds
    /// to A-Z there, as pasteText already did: typing `print 6*7` with the
    /// host's Caps Lock off answered ?SYNTAX ERROR (bug hunt 2026-09-29).
    void queueKey(uint8_t ascii)
    {
        if (!iieMode && ascii >= 'a' && ascii <= 'z')
            ascii = static_cast<uint8_t>(ascii - 'a' + 'A');
        keyboard_.queueKey(ascii);
    }
    void clearKeyStrobe()         { keyboard_.clearStrobe(); }
    void setKeyHeld(uint16_t token, bool down) { keyboard_.setKeyHeld(token, down); }
    /// The //c keyboard's 40/80-column switch, read at $C060 bit 7 (set =
    /// 40 columns). A physical key on the machine, not a soft switch: no
    /// reset touches it. POM2's default is the 80 position.
    void setIic40ColumnSwitch(bool forty) { iic40ColSwitch_ = forty; }
    bool iic40ColumnSwitch() const { return iic40ColSwitch_; }
    void releaseHeldKeys()        { keyboard_.releaseHeldKeys(); }

    /// Paste a block of text. Line-endings normalised to CR ($0D) — `\r\n`,
    /// `\r`, `\n` all collapse to one CR. Non-printable controls below
    /// `$20` other than CR / HT are dropped silently. Capped at
    /// `kPasteMaxChars` so a runaway clipboard can't overwhelm the queue.
    /// Bytes are appended to an internal FIFO; after the strobe is cleared,
    /// the next `$C000` read promotes a byte. The ROM's strobe-and-poll loop
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

    /// A byte stream from a TERMINAL — the SSC's telnet keyboard bridge.
    /// Same FIFO/cap/case-fold as pasteText, but control bytes survive
    /// (Ctrl-C, $08 backspace, ESC) and the CR/LF collapse state persists
    /// across calls, because the caller delivers one byte at a time. See
    /// `pom2::Keyboard::pasteKeyStream`.
    size_t pasteKeyStream(const char* data, size_t length) {
        return keyboard_.pasteKeyStream(data, length, /*foldToUpper=*/!iieMode);
    }

    /// How many bytes are still waiting in the paste queue. UI shows this
    /// in the Edit menu so the user knows the paste is
    /// in flight.
    size_t pendingPasteSize() const { return keyboard_.pendingPasteSize(); }
    void   cancelPaste()            { keyboard_.cancelPaste(); }

    // Speaker — $C030 toggles the flip-flop. The attached SpeakerDevice
    // receives cycle-stamped edges; this counter supports diagnostics.
    uint64_t getSpeakerToggleCount() const { return speakerToggles.load(); }

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
    void setShiftKeyMod   (bool on)   { paddles_.setShiftKeyMod(on); }
    void setControlKey    (bool down) { paddles_.setControlKey(down); }
    void setGamePortConnected(bool on) { paddles_.setConnected(on); }
    void setIiShiftWiring (pom2::PaddleInputs::IiShiftWiring w) { paddles_.setIiShiftWiring(w); }

    // Reset — returns LC/MMU/IOU/video switches to cold-boot defaults.
    // Does NOT touch RAM. Used by power-on (`coldBoot`), profile apply,
    // and IIe-class warm reset. II/II+ warm/hard reset use
    // `resetSoftSwitchesWarm()` instead so LC + display switches survive
    // (MAME `apple2.cpp:325-331` machine_reset).
    /// `warm` = the reset LINE (Ctrl-Reset, F12), not power-on: the
    /// keyboard latch survives it (MAME reset_w leaves m_strobe/m_transchar
    /// alone; only machine_reset clears them). The //c Plus ROM reads $C000
    /// after its reset delay to see whether Esc is held.
    void resetSoftSwitches(bool warm = false);

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
    /// The CPU's aux view: the RamWorks bank it has selected.
    const uint8_t* auxData() const           { return aux.data(); }
    /// The VIDEO's aux view: always RamWorks bank 0, as the scanner fetches
    /// on the real card (MAME a2eramworks3.cpp get_vram_ptr → m_ram[0]).
    /// Bank 0 lives in `aux` while it is selected and in the backing store's
    /// first slot otherwise, so this pointer MOVES on a bank switch —
    /// Apple2Display re-reads it every render() (bug hunt 2026-09-29).
    const uint8_t* videoAuxData() const
    {
        return (ramWorksBanks_ > 1 && ramWorksBank_ != 0)
            ? ramWorksBacking_.data() : aux.data();
    }
    uint8_t*       auxDataMutable()          { return aux.data(); }
    /// Writable twin of videoAuxData(): the bank the screen shows, for a
    /// tool that edits the PICTURE (the DHGR paint host), not the CPU's view.
    uint8_t* videoAuxDataMutable()
    {
        return (ramWorksBanks_ > 1 && ramWorksBank_ != 0)
            ? ramWorksBacking_.data() : aux.data();
    }
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
    /// The beam's clock. The video counters run off the 14.318 MHz crystal
    /// and an accelerator never touches them, so under one the 6502 burns
    /// more cycles per frame while the frame keeps its length. Identity on
    /// a stock machine; EmulationController re-anchors it with each chunk's
    /// ratio (setVideoClockRatio). Off the hot path: advanceCycles' gate
    /// compares CPU cycles against a threshold converted back once.
    uint64_t videoCycleAt(uint64_t cpuCycle) const
    {
        const uint64_t d = cpuCycle - vAnchorCpu_;
        return vAnchorVideo_ + (vRatio_ == 1.0 ? d
            : static_cast<uint64_t>(static_cast<double>(d) * vRatio_));
    }
    uint64_t videoNow() const { return videoCycleAt(cycleCounter); }
    uint64_t videoAccessCycle() const { return videoCycleAt(accessCycle()); }
    uint64_t nextVblCpuCycle(uint64_t cpuNow) const;   // SlotBus::nextVblCpuCycle
    /// Video cycles per CPU cycle from now on (1 / the accelerator's speed).
    void setVideoClockRatio(double r)
    {
        if (!(r > 0.0) || r == vRatio_) return;
        vAnchorVideo_ = videoNow();
        vAnchorCpu_   = cycleCounter;
        vRatio_       = r;
        vblNextEventCycle_ = 0;
    }
    /// The cycle the IN-FLIGHT bus access lands on — `cycleCounter` plus the
    /// current instruction's elapsed cycles. This is the stamp
    /// pushVideoEventLocked puts on every VideoEvent, so a card keeping its
    /// own cycle-stamped log of the SAME accesses (LeChatMauveCard's latch
    /// ring, compared against a VideoEvent's emuCycle) has to read the clock
    /// here: getCycleCounter() is the instruction START and makes the card's
    /// edge look older than the event that recorded it.
    uint64_t accessCycle() const;   // out of line: needs a complete M6502
    /// Set the clock. Deliberately NOT invalidating the beam-race event log:
    /// the display tests use this as a plain "put the beam here" primitive and
    /// push events around it. A restore that MOVES the clock has to invalidate
    /// separately — see resetVideoEventLogForClockJump().
    void     setCycleCounter(uint64_t c)
    { cycleCounter = c; vAnchorCpu_ = vAnchorVideo_ = c; vblNextEventCycle_ = 0; }

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
    /// File-snapshot environment identity (versioned, RAM-only capture).
    /// Configuration and canonical firmware, independent of soft switches,
    /// watchpoints and the currently selected firmware/RamWorks bank.
    void appendSnapshotEnvironment(std::vector<uint8_t>& out) const;
    // Session reconstruction only; ordinary snapshots never install firmware.
    bool installSessionEnvironment(const std::vector<uint8_t>& bytes, std::string& error);
    /// Validate extended state without changing RAM, devices, IRQs, caches or
    /// beam events. Used before applying untrusted file/session snapshots.
    bool validateSnapshotState(const uint8_t* data, size_t n,
                               uint32_t* savedRamWorksBanks = nullptr) const;
    /// Restore extended state from a blob produced by appendSnapshotState.
    /// Refuses malformed/short buffers without changing live state, and is
    /// best-effort on a RamWorks bank-count mismatch — it lifts only the
    /// saved CURRENT bank, so up to 8 MB of the blob is silently dropped.
    /// `savedRamWorksBanks`, when non-null, reports the bank count the blob
    /// was taken with (1 when it carries no RamWorks section) so a caller
    /// that can refuse — restoreMachineState() — does the refusing.
    bool loadSnapshotState(const uint8_t* data, size_t n,
                           uint32_t* savedRamWorksBanks = nullptr);
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
    // Extended-state wire version, shared by capture, apply and preflight.
    static constexpr uint8_t kMemStateBlobVersion = 1;
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
    // Video write log (see videoWriteLogCovers).
    struct VideoWrite { uint64_t cycle; uint16_t addr; uint8_t oldValue, newValue; bool toAux; };
    static constexpr std::size_t kVideoLogCap = std::size_t{1} << 18;
    static constexpr int kVideoLogQuietFrames = 600;
    void noteVideoWrite(uint16_t addr, uint8_t value);
    void onVideoFramePublished(uint64_t frameStart, bool hadEvents);
    void resetVideoWriteLogForClockJump();
    uint8_t* videoWriteTarget(const VideoWrite& w);
    bool     videoLogArmed_      = false;
    uint64_t videoLogValidFrom_  = 0;
    uint64_t videoPublishedStart_ = 0;
    int      videoQuietFrames_   = 0;
    std::vector<VideoWrite> videoLog_;
    bool                  vrActive_  = false;
    std::vector<uint32_t> vrOrder_;    // log indices, stably sorted by cut line
    std::vector<uint16_t> vrCutOf_;    // per log entry
    std::size_t           vrApplied_ = 0;
    std::vector<uint16_t> vrCuts_;
    std::vector<uint8_t>  vrSeen_;
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
    // The INH card, and whether it holds $D000-$FFFF reads now — folded
    // into `romFastRead_` (refreshLcInhibit).
    LcInhibitor*         lcInhibitor_ = nullptr;
    bool                 inhRead_     = false;
    // The aux-slot card and its two folds (refreshAuxSlotPort).
    AuxSlotPort*         auxPort_     = nullptr;
    bool                 auxDivert_   = false;
    bool                 auxFlip_     = false;
    // memWrite's //e fast-path gate: `bankTrace_ || auxFlip_`.
    bool                 iieWriteSlow_ = false;
    // The test BOTH slow paths and memWrite's fast path already made, widened
    // from `testMode` to "testMode or a foreign bus" — the two cases where
    // none of the Apple II decode applies. Same one test, not one more.
    bool                 flatBus_     = false;
    void refreshReadFastFlags() {
        plainRead_   = (!iieMode || testMode) && !readDivert_ && !foreignBus_;
        iieFastRead_ = iieMode && !testMode && !bankTrace_ && !readDivert_
                       && !foreignBus_ && !auxDivert_;
        romFastRead_ = !iicProfile_ && !readDivert_ && !foreignBus_ && !inhRead_;
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
    // The $C000 hot read is `keyboard_.readLatch()` (a lock-free atomic
    // mirror republished under Keyboard's own mutex, which also promotes
    // the next paste byte); the cold IIe status reads use
    // `keyboard_.lastKey7()`.
    pom2::Keyboard keyboard_;

    // Speaker.
    std::atomic<uint64_t> speakerToggles{0};

    // Game port — paddles, buttons and the Open/Solid-Apple + Shift mods,
    // plus the $C070 RC-discharge latch. All of it lives in PaddleInputs now
    // (one concern per file); the $C061-$C067 read path and $C070 forward to
    // `paddles_`, and the snapshot serialises its four 558 timers.
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
    // videoCycleAt's anchor and slope. Not snapshotted: a restore re-anchors
    // to the identity (setCycleCounter), as the clock did before it existed.
    uint64_t vAnchorCpu_   = 0;
    uint64_t vAnchorVideo_ = 0;
    double   vRatio_       = 1.0;
    uint64_t cpuCycleForVideo(uint64_t v) const
    {
        const uint64_t d = v - vAnchorVideo_;
        return vAnchorCpu_ + (vRatio_ == 1.0 ? d
            : static_cast<uint64_t>(std::ceil(static_cast<double>(d) / vRatio_)));
    }

    // Cassette: non-owning pointer set by EmulationController.
    CassetteDevice* cassette = nullptr;

    // Dallas DS1216E No-Slot Clock. Non-owning; lives in
    // EmulationController so it survives profile switches (battery-
    // backed RTC keeps state across resets on real hardware).
    pom2::NoSlotClock* noSlotClock_ = nullptr;

    // Zip Chip seam (CpuSocketAccelerator.h). The helpers run on the
    // $C000-$C0FF slow path only — never on the RAM fast path.
    pom2::CpuSocketAccelerator* accel_ = nullptr;
    pom2::CpuSocketAccelerator* activeAccel() const
    {
        // //e-class machines with slots (a Zip Chip in the socket), and the
        // //c Plus, whose built-in accelerator answers the same registers.
        return (accel_ && iieMode && (!iicProfile_ || iicProfile_->isIIcPlus()))
                   ? accel_ : nullptr;
    }
    uint64_t accelNow() const;
 public:
    /// Stock-clock cycles in the CPU span [from, to): the socket
    /// accelerator's answer, one for one without it.
    /// Stock cycles since the last $C070: the socket accelerator's exact
    /// span, then a slot accelerator's (TransWarp) multiplier on top.
    uint64_t stockSincePaddleStrobe(int idx) const
    {
        return stockSinceCycle(paddles_.latchCycle(idx));
    }
    uint64_t stockSinceCycle(uint64_t latch) const
    {
        // The power-on sentinel sits "after" the counter on purpose and
        // relies on the unsigned wrap (PaddleInputs.h); stockCyclesBetween
        // reads a reversed span as 0, which made every paddle discharge.
        const uint64_t s = latch <= cycleCounter
            ? stockCyclesBetween(latch, cycleCounter) : cycleCounter - latch;
        const double m = slots.cpuSpeedMultiplier();
        return m > 1.0 ? static_cast<uint64_t>(static_cast<double>(s) / m) : s;
    }
    uint64_t stockCyclesBetween(uint64_t from, uint64_t to) const
    {
        if (const auto* a = activeAccel()) return a->stockCyclesBetween(from, to);
        return to > from ? to - from : 0;
    }
 private:
    /// MAME apple2e.cpp:2266-2275 — the Zip's $C05E STATEREG-like byte.
    uint8_t zipStateByte() const
    {
        return static_cast<uint8_t>(
            ((iieMemMode & MF_ALTZP)   ? 0x80 : 0) | ((iieMemMode & MF_RAMRD) ? 0x40 : 0) |
            ((iieMemMode & MF_RAMWRT)  ? 0x20 : 0) | ((iieMemMode & MF_80STORE) ? 0x10 : 0) |
            (display.hiRes ? 0x08 : 0) | (display.page2 ? 0x04 : 0) |
            (lcBank2Active ? 0 : 0x02) | (lcReadRam ? 0 : 0x01));
    }
    /// A read the Zip answers: the motherboard switch still acts unless the
    /// chip says it runs from cache (ReadResult::skipMotherboard).
    bool accelIoRead(pom2::CpuSocketAccelerator& a, uint16_t addr, uint8_t& v)
    {
        const uint8_t low = static_cast<uint8_t>(addr);
        const auto r = a.ioRead(low, accelNow(), low == 0x5E ? zipStateByte() : 0);
        if (!r.answer) return false;
        if (!r.skipMotherboard) (void)softSwitchAccess(addr, false, 0);
        v = r.value;
        return true;
    }

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
    /// Install a staged image; `loadAppleIIRom` wraps it in the slot cards'
    /// before/after hooks (SlotPeripheral::beforeMainRomReload).
    int loadAppleIIRomImpl(const AppleIIRomImage& image, bool pickLower16KFor32K);

    // Klaus harness flat-RAM mode. See setTestMode().
    bool testMode = false;

    // Language Card latch state. These initialisers are the object's; a
    // power-on (resetSoftSwitches) puts ROM read, WRITE-ENABLED, bank 2 —
    // MAME's ramcard16k and AppleWin agree on read/write, and differ only on
    // the bank (MAME 1, AppleWin 2; no primary source found). Write-enable
    // follows the prewrite rule: an odd $C08x switch read twice in a row.
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
    bool iic40ColSwitch_ = false;   // setIic40ColumnSwitch
    /// //c-class internal expansion connector: has the program written into
    /// the card's page yet? Until it has, reads there stay the machine's own
    /// firmware (MAME `m_mockingboard4c`). Cleared at power-on only
    /// (`clearRam`, MAME machine_reset `apple2e.cpp:1292`); Ctrl-Reset
    /// (`reset_w`) leaves the card awake.
    bool iicExpansionAwake_ = false;
    /// //c-classe : le flux d'execution est-il "chez" la carte percee ?
    /// Ouvert par un fetch dans sa page $C5xx, referme par tout acces
    /// $C0xx hors de son device-select. Gouverne le perçage $C800-$CFFE.
    bool iicCardWindow_ = false;
    /// noteInterruptEntry/Return: whether the window was open, per nesting
    /// level (four kept; deeper levels are counted so the pairing holds).
    /// Not snapshotted — a rewind landing inside a handler that interrupted
    /// the stub's $C800 code is the one case it does not cover.
    std::array<bool, 4> irqWindowStack_{};
    uint8_t irqWindowDepth_ = 0;

    // VBL (vertical-blank) state. Apple II frame = 262 NTSC scanlines
    // × 65 CPU cycles = 17030 cycles (the long-cycle stretch is not
    // modelled here; nominal 17046 cycles/frame is close enough).
    // Visible video = scanlines 0..191; vertical blank = 192..261.
    // `vblIrqMask` gates the IRQ — the //c's VBL interrupt ($C05B enable,
    // $C05A disable, decoded while IOUDIS is clear). The pending flag is set
    // when entering VBL with the mask on and cleared by $C070-$C07F or
    // $C05A — NOT by the $C019 read (Apple IIc Technical Note #9; MAME
    // c000_iic_r case 0x19). It drives the CPU's IRQ_SRC_VBL line.
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
    // AN0 drives a Videx Videoterm's soft video switch (see
    // DisplayState::an0); AN1 has no external sink yet — it is tracked so a
    // future GameI/O-style pin model can pick it up.
    // All three are cleared by `resetSoftSwitches` (the 74LS259's /CLR
    // rides the reset line) and carried in the snapshot trailer. AN0 is
    // `display.an0` since 2026-09-29 (a Videx Videoterm watches it).
    bool an1 = false;
    bool an2 = false;

    void markRomRegion(uint16_t lo, uint16_t hi);
    uint8_t softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal);
    uint8_t languageCardSwitchAccess(uint16_t addr, bool isWrite);
    uint8_t languageCardRead(uint16_t addr) const;
    /// The RAM cell `languageCardWrite` would touch — bank 1/2 at $D000, the
    /// shared 8 KB at $E000, main or the ALTZP aux trio. Shared by
    /// `languageCardRead`'s RAM half and `peekCpuWriteTarget`.
    uint8_t languageCardRamPeek(uint16_t addr) const;
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
