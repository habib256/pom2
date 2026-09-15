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

// Transactional SlotBus rebuild order and phase contract.

#include "EmulationController.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SlotRebuildCoordinator.h"

#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ProbeCard final : public SlotPeripheral
{
public:
    explicit ProbeCard(std::vector<std::string>& events) : events_(events) {}
    ~ProbeCard() override { events_.push_back("clear-slot-bus"); }
    std::string_view name() const override { return "rebuild probe"; }

private:
    std::vector<std::string>& events_;
};

void expectLogicError(const std::function<void()>& operation)
{
    bool rejected = false;
    try {
        operation();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main()
{
    EmulationController controller;
    std::vector<std::string> events;

    // No lifecycle edge is optional: a partially-wired coordinator would
    // make the apparent transaction less safe than the explicit old code.
    expectLogicError([] {
        pom2::SlotRebuildCoordinator invalid({});
    });

    pom2::SlotRebuildCoordinator rebuild({
        [&] { events.push_back("invalidate-history"); },
        [&] { events.push_back("detach-control"); },
        [&] { events.push_back("detach-audio"); },
        [&] { events.push_back("detach-frontend"); },
        [&] { events.push_back("reset-printer"); },
        [&] {
            // The whole point of the phase: the join runs with the machine
            // lock RELEASED. A future move of this hook back inside
            // beginLocked fails here.
            assert(controller.stateMutex().try_lock());
            controller.stateMutex().unlock();
            events.push_back("stop-network");
        },
        [&] { events.push_back("detach-display"); },
        [&] { events.push_back("publish-control"); },
    });

    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Stable);
    assert(rebuild.generation() == 0);

    // The bus cannot be cleared without first proving media was flushed,
    // and host workers cannot stop before that flush.
    {
        auto state = controller.lockState();
        expectLogicError([&] { rebuild.beginLocked(state); });
    }
    expectLogicError([&] { rebuild.stopHostWorkers(); });
    assert(events.empty());

    rebuild.prepareAfterFlush();
    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Prepared);
    assert(rebuild.generation() == 1);
    assert((events == std::vector<std::string>{"invalidate-history"}));
    expectLogicError([&] { rebuild.prepareAfterFlush(); });

    // Clearing the bus still requires the host workers to have been
    // joined with the lock released.
    {
        auto state = controller.lockState();
        expectLogicError([&] { rebuild.beginLocked(state); });
    }

    rebuild.stopHostWorkers();
    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::WorkersStopped);
    assert((events == std::vector<std::string>{
        "invalidate-history",
        "stop-network",
    }));
    expectLogicError([&] { rebuild.stopHostWorkers(); });

    {
        auto state = controller.lockState();
        state.memory().slotBus().plug(4, std::make_unique<ProbeCard>(events));
        rebuild.beginLocked(state);
        assert(!state.memory().slotBus().isPlugged(4));
        assert(rebuild.phase() ==
               pom2::SlotRebuildCoordinator::Phase::Rebuilding);
        expectLogicError([&] { rebuild.beginLocked(state); });

        const std::vector<std::string> expected{
            "invalidate-history",
            "stop-network",
            "detach-control",
            "detach-audio",
            "detach-frontend",
            "reset-printer",
            "clear-slot-bus",
            "detach-display",
        };
        assert(events == expected);

        // Publication occurs only once the caller declares the rebuilt
        // topology coherent while still holding stateMutex.
        rebuild.publishLocked(state);
    }

    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Stable);
    assert(events.back() == "publish-control");

    {
        auto state = controller.lockState();
        expectLogicError([&] { rebuild.publishLocked(state); });
    }
    assert(events.back() == "publish-control");

    // A throwing join must not wedge the next Apply at Prepared.
    {
        bool failJoin = true;
        pom2::SlotRebuildCoordinator wedged({
            [] {},
            [] {},
            [] {},
            [] {},
            [] {},
            [&] {
                if (failJoin) {
                    failJoin = false;
                    throw std::runtime_error("join failed");
                }
            },
            [] {},
            [] {},
        });
        wedged.prepareAfterFlush();
        bool threw = false;
        try {
            wedged.stopHostWorkers();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
        assert(wedged.phase() ==
               pom2::SlotRebuildCoordinator::Phase::Stable);
        wedged.prepareAfterFlush();
        assert(wedged.phase() ==
               pom2::SlotRebuildCoordinator::Phase::Prepared);
        wedged.stopHostWorkers();
        assert(wedged.phase() ==
               pom2::SlotRebuildCoordinator::Phase::WorkersStopped);
        {
            auto state = controller.lockState();
            wedged.beginLocked(state);
            wedged.publishLocked(state);
        }
    }

    return 0;
}
