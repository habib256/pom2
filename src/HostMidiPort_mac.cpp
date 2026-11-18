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

// HostMidiPort on macOS — see HostMidiPort.h. Three kinds of output:
//   * kDlsName: Apple's DLS synthesizer AudioUnit (kAudioUnitSubType_
//     DLSSynth) connected to the default output — General MIDI with nothing
//     installed. It plays through its own output unit, not POM2's mixer.
//   * kVirtualName: a CoreMIDI SOURCE named "POM2 Passport MIDI". GarageBand
//     and Logic listen to every source, so this is how they hear POM2.
//   * every CoreMIDI destination by display name (Munt MT-32, an interface).
// The MIDIPacketList calls are the classic API; macOS 11 added the UMP
// MIDIEventList one beside it. Their deprecation markings are silenced
// here, on purpose and only here: the byte-stream API is exactly what a
// MIDI 1.0 card produces.

#include "HostMidiPort.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreMIDI/CoreMIDI.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace pom2 {

namespace {
constexpr const char* kDlsName = "Apple DLS synthesizer (built-in)";
constexpr const char* kVirtualName = "Virtual source \"POM2 Passport MIDI\"";

std::string cfString(CFStringRef s)
{
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

std::string endpointName(MIDIEndpointRef ep)
{
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) != noErr || !name)
        return {};
    std::string out = cfString(name);
    CFRelease(name);
    return out;
}
} // namespace

struct HostMidiPort::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef   port = 0;
    MIDIEndpointRef dest = 0;
    MIDIEndpointRef source = 0;      // the virtual source, when that is the output
    AudioUnit synth = nullptr;
    AudioUnit output = nullptr;

    bool ensureClient(std::string& error)
    {
        if (client) return true;
        if (MIDIClientCreate(CFSTR("POM2"), nullptr, nullptr, &client) != noErr) {
            error = "CoreMIDI client could not be created";
            client = 0;
            return false;
        }
        if (MIDIOutputPortCreate(client, CFSTR("POM2 MIDI out"), &port) != noErr) {
            error = "CoreMIDI output port could not be created";
            port = 0;
            return false;
        }
        return true;
    }

    bool openDls(std::string& error)
    {
        AudioComponentDescription sd{kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth,
                                     kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponentDescription od{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                     kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent sc = AudioComponentFindNext(nullptr, &sd);
        AudioComponent oc = AudioComponentFindNext(nullptr, &od);
        if (!sc || !oc || AudioComponentInstanceNew(sc, &synth) != noErr ||
            AudioComponentInstanceNew(oc, &output) != noErr) {
            error = "the DLS synthesizer is not available";
            closeDls();
            return false;
        }
        AudioUnitConnection link{synth, 0, 0};
        if (AudioUnitSetProperty(output, kAudioUnitProperty_MakeConnection, kAudioUnitScope_Input,
                                 0, &link, sizeof link) != noErr ||
            AudioUnitInitialize(synth) != noErr || AudioUnitInitialize(output) != noErr ||
            AudioOutputUnitStart(output) != noErr) {
            error = "the DLS synthesizer could not be started";
            closeDls();
            return false;
        }
        return true;
    }

    void closeDls()
    {
        if (output) { AudioOutputUnitStop(output); AudioUnitUninitialize(output);
                      AudioComponentInstanceDispose(output); output = nullptr; }
        if (synth)  { AudioUnitUninitialize(synth); AudioComponentInstanceDispose(synth); synth = nullptr; }
    }

    void closeAll()
    {
        closeDls();
        if (source) { MIDIEndpointDispose(source); source = 0; }
        dest = 0;
    }

    ~Impl()
    {
        closeAll();
        if (port) MIDIPortDispose(port);
        if (client) MIDIClientDispose(client);
    }
};

HostMidiPort::HostMidiPort() : impl_(std::make_unique<Impl>()) {}
HostMidiPort::~HostMidiPort() = default;

const char* HostMidiPort::backend() { return "CoreMIDI"; }

std::vector<std::string> HostMidiPort::list()
{
    std::vector<std::string> out = { kDlsName, kVirtualName };
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        const std::string name = endpointName(MIDIGetDestination(i));
        if (!name.empty()) out.push_back(name);
    }
    return out;
}

bool HostMidiPort::open(const std::string& name, std::string& error)
{
    close();
    error.clear();
    if (name == kDlsName) {
        if (!impl_->openDls(error)) return false;
        name_ = name;
        return true;
    }
    if (!impl_->ensureClient(error)) return false;
    if (name == kVirtualName) {
        if (MIDISourceCreate(impl_->client, CFSTR("POM2 Passport MIDI"), &impl_->source) != noErr) {
            error = "the virtual MIDI source could not be created";
            impl_->source = 0;
            return false;
        }
        name_ = name;
        return true;
    }
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        if (endpointName(ep) == name) {
            impl_->dest = ep;
            name_ = name;
            return true;
        }
    }
    error = "no MIDI output named \"" + name + "\"";
    return false;
}

void HostMidiPort::close()
{
    impl_->closeAll();
    name_.clear();
}

bool HostMidiPort::isOpen() const
{
    return impl_->synth || impl_->source || impl_->dest;
}

void HostMidiPort::send(const uint8_t* bytes, std::size_t len)
{
    if (!bytes || len == 0) return;
    if (impl_->synth) {
        if (bytes[0] == 0xF0) MusicDeviceSysEx(impl_->synth, bytes, UInt32(len));
        else if (bytes[0] < 0xF0)
            MusicDeviceMIDIEvent(impl_->synth, bytes[0], len > 1 ? bytes[1] : 0,
                                 len > 2 ? bytes[2] : 0, 0);
        return;
    }
    if (!impl_->source && !impl_->dest) return;
    // A packet list big enough for the longest SysEx the assembler passes.
    std::vector<uint8_t> storage(len + 256);
    auto* list = reinterpret_cast<MIDIPacketList*>(storage.data());
    MIDIPacket* pkt = MIDIPacketListInit(list);
    pkt = MIDIPacketListAdd(list, storage.size(), pkt, 0, len, bytes);
    if (!pkt) return;
    if (impl_->source) MIDIReceived(impl_->source, list);
    else MIDISend(impl_->port, impl_->dest, list);
}

} // namespace pom2

#pragma clang diagnostic pop
