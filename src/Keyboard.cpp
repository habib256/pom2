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

// Keyboard — see Keyboard.h. Bodies moved verbatim from Memory.cpp (the paste
// FIFO's control-byte filtering, CR/LF collapse and case-fold, and the
// newest-wins latch), with only the member renames Memory used inline
// (lastKey → lastKey_, keyReady → keyReady_, pasteQueue → pasteQueue_,
// publishKbLatch → publish, kbMutex → mtx_).

#include "Keyboard.h"

namespace pom2 {

void Keyboard::queueKey(uint8_t apple2Key)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const uint8_t b = apple2Key & 0x7F;
    if (!pasteQueue_.empty()) {
        // A host paste is draining — append so this live keystroke is
        // delivered in order AFTER it, instead of clobbering the currently
        // latched paste byte and jumping the FIFO.
        pasteQueue_.push_back(b);
        publish();
    } else {
        // No paste in flight: behave like the hardware latch — newest key
        // wins (fast typing overwrites an unread key, as on real hardware).
        lastKey_  = b;
        keyReady_ = true;
        publish();
    }
}

void Keyboard::clearStrobe()
{
    std::lock_guard<std::mutex> lk(mtx_);
    // Apple II hardware leaves the key byte in the latch and only releases
    // the strobe — KEYIN re-polls $C000 until a fresh key arrives.
    keyReady_ = false;
    // The next paste byte is NOT promoted here. $C010 is an acknowledgement,
    // and a program is free to read it more than once per key ("is the key
    // back up?"): re-arming inside the clear made every one of those polls
    // consume a queued byte. readLatch() does the hand-over instead, on the
    // $C000 read — the ROM still clocks the paste out at exactly the rate it
    // can consume, one byte per poll that finds the strobe clear.
    publish();
}

uint8_t Keyboard::readLatch()
{
    const uint8_t v = mirror_.load(std::memory_order_relaxed);
    // Strobe still set (the guest has not acked yet) or nothing queued: the
    // overwhelmingly common case, and it stays off the mutex.
    if ((v & 0x80) != 0 || !pasteArmed_.load(std::memory_order_relaxed))
        return v;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!keyReady_ && !pasteQueue_.empty()) {
        lastKey_  = pasteQueue_.front() & 0x7F;
        keyReady_ = true;
        pasteQueue_.pop_front();
        publish();
    }
    return mirror_.load(std::memory_order_relaxed);
}

void Keyboard::pushOne(uint8_t b)
{
    // First byte goes straight into the latch if it's empty; rest go into
    // the queue and drain via readLatch().
    if (!keyReady_ && pasteQueue_.empty()) {
        lastKey_  = b;
        keyReady_ = true;
        publish();
    } else {
        pasteQueue_.push_back(b);
    }
}

namespace {

// One valid UTF-8 sequence at p[0..n): its code point and length. A lead
// byte that is not the start of a well-formed 2-4 byte sequence (a lone
// continuation, an overlong $C0/$C1, a truncated tail) is NOT UTF-8 — the
// caller then treats the byte as Apple high-ASCII, as it always did.
bool decodeUtf8(const uint8_t* p, std::size_t n, uint32_t& cp, std::size_t& len)
{
    const uint8_t b = p[0];
    if      (b >= 0xC2 && b <= 0xDF) { len = 2; cp = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) { len = 3; cp = b & 0x0F; }
    else if (b >= 0xF0 && b <= 0xF4) { len = 4; cp = b & 0x07; }
    else return false;
    if (len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
        if ((p[k] & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    return true;
}

// The ASCII an Apple II keyboard could have typed for `cp`, or nullptr to
// drop it. Latin-1 letters lose their accent, typographic punctuation goes
// back to the typewriter's. The Apple II has none of the rest.
const char* asciiFor(uint32_t cp)
{
    static const char* const kLatin1[64] = {   // U+00C0..U+00FF
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","TH","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y" };
    if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
    switch (cp) {
        case 0x00A0: case 0x2007: case 0x2009: case 0x200A: case 0x202F:
            return " ";                                   // no-break / thin spaces
        case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E:
            return "\"";
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x00B4:
            return "'";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
        case 0x2212:
            return "-";
        case 0x2026: return "...";
        case 0x2022: case 0x00B7: return "*";
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        default:     return nullptr;
    }
}

}  // namespace

std::size_t Keyboard::pushChars(const char* data, std::size_t length,
                                bool foldToUpper, bool dropControls,
                                bool& prevWasCR)
{
    // Cap against the LIVE queue size, not just this call, so repeated pastes
    // can't grow pasteQueue_ without bound (a memory DoS via the AI-control or
    // clipboard paths).
    const std::size_t inFlight = pasteQueue_.size() + (keyReady_ ? 1u : 0u);
    const std::size_t room = (inFlight >= kPasteMaxChars) ? 0u : (kPasteMaxChars - inFlight);

    std::size_t queued = 0;
    // Everything below the line-ending step, for one 7-bit character.
    auto emit = [&](uint8_t b) {
        if (queued >= room) return;
        b &= 0x7F;
        if (dropControls && b < 0x20 && b != 0x0D && b != 0x09) return;
        if (foldToUpper && b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - 'a' + 'A');
        pushOne(b);
        ++queued;
    };
    for (std::size_t i = 0; i < length && queued < room; ++i) {
        uint8_t b = static_cast<uint8_t>(data[i]);

        // CLIPBOARD text is UTF-8. Masking each byte of a multi-byte
        // character turned its lead byte into a letter: "Café" typed
        // "CafC)", curly quotes typed 'b', a no-break space 'B' (bug hunt
        // 2026-09-29). Decode it and type what an Apple II keyboard could:
        // the letter without its accent, straight quotes, a space — or
        // nothing. A byte that does not start a valid sequence is Apple
        // high-ASCII and keeps the old mask. A TERMINAL stream
        // (dropControls false) is raw 8-bit and is left alone.
        if (dropControls && b >= 0x80) {
            uint32_t cp = 0;
            std::size_t len = 0;
            if (decodeUtf8(reinterpret_cast<const uint8_t*>(data) + i,
                           length - i, cp, len)) {
                prevWasCR = false;
                if (const char* a = asciiFor(cp))
                    for (; *a; ++a) emit(static_cast<uint8_t>(*a));
                i += len - 1;
                continue;
            }
        }

        // Line-ending normalisation: \r, \n, and \r\n all collapse to one
        // CR ($0D). Track the previous byte so the LF after CR doesn't
        // produce a second CR.
        if (b == '\r') {
            b = 0x0D;
            prevWasCR = true;
        } else if (b == '\n') {
            if (prevWasCR) { prevWasCR = false; continue; }  // swallowed
            b = 0x0D;
            prevWasCR = false;
        } else {
            prevWasCR = false;
        }

        // emit(): strip the high bit FIRST — Apple II is 7-bit ASCII. Order
        // matters: a byte in $80-$9F would pass the control filter untouched
        // and then be masked into a control code (a pasted "é" used to arrive
        // as Ctrl-C). Mask, then drop unprintable controls except CR and HT
        // (CLIPBOARD policy only: a TERMINAL byte stream must keep Ctrl-C,
        // $08 and ESC — see Keyboard.h), then fold a-z → A-Z on a ][ / ][+,
        // whose keyboard cannot emit $61-$7A (the caller passes
        // foldToUpper = !iieMode).
        emit(b);
    }
    publish();
    return queued;
}

std::size_t Keyboard::pasteText(const char* data, std::size_t length, bool foldToUpper)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    // A clipboard paste is one self-contained block: its CR state starts and
    // ends here.
    bool prevWasCR = false;
    return pushChars(data, length, foldToUpper, /*dropControls=*/true, prevWasCR);
}

std::size_t Keyboard::pasteKeyStream(const char* data, std::size_t length,
                                     bool foldToUpper)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    // The CR state is a MEMBER here: the caller is a byte-at-a-time terminal
    // sink, so CR LF straddles two calls.
    return pushChars(data, length, foldToUpper, /*dropControls=*/false,
                     streamPrevCR_);
}

std::size_t Keyboard::pasteRawKeys(const char* data, std::size_t length)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    const std::size_t inFlight = pasteQueue_.size() + (keyReady_ ? 1u : 0u);
    const std::size_t room = (inFlight >= kPasteMaxChars) ? 0u : (kPasteMaxChars - inFlight);
    std::size_t queued = 0;
    for (std::size_t i = 0; i < length && queued < room; ++i) {
        pushOne(static_cast<uint8_t>(data[i]) & 0x7F);
        ++queued;
    }
    publish();
    return queued;
}

} // namespace pom2
