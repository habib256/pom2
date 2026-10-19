// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"
#include "SystemProfile.h"
#include <functional>
#include <memory>
class EmulationController;
namespace pom2 {
class StateAccess;
// Frontends detach their non-owning views before replacement and publish the
// new inventory afterwards. Both callbacks run on the owning thread, locked.
struct SessionMachineHooks {
    std::function<void(const StateAccess&)> detach;
    std::function<void(const StateAccess&, SystemProfile)> publish;
};
bool reconstructSession(EmulationController& controller, const SessionArchive& session,
                        std::string& error, const SessionMachineHooks& hooks = {});
}
