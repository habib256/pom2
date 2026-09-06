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

// WorkstationCard — see the header for the card's map and where every line
// of it came from.

#include "WorkstationCard.h"

#include "ByteIO.h"
#include "CpuClock.h"
#include "Logger.h"

#include <cstring>
#include <fstream>

namespace pom2 {

namespace {
constexpr uint32_t kSnapshotMagic   = 0x57534B31u;  // "WSK1"
constexpr uint8_t  kSnapshotVersion = 2;
} // namespace

WorkstationCard::WorkstationCard(int slot)
    : slot_(slot)
    , ram_(kRamBytes, 0)
{
    scc_.setPclk(kSccClockHz);
    scc_.setRtxc(Scc8530Device::CHAN_A, kSccClockHz);
    scc_.setRtxc(Scc8530Device::CHAN_B, kSccClockHz);
    scc_.setIntCallback([this](bool asserted) {
        sccInt_ = asserted;
        updateIrqLine();
    });
}

WorkstationCard::~WorkstationCard()
{
    // The CPU holds a bare pointer to the Memory; tear down in order, and
    // detach the bus first so nothing can reach a half-destroyed card.
    if (cardMem_) cardMem_->setForeignBus(nullptr);
    cardCpu_.reset();
    cardMem_.reset();
    bus_.reset();
}

bool WorkstationCard::loadRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;

    std::vector<uint8_t> image(kRomBytes, 0);
    f.read(reinterpret_cast<char*>(image.data()),
           static_cast<std::streamsize>(image.size()));
    if (f.gcount() != static_cast<std::streamsize>(image.size())) {
        log().warn("Workstation",
                   "ROM " + path + " is not a 64 KiB dump — card not plugged");
        return false;
    }

    rom_ = std::move(image);
    romLoaded_ = true;
    startCardCpu();
    return true;
}

// The card's CPU is a second `M6502` over a private `Memory` switched into
// foreign-bus mode. `Memory` is what `M6502` reaches memory through, and
// giving the CPU a bus abstraction of its own would have cost the Apple II's
// hot path 7-16 % (PERFORMANCE §§ 8.2 / 8.5) — so the card borrows the
// mechanism that already exists and pays nothing for it.
void WorkstationCard::startCardCpu()
{
    if (!romLoaded_) return;

    bus_     = std::make_unique<Bus>(*this);
    cardMem_ = std::make_unique<Memory>();
    cardMem_->setForeignBus(bus_.get());
    cardCpu_ = std::make_unique<M6502>(cardMem_.get());
    // The card is a 65C02 board: its firmware is full of STZ / BRA / TSB.
    cardCpu_->setCpuMode(M6502::CpuMode::CMOS);
    onReset();
}

void WorkstationCard::onPlug()
{
    if (!romLoaded_)
        log().warn("Workstation",
                   "no firmware loaded — the card is inert (see RomCatalog)");
}

void WorkstationCard::onReset()
{
    if (!cardCpu_) return;

    std::fill(ram_.begin(), ram_.end(), uint8_t{0});
    latch_.fill(0);
    romBase_    = 0x8000;        // the half whose vectors the card boots from
    timerAcc_   = 0;
    timerFlag_  = false;
    sccInt_     = false;
    sccAcc_     = 0;
    postFailed_ = false;
    entropy_    = 0;
    strobes_.clear();
    scc_.reset();

    cardCpu_->hardReset();
    // hardReset takes the vector through the bus, which is already live, so
    // the PC is the dump's own RESET vector ($C000).
    cardCpu_->start();
    started_ = true;
}

// ─────────────────────────────────────────────────────────────────────────
//  The card CPU's bus
// ─────────────────────────────────────────────────────────────────────────

uint8_t WorkstationCard::busRead(uint16_t addr)
{
    if (addr < kRamBytes)  return ram_[addr];
    if (addr < 0x8000)     return ioRead(addr);
    return rom_[romBase_ + (addr - 0x8000)];
}

void WorkstationCard::busWrite(uint16_t addr, uint8_t value)
{
    if (addr < kRamBytes) { ram_[addr] = value; return; }
    if (addr < 0x8000)    { ioWrite(addr, value); return; }
    // $8000-$FFFF is ROM. The firmware never writes there — checked over
    // millions of instructions by `scc8530_workstation_firmware` — so a
    // write here means the map is wrong, not that the guest is being
    // clever. Drop it the way the silicon would.
}

uint8_t WorkstationCard::ioRead(uint16_t addr)
{
    const uint8_t sel = static_cast<uint8_t>((addr >> 8) & 0x0F);
    switch (sel) {
    case 0x5:   // SCC, A1 = A//B and A0 = D//C
        return scc_.readAbDc(static_cast<uint8_t>(addr & 3));
    case 0x2:   // free-running entropy for the LocalTalk backoff
        return entropy_;
    case 0xA:   // five bits wide — the POST checks exactly this
        return static_cast<uint8_t>(latch_[0xA] & 0x1F);
    case 0xB:   // latch + the interval timer's interrupt flag in bit 6
        return static_cast<uint8_t>((latch_[0xB] & 0xBF) |
                                    (timerFlag_ ? 0x40 : 0x00));
    default:
        return latch_[sel];
    }
}

void WorkstationCard::ioWrite(uint16_t addr, uint8_t value)
{
    const uint8_t sel = static_cast<uint8_t>((addr >> 8) & 0x0F);
    switch (sel) {
    case 0x5:
        scc_.writeAbDc(static_cast<uint8_t>(addr & 3), value);
        return;
    case 0xB:
        // The flag lives in the timer, not in the latch, so a read-modify-
        // write cannot smuggle it back in. Bit 0 written high is the
        // acknowledge strobe ($EDC2: LDA #$01 / TRB $7B00 / TSB $7B00).
        latch_[0xB] = static_cast<uint8_t>(value & 0xBF);
        if (value & 0x01) {
            timerFlag_ = false;
            updateIrqLine();
        }
        return;
    case 0xC:
        // ROM bank select. Bit 1 picks the half: the firmware's home value
        // is 0 and the far-call trampoline at $42D1 writes 2.
        latch_[0xC] = value;
        romBase_ = (value & 0x02) ? 0x0000u : 0x8000u;
        return;
    default:
        latch_[sel] = value;
        return;
    }
}

void WorkstationCard::updateIrqLine()
{
    if (!cardCpu_) return;
    // The card's two interrupt sources are wire-OR'd onto its own /IRQ:
    // source 1 is the interval timer, source 2 the SCC. Anything else the
    // handler sees it counts as spurious ($EE07).
    cardCpu_->setIrqLine(1, timerFlag_);
    cardCpu_->setIrqLine(2, sccInt_);
}

// ─────────────────────────────────────────────────────────────────────────
//  Pacing
// ─────────────────────────────────────────────────────────────────────────

void WorkstationCard::setCpuClock(double hz)
{
    if (hz > 0.0) cpuClockHz_ = static_cast<uint64_t>(hz);
}

void WorkstationCard::advanceCycles(int cycles)
{
    if (!cardCpu_ || cycles <= 0) return;

    // The card's 65C02 runs at about the Apple II's own rate, so the host's
    // cycle budget is the card's too. The POST brackets that from above: its
    // 255-byte SCC loopback has a fixed poll budget that only closes below
    // roughly 2 MHz against the 3.6864 MHz chip clock.
    //
    // THE SLICE IS NOT AN OPTIMISATION KNOB. The slot bus hands out ~4096
    // cycles at a time, and running the CPU for all of them before the SCC
    // moves at all makes the chip stand still for four byte-times while the
    // firmware burns its poll budget waiting — the self-test then fails on
    // a timeout that no real card would see. Interleaving at a granularity
    // below one poll iteration (~30 cycles) is what makes the emulated card
    // agree with the emulated chip. Widen it and the POST stops passing,
    // which `workstation_card_smoke` will say.
    int remaining = cycles;
    while (remaining > 0) {
        const int slice = remaining < kSliceCycles ? remaining : kSliceCycles;
        remaining -= slice;

        if (!cardCpu_->isHalted())
            cardCpu_->run(slice);

        // The firmware halts itself at $C174 when its self-test fails.
        // Record that rather than spinning: `postPassed()` is what asks.
        if (cardCpu_->getProgramCounter() == kPostHaltPc) postFailed_ = true;

        // The SCC has its own crystal; scale exactly, remainder kept.
        sccAcc_ += static_cast<uint64_t>(slice) * kSccClockHz;
        scc_.tick(sccAcc_ / cpuClockHz_);
        sccAcc_ %= cpuClockHz_;

        // Interval timer, and the free-running byte the backoff seeds from.
        entropy_ = static_cast<uint8_t>(entropy_ + slice);
        timerAcc_ += slice;
        while (timerAcc_ >= kTimerPeriodCycles) {
            timerAcc_ -= kTimerPeriodCycles;
            if (!timerFlag_) {
                timerFlag_ = true;
                updateIrqLine();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  The Apple II side
// ─────────────────────────────────────────────────────────────────────────

// $Cn00-$CnFF is a window onto the card's RAM page $0200 — read AND write.
// The driver depends on the write half: its own $Cn00 code sets $FB/$FC to
// $Cn00 and then stores through it (`$C80C: STA ($FB),Y`).
uint8_t WorkstationCard::slotRomRead(uint8_t low8)
{
    if (!romLoaded_) return 0xFF;
    return ram_[kSharedPage + low8];
}

void WorkstationCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    if (!romLoaded_) return;
    ram_[kSharedPage + low8] = v;
}

// "ATLK" at $CnF9-$CnFC. This is the card's identity on the Apple II bus:
// it is neither a Pascal 1.1 device nor a ProDOS block device, so software
// — CardCat included — finds it by this signature and not by $Cn05/$Cn07.
bool WorkstationCard::signaturePublished() const
{
    static constexpr uint8_t kAtlk[4] = { 'A', 'T', 'L', 'K' };
    for (int i = 0; i < 4; ++i)
        if (ram_[kSharedPage + kPageSignature + i] != kAtlk[i]) return false;
    return true;
}

uint8_t WorkstationCard::expansionRomRead(uint16_t offset)
{
    if (!romLoaded_) return 0xFF;
    return rom_[kExpansionRomOffset + (offset & 0x07FF)];
}

// $C0n0-$C0nF. The semantics are the one part of this card that the dump
// does NOT settle — see the header. Recording them is what a future session
// needs; inventing a response would be worse than $FF.
uint8_t WorkstationCard::deviceSelectRead(uint8_t low4)
{
    if (strobes_.size() < 4096)
        strobes_.push_back({ low4, 0xFF, false });
    return 0xFF;
}

void WorkstationCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (strobes_.size() < 4096)
        strobes_.push_back({ low4, v, true });
}

// ─────────────────────────────────────────────────────────────────────────
//  Snapshot
// ─────────────────────────────────────────────────────────────────────────

void WorkstationCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    if (!romLoaded_ || !cardCpu_) return;

    byteio::putU32(out, kSnapshotMagic);
    byteio::putU8 (out, kSnapshotVersion);

    // The card's whole volatile state: its RAM, its CPU, its latches, its
    // timer, and the SCC's own blob appended at the end. The ROM is not
    // state.
    out.insert(out.end(), ram_.begin(), ram_.end());

    byteio::putU16(out, cardCpu_->getProgramCounter());
    byteio::putU8 (out, cardCpu_->getAccumulator());
    byteio::putU8 (out, cardCpu_->getXRegister());
    byteio::putU8 (out, cardCpu_->getYRegister());
    byteio::putU8 (out, cardCpu_->getStatusRegister());
    byteio::putU8 (out, cardCpu_->getStackPointer());
    byteio::putU8 (out, cardCpu_->isHalted() ? 1 : 0);

    for (uint8_t v : latch_) byteio::putU8(out, v);
    byteio::putU32(out, static_cast<uint32_t>(romBase_));
    byteio::putU32(out, static_cast<uint32_t>(timerAcc_));
    byteio::putU8 (out, timerFlag_ ? 1 : 0);
    byteio::putU8 (out, entropy_);
    byteio::putU8 (out, postFailed_ ? 1 : 0);
    byteio::putU8 (out, sccInt_ ? 1 : 0);
    byteio::putU64(out, sccAcc_);
    scc_.appendSnapshot(out);
}

void WorkstationCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!romLoaded_ || !cardCpu_ || !data) return;

    // A slot can hold a different card than it did when the snapshot was
    // taken, so a foreign, short or newer blob is ignored rather than
    // trusted — `Reader::has` is what makes every read below safe.
    byteio::Reader r(data, len);
    if (!r.has(5)) return;
    if (r.u32() != kSnapshotMagic) return;
    if (r.u8()  != kSnapshotVersion) return;
    if (!r.has(kRamBytes + 2 + 5 + 1 + 16 + 4 + 4 + 3 + 1 + 8)) return;

    std::memcpy(ram_.data(), data + r.pos, kRamBytes);
    r.pos += kRamBytes;

    cardCpu_->setProgramCounter(r.u16());
    cardCpu_->setAccumulator(r.u8());
    cardCpu_->setXRegister(r.u8());
    cardCpu_->setYRegister(r.u8());
    cardCpu_->setStatusRegister(r.u8());
    cardCpu_->setStackPointer(r.u8());
    cardCpu_->setHalted(r.u8() != 0);

    for (auto& v : latch_) v = r.u8();
    romBase_ = r.u32();
    if (romBase_ != 0x0000 && romBase_ != 0x8000) romBase_ = 0x8000;
    // Clamp: `timerAcc_` is consumed by `while (timerAcc_ >= period)
    // timerAcc_ -= period;`. A restored NEGATIVE value (the field is signed
    // and the blob is a raw u32) never satisfies that test, so the card's
    // 1 ms tick — and with it the LocalTalk timer IRQ — stalled for good.
    // An absurdly large one spins that loop for millions of iterations
    // inside one advanceCycles.
    timerAcc_   = static_cast<int>(r.u32());
    if (timerAcc_ < 0 || timerAcc_ >= kTimerPeriodCycles) timerAcc_ = 0;
    timerFlag_  = r.u8() != 0;
    entropy_    = r.u8();
    postFailed_ = r.u8() != 0;

    sccInt_ = r.u8() != 0;
    // Same shape: the live invariant is `sccAcc_ < cpuClockHz_` (it is a
    // remainder, `sccAcc_ %= cpuClockHz_`). A blob carrying more than that
    // makes the very next `sccAcc_ / cpuClockHz_` hand the SCC a tick count
    // in the billions.
    sccAcc_ = r.u64();
    if (cpuClockHz_ == 0 || sccAcc_ >= cpuClockHz_) sccAcc_ = 0;
    // The chip carries its own blob. If it refuses one — foreign, short,
    // newer — the card is left with a chip its firmware will reprogram
    // rather than with half a restore.
    if (!scc_.restoreSnapshot(data + r.pos, len - r.pos)) {
        scc_.reset();
        sccInt_ = false;
        sccAcc_ = 0;
    }
    updateIrqLine();
}

} // namespace pom2
