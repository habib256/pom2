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

// AudioCoordinator slot-topology and immutable-snapshot contract.

#include "AudioCoordinator.h"
#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "FloppySoundDevice.h"
#include "SpeakerDevice.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "EmulationController.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SlotBus.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool near(float lhs, float rhs)
{
    return std::fabs(lhs - rhs) < 0.0001f;
}

} // namespace

int main()
{
    EmulationController controller;
    pom2::AudioCoordinator audio(controller.audio(), controller);
    pom2::Settings settings;
    pom2::PrinterSoundDevice printer;

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();

        auto mb2 = std::make_unique<MockingboardCard>(
            2, MockingboardCard::Variant::AC);
        mb2->setVolume(0.25f);
        mb2->setMuted(true);
        bus.plug(2, std::move(mb2));

        auto echo = std::make_unique<EchoPlusCard>(3);
        echo->setVolume(0.35f);
        bus.plug(3, std::move(echo));

        auto phasor = std::make_unique<PhasorCard>(4);
        phasor->setVolume(0.45f);
        bus.plug(4, std::move(phasor));

        bus.plug(5, std::make_unique<EchoPlusTMS5220Card>(5));

        auto mb6 = std::make_unique<MockingboardCard>(
            6, MockingboardCard::Variant::SoundII);
        mb6->setVolume(0.65f);
        bus.plug(6, std::move(mb6));
    }

    const auto inventory = audio.captureInventory();
    assert(inventory.mockingboardSlots == std::vector<int>({2, 6}));
    assert(inventory.phasorSlots == std::vector<int>({4}));
    assert(inventory.echoPlusSlots == std::vector<int>({3}));
    assert(inventory.echoPlusTmsSlots == std::vector<int>({5}));
    assert(inventory.primaryMockingboardSlot() == 6);

    const auto mixer = audio.captureMixerCards();
    assert(mixer.size() == 4); // TMS scaffold has no AudioSource yet
    assert(mixer[0].slot == 2 && near(mixer[0].volume, 0.25f));
    assert(mixer[3].slot == 6 && near(mixer[3].volume, 0.65f));

    const auto mb = audio.captureMockingboard();
    assert(mb.plugged && mb.slot == 6 && mb.hasSsi);
    const auto ph = audio.capturePhasor();
    assert(ph.plugged && ph.slot == 4);
    const auto ep = audio.captureEchoPlus();
    assert(ep.plugged && ep.slot == 3);

    pom2::AudioCoordinator::MixerCardCommand command;
    command.kind = pom2::AudioCoordinator::CardKind::Mockingboard;
    command.slot = 2;
    command.volume = 0.75f;
    command.muted = false;
    assert(audio.applyMixerCard(command));
    const auto changed = audio.captureMixerCards();
    assert(near(changed[0].volume, 0.75f));
    assert(!changed[0].muted);

    audio.persist(settings,
                  controller.speaker(), controller.cassette(),
                  controller.floppySound525(), controller.floppySound35(),
                  printer);
    assert(near(settings.getFloat("mockingboard_slot2_volume"), 0.75f));
    assert(near(settings.getFloat("mockingboard_slot6_volume"), 0.65f));
    // Compatibility key follows the former last-plugged/highest-slot policy.
    assert(near(settings.getFloat("mockingboard_volume"), 0.65f));

    settings.setFloat("phasor_volume", 0.5f);
    settings.setFloat("phasor_slot4_volume", 0.9f);
    settings.setBool("phasor_slot4_muted", true);
    const auto restored = audio.restoreCardSettings(
        settings, pom2::AudioCoordinator::CardKind::Phasor, 4, 0.1f);
    assert(near(restored.volume, 0.9f));
    assert(restored.muted);

    // Reuse the old primary slot for a different type. Any cached alias would
    // now be stale; the coordinator instead reports the live bus topology.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(6);
        bus.plug(6, std::make_unique<EchoPlusCard>(6));
    }
    const auto rebuilt = audio.captureInventory();
    assert(rebuilt.mockingboardSlots == std::vector<int>({2}));
    assert(rebuilt.echoPlusSlots == std::vector<int>({3, 6}));
    assert(audio.captureMockingboard().slot == 2);
    assert(audio.captureEchoPlus().slot == 6);
    assert(audio.applyMixerCard(command)); // slot 2 is still a Mockingboard

    command.slot = 6; // but slot 6 no longer matches the command's kind
    assert(!audio.applyMixerCard(command));

    // ── restore() is THE audio restore path (bug hunt #4) ───────────────
    //
    // MainWindow's constructor used to hand-roll the same restore in two
    // blocks plus a third inside the printer setup, and the copies had
    // drifted: `printer_sound_pan` and the cassette mute were written by
    // persist() and read back by nobody. The constructor now calls this,
    // so the round trip has to be complete — every key persist() writes
    // must come back through restore().
    {
        pom2::Settings cfg;
        auto& speaker = controller.speaker();
        auto& tape = controller.cassette();
        auto& fs525 = controller.floppySound525();
        auto& fs35 = controller.floppySound35();

        speaker.setVolume(0.42f);
        speaker.setMuted(true);
        speaker.pan.store(-0.75f);
        tape.setVolume(0.31f);
        tape.setMuted(true);              // had NO settings key before
        tape.setAutoRewind(true);
        tape.pan.store(0.5f);
        fs525.setVolume(0.22f);
        fs525.setMuted(true);
        fs525.pan.store(-0.25f);
        fs35.setVolume(0.66f);
        fs35.setMuted(false);
        fs35.pan.store(0.9f);
        printer.setVolume(0.77f);
        printer.setMuted(true);
        printer.pan.store(-0.6f);         // written, never restored before
        controller.audio().setMasterVolume(0.8f);
        controller.audio().setMasterMuted(true);
        controller.audio().setMonoDownmix(true);

        audio.persist(cfg, speaker, tape, fs525, fs35, printer);

        // Scramble every one of them, then restore.
        speaker.setVolume(1.0f); speaker.setMuted(false); speaker.pan.store(0.0f);
        tape.setVolume(1.0f); tape.setMuted(false); tape.setAutoRewind(false);
        tape.pan.store(0.0f);
        fs525.setVolume(1.0f); fs525.setMuted(false); fs525.pan.store(0.0f);
        fs35.setVolume(1.0f); fs35.setMuted(true); fs35.pan.store(0.0f);
        printer.setVolume(0.0f); printer.setMuted(false); printer.pan.store(0.0f);
        controller.audio().setMasterVolume(1.0f);
        controller.audio().setMasterMuted(false);
        controller.audio().setMonoDownmix(false);

        audio.restore(cfg, speaker, tape, fs525, fs35, printer);

        assert(near(speaker.getVolume(), 0.42f) && speaker.isMuted());
        assert(near(speaker.pan.load(), -0.75f));
        assert(near(tape.getVolume(), 0.31f));
        assert(tape.isMuted() && "cassette_muted must round-trip");
        assert(tape.isAutoRewindEnabled());
        assert(near(tape.pan.load(), 0.5f));

        // Bug hunt #10: restore() folded the browser attenuation (kDiskGain,
        // 0.25 on WASM) into the value it handed the device and persist()
        // wrote getVolume() straight back, so restore->persist was a 4x
        // divider — the drive sounds faded out over a handful of browser
        // sessions. On a native build kDiskGain is 1, so this passes before
        // and after the fix here; it fails on the Emscripten build without
        // it, and guards any future platform gain.
        cfg.setFloat("floppy_sound_volume", 0.42f);
        for (int round = 0; round < 3; ++round) {
            audio.restore(cfg, speaker, tape, fs525, fs35, printer);
            audio.persist(cfg, speaker, tape, fs525, fs35, printer);
            assert(near(cfg.getFloat("floppy_sound_volume", 0.0f), 0.42f) &&
                   "a restore/persist round trip must not scale the volume");
            assert(near(fs525.getVolume(), 0.42f));
        }
        // Put the drive bank back where the assertions below expect it.
        cfg.setFloat("floppy_sound_volume", 0.22f);
        audio.restore(cfg, speaker, tape, fs525, fs35, printer);
        assert(near(fs525.getVolume(), 0.22f) && fs525.isMuted());
        assert(near(fs525.pan.load(), -0.25f));
        assert(near(fs35.getVolume(), 0.66f) && !fs35.isMuted());
        assert(near(fs35.pan.load(), 0.9f));
        assert(near(printer.volume(), 0.77f) && printer.muted());
        assert(near(printer.pan.load(), -0.6f) &&
               "printer_sound_pan must round-trip");
        assert(near(controller.audio().getMasterVolume(), 0.8f));
        assert(controller.audio().isMasterMuted());
        assert(controller.audio().isMonoDownmix());
    }

    // ── Legacy type-wide keys stay the fallback ─────────────────────────
    // A state.cfg written before the per-slot keys existed has only
    // `mockingboard_volume`; the plug lambdas in MainWindow_SlotConfig now
    // resolve through restoreCardSettings, so that old file must still win
    // over the hard-coded default.
    {
        pom2::Settings legacyOnly;
        legacyOnly.setFloat("mockingboard_volume", 0.11f);
        legacyOnly.setBool("mockingboard_muted", true);
        const auto mix = audio.restoreCardSettings(
            legacyOnly, pom2::AudioCoordinator::CardKind::Mockingboard, 5, 0.5f);
        assert(near(mix.volume, 0.11f) && mix.muted);
        // No key at all → the caller's default.
        pom2::Settings empty;
        const auto fresh = audio.restoreCardSettings(
            empty, pom2::AudioCoordinator::CardKind::EchoPlus, 3, 0.7f);
        assert(near(fresh.volume, 0.7f) && !fresh.muted);
    }

    // The legacy type-wide table in persist() is sized from the enum, not
    // hard-coded to 3 — the day the TMS scaffold grows an AudioSource it
    // must not fall off the end of the `index < legacy.size()` guard.
    static_assert(static_cast<std::size_t>(
                      pom2::AudioCoordinator::CardKind::EchoPlusTms5220) <
                      pom2::AudioCoordinator::kCardKindCount,
                  "legacy key table must cover every CardKind");

    // ── A stopped machine silences the host bus (bug hunt #4) ───────────
    // The audio device keeps calling its sources ~200x/s whatever `mode`
    // says, so a paused machine used to keep the AY generators and the
    // floppy motor loop droning on their last register set. Step counts as
    // NOT running on purpose: the worker leaves Step by storing Stopped
    // itself, so a single-step burst never flips the flag — no click per
    // instruction.
    {
        auto& dev = controller.audio();
        controller.setMode(EmulationController::Mode::Running);
        assert(!dev.isSuspended());
        controller.setMode(EmulationController::Mode::Stopped);
        assert(dev.isSuspended() && "a stopped machine must not drone");
        controller.setMode(EmulationController::Mode::Step);
        assert(dev.isSuspended() && "single-step stays silent");
        controller.setMode(EmulationController::Mode::Running);
        assert(!dev.isSuspended());
        controller.setMode(EmulationController::Mode::Stopped);
    }

    std::cout << "audio coordinator: OK\n";
    return 0;
}
