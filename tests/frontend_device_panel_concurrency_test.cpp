// Headless ImGui frames across real slot changes. The UI must render a copied
// device snapshot after the card that supplied it has been unplugged, while
// coordinator captures and slot changes contend for the same machine lock.

#include "DevicePanelCoordinator.h"
#include "LeChatMauveCard.h"
#include "SystemProfile.h"
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

    // ── `chatmauve_variant` follows the LIVE machine, not a persisted key ──
    // On a //c-class machine the Chat Mauve model is hardware (the rear DB-15
    // is the Adaptateur IIc), so the key must not be written there. The guard
    // used to re-parse the PERSISTED `system_profile`, which the restore
    // paths deliberately never write — and `profileFromKey("")` answers a
    // ][+, i.e. "has slots". So on exactly the launches that take a restore
    // (a package carrying only the //c firmware; a saved profile reapplied)
    // the //c guard inverted, the key was written, and a later //e session
    // inherited the //c experiment (hunt #21).
    {
        pom2::Settings s;
        pom2::DevicePanelCoordinator c(controller, s);
        // The coordinator resolves the card from the bus and returns early
        // without one, so the adapter has to be live for the write to be
        // reachable at all.
        {
            auto state = controller.lockState();
            state.memory().slotBus().unplug(3);
            state.memory().slotBus().plug(
                7, std::make_unique<LeChatMauveCard>(7));
        }
        pom2::LeChatMauve_ImGui::FrameResult variant{};
        variant.requestVariant = true;
        variant.variantTo      = LeChatMauveCard::Variant::Feline;

        // No `system_profile` key — the restore-path state — on a live //c.
        assert(!s.has("system_profile"));
        c.setProfileProvider([] { return pom2::SystemProfile::AppleIIcPAL; });
        c.applyChatMauve(variant);
        assert(!s.has("chatmauve_variant"));

        // The same call on a live //e does persist it.
        c.setProfileProvider([] { return pom2::SystemProfile::AppleIIePAL; });
        c.applyChatMauve(variant);
        assert(s.getString("chatmauve_variant", "") ==
               LeChatMauveCard::variantKey(LeChatMauveCard::Variant::Feline));

        // And a persisted key naming a //e cannot talk a live //c into it.
        pom2::Settings mismatched;
        mismatched.setString("system_profile", "iie-pal");
        pom2::DevicePanelCoordinator c2(controller, mismatched);
        c2.setProfileProvider([] { return pom2::SystemProfile::AppleIIc; });
        c2.applyChatMauve(variant);
        assert(!mismatched.has("chatmauve_variant"));
    }
    std::printf("  ok: chatmauve_variant follows the live profile\n");
}
