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

// Smoke test for the paste-text → keyboard pipeline. Pins:
//   - line-ending normalisation (\r\n / \r / \n all → one CR)
//   - unprintable controls below $20 dropped (except CR / HT)
//   - high bit stripped (Apple II is 7-bit)
//   - queue drains exactly one byte per $C010 strobe clear
//   - cap at Memory::kPasteMaxChars (no overflow)

#include "Memory.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>

namespace {

// Read $C000 + clear strobe via $C010, returning the latched byte (low 7
// bits) and the strobe state both before and after the clear. Mimics the
// Monitor's KEYIN flow.
uint8_t consumeKey(Memory& mem)
{
    const uint8_t latched = mem.memRead(0xC000);
    assert(latched & 0x80);                  // strobe must be high
    (void)mem.memRead(0xC010);               // clear strobe — drains queue
    return latched & 0x7F;
}

bool keyReady(Memory& mem) { return (mem.memRead(0xC000) & 0x80) != 0; }

} // namespace

// A $C010 release-poll must not eat the queue — the //e's own "wait for the
// key to come back up" idiom (BIT $C010 / LDA $C010 : BMI -). Promoting the
// next paste byte on the strobe CLEAR re-armed AKD inside the same access,
// so the poll spun and consumed one queued byte per iteration: a ten-character
// paste delivered one and dropped nine.
static void testReleasePollDoesNotEatTheQueue()
{
    Memory mem;
    mem.setIIEMode(true);
    assert(mem.pasteText("ABCDEFGHIJ") == 10);
    std::string out;
    for (int guard = 0; guard < 64 && (mem.memRead(0xC000) & 0x80); ++guard) {
        out.push_back(static_cast<char>(mem.memRead(0xC000) & 0x7F));
        (void)mem.memRead(0xC010);                          // BIT $C010 — the ack
        int spin = 0;
        while ((mem.memRead(0xC010) & 0x80) && ++spin < 8) {}   // LDA $C010 : BMI -
        assert(spin < 8 && "AKD must fall after the ack");
    }
    assert(out == "ABCDEFGHIJ" && "the release poll ate the paste queue");
    std::printf("paste_smoke: $C010 release poll keeps the queue OK\n");
}

int main()
{
    testReleasePollDoesNotEatTheQueue();
    // ── Empty paste is a no-op ───────────────────────────────────────────
    {
        Memory mem;
        const size_t queued = mem.pasteText("");
        assert(queued == 0);
        assert(!keyReady(mem));
        assert(mem.pendingPasteSize() == 0);
    }

    // ── Single line, plain ASCII, no line endings ────────────────────────
    {
        Memory mem;
        const size_t queued = mem.pasteText("PRINT 1");
        assert(queued == 7);
        // First byte is in the latch, the other 6 in the queue.
        assert(mem.pendingPasteSize() == 6);
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "PRINT 1");
    }

    // ── \r\n normalisation: "A\r\nB" → "A\rB" (one CR, no double LF) ─────
    {
        Memory mem;
        mem.pasteText("A\r\nB");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out.size() == 3);
        assert(out[0] == 'A');
        assert(out[1] == 0x0D);
        assert(out[2] == 'B');
    }

    // ── \n alone also becomes CR ─────────────────────────────────────────
    {
        Memory mem;
        mem.pasteText("X\nY");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "X\rY");
    }

    // ── Bare \r becomes CR (no LF following) ─────────────────────────────
    {
        Memory mem;
        mem.pasteText("X\rY");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "X\rY");
    }

    // ── Controls below $20 dropped (except CR/HT) ────────────────────────
    {
        Memory mem;
        std::string in;
        in.push_back('A');
        in.push_back(0x07);  // BEL — drop
        in.push_back('B');
        in.push_back(0x09);  // HT — keep
        in.push_back('C');
        in.push_back(0x1B);  // ESC — drop
        in.push_back('D');
        const size_t queued = mem.pasteText(in);
        assert(queued == 5);   // A B HT C D
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out.size() == 5);
        assert(out[0] == 'A' && out[1] == 'B' && out[2] == 0x09 &&
               out[3] == 'C' && out[4] == 'D');
    }

    // ── High bit stripped ────────────────────────────────────────────────
    {
        Memory mem;
        std::string in;
        in.push_back(static_cast<char>(0xC1));  // 'A' with bit 7 set
        mem.pasteText(in);
        assert(consumeKey(mem) == 'A');
    }

    // ── UTF-8 bytes are masked BEFORE the control filter ─────────────────
    // Order regression (hunt #4 item #8): the filter used to run on the raw
    // byte, so anything in $80-$9F sailed past it and was only THEN masked
    // into a control code. "Ã" ($C3 $83) delivered $03 = Ctrl-C, and "’"
    // ($E2 $80 $99) delivered a NUL and a Ctrl-Y — every accented French
    // paste typed control characters at the guest.
    {
        Memory mem;
        mem.setIIEMode(true);                  // no case folding, easier to read
        mem.pasteText("\xE2\x80\x99");         // U+2019 RIGHT SINGLE QUOTE
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        for (unsigned char c : out)
            assert(c >= 0x20 || c == 0x0D || c == 0x09);
        assert(out == "b");                    // $E2 & $7F; $80/$99 dropped
    }
    {
        Memory mem;
        mem.setIIEMode(true);
        mem.pasteText("A\xC3\x83Z");           // 'A' + U+00C3 + 'Z'
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        for (unsigned char c : out)
            assert(c >= 0x20 || c == 0x0D || c == 0x09);
        assert(out == "ACZ");                  // $C3 & $7F = 'C'; $83 dropped
    }

    // ── Cap at Memory::kPasteMaxChars ────────────────────────────────────
    {
        Memory mem;
        std::string huge(Memory::kPasteMaxChars + 100, 'X');
        const size_t queued = mem.pasteText(huge);
        assert(queued == Memory::kPasteMaxChars);
    }

    // ── cancelPaste empties the queue but doesn't clear the latch ────────
    {
        Memory mem;
        mem.pasteText("HELLO");
        assert(mem.pendingPasteSize() == 4);  // first byte in latch
        mem.cancelPaste();
        assert(mem.pendingPasteSize() == 0);
        // Latch still has the first byte ready until the strobe is cleared.
        assert(keyReady(mem));
        (void)mem.memRead(0xC010);
        // Now the latch should be empty (queue was cancelled, no replenish).
        assert(!keyReady(mem));
    }

    // ── queueKey during a paste APPENDS (doesn't clobber the FIFO) ───────
    // A live keystroke arriving mid-paste must queue behind the in-flight
    // paste, not overwrite the currently-latched paste byte and jump the
    // FIFO. Pins Memory::queueKey's "paste in flight" branch.
    {
        Memory mem;
        mem.pasteText("AB");                  // 'A' latched, 'B' queued
        assert(mem.pendingPasteSize() == 1);
        mem.queueKey('Z');                    // append after 'B'
        assert(mem.pendingPasteSize() == 2);
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "ABZ");
    }

    // ── queueKey with NO paste in flight overwrites the latch (hardware) ──
    {
        Memory mem;
        mem.queueKey('A');
        assert(keyReady(mem));
        mem.queueKey('B');                    // newest key wins, like the latch
        assert(mem.pendingPasteSize() == 0);
        assert(consumeKey(mem) == 'B');
        assert(!keyReady(mem));               // only one key survived
    }

    // ── A reset abandons an in-flight paste (IIe full + II/II+ warm) ─────
    {
        Memory mem;                            // II+ default (iieMode off)
        mem.pasteText("HELLO");
        assert(mem.pendingPasteSize() == 4);
        mem.resetSoftSwitchesWarm();           // II/II+ machine_reset path
        assert(mem.pendingPasteSize() == 0);
        assert(!keyReady(mem));
    }
    {
        Memory mem;
        mem.setIIEMode(true);
        mem.pasteText("HELLO");
        assert(mem.pendingPasteSize() == 4);
        mem.resetSoftSwitches();               // IIe full reset_w path
        assert(mem.pendingPasteSize() == 0);
        assert(!keyReady(mem));
    }

    // ── ][/][+ has no lowercase: paste folds a-z → A-Z; IIe keeps case ──
    {
        Memory mem;                            // iieMode off
        mem.pasteText("print 1");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "PRINT 1");
    }
    {
        Memory mem;
        mem.setIIEMode(true);                  // IIe keyboard has lowercase
        mem.pasteText("print 1");
        std::string out;
        while (keyReady(mem)) out.push_back(static_cast<char>(consumeKey(mem)));
        assert(out == "print 1");
    }

    // ── Cap is against the LIVE queue, not per-call: repeated pastes can't
    //    grow pasteQueue past kPasteMaxChars (memory-DoS guard). ──────────
    {
        Memory mem;
        const size_t q1 = mem.pasteText(std::string(Memory::kPasteMaxChars - 10, 'X'));
        assert(q1 == Memory::kPasteMaxChars - 10);
        const size_t q2 = mem.pasteText(std::string(100, 'Y'));
        assert(q2 == 10);                      // only 10 slots remained
        const size_t q3 = mem.pasteText("Z");
        assert(q3 == 0);                       // queue is full
        // pasteRawKeys shares the same live-queue accounting.
        const char z = 'Z';
        assert(mem.pasteRawKeys(&z, 1) == 0);
    }

    std::printf("Paste smoke: OK (line endings, controls, 7-bit, cap, cancel, "
                "queueKey-order, reset-clear, case-fold, live-cap)\n");
    return 0;
}
