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

// makeFujiNetCard — the single place that decides what a FujiNet card's host
// side actually is.
//
// FujiNetCard takes its link, transport and `N:` device by injection so that
// it can stay a DEVICE: a card may not own a thread or a socket, and holding
// SpOverSlipLink by value was what forced the card up into RUNTIME. The cost
// of that is a three-argument constructor nobody wants to spell at a call
// site, so this is the counterweight — production code says
// `makeFujiNetCard(slot)` and gets the real wiring.
//
// Keeping the choice here also means there is exactly ONE edit to make if the
// production transport ever changes, and exactly one place to read to find out
// what a real card is made of.

#ifndef POM2_FUJINET_CARD_FACTORY_H
#define POM2_FUJINET_CARD_FACTORY_H

#include "FujiNetCard.h"

#include <memory>

namespace pom2 {

/// A FujiNet card wired to its production host side: an SpOverSlipLink (which
/// serves as both the command surface and the transport) plus POM2's built-in
/// `N:` network device. The transport is created OFF — the caller arms it
/// (setTcpMode / setSerialMode) and calls start().
std::unique_ptr<FujiNetCard> makeFujiNetCard(int slot = FujiNetCard::kDefaultSlot,
                                             bool allowLoopback = false);

} // namespace pom2

#endif // POM2_FUJINET_CARD_FACTORY_H
