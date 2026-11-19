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

// TestPortCard — the paravirtual TEST PORT (ARCH-017).
//
// Not a piece of Apple hardware, and it says so: no card ever sold sat at
// these addresses with this meaning. It is a write-only port a 6502 program
// uses to talk to the HOST that runs it, so `pom2_headless` can run 6502 unit
// tests on a complete Apple II — ROM, slots, DOS, the real memory map — and
// return their verdict as a process exit status. sim65 runs the same kind of
// test with no I/O at all; AppleWin merged a "debugger breakpoint card" for
// the same purpose on 2025-10-18 (PR #1441). This is POM2's.
//
// THE WHOLE DEVICE — three device-select registers, writes only:
//
//   $C0n0  LOG     one character of a log line. Bit 7 is ignored (Apple text
//                  and ASCII both work); CR or LF ends the line, CR LF counts
//                  once. The finished line reaches the host's sink.
//   $C0n1  EXIT    end the run with this byte as the exit status. A pending
//                  partial log line is delivered first.
//   $C0n2  ASSERT  0 = the assertion failed: the pending partial line (the
//                  message, if the program wrote one) is reported as
//                  "ASSERT #k FAILED: …" and the run ends with status
//                  kAssertFailedStatus. Any other value passes and counts.
//
// After EXIT (or a failed ASSERT) the port is spent: further writes are
// ignored, so whatever the program does on its way to the host noticing —
// the host polls between frames — cannot add lines or change the status.
//
// READS drive nothing: every $C0nX read is the floating bus, exactly what an
// empty slot answers. So the port is invisible to anything that probes the
// slot, and a machine without the option reads the same bytes as before.
//
// The card is OFF unless asked for (`pom2_headless --test-port <slot>`).
// Nothing about it is saved in a snapshot: it belongs to the run, not to the
// machine.

#ifndef POM2_TEST_PORT_CARD_H
#define POM2_TEST_PORT_CARD_H

#include "SlotPeripheral.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class TestPortCard : public SlotPeripheral
{
public:
    static constexpr uint8_t kLog    = 0x0;
    static constexpr uint8_t kExit   = 0x1;
    static constexpr uint8_t kAssert = 0x2;

    /// The exit status of a run that ended on a failed assertion.
    static constexpr int kAssertFailedStatus = 2;

    using LineSink = std::function<void(const std::string&)>;

    explicit TestPortCard(int slot);

    std::string_view name() const override { return "POM2 test port (paravirtual)"; }
    int getSlot() const { return slot_; }

    void deviceSelectWrite(uint8_t low4, uint8_t v) override;

    /// Every finished line goes here as it completes, as well as into
    /// `lines()`. Set before the CPU runs.
    void setLineSink(LineSink sink) { sink_ = std::move(sink); }

    bool finished() const { return finished_; }
    int  exitStatus() const { return status_; }
    int  assertionsPassed() const { return passed_; }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    void endLine();
    void finish(int status);

    int  slot_;
    LineSink sink_;
    std::string pending_;
    std::vector<std::string> lines_;
    bool lastWasCr_ = false;
    bool finished_  = false;
    int  status_    = 0;
    int  passed_    = 0;
    int  asserts_   = 0;
};

} // namespace pom2

#endif // POM2_TEST_PORT_CARD_H
