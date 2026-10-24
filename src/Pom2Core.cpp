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

#include <pom2/core.hpp>

#include "Apple2Display.h"
#include "AudioMix.h"
#include "CassetteDevice.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "AudioSource.h"   // AudioSource + RateAware, named in setAudioSampleRate
#include "IIcMouse.h"
#include "LeChatMauveCard.h"
#include "MediaWritePolicy.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "SlotBus.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SpeakerDevice.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace pom2 {

class Core::Impl {
public:
    explicit Impl(const CoreConfig& requested)
        : cpu(&memory)
    {
        const CoreConfig config = resolve(requested);
        machine = config.machine;
        iieMemory = config.iieMemory;
        busWatchSink.owner = this;
        memory.setCpu(&cpu);
        memory.setIIEMode(config.iieMemory);
        videoStandard = config.timing == VideoTiming::PAL
            ? ::VideoStandard::PAL
            : ::VideoStandard::NTSC;
        memory.setVideoStandard(videoStandard);
        cpu.setCpuMode(config.cpu == CpuModel::CMOS65C02
            ? M6502::CpuMode::CMOS
            : M6502::CpuMode::NMOS);
        display.setAuxMemory(memory.auxData());
        cpuClockHz = static_cast<double>(pom2VideoTiming(
            config.timing == VideoTiming::PAL
                ? ::VideoStandard::PAL
                : ::VideoStandard::NTSC).cpuClockHz);
        speaker.setCpuClock(cpuClockHz);
        cassette.setCpuClock(cpuClockHz);
        cassette.setAudioAvailable(true);
        cassette.setAudioOutputSampleRate(audioSampleRate);
        memory.setSpeakerDevice(&speaker);
        memory.setCassetteDevice(&cassette);
        if (machine == Machine::AppleIIc) {
            // The //c's IOU mouse: no card, no ROM of its own — port 4 only
            // owns its lifecycle, IRQ and snapshot (SlotCardFactory "iicmouse").
            auto mouse = std::make_unique<IIcMouse>(4);
            mouseIic = mouse.get();
            mouseSlot = 4;
            memory.slotBus().plug(4, std::move(mouse));
        }
    }

    static CoreConfig resolve(CoreConfig c)
    {
        switch (c.machine) {
        case Machine::Custom:           break;
        case Machine::AppleIIPlus:      c.cpu = CpuModel::NMOS6502;  c.iieMemory = false; break;
        case Machine::AppleIIe:         c.cpu = CpuModel::NMOS6502;  c.iieMemory = true;  break;
        case Machine::AppleIIeEnhanced: c.cpu = CpuModel::CMOS65C02; c.iieMemory = true;  break;
        case Machine::AppleIIc:         c.cpu = CpuModel::CMOS65C02; c.iieMemory = true;  break;
        }
        return c;
    }

    ~Impl()
    {
        memory.setWatchSink(nullptr);
        memory.setCassetteDevice(nullptr);
        memory.setSpeakerDevice(nullptr);
    }

    Memory memory;
    M6502  cpu;
    Apple2Display display;
    SpeakerDevice speaker;
    CassetteDevice cassette;
    DiskIICard* diskII = nullptr; // non-owning; its slot owns the card
    SmartPortCard* smartPort = nullptr; // non-owning; its slot owns the card
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
    std::uint8_t       mouseRawX = 0, mouseRawY = 0;   // the host's running counters
    // setMouse on the MCU card: where the screen holes stood when the last
    // correction went out. Until they move, that correction is not published.
    bool               mousePushOut = false;
    int                mousePushHoleX = 0, mousePushHoleY = 0;
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

Core::Core(CoreConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

Core::~Core() = default;
Core::Core(Core&&) noexcept = default;
Core& Core::operator=(Core&&) noexcept = default;

bool Core::loadRom(std::string_view path, bool lowerBankFor32K)
{
    const std::string ownedPath(path);
    // A //c's 32 KiB dump boots from its lower bank (IIcClassProfile).
    const bool loaded = impl_->memory.loadAppleIIRom(
        ownedPath.c_str(),
        lowerBankFor32K || impl_->machine == Machine::AppleIIc) != 0;
    impl_->error = loaded ? std::string{} : impl_->memory.getLastError();
    return loaded;
}

bool Core::loadCharacterRom(std::string_view path, int bank)
{
    const std::string ownedPath(path);
    const bool loaded = impl_->memory.loadCharRom(ownedPath.c_str(), bank) != 0;
    impl_->error = loaded ? std::string{} : impl_->memory.getLastError();
    return loaded;
}

std::string Core::lastError() const
{
    return impl_->error;
}

namespace {

// The JSR-dispatch trio of Apple II Reference Manual Appx C, the check the
// emulator's own bootFromSlot makes. $Cn07 is not checked: a SmartPort reads
// $00 there and the //c-class block stub $01.
template <typename Read>
bool hasBootSignature(Read read)
{
    return read(0x01) == 0x20 && read(0x03) == 0x00 && read(0x05) == 0x03;
}

} // namespace

bool Core::attachDiskII(std::string_view bootRom,
                        std::string_view sequencerRom,
                        int slot)
{
    if (slot < 1 || slot > 7) {
        impl_->error = "Disk II slot must be between 1 and 7";
        return false;
    }
    // A //c has no slots: its Disk II is the IWM behind $C0E0-$C0EF, and its
    // firmware's disk page is $C600.
    if (impl_->machine == Machine::AppleIIc
        && slot != DiskIICard::kDefaultSlot) {
        impl_->error = "The //c's disk port is slot 6";
        return false;
    }
    SlotPeripheral* existing = impl_->memory.slotBus().peripheral(slot);
    if (existing && existing != impl_->diskII) {
        impl_->error = "Slot " + std::to_string(slot) + " is already occupied";
        return false;
    }
    auto card = std::make_unique<DiskIICard>(slot);
    card->setCpu(&impl_->cpu);

    if (!card->loadBootRom(std::string(bootRom))) {
        impl_->error = "Cannot load Disk II boot ROM: " + std::string(bootRom);
        return false;
    }
    if (!sequencerRom.empty()
        && !card->loadLssRom(std::string(sequencerRom))) {
        impl_->error = "Cannot load Disk II sequencer ROM: "
                     + std::string(sequencerRom);
        return false;
    }

    // One controller per core: moving it retires the old one, whose
    // destructor writes its drives back.
    if (impl_->diskII && impl_->diskII->getSlot() != slot)
        impl_->memory.slotBus().unplug(impl_->diskII->getSlot());
    impl_->diskII = card.get();
    impl_->memory.slotBus().plug(slot, std::move(card));
    impl_->error.clear();
    return true;
}

bool Core::insertDisk(int drive, std::string_view path)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    const bool inserted = impl_->diskII->insertDisk(drive, std::string(path));
    impl_->error = inserted ? std::string{}
                            : impl_->diskII->getLastError(drive);
    return inserted;
}

bool Core::ejectDisk(int drive)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    const bool ejected = impl_->diskII->ejectDisk(drive);
    impl_->error = ejected ? std::string{}
                           : impl_->diskII->getLastError(drive);
    return ejected;
}

bool Core::diskInserted(int drive) const
{
    return impl_->diskII && impl_->diskII->isDiskLoaded(drive);
}

bool Core::flushDisk(int drive)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    const bool flushed = impl_->diskII->flushDrive(drive);
    impl_->error = flushed ? std::string{}
                           : impl_->diskII->getLastError(drive);
    if (!flushed && impl_->error.empty())
        impl_->error = "the image could not be saved";
    return flushed;
}

bool Core::setDiskWriteProtected(int drive, bool protectedOn)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    if (!impl_->diskII->isDiskLoaded(drive)) {
        impl_->error = "No disk in that drive";
        return false;
    }
    // A protected image never saves (DiskImage::saveDirty), so writes the
    // guest already made go to the file first; a failed save refuses.
    if (protectedOn && !flushDisk(drive)) return false;
    // The write-back gate is what DiskImage::isWriteProtected() — the
    // guest's write-protect line — reads. Lifting it goes back to the
    // process policy, never above it.
    impl_->diskII->driveImage(drive).setWriteBackEnabled(
        !protectedOn && pom2::mediaWritableByDefault());
    impl_->error.clear();
    return true;
}

bool Core::diskWriteProtected(int drive) const
{
    return impl_->diskII && impl_->diskII->isDiskLoaded(drive)
        && impl_->diskII->driveImage(drive).isWriteProtected();
}

bool Core::bootDisk()
{
    if (!impl_->diskII || !impl_->diskII->hasBootRom()) {
        impl_->error = "Disk II boot requires a controller and its boot ROM";
        return false;
    }
    // The P5 PROM reads drive 1 and nothing else: with drive 0 empty it
    // waits for a sector that never comes, so the boot is refused here.
    if (!impl_->diskII->isDiskLoaded(0)) {
        impl_->error = "Disk II boots from drive 0, which is empty";
        return false;
    }
    coldBoot();
    impl_->diskII->seekTrack0();
    // The bus, not the card, after the reset — see bootSlot(): a //e maps
    // its 80-column firmware at $C300 out of reset.
    const int slot = impl_->diskII->getSlot();
    const auto cnxx = static_cast<std::uint16_t>(0xC000 | (slot << 8));
    if (!hasBootSignature([this, cnxx](std::uint8_t off) {
            return impl_->memory.memRead(static_cast<std::uint16_t>(cnxx + off));
        })) {
        impl_->error = "Slot " + std::to_string(slot)
                     + " boot PROM is not visible at $C" + std::to_string(slot)
                     + "00 on this machine; it was cold-booted";
        return false;
    }
    impl_->cpu.setProgramCounter(cnxx);
    impl_->error.clear();
    return true;
}

bool Core::attachSmartPort(int slot)
{
    if (slot < 1 || slot > 7) {
        impl_->error = "SmartPort slot must be between 1 and 7";
        return false;
    }
    if (impl_->memory.slotBus().isPlugged(slot)) {
        impl_->error = "Requested SmartPort slot is already occupied";
        return false;
    }
    if (impl_->smartPort) {
        impl_->error = "A SmartPort controller is already attached";
        return false;
    }
    // Synthetic ROM only: SlotCardFactory loads roms/liron.rom on a //e, but
    // the facade reads no file it was not handed, and the HLE service entries
    // are the same either way. Two units — ProDOS drive 1 / drive 2 of the
    // slot; with eight, ProDOS 2.4 would remap the empty six onto free slots.
    auto card = std::make_unique<SmartPortCard>(slot);
    card->setUnitCount(2);
    for (std::size_t unit = 0; unit < 2; ++unit)
        card->setUnit(unit, std::make_unique<SmartPort35Unit>());
    impl_->smartPort = card.get();
    impl_->memory.slotBus().plug(slot, std::move(card));
    impl_->error.clear();
    return true;
}

namespace {

SmartPortUnit* smartPortUnit(SmartPortCard* card, int unit, std::string& error)
{
    if (!card) {
        error = "SmartPort controller is not attached";
        return nullptr;
    }
    if (unit < 0 || unit > 1) {
        error = "SmartPort unit index must be 0 or 1";
        return nullptr;
    }
    return card->unit(static_cast<std::size_t>(unit));
}

} // namespace

bool Core::insertSmartPortDisk(int unit, std::string_view path)
{
    SmartPortUnit* target = smartPortUnit(impl_->smartPort, unit, impl_->error);
    if (!target) return false;
    // SmartPort35Unit::loadImage saves the outgoing disk's dirty blocks first
    // and leaves it mounted if that save fails.
    const bool inserted = target->loadImage(std::string(path));
    impl_->error = inserted ? std::string{} : target->lastError();
    return inserted;
}

bool Core::ejectSmartPortDisk(int unit)
{
    SmartPortUnit* target = smartPortUnit(impl_->smartPort, unit, impl_->error);
    if (!target) return false;
    // Save-on-eject: a failed save keeps the disk mounted and dirty.
    const bool ejected = target->eject();
    impl_->error = ejected ? std::string{} : target->lastError();
    return ejected;
}

bool Core::smartPortDiskInserted(int unit) const
{
    if (!impl_->smartPort || unit < 0 || unit > 1) return false;
    const SmartPortUnit* target =
        impl_->smartPort->unit(static_cast<std::size_t>(unit));
    return target && target->isLoaded();
}

bool Core::flushSmartPortDisk(int unit)
{
    SmartPortUnit* target = smartPortUnit(impl_->smartPort, unit, impl_->error);
    if (!target) return false;
    if (!target->hasUnsavedChanges()) {
        impl_->error.clear();
        return true;
    }
    const bool saved = target->saveDirty();
    impl_->error = saved ? std::string{} : target->lastError();
    if (!saved && impl_->error.empty())
        impl_->error = "the image could not be saved";
    return saved;
}

bool Core::attachRgbCard(int slot, RgbCardModel model)
{
    const bool iic = impl_->machine == Machine::AppleIIc;
    if (impl_->rgbCard) {
        impl_->error = "An RGB card is already attached";
        return false;
    }
    // The model is hardware, as in the application (MainWindow_SlotConfig's
    // plugChatMauve): the //c's DB-15 takes the adapter and nothing else,
    // and the Féline sits in a //e's aux slot.
    if (model == RgbCardModel::LeChatMauveFeline && (iic || !impl_->iieMemory)) {
        impl_->error = iic ? "A //c takes the Adaptateur IIc, not the Feline"
                           : "The Feline needs a //e (iieMemory)";
        return false;
    }
    if (model == RgbCardModel::LeChatMauveIIcAdapter && !iic) {
        impl_->error = "The Adaptateur IIc needs Machine::AppleIIc";
        return false;
    }
    if (iic) slot = LeChatMauveCard::kDefaultSlot;
    if (slot < 1 || slot > 7) {
        impl_->error = "RGB card slot must be between 1 and 7";
        return false;
    }
    if (impl_->memory.slotBus().isPlugged(slot)) {
        impl_->error = "Requested RGB card slot is already occupied";
        return false;
    }
    auto card = std::make_unique<LeChatMauveCard>(slot,
        model == RgbCardModel::LeChatMauveIIcAdapter
            ? LeChatMauveCard::Variant::IIcAdapter
            : LeChatMauveCard::Variant::Feline);
    card->setMemory(&impl_->memory);
    impl_->rgbCard = card.get();
    impl_->memory.slotBus().plug(slot, std::move(card));
    impl_->display.setChatMauveCard(impl_->rgbCard);
    impl_->error.clear();
    return true;
}

bool Core::rgbCardAttached() const
{
    return impl_->rgbCard != nullptr;
}

RgbDhgrMode Core::rgbDhgrMode() const
{
    if (!impl_->rgbCard) return RgbDhgrMode::Color140;
    switch (impl_->rgbCard->dhgrMode()) {
        case LeChatMauveCard::DhgrMode::BW560: return RgbDhgrMode::Mono560;
        case LeChatMauveCard::DhgrMode::Mixed: return RgbDhgrMode::Mixed;
        default:                               return RgbDhgrMode::Color140;
    }
}

bool Core::bootSlot(int slot)
{
    if (impl_->diskII && slot == impl_->diskII->getSlot()) return bootDisk();
    SlotPeripheral* card = (slot >= 1 && slot <= 7)
        ? impl_->memory.slotBus().peripheral(slot) : nullptr;
    if (!card) {
        impl_->error = "No card in the requested boot slot";
        return false;
    }
    // The boot signature (hasBootSignature) is asked of the card first, so
    // a slot with nothing bootable is refused with RAM untouched…
    if (!hasBootSignature([card](std::uint8_t off) { return card->slotRomRead(off); })) {
        impl_->error = "Slot " + std::to_string(slot)
                     + " ROM carries no boot signature";
        return false;
    }
    // …then of the BUS, after the reset, because the CPU runs what the bus
    // maps at $Cn00, not what the card holds. A //e comes out of reset with
    // SLOTC3ROM off ($C300 = the internal 80-column firmware); a //c forces
    // INTCXROM and shows a slot-5 card only while armed — the arm every
    // reset clears and EmulationController::bootFromSlot re-applies. Checking
    // the card alone returned true there and jumped into the machine's own
    // firmware.
    coldBoot();
    impl_->memory.setIicSmartPortArmed(slot == 5);
    const auto cnxx = static_cast<std::uint16_t>(0xC000 | (slot << 8));
    if (!hasBootSignature([this, cnxx](std::uint8_t off) {
            return impl_->memory.memRead(static_cast<std::uint16_t>(cnxx + off));
        })) {
        impl_->memory.setIicSmartPortArmed(false);
        impl_->error = "Slot " + std::to_string(slot)
                     + " ROM is not visible at $C" + std::to_string(slot)
                     + "00 on this machine; it was cold-booted";
        return false;
    }
    impl_->cpu.setProgramCounter(cnxx);
    impl_->error.clear();
    return true;
}

bool Core::loadCassette(std::string_view path)
{
    const bool loaded = impl_->cassette.loadTape(std::string(path));
    impl_->error = loaded ? std::string{} : impl_->cassette.getLastError();
    return loaded;
}

bool Core::saveCassette(std::string_view path)
{
    const bool saved = impl_->cassette.saveTape(std::string(path));
    impl_->error = saved ? std::string{} : impl_->cassette.getLastError();
    return saved;
}

void Core::playCassette()
{
    impl_->cassette.playTape();
}

void Core::stopCassette()
{
    impl_->cassette.stopTape();
}

void Core::pauseCassette(bool paused)
{
    impl_->cassette.setPlaybackPaused(paused);
}

void Core::rewindCassette()
{
    impl_->cassette.rewindTape();
}

void Core::ejectCassette()
{
    impl_->cassette.ejectTape();
}

void Core::armCassetteRecording()
{
    impl_->cassette.armRecording();
}

void Core::clearCassetteRecording()
{
    impl_->cassette.clearRecordedTape();
}

CassetteState Core::cassetteState() const
{
    return {
        impl_->cassette.hasLoadedTape(),
        impl_->cassette.isPlaybackActive()
            || impl_->cassette.isPlaybackArmed(),
        impl_->cassette.isPlaybackPaused(),
        impl_->cassette.isRewinding(),
        impl_->cassette.hasRecordedTape(),
        impl_->cassette.isAudioStreamMode(),
    };
}

void Core::setCassetteVolume(float volume)
{
    impl_->cassette.setVolume(volume);
}

bool Core::attachMockingboard(int slot, MockingboardModel model)
{
    // The //c has no slots: its Mockingboard is the 4c on the internal
    // expansion connector, which POM2 hosts on virtual slot 3 and Memory
    // maps at $C400-$C4FF once a write there wakes it (iicExpansionAwake).
    if (impl_->machine == Machine::AppleIIc) slot = 3;
    if (slot < 1 || slot > 7) {
        impl_->error = "Mockingboard slot must be between 1 and 7";
        return false;
    }
    if (impl_->memory.slotBus().isPlugged(slot)) {
        impl_->error = "Requested Mockingboard slot is already occupied";
        return false;
    }
    auto card = std::make_unique<MockingboardCard>(
        slot, model == MockingboardModel::SoundII
            ? MockingboardCard::Variant::SoundII
            : MockingboardCard::Variant::AC);
    card->setCpu(&impl_->cpu);
    card->setCpuClock(impl_->cpuClockHz);
    card->setStandardClock(impl_->cpuClockHz);   // no accelerator here
    card->setSampleRate(impl_->audioSampleRate);
    impl_->mockingboards.push_back(card.get());
    impl_->memory.slotBus().plug(slot, std::move(card));
    impl_->error.clear();
    return true;
}

bool Core::mockingboardAttached() const
{
    return !impl_->mockingboards.empty();
}

void Core::setMockingboardVolume(float volume)
{
    // Every attached card, not just the last one: they are all mixed.
    for (MockingboardCard* card : impl_->mockingboards)
        if (card) card->setVolume(volume);
}

bool Core::attachMouse(int slot, MouseModel model, std::string_view romDirectory)
{
    if (slot < 1 || slot > 7) {
        impl_->error = "Mouse slot must be between 1 and 7";
        return false;
    }
    if (impl_->memory.slotBus().isPlugged(slot)) {
        impl_->error = "Requested mouse slot is already occupied";
        return false;
    }
    if (impl_->mouseIic) {
        impl_->error = "The //c has its mouse built in; moveMouse()/setMouse() drive it";
        return false;
    }
    if (impl_->mouseMcu || impl_->mouseHle) {
        impl_->error = "A mouse card is already attached";
        return false;
    }
    const std::string dir(romDirectory);
    const std::string slotRom = dir + "/mouse_341-0270-c.bin";
    if (model == MouseModel::AppleMouseAppleWin) {
        auto card = std::make_unique<MouseCardAppleWin>(slot);
        if (!card->loadRom(slotRom)) {
            impl_->error = "Cannot load " + slotRom + " (2048 bytes expected)";
            return false;
        }
        // VBL on the beam: line 192 of a 262- or 312-line frame (SlotCardFactory).
        const auto& timing = pom2VideoTiming(impl_->videoStandard);
        card->setVblCycles(timing.scanlinesPerFrame * timing.cyclesPerScanline);
        card->setCpuClock(impl_->cpuClockHz);
        impl_->mouseHle = card.get();
        impl_->memory.slotBus().plug(slot, std::move(card));
    } else {
        const std::string mcuRom = dir + "/mouse_341-0269.bin";
        auto card = std::make_unique<MouseCard>(slot);
        if (!card->loadRoms(slotRom, mcuRom)) {
            impl_->error = "Cannot load " + slotRom + " and " + mcuRom +
                           " (2048 bytes each expected)";
            return false;
        }
        card->setCpuClock(impl_->cpuClockHz);
        impl_->mouseMcu = card.get();
        impl_->memory.slotBus().plug(slot, std::move(card));
    }
    impl_->mouseSlot = slot;
    impl_->mouseRawX = impl_->mouseRawY = 0;
    impl_->error.clear();
    return true;
}

bool Core::mouseAttached() const
{
    return impl_->mouseMcu || impl_->mouseHle || impl_->mouseIic;
}

void Core::moveMouse(int dx, int dy, bool button)
{
    if (!mouseAttached()) return;
    // In steps of at most ±127: the AppleWin model banks each call's signed
    // 8-bit delta, so a large move arrives whole; the MCU reads the counter
    // difference at its next poll (see the header).
    do {
        const int sx = std::clamp(dx, -127, 127), sy = std::clamp(dy, -127, 127);
        impl_->mouseRawX = static_cast<std::uint8_t>(impl_->mouseRawX + sx);
        impl_->mouseRawY = static_cast<std::uint8_t>(impl_->mouseRawY + sy);
        if (impl_->mouseHle)      impl_->mouseHle->setHostMouse(impl_->mouseRawX, impl_->mouseRawY, button);
        else if (impl_->mouseIic) impl_->mouseIic->setHostMouse(impl_->mouseRawX, impl_->mouseRawY, button);
        else                      impl_->mouseMcu->setHostMouse(impl_->mouseRawX, impl_->mouseRawY, button);
        dx -= sx; dy -= sy;
    } while (dx != 0 || dy != 0);
}

bool Core::setMouse(int x, int y, bool button)
{
    if (!mouseAttached()) return false;
    int curX = 0, curY = 0;
    if (impl_->mouseHle) {
        // The application's closed-loop rule (MouseSync.h): a correction is
        // computed only once the previous one has been consumed.
        const auto s = impl_->mouseHle->debugSnapshot();
        if (!s.hostDrained) { moveMouse(0, 0, button); return false; }
        curX = s.iX; curY = s.iY;
        x = std::clamp(x, s.iMinX, std::max(s.iMinX, s.iMaxX));
        y = std::clamp(y, s.iMinY, std::max(s.iMinY, s.iMaxY));
    } else {
        // The MCU keeps its position to itself; the firmware publishes it in
        // the slot's screen holes when the guest reads the mouse. The MCU
        // replays a push as quadrature edges over several frames, so a
        // correction computed before it has been replayed — and published —
        // stacks on the previous one and overshoots (the A2FILECMD jump,
        // MouseSync.h). Wait for both.
        // The //c's IOU: settled() = every edge emitted and serviced by the
        // ROM's IRQ handler, so the screen holes are final (MouseCoordinator).
        const bool pending = impl_->mouseIic ? !impl_->mouseIic->settled()
                                             : impl_->mouseMcu->hostMotionPending();
        if (pending) { moveMouse(0, 0, button); return false; }
        const int n = impl_->mouseSlot;
        Memory& m = impl_->memory;
        curX = m.peekCpuView(static_cast<std::uint16_t>(0x0478 + n)) |
               (m.peekCpuView(static_cast<std::uint16_t>(0x0578 + n)) << 8);
        curY = m.peekCpuView(static_cast<std::uint16_t>(0x04F8 + n)) |
               (m.peekCpuView(static_cast<std::uint16_t>(0x05F8 + n)) << 8);
        curX = static_cast<std::int16_t>(curX);
        curY = static_cast<std::int16_t>(curY);
        if (impl_->mousePushOut && curX == impl_->mousePushHoleX &&
            curY == impl_->mousePushHoleY) {
            moveMouse(0, 0, button);            // replayed, not yet read back
            return false;
        }
    }
    const int dx = std::clamp(x - curX, -127, 127);
    const int dy = std::clamp(y - curY, -127, 127);
    moveMouse(dx, dy, button);
    impl_->mousePushOut = dx != 0 || dy != 0;
    impl_->mousePushHoleX = curX;
    impl_->mousePushHoleY = curY;
    return dx == 0 && dy == 0;
}

void Core::softReset()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.resetSoftSwitchesWarm();
    impl_->memory.slotBus().reset();
    impl_->cpu.softReset();
}

void Core::hardReset()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.resetSoftSwitchesWarm();
    impl_->memory.slotBus().reset();
    impl_->speaker.reset();
    impl_->cassette.resetCpuSide();
    impl_->cpu.hardReset();
}

void Core::coldBoot()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.clearRam();
    impl_->memory.resetSoftSwitches();
    impl_->memory.slotBus().reset();
    impl_->speaker.reset();
    impl_->cassette.resetCpuSide();
    impl_->cpu.hardReset();
}

namespace {

// Raises Impl::cpuRunning for the CPU's own accesses (see BusWatchSink).
struct CpuRunningScope {
    explicit CpuRunningScope(bool& flag) : flag_(flag) { flag_ = true; }
    ~CpuRunningScope() { flag_ = false; }
    bool& flag_;
};

} // namespace

int Core::run(int minCycles)
{
    if (minCycles <= 0) return 0;
    CpuRunningScope running(impl_->cpuRunning);
    return impl_->cpu.run(minCycles);
}

void Core::step()
{
    CpuRunningScope running(impl_->cpuRunning);
    impl_->cpu.step();
}

DisplayState Core::displayState() const
{
    const Memory::DisplayState d = impl_->memory.getDisplayState();
    DisplayState out;
    out.text         = d.textMode;
    out.mixed        = d.mixedMode;
    out.page2        = d.page2;
    out.hiRes        = d.hiRes;
    out.eightyColumn = d.eightyCol;
    out.doubleHiRes  = d.dhgr;
    out.an3          = d.an3;
    out.altCharSet   = d.altChar;
    out.eightyStore  = d.eightyStore;
    return out;
}

std::uint64_t Core::speakerToggleCount() const
{
    return impl_->memory.getSpeakerToggleCount();
}

void Core::setBusWatch(BusWatch watch)
{
    impl_->busWatch = std::move(watch);
    // A null sink is the zero-cost state Memory checks for.
    impl_->memory.setWatchSink(impl_->busWatch ? &impl_->busWatchSink : nullptr);
}

bool Core::watchAddresses(std::uint16_t first, std::uint16_t last,
                          bool reads, bool writes)
{
    if (first > last) {
        impl_->error = "Watched range is empty: first > last";
        return false;
    }
    for (std::uint32_t a = first; a <= last; ++a) {
        const auto addr = static_cast<std::uint16_t>(a);
        if (reads)  impl_->memory.setReadWatch(addr, true);
        if (writes) impl_->memory.setWriteWatch(addr, true);
    }
    impl_->error.clear();
    return true;
}

void Core::clearWatchedAddresses()
{
    impl_->memory.clearReadWatches();
    impl_->memory.clearWriteWatches();
}

std::uint8_t Core::read(std::uint16_t address)
{
    return impl_->memory.memRead(address);
}

void Core::write(std::uint16_t address, std::uint8_t value)
{
    impl_->memory.memWrite(address, value);
}

void Core::setKeyHeld(std::uint16_t token, bool down)
{
    impl_->memory.setKeyHeld(token, down);
}

void Core::releaseHeldKeys()
{
    impl_->memory.releaseHeldKeys();
}

void Core::queueKey(std::uint8_t ascii)
{
    impl_->memory.queueKey(ascii);
}

std::size_t Core::pasteText(std::string_view text)
{
    return impl_->memory.pasteText(text.data(), text.size());
}

bool Core::setPaddle(int paddle, std::uint8_t value)
{
    if (paddle < 0 || paddle > 3) {
        impl_->error = "Paddle index must be between 0 and 3";
        return false;
    }
    impl_->memory.setPaddle(paddle, value);
    impl_->error.clear();
    return true;
}

bool Core::setPaddleButton(int button, bool pressed)
{
    if (button < 0 || button > 2) {
        impl_->error = "Paddle button index must be between 0 and 2";
        return false;
    }
    impl_->memory.setPaddleButton(button, pressed);
    impl_->error.clear();
    return true;
}

void Core::setJoystickAxes(float x, float y)
{
    const auto axisToPaddle = [](float axis) -> std::uint8_t {
        if (!std::isfinite(axis)) return 128;
        const float mapped = (std::clamp(axis, -1.0f, 1.0f) + 1.0f) * 127.5f;
        return static_cast<std::uint8_t>(mapped + 0.5f);
    };
    impl_->memory.setPaddle(0, axisToPaddle(x));
    impl_->memory.setPaddle(1, axisToPaddle(y));
}

FramebufferView Core::renderFrame()
{
    impl_->display.render(impl_->memory);
    return {
        impl_->display.pixels(),
        impl_->display.width(),
        impl_->display.height(),
    };
}

void Core::setAudioSampleRate(std::uint32_t sampleRate)
{
    if (sampleRate == 0) return;
    impl_->audioSampleRate = sampleRate;
    impl_->speaker.setSampleRate(sampleRate);
    impl_->cassette.setAudioOutputSampleRate(sampleRate);
    for (MockingboardCard* card : impl_->mockingboards)
        if (card) card->setSampleRate(sampleRate);
    // …and every OTHER card the mixer walks. `pullAudio` sums each plugged
    // card's audioSource(), so anything rendering at a rate it was not told
    // about drifts against the producer: its cycles-per-sample is computed
    // from the stale rate, the cursor runs onto the producer and the card
    // re-anchors every buffer (the documented 40-46 ms dropout), with every
    // note sharp by the ratio. Same idiom as `AudioDevice::addSource`.
    for (int slot = 1; slot <= 7; ++slot) {
        SlotPeripheral* card = impl_->memory.slotBus().peripheral(slot);
        if (!card) continue;
        if (AudioSource* source = card->audioSource())
            if (auto* ra = dynamic_cast<RateAware*>(source))
                ra->setSampleRate(sampleRate);
    }
}

std::size_t Core::pullSpeakerAudio(float* monoOutput,
                                   std::size_t frameCount,
                                   std::uint32_t sampleRate)
{
    if (!monoOutput || frameCount == 0 || sampleRate == 0) return 0;

    setAudioSampleRate(sampleRate);
    std::size_t produced = 0;
    constexpr std::size_t kMaxChunk =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    while (produced < frameCount) {
        const std::size_t remaining = frameCount - produced;
        const int chunk = static_cast<int>(std::min(remaining, kMaxChunk));
        impl_->speaker.fillAudioBuffer(monoOutput + produced, chunk);
        produced += static_cast<std::size_t>(chunk);
    }
    return produced;
}

void Core::setSpeakerVolume(float volume)
{
    impl_->speaker.setVolume(volume);
}

std::size_t Core::pullAudio(float* interleavedStereo,
                            std::size_t frameCount,
                            std::uint32_t sampleRate)
{
    if (!interleavedStereo || frameCount == 0 || sampleRate == 0) return 0;

    setAudioSampleRate(sampleRate);

    // THE mixer — the same law `AudioDevice` plays through (`AudioMix.h`),
    // over this core's own bus state. This used to be a SECOND, divergent
    // mixer: speaker + cassette + one Mockingboard, summed and clamped, with
    // no pan, no master gain, no mute, no mono downmix, no suspend, no
    // meters and no second sound card. Every later fix to the real mixer
    // missed it, which is what makes divergence worth REMOVING rather than
    // patching.
    impl_->mixList.clear();
    impl_->mixList.push_back(&impl_->speaker);
    impl_->mixList.push_back(&impl_->cassette);
    for (int slot = 1; slot <= 7; ++slot) {
        if (SlotPeripheral* card = impl_->memory.slotBus().peripheral(slot)) {
            if (AudioSource* source = card->audioSource())
                impl_->mixList.push_back(source);
        }
    }

    // The core exposes no master controls, so: unity gain, unmuted, stereo —
    // the signal the GUI plays at its own defaults.
    pom2::MixParams params;
    params.sampleRate = sampleRate;

    constexpr std::size_t kMixChunk = 4096;
    std::size_t produced = 0;
    while (produced < frameCount) {
        const std::size_t count = std::min(frameCount - produced, kMixChunk);
        pom2::mixSourcesInto(interleavedStereo + 2 * produced,
                             static_cast<int>(count), impl_->mixList,
                             params, impl_->mixBus);
        produced += count;
    }
    return produced;
}

void Core::setCpuState(const CpuState& state)
{
    M6502& cpu = impl_->cpu;
    cpu.setAccumulator(state.accumulator);
    cpu.setXRegister(state.x);
    cpu.setYRegister(state.y);
    cpu.setStatusRegister(static_cast<std::uint8_t>(state.status | 0x20));
    cpu.setStackPointer(state.stackPointer);
    cpu.setProgramCounter(state.programCounter);
}

CpuState Core::cpuState() const
{
    return {
        impl_->cpu.getAccumulator(),
        impl_->cpu.getXRegister(),
        impl_->cpu.getYRegister(),
        impl_->cpu.getStatusRegister(),
        impl_->cpu.getStackPointer(),
        impl_->cpu.getProgramCounter(),
        impl_->memory.getCycleCounter(),
        impl_->cpu.isHalted(),
    };
}

} // namespace pom2
