// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// The per-game RetroPad profiles of the libretro core (libretro/pad_profiles.*):
// the profile line, the title normalisation, the lookup, and the rules every
// row of the table must keep — a game that plays with a joystick keeps the
// D-pad and B/A on it (only its other buttons get keys), every row parses
// and cites a source, and no two rows share a title.

#include "pad_profiles.hpp"

#include <libretro.h>

#include <cstdio>
#include <set>
#include <string>

using namespace pom2::libretro;

namespace {
int failures = 0;
void check(bool ok, const std::string& what)
{
    if (!ok) { std::printf("  [FAIL] %s\n", what.c_str()); ++failures; }
}
} // namespace

int main()
{
    std::printf("== libretro pad profiles ==\n");
    // The line.
    PadProfile p;
    std::string error;
    check(parsePadSpec("up=I;down=M;left=J;right=K;b=U:Dig left;a=O:Dig right;start=ctrl-p:Pause;"
                       "select=esc;y=space;x=,", p, error), "a keyboard game's line parses: " + error);
    check(p.dpadKeys(), "four keys on the D-pad");
    check(p.buttons[RETRO_DEVICE_ID_JOYPAD_UP].ascii == 'I' &&
          p.buttons[RETRO_DEVICE_ID_JOYPAD_UP].token == RETROK_i, "a letter types its capital, holds its key");
    check(p.buttons[RETRO_DEVICE_ID_JOYPAD_B].label == "Dig left", "a label is kept");
    check(p.buttons[RETRO_DEVICE_ID_JOYPAD_START].ascii == 0x10, "ctrl-p is $10");
    check(p.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT].ascii == 0x1b, "esc is $1B");
    check(p.buttons[RETRO_DEVICE_ID_JOYPAD_X].ascii == ',' &&
          p.buttons[RETRO_DEVICE_ID_JOYPAD_X].token == RETROK_COMMA, "punctuation types itself");
    PadProfile q;
    check(parsePadSpec("a=::Jump", q, error) && q.buttons[RETRO_DEVICE_ID_JOYPAD_A].ascii == ':' &&
          q.buttons[RETRO_DEVICE_ID_JOYPAD_A].label == "Jump", "':' as a key, with a label");
    PadProfile bad;
    check(!parsePadSpec("up=I;down=M", bad, error), "half a D-pad is refused");
    PadProfile three;
    check(parsePadSpec("up=F;down=none;left=L;right=R", three, error) && three.dpadKeys(),
          "a direction with no key says none");
    check(!parsePadSpec("b=F13", bad, error), "an unknown key is refused");
    check(!parsePadSpec("z=A", bad, error), "an unknown button is refused");
    PadProfile joy;
    check(parsePadSpec("b=b0;a=b1;start=space:Start;up=joy", joy, error) && !joy.dpadKeys(),
          "a joystick game's line parses");

    // Titles.
    check(normalizeTitle("Lode Runner (1983)(Broderbund)[cr].dsk") == "loderunner", "tags and extension go");
    check(normalizeTitle("/disks/The Bard's Tale.woz") == "bardstale", "folder, article and apostrophe go");
    check(normalizeTitle("Mr. Do.x") == "mrdo", "a dotted title keeps its words");

    // The table.
    std::set<std::string> titles;
    check(padProfiles().size() == padProfileRowCount(), "every row of the table parses (" +
          std::to_string(padProfiles().size()) + " of " + std::to_string(padProfileRowCount()) + ")");
    for (const auto& profile : padProfiles()) {
        const std::string who = profile.title;
        check(!profile.source.empty(), who + " cites a source");
        check(titles.insert(normalizeTitle(profile.title + ".x")).second, who + " appears once");
        if (profile.joystick) {
            check(!profile.dpadKeys(), who + ": a joystick game keeps the D-pad on the joystick");
            for (unsigned id : {RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A}) {
                const auto action = profile.buttons[id].action;
                check(action == PadAction::Default || action == PadAction::Button0 ||
                      action == PadAction::Button1, who + ": a joystick game keeps B and A on its buttons");
            }
        }
        check(findPadProfile({profile.title + ".x"}) == &profile, who + " is found by its title");
        check(findPadProfile({"/games/" + profile.title + " (1984)(Publisher).woz"}) == &profile,
              who + " is found by a file name");
    }
    check(findPadProfile({"No Such Game.dsk"}) == nullptr, "an unknown game has no profile");
    std::printf("%zu profiles checked\n%s (%d failure%s)\n", padProfiles().size(),
                failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
