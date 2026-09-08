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

#include "FujiNetCardFactory.h"

#include "FujiNetNetDevice.h"
#include "SpOverSlipLink.h"

namespace pom2 {

std::unique_ptr<FujiNetCard> makeFujiNetCard(int slot, bool allowLoopback)
{
    auto link = std::make_unique<SpOverSlipLink>();
    // The card takes ownership of the link through the FujiNetLink interface,
    // so grab the transport reference BEFORE the move — afterwards `link` is
    // empty even though the object it pointed at is very much alive.
    SpOverSlipLink& transport = *link;
    auto net = std::make_unique<FujiNetNetDevice>();
    net->setAllowLoopback(allowLoopback);
    return std::make_unique<FujiNetCard>(slot, std::move(link), transport,
                                         std::move(net));
}

} // namespace pom2
