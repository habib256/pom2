// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
// Optional gameplay regression with user-supplied media. No game/ROM bytes
// embedded here. Addresses are [C] Bandits (Asimov bandits-fixed.zip), whose
// hash and driver disassembly findings are in docs/hardware/sirius-joyport.md.
#include "Memory.h"
#include "M6502.h"
#include "DiskIICard.h"
#include "SlotBus.h"
#include "MachineSnapshot.h"
#include "SnapshotIO.h"
#include "Apple2Display.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using S = pom2::SiriusJoyport;
using Blob = std::vector<uint8_t>;
constexpr int kFrameCycles = 17030;
constexpr uint16_t kX = 0xF7, kShip = 0xF4, kEnergy = 0x8A00, kLives = 0x8FDF;
constexpr std::array<uint16_t, 3> kProjectile{0xF0, 0xD2, 0xD8};

struct Observer final : pom2::MemoryWatchSink {
    M6502* cpu = nullptr;
    bool introWait = false, gameKeyboard = false;
    int shots = 0, buttons = 0, vertical = 0, horizontal = 0;
    std::array<uint8_t, 3> previous{};

    void noteAccess(uint16_t addr, uint8_t value, bool write) override {
        if (!write) {
            if (addr == 0xC000) {
                introWait |= cpu->getProgramCounter() == 0x0F52;
                gameKeyboard |= cpu->getProgramCounter() == 0x0209;
            }
            if (addr >= 0xC061 && addr <= 0xC063) ++buttons;
            if (addr == 0xC05A) ++horizontal;
            if (addr == 0xC05B) ++vertical;
        } else {
            for (size_t i = 0; i < kProjectile.size(); ++i) {
                if (addr != kProjectile[i]) continue;
                // Count the game's launch transition, not each display update
                // or the NMOS RMW dummy write of the old value.
                if (value == 1 && previous[i] == 0) ++shots;
                previous[i] = value;
            }
        }
    }
    void arm(Memory& m, M6502& c) {
        cpu = &c;
        m.setWatchSink(this);
        for (int addr : {0xC000, 0xC05A, 0xC05B, 0xC061, 0xC062, 0xC063})
            m.setReadWatch(addr, true);
        for (size_t i = 0; i < kProjectile.size(); ++i) {
            m.setWriteWatch(kProjectile[i], true);
            previous[i] = m.peekMainRam(kProjectile[i]);
        }
    }
};

bool selectedJoyport(const Memory& m) {
    // The guest installs movement=$03C0, shield=$0235 and active-low fire.
    return m.peekMainRam(0x0422) == 0xC0 && m.peekMainRam(0x0423) == 3 &&
           m.peekMainRam(0x0424) == 0x35 && m.peekMainRam(0x0425) == 2 &&
           m.peekMainRam(0x028B) == 0x30;
}

void screenshot(Memory& m, const std::string& prefix, const char* name) {
    if (prefix.empty()) return;
    Apple2Display display;
    display.render(m);
    const auto* pixels = display.pixels();
    const auto path = prefix + "-" + name + ".ppm";
    FILE* out = std::fopen(path.c_str(), "wb");
    assert(out && "could not create requested screenshot");
    std::fprintf(out, "P6\n%d %d\n255\n", display.width(), display.height());
    for (int i = 0; i < display.width() * display.height(); ++i) {
        const uint8_t rgb[]{uint8_t(pixels[i]), uint8_t(pixels[i] >> 8), uint8_t(pixels[i] >> 16)};
        assert(std::fwrite(rgb, 1, 3, out) == 3);
    }
    assert(std::fclose(out) == 0);
}
}

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "Usage: %s apple2.rom disk2.rom bandits-fixed.dsk [screenshot-prefix]\n", argv[0]);
        return 77;
    }
    const std::string prefix = argc == 5 ? argv[4] : "";
    Memory m;
    m.setIIEMode(false);
    if (!m.loadAppleIIRom(argv[1])) return 1;
    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(argv[2]) || !disk->insertDisk(argv[3])) return 1;
    m.slotBus().plug(6, std::move(disk));
    m.setJoyportMode(S::Left);
    M6502 cpu(&m);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    cpu.hardReset(); m.slotBus().reset();
    Observer observer;
    observer.arm(m, cpu);
    bool sentIntroSpace = false, sentNul = false, ready = false;
    for (int frame = 0; frame < 8000; ++frame) {
        cpu.run(kFrameCycles);
        // The disk's loader accepts Space too. Stop repeating as soon as
        // the title's own keyboard wait appears: Space in play means fire.
        if (!observer.introWait && frame > 600 && frame % 60 == 0) m.queueKey(' ');
        if (observer.introWait && !sentIntroSpace) {
            m.queueKey(' '); sentIntroSpace = true;
        }
        if (observer.gameKeyboard && !sentNul) {
            m.queueKey(0); sentNul = true; // NUL, historically II Ctrl+Shift+P
        }
        if (sentNul && selectedJoyport(m)) {
            std::printf("Bandits entered play and selected Joyport at frame %d\n", frame);
            ready = true; break;
        }
    }
    assert(ready && "named disk did not reach its Joyport driver within the budget");
    // Let the game observe released inputs before the first press. Both
    // fire and shield have release latches; selecting a driver is not reset.
    for (int i = 0; i < 15; ++i) cpu.run(kFrameCycles);
    assert(m.peekMainRam(kShip) == 1 && m.peekMainRam(kEnergy) == 9);
    screenshot(m, prefix, "ready");
    Blob checkpoint;
    pom2::SnapshotWriter writer(checkpoint);
    pom2::captureMachineState(writer, cpu, m, true);
    assert(writer.finish());
    auto restore = [&](int mode) {
        m.setWatchSink(nullptr);
        pom2::SnapshotReader reader(checkpoint.data(), checkpoint.size());
        // Our own in-memory capture: include slot state as rewind does.
        assert(pom2::restoreMachineState(reader, cpu, m, false).ok);
        m.setJoyportMode(mode);
        observer = {}; observer.arm(m, cpu);
    };

    enum Action { Idle, Left, Right, HoldFire, Up, Down, ThreeShots, WrongSocket };
    const char* names[]{"idle", "left", "right", "held fire", "up", "down", "three shots", "other socket"};
    const uint8_t contacts[]{0, S::Leftward, S::Rightward, S::Fire, S::Up, S::Down};
    // Bandits writes AN0 off when selecting Joyport. Centre therefore tests
    // the implemented left convention; it cannot prove physical polarity.
    for (int mode : {int(S::Left), int(S::Right), int(S::Centre)}) {
        const int jack = mode == S::Right ? 1 : 0;
        for (int action = Idle; action <= WrongSocket; ++action) {
            restore(mode);
            const int startX = m.peekMainRam(kX);
            bool shield = false, capturedFire = false;
            for (int frame = 0; frame < 60; ++frame) {
                if (action == WrongSocket) m.setJoyportContacts(jack ^ 1, S::Rightward | S::Fire | S::Up);
                else m.setJoyportContacts(jack, action == ThreeShots
                    ? (frame % 20 < 10 ? S::Fire : 0) : contacts[action]);
                cpu.run(kFrameCycles);
                shield |= m.peekMainRam(kShip) == 0x40;
                if (mode == S::Left && action == HoldFire && observer.shots && !capturedFire) {
                    screenshot(m, prefix, "fire"); capturedFire = true;
                }
            }
            const int endX = m.peekMainRam(kX);
            assert(endX == (action == Left ? 47 : action == Right ? 211 : startX));
            assert(observer.shots == (action == HoldFire ? 1 : action == ThreeShots ? 3 : 0));
            assert(shield == (action == Up));
            assert(observer.buttons > 0 && observer.vertical > 0 && observer.horizontal > 0);
            assert(selectedJoyport(m));
            if (mode == S::Left && action == Up) screenshot(m, prefix, "shield");
            std::printf("mode=%d %-12s x=%d->%d shots=%d shield=%d\n",
                        mode, names[action], startX, endX, observer.shots, shield);
        }
    }

    // Thirty seconds of actual play: shots, shield energy use, losing lives
    // and respawning. No invincibility patch, no injected score or game state.
    restore(S::Left);
    const int initialLives = m.peekMainRam(kLives);
    bool energyUsed = false, dying = false, respawned = false;
    for (int frame = 0; frame < 1800; ++frame) {
        m.setJoyportContacts(0, (frame % 20 < 10 ? S::Fire : 0) |
                               (frame % 300 < 60 ? S::Up : 0));
        cpu.run(kFrameCycles);
        energyUsed |= m.peekMainRam(kEnergy) < 9;
        if (m.peekMainRam(kShip) & 0x80) dying = true;
        else if (dying && m.peekMainRam(kLives) < initialLives) respawned = true;
    }
    assert(observer.shots > 20 && energyUsed && dying && respawned);
    assert(m.peekMainRam(kLives) < initialLives && selectedJoyport(m));
    screenshot(m, prefix, "gameplay");
    std::printf("30-second play: %d shots, lives %d->%d, shield energy used, death and respawn observed\n",
                observer.shots, initialLives, m.peekMainRam(kLives));
    m.setWatchSink(nullptr);
}
