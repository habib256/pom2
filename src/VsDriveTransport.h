// POM2 Apple II Emulator — GPL-3.0-or-later
// Copyright (C) 2026 VERHILLE Arnaud
#pragma once

#include "SuperSerialTransport.h"

#include <array>
#include <memory>
#include <string>

class SuperSerialCard;

namespace pom2 {

// Prepare both images before replacing the card's current transport. A failed
// mount leaves the existing serial connection untouched.
std::unique_ptr<SuperSerialTransport> makeVsDriveTransport(
    SuperSerialCard& card, const std::array<std::string, 2>& paths,
    std::string& error, const std::array<bool, 2>& readOnly = {});

} // namespace pom2
