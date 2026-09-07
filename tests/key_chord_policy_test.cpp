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
    // A multi-byte cap (non-Latin layout) is not a Ctrl-letter either.
    check(letterFromKeyName("\xD0\xB9", kKeyA) == 0,
          "a multi-byte key name yields no Ctrl-letter");

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
    check(isCtrlLetter(kModControl), "plain Ctrl injects a Ctrl-letter");
    check(isCtrlLetter(kModControl | kModShift), "Ctrl+Shift still does");
    check(!isCtrlLetter(kModControl | kModAlt),
          "Ctrl+Alt does NOT — on Windows that is AltGr (AltGr+E typed $05)");
    check(!isCtrlLetter(kModAlt), "Alt alone is not Ctrl");
    check(!isCtrlLetter(0), "no modifier is not Ctrl");
}

void testAltAppleKeys()
{
    check(altDrivesAppleKeys(0, /*enabled=*/true, /*windows=*/false),
          "Alt drives Open/Solid Apple by default");
    check(!altDrivesAppleKeys(0, /*enabled=*/false, false),
          "keyboard_alt_apple_keys=false takes the wire away "
          "(macOS French Option types { } [ ] | and was pressing fire)");
    check(!altDrivesAppleKeys(kModControl | kModAlt, true, /*windows=*/true),
          "Windows AltGr never presses Solid-Apple");
    check(altDrivesAppleKeys(kModControl | kModAlt, true, /*windows=*/false),
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
    testLayoutLetter();
    testAltGr();
    testCtrlLetter();
    testAltAppleKeys();
    testNoEmscriptenAbortStubs();

    if (failures) {
        std::printf("key_chord_policy: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("key_chord_policy OK\n");
    return 0;
}
