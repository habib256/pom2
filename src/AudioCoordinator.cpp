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

#include "AudioCoordinator.h"

#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "EmulationController.h"
#include "FloppySoundDevice.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SpeakerDevice.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace pom2 {
namespace {

template <typename Card>
Card* cardAt(SlotBus& bus, int slot)
{
    if (slot <= 0 || slot >= SlotBus::kSlotCount) return nullptr;
    return dynamic_cast<Card*>(bus.peripheral(slot));
}

template <typename Card>
Card* primaryCard(SlotBus& bus)
{
    // MainWindow used to overwrite its alias while slots were composed in
    // ascending order. Descending discovery preserves that visible choice.
    for (int slot = SlotBus::kSlotCount - 1; slot > 0; --slot) {
        if (auto* card = cardAt<Card>(bus, slot)) return card;
    }
    return nullptr;
}

const char* settingsStem(AudioCoordinator::CardKind kind)
{
    switch (kind) {
        case AudioCoordinator::CardKind::Mockingboard: return "mockingboard";
        case AudioCoordinator::CardKind::Phasor: return "phasor";
        case AudioCoordinator::CardKind::EchoPlus: return "echoplus";
        case AudioCoordinator::CardKind::EchoPlusTms5220: return "echoplus_tms";
    }
    return "audio_card";
}

std::string slotSettingsKey(AudioCoordinator::CardKind kind, int slot,
                            const char* suffix)
{
    return std::string(settingsStem(kind)) + "_slot" +
           std::to_string(slot) + "_" + suffix;
}

void copySsi(AudioCoordinator::Ssi263Snapshot& out,
             const MockingboardCard::Ssi263Snap& in)
{
    std::copy(std::begin(in.regs), std::end(in.regs), out.regs.begin());
    out.currentPhoneme = in.currentPhoneme;
    out.mode = in.mode;
    out.aRequest = in.aRequest;
    out.powerDown = in.powerDown;
    out.irqEnabled = in.irqEnabled;
    out.phonemeRemainingCycles = in.phonemeRemainingCycles;
    out.phonemeWriteCount = in.phonemeWriteCount;
}

void copySsi(AudioCoordinator::Ssi263Snapshot& out,
             const EchoPlusCard::ChipSnap& in)
{
    std::copy(std::begin(in.regs), std::end(in.regs), out.regs.begin());
    out.currentPhoneme = in.currentPhoneme;
    out.mode = in.mode;
    out.aRequest = in.aRequest;
    out.powerDown = in.powerDown;
    out.irqEnabled = in.irqEnabled;
    out.phonemeRemainingCycles = in.phonemeRemainingCycles;
    out.phonemeWriteCount = in.phonemeWriteCount;
}

} // namespace

int AudioCoordinator::InventorySnapshot::primaryMockingboardSlot() const noexcept
{
    return mockingboardSlots.empty() ? -1 : mockingboardSlots.back();
}

int AudioCoordinator::InventorySnapshot::primaryPhasorSlot() const noexcept
{
    return phasorSlots.empty() ? -1 : phasorSlots.back();
}

int AudioCoordinator::InventorySnapshot::primaryEchoPlusSlot() const noexcept
{
    return echoPlusSlots.empty() ? -1 : echoPlusSlots.back();
}

AudioCoordinator::~AudioCoordinator()
{
    unregisterAll();
}

void AudioCoordinator::registerSource(AudioSource* source)
{
    if (!source || !device_.isAvailable() || contains(source)) return;
    device_.addSource(source);
    sources_.push_back(source);
}

void AudioCoordinator::unregisterAll()
{
    if (device_.isAvailable()) {
        for (AudioSource* source : sources_) device_.removeSource(source);
    }
    sources_.clear();
}

bool AudioCoordinator::contains(const AudioSource* source) const
{
    return std::find(sources_.begin(), sources_.end(), source) != sources_.end();
}

AudioCoordinator::InventorySnapshot AudioCoordinator::captureInventory() const
{
    InventorySnapshot snapshot;
    if (!controller_) return snapshot;

    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (dynamic_cast<MockingboardCard*>(peripheral))
            snapshot.mockingboardSlots.push_back(slot);
        else if (dynamic_cast<PhasorCard*>(peripheral))
            snapshot.phasorSlots.push_back(slot);
        else if (dynamic_cast<EchoPlusCard*>(peripheral))
            snapshot.echoPlusSlots.push_back(slot);
        else if (dynamic_cast<EchoPlusTMS5220Card*>(peripheral))
            snapshot.echoPlusTmsSlots.push_back(slot);
    }
    return snapshot;
}

std::vector<AudioCoordinator::MixerCardSnapshot>
AudioCoordinator::captureMixerCards() const
{
    std::vector<MixerCardSnapshot> snapshots;
    if (!controller_) return snapshots;

    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        MixerCardSnapshot snapshot;
        AudioSource* source = nullptr;
        if (auto* mockingboard = cardAt<MockingboardCard>(bus, slot)) {
            snapshot.kind = CardKind::Mockingboard;
            snapshot.volume = mockingboard->getVolume();
            snapshot.muted = mockingboard->isMuted();
            source = mockingboard->audioSource();
        } else if (auto* phasor = cardAt<PhasorCard>(bus, slot)) {
            snapshot.kind = CardKind::Phasor;
            snapshot.volume = phasor->getVolume();
            snapshot.muted = phasor->isMuted();
            source = phasor->audioSource();
        } else if (auto* echo = cardAt<EchoPlusCard>(bus, slot)) {
            snapshot.kind = CardKind::EchoPlus;
            snapshot.volume = echo->getVolume();
            snapshot.muted = echo->isMuted();
            source = echo->audioSource();
        } else {
            continue;
        }
        snapshot.slot = slot;
        if (source) {
            snapshot.peak =
                source->lastBufferPeak.load(std::memory_order_relaxed);
            snapshot.clicks =
                source->clicksPerSecond.load(std::memory_order_relaxed);
        }
        snapshots.push_back(snapshot);
    }
    return snapshots;
}

bool AudioCoordinator::applyMixerCard(const MixerCardCommand& command)
{
    if (!controller_) return false;
    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();

    switch (command.kind) {
        case CardKind::Mockingboard:
            if (auto* card = cardAt<MockingboardCard>(bus, command.slot)) {
                card->setVolume(command.volume);
                card->setMuted(command.muted);
                return true;
            }
            break;
        case CardKind::Phasor:
            if (auto* card = cardAt<PhasorCard>(bus, command.slot)) {
                card->setVolume(command.volume);
                card->setMuted(command.muted);
                return true;
            }
            break;
        case CardKind::EchoPlus:
            if (auto* card = cardAt<EchoPlusCard>(bus, command.slot)) {
                card->setVolume(command.volume);
                card->setMuted(command.muted);
                return true;
            }
            break;
        case CardKind::EchoPlusTms5220:
            break; // scaffold card has no AudioSource yet
    }
    return false;
}

AudioCoordinator::MockingboardSnapshot
AudioCoordinator::captureMockingboard() const
{
    MockingboardSnapshot snapshot;
    if (!controller_) return snapshot;

    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();
    auto* card = primaryCard<MockingboardCard>(bus);
    if (!card) return snapshot;

    // ONE acquisition of the card mutex for the whole panel — see
    // MockingboardCard::captureDiagnostics. This used to be 51.
    const MockingboardCard::Diagnostics d = card->captureDiagnostics();
    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    snapshot.volume = card->getVolume();
    snapshot.muted = card->isMuted();
    snapshot.slotIrq = d.irqAsserted;
    for (int c = 0; c < 2; ++c) {
        auto& via = snapshot.via[static_cast<std::size_t>(c)];
        const auto& src = d.chip[c];
        via.t1cl = src.via[0x04];
        via.t1ch = src.via[0x05];
        via.t1ll = src.via[0x06];
        via.t1lh = src.via[0x07];
        via.sr = src.via[0x0A];
        via.acr = src.via[0x0B];
        via.pcr = src.via[0x0C];
        via.ifr = src.via[0x0D];
        via.ier = src.via[0x0E];
        for (int reg = 0; reg < 16; ++reg)
            via.ay[static_cast<std::size_t>(reg)] = src.ay[reg];
        via.viaWrites = src.viaWrites;
        via.ayWrites = src.ayWrites;
        via.ayResets = src.ayResets;
        via.cmdInactive = src.cmd[0];
        via.cmdRead = src.cmd[1];
        via.cmdWrite = src.cmd[2];
        via.cmdLatch = src.cmd[3];
    }
    snapshot.hasSsi = d.hasSsi;
    if (snapshot.hasSsi) copySsi(snapshot.ssi, d.ssi);
    return snapshot;
}

AudioCoordinator::PhasorSnapshot AudioCoordinator::capturePhasor() const
{
    PhasorSnapshot snapshot;
    if (!controller_) return snapshot;

    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();
    auto* card = primaryCard<PhasorCard>(bus);
    if (!card) return snapshot;

    // ONE acquisition for the whole panel — this used to be 82.
    const PhasorCard::Diagnostics d = card->captureDiagnostics();
    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    snapshot.volume = card->getVolume();
    snapshot.muted = card->isMuted();
    snapshot.slotIrq = d.irqAsserted;
    snapshot.mode = d.mode;
    snapshot.clockScale = d.clockScale;
    for (int c = 0; c < 2; ++c) {
        auto& via = snapshot.via[static_cast<std::size_t>(c)];
        via.t1cl = d.via[c][0x04];
        via.t1ch = d.via[c][0x05];
        via.t1ll = d.via[c][0x06];
        via.t1lh = d.via[c][0x07];
        via.sr = d.via[c][0x0A];
        via.acr = d.via[c][0x0B];
        via.pcr = d.via[c][0x0C];
        via.ifr = d.via[c][0x0D];
        via.ier = d.via[c][0x0E];
        via.writes = d.viaWrites[c];
    }
    for (int c = 0; c < 4; ++c) {
        auto& ay = snapshot.ay[static_cast<std::size_t>(c)];
        for (int reg = 0; reg < 16; ++reg)
            ay.regs[static_cast<std::size_t>(reg)] = d.ay[c][reg];
        ay.writes = d.ayWrites[c];
        ay.resets = d.ayResets[c];
    }
    return snapshot;
}

AudioCoordinator::EchoPlusSnapshot AudioCoordinator::captureEchoPlus() const
{
    EchoPlusSnapshot snapshot;
    if (!controller_) return snapshot;

    auto state = controller_->lockState();
    auto& bus = state.memory().slotBus();
    auto* card = primaryCard<EchoPlusCard>(bus);
    if (!card) return snapshot;

    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    snapshot.volume = card->getVolume();
    snapshot.muted = card->isMuted();
    snapshot.slotIrq = card->isIrqAsserted();
    copySsi(snapshot.chip, card->snapshotChip());
    return snapshot;
}

AudioCoordinator::CardMixSettings AudioCoordinator::restoreCardSettings(
    const Settings& settings, CardKind kind, int slot, float defaultVolume) const
{
    const std::string legacyVolume = std::string(settingsStem(kind)) + "_volume";
    const std::string legacyMuted = std::string(settingsStem(kind)) + "_muted";
    CardMixSettings result;
    result.volume = settings.getFloat(
        slotSettingsKey(kind, slot, "volume"),
        settings.getFloat(legacyVolume, defaultVolume));
    result.muted = settings.getBool(
        slotSettingsKey(kind, slot, "muted"),
        settings.getBool(legacyMuted, false));
    return result;
}

void AudioCoordinator::restore(Settings& settings,
                               SpeakerDevice& speaker,
                               CassetteDevice& cassette,
                               FloppySoundDevice& floppy525,
                               FloppySoundDevice& floppy35,
                               PrinterSoundDevice& printer)
{
#ifdef __EMSCRIPTEN__
    constexpr float kDiskGain = 0.25f;
#else
    constexpr float kDiskGain = 1.0f;
#endif
    // The browser attenuation belongs in the DEFAULT, not in the value that
    // goes back to disk: persist() writes getVolume() straight back to the
    // same key, so folding the gain in here made restore->persist a 4x
    // DIVIDER and the drive sounds faded out over a handful of browser
    // sessions (0.6 -> 0.15 -> 0.0375 -> ...). A first-run browser session
    // still gets the quieter level; a user's own choice now round-trips
    // (bug hunt #10).
    const float volume525 =
        settings.getFloat("floppy_sound_volume", 0.6f * kDiskGain);
    const bool muted525 = settings.getBool("floppy_sound_muted", false);
    const float volume35 = settings.getFloat("floppy_sound_volume_35", volume525);

    floppy525.setVolume(volume525);
    floppy525.setMuted(muted525);
    floppy35.setVolume(volume35);
    floppy35.setMuted(settings.getBool("floppy_sound_muted_35", muted525));

    device_.setMasterVolume(settings.getFloat("master_volume", 1.0f));
    device_.setMasterMuted(settings.getBool("master_muted", false));
    device_.setMonoDownmix(settings.getBool("audio_mono_downmix", false));

    speaker.setVolume(settings.getFloat("speaker_volume", 1.0f));
    speaker.setMuted(settings.getBool("speaker_muted", false));
    cassette.setVolume(settings.getFloat("cassette_volume", 0.6f));
    // The mixer panel has had a cassette mute toggle since the stereo bus
    // landed, but no key backed it: every other channel's mute survived a
    // restart and this one silently came back on. Default false = audible,
    // which is what an absent key meant before.
    cassette.setMuted(settings.getBool("cassette_muted", false));
    cassette.setAutoRewind(settings.getBool("cassette_auto_rewind", false));

    speaker.pan.store(settings.getFloat("speaker_pan", 0.0f));
    cassette.pan.store(settings.getFloat("cassette_pan", 0.0f));
    floppy525.pan.store(settings.getFloat("floppy_sound_pan", 0.0f));
    floppy35.pan.store(settings.getFloat("floppy_sound_pan_35", 0.0f));

    printer.setVolume(settings.getFloat("printer_sound_volume", 0.35f));
    printer.setMuted(settings.getBool("printer_sound_muted", false));
    printer.pan.store(settings.getFloat("printer_sound_pan", 0.0f));
}

void AudioCoordinator::persist(Settings& settings,
                               const SpeakerDevice& speaker,
                               const CassetteDevice& cassette,
                               const FloppySoundDevice& floppy525,
                               const FloppySoundDevice& floppy35,
                               const PrinterSoundDevice& printer) const
{
    settings.setFloat("speaker_volume", speaker.getVolume());
    settings.setBool("speaker_muted", speaker.isMuted());
    settings.setFloat("cassette_volume", cassette.getVolume());
    settings.setBool("cassette_muted", cassette.isMuted());
    settings.setBool("cassette_auto_rewind", cassette.isAutoRewindEnabled());
    settings.setFloat("floppy_sound_volume", floppy525.getVolume());
    settings.setBool("floppy_sound_muted", floppy525.isMuted());
    settings.setFloat("floppy_sound_volume_35", floppy35.getVolume());
    settings.setBool("floppy_sound_muted_35", floppy35.isMuted());
    settings.setFloat("master_volume", device_.getMasterVolume());
    settings.setBool("master_muted", device_.isMasterMuted());
    settings.setBool("audio_mono_downmix", device_.isMonoDownmix());
    settings.setFloat("speaker_pan", speaker.pan.load());
    settings.setFloat("cassette_pan", cassette.pan.load());
    settings.setFloat("floppy_sound_pan", floppy525.pan.load());
    settings.setFloat("floppy_sound_pan_35", floppy35.pan.load());
    settings.setFloat("printer_sound_volume", printer.volume());
    settings.setBool("printer_sound_muted", printer.muted());
    settings.setFloat("printer_sound_pan", printer.pan.load());

    // Capture under the machine lock, then release it before crossing into
    // Settings. Every live instance receives its own key. The highest slot of
    // each type also updates the legacy key, preserving the old last-plugged
    // behaviour for existing configurations.
    const auto cards = captureMixerCards();
    std::array<const MixerCardSnapshot*, kCardKindCount> legacy{};
    for (const auto& card : cards) {
        settings.setFloat(slotSettingsKey(card.kind, card.slot, "volume"),
                          card.volume);
        settings.setBool(slotSettingsKey(card.kind, card.slot, "muted"),
                         card.muted);
        const auto index = static_cast<std::size_t>(card.kind);
        if (index < legacy.size()) legacy[index] = &card;
    }
    for (const MixerCardSnapshot* card : legacy) {
        if (!card) continue;
        settings.setFloat(std::string(settingsStem(card->kind)) + "_volume",
                          card->volume);
        settings.setBool(std::string(settingsStem(card->kind)) + "_muted",
                         card->muted);
    }
}

} // namespace pom2
