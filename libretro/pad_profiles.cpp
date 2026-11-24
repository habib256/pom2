// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_profiles.hpp"

#include <libretro.h>

#include <cctype>

namespace pom2::libretro {
namespace {

// One game. `joystick` says the game plays with an Apple II joystick, which
// then has priority (the user's rule, 2026-11-24): its profile keeps the
// D-pad on the joystick and B/A on its buttons, and only adds keys on the
// other buttons. A keyboard-only game gets its movement keys on the D-pad.
// Every row cites what its keys were read from.
struct ProfileRow {
    const char* title;
    const char* aliases;   ///< more titles, '|'-separated, or ""
    bool joystick;
    const char* spec;
    const char* source;
};

#include "pad_profiles_data.inc"

struct ButtonName { const char* name; unsigned id; };
constexpr ButtonName kButtons[] = {
    {"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
    {"b", RETRO_DEVICE_ID_JOYPAD_B}, {"a", RETRO_DEVICE_ID_JOYPAD_A},
    {"y", RETRO_DEVICE_ID_JOYPAD_Y}, {"x", RETRO_DEVICE_ID_JOYPAD_X},
    {"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R},
    {"l2", RETRO_DEVICE_ID_JOYPAD_L2}, {"r2", RETRO_DEVICE_ID_JOYPAD_R2},
    {"start", RETRO_DEVICE_ID_JOYPAD_START}, {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
};

std::string lower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// A key name to the character it types and the RETROK it holds.
bool parseKey(std::string_view name, PadAssign& out)
{
    const auto key = lower(name);
    struct Named { const char* name; std::uint8_t ascii; std::uint16_t token; };
    static constexpr Named kNamed[] = {
        {"space", ' ', RETROK_SPACE}, {"return", 0x0d, RETROK_RETURN}, {"esc", 0x1b, RETROK_ESCAPE},
        {"tab", 0x09, RETROK_TAB}, {"delete", 0x7f, RETROK_DELETE},
        {"left", 0x08, RETROK_LEFT}, {"right", 0x15, RETROK_RIGHT},
        {"up", 0x0b, RETROK_UP}, {"down", 0x0a, RETROK_DOWN},
    };
    for (const auto& n : kNamed)
        if (key == n.name) { out.ascii = n.ascii; out.token = n.token; return true; }
    if (key.size() == 6 && key.compare(0, 5, "ctrl-") == 0 && key[5] >= 'a' && key[5] <= 'z') {
        out.ascii = static_cast<std::uint8_t>(key[5] - 'a' + 1);
        out.token = static_cast<std::uint16_t>(RETROK_a + (key[5] - 'a'));
        return true;
    }
    if (key.size() != 1) return false;
    const char c = key[0];
    if (c >= 'a' && c <= 'z') {   // the Apple II's own case: games read capitals
        out.ascii = static_cast<std::uint8_t>(c - 'a' + 'A');
        out.token = static_cast<std::uint16_t>(RETROK_a + (c - 'a'));
        return true;
    }
    if (c >= '0' && c <= '9') {
        out.ascii = static_cast<std::uint8_t>(c);
        out.token = static_cast<std::uint16_t>(RETROK_0 + (c - '0'));
        return true;
    }
    // Punctuation: its own US key, unshifted where it has one.
    struct Punct { char c; std::uint16_t token; };
    static constexpr Punct kPunct[] = {
        {',', RETROK_COMMA}, {'.', RETROK_PERIOD}, {'/', RETROK_SLASH}, {';', RETROK_SEMICOLON},
        {'\'', RETROK_QUOTE}, {'[', RETROK_LEFTBRACKET}, {']', RETROK_RIGHTBRACKET},
        {'-', RETROK_MINUS}, {'=', RETROK_EQUALS}, {'\\', RETROK_BACKSLASH}, {'`', RETROK_BACKQUOTE},
        {'<', RETROK_COMMA}, {'>', RETROK_PERIOD}, {'?', RETROK_SLASH}, {':', RETROK_SEMICOLON},
    };
    for (const auto& p : kPunct)
        if (c == p.c) { out.ascii = static_cast<std::uint8_t>(c); out.token = p.token; return true; }
    return false;
}

} // namespace

bool PadProfile::dpadKeys() const
{
    for (unsigned id : {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT})
        if (buttons[id].action == PadAction::Key) return true;
    return false;
}

bool parsePadSpec(std::string_view spec, PadProfile& out, std::string& error)
{
    while (!spec.empty()) {
        const auto semi = spec.find(';');
        const auto item = trim(spec.substr(0, semi));
        spec = semi == std::string_view::npos ? std::string_view() : spec.substr(semi + 1);
        if (item.empty()) continue;
        const auto eq = item.find('=');
        if (eq == std::string_view::npos) { error = "no '=' in \"" + std::string(item) + "\""; return false; }
        const auto button = lower(trim(item.substr(0, eq)));
        auto value = trim(item.substr(eq + 1));
        std::string label;
        // A label follows the LAST ':' — the key itself may be ':' (`a=::Jump`).
        if (const auto colon = value.rfind(':'); colon != std::string_view::npos && colon > 0) {
            label = std::string(trim(value.substr(colon + 1)));
            value = trim(value.substr(0, colon));
        }
        const ButtonName* which = nullptr;
        for (const auto& b : kButtons)
            if (button == b.name) which = &b;
        if (!which) { error = "unknown button \"" + button + "\""; return false; }
        PadAssign assign;
        assign.label = label;
        const auto v = lower(value);
        if (v == "joy") assign.action = PadAction::Joystick;
        else if (v == "b0") assign.action = PadAction::Button0;
        else if (v == "b1") assign.action = PadAction::Button1;
        else if (v == "none") assign.action = PadAction::None;
        else if (parseKey(value, assign)) assign.action = PadAction::Key;
        else { error = "unknown key \"" + std::string(value) + "\" for " + button; return false; }
        out.buttons[which->id] = assign;
    }
    // The D-pad moves the joystick or types keys, not a mixture; a direction
    // the game has no key for says `none` (Wizardry cannot step back).
    int keys = 0, nones = 0;
    for (unsigned id : {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT}) {
        keys += out.buttons[id].action == PadAction::Key;
        nones += out.buttons[id].action == PadAction::None;
    }
    if (keys != 0 && keys + nones != 4) {
        error = "a D-pad that types keys gives each direction a key or none";
        return false;
    }
    return true;
}

std::string normalizeTitle(std::string_view name)
{
    // The file name, without its folder or extension.
    if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0 && name.size() - dot <= 6)
        name = name.substr(0, dot);
    std::string out;
    int depth = 0;
    for (char c : name) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth) continue;
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // A leading article ("The Bard's Tale" and "Bard's Tale").
    if (out.size() > 3 && out.compare(0, 3, "the") == 0) out.erase(0, 3);
    return out;
}

const std::vector<PadProfile>& padProfiles()
{
    static const std::vector<PadProfile> profiles = [] {
        std::vector<PadProfile> all;
        for (const ProfileRow* row = kProfileRows; row->title; ++row) {
            PadProfile p;
            p.title = row->title;
            p.source = row->source;
            p.joystick = row->joystick;
            std::string error;
            if (!parsePadSpec(row->spec, p, error)) continue;   // the unit test refuses it first
            all.push_back(std::move(p));
        }
        return all;
    }();
    return profiles;
}

const PadProfile* findPadProfile(const std::vector<std::string>& names)
{
    const auto& profiles = padProfiles();
    for (const auto& name : names) {
        const auto wanted = normalizeTitle(name);
        if (wanted.empty()) continue;
        for (const ProfileRow* row = kProfileRows; row->title; ++row) {
            // A title is not a file name: ".x" keeps "Mr. Do" from losing "Do".
            const std::string titles = std::string(row->title) + "|" + row->aliases;
            std::size_t start = 0;
            bool hit = false;
            while (start <= titles.size() && !hit) {
                auto bar = titles.find('|', start);
                if (bar == std::string::npos) bar = titles.size();
                hit = bar > start && normalizeTitle(titles.substr(start, bar - start) + ".x") == wanted;
                start = bar + 1;
            }
            if (!hit) continue;
            for (const auto& p : profiles)
                if (p.title == row->title) return &p;
        }
    }
    return nullptr;
}

} // namespace pom2::libretro
