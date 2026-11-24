// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace pom2::libretro {

struct PadKeyboard {
    std::uint16_t held = 0;
    std::uint8_t column = 0;
    std::uint8_t row = 0;
    bool open = false;
    bool shifted = false;

    // Return an Apple key (zero means no key) on a rising RetroPad button.
    std::uint8_t update(std::uint16_t buttons);
    void draw(std::uint32_t* pixels, unsigned width, unsigned height) const;
};

} // namespace pom2::libretro
