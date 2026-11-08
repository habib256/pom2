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

// The reference is included only here: public SDK headers stay self-contained.
#include "Mea8000.h"
#include "ByteIO.h"
#include "../third_party/porteparole-mea8000/src/Mea8000.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace pom2 {
struct Mea8000::Impl { ::Mea8000 core; };
Mea8000::Mea8000() : impl_(std::make_unique<Impl>()) {}
Mea8000::~Mea8000() = default;
Mea8000::Mea8000(const Mea8000& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Mea8000& Mea8000::operator=(const Mea8000& other) { *impl_ = *other.impl_; return *this; }
void Mea8000::reset() { impl_->core.Reset(); }
bool Mea8000::writeData(uint8_t value) { return impl_->core.WriteData(value); }
void Mea8000::writeCommand(uint8_t value) { impl_->core.WriteCommand(value); }
uint8_t Mea8000::status() const { return impl_->core.ReadStatus(); }
bool Mea8000::running() const { return impl_->core.Active(); }
Mea8000::State Mea8000::state() const { return static_cast<State>(impl_->core.GetMode()); }
void Mea8000::advanceClocks(uint64_t clocks, std::vector<int16_t>* output)
{
    impl_->core.AdvanceClocks(clocks, output);
}
void Mea8000::appendSnapshot(std::vector<uint8_t>& out) const
{
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    out.insert(out.end(), {'M', 'E', 'A', 2});
    for (double value : impl_->core.SaveState()) {
        uint64_t bits; std::memcpy(&bits, &value, 8);
        byteio::putU64(out, bits);
    }
}
bool Mea8000::loadSnapshot(const uint8_t* data, std::size_t size)
{
    if (!data || size != kSnapshotBytes || data[0] != 'M' || data[1] != 'E' ||
        data[2] != 'A' || data[3] != 2) return false;
    byteio::Reader r(data, size); r.pos = 4;
    std::vector<double> values;
    while (r.has(8)) {
        uint64_t bits = r.u64(); double value; std::memcpy(&value, &bits, 8);
        // Bound corrupted filter histories as well as rejecting NaN/Inf.
        if (!std::isfinite(value) || std::abs(value) > 1e12) return false;
        values.push_back(value);
    }
    // Emscripten builds do not enable C++ exception catching. Validate
    // EVERY integer field before the reference's throwing LoadState, so a
    // malformed browser session is rejected instead of aborting the module.
    for (std::size_t i = 0; i < 15; ++i) {
        const double max = i < 8 ? 4294967295.0 : (i < 11 ? 1.0 : 255.0);
        if (values[i] < 0 || values[i] > max || values[i] != std::floor(values[i])) return false;
    }
    if (values[0] > 3 || values[3] > 4 || values[4] >= 480 ||
        values[5] >= kClockHz || values[6] > 12 || values[2] > 512 ||
        (values[0] >= 2 && (values[2] < 64 || values[1] >= values[2]))) return false;
    // Enforce reachable parameter ranges before the reference's own validation.
    if (values[15] < 0 || values[15] > 510 || values[16] < 0 || values[16] > 510 ||
        values[17] < 0 || values[17] > 1 || values[18] < 0 || values[18] > 1 ||
        values[19] < 0 || values[19] >= 1) return false;
    for (int i = 21; i < 37; ++i)
        if (values[i] < 0 || values[i] > 3500) return false;
    if ((values[0] == 1 && values[3] >= 4) ||
        (values[0] >= 2 && values[2] != 64 && values[2] != 128 &&
         values[2] != 256 && values[2] != 512)) return false;
    try { impl_->core.LoadState(values); }
    catch (const std::runtime_error&) { return false; }
    return true;
}
} // namespace pom2
