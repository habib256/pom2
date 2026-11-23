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

#pragma once

#include <pom2/core.hpp>

#include "Apple2Display.h"
#include "AudioMix.h"
#include "CassetteDevice.h"
#include "Disk35Image.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryWatchSink.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "SpeakerDevice.h"

class DiskIICard;
class EzColorCard;
class IIcMouse;
class LeChatMauveCard;
class MockingboardCard;
class MouseCard;
class MouseCardAppleWin;

namespace pom2 {

class IIcExternalSmartPort;
class MountableMediaCard;
class SmartPortCard;

// Private SDK implementation shared by the facade and its snapshot codec.
class Core::Impl {
public:
    explicit Impl(const CoreConfig& requested);
    ~Impl();

    bool iicClass() const;
    bool canSnapshot() const;
    void wireIIcPlus();
    void tickIwm();
    static CoreConfig resolve(CoreConfig config);

    // The //c+'s disk hardware, Machine::AppleIIcPlus only: allocated there
    // and nowhere else, so that no other Core builds a Disk35Image (whose
    // first construction fixes the process-wide POM2_MEDIA_WRITE_DEFAULT
    // reading). Declared before `memory`, so it outlives it: Memory and its
    // //c-class profile hold pointers into it.
    struct IIcPlusDisks {
        pom2::IWMDevice    iwm;
        pom2::SmartPortHub hub;
        pom2::Disk35Image  image35[2];            // internal, external
        pom2::Sony35Drive  drive35[2];
        bool               external35 = false;    // on the chain yet
        std::uint64_t      lastIwmTick = 0;
    };
    std::unique_ptr<IIcPlusDisks> plus;
    // The plain //c's rear connector, once a hard disk hangs off it. Declared
    // before `memory` for the same reason: its profile holds a pointer to it.
    std::unique_ptr<pom2::IIcExternalSmartPort> iicRearPort;
    Memory memory;
    M6502  cpu;
    Apple2Display display;
    SpeakerDevice speaker;
    CassetteDevice cassette;
    DiskIICard* diskII = nullptr; // non-owning; its slot owns the card
    SmartPortCard* smartPort = nullptr; // non-owning; its slot owns the card
    // attachHardDisk's device, a ProDOSHardDiskCard or (//c) a SmartPortCard
    // of HDV units: non-owning, and the same two bays either way.
    pom2::MountableMediaCard* hardDisk = nullptr;
    int hardDiskSlot = 0;
    // Non-owning; their slots own them. A LIST, not one pointer: `pullAudio`
    // mixes every card on the bus, so a second attached card was being mixed
    // at a rate and volume it was never told about — `attachMockingboard`
    // simply overwrote the single pointer and the earlier card kept 44 100
    // while the mixer consumed 48 000.
    std::vector<MockingboardCard*> mockingboards;
    // Non-owning; the slot owns the card. One of the two, or neither.
    MouseCard*         mouseMcu = nullptr;
    MouseCardAppleWin* mouseHle = nullptr;
    IIcMouse*          mouseIic = nullptr;   // the //c's built-in one
    Machine            machine  = Machine::Custom;
    bool               iieMemory = false;
    LeChatMauveCard*   rgbCard  = nullptr;   // non-owning; its slot owns it
    EzColorCard*       ezColor  = nullptr;   // non-owning; its slot owns it
    std::vector<std::uint32_t> ezFrame;      // renderEzColorFrame()'s pixels
    // setBusWatch: Memory reports each watched access here. Only the CPU's
    // accesses go out — `cpuRunning` is set across run()/step(), so the
    // host's read()/write() and the facade's own peeks (boot signatures,
    // the mouse's screen holes) stay silent.
    struct BusWatchSink final : MemoryWatchSink {
        Impl* owner = nullptr;
        void noteAccess(std::uint16_t addr, std::uint8_t value, bool write) override
        {
            if (owner->cpuRunning && owner->busWatch)
                owner->busWatch(addr, value, write, owner->memory.accessCycle());
        }
    } busWatchSink;
    Core::BusWatch     busWatch;
    bool               cpuRunning = false;
    int                mouseSlot = 0;
    std::string        mouseRomIdentity;
    std::uint8_t       mouseRawX = 0, mouseRawY = 0;   // the host's running counters
    // setMouse on the MCU card: where the screen holes stood when the last
    // correction went out. Until they move, that correction is not published.
    bool               mousePushOut = false;
    int                mousePushHoleX = 0, mousePushHoleY = 0;
    // Calls spent waiting on a push the holes never showed: one that could
    // not move the cursor (target past the clamp, mouse off, a guest that
    // stopped reading) latched setMouse false for the whole session.
    int                mousePushWaits = 0;
    ::VideoStandard    videoStandard = ::VideoStandard::NTSC;
    double cpuClockHz = static_cast<double>(POM2_CPU_CLOCK_HZ);
    std::uint32_t audioSampleRate = 44100;
    std::vector<float> speakerScratch;
    std::vector<float> cassetteScratch;
    std::vector<float> leftScratch;
    std::vector<float> rightScratch;
    /// The bus this core mixes into — the same law the emulator's own audio
    /// device plays through (`AudioMix.h`). `mixList` is refilled per buffer
    /// and keeps its capacity, so the audio path stays allocation-free.
    pom2::MixBusState         mixBus;
    std::vector<AudioSource*> mixList;
    std::string error;
};

}  // namespace pom2
