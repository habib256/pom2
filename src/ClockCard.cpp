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
#include "SlotRomAsm.h"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <ctime>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// uPD1990AC mode codes carried on C0/C1/C2 (bits 3,4,5 of the $C0n0
// write). MAME `upd1990a.h:76-91` defines the full 16-entry table; the
// parallel uPD1990AC's 3-bit C field reaches codes 0..7 only, so the
// interval timers (MODE_TP_*_INT, enum 8..11) are uPD4990A-serial-only
// and unreachable here.
//
// Code 7 is NOT "TP 4096 Hz". MAME `upd1990a.cpp:198-205` (`stb_w`):
//
//     if (is_serial_mode())  m_c = m_shift_reg[6];
//     else { m_c = m_c_unlatched; if (m_c == 7) m_c = MODE_TEST; }
//
// and `is_serial_mode()` (`:63-67`) is `m_variant == TYPE_4990A &&
// m_c_unlatched == 7`. `a2thunderclock.cpp:94` instantiates a plain
// `UPD1990A(config, m_upd1990ac, 32.768_kHz_XTAL)`, so on THIS card the
// parallel branch always runs and C=7 always means MODE_TEST (enum 15).
// MODE_TP_4096HZ (enum 7) is reachable only through the serial command
// byte of a uPD4990A, which the ThunderClock+ does not have. The shipped
// firmware never writes 7 either — it offers 64/256/2048 Hz only.
constexpr uint8_t kModeRegisterHold = 0x00;
constexpr uint8_t kModeShift        = 0x01;
constexpr uint8_t kModeTimeSet      = 0x02;
constexpr uint8_t kModeTimeRead     = 0x03;
constexpr uint8_t kModeTp64Hz       = 0x04;
constexpr uint8_t kModeTp256Hz      = 0x05;
constexpr uint8_t kModeTp2048Hz     = 0x06;
constexpr uint8_t kModeTest         = 0x07;   // MAME MODE_TEST, not TP 4096 Hz

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

ClockCard::ClockCard(int slotNum, TimeFn fn, bool probeDump)
    : slot(slotNum), timeFn(fn)
{
    buildRom();
    if (probeDump) tryLoadDump();
    onReset();
    // The chip's latched mode starts as REGISTER_HOLD (MAME `upd1990a.cpp`
    // device_start: `m_c = 0`), and REGISTER_HOLD is a 64 Hz TP mode
    // (`:222-227`) — so TP runs. MAME only arms the timer at the first STB,
    // which leaves a freshly started chip with TP off in a mode that drives
    // it. A real card's chip is battery-backed and has been ticking since
    // its NiCd was charged; A2osX's ThunderClock IRQ setup relies on that:
    // it writes $60 (enable + C2) with no STB and waits for 64 Hz ticks that
    // POM2 never produced, so every task that sleeps never woke (2026-10-23).
    setTpRate(kChipXtalHz / 512);
}

std::unique_ptr<ClockCard> ClockCard::makeForTest(int slotNum, TimeFn fn,
                                                  bool probeDump)
{
    return std::unique_ptr<ClockCard>(new ClockCard(slotNum, fn, probeDump));
}

void ClockCard::onReset()
{
    shiftReg.fill(0);
    prevWrite          = 0;
    lastMode           = kModeRegisterHold;
    dataOutStart_      = false;
    dataOutCycles_     = 0;
    // userOffsetActive / userOffsetSeconds deliberately SURVIVE a bus
    // RESET: the uPD1990AC on the ThunderClock+ is battery-backed (the
    // card carries an on-board NiCd; the chip's time counter keeps
    // ticking with the machine powered off — uPD1990AC datasheet, V_BATT
    // operation; mirrored by MAME `upd1990a.cpp` whose device_reset never
    // touches the time counter). Wiping the user-set time here made every
    // Ctrl-Reset / profile switch forget a TIME_SET.
    // RESET disables the CARD's interrupt hardware (manual 5-2 point 2).
    // It does not stop TP: the uPD1990AC has no reset pin and runs from its
    // own battery, so TP keeps the rate it had, as the time counter keeps
    // its time (see above). Stopping TP here left a guest that enables
    // interrupts after a reset with no tick at all.
    irqEnabled_        = false;
    // MAME `upd1990a.cpp:89` `m_testmode = false;` in `device_start`'s
    // init block; the flag is only ever raised by a MODE_TEST latch.
    testMode_          = false;
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
        (dataOut()             ? 0x80 : 0x00) |
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
        // REGISTER_HOLD and TIME_READ start DATA_OUT's free-running pulse
        // (MAME `:226-228`, `:296-304`: the timer is adjusted to fire at
        // once, so the pin toggles on entry).
        if (mode == kModeRegisterHold || mode == kModeTimeRead) {
            dataOutStart_  = !dataOut();
            dataOutCycles_ = 0;
        }
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

    // CLK rising edge — shift the 40-bit register right by one bit, in
    // MODE_SHIFT only (MAME `upd1990a.cpp:393` gates on `m_c == MODE_SHIFT`;
    // in the other modes CLK does nothing to the register). Each byte
    // shifts towards LSB, the next byte's LSB sliding into its MSB, and
    // DATA_IN enters the MSB of the last one. POM2 used to shift in every
    // mode on the belief that ProDOS clocks the bits out in TIME_READ; the
    // card's firmware it calls does not — `$C951` strobes TIME_READ ($18)
    // to latch the time, then SHIFT ($08), before its first CLK.
    if (clkNow && !clkPrev && lastMode == kModeShift) {
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
    // MAME `upd1990a.cpp:210-219`, run before the mode switch: REGISTER_HOLD
    // and the TP-rate modes re-enable the time counter and LEAVE test mode;
    // SHIFT / TIME_SET / TIME_READ leave the flag alone, which is what makes
    // a TEST → SHIFT sequence give a 32 Hz TP instead of the previous rate.
    if (mode == kModeRegisterHold ||
        (mode >= kModeTp64Hz && mode < kModeTest))
        testMode_ = false;
    else if (mode == kModeTest)
        testMode_ = true;

    // TP rate = XTAL / divider. MAME `upd1990a.cpp:313-322` (TP modes) and
    // `:222-228` (REGISTER_HOLD default) — divider values 512/128/16 for
    // 64/256/2048 Hz against the 32.768 kHz crystal. Only these modes touch
    // TP; SHIFT / TIME_SET / TIME_READ leave it running at its prior rate in
    // normal (non-test) mode, matching MAME.
    switch (mode) {
    case kModeRegisterHold: setTpRate(kChipXtalHz / 512); break;   // 64 Hz
    case kModeTp64Hz:       setTpRate(kChipXtalHz / 512); break;   // 64 Hz
    case kModeTp256Hz:      setTpRate(kChipXtalHz / 128); break;   // 256 Hz
    case kModeTp2048Hz:     setTpRate(kChipXtalHz / 16);  break;   // 2048 Hz
    case kModeTest:
        // MAME `:344-359` `case MODE_TEST`: stop the time counter, stop the
        // data-out pulse, arm the internal 1024 Hz test tick — and never
        // touch `m_timer_tp`, so TP keeps whatever rate it already had.
        // POM2 used to read this code as "TP 4096 Hz", giving a rate the
        // real part cannot produce through its parallel C field at all.
        break;
    case kModeShift:
    case kModeTimeRead:
        // MAME `:240-242` (MODE_SHIFT) and `:307-309` (MODE_TIME_READ):
        // while `m_testmode` both program TP to XTAL/1024 = 32 Hz
        // ("32Hz time pulse in testmode"). Outside test mode they leave TP
        // alone, which is what the `default:` below does.
        if (testMode_) setTpRate(kChipXtalHz / 1024);   // 32 Hz
        break;
    default:                /* TP rate unchanged */        break;
    }
}

void ClockCard::setTpRate(int hz)
{
    tpRateHz_   = hz;
    tpAccumQ8_  = 0;
    if (hz <= 0) {
        tpHalfPeriodQ8_ = 0;
        tpLevel_        = false;
        return;
    }
    // The chip toggles TP at 2× the labelled rate, so a half-period (one
    // toggle) is CPU_HZ / (2·hz) emulated cycles. A full period (one rising
    // edge) is the IRQ-worthy event → `hz` IRQs/s.
    //
    // Held in 1/256ths of a cycle: the exact value is fractional (249.69 at
    // 2048 Hz) and rounding it to a whole cycle biased every toggle the same
    // way, so 2048 Hz came out at 2045.45. See the field's comment.
    tpHalfPeriodQ8_ = tpHalfPeriodQ8For(hz);
}

int ClockCard::tpHalfPeriodQ8For(int hz) const
{
    if (hz <= 0) return 0;
    const double q8 = cpuClockHz_ * 256.0 / (2.0 * hz);
    // Never zero: a 0 period would make advanceCycles' `while` never end.
    return q8 < 1.0 ? 1 : static_cast<int>(q8 + 0.5);
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
        tpHalfPeriodQ8_ = tpHalfPeriodQ8For(tpRateHz_);
        if (tpAccumQ8_ > tpHalfPeriodQ8_) tpAccumQ8_ = 0;
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

bool ClockCard::dataOut() const
{
    // MAME `upd1990a.cpp:222-310`: SHIFT and TIME_SET put the register's
    // LSB on the pin; REGISTER_HOLD drives a 1 Hz square wave and TIME_READ
    // a 0.5 Hz one on a uPD1990A (512 Hz in test mode). Any other mode
    // leaves the pin where the previous one put it: the register's LSB.
    double halfPeriod = 0.0;
    if (lastMode == kModeRegisterHold)  halfPeriod = cpuClockHz_ / 2.0;
    else if (lastMode == kModeTimeRead) halfPeriod = testMode_ ? cpuClockHz_ / 1024.0
                                                               : cpuClockHz_;
    if (halfPeriod <= 0.0) return (shiftReg[0] & 0x01) != 0;
    const auto toggles = static_cast<uint64_t>(static_cast<double>(dataOutCycles_) / halfPeriod);
    return dataOutStart_ != ((toggles & 1) != 0);
}

void ClockCard::advanceCycles(int cycles)
{
    if (cycles > 0) dataOutCycles_ += static_cast<uint64_t>(cycles);
    if (cycles <= 0 || tpHalfPeriodQ8_ <= 0) return;
    // int64 for the running sum only: `cycles` can be a whole emulated second
    // in a test harness, and 1022727 × 256 already needs 28 bits before the
    // accumulator is added. The residue left after the loop is < one
    // half-period (≤ ~4.1 M in Q8, at 32 Hz), so the int32 member is safe.
    long long acc = static_cast<long long>(tpAccumQ8_) +
                    static_cast<long long>(cycles) * 256;
    while (acc >= tpHalfPeriodQ8_) {
        acc -= tpHalfPeriodQ8_;
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
    tpAccumQ8_ = static_cast<int>(acc);
}

void ClockCard::buildRom()
{
    rom.fill(0x00);

    // The fallback when roms/thunderclock_u9_v1.3.bin is absent. ProDOS
    // finds the card by the bytes at even offsets 0/2/4/6, copies its own
    // clock driver into the global page, and that driver CALLS THE CARD:
    // `LDA #$A3 / JSR $Cn0B / JSR $Cn08`, then parses $200 (Apple II
    // Technical Note ProDOS #1, "Clock card": the card "deposits an ASCII
    // string in the GETLN input buffer in the form 07,04,14,22,46,57" —
    // month, day of week, date, hour, minute, second). The bytes were read
    // out of the ProDOS kernel on packaging/disks/CHROMABREAK.po. Until
    // 2026-10-20 this page held the signature and an RTS at $Cn08, so a
    // card without its dump read no time at all and $Cn0B ran into the
    // padding.
    //
    // Same entry convention as the Thunderware EPROM: $Cn08 = SEC (read a
    // string), $Cn0B = CLC (write a character — the format; only `#`, the
    // numeric one ProDOS asks for, exists here, so it is accepted and
    // ignored). The read talks to the emulated uPD1990AC exactly as a
    // program would: TIME_READ with an STB pulse loads the time into the
    // shift register, SHIFT mode, then 40 CLK pulses, LSB first —
    // seconds, minutes, hours, date (BCD), then month (binary, high
    // nibble) and day of week. The slot's screen holes $0478+n / $04F8+n
    // hold the byte being assembled and the output position. Ends in
    // REGISTER_HOLD with interrupts off, as the manual says a read leaves
    // the card (5-2).
    const auto dev   = static_cast<uint8_t>(0x80 + 16 * slot);    // $C0n0
    const auto tmp   = static_cast<uint8_t>(0x78 + slot);         // $0478+n
    const auto pos   = static_cast<uint8_t>(0xF8 + slot);         // $04F8+n
    const auto modeRead  = static_cast<uint8_t>(kModeTimeRead << 3);
    const auto modeShift = static_cast<uint8_t>(kModeShift << 3);

    pom2::SlotRomAsm a(rom, slot, "ThunderClock fallback");
    a.region("signature", 0x00, 0x08)
     .emit({ 0x08,               // PHP        — signature
             0xD8,               // CLD
             0x28,               // PLP        — signature
             0xD8,               // CLD
             0x58,               // CLI        — signature
             0x78,               // SEI
             0x70, 0x00 });      // BVS +0     — signature; falls into $Cn08
    a.region("entries", 0x08, 0x10)
     .emit({ 0x38 })             // $Cn08 SEC  — read
     .branch(0xB0, "dispatch")   //       BCS dispatch
     .emit({ 0x18 })             // $Cn0B CLC  — write (the format char)
     .label("dispatch")
     .branch(0xB0, "read")       //       BCS read
     .emit({ 0x60 });            //       RTS  — `#` accepted
    a.region("read", 0x10, 0xC0)
     .emit({ 0x8A, 0x48, 0x98, 0x48,            // TXA PHA TYA PHA
             0xA9, modeRead,                    // LDA #TIME_READ
             0x8D, dev, 0xC0,                   // STA $C0n0
             0x09, kBitStb,                     // ORA #STB
             0x8D, dev, 0xC0,                   // STA $C0n0 — load the time
             0xA9, modeShift,                   // LDA #SHIFT
             0x8D, dev, 0xC0,                   // STA $C0n0
             0x09, kBitStb,                     // ORA #STB
             0x8D, dev, 0xC0,                   // STA $C0n0
             0xA9, modeShift,                   // LDA #SHIFT
             0x8D, dev, 0xC0,                   // STA $C0n0 — STB low
             0xA9, 15,                          // LDA #15   — seconds go last
             0x8D, pos, 0x04 })                 // STA pos
     .label("byte")
     .emit({ 0xA2, 0x08 })                      // LDX #8
     .label("bit")
     .emit({ 0xAD, dev, 0xC0,                   // LDA $C0n0 — DATA_OUT in bit 7
             0x0A,                              // ASL A
             0x6E, tmp, 0x04,                   // ROR tmp
             0xA9, static_cast<uint8_t>(modeShift | kBitClk),
             0x8D, dev, 0xC0,                   // STA $C0n0 — CLK rising
             0xA9, modeShift,
             0x8D, dev, 0xC0,                   // STA $C0n0 — CLK low
             0xCA })                            // DEX
     .branch(0xD0, "bit")                       // BNE bit
     .emit({ 0xAC, pos, 0x04,                   // LDY pos
             0xA9, 0xAC,                        // LDA #','
             0x99, 0xFF, 0x01,                  // STA $01FF,Y — comma before
             0xC0, 0x03 })                      // CPY #3
     .branch(0xF0, "last")                      // BEQ last  — month + weekday
     .emit({ 0xAD, tmp, 0x04,                   // LDA tmp   — a BCD byte
             0x4A, 0x4A, 0x4A, 0x4A,            // LSR ×4
             0x09, 0xB0,                        // ORA #'0'
             0x99, 0x00, 0x02,                  // STA $0200,Y
             0xAD, tmp, 0x04,                   // LDA tmp
             0x29, 0x0F,                        // AND #$0F
             0x09, 0xB0,                        // ORA #'0'
             0x99, 0x01, 0x02,                  // STA $0201,Y
             0x88, 0x88, 0x88,                  // DEY ×3
             0x8C, pos, 0x04 })                 // STY pos
     .branch(0xD0, "byte")                      // BNE byte  — Y ≥ 3 here
     .label("last")
     .emit({ 0xAD, tmp, 0x04,                   // LDA tmp
             0x29, 0x0F,                        // AND #$0F  — weekday 0..6
             0x09, 0xB0,                        // ORA #'0'
             0x8D, 0x04, 0x02,                  // STA $0204
             0xA9, 0xB0,                        // LDA #'0'
             0x8D, 0x03, 0x02,                  // STA $0203
             0xAD, tmp, 0x04,                   // LDA tmp
             0x4A, 0x4A, 0x4A, 0x4A,            // LSR ×4    — month 1..12
             0xA2, 0xB0,                        // LDX #'0'
             0xC9, 0x0A })                      // CMP #10
     .branch(0x90, "units")                     // BCC units
     .emit({ 0xE9, 0x0A,                        // SBC #10   — carry is set
             0xA2, 0xB1 })                      // LDX #'1'
     .label("units")
     .emit({ 0x09, 0xB0,                        // ORA #'0'
             0x8D, 0x01, 0x02,                  // STA $0201
             0x8E, 0x00, 0x02,                  // STX $0200
             0xA9, 0x8D,                        // LDA #CR
             0x8D, 0x11, 0x02,                  // STA $0211
             0xA9, 0x00,                        // LDA #REGISTER_HOLD
             0x8D, dev, 0xC0,                   // STA $C0n0 — IRQs off too
             0x68, 0xA8, 0x68, 0xAA,            // PLA TAY PLA TAX
             0x60 });                           // RTS
    romLayoutError_ = !a.finish();
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
    uint8_t v = 0xFF;
    (void)peekExpansionRom(offset, v);
    return v;
}

bool ClockCard::peekExpansionRom(uint16_t offset, uint8_t& out) const
{
    out = (expansionRomLoaded_ && offset < expansionRom_.size())
        ? expansionRom_[offset] : 0xFF;
    return true;
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
    put32(tpHalfPeriodQ8_);
    put32(tpAccumQ8_);
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
    tpRateHz_       = get32();
    tpHalfPeriodQ8_ = get32();
    tpAccumQ8_      = get32();
    // Untrusted blob: advanceCycles loops `while (accum >= half)`, so a
    // crafted accum near INT_MAX with half == 1 would spin ~2^31 times.
    // Clamp both to the sane range this card can actually produce.
    // 2048, not 4096: the parallel C field cannot select MODE_TP_4096HZ (see
    // the mode-code note at the top of this file), so the fastest rate this
    // card can produce is 2048 Hz.
    if (tpRateHz_ < 0 || tpRateHz_ > 2048) tpRateHz_ = 0;
    // `testMode_` is not a blob field (the 'CLK1' record is fixed-size and
    // predates it). Derive it from the mode the blob DOES carry: a snapshot
    // taken while the chip sat in MODE_TEST comes back in test mode. The one
    // case this loses is TEST followed by a SHIFT/TIME_SET/TIME_READ latch,
    // which leaves the flag up on real silicon (MAME `:210-219` only clears
    // it for REGISTER_HOLD and the TP modes) — unreachable from the shipped
    // ThunderClock+ firmware, which never writes code 7 at all.
    testMode_ = (lastMode == kModeTest);
    if (tpHalfPeriodQ8_ < 0) tpHalfPeriodQ8_ = 0;
    // The half-period is DERIVED (rate + the machine's CPU clock), not
    // independent state — `setCpuClock` re-derives it for exactly this
    // reason. Taking the blob's value verbatim imported the period of the
    // machine the snapshot came from: a snapshot saved on an NTSC profile
    // and loaded on a PAL one (or a rewind frame recorded before a profile
    // switch) left the ThunderClock ticking ~0.7 % off for the session, and
    // a corrupt blob could pair a live 1024 Hz rate with a 1-cycle period.
    // Same derivation as setTpRate/setCpuClock.
    tpHalfPeriodQ8_ = tpHalfPeriodQ8For(tpRateHz_);
    if (tpAccumQ8_ < 0 ||
        (tpHalfPeriodQ8_ > 0 && tpAccumQ8_ > tpHalfPeriodQ8_))
        tpAccumQ8_ = 0;
    tpLevel_            = data[p++] != 0;
    irqEnabled_         = data[p++] != 0;
    irqPending_         = data[p++] != 0;
    // Re-drive the slot IRQ line from the restored request flip-flop:
    // assertIrq() owns the wire-OR bookkeeping in SlotPeripheral.
    assertIrq(irqEnabled_ && irqPending_);
}
