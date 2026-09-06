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

// SLIP framing test — pins src/SlipFramer.h.
//
// SP-over-SLIP puts every SmartPort packet inside a SLIP frame, so a framing
// bug does not corrupt one byte, it desynchronises the whole link and every
// subsequent disk block lands in the wrong place. What is pinned here:
//
//   1. THE ESCAPES ARE THE SPEC'S. $C0 → $DB $DC and $DB → $DB $DD, and
//      nothing else is escaped. A 512-byte ProDOS block contains $C0 and $DB
//      constantly (they are ordinary data), so this is exercised on every
//      real disk read.
//   2. A FRAME SPLIT ACROSS READS STILL DECODES. Neither transport delivers
//      whole frames — TCP returns whatever is in the window, a UART returns
//      whatever it latched — so the decoder is fed byte at a time in
//      production and must not care where the chunk boundaries fell.
//   3. A DELIMITER MID-ESCAPE IS REPORTED, NOT ABSORBED. That is the FujiNet
//      spec's stated reason for framing at all: it is what an Apple II reset
//      during transmission looks like on the wire. Absorbing it would splice
//      two packets into one and the sequence-number check downstream would
//      then reject a response that was never really lost.
//   4. THE DECODER SURVIVES ITS OWN ERRORS. After a truncation it must be
//      ready for the next frame — a link that has to be torn down and rebuilt
//      after every guest reset would be useless.

#include "SlipFramer.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

using pom2::SlipFramer;

/// Feed a whole buffer, collecting completed frames and counting truncations.
std::vector<std::vector<uint8_t>> feedAll(SlipFramer& f,
                                          const std::vector<uint8_t>& wire,
                                          int* truncations = nullptr)
{
    std::vector<std::vector<uint8_t>> out;
    for (uint8_t b : wire) {
        switch (f.feed(b)) {
        case SlipFramer::Feed::Frame:     out.push_back(f.frame()); break;
        case SlipFramer::Feed::Truncated: if (truncations) ++*truncations; break;
        case SlipFramer::Feed::NeedMore:  break;
        }
    }
    return out;
}

void testEscaping()
{
    // Body deliberately contains both special bytes, adjacent, plus the two
    // escape-completion bytes on their own (which must NOT be escaped).
    const std::vector<uint8_t> body{ 0x01, 0xC0, 0xDB, 0xDC, 0xDD, 0xC0, 0xFF };
    std::vector<uint8_t> wire;
    SlipFramer::encode(body, wire);

    const std::vector<uint8_t> expect{
        0xC0,                     // opening delimiter
        0x01,
        0xDB, 0xDC,               // $C0 escaped
        0xDB, 0xDD,               // $DB escaped
        0xDC,                     // plain data — not special on its own
        0xDD,                     // ditto
        0xDB, 0xDC,               // $C0 escaped again
        0xFF,
        0xC0,                     // closing delimiter
    };
    assert(wire == expect);

    SlipFramer f;
    const auto frames = feedAll(f, wire);
    assert(frames.size() == 1);
    assert(frames[0] == body);
}

void testRoundTripAllByteValues()
{
    // Every possible byte value, so no value can be quietly special-cased.
    std::vector<uint8_t> body(256);
    for (int i = 0; i < 256; ++i) body[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

    std::vector<uint8_t> wire;
    SlipFramer::encode(body, wire);

    SlipFramer f;
    const auto frames = feedAll(f, wire);
    assert(frames.size() == 1);
    assert(frames[0] == body);
}

void testBackToBackFrames()
{
    // Two frames in one buffer: the encoder emits a closing AND an opening
    // delimiter between them, so the decoder sees $C0 $C0 and must not
    // mistake that for an empty packet.
    const std::vector<uint8_t> a{ 0x10, 0xC0, 0x11 };
    const std::vector<uint8_t> b{ 0x20 };
    std::vector<uint8_t> wire;
    SlipFramer::encode(a, wire);
    SlipFramer::encode(b, wire);

    SlipFramer f;
    const auto frames = feedAll(f, wire);
    assert(frames.size() == 2);
    assert(frames[0] == a);
    assert(frames[1] == b);
}

void testSharedDelimiterBetweenFrames()
{
    // RFC 1055 lets a sender end one packet and begin the next with a SINGLE
    // END, and POM2 has to decode what a peer sends rather than only what its
    // own encoder emits. The decoder used to drop back to "waiting for a
    // delimiter" here, so `C0 a C0 b C0` delivered `a`, skipped `b` as
    // inter-frame noise, and let b's closing delimiter merely OPEN a frame:
    // one packet lost, and every answer after it read one request behind.
    const std::vector<uint8_t> a{ 0x10, 0x11 };
    const std::vector<uint8_t> b{ 0x20, 0xC0, 0x21 };   // and it still unescapes
    std::vector<uint8_t> wire{ 0xC0 };
    for (uint8_t x : a) wire.push_back(x);
    wire.push_back(0xC0);                               // closes a, opens b
    wire.push_back(0x20);
    wire.push_back(0xDB); wire.push_back(0xDC);         // escaped $C0
    wire.push_back(0x21);
    wire.push_back(0xC0);                               // closes b

    SlipFramer f;
    int truncations = 0;
    const auto frames = feedAll(f, wire, &truncations);
    assert(truncations == 0);
    assert(frames.size() == 2);
    assert(frames[0] == a);
    assert(frames[1] == b);

    // Three packets in a row, sharing both delimiters — the shape a peer that
    // has traffic queued produces.
    SlipFramer g;
    const std::vector<uint8_t> chain{ 0xC0, 0x01, 0xC0, 0x02, 0xC0, 0x03, 0xC0 };
    const auto three = feedAll(g, chain);
    assert(three.size() == 3);
    assert(three[0] == (std::vector<uint8_t>{ 0x01 }));
    assert(three[1] == (std::vector<uint8_t>{ 0x02 }));
    assert(three[2] == (std::vector<uint8_t>{ 0x03 }));
}

void testLeadingGarbageIsSkipped()
{
    // A transport that attaches mid-stream sees the tail of somebody else's
    // frame first. It must be discarded, not prepended to the next packet.
    std::vector<uint8_t> wire{ 0x99, 0x98, 0x97 };
    const std::vector<uint8_t> body{ 0x42, 0x43 };
    SlipFramer::encode(body, wire);

    SlipFramer f;
    const auto frames = feedAll(f, wire);
    assert(frames.size() == 1);
    assert(frames[0] == body);
}

void testTruncatedMidEscape()
{
    // $DB immediately followed by the delimiter = a reset landed between the
    // escape and its completion byte.
    std::vector<uint8_t> wire{ 0xC0, 0x01, 0x02, 0xDB, 0xC0 };
    // ...and the delimiter that ended the broken frame opens the next one,
    // which arrives intact.
    const std::vector<uint8_t> good{ 0x07, 0x08 };
    for (uint8_t b : good) wire.push_back(b);
    wire.push_back(0xC0);

    SlipFramer f;
    int truncations = 0;
    const auto frames = feedAll(f, wire, &truncations);
    assert(truncations == 1);
    // The broken frame is NOT delivered...
    assert(frames.size() == 1);
    // ...and the one after it is, intact — no byte of the broken frame
    // leaked into it.
    assert(frames[0] == good);
}

void testInvalidEscapeByte()
{
    // $DB followed by something that is neither $DC nor $DD is not SLIP.
    std::vector<uint8_t> wire{ 0xC0, 0x01, 0xDB, 0x55, 0x02, 0xC0 };
    const std::vector<uint8_t> good{ 0x31 };
    SlipFramer::encode(good, wire);

    SlipFramer f;
    int truncations = 0;
    const auto frames = feedAll(f, wire, &truncations);
    assert(truncations == 1);
    assert(frames.size() == 1);
    assert(frames[0] == good);
}

void testOversizeFrameGivesUpAndResyncs()
{
    SlipFramer f;
    int truncations = 0;

    // Open a frame and never close it, past the cap.
    assert(f.feed(0xC0) == SlipFramer::Feed::NeedMore);
    bool sawTruncation = false;
    for (size_t i = 0; i < SlipFramer::kMaxFrameBytes + 16; ++i) {
        if (f.feed(0x5A) == SlipFramer::Feed::Truncated) { sawTruncation = true; break; }
    }
    assert(sawTruncation);

    // The decoder must still be usable afterwards.
    const std::vector<uint8_t> good{ 0x77, 0xC0, 0x88 };
    std::vector<uint8_t> wire;
    SlipFramer::encode(good, wire);
    const auto frames = feedAll(f, wire, &truncations);
    assert(frames.size() == 1);
    assert(frames[0] == good);
}

void testChunkBoundariesDoNotMatter()
{
    // Same wire bytes, fed in every possible single split point. Whatever the
    // transport hands us, the decode must be identical — this is the property
    // that makes the framer safe against TCP segmentation and UART latency.
    const std::vector<uint8_t> body{ 0xC0, 0xDB, 0x00, 0xFF, 0xC0 };
    std::vector<uint8_t> wire;
    SlipFramer::encode(body, wire);

    for (size_t split = 0; split <= wire.size(); ++split) {
        SlipFramer f;
        std::vector<std::vector<uint8_t>> frames;
        auto pump = [&](size_t from, size_t to) {
            for (size_t i = from; i < to; ++i)
                if (f.feed(wire[i]) == SlipFramer::Feed::Frame)
                    frames.push_back(f.frame());
        };
        pump(0, split);
        pump(split, wire.size());
        assert(frames.size() == 1);
        assert(frames[0] == body);
    }
}

void testResetDropsPartialFrame()
{
    SlipFramer f;
    f.feed(0xC0);
    f.feed(0x11);
    f.feed(0x22);
    f.reset();                    // transport reconnected under us

    const std::vector<uint8_t> body{ 0x33 };
    std::vector<uint8_t> wire;
    SlipFramer::encode(body, wire);
    const auto frames = feedAll(f, wire);
    assert(frames.size() == 1);
    assert(frames[0] == body);    // $11 $22 did not survive the reset
}

} // namespace

int main()
{
    testEscaping();
    testRoundTripAllByteValues();
    testBackToBackFrames();
    testSharedDelimiterBetweenFrames();
    testLeadingGarbageIsSkipped();
    testTruncatedMidEscape();
    testInvalidEscapeByte();
    testOversizeFrameGivesUpAndResyncs();
    testChunkBoundariesDoNotMatter();
    testResetDropsPartialFrame();

    std::puts("slip_framer: OK");
    return 0;
}
