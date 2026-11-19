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

// TestPortCard — see the header. A POM2 convention, not hardware: there is
// no evidence grade to cite because there is no silicon to be faithful to.

#include "TestPortCard.h"

namespace pom2 {

TestPortCard::TestPortCard(int slot)
    : slot_(slot)
{
}

void TestPortCard::endLine()
{
    lines_.push_back(pending_);
    if (sink_) sink_(pending_);
    pending_.clear();
}

void TestPortCard::finish(int status)
{
    if (!pending_.empty()) endLine();
    status_   = status;
    finished_ = true;
}

void TestPortCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (finished_) return;
    switch (low4) {
        case kLog: {
            const uint8_t c = static_cast<uint8_t>(v & 0x7F);
            if (c == '\n' && lastWasCr_) { lastWasCr_ = false; return; }   // CR LF = one end
            lastWasCr_ = (c == '\r');
            if (c == '\r' || c == '\n') { endLine(); return; }
            pending_.push_back(static_cast<char>(c));
            return;
        }
        case kExit:
            finish(v);
            return;
        case kAssert:
            ++asserts_;
            if (v != 0) { ++passed_; return; }
            pending_ = "ASSERT #" + std::to_string(asserts_) + " FAILED"
                     + (pending_.empty() ? std::string() : ": " + pending_);
            lastWasCr_ = false;
            finish(kAssertFailedStatus);
            return;
        default:
            return;   // $C0n3-$C0nF: not decoded
    }
}

} // namespace pom2
