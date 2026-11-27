// POM2 Apple II Emulator — GPL-3.0-or-later
//
// Host input recording (EmulationController_Input.cpp, InputRecording.h):
//   1. the .pom2rec codec round-trips every kind of entry, and refuses a
//      truncated or reordered file;
//   2. a real //e session — typed BASIC, a paddle, a Ctrl-Reset — recorded,
//      then played on a FRESH machine from the recording's own session,
//      reaches the end with every fingerprint matching;
//   3. a recording altered by one key is caught: the playback stops and
//      says it left the recording;
//   4. the host's keyboard is not heard during a playback, and the clocks a
//      program reads run on the recording's own time while one runs.

#include "EmulationController.h"
#include "HostClock.h"
#include "InputRecording.h"
#include "ResourcePaths.h"
#include "SnapshotIO.h"
#include "SystemProfile.h"
#include "AtomicFileReplace.h"
#include "TestTempPath.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using namespace pom2;

namespace {

std::unique_ptr<EmulationController> makeIIe(const std::string& rom)
{
    auto c = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    c->setMachineId(snapshotMachineId(SystemProfile::AppleIIe));
    c->setVideoStandard(VideoStandard::NTSC);
    c->setCyclesPerFrame(POM2_TIMING_NTSC.cyclesPerFrame);
    {
        auto st = c->lockState();
        st.memory().setIIEMode(true);
        const bool loaded = st.memory().loadAppleIIRom(rom.c_str());
        assert(loaded); (void)loaded;
        st.cpu().setCpuMode(M6502::CpuMode::CMOS);
        st.memory().resetSoftSwitches();
    }
    c->coldBoot();
    c->setMode(EmulationController::Mode::Running);
    return c;
}

void frames(EmulationController& c, int n) { for (int i = 0; i < n; ++i) c.tickFrame(); }

void type(EmulationController& c, const char* text)
{
    for (const char* p = text; *p; ++p) {
        c.memory().queueKey(static_cast<uint8_t>(*p == '\n' ? 0x0D : *p));
        frames(c, 3);
    }
}

bool screenHas(EmulationController& c, const char* s)
{
    auto st = c.lockState();
    std::string screen;
    for (uint16_t a = 0x400; a < 0x800; ++a) screen.push_back(static_cast<char>(st.memory().peekCpuView(a) & 0x7F));
    return screen.find(s) != std::string::npos;
}

void testCodec()
{
    InputRecordingFile f;
    f.session = {1, 2, 3, 4, 5};
    for (int k = static_cast<int>(InputEvent::Kind::Key); k <= static_cast<int>(InputEvent::Kind::Start); ++k) {
        TimedInput t;
        t.cycle = 1000u + static_cast<uint64_t>(k) * 7u;
        t.event.kind = static_cast<InputEvent::Kind>(k);
        t.event.a = -k; t.event.b = k * 3; t.event.c = 1;
        t.event.hz = 1022727.0 * k; t.event.stockHz = 1.5; t.event.machineHz = 2.25; t.event.ratio = 4.0;
        t.event.hash = 0x0123456789ABCDEFULL ^ static_cast<uint64_t>(k);
        t.event.when = -1234567 * k;
        t.event.text = std::string(static_cast<size_t>(k), 'x');
        f.entries.push_back(t);
    }
    std::vector<uint8_t> bytes;
    encodeInputRecording(f, bytes);
    InputRecordingFile back;
    std::string error;
    assert(decodeInputRecording(bytes, back, error));
    assert(back.session == f.session && back.entries.size() == f.entries.size());
    for (size_t i = 0; i < f.entries.size(); ++i) {
        const auto& a = f.entries[i]; const auto& b = back.entries[i];
        assert(a.cycle == b.cycle && a.event.kind == b.event.kind && a.event.a == b.event.a &&
               a.event.b == b.event.b && a.event.c == b.event.c && a.event.hz == b.event.hz &&
               a.event.ratio == b.event.ratio && a.event.hash == b.event.hash &&
               a.event.when == b.event.when && a.event.text == b.event.text);
    }
    for (size_t cut = 0; cut < bytes.size(); cut += 37) {
        std::vector<uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        assert(!decodeInputRecording(shorter, back, error) && "a truncated recording is refused");
    }
    std::swap(f.entries[0].cycle, f.entries[1].cycle);
    encodeInputRecording(f, bytes);
    assert(!decodeInputRecording(bytes, back, error) && "entries going back in time are refused");
    std::printf("[ OK ] the .pom2rec codec round-trips and refuses a damaged file\n");
}

void testRecordAndPlay(const std::string& rom)
{
    const std::string path = pom2test::tempPath("input-recording.pom2rec");
    auto a = makeIIe(rom);
    frames(*a, 120);                            // to the BASIC prompt
    std::string error;
    assert(startInputRecording(*a, error));
    assert(a->inputJournalMode() == EmulationController::InputJournalMode::Recording);
    assert(hostTimeOverride().fn.load() != nullptr && "the clocks read the recording's own time");
    type(*a, "PRINT 6*7\n");
    a->memory().setPaddle(0, 200);
    frames(*a, 40);
    assert(screenHas(*a, "42"));
    a->softReset();                             // Ctrl-Reset: kept, at its cycle
    frames(*a, 30);
    type(*a, "PRINT PDL(0)+1\n");
    frames(*a, 30);
    assert(screenHas(*a, "201"));             // the paddle, read after the Ctrl-Reset
    std::string why;
    assert(saveInputRecording(*a, path, why, error));
    assert(why == "stopped by the user");
    assert(a->inputJournalMode() == EmulationController::InputJournalMode::Off);
    assert(hostTimeOverride().fn.load() == nullptr);
    std::printf("[ OK ] a //e session with typing, a paddle and a Ctrl-Reset is recorded\n");

    // A fresh machine, nothing typed: the recording's session and inputs only.
    auto b = makeIIe(rom);
    frames(*b, 7);
    assert(playInputRecording(*b, path, error));
    assert(b->inputJournalMode() == EmulationController::InputJournalMode::Playing);
    for (int i = 0; i < 3000 && b->inputJournalMode() != EmulationController::InputJournalMode::Off; ++i) {
        b->memory().queueKey('X');              // the host is not heard
        b->tickFrame();
    }
    std::printf("  playback: %s\n", b->inputJournalStatus().c_str());
    assert(b->inputJournalMode() == EmulationController::InputJournalMode::Off);
    assert(!b->inputPlaybackDiverged());
    assert(b->inputJournalStatus().find("finished") != std::string::npos);
    assert(screenHas(*b, "201"));
    assert(!screenHas(*b, "X") || screenHas(*a, "X"));
    std::printf("[ OK ] played on a fresh machine, it stands on every recorded fingerprint\n");

    // One key changed: '6' becomes '7'. The playback must say it left.
    std::vector<uint8_t> bytes;
    assert(readSnapshotFileBytes(path, bytes, error));
    InputRecordingFile file;
    assert(decodeInputRecording(bytes, file, error));
    bool changed = false;
    for (auto& t : file.entries)
        if (!changed && t.event.kind == InputEvent::Kind::Key && t.event.a == '6') { t.event.a = '7'; changed = true; }
    assert(changed);
    encodeInputRecording(file, bytes);
    std::error_code ec;
    assert(writeFileAtomic(std::filesystem::path(path), bytes.data(), bytes.size(), ec));
    auto c = makeIIe(rom);
    assert(playInputRecording(*c, path, error));
    for (int i = 0; i < 3000 && c->inputJournalMode() != EmulationController::InputJournalMode::Off; ++i)
        c->tickFrame();
    std::printf("  altered playback: %s\n", c->inputJournalStatus().c_str());
    assert(c->inputPlaybackDiverged() && "an altered recording is caught, not played");
    std::filesystem::remove(path);
    std::printf("[ OK ] a recording altered by one key is caught: the playback stops and says where\n");
}

} // namespace

int main()
{
    testCodec();
    const std::string rom = findResource("roms/apple2e.rom");
    if (rom.empty()) {
        std::printf("SKIP input_recording: needs roms/apple2e.rom\n");
        return 77;
    }
    testRecordAndPlay(rom);
    std::printf("input_recording: all assertions passed\n");
    return 0;
}
