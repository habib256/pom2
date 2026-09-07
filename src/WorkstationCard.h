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

// WorkstationCard — the Apple II Workstation Card, the board that put a IIe
// on LocalTalk so it could netboot from an AppleShare server and reach the
// LaserWriters on the same net.
//
// IT IS A COPROCESSOR, NOT A ROM CARD. That is the whole reason this file
// looks nothing like GrapplerCard. On board: a **65C02 of its own**, 28 KiB
// of its own RAM, a **Zilog 8530 SCC** doing LocalTalk's SDLC, an interval
// timer, and 64 KiB of ROM banked into a 32 KiB window. The Apple II never
// executes that firmware; it talks to the card through a shared RAM page and
// a handful of `$C0nX` strobes. So POM2 runs a second `M6502` here, over the
// card's own address space, through `Memory::ForeignBus`.
//
// EVERY LINE OF THE MAP BELOW WAS READ OUT OF THE DUMP, then confirmed by
// running the firmware — the derivation is in `docs/printer_plan_2.md` § 5.2
// and pinned by `scc8530_workstation_firmware`. It is written here as the
// card's specification, with the evidence, because there is no MAME oracle
// for this board: MAME has no Workstation Card at all.
//
//   $0000-$6FFF  RAM (28 KiB). The reset routine sizes it by writing
//                $40 $41 $42 $43 to $0000/$2000/$4000/$6000 and reading
//                them back — the classic aliasing probe.
//   $0200-$02FF  ...of which this page is ALSO what the Apple II sees at
//                $Cn00 (see below).
//   $7000        Status / progress code. The POST writes $03, $0B, $0F, $07
//                at its phase boundaries and $04 just before halting.
//   $7200        Read-only entropy. `$DBCC: LDY $7200` feeds an eight-`ROL`
//                scramble that seeds LocalTalk's random backoff, so it wants
//                to be free-running, not constant.
//   $7500-$7503  Zilog 8530 SCC, wired A1 = A//B and A0 = D//C (MAME's
//                `ab_dc` ordering). `$EE13: LDA #$03 / STA $7502 /
//                LDA $7502` polls RR3, which exists in channel A only.
//   $7800-$7900  Eight-bit read/write latches; the POST checks the readback.
//   $7A00        A **five**-bit latch: the POST writes $FF with a `DEC` and
//                then compares against `#$1F` ($F131-$F139).
//   $7B00        Latch, plus the interval timer: **bit 6 reads as the timer
//                interrupt flag** and writing bit 0 = 1 acknowledges it
//                (`$EDBD: BIT $7B00 / BVC` … `LDA #$01 / TRB / TSB`).
//   $7C00        **ROM bank select**, bit 1 picking the 32 KiB half. Proved
//                by the trampoline the firmware relocates into RAM at
//                $42D1: `LDA $40BB / STA $7C00 / JSR $CC32 / LDA $029A /
//                STA $7C00` — a far call that has to switch the ROM out
//                from under itself, which is exactly why it lives in RAM.
//   $8000-$FFFF  ROM, 32 KiB window over the 64 KiB dump. The power-on half
//                is the upper one (its vector table at file $FFFA gives
//                RESET $C000, and $C000 is the RAM-sizing probe).
//
// THE APPLE II SIDE.
//
//   $Cn00-$CnFF  A read/write **window onto the card's RAM page $0200**, not
//                a ROM page. Its published layout, read off a running card:
//                entry points from `$Cn14` (each `LDY #cmd` then a branch to
//                the common body at `$Cn36`), the node address at `$CnF0`,
//                and **`ATLK` at `$CnF9-$CnFC`** — the signature guest
//                software identifies the card by, since it is neither a
//                Pascal 1.1 device nor a ProDOS block device. The driver proves it: the $Cn00 code sets
//                `$FB/$FC` to $Cn00 and then does `STA ($FB),Y` — it writes
//                into its own firmware page. What the host reads there is
//                what the card put there, copied out of ROM at boot
//                (`$C0DA: LDX #$E3 / LDA $C3D9,X / STA $01FF,X`, i.e. ROM
//                $C3DA-$C4BC into RAM $0200-$02E2). The correspondence is
//                checkable: the driver reads `$Cn9A` and the card's own
//                code uses `$029A`.
//   $C800-$CFFF  Expansion ROM, served from the dump at file $C800. The
//                $Cn00 page enters it with `JMP $CC00` after the usual
//                `STA $07F8` / `LDX $CFFF` dance.
//   $C0n0-$C0nF  Strobes between the two CPUs. Reads answer $FF and writes
//                are recorded by `hostStrobeLog()` — and **a driver call
//                completes anyway**, so whatever they do, this path does not
//                need it. The host writes $71 to `$C080,X`/`$C081,X` from
//                the page and hands a byte to `$C080,X` at `$CnB8` (X from
//                `$CnEB`, the byte from `$CnEA`); the card firmware reads no
//                register that could receive any of it and has no interrupt
//                path for it (`$EE07` counts anything that is neither SCC
//                nor timer as spurious). Left unmodelled deliberately: there
//                is nothing left to explain with them, and inventing a
//                behaviour would be worse than $FF.
//
// THE HANDSHAKE, as far as it is decoded. Read this before touching
// `deviceSelectWrite`; it is the difference between finishing the card and
// guessing at it.
//
//   * **Detection.** Guest software scans slots for `ATLK` at `$CnF9-$CnFC`
//     and then reads `$CnFD` as a version byte — $01 or $02. `ATINIT` does
//     exactly that at its `$31B4`. POM2's card passes.
//   * **The driver entry is `$Cn14`**, called ProDOS-MLI style:
//     `JSR $Cn14 / .BYTE command / .WORD parameter-block`. ATINIT's first
//     call is command **$42**. (The entry table starts at `$Cn14` and each
//     entry loads its own command into Y before branching to the common
//     body at `$Cn36`.)
//   * **The two CPUs rendezvous by rewriting code the other is executing.**
//     This is the part that looks impossible until you see it: the host
//     spins inside the shared page on `$Cn89: 18 90 EB` — `CLC` then a
//     branch back — and the CARD releases it by writing `$38` (`SEC`) into
//     `$0289`, so the branch stops being taken and the host falls through
//     to `$Cn8C: JMP $CC00`. The mirror image runs at `$02A9`: the card
//     writes `$38` there and then waits (`$D577-$D583`) until it reads
//     `$18`, which is the `CLC` opcode of the host's own spin loop.
//   * **`$02EE` is the request byte** (`$CnEE` to the host). The card polls
//     it from its idle loop (`$DAB2: JSR $D016`):
//         $D016: LDA $02EE / BMI      ; bit 7 = the host wants service
//         $D021: BIT $02EE / BVC $D021 ; then WAIT for bit 6
//         $D031: STZ $02EE            ; consume it
//         $D034: AND #$0F             ; low nibble = the command
//         $D045: LDA $02DB / CMP #$42 ; ...and $42 is ATINIT's call
//   * **The card steers the host by PATCHING the host's code.** This is the
//     part that makes the page make sense. Scanning every absolute write the
//     card firmware makes into `$02xx`: it writes `$CnBB`/`$CnBC` fourteen
//     times each — those are the operand bytes of the host's
//     `$CnBA: JMP $Cn95` — and `$CnC3`/`$CnC4`/`$CnC6`/`$CnC7` four times
//     each, which are the address operands of the host's block-move loop
//     `$CnC2: LDA $FFFF,X / STA $FFFF,X` (shipped with `FF FF` placeholders).
//     So the card decides where the host jumps and what it copies, by
//     rewriting the instructions before releasing it.
//   * **Data slots.** The card writes `$CnEA` (the byte the host then hands
//     back through `$C080,X`), `$CnE8`, `$CnEC`, `$CnED`, `$CnE6`; it reads
//     `$CnDB` (the command — `$42` for ATINIT), `$CnDC`/`$CnDD` (the
//     parameter-block pointer the host wrote), `$CnE4`, `$CnE9`, `$CnF1`.
//     A card-to-host byte stream looks like `STA $02EA / LDA #$38 /
//     STA $02A9` — put the byte, then release the host's spin.
//   * **`$0289` is written exactly once in the whole firmware** (`$D01E`),
//     and never restored to `$18`. So the first rendezvous is a one-shot per
//     transaction, not a repeating gate — which is why a host that keeps
//     re-entering the page is a symptom of the transaction never completing,
//     not of a missing restore.
//   * **Bit 6 of `$02EE` was a red herring, and the story is worth
//     keeping.** The card waits on `BIT $02EE / BVC` for a bit the host's
//     page code never writes, which looked like proof that the `$C08x`
//     strobe must set it. Wiring that did move the card one step further —
//     which made it look righter still. It was not: the real fault was the
//     `$C800-$CFFF` window being based $400 too high, so the host's
//     `JMP $CC00` landed on a block-copy loop instead of the driver
//     prologue. With the base corrected the whole transaction completes
//     **with no strobe modelling at all**. The lesson: a change that moves a
//     stuck system one step is not evidence that it is correct.
//
// WHAT THIS COSTS. The card runs a second 6502 at the Apple II's own rate,
// so plugging it roughly doubles the emulation work. That is what a
// coprocessor card is; it is not a defect to optimise away.

#ifndef POM2_WORKSTATION_CARD_H
#define POM2_WORKSTATION_CARD_H

#include "M6502.h"
#include "Memory.h"
#include "Scc8530Device.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pom2 {

class WorkstationCard : public SlotPeripheral
{
public:
    /// The card's crystal, derived rather than assumed: the firmware ends up
    /// with WR12/WR13 = 6 and WR4's x1 clock, and 3686400 / (6 + 2) / 2 is
    /// exactly 230400 — the LocalTalk bit rate.
    static constexpr uint32_t kSccClockHz = 3686400;
    /// Size of the dump the card needs.
    static constexpr std::size_t kRomBytes = 0x10000;
    /// Card RAM, $0000-$6FFF.
    static constexpr std::size_t kRamBytes = 0x7000;
    /// Where the Apple II's $Cn00 window lands in that RAM.
    static constexpr uint16_t kSharedPage = 0x0200;
    /// Offsets in the dump of the two host-visible slices.
    /// Where the host's `$C800-$CFFF` expansion ROM lives in the dump.
    /// Determined by experiment, not by reading: with the card plugged and a
    /// `JSR $Cn14 / .BYTE $42 / .WORD block` call driven at it, **0xC400 is
    /// the only base at which the transaction completes** — the host returns
    /// past its inline parameters, `$CnDB` receives the command byte and
    /// both semaphores come back to rest. Nine candidates were swept
    /// (0xC000, 0xC200, 0xC3DA, 0xC400, 0xC4BD, 0xC4DA, 0xC600, 0xC800,
    /// 0xCC00); the other eight deadlock. Note it overlaps the tail of the
    /// `$Cn00` page image at 0xC3DA-0xC4BC, which is fine: the host enters
    /// this window at `$CC00` (file 0xC800), where the prologue
    /// `CLD / PHP / SEI / LDA #$50 / STA $C080,X` lives.
    static constexpr std::size_t kExpansionRomOffset = 0xC400;

    explicit WorkstationCard(int slot);
    ~WorkstationCard() override;

    WorkstationCard(const WorkstationCard&) = delete;
    WorkstationCard& operator=(const WorkstationCard&) = delete;

    /// Load the 64 KiB firmware (Apple 341-0358-A). Returns false — and
    /// leaves the card inert — if the file is missing or the wrong size.
    /// Without it the card CPU has nothing to run, so `plug` sites should
    /// treat a false return as "do not plug".
    bool loadRom(const std::string& path);
    bool romLoaded() const { return romLoaded_; }

    // ─── SlotPeripheral ──────────────────────────────────────────────────
    std::string_view name() const override { return "Apple II Workstation Card"; }
    int getSlot() const { return slot_; }

    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead (uint8_t low8) override;
    void    slotRomWrite(uint8_t low8, uint8_t v) override;
    uint8_t expansionRomRead (uint16_t offset) override;
    /// The Workstation Card's $C800 page is real firmware, so it drives
    /// /IOSTB — MAME `a2bus.h:145` `take_c800()` (default false, overridden
    /// true by every card with an expansion ROM, e.g. `a2ssc.cpp:50`).
    bool takesC800() const override { return true; }

    void onPlug()  override;
    void onReset() override;
    void advanceCycles(int cycles) override;
    void setCpuClock(double hz) override;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Inspection (tests, debug panel) ─────────────────────────────────

    /// Where the card's own CPU is.
    uint16_t cardPc() const { return cardCpu_ ? cardCpu_->getProgramCounter() : 0; }
    bool cardHalted() const { return cardCpu_ && cardCpu_->isHalted(); }
    /// The firmware's POST error accumulator at $0100; 0 means every test
    /// passed. It halts at $C174 when a bit survives its `AND #$EF` mask.
    uint8_t postErrors() const { return ram_[0x0100]; }
    static constexpr uint16_t kPostHaltPc = 0xC174;
    /// True once the card has run far enough to have finished its self-test
    /// without reporting a fault.
    bool postPassed() const { return started_ && postErrors() == 0 && !postFailed_; }

    uint8_t cardRam(uint16_t addr) const
    { return addr < kRamBytes ? ram_[addr] : 0; }
    const Scc8530Device& scc() const { return scc_; }
    Scc8530Device& scc() { return scc_; }
    /// Which 32 KiB half of the dump is currently at $8000-$FFFF.
    bool romHighHalf() const { return romBase_ == 0x8000; }
    /// The $7000 progress code the firmware last wrote.
    uint8_t statusCode() const { return latch_[0x0]; }

    // ─── What the card publishes to the Apple II ─────────────────────────
    // The shared page carries more than the driver image; these are the two
    // fields guest software actually looks at, and they are how the card is
    // found at all.

    /// Offsets inside the `$Cn00` window, read out of the published page.
    static constexpr uint8_t kPageNodeId   = 0xF0;  ///< LocalTalk node address
    static constexpr uint8_t kPageSignature = 0xF9; ///< four bytes: "ATLK"

    /// True once the card has published its driver page — i.e. once the
    /// `ATLK` signature guest software identifies it by is on the bus.
    bool signaturePublished() const;

    /// The LocalTalk node address the card acquired, or 0 before it has one.
    /// The same value ends up in the SCC's WR6 as its SDLC address filter.
    uint8_t localTalkNode() const
    { return ram_[kSharedPage + kPageNodeId]; }

    /// The host `$C0nX` accesses seen so far, oldest first, as
    /// `(register, value, isWrite)`. Empty until the guest touches them, and
    /// capped so a polling guest cannot grow it without bound. This exists
    /// because the strobe semantics are NOT established; it is the evidence a
    /// future session needs, not a feature. Written by the CPU worker, so a
    /// UI reader takes `stateMutex` like any other emulated state.
    struct HostStrobe { uint8_t reg; uint8_t value; bool write; };
    const std::vector<HostStrobe>& hostStrobeLog() const { return strobes_; }

private:
    /// The card CPU's address space. Handed to its private `Memory` through
    /// `Memory::ForeignBus`, which costs the Apple II's own bus nothing —
    /// see Memory.h § Foreign bus.
    struct Bus final : Memory::ForeignBus {
        explicit Bus(WorkstationCard& o) : owner(o) {}
        uint8_t read(uint16_t addr) override  { return owner.busRead(addr); }
        void write(uint16_t addr, uint8_t v) override { owner.busWrite(addr, v); }
        WorkstationCard& owner;
    };

    uint8_t busRead(uint16_t addr);
    void    busWrite(uint16_t addr, uint8_t value);
    uint8_t ioRead(uint16_t addr);
    void    ioWrite(uint16_t addr, uint8_t value);
    void    updateIrqLine();
    void    startCardCpu();

    int slot_;
    bool romLoaded_ = false;
    bool started_   = false;
    bool postFailed_ = false;

    std::vector<uint8_t> rom_;              ///< the 64 KiB dump
    std::vector<uint8_t> ram_;              ///< card RAM, $0000-$6FFF
    std::size_t romBase_ = 0x8000;          ///< which half is at $8000

    std::array<uint8_t, 16> latch_{};       ///< the $7x00 selects

    // Interval timer. The period is NOT established by the dump; 1 ms is
    // what the firmware was driven with when it completed its boot, and its
    // own prescalers (/$25, /$38, /$30 at $EDD5-$EDF4) are consistent with a
    // tick in that range. Change it only with evidence.
    static constexpr int kTimerPeriodCycles = 1020;
    /// How finely `advanceCycles` interleaves the card CPU with the SCC and
    /// the timer. Below one poll iteration of the firmware's self-test
    /// loop — see the note in advanceCycles for why this is correctness,
    /// not tuning.
    static constexpr int kSliceCycles = 24;
    int  timerAcc_   = 0;
    bool timerFlag_  = false;
    /// Free-running counter behind $7200, the LocalTalk backoff entropy.
    uint8_t entropy_ = 0;

    bool sccInt_ = false;
    uint64_t sccAcc_ = 0;                   ///< PCLK accumulator
    uint64_t cpuClockHz_ = 1020000;

    std::unique_ptr<Bus>    bus_;
    std::unique_ptr<Memory> cardMem_;
    std::unique_ptr<M6502>  cardCpu_;
    Scc8530Device scc_;

    std::vector<HostStrobe> strobes_;
};

} // namespace pom2

#endif // POM2_WORKSTATION_CARD_H
