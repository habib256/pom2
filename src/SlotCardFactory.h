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

// Central slot-card construction. This factory owns implementation selection,
// ROM discovery/validation and explicit fallbacks. Runtime wiring (CPU, audio,
// transports, mounted media and SlotBus ownership) remains with the composer
// or the additive session provisioner.

#ifndef POM2_SLOT_CARD_FACTORY_H
#define POM2_SLOT_CARD_FACTORY_H

#include "SlotPeripheral.h"
#include "SystemProfile.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class SuperSerialCard;

namespace pom2 {

class SlotCardFactory final
{
public:
    using ResourceLocator =
        std::function<std::string(std::string_view resource)>;

    struct Request {
        std::string key;
        int slot = -1;
        bool cpuIsCmos = false;
        SystemProfile profile = SystemProfile::AppleIIe;
    };

    struct Result {
        std::unique_ptr<SlotPeripheral> card;
        std::string requestedKey;
        std::string actualKey;
        std::string resourcePath;
        std::string status;
        std::string warningCategory;
        std::string warning;
        bool fallback = false;

        explicit operator bool() const noexcept { return card != nullptr; }
    };

    /// An empty locator selects the production ResourcePaths adapter.
    explicit SlotCardFactory(ResourceLocator locator = {});

    /// Handles the factory-owned subset of the slot catalog. Unknown or
    /// deliberately composer-owned keys return an empty Result with no warning.
    Result create(const Request& request) const;

    /// The Super Serial Card is composed by its caller (transport, keyboard
    /// sink, settings), but its EPROM is looked up here like every other card
    /// ROM. A //c-class profile gets none: its serial ports are built in and
    /// their firmware is the system ROM's. Returns the path loaded, or empty
    /// when the card keeps its hand-assembled page.
    std::string loadSuperSerialFirmware(SuperSerialCard& card,
                                        SystemProfile profile) const;

private:
    Result createMouse(const Request& request) const;
    ResourceLocator locate_;
};

} // namespace pom2

#endif // POM2_SLOT_CARD_FACTORY_H
