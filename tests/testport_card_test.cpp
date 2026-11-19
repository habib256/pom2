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

// The paravirtual test port (ARCH-017): its three registers, the line
// rules, a spent port after EXIT, and — the promise that makes it safe to
// have — that its reads are the floating bus an empty slot answers.

#include "Memory.h"
#include "SlotBus.h"
#include "TestPortCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using pom2::TestPortCard;

namespace {

void logText(TestPortCard& c, const char* s)
{
    for (; *s; ++s) c.deviceSelectWrite(TestPortCard::kLog, static_cast<uint8_t>(*s));
}

void testLines()
{
    TestPortCard c(7);
    std::vector<std::string> seen;
    c.setLineSink([&](const std::string& l) { seen.push_back(l); });
    logText(c, "one\r");
    logText(c, "two\r\n");        // CR LF ends ONE line
    logText(c, "three\n");
    logText(c, "\r");             // an empty line is a line
    // Apple text: bit 7 set, and $8D is the Apple's CR.
    for (const char* s = "HI"; *s; ++s)
        c.deviceSelectWrite(TestPortCard::kLog, static_cast<uint8_t>(*s | 0x80));
    c.deviceSelectWrite(TestPortCard::kLog, 0x8D);
    const std::vector<std::string> want = { "one", "two", "three", "", "HI" };
    assert(seen == want);
    assert(c.lines() == want);
    assert(!c.finished());
    std::printf("  ok: CR, LF, CR LF once, bit 7 ignored\n");
}

void testExit()
{
    TestPortCard c(7);
    logText(c, "partial");
    c.deviceSelectWrite(TestPortCard::kExit, 3);
    assert(c.finished() && c.exitStatus() == 3);
    assert(c.lines().size() == 1 && c.lines()[0] == "partial");   // flushed on exit
    // Spent: nothing after EXIT changes the verdict or the log.
    logText(c, "late\r");
    c.deviceSelectWrite(TestPortCard::kExit, 0);
    c.deviceSelectWrite(TestPortCard::kAssert, 0);
    assert(c.exitStatus() == 3 && c.lines().size() == 1);
    std::printf("  ok: EXIT flushes the partial line, then the port is spent\n");
}

void testAssert()
{
    TestPortCard c(7);
    c.deviceSelectWrite(TestPortCard::kAssert, 1);
    c.deviceSelectWrite(TestPortCard::kAssert, 0xFF);
    assert(!c.finished() && c.assertionsPassed() == 2);
    logText(c, "x == 1");
    c.deviceSelectWrite(TestPortCard::kAssert, 0);
    assert(c.finished() && c.exitStatus() == TestPortCard::kAssertFailedStatus);
    assert(c.lines().back() == "ASSERT #3 FAILED: x == 1");

    TestPortCard bare(7);
    bare.deviceSelectWrite(TestPortCard::kAssert, 0);
    assert(bare.lines().back() == "ASSERT #1 FAILED");
    std::printf("  ok: ASSERT counts passes, a failure ends the run with status 2\n");
}

void testUndecodedOffsets()
{
    TestPortCard c(7);
    for (int off = 3; off < 16; ++off)
        c.deviceSelectWrite(static_cast<uint8_t>(off), 0);
    assert(!c.finished() && c.lines().empty());
    std::printf("  ok: $C0n3-$C0nF are not decoded\n");
}

void testThroughTheBus()
{
    Memory mem;
    // Reads are the floating bus: the empty slot 5 answers the same bus at
    // the same moment (memRead advances no cycle here), so the two agree.
    auto card = std::make_unique<TestPortCard>(7);
    auto* live = card.get();
    mem.slotBus().plug(7, std::move(card));
    for (int off = 0; off < 16; ++off) {
        const uint8_t viaPort  = mem.memRead(static_cast<uint16_t>(0xC0F0 + off));
        const uint8_t viaEmpty = mem.memRead(static_cast<uint16_t>(0xC0D0 + off));   // slot 5, empty
        assert(viaPort == viaEmpty && "a test-port read must be the floating bus");
    }
    // A guest's STA $C0F0 / $C0F1 reaches the card.
    for (const char* s = "OK"; *s; ++s) mem.memWrite(0xC0F0, static_cast<uint8_t>(*s));
    mem.memWrite(0xC0F1, 7);
    assert(live->finished() && live->exitStatus() == 7);
    assert(live->lines().size() == 1 && live->lines()[0] == "OK");
    std::printf("  ok: guest writes reach it, guest reads see the floating bus\n");
}

} // namespace

int main()
{
    std::printf("testport_card:\n");
    testLines();
    testExit();
    testAssert();
    testUndecodedOffsets();
    testThroughTheBus();
    std::printf("testport_card: all passed\n");
    return 0;
}
