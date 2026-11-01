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
#include "SamCard.h"
#include "EmulationController.h"
#include "FloppySoundDevice.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SpeakerDevice.h"

#include "AudioBufferPolicy.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
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
        case AudioCoordinator::CardKind::Sam: return "sam";
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
        } else if (auto* speechCard = cardAt<EchoPlusTMS5220Card>(bus, slot)) {
            snapshot.kind = CardKind::EchoPlusTms5220;
            snapshot.volume = speechCard->getVolume(); snapshot.muted = speechCard->isMuted();
            source = speechCard->audioSource();
        } else if (auto* sam = cardAt<SamCard>(bus, slot)) {
            snapshot.kind = CardKind::Sam;
            snapshot.volume = sam->getVolume();
            snapshot.muted = sam->isMuted();
            source = sam->audioSource();
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
            if (auto* card = cardAt<EchoPlusTMS5220Card>(bus, command.slot)) {
                card->setVolume(command.volume); card->setMuted(command.muted); return true;
            }
            break;
        case CardKind::Sam:
            if (auto* card = cardAt<SamCard>(bus, command.slot)) {
                card->setVolume(command.volume);
                card->setMuted(command.muted);
                return true;
            }
            break;
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

    // The device buffer (AudioBufferPolicy.h): a pinned rung wins; otherwise
    // the rung the ladder reached last time, so a Raspberry Pi that climbed
    // to 2048 frames yesterday does not crackle through the climb again
    // today. A learned value below the platform default is stale (the
    // default moved) and is ignored.
    pinnedFrames_  = static_cast<std::uint32_t>(
        std::max(0, settings.getInt("audio_buffer_frames", 0)));
    learnedFrames_ = static_cast<std::uint32_t>(
        std::max(0, settings.getInt("audio_buffer_learned", 0)));
    {
        using P = pom2::AudioBufferPolicy;
        const auto platform = device_.platform();
        if (!P::isRung(pinnedFrames_, platform)) pinnedFrames_ = 0;
        if (!P::isRung(learnedFrames_, platform)) learnedFrames_ = 0;
        if (pinnedFrames_)
            applyLadderRung(pinnedFrames_, "pinned in the settings");
        else if (learnedFrames_ > device_.periodFrames())
            applyLadderRung(learnedFrames_, "learned on an earlier run");
    }

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

    // Virtual ][ recordings, when the local set decoded. Absent key, or a
    // folder that is not on this machine, stays on the MAME samples.
    const bool wantVii = settings.getString("mechanical_sound_bank", "auto") == "virtual2";
    const bool vii = wantVii && floppy525.virtualIIReady();
    floppy525.setBank(vii ? FloppySoundDevice::Bank::VirtualII
                          : FloppySoundDevice::Bank::Mame);
    floppy35.setBank(vii && floppy35.virtualIIReady()
                         ? FloppySoundDevice::Bank::VirtualII
                         : FloppySoundDevice::Bank::Mame);
    printer.setSampled(vii && printer.sampleReady());
    settings.setBool("mechanical_sound_import_pending", false);
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
    settings.setInt("audio_buffer_frames", static_cast<int>(pinnedFrames_));
    settings.setInt("audio_buffer_learned", static_cast<int>(learnedFrames_));
    settings.setFloat("speaker_pan", speaker.pan.load());
    settings.setFloat("cassette_pan", cassette.pan.load());
    settings.setFloat("floppy_sound_pan", floppy525.pan.load());
    settings.setFloat("floppy_sound_pan_35", floppy35.pan.load());
    settings.setFloat("printer_sound_volume", printer.volume());
    settings.setBool("printer_sound_muted", printer.muted());
    settings.setFloat("printer_sound_pan", printer.pan.load());
    // Without the recordings on this machine the mixer cannot offer
    // Virtual ][, so running on MAME is not a choice the user made: keep
    // whatever the file says. Writing "mame" here made a session without
    // the folder (a USB copy, a fresh checkout) forget the setting for
    // every later one that has it.
    const bool viiNow = floppy525.bank() == FloppySoundDevice::Bank::VirtualII;
    if (!settings.getBool("mechanical_sound_import_pending", false) &&
        settings.getString("mechanical_sound_bank", "auto") != "auto" &&
        (viiNow || floppy525.virtualIIReady()))
        settings.setString("mechanical_sound_bank", viiNow ? "virtual2" : "mame");

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

AudioCoordinator::BufferSnapshot AudioCoordinator::captureBuffer() const
{
    using P = pom2::AudioBufferPolicy;
    BufferSnapshot s;
    s.periodFrames  = device_.periodFrames();
    s.periods       = P::kPeriods;
    s.sampleRate    = device_.getActualSampleRate();
    s.bufferMs      = device_.bufferSeconds() * 1000.0;
    s.lateCallbacks = device_.lateCallbacks();
    s.pinnedFrames  = pinnedFrames_;
    s.minFrames     = P::kMinPeriodFrames;
    s.maxFrames     = P::maxPeriodFrames(device_.platform());
    s.available     = device_.isAvailable();
    return s;
}

void AudioCoordinator::applyLadderRung(std::uint32_t frames, const std::string& why)
{
    const std::uint32_t before = device_.periodFrames();
    const double beforeMs = device_.bufferSeconds() * 1000.0;
    if (!device_.reconfigurePeriod(frames)) {
        log().warn("Audio", "device buffer: could not re-open at " +
                            std::to_string(frames) + " frames (" + why + ")");
        return;
    }
    if (device_.periodFrames() != before) {
        log().info("Audio", "device buffer: " + std::to_string(before) +
                            " -> " + std::to_string(frames) + " frames (" +
                            std::to_string(static_cast<int>(beforeMs + 0.5)) +
                            " -> " +
                            std::to_string(static_cast<int>(
                                device_.bufferSeconds() * 1000.0 + 0.5)) +
                            " ms), " + why);
    }
    // A new window: the old device's late callbacks are not the new one's.
    lateAtWindowStart_ = device_.lateCallbacks();
    windowStartSec_    = -1.0;
}

void AudioCoordinator::pinBuffer(std::uint32_t frames)
{
    using P = pom2::AudioBufferPolicy;
    if (frames != 0 && !P::isRung(frames, device_.platform())) return;
    pinnedFrames_ = frames;
    if (frames != 0) {
        applyLadderRung(frames, "pinned in the Audio panel");
    } else {
        // Back to automatic: keep the current rung as the ladder's start.
        learnedFrames_     = std::max(learnedFrames_, device_.periodFrames());
        lateAtWindowStart_ = device_.lateCallbacks();
        windowStartSec_    = -1.0;
    }
}

void AudioCoordinator::tick()
{
    using P = pom2::AudioBufferPolicy;
    if (pinnedFrames_ != 0 || !device_.isAvailable()) return;
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const std::uint64_t late = device_.lateCallbacks();
    if (windowStartSec_ < 0.0 || now - windowStartSec_ > P::kWindowSec) {
        windowStartSec_    = now;
        lateAtWindowStart_ = late;
        return;
    }
    const std::uint64_t lateInWindow = late - lateAtWindowStart_;
    const std::uint32_t current = device_.periodFrames();
    const std::uint32_t next =
        P::escalatedPeriodFrames(current, lateInWindow, device_.platform());
    if (next == current) return;
    learnedFrames_ = std::max(learnedFrames_, next);
    applyLadderRung(next, std::to_string(lateInWindow) +
                              " late callbacks in the last " +
                              std::to_string(static_cast<int>(now - windowStartSec_ + 0.5)) +
                              " s — the host could not keep the smaller buffer fed");
}

} // namespace pom2
