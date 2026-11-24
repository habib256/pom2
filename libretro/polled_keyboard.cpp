// POM2 Apple II Emulator
// SPDX-License-Identifier: GPL-3.0-or-later
#include "polled_keyboard.hpp"

#include <libretro.h>

#include <array>

namespace pom2::libretro {

namespace {

constexpr auto kKeys = [] {
    std::array<unsigned, 26 + 10 + 11 + 12 + 7> keys{};
    std::size_t n = 0;
    for (unsigned k = RETROK_a; k <= RETROK_z; ++k) keys[n++] = k;
    for (unsigned k = RETROK_0; k <= RETROK_9; ++k) keys[n++] = k;
    for (unsigned k : {RETROK_BACKQUOTE, RETROK_MINUS, RETROK_EQUALS, RETROK_LEFTBRACKET,
                       RETROK_RIGHTBRACKET, RETROK_BACKSLASH, RETROK_SEMICOLON, RETROK_QUOTE,
                       RETROK_COMMA, RETROK_PERIOD, RETROK_SLASH})
        keys[n++] = k;
    for (unsigned k : {RETROK_SPACE, RETROK_RETURN, RETROK_KP_ENTER, RETROK_BACKSPACE, RETROK_TAB,
                       RETROK_ESCAPE, RETROK_DELETE, RETROK_LEFT, RETROK_RIGHT, RETROK_UP, RETROK_DOWN,
                       RETROK_CAPSLOCK})
        keys[n++] = k;
    for (unsigned k : {RETROK_LSHIFT, RETROK_RSHIFT, RETROK_LCTRL, RETROK_RCTRL, RETROK_LALT,
                       RETROK_RALT, RETROK_LSUPER})
        keys[n++] = k;
    return keys;
}();

} // namespace

const unsigned* polledKeys(std::size_t& count)
{
    count = kKeys.size();
    return kKeys.data();
}

std::uint32_t usCharacter(unsigned keycode, bool shift)
{
    if (keycode >= RETROK_a && keycode <= RETROK_z)
        return shift ? keycode - RETROK_a + 'A' : keycode;
    if (keycode >= RETROK_0 && keycode <= RETROK_9)
        return shift ? static_cast<std::uint32_t>(")!@#$%^&*("[keycode - RETROK_0]) : keycode;
    switch (keycode) {
    case RETROK_SPACE:        return ' ';
    case RETROK_BACKQUOTE:    return shift ? '~' : '`';
    case RETROK_MINUS:        return shift ? '_' : '-';
    case RETROK_EQUALS:       return shift ? '+' : '=';
    case RETROK_LEFTBRACKET:  return shift ? '{' : '[';
    case RETROK_RIGHTBRACKET: return shift ? '}' : ']';
    case RETROK_BACKSLASH:    return shift ? '|' : '\\';
    case RETROK_SEMICOLON:    return shift ? ':' : ';';
    case RETROK_QUOTE:        return shift ? '"' : '\'';
    case RETROK_COMMA:        return shift ? '<' : ',';
    case RETROK_PERIOD:       return shift ? '>' : '.';
    case RETROK_SLASH:        return shift ? '?' : '/';
    default:                  return 0;
    }
}

} // namespace pom2::libretro
