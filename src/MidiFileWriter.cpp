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

// Standard MIDI File recording — see MidiFileWriter.h.

#include "MidiFileWriter.h"

#include "AtomicFileReplace.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

void MidiFileWriter::start(uint64_t startCycle, double cpuHz)
{
    recording_ = true;
    startCycle_ = startCycle;
    if (cpuHz > 0.0) cpuHz_ = cpuHz;
    lastTick_ = 0;
    track_.clear();
    events_ = 0;
}

void MidiFileWriter::discard()
{
    recording_ = false;
    track_.clear();
    events_ = 0;
}

void MidiFileWriter::putVarLen(uint32_t v)
{
    uint8_t buf[5];
    int n = 0;
    buf[n++] = uint8_t(v & 0x7F);
    while (v >>= 7) buf[n++] = uint8_t(0x80 | (v & 0x7F));
    while (n) track_.push_back(buf[--n]);
}

void MidiFileWriter::add(const MidiMessage& m)
{
    if (!recording_ || m.bytes.empty()) return;
    const uint8_t status = m.bytes[0];
    const bool sysEx = status == 0xF0;
    if (status >= 0xF0 && !sysEx) return;               // real-time / system common
    if (!sysEx && m.bytes.size() < 2) return;
    // Absolute ticks from the start, rounded once: deltas never drift.
    const double seconds = m.cycle > startCycle_ ? double(m.cycle - startCycle_) / cpuHz_ : 0.0;
    const uint64_t tick = std::max<uint64_t>(lastTick_, uint64_t(std::llround(seconds * kTicksPerSecond)));
    putVarLen(uint32_t(std::min<uint64_t>(tick - lastTick_, 0x0FFFFFFF)));
    lastTick_ = tick;
    if (sysEx) {
        track_.push_back(0xF0);
        putVarLen(uint32_t(m.bytes.size() - 1));
        track_.insert(track_.end(), m.bytes.begin() + 1, m.bytes.end());
    } else {
        track_.insert(track_.end(), m.bytes.begin(), m.bytes.end());
    }
    ++events_;
}

std::vector<uint8_t> MidiFileWriter::bytes() const
{
    std::vector<uint8_t> trk = {
        0x00, 0xFF, 0x51, 0x03,
        uint8_t(kMicrosPerQuarter >> 16), uint8_t(kMicrosPerQuarter >> 8), uint8_t(kMicrosPerQuarter),
    };
    trk.insert(trk.end(), track_.begin(), track_.end());
    trk.insert(trk.end(), { 0x00, 0xFF, 0x2F, 0x00 });
    std::vector<uint8_t> f = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6,
        0, 0,                                            // format 0
        0, 1,                                            // one track
        uint8_t(kTicksPerQuarter >> 8), uint8_t(kTicksPerQuarter & 0xFF),
        'M', 'T', 'r', 'k',
        uint8_t(trk.size() >> 24), uint8_t(trk.size() >> 16), uint8_t(trk.size() >> 8), uint8_t(trk.size()),
    };
    f.insert(f.end(), trk.begin(), trk.end());
    return f;
}

bool MidiFileWriter::finish(const std::filesystem::path& path, std::string& error)
{
    const std::vector<uint8_t> data = bytes();
    recording_ = false;
    std::error_code ec;
    if (!writeFileAtomic(path, data.data(), data.size(), ec)) {
        error = "cannot write " + path.u8string() + ": " + ec.message();
        return false;
    }
    error.clear();
    return true;
}

} // namespace pom2
