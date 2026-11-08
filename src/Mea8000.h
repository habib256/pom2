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

// POM2 adapter for Sébastien Pourcel's PorteParole-MEA8000, GPL-2.0-or-later.
// The unmodified synthesis core lives in third_party/porteparole-mea8000/.
// Timing, excitation, resonators and analogue output follow that reference.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pom2 {
class Mea8000 {
public:
    static constexpr uint32_t kClockHz = 4000000;
    static constexpr uint32_t kOutputRate = 48000;
    enum class State : uint8_t { Stopped, WaitFirst, Started, Slowing };
    Mea8000();
    ~Mea8000();
    Mea8000(const Mea8000&);
    Mea8000& operator=(const Mea8000&);
    void reset();
    bool writeData(uint8_t value);
    void writeCommand(uint8_t value);
    uint8_t status() const;
    bool acceptsByte() const { return status() != 0; }
    bool running() const;
    State state() const;
    void advanceClocks(uint64_t clocks, std::vector<int16_t>* output = nullptr);
    // MEA v2: 51 upstream scalar fields, encoded as little-endian IEEE doubles.
    static constexpr std::size_t kSnapshotBytes = 4 + 51 * 8;
    void appendSnapshot(std::vector<uint8_t>& out) const;
    bool loadSnapshot(const uint8_t* data, std::size_t size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pom2
