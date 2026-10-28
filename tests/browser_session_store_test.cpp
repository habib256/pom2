// POM2 — GPL-3.0-or-later
#include "BrowserSessionStore.h"
#include "EmulationController.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace {
struct Unsupported final : SlotPeripheral {
    std::string_view name() const override { return "unsupported session card"; }
};
std::string read(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
}
int main()
{
    const auto root = pom2test::tempPath("pom2-browser-session-store");
    std::filesystem::create_directories(root);
    auto ctrl = std::make_unique<EmulationController>();
    ctrl->setMachineId(pom2::snapshotMachineId(pom2::SystemProfile::AppleIIPlus));
    ctrl->setMode(EmulationController::Mode::Stopped);
    auto change = [&](uint16_t pc, uint8_t value) {
        auto state = ctrl->lockState();
        state.cpu().setProgramCounter(pc);
        state.memory().writeRamUnchecked(0x300, value);
    };
    auto verify = [&](uint16_t pc, uint8_t value) {
        auto state = ctrl->lockState();
        assert(state.cpu().getProgramCounter() == pc);
        assert(state.memory().peekCpuView(0x300) == value);
    };
    pom2::BrowserSessionStore store(root);
    store.initialize(*ctrl);
    assert(!store.enabled());
    change(0x2345, 0xAB);
    assert(store.setEnabled(*ctrl, true));
    const auto path = root + "/browser-resume.pom2session";
    const auto first = read(path);
    assert(!first.empty());
    change(0x4567, 0xCD);
    pom2::BrowserSessionStore restored(root);
    restored.initialize(*ctrl);
    assert(restored.enabled() && !restored.suspended());
    verify(0x2345, 0xAB);
    assert(ctrl->getMode() == EmulationController::Mode::Stopped);
    // A rejected configuration must preserve the previous durable checkpoint.
    { auto state = ctrl->lockState(); state.memory().slotBus().plug(3, std::make_unique<Unsupported>()); }
    restored.checkpoint(*ctrl, false);
    assert(restored.suspended() && !restored.error().empty());
    assert(read(path) == first);
    { auto state = ctrl->lockState(); state.memory().slotBus().unplug(3); }
    assert(restored.setEnabled(*ctrl, true));
    change(0x3456, 0xEF);
    restored.checkpoint(*ctrl, true);
    change(0x4567, 0xCD);
    pom2::BrowserSessionStore running(root);
    running.initialize(*ctrl);
    verify(0x3456, 0xEF);
    assert(ctrl->getMode() == EmulationController::Mode::Running);
    ctrl->setMode(EmulationController::Mode::Stopped);
    const auto committed = read(path);
    const auto marker = root + "/browser-resume.enabled";
    const auto published = read(marker);
    const auto inactive = root + "/browser-resume-next.pom2session";
    std::filesystem::remove(inactive);
    std::filesystem::create_directory(inactive);
    { std::ofstream blocker(inactive + "/blocker"); blocker << "do not replace this directory"; }
    running.checkpoint(*ctrl, false);
    assert(running.suspended());
    assert(read(path) == committed && read(marker) == published);
    std::filesystem::remove_all(inactive);
    // Corruption refuses to mutate the live machine and suspends heartbeat
    // writes so the broken checkpoint remains available for diagnosis.
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << "broken"; }
    change(0x5678, 0x12);
    pom2::BrowserSessionStore broken(root);
    broken.initialize(*ctrl);
    assert(broken.suspended() && !broken.error().empty());
    verify(0x5678, 0x12);
    broken.checkpoint(*ctrl, false);
    assert(read(path) == "broken");
    assert(broken.setEnabled(*ctrl, false));
    assert(!std::filesystem::exists(root + "/browser-resume.enabled"));
    pom2::BrowserSessionStore disabled(root);
    disabled.initialize(*ctrl);
    assert(!disabled.enabled());
    verify(0x5678, 0x12);
    { std::ofstream file(marker, std::ios::binary); file << std::string(1024, 'x'); }
    pom2::BrowserSessionStore malformed(root);
    malformed.initialize(*ctrl);
    assert(malformed.suspended());
    verify(0x5678, 0x12);
    std::filesystem::remove_all(root);

    // A damaged marked generation falls back to the other one — the previous
    // good checkpoint — and the next checkpoint overwrites the damaged file,
    // never the one that restored.
    const auto root2 = pom2test::tempPath("pom2-browser-session-fallback");
    std::filesystem::create_directories(root2);
    const auto gen0 = root2 + "/browser-resume.pom2session";
    const auto gen1 = root2 + "/browser-resume-next.pom2session";
    {
        pom2::BrowserSessionStore s(root2);
        s.initialize(*ctrl);
        change(0x1111, 0x11);
        assert(s.setEnabled(*ctrl, true));      // gen0 = 1111/11
        change(0x2222, 0x22);
        s.checkpoint(*ctrl, false);             // gen1 = 2222/22, marked
        assert(!s.suspended());
    }
    const auto good = read(gen0);
    { std::ofstream file(gen1, std::ios::binary | std::ios::trunc); file << "broken"; }
    change(0x3333, 0x33);
    pom2::BrowserSessionStore fallback(root2);
    fallback.initialize(*ctrl);
    assert(!fallback.suspended() && "the older generation restores");
    verify(0x1111, 0x11);
    change(0x4444, 0x44);
    fallback.checkpoint(*ctrl, false);
    assert(!fallback.suspended());
    assert(read(gen0) == good && "the generation that restored is kept");
    assert(read(gen1) != "broken" && "the damaged generation is rewritten");
    std::filesystem::remove_all(root2);
}
