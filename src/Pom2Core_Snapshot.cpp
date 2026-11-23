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

#include "Pom2CoreImpl.h"

#include "DiskIICard.h"
#include "IIcMouse.h"
#include "LeChatMauveCard.h"
#include "MachineSnapshot.h"
#include "MediaWritePolicy.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "SessionIO.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SnapshotIO.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace pom2 {

bool Core::Impl::canSnapshot() const
{
    return !plus && !hardDisk && !ezColor &&
           std::all_of(mockingboards.begin(), mockingboards.end(), [](const auto* card) {
               return card->getVariant() == MockingboardCard::Variant::AC;
           }) && !cassette.hasLoadedTape() &&
           !cassette.hasRecordedTape() && !cassette.isRecordingArmed();
}

namespace {
constexpr uint8_t kSdkStateMagic[] = {'P', '2', 'S', 'D', 'K', 2};
constexpr size_t kMaxSdkStateBytes = 40u << 20;
}

bool Core::supportsState() const
{
    return impl_->canSnapshot();
}

bool Core::saveState(std::vector<std::uint8_t>& out)
{
    auto& state = *impl_;
    if (!state.canSnapshot()) {
        state.error = "SDK snapshots do not cover this peripheral configuration";
        return false;
    }
    std::vector<uint8_t> machine, speaker, keyboard, video, disk, diskIdentity, diskTiming, smartPort, rgb, mb, mouse;
    SnapshotWriter writer(machine);
    captureMachineState(writer, state.cpu, state.memory);
    if (!writer.finish()) {
        state.error = "Cannot capture machine snapshot";
        return false;
    }
    state.speaker.appendSdkState(speaker);
    state.memory.appendSdkKeyboard(keyboard);
    state.memory.appendSdkVideoState(video);
    if (state.diskII) {
        state.diskII->appendSessionIdentity(diskIdentity);
        state.diskII->appendSdkTiming(diskTiming);
        if (!state.diskII->appendSessionState(disk, state.error)) return false;
    }
    if (state.smartPort) {
        smartPort.push_back(static_cast<uint8_t>(state.smartPort->getSlot()));
        std::vector<uint8_t> unit;
        for (size_t i = 0; i < 2; ++i) {
            unit.clear();
            const auto& image = static_cast<const SmartPort35Unit*>(state.smartPort->unit(i))->image();
            if (!image.appendSessionMedia(unit, state.error)) return false;
            sessionio::blob(smartPort, unit);
        }
        unit.clear();
        state.smartPort->appendSnapshotState(unit);
        sessionio::blob(smartPort, unit);
    }
    if (state.rgbCard) {
        rgb.push_back(static_cast<uint8_t>(state.rgbCard->getSlot()));
        rgb.push_back(static_cast<uint8_t>(state.rgbCard->variant()));
        state.rgbCard->appendSnapshotState(rgb);
    }
    for (const auto* card : state.mockingboards) {
        mb.push_back(static_cast<uint8_t>(card->getSlot()));
        std::vector<uint8_t> payload;
        card->appendSnapshotState(payload);
        sessionio::blob(mb, payload);
        payload.clear();
        card->appendSdkAudio(payload);
        sessionio::blob(mb, payload);
    }
    if (state.mouseMcu || state.mouseHle || state.mouseIic) {
        mouse.insert(mouse.end(), {static_cast<uint8_t>(state.mouseMcu ? 1 : state.mouseHle ? 2 : 3),
            static_cast<uint8_t>(state.mouseSlot), state.mouseRawX, state.mouseRawY,
            static_cast<uint8_t>(state.mousePushOut)});
        byteio::putU32(mouse, static_cast<uint32_t>(state.mousePushHoleX));
        byteio::putU32(mouse, static_cast<uint32_t>(state.mousePushHoleY));
        byteio::putU32(mouse, static_cast<uint32_t>(state.mousePushWaits));
        const std::vector<uint8_t> rom(state.mouseRomIdentity.begin(), state.mouseRomIdentity.end());
        sessionio::blob(mouse, rom);
        std::vector<uint8_t> cardState, input;
        if (state.mouseMcu) {
            state.mouseMcu->appendSnapshotState(cardState);
            state.mouseMcu->appendSdkInput(input);
        } else if (state.mouseHle) {
            state.mouseHle->appendSnapshotState(cardState);
            state.mouseHle->appendSdkInput(input);
        } else {
            input.push_back(static_cast<uint8_t>(state.mouseIic->sdkHostButton()));
        }
        sessionio::blob(mouse, cardState);
        sessionio::blob(mouse, input);
    }
    out.assign(std::begin(kSdkStateMagic), std::end(kSdkStateMagic));
    out.push_back(static_cast<uint8_t>(state.machine));
    byteio::putU32(out, state.audioSampleRate);
    const auto paddles = state.memory.sdkPaddleState();
    out.insert(out.end(), paddles.begin(), paddles.end());
    for (const auto* blob : {&machine, &speaker, &keyboard, &video, &diskIdentity,
                             &disk, &diskTiming, &smartPort, &rgb, &mb, &mouse})
        sessionio::blob(out, *blob);
    state.error.clear();
    return true;
}

bool Core::loadState(const void* data, std::size_t size, std::string_view diskBacking,
                     std::string_view smartPortBacking, const std::function<bool()>& beforeRestore)
{
    auto& state = *impl_;
    const auto fail = [&](std::string error) {
        state.error = std::move(error);
        return false;
    };
    if (!data || size < sizeof(kSdkStateMagic) || size > kMaxSdkStateBytes ||
        std::memcmp(data, kSdkStateMagic, sizeof(kSdkStateMagic)) != 0)
        return fail("Invalid SDK snapshot header");
    if (!state.canSnapshot())
        return fail("SDK snapshots do not cover this peripheral configuration");

    sessionio::Reader reader(static_cast<const uint8_t*>(data), size);
    reader.pos = sizeof(kSdkStateMagic);
    if (reader.u8() != static_cast<uint8_t>(state.machine) || reader.u32() != state.audioSampleRate)
        return fail("SDK snapshot machine or sample rate differs");
    if (!reader.need(6)) return fail("Invalid SDK game-port state");
    const auto* paddles = reader.data + reader.pos;
    reader.pos += 6;
    std::vector<uint8_t> machine, speaker, keyboard, video, diskIdentity, disk, diskTiming, smartPort, rgb, mb, mouse;
    if (!reader.blob(machine, 2u << 20) || !reader.blob(speaker, 256u << 10) ||
        !reader.blob(keyboard, 8u << 10) || !reader.blob(video, 4u << 20) ||
        !reader.blob(diskIdentity, 4096) ||
        !reader.blob(disk, 36u << 20) || !reader.blob(diskTiming, 64) ||
        !reader.blob(smartPort, 36u << 20) ||
        !reader.blob(rgb, 64) || !reader.blob(mb, 5u << 20) ||
        !reader.blob(mouse, 16u << 10) || !reader.done())
        return fail("Invalid SDK snapshot section lengths");
    if (!state.speaker.loadSdkState(speaker.data(), speaker.size(), false) ||
        !state.memory.loadSdkKeyboard(keyboard.data(), keyboard.size(), false) ||
        !state.memory.loadSdkPaddles(paddles, 6, false) ||
        !state.memory.loadSdkVideoState(video.data(), video.size(), false))
        return fail("Invalid SDK speaker, keyboard or video state");
    if (bool(state.diskII) != !disk.empty() || bool(state.smartPort) != !smartPort.empty() ||
        bool(state.rgbCard) != !rgb.empty())
        return fail("SDK snapshot slot topology differs");
    if (!state.diskII && (!diskBacking.empty() || !diskIdentity.empty() || !diskTiming.empty()))
        return fail("SDK snapshot contains unexpected Disk II data");
    if (!state.smartPort && !smartPortBacking.empty())
        return fail("SDK snapshot contains unexpected SmartPort backing");

    DiskIICard::SessionRestore prepared;
    if (state.diskII) {
        std::vector<uint8_t> identity;
        state.diskII->appendSessionIdentity(identity);
        if (diskIdentity != identity)
            return fail("SDK snapshot Disk II firmware differs");
        if (!state.diskII->loadSdkTiming(diskTiming.data(), diskTiming.size(), false))
            return fail("Invalid SDK Disk II timing state");
        // Prepare against a clean controller. The live drive can contain
        // pending writes from the future timeline we are about to abandon.
        DiskIICard validator(state.diskII->getSlot());
        validator.installSessionIdentity(identity);
        if (!validator.prepareSessionState(disk.data(), disk.size(), prepared, state.error,
                                           /*allowWoz=*/true, /*allowWritable=*/true))
            return false;
        if (!diskBacking.empty() &&
            (!mediaWritableByDefault() ||
             !DiskIICard::bindSdkBacking(prepared, std::string(diskBacking))))
            return fail("Cannot bind restored Disk II medium to its working copy");
    }
    std::unique_ptr<SmartPortCard> preparedSmartPort;
    std::vector<uint8_t> smartPortRuntime;
    if (state.smartPort) {
        sessionio::Reader units(smartPort.data(), smartPort.size());
        if (units.u8() != state.smartPort->getSlot())
            return fail("SDK snapshot SmartPort slot differs");
        preparedSmartPort = std::make_unique<SmartPortCard>(state.smartPort->getSlot());
        preparedSmartPort->setUnitCount(2);
        for (size_t i = 0; i < 2; ++i) {
            std::vector<uint8_t> payload;
            if (!units.blob(payload, (16u << 20) + 32)) return fail("Invalid SDK SmartPort disk length");
            auto unit = std::make_unique<SmartPort35Unit>();
            if (!unit->image().loadSessionMedia(payload.data(), payload.size(), state.error, true))
                return false;
            if (i == 0 && !smartPortBacking.empty() &&
                (!mediaWritableByDefault() ||
                 !unit->image().bindSnapshotBacking(std::string(smartPortBacking))))
                return fail("Cannot bind restored SmartPort medium to its working copy");
            preparedSmartPort->setUnit(i, std::move(unit));
        }
        if (!units.blob(smartPortRuntime, 96u << 10) || !units.done())
            return fail("Invalid SDK SmartPort controller length");
        // Each runtime tail holds the medium paths. These are the only
        // fields changed by reconstructing the paired, embedded media.
        constexpr auto identitySize = 8 * SmartPortCard::kMaxUnits;
        std::vector<uint8_t> canonical;
        preparedSmartPort->appendSnapshotState(canonical);
        if (smartPortRuntime.size() < canonical.size())
            return fail("Invalid SDK SmartPort controller state");
        std::copy(canonical.end() - identitySize, canonical.end(), smartPortRuntime.end() - identitySize);
        preparedSmartPort->loadSnapshotState(smartPortRuntime.data(), smartPortRuntime.size());
        canonical.clear();
        preparedSmartPort->appendSnapshotState(canonical);
        if (canonical != smartPortRuntime) return fail("Invalid SDK SmartPort controller state");
    }
    if (state.rgbCard) {
        std::vector<uint8_t> live;
        state.rgbCard->appendSnapshotState(live);
        if (rgb.size() != live.size() + 2 || rgb[0] != state.rgbCard->getSlot() ||
            rgb[1] != static_cast<uint8_t>(state.rgbCard->variant()) ||
            !std::equal(rgb.begin() + 2, rgb.begin() + 5, live.begin()))
            return fail("Invalid SDK RGB card state");
    }

    struct MockingboardState {
        std::vector<uint8_t> registers;
        std::vector<uint8_t> timeline;
    };
    std::vector<MockingboardState> boardStates(state.mockingboards.size());
    sessionio::Reader boardReader(mb.data(), mb.size());
    for (size_t i = 0; i < boardStates.size(); ++i) {
        auto& board = boardStates[i];
        auto* live = state.mockingboards[i];
        if (boardReader.u8() != live->getSlot() ||
            !boardReader.blob(board.registers, 1024) ||
            !boardReader.blob(board.timeline, 600u << 10) ||
            !live->loadSdkAudio(board.timeline.data(), board.timeline.size(), false))
            return fail("Invalid SDK Mockingboard state");
        MockingboardCard validator(live->getSlot());
        validator.loadSnapshotState(board.registers.data(), board.registers.size());
        std::vector<uint8_t> canonical;
        validator.appendSnapshotState(canonical);
        if (canonical != board.registers) return fail("Invalid SDK Mockingboard registers");
    }
    if (!boardReader.done()) return fail("SDK snapshot Mockingboard topology differs");

    const uint8_t mouseModel = state.mouseMcu ? 1 : state.mouseHle ? 2 : state.mouseIic ? 3 : 0;
    if (bool(mouseModel) != !mouse.empty()) return fail("SDK snapshot mouse configuration differs");
    uint8_t rawX = 0, rawY = 0, pushActive = 0;
    int32_t pushX = 0, pushY = 0, pushWaits = 0;
    std::vector<uint8_t> mouseRegisters, mouseInput;
    if (mouseModel) {
        sessionio::Reader mouseReader(mouse.data(), mouse.size());
        if (mouseReader.u8() != mouseModel || mouseReader.u8() != state.mouseSlot)
            return fail("SDK snapshot mouse model or slot differs");
        rawX = mouseReader.u8(); rawY = mouseReader.u8(); pushActive = mouseReader.u8();
        pushX = static_cast<int32_t>(mouseReader.u32()); pushY = static_cast<int32_t>(mouseReader.u32());
        pushWaits = static_cast<int32_t>(mouseReader.u32());
        std::vector<uint8_t> rom;
        if (pushActive > 1 || pushWaits < 0 || pushWaits > 30 || !mouseReader.blob(rom, 128) ||
            std::string(rom.begin(), rom.end()) != state.mouseRomIdentity ||
            !mouseReader.blob(mouseRegisters, 8192) || !mouseReader.blob(mouseInput, 128) || !mouseReader.done())
            return fail("Invalid SDK mouse state");
        std::vector<uint8_t> canonical;
        if (state.mouseMcu) {
            MouseCard validator(state.mouseSlot);
            validator.loadSnapshotState(mouseRegisters.data(), mouseRegisters.size());
            validator.appendSnapshotState(canonical);
            if (!state.mouseMcu->loadSdkInput(mouseInput.data(), mouseInput.size(), false))
                return fail("Invalid SDK Mouse Card input");
        } else if (state.mouseHle) {
            MouseCardAppleWin validator(state.mouseSlot);
            validator.loadSnapshotState(mouseRegisters.data(), mouseRegisters.size());
            validator.appendSnapshotState(canonical);
            if (!state.mouseHle->loadSdkInput(mouseInput.data(), mouseInput.size(), false))
                return fail("Invalid SDK AppleWin Mouse Card input");
        } else if (mouseInput.size() != 1 || mouseInput[0] > 1) {
            return fail("Invalid SDK //c mouse input");
        }
        if (canonical != mouseRegisters) return fail("Invalid SDK mouse registers");
    }

    SnapshotReader machineReader(machine.data(), machine.size());
    std::string section;
    uint32_t sectionSize = 0;
    if (!machineReader.nextSection(section, sectionSize) || section != "ENV")
        return fail("SDK snapshot must name its firmware");
    SnapshotReader machinePayload(machine.data(), machine.size());
    const auto restored = restoreMachineState(machinePayload, state.cpu, state.memory,
                                              /*transactional=*/true, /*logSuccess=*/false,
                                              beforeRestore);
    if (!restored.ok) return fail(restored.error);
    if (state.diskII) {
        state.diskII->applySessionState(std::move(prepared));
        state.diskII->loadSdkTiming(diskTiming.data(), diskTiming.size(), true);
    }
    if (preparedSmartPort) {
        for (size_t i = 0; i < 2; ++i)
            state.smartPort->setUnit(i, preparedSmartPort->setUnit(i, nullptr));
        state.smartPort->loadSnapshotState(smartPortRuntime.data(), smartPortRuntime.size());
    }
    if (state.rgbCard) state.rgbCard->loadSnapshotState(rgb.data() + 2, rgb.size() - 2);
    for (size_t i = 0; i < boardStates.size(); ++i) {
        const auto& board = boardStates[i];
        auto* card = state.mockingboards[i];
        card->loadSnapshotState(board.registers.data(), board.registers.size());
        card->loadSdkAudio(board.timeline.data(), board.timeline.size(), true);
    }
    state.memory.loadSdkKeyboard(keyboard.data(), keyboard.size(), true);
    state.memory.loadSdkPaddles(paddles, 6, true);
    state.memory.loadSdkVideoState(video.data(), video.size(), true);
    state.speaker.loadSdkState(speaker.data(), speaker.size(), true);
    state.display.invalidateTextFrameCache();
    state.mouseRawX = rawX;
    state.mouseRawY = rawY;
    state.mousePushOut = pushActive != 0;
    state.mousePushHoleX = pushX;
    state.mousePushHoleY = pushY;
    state.mousePushWaits = pushWaits;
    if (state.mouseMcu) {
        state.mouseMcu->loadSnapshotState(mouseRegisters.data(), mouseRegisters.size());
        state.mouseMcu->loadSdkInput(mouseInput.data(), mouseInput.size(), true);
    } else if (state.mouseHle) {
        state.mouseHle->loadSnapshotState(mouseRegisters.data(), mouseRegisters.size());
        state.mouseHle->loadSdkInput(mouseInput.data(), mouseInput.size(), true);
    } else if (state.mouseIic) {
        state.mouseIic->restoreSdkInput(rawX, rawY, mouseInput[0] != 0);
    }
    state.error.clear();
    return true;
}

}  // namespace pom2
