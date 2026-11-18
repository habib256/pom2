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

// HostMidiPort — a MIDI output of the HOST: where the Passport MIDI card's
// messages go to become sound, in a synthesizer POM2 does not contain. One
// interface, one file per platform (CMake picks it):
//   macOS    HostMidiPort_mac.cpp   CoreMIDI destinations, a virtual source
//                                   other apps (GarageBand, Logic) can
//                                   listen to, and Apple's built-in DLS
//                                   synthesizer (an AudioUnit) so there is
//                                   sound with nothing installed;
//   Windows  HostMidiPort_win.cpp   WinMM midiOut devices, the Microsoft GS
//                                   Wavetable Synth among them;
//   Linux    HostMidiPort_alsa.cpp  ALSA sequencer ports (a synthesizer such
//                                   as FluidSynth or Munt shows up there),
//                                   when the build found ALSA;
//   else     HostMidiPort_null.cpp  no port; the .mid recording still works.
// Ports are named by their display name, which is what the setting keeps.
// Not thread-safe: MidiOutputCoordinator owns one and calls it from its
// sender thread only (and lists ports from the UI thread, which any backend
// here allows).

#ifndef POM2_HOST_MIDI_PORT_H
#define POM2_HOST_MIDI_PORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pom2 {

class HostMidiPort
{
public:
    HostMidiPort();
    ~HostMidiPort();
    HostMidiPort(const HostMidiPort&) = delete;
    HostMidiPort& operator=(const HostMidiPort&) = delete;

    /// Display names of the outputs available now.
    static std::vector<std::string> list();
    /// What this build's backend is ("CoreMIDI", "WinMM", "ALSA", "none").
    static const char* backend();

    /// Open the output named `name` (closing any other). False + `error`.
    bool open(const std::string& name, std::string& error);
    void close();
    bool isOpen() const;
    const std::string& openName() const { return name_; }

    /// One complete message (MidiMessageAssembler's output).
    void send(const uint8_t* bytes, std::size_t len);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

} // namespace pom2

#endif
