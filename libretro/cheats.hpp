// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pom2::libretro {

// One RAM write a cheat applies before every frame. Addresses are those of
// the core's memory map, which RetroAchievements uses too: $00000-$0BFFF main
// RAM, $0C000-$0CFFF the language card's $D000 bank 1, $0D000-$0DFFF its bank
// 2, $0E000-$0FFFF its upper RAM, and the same in auxiliary RAM at $10000.
struct CheatWrite {
    std::uint32_t address = 0;
    std::uint8_t value = 0;
    std::optional<std::uint8_t> compare;   // write only while the cell holds this
};

// Parse one cheat: codes joined by '+', ';', ',' or whitespace runs between
// codes, each "ADDR:VV", "ADDR=VV", "ADDR?CC:VV" (compare) or the 8-hex-digit
// "AAAAAAVV". Hexadecimal, an optional '$' or "0x" prefix. Returns no value
// for text that is not a cheat.
std::optional<std::vector<CheatWrite>> parseCheat(std::string_view code);

} // namespace pom2::libretro
