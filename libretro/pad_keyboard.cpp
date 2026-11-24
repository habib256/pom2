// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_keyboard.hpp"

#include "hgrpaint/HgrFont.h"
#include <libretro.h>

#include <algorithm>
#include <string_view>

namespace pom2::libretro {
namespace {
constexpr std::string_view keys[] = {"1234567890-=", "QWERTYUIOP[]", "ASDFGHJKL;'\\", "ZXCVBNM,./` "};
constexpr std::string_view shiftedKeys[] = {"!@#$%^&*()_+", "qwertyuiop{}", "asdfghjkl:\"|", "zxcvbnm<>?~ "};
constexpr unsigned rows = 4;
constexpr unsigned cellWidth = 15;
constexpr unsigned cellHeight = 12;
}

std::uint8_t PadKeyboard::update(std::uint16_t buttons)
{
    const unsigned pressed = buttons & ~held;
    held = buttons;
    const auto edge = [pressed](unsigned id) { return (pressed & (1u << id)) != 0; };
    if (edge(RETRO_DEVICE_ID_JOYPAD_L3)) {
        open = !open;
        return 0;
    }
    if (!open) return 0;
    if (edge(RETRO_DEVICE_ID_JOYPAD_UP)) row = (row + rows - 1) % rows;
    if (edge(RETRO_DEVICE_ID_JOYPAD_DOWN)) row = (row + 1) % rows;
    const auto columns = static_cast<unsigned>(keys[row].size());
    column %= columns;
    if (edge(RETRO_DEVICE_ID_JOYPAD_LEFT)) column = (column + columns - 1) % columns;
    if (edge(RETRO_DEVICE_ID_JOYPAD_RIGHT)) column = (column + 1) % columns;
    if (edge(RETRO_DEVICE_ID_JOYPAD_Y)) shifted = !shifted;
    if (edge(RETRO_DEVICE_ID_JOYPAD_START)) {
        open = false;
        return 0x0d;
    }
    if (edge(RETRO_DEVICE_ID_JOYPAD_SELECT)) return ' ';
    if (edge(RETRO_DEVICE_ID_JOYPAD_X)) return 0x08;
    if (edge(RETRO_DEVICE_ID_JOYPAD_B)) return (shifted ? shiftedKeys : keys)[row][column];
    return 0;
}

void PadKeyboard::draw(std::uint32_t* pixels, unsigned width, unsigned height) const
{
    if (!open) return;
    const unsigned scale = width >= 560 ? 2 : 1;
    const unsigned panelWidth = 236;
    const unsigned xStart = (width - panelWidth * scale) / 2;
    const unsigned yStart = height - rows * cellHeight - 16;
    const auto fill = [&](unsigned x, unsigned y, unsigned w, unsigned h, std::uint32_t color) {
        for (unsigned dy = 0; dy < h; ++dy)
            std::fill_n(pixels + (y + dy) * width + xStart + x * scale, w * scale, color);
    };
    const auto glyph = [&](unsigned x, unsigned y, char ch, std::uint32_t color) {
        for (int gy = 0; gy < 8; ++gy)
            for (int gx = 0; gx < 7; ++gx)
                if (hgrpaint::bbFontPixel(ch, gx, gy)) fill(x + gx, y + gy, 1, 1, color);
    };
    fill(0, yStart, panelWidth, rows * cellHeight + 14, 0x15191d);
    for (unsigned r = 0; r < rows; ++r) {
        const auto line = (shifted ? shiftedKeys : keys)[r];
        for (unsigned c = 0; c < line.size(); ++c) {
            const unsigned x = (panelWidth - cellWidth * line.size()) / 2 + c * cellWidth;
            const unsigned y = yStart + 2 + r * cellHeight;
            const bool selected = row == r && column == c;
            fill(x, y, cellWidth - 1, cellHeight - 1, selected ? 0xb8ef7b : 0x35404b);
            glyph(x + 3, y + 2, line[c], selected ? 0x101810 : 0xffffff);
        }
    }
    constexpr std::string_view legend = "B KEY  Y SHIFT  X DEL  START CR";
    for (unsigned i = 0; i < legend.size(); ++i)
        glyph(8 + 7 * i, yStart + rows * cellHeight + 4, legend[i], 0xc6cecf);
}
} // namespace pom2::libretro
