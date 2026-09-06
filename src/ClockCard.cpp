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

#include "ClockCard.h"

#include "CpuClock.h"
#include "Logger.h"
#include "ResourcePaths.h"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <ctime>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// uPD1990AC mode codes carried on C0/C1/C2 (bits 3,4,5 of the $C0n0
// write). MAME `upd1990a.h:58-73` defines the full 16-entry table; the
// parallel uPD1990AC's 3-bit C field reaches modes 0..7 only. We react to
// the first four plus the four TP (Timing-Pulse) rates below; the interval
// timers (modes 8..15) are uPD4990A-serial-only and unreachable here.
constexpr uint8_t kModeRegisterHold = 0x00;
[[maybe_unused]] constexpr uint8_t kModeShift = 0x01;
constexpr uint8_t kModeTimeSet      = 0x02;
constexpr uint8_t kModeTimeRead     = 0x03;
constexpr uint8_t kModeTp64Hz       = 0x04;
constexpr uint8_t kModeTp256Hz      = 0x05;
constexpr uint8_t kModeTp2048Hz     = 0x06;
constexpr uint8_t kModeTp4096Hz     = 0x07;

// $C0n0 write-side bit positions.
constexpr uint8_t kBitDataIn    = 0x01;
constexpr uint8_t kBitClk       = 0x02;
constexpr uint8_t kBitStb       = 0x04;
constexpr uint8_t kBitIrqEnable = 0x40;   // bit 6 — ThunderClock+ INTERRUPT
                                          // CONTROL REGISTER (manual 5-1)

// uPD1990AC crystal — 32.768 kHz (a2thunderclock.cpp:73). TP rates are
// derived as XTAL / divider; see programTpTimer().
constexpr int kChipXtalHz = 32768;

uint8_t toBcd(int n)
{
    if (n < 0) n = 0;
    if (n > 99) n = 99;
    return static_cast<uint8_t>(((n / 10) << 4) | (n % 10));
}

int bcdToInt(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

// Reentrant broken-down-time conversion. `std::localtime` hands back a
// pointer to a single process-wide `tm`, and this card runs on the CPU
// thread (deviceSelectWrite → latchTimeToShiftReg / commitTimeSetFromShiftReg)
// while the UI thread converts times of its own; the guest would then read
// back somebody else's `tm` — for a card carrying a TIME_SET offset, a
// different time than the one it asked for. Same split as
// `NoSlotClock::defaultTimeFn`.
std::tm localTimeR(std::time_t t)
{
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::tm hostLocalTime()
{
    return localTimeR(std::time(nullptr));
}

}  // namespace

ClockCard::ClockCard(int slotNum) : ClockCard(slotNum, &hostLocalTime) {}

ClockCard::ClockCard(int slotNum, TimeFn fn) : slot(slotNum), timeFn(fn)
{
    buildRom();
    tryLoadDump();
    onReset();
}

std::unique_ptr<ClockCard> ClockCard::makeForTest(int slotNum, TimeFn fn)
{
    return std::unique_ptr<ClockCard>(new ClockCard(slotNum, fn));
}

void ClockCard::onReset()
{
    shiftReg.fill(0);
    prevWrite          = 0;
    lastMode           = kModeRegisterHold;
    // userOffsetActive / userOffsetSeconds deliberately SURVIVE a bus
    // RESET: the uPD1990AC on the ThunderClock+ is battery-backed (the
    // card carries an on-board NiCd; the chip's time counter keeps
    // ticking with the machine powered off — uPD1990AC datasheet, V_BATT
    // operation; mirrored by MAME `upd1990a.cpp` whose device_reset never
    // touches the time counter). Wiping the user-set time here made every
    // Ctrl-Reset / profile switch forget a TIME_SET.
    // RESET disables interrupts (manual 5-2 point 2) and stops the TP
    // timer — the real chip doesn't program m_timer_tp until an STB
    // latches a TP/REGISTER_HOLD mode.
    irqEnabled_        = false;
    setTpRate(0);
    clearIrqRequest();
}

std::tm ClockCard::effectiveTime() const
{
    std::tm host = timeFn();
    if (!userOffsetActive) return host;
    // Compose `host + offset` via std::mktime / localtime_r so the
    // BCD bytes that come out the shift register reflect the user's
    // set time + elapsed real seconds since the set point. Mirrors
    // MAME's internal time counter which ticks at 1 Hz from
    // `m_timer_clock` once TIME_SET commits.
    std::time_t hostT = std::mktime(&host);
    if (hostT == static_cast<std::time_t>(-1)) return host;
    hostT += userOffsetSeconds;
    return localTimeR(hostT);
}

void ClockCard::latchTimeToShiftReg()
{
    const std::tm lt = effectiveTime();
    // tm_wday: 0=Sun..6=Sat — matches uPD1990AC's day-of-week field
    // layout. ProDOS doesn't consult day-of-week; we still populate it
    // so the chip's output is faithful for code that does.
    const int dow   = (lt.tm_wday >= 0 && lt.tm_wday <= 6) ? lt.tm_wday : 0;
    const int month = lt.tm_mon + 1;        // tm_mon is 0..11
    shiftReg[0] = toBcd(lt.tm_sec);
    shiftReg[1] = toBcd(lt.tm_min);
    shiftReg[2] = toBcd(lt.tm_hour);
    shiftReg[3] = toBcd(lt.tm_mday);
    // shiftReg[4]: high nibble = month (4-bit binary, NOT BCD — MAME
    // upd1990a.cpp:95 stores month as plain 1..12 in the high 4 bits),
    // low nibble = day-of-week. Months 1..12 all fit in 4 bits.
    shiftReg[4] = static_cast<uint8_t>((month << 4) | (dow & 0x0F));
    // No year byte: the uPD1990A(C) has no year counter and the
    // ThunderClock firmware never clocks a 6th byte (see the 40-bit note
    // in the CLK handler). shiftReg[5] stays out of the shift path.
    shiftReg[5] = 0;
}

void ClockCard::commitTimeSetFromShiftReg()
{
    // MAME `upd1990a.cpp:194-225` — MODE_TIME_SET: copy shift_reg into
    // the chip's internal time_counter, then `set_time()`. POM2 has no
    // separate time_counter (we synthesise BCD bytes from `timeFn()`
    // each read), so we instead decode shiftReg → time_t and store the
    // delta vs the current host clock. Future reads compose
    // `timeFn() + userOffsetSeconds` to produce the running set-time.
    std::tm desired{};
    desired.tm_sec  = bcdToInt(shiftReg[0]);
    desired.tm_min  = bcdToInt(shiftReg[1]);
    desired.tm_hour = bcdToInt(shiftReg[2]);
    desired.tm_mday = bcdToInt(shiftReg[3]);
    desired.tm_mon  = ((shiftReg[4] >> 4) & 0x0F) - 1;     // 1..12 → 0..11
    // The chip carries NO year (40-bit register: sec/min/hour/day/
    // month+dow). Take the year from the host clock — that is what a real
    // ThunderClock+ does, since ProDOS only ever reads month/day/time
    // from it and supplies its own year.
    {
        std::tm hostNow = timeFn();
        desired.tm_year = hostNow.tm_year;
    }
    desired.tm_wday = shiftReg[4] & 0x0F;
    desired.tm_isdst = -1;     // let mktime decide

    const std::time_t desiredT = std::mktime(&desired);
    std::tm hostTm = timeFn();
    const std::time_t hostT = std::mktime(&hostTm);
    if (desiredT == static_cast<std::time_t>(-1)
        || hostT == static_cast<std::time_t>(-1)) {
        // Malformed input — leave the offset alone so a botched
        // TIME_SET doesn't poison subsequent reads.
        return;
    }
    userOffsetSeconds = desiredT - hostT;
    userOffsetActive  = true;
}

uint8_t ClockCard::deviceSelectRead(uint8_t low4)
{
    // DATA_OUT in bit 7, the "interrupt asserted" flag in bit 5, all other
    // bits zero. The host driver does `BIT $C0n0 / BMI ...` to test
    // DATA_OUT, and `LDA $C080,Y / AND #$20` to test the IRQ flag (manual
    // 5-3). MAME `a2thunderclock.cpp:112-115` ignores the low 4 bits of
    // `offset` and mirrors DATA_OUT across all of $C0n0-$C0nF, so a probe
    // at $C0n5 sees the same bit 7 as $C0n0; we mirror the IRQ flag the
    // same way. Sample the flag BEFORE the access clears the request.
    (void)low4;
    const uint8_t out = static_cast<uint8_t>(
        ((shiftReg[0] & 0x01) ? 0x80 : 0x00) |
        (irqPending_           ? 0x20 : 0x00));
    // Any read or write of the card clears a pending request (manual 5-2
    // point 3 + the `LDA $C088,Y / LDA $C080,Y` clear sequence at 5-1).
    clearIrqRequest();
    return out;
}

void ClockCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Any device-select access clears a pending interrupt request (manual
    // 5-2 point 3) — applies to every offset, before the $C0n0-only decode.
    clearIrqRequest();

    if (low4 != 0) return;

    // $C0n0 bit 6 is the INTERRUPT CONTROL REGISTER enable latch: writing
    // $40 enables interrupts, any write with bit 6 clear disables them
    // (manual 5-1 / 5-2). The clock-read/write driver code always issues
    // bit-6-clear writes, which is why "interrupts are left disabled after
    // you read or wrote the THUNDERCLOCK" (manual 5-2). Enable is
    // independent of STB, so a bare $40 write arms IRQs without disturbing
    // the latched chip mode / TP rate.
    irqEnabled_ = (v & kBitIrqEnable) != 0;

    const bool clkPrev = (prevWrite & kBitClk) != 0;
    const bool clkNow  = (v         & kBitClk) != 0;
    const bool stbPrev = (prevWrite & kBitStb) != 0;
    const bool stbNow  = (v         & kBitStb) != 0;
    const uint8_t mode = static_cast<uint8_t>((v >> 3) & 0x07);

    // STB rising edge — latch the mode on C0/C1/C2.
    //   MODE_TIME_READ: load host time (+ optional user offset) into
    //                   shiftReg so the next 48 CLK pulses serialise it.
    //   MODE_TIME_SET : commit the shift register (loaded by the prior
    //                   48 CLK pulses in MODE_SHIFT) as the user-set
    //                   time. MAME `upd1990a.cpp:194-225`.
    //   MODE_SHIFT / MODE_REGISTER_HOLD: no per-edge action — the chip
    //                   just remembers the mode.
    if (stbNow && !stbPrev) {
        lastMode = mode;
        if (mode == kModeTimeRead) {
            latchTimeToShiftReg();
        } else if (mode == kModeTimeSet) {
            commitTimeSetFromShiftReg();
        }
        // (Re)program the TP timer for the freshly-latched mode. The chip
        // reprograms TP on STB just like the read/set/shift functions
        // (MAME `upd1990a.cpp:174-257`).
        programTpTimer(mode);
    }

    // CLK rising edge — shift the 48-bit register right by one bit.
    // Each byte shifts towards LSB, with the LSB of the next byte
    // sliding into the MSB of the previous one. The MSB of the last
    // byte gets the DATA_IN bit (bit 0 of $C0n0). After the shift,
    // DATA_OUT (= shiftReg[0] LSB) carries the next bit out.
    //
    // Deliberate divergence from MAME `upd1990a.cpp:312-327`, which
    // gates this shift behind `m_c == MODE_SHIFT`. POM2 keeps the shift
    // lax (any mode) because ProDOS's hardcoded ThunderClock driver
    // pulses CLK while still in MODE_TIME_READ without first switching
    // to MODE_SHIFT — strict gating breaks stock ProDOS reads, and the
    // ThunderClock+ card paired with a real uPD1990AC has been
    // observed to permit this shortcut in practice (the MODE bit just
    // selects DATA_OUT path / time-counter clocking; the shift register
    // is wired to CLK directly). DATA_IN is still latched into the MSB
    // because MODE_TIME_SET expects it.
    if (clkNow && !clkPrev) {
        // 40-BIT register, not 48. MAME `upd1990a.cpp` shifts
        // `max_shift = is_serial_mode() ? 6 : 5` bytes, and
        // `is_serial_mode()` requires TYPE_4990A — a plain UPD1990A (what
        // `a2thunderclock.cpp` instantiates) is ALWAYS 5 bytes. Confirmed
        // against the shipped firmware: roms/thunderclock_u9_v1.3.bin's
        // nibble routine at $CACF emits 4 CLK pulses and the time-read
        // path calls it 10× = 40 pulses over sec/min/hour/day/month+dow —
        // there is no year field on this card. Shifting 48 bits made
        // MODE_TIME_SET land one byte out of alignment (every field slid:
        // a set of 23:58:59 Dec-31 read back as 00:59:58 day 23), and the
        // clock committed the garbage silently.
        const uint8_t dataIn = (v & kBitDataIn) ? 1 : 0;
        for (int i = 0; i < 4; ++i) {
            shiftReg[i] = static_cast<uint8_t>(
                (shiftReg[i] >> 1) | ((shiftReg[i + 1] & 0x01) << 7));
        }
        shiftReg[4] = static_cast<uint8_t>(
            (shiftReg[4] >> 1) | (dataIn << 7));
    }

    prevWrite = v;
}

void ClockCard::programTpTimer(uint8_t mode)
{
    // TP rate = XTAL / divider. MAME `upd1990a.cpp:248-257` (TP modes) and
    // `:176-181` (REGISTER_HOLD default) — divider values 512/128/16/8 for
    // 64/256/2048/4096 Hz against the 32.768 kHz crystal. Only these modes
    // touch TP; SHIFT / TIME_SET / TIME_READ leave it running at its prior
    // rate in normal (non-test) mode, matching MAME.
    switch (mode) {
    case kModeRegisterHold: setTpRate(kChipXtalHz / 512); break;   // 64 Hz
    case kModeTp64Hz:       setTpRate(kChipXtalHz / 512); break;   // 64 Hz
    case kModeTp256Hz:      setTpRate(kChipXtalHz / 128); break;   // 256 Hz
    case kModeTp2048Hz:     setTpRate(kChipXtalHz / 16);  break;   // 2048 Hz
    case kModeTp4096Hz:     setTpRate(kChipXtalHz / 8);   break;   // 4096 Hz
    default:                /* TP rate unchanged */        break;
    }
}

void ClockCard::setTpRate(int hz)
{
    tpRateHz_      = hz;
    tpAccumCycles_ = 0;
    if (hz <= 0) {
        tpHalfPeriodCycles_ = 0;
        tpLevel_            = false;
        return;
    }
    // The chip toggles TP at 2× the labelled rate, so a half-period (one
    // toggle) is CPU_HZ / (2·hz) emulated cycles, rounded to nearest. A
    // full period (one rising edge) is the IRQ-worthy event → `hz` IRQs/s.
    tpHalfPeriodCycles_ = static_cast<int>(
        (cpuClockHz_ + hz) / (2.0 * hz));
}

void ClockCard::setCpuClock(double hz)
{
    if (hz <= 0.0) return;
    cpuClockHz_ = hz;
    // Re-derive DIRECTLY from the cached rate, not by replaying
    // programTpTimer(lastMode): that function's `default:` leaves the rate
    // untouched, so if the last latched mode was SHIFT / TIME_SET /
    // TIME_READ the armed timer kept its stale NTSC-derived period —
    // exactly contradicting this function's contract.
    if (tpRateHz_ > 0) {
        tpHalfPeriodCycles_ = static_cast<int>(
            (cpuClockHz_ + tpRateHz_) / (2.0 * tpRateHz_));
        if (tpAccumCycles_ > tpHalfPeriodCycles_) tpAccumCycles_ = 0;
    }
}

void ClockCard::clearIrqRequest()
{
    // Drop the request flip-flop and release the slot IRQ contribution.
    // The enable latch is intentionally left intact so a periodic TP
    // source re-asserts on the next rising edge (a clock/scheduler IRQ
    // handler clears by reading the card, then keeps receiving ticks).
    if (irqPending_) {
        irqPending_ = false;
        assertIrq(false);
    }
}

void ClockCard::advanceCycles(int cycles)
{
    if (cycles <= 0 || tpHalfPeriodCycles_ <= 0) return;
    tpAccumCycles_ += cycles;
    while (tpAccumCycles_ >= tpHalfPeriodCycles_) {
        tpAccumCycles_ -= tpHalfPeriodCycles_;
        const bool rising = !tpLevel_;     // level is about to go 0 → 1
        tpLevel_ = !tpLevel_;
        // The ThunderClock+ clocks its interrupt-request FF on the TP
        // rising edge; the FF drives the wire-OR'd slot IRQ while the
        // $C0n0 enable latch is set. assertIrq() is idempotent, so holding
        // the level across repeated edges (until a device-access clear)
        // costs nothing.
        if (rising && irqEnabled_) {
            irqPending_ = true;
            assertIrq(true);
        }
    }
}

void ClockCard::buildRom()
{
    rom.fill(0xEA);     // NOP padding everywhere we don't write

    // ProDOS clock-detection signature. ProDOS only inspects the bytes at
    // even offsets 0/2/4/6; the odd-offset fillers form a benign 1-byte
    // execution path. ProDOS itself never JMPs to $Cs00 for clock reads
    // — it copies its hardcoded driver into RAM (~$D742) and patches
    // $BF06-$BF08 to jump there — so the body past the signature is
    // dead code in practice. We still leave a `BRK; RTS` at $Cs08 in
    // case stray firmware ever does call here, to fail loud rather than
    // wander into NOP-padded territory.
    rom[0x00] = 0x08;   // PHP        — signature
    rom[0x01] = 0xD8;   // CLD        — 1-byte filler
    rom[0x02] = 0x28;   // PLP        — signature
    rom[0x03] = 0xD8;   // CLD
    rom[0x04] = 0x58;   // CLI        — signature (briefly enables IRQ)
    rom[0x05] = 0x78;   // SEI        — re-disable IRQ immediately
    rom[0x06] = 0x70;   // BVS        — signature
    rom[0x07] = 0x00;   // BVS operand: branch +0 → $Cs08 either way
    rom[0x08] = 0x60;   // RTS — minimal "call did nothing" trap
}

void ClockCard::tryLoadDump()
{
    // Probe order: prefer the explicit Rev 1.3 dump from
    // markadev/AppleII-RevEng/Thunderware-Thunderclock-Plus/
    // Thunderware_REV_1.3_ROM_U9.bin; accept a few common aliases so users
    // who renamed by chip number or revision still drop the file in roms/.
    static const char* kCandidates[] = {
        "roms/thunderclock_u9_v1.3.bin",
        "roms/thunderclock_u9.bin",
        "roms/thunderclock.rom",
        "roms/Thunderware_REV_1.3_ROM_U9.bin",
    };
    std::string resolved;
    for (const char* c : kCandidates) {
        std::string r = pom2::findResource(c);
        if (!r.empty()) { resolved = std::move(r); break; }
    }
    if (resolved.empty()) return;

    std::ifstream f(resolved, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> bytes(0x801);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(f.gcount()));

    // Accept 256 B (slot ROM only) or 2 KB (slot ROM + $C800 expansion).
    // Anything else is almost certainly the wrong file — reject so a
    // mis-named dump can't silently break ProDOS detection.
    if (bytes.size() != 256 && bytes.size() != 0x800) {
        pom2::log().warn("Clock",
            "ThunderClock ROM " + resolved + " has unexpected size " +
            std::to_string(bytes.size()) +
            " B (expected 256 or 2048) — using synthetic ROM");
        return;
    }

    // Sanity-check the ProDOS detection signature at offsets 0/2/4/6.
    // Without these the card is invisible to ProDOS regardless of source,
    // so a dump that lacks them is a poor substitute for the synth ROM.
    if (bytes[0] != 0x08 || bytes[2] != 0x28 ||
        bytes[4] != 0x58 || bytes[6] != 0x70) {
        pom2::log().warn("Clock",
            "ThunderClock ROM " + resolved +
            " missing $08/$28/$58/$70 ProDOS signature — using synthetic ROM");
        return;
    }

    std::copy_n(bytes.begin(), 256, rom.begin());
    if (bytes.size() == 0x800) {
        // 2 KB Thunderware EPROM: the same 2 KB chip is decoded into BOTH
        // the slot ROM window ($CnXX = first 256 B of the chip) AND the
        // shared expansion-ROM window ($C800-$CFFF = the entire 2 KB).
        // Mirroring the full chip into expansionRom_ keeps the firmware's
        // own JMP $C8nn continuations working — they expect to find their
        // own slot-ROM bytes at $C800-$C8FF.
        std::copy(bytes.begin(), bytes.end(), expansionRom_.begin());
        expansionRomLoaded_ = true;
    }
    romFromDump_ = true;
    romSource_   = resolved;
    pom2::log().info("Clock", "Loaded ThunderClock+ ROM dump: " + resolved);
}

uint8_t ClockCard::expansionRomRead(uint16_t offset)
{
    if (!expansionRomLoaded_) return 0xFF;
    if (offset >= expansionRom_.size()) return 0xFF;
    return expansionRom_[offset];
}

// ── Snapshot / rewind ─────────────────────────────────────────────────────

namespace {
constexpr uint8_t kClkSnapMagic[4] = { 'C', 'L', 'K', '1' };
constexpr size_t  kClkSnapBytes    = 4 + 6 + 2 + 1 + 8 + 4 + 4 + 4 + 1 + 2;
}

void ClockCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    auto put32 = [&](int32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    };
    out.insert(out.end(), kClkSnapMagic, kClkSnapMagic + 4);
    out.insert(out.end(), shiftReg.begin(), shiftReg.end());
    out.push_back(prevWrite);
    out.push_back(lastMode);
    out.push_back(userOffsetActive ? 1 : 0);
    const uint64_t off = static_cast<uint64_t>(userOffsetSeconds);
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(off >> (8 * i)));
    put32(tpRateHz_);
    put32(tpHalfPeriodCycles_);
    put32(tpAccumCycles_);
    out.push_back(tpLevel_ ? 1 : 0);
    out.push_back(irqEnabled_ ? 1 : 0);
    out.push_back(irqPending_ ? 1 : 0);
}

void ClockCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (data == nullptr || len < kClkSnapBytes ||
        std::memcmp(data, kClkSnapMagic, 4) != 0)
        return;   // foreign blob — a different card sat in this slot
    size_t p = 4;
    auto get32 = [&]() -> int32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[p++]) << (8 * i);
        return static_cast<int32_t>(v);
    };
    std::memcpy(shiftReg.data(), data + p, shiftReg.size()); p += shiftReg.size();
    prevWrite        = data[p++];
    lastMode         = data[p++];
    userOffsetActive = data[p++] != 0;
    uint64_t off = 0;
    for (int i = 0; i < 8; ++i) off |= static_cast<uint64_t>(data[p++]) << (8 * i);
    userOffsetSeconds   = static_cast<std::time_t>(off);
    tpRateHz_           = get32();
    tpHalfPeriodCycles_ = get32();
    tpAccumCycles_      = get32();
    // Untrusted blob: advanceCycles loops `while (accum >= half)`, so a
    // crafted accum near INT_MAX with half == 1 would spin ~2^31 times.
    // Clamp both to the sane range this card can actually produce.
    if (tpRateHz_ < 0 || tpRateHz_ > 4096) tpRateHz_ = 0;
    if (tpHalfPeriodCycles_ < 0) tpHalfPeriodCycles_ = 0;
    // The half-period is DERIVED (rate + the machine's CPU clock), not
    // independent state — `setCpuClock` re-derives it for exactly this
    // reason. Taking the blob's value verbatim imported the period of the
    // machine the snapshot came from: a snapshot saved on an NTSC profile
    // and loaded on a PAL one (or a rewind frame recorded before a profile
    // switch) left the ThunderClock ticking ~0.7 % off for the session, and
    // a corrupt blob could pair a live 1024 Hz rate with a 1-cycle period.
    // Same derivation as setTpRate/setCpuClock.
    if (tpRateHz_ > 0) {
        tpHalfPeriodCycles_ = static_cast<int>(
            (cpuClockHz_ + tpRateHz_) / (2.0 * tpRateHz_));
    } else {
        tpHalfPeriodCycles_ = 0;
    }
    if (tpAccumCycles_ < 0 ||
        (tpHalfPeriodCycles_ > 0 && tpAccumCycles_ > tpHalfPeriodCycles_))
        tpAccumCycles_ = 0;
    tpLevel_            = data[p++] != 0;
    irqEnabled_         = data[p++] != 0;
    irqPending_         = data[p++] != 0;
    // Re-drive the slot IRQ line from the restored request flip-flop:
    // assertIrq() owns the wire-OR bookkeeping in SlotPeripheral.
    assertIrq(irqEnabled_ && irqPending_);
}
