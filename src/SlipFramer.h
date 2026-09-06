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

// SlipFramer — SLIP (RFC 1055) framing for the FujiNet SP-over-SLIP link.
//
// SmartPort-over-SLIP wraps every request/response packet in a SLIP frame for
// two reasons the FujiNet spec spells out:
//
//   1. Packet boundaries are recoverable WITHOUT parsing the (variable-sized)
//      packet — the framer never has to know what a SmartPort command is.
//   2. A frame cut short by an Apple II reset mid-transmission is DETECTABLE,
//      rather than silently merging into the next packet.
//
// Encoding, per the spec (https://github.com/FujiNetWIFI/fujinet-firmware/wiki/
// Apple-II-SP-over-SLIP, wiki revision of 2025-01-25):
//
//   frame  := $C0 body $C0
//   $C0 inside the body → $DB $DC
//   $DB inside the body → $DB $DD
//
// This header is pure: bytes in, bytes out. No sockets, no serial ports, no
// threads, no SmartPort knowledge — which is what makes it testable on its own
// (tests/slip_framer_test.cpp) and reusable by both transports.
//
// The decoder is INCREMENTAL because neither transport delivers whole frames:
// a TCP read returns whatever happens to be in the window, and a serial read
// returns whatever the UART has latched. Feed it every byte you receive; it
// tells you when a frame completed.

#ifndef POM2_SLIP_FRAMER_H
#define POM2_SLIP_FRAMER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class SlipFramer
{
public:
    static constexpr uint8_t kEnd    = 0xC0;
    static constexpr uint8_t kEsc    = 0xDB;
    static constexpr uint8_t kEscEnd = 0xDC;
    static constexpr uint8_t kEscEsc = 0xDD;

    /// Hard ceiling on a decoded frame. The largest legitimate packet is a
    /// SmartPort `Read` response, whose byte count is 16-bit, plus the 2-byte
    /// header — so 64 KB + slack. The cap exists so a peer spewing garbage
    /// (or a mis-framed stream that never sees an $C0) cannot grow the buffer
    /// without bound; overrun is reported as Truncated and the decoder
    /// resynchronises on the next delimiter.
    static constexpr std::size_t kMaxFrameBytes = 70 * 1024;

    /// Result of feeding one byte.
    enum class Feed {
        NeedMore,    ///< byte consumed, frame not complete yet
        Frame,       ///< a complete frame is available via `frame()`
        Truncated,   ///< the frame in flight was malformed — see below
    };

    /// Append `n` bytes of `p` to `out` as one complete SLIP frame
    /// (delimiter, escaped body, delimiter). Does NOT clear `out`, so a
    /// caller can build several frames back to back into one write buffer.
    static void encode(const uint8_t* p, std::size_t n,
                       std::vector<uint8_t>& out)
    {
        out.reserve(out.size() + n + 2);
        out.push_back(kEnd);
        for (std::size_t i = 0; i < n; ++i) {
            const uint8_t b = p[i];
            if (b == kEnd) {
                out.push_back(kEsc);
                out.push_back(kEscEnd);
            } else if (b == kEsc) {
                out.push_back(kEsc);
                out.push_back(kEscEsc);
            } else {
                out.push_back(b);
            }
        }
        out.push_back(kEnd);
    }

    static void encode(const std::vector<uint8_t>& body,
                       std::vector<uint8_t>& out)
    { encode(body.data(), body.size(), out); }

    /// Feed one received byte.
    ///
    /// `Truncated` means the byte stream violated the framing — a delimiter
    /// arrived in the middle of an escape sequence (exactly what an Apple II
    /// reset mid-transmission looks like), an escape was followed by a byte
    /// that is neither $DC nor $DD, or the body outgrew `kMaxFrameBytes`. In
    /// every case the partial body is dropped and the decoder is left ready
    /// for the NEXT frame, so a caller can log and carry on rather than tear
    /// the link down.
    Feed feed(uint8_t b)
    {
        // The frame handed out by the last `Feed::Frame` stays readable until
        // this call — `frame()` documents exactly that — so a delimiter that
        // both closes one frame and opens the next cannot clear the body on
        // the spot. It arms this instead, and the clear happens here.
        if (openedByCloser_) { body_.clear(); openedByCloser_ = false; }

        switch (state_) {
        case State::Idle:
            // Outside a frame: everything but a delimiter is line noise
            // (or the tail of a frame we already gave up on).
            if (b == kEnd) { body_.clear(); state_ = State::Body; }
            return Feed::NeedMore;

        case State::Body:
            if (b == kEnd) {
                // A delimiter closes the frame — unless nothing has arrived
                // yet, in which case this is either the encoder's leading
                // delimiter or two frames' delimiters back to back. Both are
                // legal and neither is an empty packet.
                if (body_.empty()) return Feed::NeedMore;
                // It also OPENS the next one. RFC 1055 lets a sender put a
                // single END between packets instead of a closing one and an
                // opening one ("if there is no data waiting... the END is
                // sent once"), and POM2 has to decode what a peer sends, not
                // what its own encoder emits: dropping to Idle here threw the
                // whole of `C0 body1 C0 body2 C0`'s second packet away — the
                // body was skipped as inter-frame noise and its closing
                // delimiter merely opened the next frame, so the link lost a
                // packet and then answered every later request one behind.
                // The price is that bytes between two frames are now a
                // (runt) frame rather than skipped noise, which is what RFC
                // 1055 says they are.
                openedByCloser_ = true;
                return Feed::Frame;
            }
            if (b == kEsc) { state_ = State::Escape; return Feed::NeedMore; }
            if (body_.size() >= kMaxFrameBytes) return giveUp();
            body_.push_back(b);
            return Feed::NeedMore;

        case State::Escape:
            if (b == kEscEnd) {
                if (body_.size() >= kMaxFrameBytes) return giveUp();
                body_.push_back(kEnd);
                state_ = State::Body;
                return Feed::NeedMore;
            }
            if (b == kEscEsc) {
                if (body_.size() >= kMaxFrameBytes) return giveUp();
                body_.push_back(kEsc);
                state_ = State::Body;
                return Feed::NeedMore;
            }
            if (b == kEnd) {
                // Frame ended mid-escape. This is THE case the spec wants
                // detected: an Apple II reset during transmission. The
                // delimiter also opens the next frame, so resync onto it
                // rather than dropping back to Idle and losing a packet.
                body_.clear();
                state_ = State::Body;
                return Feed::Truncated;
            }
            // $DB followed by anything else is not SLIP. Drop the frame and
            // wait for a clean delimiter.
            return giveUp();
        }
        return Feed::NeedMore;   // unreachable; keeps every compiler quiet
    }

    /// The frame body decoded by the `Feed::Frame` that just returned.
    /// Valid until the next `feed()`.
    const std::vector<uint8_t>& frame() const { return body_; }

    /// Drop any partial frame and return to "waiting for a delimiter".
    /// Called when a transport reconnects, so bytes from the previous peer
    /// cannot glue themselves to the first packet of the new one.
    void reset()
    {
        body_.clear();
        openedByCloser_ = false;
        state_ = State::Idle;
    }

private:
    enum class State { Idle, Body, Escape };

    Feed giveUp()
    {
        body_.clear();
        openedByCloser_ = false;
        state_ = State::Idle;
        return Feed::Truncated;
    }

    std::vector<uint8_t> body_;
    State                state_ = State::Idle;
    /// Set by the delimiter that closed a frame AND opened the next one: the
    /// body it delivered has to outlive that call, so the clear is deferred
    /// to the next feed().
    bool                 openedByCloser_ = false;
};

} // namespace pom2

#endif // POM2_SLIP_FRAMER_H
