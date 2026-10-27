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

// Two guards over the keyboard input path, both for defects that are only
// reachable on hardware CI does not have.
//
// 1. The POLICY in KeyChord.h — GLFW-free by construction, so the AZERTY /
//    QWERTZ / macOS-French cases that broke it can be driven here instead of
//    on a borrowed laptop.
//
// 2. A SOURCE SCAN for the Emscripten GLFW entry points that are implemented
//    as `abort('… not implemented.')` in the upstream port. An abort() tears
//    the whole WASM module down — the page reports a load failure and the
//    machine is gone — and none of these are compile errors, so the browser
//    build ships them happily. `glfwGetKeyName` was reached on EVERY Ctrl+key
//    in the browser: the first Ctrl-C in Applesoft killed the session. The
//    scan is a mini-preprocessor: it walks the #if/#ifdef/#else/#endif nesting
//    and fails on any call site that is still compiled when __EMSCRIPTEN__ is
//    defined.

#include "KeyChord.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef POM2_SOURCE_DIR
#define POM2_SOURCE_DIR "."
#endif
#define POM2_STR2(x) #x
#define POM2_STR(x) POM2_STR2(x)

namespace {

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

using namespace pom2::keychord;

// ── 1. The policy ───────────────────────────────────────────────────────

void testLayoutLetter()
{
    // A layout that names the key with a letter wins over the position.
    // AZERTY: the key at QWERTY's A position is capped 'Q'.
    check(letterFromKeyName("q", kKeyA) == 'Q',
          "the LAYOUT's letter wins over the US position");
    check(letterFromKeyName("A", kKeyA) == 'A', "an uppercase name is kept");

    // The regression: a key whose layout name is NOT a letter must yield
    // nothing. On AZERTY the key at QWERTY's M position types ',' — the old
    // fallback called it 'M' and Ctrl+',' arrived as Ctrl-M, i.e. RETURN.
    check(letterFromKeyName(",", 77 /* GLFW_KEY_M */) == 0,
          "a punctuation key is not the US letter at its position");
    check(letterFromKeyName(";", 77) == 0, "…for any punctuation");
    check(letterFromKeyName("\xd1\x81", kKeyA + 2) == 'C',
          "a non-Latin cap falls back to the US position — Ctrl-C must reach Applesoft");
    // A multi-byte cap (non-Latin layout) is not a Ctrl-letter either.
    // A multi-byte cap (a non-Latin layout) is NOT "no letter": the key
    // falls back to its US position, or Ctrl-C could never break Applesoft.
    check(letterFromKeyName("\xD0\xB9", kKeyA) != 0,
          "a multi-byte key name falls back to the US position");

    // The fallback survives for the case it was written for: GLFW has no
    // name at all (non-printable key, or the browser, where the entry point
    // must not even be called).
    check(letterFromKeyName(nullptr, kKeyA) == 'A',
          "no name at all falls back to the US position");
    check(letterFromKeyName("", kKeyZ) == 'Z', "…as does an empty name");
    check(letterFromKeyName(nullptr, 256 /* GLFW_KEY_ESCAPE */) == 0,
          "and a non-letter position is still nothing");
}

void testAltGr()
{
    // Windows AltGr = the RIGHT Alt reported as CONTROL|ALT.
    const int altGrMods = kModControl | kModAlt;
    check(isAltGr(altGrMods, /*rightAltHeld=*/true, /*windows=*/true),
          "Windows: right Alt + CONTROL|ALT is AltGr");
    check(!isAltGr(altGrMods, /*rightAltHeld=*/false, true),
          "Windows: the LEFT Alt with Ctrl is a real Ctrl+Alt chord");
    check(!isAltGr(altGrMods, true, /*windows=*/false),
          "elsewhere the same modifiers are a real Ctrl+Alt chord");
    check(!isAltGr(kModAlt, true, true),
          "Alt alone is not AltGr (Linux AltGr carries no CONTROL)");

    // Chords must not fire on AltGr: AltGr+F and AltGr+G are how several
    // layouts type ordinary characters, and both chords are routed even when
    // an ImGui text field has focus.
    check(!chordMayFire(altGrMods, true, true),
          "Windows AltGr does not fire Ctrl+Alt+F / Ctrl+Alt+G");
    check(chordMayFire(altGrMods, false, true),
          "a left-Alt Ctrl+Alt chord still fires on Windows");
    check(chordMayFire(altGrMods, true, false),
          "and every chord still fires off Windows");
}

void testCtrlLetter()
{
    check(isCtrlLetter(kModControl, false, true), "plain Ctrl injects a Ctrl-letter");
    check(isCtrlLetter(kModControl | kModShift, false, false), "Ctrl+Shift still does");
    check(!isCtrlLetter(kModControl | kModAlt, /*rightAlt=*/true, /*windows=*/true),
          "Windows AltGr does NOT (AltGr+E typed $05)");
    check(isCtrlLetter(kModControl | kModAlt, /*rightAlt=*/false, /*windows=*/true),
          "Open-Apple (left Alt) + Ctrl is still a Ctrl-letter: OA+Ctrl+S is $13");
    check(isCtrlLetter(kModControl | kModAlt, true, /*windows=*/false),
          "off Windows, Solid-Apple + Ctrl is a Ctrl-letter too");
    check(!isCtrlLetter(kModAlt, false, false), "Alt alone is not Ctrl");
    check(!isCtrlLetter(0, false, false), "no modifier is not Ctrl");
}

void testAltAppleKeys()
{
    check(altDrivesAppleKeys(0, /*enabled=*/true, /*windows=*/false, false),
          "Alt drives Open/Solid Apple by default");
    check(!altDrivesAppleKeys(0, /*enabled=*/false, false, false),
          "keyboard_alt_apple_keys=false takes the wire away "
          "(macOS French Option types { } [ ] | and was pressing fire)");
    check(altDrivesAppleKeys(kModControl | kModAlt, true, /*windows=*/true,
                             /*isRightAlt=*/false),
          "Windows Ctrl + LEFT Alt is not AltGr — Open-Apple + Ctrl must still work");
    check(!altDrivesAppleKeys(kModControl | kModAlt, true, /*windows=*/true,
                              /*isRightAlt=*/true),
          "Windows AltGr never presses Solid-Apple");
    check(altDrivesAppleKeys(kModControl | kModAlt, true, /*windows=*/false,
                             /*isRightAlt=*/true),
          "…but the same modifiers elsewhere still do");
}

// ── 2. The Emscripten abort()-stub scan ─────────────────────────────────

// GLFW entry points Emscripten's port implements as abort(). Adding one here
// is cheap; the cost of a missing one is a browser session that dies on a
// keystroke.
const char* const kAbortStubs[] = {
    "glfwGetKeyName",
    "glfwSetWindowMonitor",
    "glfwSetWindowIcon",
    "glfwSetWindowSizeLimits",
    "glfwSetWindowAspectRatio",
    "glfwGetWindowFrameSize",
};

const char* const kScannedFiles[] = {
    "src/main.cpp",
    "src/MainWindow.cpp",
    "src/MainWindow_Input.cpp",
    "src/MainWindow_Kiosk.cpp",
    "src/MainWindow_Slots.cpp",
    "src/MainWindow_Chrome.cpp",
    "src/JoystickInput.cpp",
};

/// Would a line at this preprocessor nesting be COMPILED with __EMSCRIPTEN__
/// defined? The stack holds one entry per open conditional:
///   +1 = "this branch requires __EMSCRIPTEN__ to be UNdefined"
///    0 = anything else (unrelated condition, or the Emscripten branch)
/// A line is browser-reachable when no open frame excludes Emscripten.
struct PpFrame { bool excludesEmscripten; bool sawEmscriptenCond; };

bool scanFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        std::printf("FAIL: cannot open %s for the Emscripten scan\n",
                    path.c_str());
        ++failures;
        return false;
    }
    std::vector<PpFrame> stack;
    std::string line;
    int lineNo = 0;
    bool ok = true;
    while (std::getline(f, line)) {
        ++lineNo;
        std::string s = line;
        // left-trim
        std::size_t b = s.find_first_not_of(" \t");
        s = (b == std::string::npos) ? std::string() : s.substr(b);

        if (!s.empty() && s[0] == '#') {
            const bool hasEms = s.find("__EMSCRIPTEN__") != std::string::npos;
            if (s.rfind("#ifndef", 0) == 0)
                stack.push_back({hasEms, hasEms});
            else if (s.rfind("#ifdef", 0) == 0 || s.rfind("#if", 0) == 0)
                stack.push_back({false, hasEms});
            else if (s.rfind("#elif", 0) == 0) {
                if (!stack.empty()) stack.back().excludesEmscripten = false;
            } else if (s.rfind("#else", 0) == 0) {
                // The other branch of an __EMSCRIPTEN__ conditional flips it.
                if (!stack.empty() && stack.back().sawEmscriptenCond)
                    stack.back().excludesEmscripten =
                        !stack.back().excludesEmscripten;
            } else if (s.rfind("#endif", 0) == 0) {
                if (!stack.empty()) stack.pop_back();
            }
            continue;
        }
        if (s.rfind("//", 0) == 0) continue;          // whole-line comment

        bool browserReachable = true;
        for (const auto& fr : stack)
            if (fr.excludesEmscripten) { browserReachable = false; break; }
        if (!browserReachable) continue;

        for (const char* fn : kAbortStubs) {
            const std::string needle = std::string(fn) + "(";
            const std::size_t at = s.find(needle);
            if (at == std::string::npos) continue;
            // Ignore a mention inside a trailing comment.
            const std::size_t cm = s.find("//");
            if (cm != std::string::npos && cm < at) continue;
            std::printf("FAIL: %s:%d calls %s outside an __EMSCRIPTEN__ "
                        "guard — that entry point is an abort() stub in the "
                        "browser port and kills the whole module\n",
                        path.c_str(), lineNo, fn);
            ++failures;
            ok = false;
        }
    }
    return ok;
}

// Open-Apple / Solid-Apple + key typed no key at all: GLFW sends no
// character for an Alt chord (bug hunt 2026-10-23). The chord's key comes
// from the layout name, with the //e's US shifted symbols.
void testAppleChordChar()
{
    using pom2::keychord::appleChordChar;
    check(appleChordChar("s", kKeyA + 18, false) == 's', "OA-s types s");
    check(appleChordChar("s", kKeyA + 18, true)  == 'S', "OA-S types S");
    check(appleChordChar("/", 47, true) == '?', "OA-? types ?");
    check(appleChordChar("1", 49, false) == '1', "OA-1 types 1");
    check(appleChordChar(nullptr, kKeyA + 16, false) == 'q',
          "no layout name (Emscripten): the US position");
    check(appleChordChar("\xD1\x8F", kKeyA, false) == 'a',
          "a Cyrillic cap falls back to the US letter");
    check(appleChordChar(nullptr, 256, false) == 0, "Escape and friends: nothing");
    check(appleChordChar(nullptr, 32, false) == ' ',
          "OA+Space types a space (Space has no layout name)");
}

// IIe Technical Reference table 2-3: Ctrl with the punctuation keys.
void testCtrlPunctuation()
{
    using pom2::keychord::ctrlPunctCode;
    check(ctrlPunctCode("[", 91) == 0x1B, "Ctrl+[ is ESC ($1B)");
    check(ctrlPunctCode("\\", 92) == 0x1C, "Ctrl+\\ is $1C");
    check(ctrlPunctCode("]", 93) == 0x1D, "Ctrl+] is $1D");
    check(ctrlPunctCode("6", 54) == 0x1E, "Ctrl+6 (^) is $1E");
    check(ctrlPunctCode("-", 45) == 0x1F, "Ctrl+- (_) is $1F");
    check(ctrlPunctCode("2", 50) == 0x00, "Ctrl+2 (@) is $00");
    check(ctrlPunctCode(nullptr, 93) == 0x1D, "no layout name: the US position");
    check(ctrlPunctCode(",", 77) == -1, "a cap with no control code: nothing");
    check(ctrlPunctCode("\xC3\xA0", 48) == -1, "AZERTY's à: nothing");
}

// Linux AltGr arrives as RIGHT_ALT with Mod5, so the keys typed under it
// carry no ALT bit; a Mod1 Alt always does.
void testLinuxAltGr()
{
    using pom2::keychord::rightAltIsLevel3;
    check(rightAltIsLevel3(kKeyA + 16, 0, /*rightAltHeld=*/true, /*windows=*/false),
          "Right Alt held + a key without ALT = Linux AltGr");
    check(!rightAltIsLevel3(kKeyA + 16, kModAlt, true, false),
          "a real Alt (ALT bit set) is still Solid-Apple");
    check(!rightAltIsLevel3(kKeyA + 16, 0, true, /*windows=*/true),
          "Windows has its own AltGr form");
    check(!rightAltIsLevel3(340, 0, true, false), "a modifier key proves nothing");
    check(!rightAltIsLevel3(kKeyA + 16, 0, false, false), "no Right Alt held");
}

void testNoEmscriptenAbortStubs()
{
    const std::string root = POM2_STR(POM2_SOURCE_DIR);
    bool allOk = true;
    for (const char* rel : kScannedFiles)
        allOk &= scanFile(root + "/" + rel);
    check(allOk,
          "no Emscripten abort()-stub GLFW call is reachable in the browser build");
}

}  // namespace

int main()
{
    check(routeHostKey(true, false, true), "release reaches the guest after UI focus changes");
    check(!routeHostKey(true, false, false), "UI text presses stay outside the guest");
    check(routeHostKey(true, true, false), "global shortcuts survive UI capture");
    testLayoutLetter();
    testAltGr();
    testCtrlLetter();
    testAltAppleKeys();
    testAppleChordChar();
    testCtrlPunctuation();
    testLinuxAltGr();
    testNoEmscriptenAbortStubs();

    if (failures) {
        std::printf("key_chord_policy: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("key_chord_policy OK\n");
    return 0;
}
