// POM2 Apple II Emulator
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pom2::libretro {

// Per-game RetroPad profiles. A keyboard game on a pad: the D-pad types the
// game's movement keys, the face buttons its actions, Start its start key.
// A game is found by its title — a WOZ disk's META `title`, or the file name
// with its (year)(publisher)[crack] tags dropped — and each profile names the
// manual or reference card its keys come from (pad_profiles.cpp).
//
// A profile is one line: `button=value[:label]` items joined by ';'.
//   button  up down left right b a y x l r l2 r2 start select
//   value   joy (the D-pad moves the joystick, the default) · b0 · b1
//           (joystick buttons) · none · a key: a letter or digit,
//           space return esc tab delete, the arrows left right up down,
//           ctrl-<letter>, or one punctuation character
//   label   what the key does in the game, shown in RetroArch's controls

enum class PadAction : std::uint8_t { Default, Joystick, Button0, Button1, Key, None };

struct PadAssign {
    PadAction action = PadAction::Default;
    std::uint8_t ascii = 0;     ///< the character typed (Key)
    std::uint16_t token = 0;    ///< the RETROK held while the button is (Key)
    std::string label;          ///< for the controls menu
};

struct PadProfile {
    std::string title;
    std::string source;
    bool joystick = false;                  ///< the game plays with a joystick
    std::array<PadAssign, 16> buttons{};   ///< indexed by RETRO_DEVICE_ID_JOYPAD_*
    bool dpadKeys() const;                  ///< the D-pad types keys
};

/// Parse a profile line; false and `error` set on a bad item.
bool parsePadSpec(std::string_view spec, PadProfile& out, std::string& error);

/// "Lode Runner (1983)(Broderbund)[cr].dsk" -> "loderunner": lowercase
/// letters and digits, (…) and […] tags and the extension dropped.
std::string normalizeTitle(std::string_view name);

/// The profile of the first name that matches one, or nullptr. A file name
/// loses its extension; pass a bare title (a WOZ META title) with ".x".
const PadProfile* findPadProfile(const std::vector<std::string>& names);

/// Every profile in the table, parsed (for tests and the docs).
const std::vector<PadProfile>& padProfiles();

} // namespace pom2::libretro
