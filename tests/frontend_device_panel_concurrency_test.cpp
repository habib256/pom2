// Headless ImGui frames across real slot changes. The UI must render a copied
// device snapshot after the card that supplied it has been unplugged, while
// coordinator captures and slot changes contend for the same machine lock.

#include "DevicePanelCoordinator.h"
#include "EmulationController.h"
#include "Memory.h"
#include "Settings.h"
#include "SlotBus.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "Uthernet_ImGui.h"

#include "imgui.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1440, 900);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    assert(pixels && width > 0 && height > 0);

    EmulationController controller;
    pom2::Settings settings;
    pom2::DevicePanelCoordinator coordinator(controller, settings);
    pom2::Uthernet_ImGui panel;

    // The worker changes slot 3 once for each frame. The UI captures before
    // asking for a change and renders that snapshot after the old card is
    // destroyed; every frame therefore exercises stale-snapshot rendering.
    std::atomic<int> requested{0};
    std::atomic<int> completed{0};
    std::atomic<bool> raceStart{false};
    std::atomic<bool> raceStop{false};
    std::atomic<int> raceSwaps{0};
    std::thread replugger([&] {
        for (int generation = 1; generation <= 80; ++generation) {
            while (requested.load(std::memory_order_acquire) < generation)
                std::this_thread::yield();
            {
                auto state = controller.lockState();
                auto& bus = state.memory().slotBus();
                bus.unplug(3);
                if (generation & 1)
                    bus.plug(3, std::make_unique<pom2::UthernetCard>(3));
                else
                    bus.plug(3, std::make_unique<pom2::UthernetIICard>(3));
            }
            completed.store(generation, std::memory_order_release);
        }
        while (!raceStart.load(std::memory_order_acquire))
            std::this_thread::yield();
        while (!raceStop.load(std::memory_order_acquire)) {
            const int next = raceSwaps.load(std::memory_order_relaxed) + 1;
            {
                auto state = controller.lockState();
                auto& bus = state.memory().slotBus();
                bus.unplug(3);
                if (next & 1)
                    bus.plug(3, std::make_unique<pom2::UthernetCard>(3));
                else
                    bus.plug(3, std::make_unique<pom2::UthernetIICard>(3));
            }
            raceSwaps.store(next, std::memory_order_release);
            std::this_thread::yield();
        }
    });

    bool open = true;
    int u1Frames = 0;
    int u2Frames = 0;
    for (int generation = 1; generation <= 80; ++generation) {
        const auto inventory = coordinator.captureInventory();
        const auto snapshot = coordinator.captureEthernet();
        assert(inventory.ethernetPlugged() ==
               (snapshot.u1Plugged || snapshot.u2Plugged));
        u1Frames += snapshot.u1Plugged;
        u2Frames += snapshot.u2Plugged;

        requested.store(generation, std::memory_order_release);
        while (completed.load(std::memory_order_acquire) < generation)
            std::this_thread::yield();

        ImGui::NewFrame();
        const auto action = panel.render("Ethernet", open, snapshot);
        ImGui::Render();
        coordinator.applyEthernet(action);
        assert(open);
    }
    // Now capture and render while the worker keeps changing the topology.
    // This exercises actual lock contention in addition to the deterministic
    // stale-snapshot case above.
    raceStart.store(true, std::memory_order_release);
    while (raceSwaps.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    for (int frame = 0; frame < 40; ++frame) {
        const auto snapshot = coordinator.captureEthernet();
        ImGui::NewFrame();
        const auto action = panel.render("Ethernet", open, snapshot);
        ImGui::Render();
        coordinator.applyEthernet(action);
    }
    raceStop.store(true, std::memory_order_release);
    replugger.join();
    assert(raceSwaps.load(std::memory_order_acquire) > 0);
    assert(u1Frames > 0 && u2Frames > 0);
    ImGui::DestroyContext();
    std::printf("frontend device panel concurrency: 120 frames, U1=%d U2=%d swaps=%d\n",
                u1Frames, u2Frames, raceSwaps.load());
}
