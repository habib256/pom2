// POM2 — GPL-3.0-or-later
// Mount/restore/reset are distinct events: only a user mount plays the lid.
#include "DiskIICard.h"
#include "EmulationController.h"
#include "MediaMount.h"
#include "TestTempPath.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
static void wav(const fs::path& path, int16_t sample = 0)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    auto le = [&](uint32_t value, int size) {
        for (int i = 0; i < size; ++i) out.put(static_cast<char>(value >> (8 * i)));
    };
    out.write("RIFF", 4); le(36 + 2048, 4); out.write("WAVEfmt ", 8);
    le(16, 4); le(1, 2); le(1, 2); le(44100, 4); le(88200, 4);
    le(2, 2); le(16, 2); out.write("data", 4); le(2048, 4);
    for (int i = 0; i < 1024; ++i) le(static_cast<uint16_t>(sample), 2);
}

int main()
{
    const fs::path dir = pom2test::tempPath("pom2_latch_events");
    fs::create_directories(dir);
    const auto disk = dir / "blank.dsk";
    { std::ofstream out(disk, std::ios::binary); std::string bytes(143360, '\0'); out.write(bytes.data(), bytes.size()); }
    for (const char* name : {"seek_2ms", "seek_6ms", "seek_12ms", "seek_20ms",
             "spin_empty", "spin_loaded", "spin_start_empty", "spin_start_loaded", "spin_end", "step_1_1"})
        wav(dir / (std::string("525_") + name + ".wav"));
    wav(dir / "525_door_close.wav", 10000);
    wav(dir / "525_door_open.wav", 6000);
    wav(dir / "lecteur/Disk Rotation.wav");
    wav(dir / "lecteur/Move arm.wav");
    wav(dir / "lecteur/Disk Insertion.wav", 10000);
    wav(dir / "lecteur/Disk Removal.wav", 6000);

    for (auto bank : {FloppySoundDevice::Bank::Mame, FloppySoundDevice::Bank::VirtualII}) {
        EmulationController ctrl(AudioDevice::OutputMode::Offline);
        auto& sound = ctrl.floppySound525();
        assert(sound.loadSamples(dir.string()));
        assert(sound.loadExtras(dir.string()) == 2);
        assert(sound.loadVirtualII(dir.string()));
        sound.setSampleRate(44100); sound.setVolume(1); sound.setBank(bank);
        auto card = std::make_unique<DiskIICard>(6);
        auto* drive = card.get();
        drive->setFloppySound(&sound);
        ctrl.memory().slotBus().plug(6, std::move(card));
        std::array<float, 256> pcm{};
        auto audible = [&] {
            pcm.fill(0); sound.fillAudioBuffer(pcm.data(), static_cast<int>(pcm.size()));
            return std::any_of(pcm.begin(), pcm.end(), [](float value) { return value != 0; });
        };
        // The low-level path used for saved media paths is silent.
        assert(drive->insertDisk(0, disk.string()));
        assert(!audible());
        ctrl.softReset(); assert(!audible());
        ctrl.hardReset(); assert(!audible());
        ctrl.coldBoot(); assert(!audible());
        // The public mount path emits the door at insertion, before boot.
        std::string error;
        assert(pom2::mountDiskII(ctrl, *drive, 0, disk.string(), error));
        assert(audible() && "successful user insertion must play the lid immediately");
    }
    fs::remove_all(dir);
    std::puts("floppy latch: insertion audible, path restore and all resets quiet, both banks");
}
