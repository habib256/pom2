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

// Keyboard — the Apple II keyboard latch and the host paste FIFO, split out
// of the Memory god-object (TODO P2, "one concern per file").
//
// The hardware is a single-byte latch: $C000 reads `key | strobe`, $C010
// clears the strobe (the byte stays latched — KEYIN re-polls until a fresh
// key arrives). POM2 adds a host paste FIFO behind it: pasted text lands one
// byte at a time, promoted into the latch on each strobe clear, so the ROM's
// own strobe-and-poll loop clocks it out at exactly the rate it consumes.
//
// Threading: writers (the GLFW key callback on the UI thread, the AI-control
// HTTP thread, the clipboard path) serialise on `mtx_`. The CPU worker's hot
// $C000 read does NOT take the lock — it reads `mirror_`, an atomic
// republished under `mtx_` after every change, because a mutex on that path
// measured ~5 % of a banner profile. `lastKey7()` is the locked read used by
// the cold $C011/$C012/$C019 IIe status registers, which carry the last char
// in their low 7 bits. Pinned by `input_io_smoke`, `paste_smoke` and
// `ui_worker_contention`.
//
// **Deliberately absent from every snapshot** (reviewed 2026-09-06). Both the
// latch and the FIFO are HOST INPUT in flight, not machine state:
//
//   * The latch is the last key the USER pressed. Restoring it would
//     re-deliver a keystroke the guest already consumed — a rewind past a
//     RETURN would make the restored machine take that RETURN a second time,
//     which is the opposite of "the machine goes back, the user's hands do
//     not". Conversely it would drop a press the user made during a scrub.
//   * The paste FIFO is a queue the user started from the host; rewinding it
//     would re-type text the guest already received, and running it forward
//     would silently swallow the rest.
//   * Both live behind `mtx_`, written from the UI / AI-control threads,
//     while a snapshot restore holds `stateMutex` — the two lock domains do
//     not serialise, so a captured value can be stale before it is written
//     back.
//
// The consequence is bounded and correct: after a restore the guest sees
// whatever the host has typed since, and nothing it already read.

#ifndef POM2_KEYBOARD_H
#define POM2_KEYBOARD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace pom2 {

class Keyboard
{
public:
    // Runaway-paste guard: the live FIFO never exceeds this, so repeated
    // pastes from the clipboard or the AI-control path can't grow it without
    // bound.
    static constexpr std::size_t kPasteMaxChars = 4096;

    // Hot path: the $C000 read. `key | (strobe ? 0x80 : 0)`, lock-free.
    uint8_t latchMirror() const { return mirror_.load(std::memory_order_relaxed); }

    // The last latched character, low 7 bits, taken under the lock — the
    // value the IIe $C011/$C012/$C019 status reads fold into bit 0-6.
    uint8_t lastKey7() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return static_cast<uint8_t>(lastKey_ & 0x7F);
    }

    // A live keystroke: newest-wins into the latch when no paste is in
    // flight, else appended so it is delivered in FIFO order after the paste.
    void queueKey(uint8_t apple2Key);

    // $C010: drop the strobe; promote the next paste byte if there is one.
    void clearStrobe();

    // Host paste. `foldToUpper` maps a-z → A-Z (the ][/][+ keyboard has no
    // lowercase); the caller passes `!iieMode`. Control bytes except CR/HT
    // are dropped and CR/LF/CRLF collapse to one CR. Returns bytes accepted.
    std::size_t pasteText(const char* data, std::size_t length, bool foldToUpper);
    // Raw variant: no filtering or case-folding, same FIFO + cap. For tests
    // and byte-exact injection.
    std::size_t pasteRawKeys(const char* data, std::size_t length);

    std::size_t pendingPasteSize() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return pasteQueue_.size();
    }
    void cancelPaste() {
        std::lock_guard<std::mutex> lk(mtx_);
        pasteQueue_.clear();
    }

    // Reset (construction / soft reset): abandon any in-flight paste and
    // drop the strobe. The latched byte itself is cleared too.
    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        lastKey_  = 0;
        keyReady_ = false;
        pasteQueue_.clear();
        publish();
    }

private:
    // mtx_ held by the caller.
    void publish() {
        mirror_.store(static_cast<uint8_t>(lastKey_ | (keyReady_ ? 0x80 : 0x00)),
                      std::memory_order_relaxed);
    }

    mutable std::mutex   mtx_;
    uint8_t              lastKey_  = 0;
    bool                 keyReady_ = false;
    std::atomic<uint8_t> mirror_{ 0 };
    std::deque<uint8_t>  pasteQueue_;
};

} // namespace pom2

#endif // POM2_KEYBOARD_H
