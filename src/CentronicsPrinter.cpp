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

#include "CentronicsPrinter.h"

#include <algorithm>

namespace pom2 {

bool CentronicsPrinter::strobe(uint8_t byte)
{
    if (accepts()) {
        print(byte);
        return true;
    }
    pending_ = true;
    pendingByte_ = byte;
    return false;
}

void CentronicsPrinter::release()
{
    if (!pending_ || !accepts()) return;
    pending_ = false;
    print(pendingByte_);
    if (onAck_) onAck_();
}

void CentronicsPrinter::print(uint8_t byte)
{
    std::lock_guard<std::mutex> lk(spoolMtx_);
    if (spool_.size() == kMaxSpoolBytes) {
        spool_.pop_front();
        ++spoolBase_;
    }
    spool_.push_back(byte);
}

std::size_t CentronicsPrinter::drainSpoolFrom(std::size_t from,
                                              std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(spoolMtx_);
    const std::size_t total = spoolBase_ + spool_.size();
    std::size_t start = (from > total) ? spoolBase_ : std::max(from, spoolBase_);
    start -= spoolBase_;
    out.insert(out.end(), spool_.begin() + static_cast<std::ptrdiff_t>(start),
               spool_.end());
    return total;
}

std::size_t CentronicsPrinter::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(spoolMtx_);
    return spoolBase_ + spool_.size();
}

}  // namespace pom2
