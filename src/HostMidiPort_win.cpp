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

// HostMidiPort on Windows — WinMM's midiOut devices (the "Microsoft GS
// Wavetable Synth" is always one of them). See HostMidiPort.h.
// Short messages go through midiOutShortMsg; a SysEx through a prepared
// MIDIHDR and midiOutLongMsg, waited for before its buffer is released.

#include "HostMidiPort.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <chrono>
#include <thread>

namespace pom2 {

namespace {
std::string deviceName(UINT id)
{
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(id, &caps, sizeof caps) != MMSYSERR_NOERROR) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(std::size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, out.data(), n, nullptr, nullptr);
    return out;
}
} // namespace

struct HostMidiPort::Impl {
    HMIDIOUT out = nullptr;
};

HostMidiPort::HostMidiPort() : impl_(std::make_unique<Impl>()) {}
HostMidiPort::~HostMidiPort() { close(); }

const char* HostMidiPort::backend() { return "WinMM"; }

std::vector<std::string> HostMidiPort::list()
{
    std::vector<std::string> names;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        std::string name = deviceName(i);
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

bool HostMidiPort::open(const std::string& name, std::string& error)
{
    close();
    error.clear();
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        if (deviceName(i) != name) continue;
        HMIDIOUT h = nullptr;
        if (midiOutOpen(&h, i, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            error = "the MIDI output \"" + name + "\" could not be opened (in use?)";
            return false;
        }
        impl_->out = h;
        name_ = name;
        return true;
    }
    error = "no MIDI output named \"" + name + "\"";
    return false;
}

void HostMidiPort::close()
{
    if (impl_->out) {
        midiOutReset(impl_->out);
        midiOutClose(impl_->out);
        impl_->out = nullptr;
    }
    name_.clear();
}

bool HostMidiPort::isOpen() const { return impl_->out != nullptr; }

void HostMidiPort::send(const uint8_t* bytes, std::size_t len)
{
    if (!impl_->out || !bytes || len == 0) return;
    if (bytes[0] != 0xF0) {
        DWORD msg = bytes[0];
        if (len > 1) msg |= DWORD(bytes[1]) << 8;
        if (len > 2) msg |= DWORD(bytes[2]) << 16;
        midiOutShortMsg(impl_->out, msg);
        return;
    }
    std::vector<char> data(bytes, bytes + len);
    MIDIHDR hdr{};
    hdr.lpData = data.data();
    hdr.dwBufferLength = DWORD(len);
    hdr.dwBytesRecorded = DWORD(len);
    if (midiOutPrepareHeader(impl_->out, &hdr, sizeof hdr) != MMSYSERR_NOERROR) return;
    if (midiOutLongMsg(impl_->out, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
        // The buffer must outlive the transfer: 31 250 baud, 320 µs a byte,
        // so even a 64 KB SysEx is done in ~21 s. Bounded wait.
        for (int i = 0; i < 25000 && !(hdr.dwFlags & MHDR_DONE); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    midiOutUnprepareHeader(impl_->out, &hdr, sizeof hdr);
}

} // namespace pom2
