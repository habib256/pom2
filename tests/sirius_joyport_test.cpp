// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#include "Memory.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "SnapshotIO.h"
#include <cassert>
#include <vector>

using S = pom2::SiriusJoyport;
using Blob = std::vector<uint8_t>;
static unsigned pb(Memory& m, int button) { return m.memRead(0xC061 + button) & 0x80; }
static Blob state(Memory& m) { Blob b; m.appendSnapshotState(b); return b; }

int main()
{
    Memory m;
    for (bool iie : {false, true}) {
        m.setIIEMode(iie);
        m.setJoyportMode(S::Centre);
        m.setJoyportContacts(0, S::Leftward | S::Up | S::Fire);
        m.setJoyportContacts(1, S::Rightward | S::Down);
        // The manual's BASIC scanner: select left, horizontal, then vertical.
        m.memWrite(0xC058, 0xFF); m.memWrite(0xC05A, 0xFF);
        assert(pb(m, 0) == 0 && pb(m, 1) == 0 && pb(m, 2) == 0x80);
        (void)m.memRead(0xC05B);
        assert(pb(m, 1) == 0 && pb(m, 2) == 0x80);
        // No host poll between switches: CPU reads must select immediately.
        (void)m.memRead(0xC059);
        assert(pb(m, 0) == 0x80 && pb(m, 1) == 0x80 && pb(m, 2) == 0);
        (void)m.memRead(0xC05A);
        assert(pb(m, 1) == 0x80 && pb(m, 2) == 0);
        for (int b = 0; b < 3; ++b) {
            const auto floating = m.peekFloatingBus() & 127;
            assert(m.memRead(0xC069 + b) == (pb(m, b) | floating));
        }
        // Writes to inputs have no effect; the switch overrides software.
        m.memWrite(0xC062, 0);
        m.setJoyportMode(S::Left);
        assert(pb(m, 0) == 0 && pb(m, 1) == 0);
        m.setJoyportMode(S::Right); (void)m.memRead(0xC058);
        assert(pb(m, 0) == 0x80 && pb(m, 1) == 0x80);
        // No rear pots: the paddle timer stays high even long after trigger.
        for (int p = 0; p < 4; ++p) m.setPaddle(p, 0);
        (void)m.memRead(0xC070); m.setCycleCounter(m.getCycleCounter() + 100000);
        for (int p = 0; p < 4; ++p) assert(m.memRead(0xC064 + p) & 0x80);
        // Reset affects motherboard ANs, never disconnects an attached Joyport.
        m.setJoyportMode(S::Centre); (void)m.memRead(0xC059);
        m.resetSoftSwitches();
        assert(m.joyportActive() && pb(m, 0) == 0 && pb(m, 1) == 0);
        m.setJoyportContacts(0, 0);
        assert(pb(m, 0) == 0x80 && pb(m, 1) == 0x80); // IIe self-test condition
        m.setGamePortConnected(false); assert(!m.joyportActive());
        m.setGamePortConnected(true);
    }
    // Saving between axis scans restores the selected socket and contacts.
    m.setJoyportMode(S::Centre); m.setJoyportContacts(1, S::Down);
    (void)m.memRead(0xC059); (void)m.memRead(0xC05B);
    const Blob golden = state(m);
    m.setJoyportMode(S::Off); m.setJoyportContacts(1, 0); (void)m.memRead(0xC058);
    assert(m.loadSnapshotState(golden.data(), golden.size()));
    assert(m.joyportMode() == S::Centre && pb(m, 1) == 0x80 && pb(m, 2) == 0);
    for (int field = 0; field < 5; ++field) {
        Blob bad = golden; bad[bad.size() - 5 + field] = 0xFF;
        assert(!m.loadSnapshotState(bad.data(), bad.size()));
        assert(state(m) == golden); // malformed peripheral never mutates RAM/latches
    }
    Blob old = golden; old.resize(old.size() - 9);
    assert(m.loadSnapshotState(old.data(), old.size()));
    assert(m.joyportMode() == S::Off);

    m.setJoyportMode(S::Left); m.setGamePortConnected(false);
    const auto unplugged = state(m);
    m.setGamePortConnected(true);
    assert(m.loadSnapshotState(unplugged.data(), unplugged.size()));
    assert(!m.gamePortConnected() && !m.joyportActive());
    m.setGamePortConnected(true);

    // The machine snapshot path (files and rewind) carries the same state.
    M6502 cpu(&m);
    for (bool rewind : {false, true}) {
        m.setJoyportMode(S::Right); m.setJoyportContacts(1, S::Fire);
        Blob saved; pom2::SnapshotWriter writer(saved);
        pom2::captureMachineState(writer, cpu, m, rewind); assert(writer.finish());
        m.setJoyportMode(S::Off);
        pom2::SnapshotReader reader(saved.data(), saved.size());
        assert(pom2::restoreMachineState(reader, cpu, m).ok);
        assert(m.joyportMode() == S::Right && pb(m, 0) == 0);
    }
    // A //c's $C05x are IOU controls, not a connector for this peripheral.
    Memory c;
    c.setIIEMode(true);
    Memory::AppleIIRomImage rom; rom.bytes.resize(0x4000, 0xEA);
    rom.bytes[0x3BC0] = 0; // motherboard firmware's //c identification byte
    assert(c.loadAppleIIRom(rom, true));
    assert(c.isIIcClass());
    c.setJoyportMode(S::Left); c.setJoyportContacts(0, S::Fire);
    assert(!c.joyportActive() && pb(c, 0) == 0);
}
