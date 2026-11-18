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

// HostMidiPort with no backend (the browser, a Linux build without ALSA):
// nothing to list, nothing opens. See HostMidiPort.h.

#include "HostMidiPort.h"

namespace pom2 {

struct HostMidiPort::Impl {};

HostMidiPort::HostMidiPort() = default;
HostMidiPort::~HostMidiPort() = default;

std::vector<std::string> HostMidiPort::list() { return {}; }
const char* HostMidiPort::backend() { return "none"; }

bool HostMidiPort::open(const std::string& name, std::string& error)
{
    error = "this build has no host MIDI output (\"" + name + "\" unavailable); "
            "record a .mid instead";
    return false;
}

void HostMidiPort::close() { name_.clear(); }
bool HostMidiPort::isOpen() const { return false; }
void HostMidiPort::send(const uint8_t*, std::size_t) {}

} // namespace pom2
