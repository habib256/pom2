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

// HostMidiPort on Linux — the ALSA sequencer. POM2 opens one client
// ("POM2") with one output port and connects it to the destination the user
// picked: a synthesizer (FluidSynth, TiMidity, Munt) or a hardware port.
// Bytes are turned into sequencer events by ALSA's own snd_midi_event
// encoder. Built only when CMake found ALSA (else HostMidiPort_null.cpp).
// See HostMidiPort.h.

#include "HostMidiPort.h"

#include <alsa/asoundlib.h>

namespace pom2 {

namespace {
struct Destination { int client, port; std::string name; };

std::vector<Destination> destinations(snd_seq_t* seq)
{
    std::vector<Destination> out;
    snd_seq_client_info_t* ci = nullptr;
    snd_seq_port_info_t* pi = nullptr;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    const int self = snd_seq_client_id(seq);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        const int client = snd_seq_client_info_get_client(ci);
        if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pi, client);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pi);
            const unsigned want = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
            if ((caps & want) != want) continue;
            out.push_back({ client, snd_seq_port_info_get_port(pi),
                            std::string(snd_seq_client_info_get_name(ci)) + ": " +
                            snd_seq_port_info_get_name(pi) });
        }
    }
    return out;
}
} // namespace

struct HostMidiPort::Impl {
    snd_seq_t* seq = nullptr;
    int port = -1;
    snd_midi_event_t* encoder = nullptr;

    void closeAll()
    {
        if (encoder) { snd_midi_event_free(encoder); encoder = nullptr; }
        if (seq) { snd_seq_close(seq); seq = nullptr; }
        port = -1;
    }
    ~Impl() { closeAll(); }
};

HostMidiPort::HostMidiPort() : impl_(std::make_unique<Impl>()) {}
HostMidiPort::~HostMidiPort() = default;

const char* HostMidiPort::backend() { return "ALSA"; }

std::vector<std::string> HostMidiPort::list()
{
    std::vector<std::string> names;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return names;
    for (auto& d : destinations(seq)) names.push_back(std::move(d.name));
    snd_seq_close(seq);
    return names;
}

bool HostMidiPort::open(const std::string& name, std::string& error)
{
    close();
    error.clear();
    if (snd_seq_open(&impl_->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        impl_->seq = nullptr;
        error = "the ALSA sequencer could not be opened";
        return false;
    }
    snd_seq_set_client_name(impl_->seq, "POM2");
    impl_->port = snd_seq_create_simple_port(impl_->seq, "Passport MIDI",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (impl_->port < 0 || snd_midi_event_new(70 * 1024, &impl_->encoder) < 0) {
        impl_->encoder = nullptr;
        impl_->closeAll();
        error = "the ALSA output port could not be created";
        return false;
    }
    for (const auto& d : destinations(impl_->seq)) {
        if (d.name != name) continue;
        if (snd_seq_connect_to(impl_->seq, impl_->port, d.client, d.port) < 0) {
            impl_->closeAll();
            error = "could not connect to \"" + name + "\"";
            return false;
        }
        name_ = name;
        return true;
    }
    impl_->closeAll();
    error = "no MIDI output named \"" + name + "\"";
    return false;
}

void HostMidiPort::close()
{
    impl_->closeAll();
    name_.clear();
}

bool HostMidiPort::isOpen() const { return impl_->seq != nullptr; }

void HostMidiPort::send(const uint8_t* bytes, std::size_t len)
{
    if (!impl_->seq || !impl_->encoder || !bytes || len == 0) return;
    snd_midi_event_reset_encode(impl_->encoder);
    std::size_t at = 0;
    while (at < len) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        const long used = snd_midi_event_encode(impl_->encoder, bytes + at, long(len - at), &ev);
        if (used <= 0) break;
        at += std::size_t(used);
        if (ev.type == SND_SEQ_EVENT_NONE) continue;
        snd_seq_ev_set_source(&ev, impl_->port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(impl_->seq, &ev);
    }
}

} // namespace pom2
