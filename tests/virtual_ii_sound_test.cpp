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

// Virtual ][ mechanical-sound bank.
//
// The commercial recordings stay out of the tree (roms/virtual_ii_sons/ is
// gitignored). This test synthesises a set with the same filenames and pins
// the playback model: motor loop + boot chirp, an arm take a phase burst
// does not restart, distinct lid close / open, one I/O-error grunt that a
// retry does not machine-gun, PopOff-then-PopOn on the second cold boot,
// and the matrix-printer loop in place of grains. A MAME-bank 5.25" latch
// stays silent; a 3.5" latch still clicks.

#include "FloppySoundDevice.h"
#include "PrinterSoundDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeWav(const fs::path& path, const std::vector<int16_t>& pcm)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    const uint32_t riff = 36 + dataBytes;
    const uint32_t rate = 44100;
    auto w32 = [&](uint32_t v) {
        f.write(reinterpret_cast<const char*>(&v), 4);
    };
    auto w16 = [&](uint16_t v) {
        f.write(reinterpret_cast<const char*>(&v), 2);
    };
    f.write("RIFF", 4);
    w32(riff);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(1);
    w16(1);
    w32(rate);
    w32(rate * 2);
    w16(2);
    w16(16);
    f.write("data", 4);
    w32(dataBytes);
    f.write(reinterpret_cast<const char*>(pcm.data()),
            static_cast<std::streamsize>(dataBytes));
}

std::vector<int16_t> tone(int n, int16_t amp, int quietHead = 0)
{
    std::vector<int16_t> p(static_cast<size_t>(n), amp);
    for (int i = 0; i < quietHead && i < n; ++i) p[static_cast<size_t>(i)] = 0;
    return p;
}

float peakOf(const float* p, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(p[i]));
    return m;
}

void render(FloppySoundDevice& dev, float* buf, int n)
{
    std::fill(buf, buf + n, 0.0f);
    dev.fillAudioBuffer(buf, n);
}

int fail(const char* msg)
{
    std::printf("FAIL: %s\n", msg);
    return 1;
}

void loadDev(FloppySoundDevice& dev, const fs::path& root)
{
    dev.setSampleRate(44100);
    dev.setVolume(1.0f);
    dev.loadVirtualII(root.string());
    dev.setBank(FloppySoundDevice::Bank::VirtualII);
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "pom2_virtual_ii_sound";
    std::error_code ec;
    fs::remove_all(root, ec);
    writeWav(root / "lecteur/Disk Rotation.wav", tone(400, 8000));
    writeWav(root / "lecteur/Boot.wav", tone(48, 24000));
    writeWav(root / "lecteur/Move arm.wav", tone(800, 20000, 200));
    writeWav(root / "lecteur/Disk Insertion.wav", tone(200, 10000));
    writeWav(root / "lecteur/Disk Removal.wav", tone(200, 24000));
    // Loud attack, quiet tail — a retrigger is obvious.
    {
        auto err = tone(200, 2000);
        for (int i = 0; i < 24; ++i) err[static_cast<size_t>(i)] = 26000;
        writeWav(root / "lecteur/I-O Error.wav", err);
    }
    writeWav(root / "interface/PopOff2.wav", tone(400, 6000));
    writeWav(root / "interface/PopOn2.wav", tone(400, 26000));
    writeWav(root / "imprimante/Matrix Printer.wav", tone(2000, 16000));

    FloppySoundDevice probe;
    loadDev(probe, root);
    if (!probe.virtualIIReady())
        return fail("synthesised Virtual ][ set did not load");

    float buf[1024];

    // Motor on mixes the boot chirp over the revolution loop.
    FloppySoundDevice dev;
    loadDev(dev, root);
    dev.motor(true, true);
    render(dev, buf, 32);
    if (peakOf(buf, 32) < 0.45f)
        return fail("motor-on did not play the boot chirp");
    if (!dev.audioMotorOn())
        return fail("motor-on did not latch the spindle");
    render(dev, buf, 64);               // finish the 48-frame chirp
    render(dev, buf, 256);
    const float spin = peakOf(buf, 256);
    if (spin < 0.10f || spin > 0.40f)
        return fail("revolution loop is missing or still the boot chirp");

    // A bank change while the spindle turns keeps it turning. The command
    // stream only carries motor EDGES, so a switch that dropped the motor
    // voice left the drive silent until the guest cycled it.
    {
        FloppySoundDevice sw;
        loadDev(sw, root);
        sw.motor(true, true);
        render(sw, buf, 256);
        sw.setBank(FloppySoundDevice::Bank::Mame);
        render(sw, buf, 64);
        sw.setBank(FloppySoundDevice::Bank::VirtualII);
        render(sw, buf, 256);
        if (peakOf(buf, 256) < 0.10f)
            return fail("bank round-trip silenced a spinning drive");
        if (!sw.audioMotorOn())
            return fail("bank round-trip dropped the spindle flag");
    }

    // Arm: the quiet head must survive a second step inside the join
    // window, and a later isolated step starts the take over.
    FloppySoundDevice arm;
    loadDev(arm, root);
    arm.step(0, 0);
    render(arm, buf, 80);
    if (peakOf(buf, 80) > 0.08f)
        return fail("arm take restarted or skipped its quiet head");
    arm.step(1, 10000);                 // ~10 ms — still one movement
    render(arm, buf, 250);
    if (peakOf(buf, 250) < 0.40f)
        return fail("arm take did not continue through a phase burst");
    arm.step(2, 10000 + 700000);        // ~0.7 s — the head had stopped
    render(arm, buf, 40);
    if (peakOf(buf, 40) > 0.08f)
        return fail("a new arm movement did not restart the take");

    // Lid close and lid open are different recordings.
    FloppySoundDevice door;
    loadDev(door, root);
    door.latch(true);
    render(door, buf, 40);
    const float inserted = peakOf(buf, 40);
    door.latch(false);
    render(door, buf, 40);
    const float ejected = peakOf(buf, 40);
    if (inserted < 0.20f || inserted > 0.40f)
        return fail("lid close is not the insertion recording");
    if (ejected < 0.55f)
        return fail("lid open is not the removal recording");

    // I/O error: one grunt, and a retry while it is still sounding does
    // not restart the attack.
    FloppySoundDevice errDev;
    loadDev(errDev, root);
    errDev.ioError();
    render(errDev, buf, 8);
    if (peakOf(buf, 8) < 0.50f)
        return fail("I/O error did not play");
    render(errDev, buf, 40);            // into the quiet tail
    errDev.ioError();
    render(errDev, buf, 8);
    if (peakOf(buf, 8) > 0.20f)
        return fail("I/O error retriggered mid-grunt");

    // First power switch is PopOn. The next cold boot is PopOff, then PopOn.
    FloppySoundDevice pwr;
    loadDev(pwr, root);
    pwr.powerSwitch();
    render(pwr, buf, 64);
    if (peakOf(buf, 64) < 0.50f)
        return fail("first power switch did not play PopOn");
    render(pwr, buf, 512);              // let PopOn finish
    pwr.powerSwitch();
    render(pwr, buf, 64);
    const float off = peakOf(buf, 64);
    if (off < 0.08f || off > 0.30f)
        return fail("second power switch did not lead with PopOff");
    render(pwr, buf, 512);              // finish PopOff; PopOn arms for the next buffer
    render(pwr, buf, 64);
    if (peakOf(buf, 64) < 0.50f)
        return fail("power cycle did not follow PopOff with PopOn");

    // Spindle hold-off, then silence. 800 ms at 44.1 kHz, plus the fade.
    dev.motor(false, true);
    for (int i = 0; i < 40; ++i) render(dev, buf, 1024);
    render(dev, buf, 256);
    if (peakOf(buf, 256) > 0.05f)
        return fail("motor-off hold did not release the revolution loop");
    if (dev.audioMotorOn())
        return fail("motor-off left the spindle flagged on");

    // MAME bank: a 5.25" latch does not click; a 3.5" latch still does.
    dev.setBank(FloppySoundDevice::Bank::Mame);
    const int queued = dev.queuedCommandCount();
    dev.latch(true);
    dev.ioError();
    if (dev.queuedCommandCount() != queued)
        return fail("MAME 5.25\" latch or ioError queued a command");
    render(dev, buf, 128);
    if (peakOf(buf, 128) > 0.02f)
        return fail("leaving the Virtual ][ bank kept a voice running");

    FloppySoundDevice mame;
    if (mame.loadSamples("roms/floppy_samples", FloppySoundDevice::FormFactor::FF525)) {
        const int before = mame.queuedCommandCount();
        mame.latch(true);
        if (mame.queuedCommandCount() != before)
            return fail("loaded MAME 5.25\" bank clicked on latch");
        if (!mame.loadSamples("roms/floppy_samples", FloppySoundDevice::FormFactor::FF35))
            return fail("3.5\" MAME samples did not load");
        mame.latch(false);
        if (mame.queuedCommandCount() != before + 1)
            return fail("MAME 3.5\" latch did not click");
    }

    // Printer: the recording replaces grains, and it stops when the head does.
    pom2::PrinterSoundDevice printer;
    printer.setSampleRate(44100);
    printer.setVolume(1.0f);
    if (!printer.loadSample((root / "imprimante/Matrix Printer.wav").string()))
        return fail("matrix recording did not load");
    printer.setSampled(true);
    printer.strike(9);
    if (printer.activeGrains() != 0)
        return fail("sampled printer still scheduled grains");
    float pbuf[256];
    std::fill(pbuf, pbuf + 256, 0.0f);
    printer.fillAudioBuffer(pbuf, 256);
    if (peakOf(pbuf, 256) < 0.15f)
        return fail("matrix recording stayed silent during a strike");
    for (int i = 0; i < 20; ++i) {
        std::fill(pbuf, pbuf + 256, 0.0f);
        printer.fillAudioBuffer(pbuf, 256);
    }
    std::fill(pbuf, pbuf + 256, 0.0f);
    printer.fillAudioBuffer(pbuf, 256);
    if (peakOf(pbuf, 256) > 0.05f)
        return fail("matrix recording kept running after the head stopped");

    // The loop resumes wherever its cursor stopped — mid-waveform. Starting
    // it at full gain is a step from silence: a click at every strike.
    printer.strike(9);
    std::fill(pbuf, pbuf + 256, 0.0f);
    printer.fillAudioBuffer(pbuf, 256);
    if (std::fabs(pbuf[0]) > 0.02f)
        return fail("matrix recording restarted without a fade-in");
    if (peakOf(pbuf, 256) < 0.15f)
        return fail("matrix recording did not come back on a strike");

    pom2::PrinterSoundDevice grains;
    grains.setSampleRate(44100);
    grains.setVolume(1.0f);
    grains.strike(9);
    if (grains.activeGrains() < 1)
        return fail("grain printer went silent when the recording exists elsewhere");

    // The real bundle, when this machine has it. CI does not.
    if (fs::is_regular_file("roms/virtual_ii_sons/lecteur/Disk Rotation.wav")) {
        FloppySoundDevice real;
        if (!real.loadVirtualII("roms/virtual_ii_sons"))
            return fail("roms/virtual_ii_sons/ did not load");
        pom2::PrinterSoundDevice realPrinter;
        if (!realPrinter.loadSample("roms/virtual_ii_sons/imprimante/Matrix Printer.wav"))
            return fail("real matrix recording did not load");
        std::printf("OK : real Virtual ][ set in roms/virtual_ii_sons/\n");
    }

    fs::remove_all(root, ec);
    std::printf("OK : Virtual ][ sound bank\n");
    return 0;
}
