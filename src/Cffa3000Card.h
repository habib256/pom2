// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef POM2_CFFA3000_CARD_H
#define POM2_CFFA3000_CARD_H

#include "SmartPortCard.h"

namespace pom2 {

// Host-served SmartPort half of a CFFA3000. The real board's AVR creates
// writable slot firmware at runtime; its CFFA 2.0 ATA ROM is unrelated.
// Reuse the SmartPort command and media implementation, with the CFFA3000's
// eight default SmartPort devices. A Disk II card in another slot supplies
// the board's optional virtual floppy slot in the HLE configuration.
class Cffa3000Card final : public SmartPortCard
{
public:
    explicit Cffa3000Card(int slot) : SmartPortCard(slot)
    { setUnitCount(8); }

    std::string_view name() const override { return "CFFA3000 (SmartPort)"; }

protected:
    // Not a cable chain: the board's eight SmartPort devices are fixed slots
    // in its own firmware, numbered whether a medium is in them or not, so
    // device n is bay n and nothing is compacted.
    bool compactsChain() const override { return false; }
};

} // namespace pom2

#endif // POM2_CFFA3000_CARD_H
