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

// KeyChord — the keyboard POLICY, held apart from GLFW so it can be tested.
//
// Same shape, and the same reason, as `MouseGrab.h`: what belongs here is
// every decision that is arguable — which modifier means what, on which
// layout, on which platform — while the plumbing (calling glfwGetKeyName,
// reading the callback's mods) stays at the call site. Three defects that
// were only reachable on a keyboard nobody in CI owns lived in that
// plumbing, so the policy is now something a test can drive directly:
//
//   * `letterFromKeyName` — GLFW key codes are POSITIONS on a US QWERTY
//     board. `glfwGetKeyName` gives the layout's character, and the old
//     fallback logic ran whenever that character was not a letter, which on
//     AZERTY turned Ctrl+',' (the key at QWERTY's M position) into Ctrl-M,
//     i.e. a RETURN the user never typed. The positional fallback belongs to
//     the case where GLFW has NO name for the key, not to the case where it
//     has one and it is not a letter.
//
//   * `isAltGr` — on Windows, AltGr arrives as CONTROL|ALT. Every accented
//     and bracketed character a German, French or Polish layout puts behind
//     AltGr therefore looked like a Ctrl-letter (AltGr+E → $05) and, worse,
//     AltGr+F and AltGr+G looked like POM2's own kiosk and mouse-grab
//     chords — fired from inside an ImGui text field. GLFW does not say
//     WHICH Alt is down in `mods`, so the right-Alt state is tracked from
//     the key events and passed in.
//
//   * `altDrivesAppleKeys` — Left/Right Alt are Open/Solid Apple, which the
//     firmware reads as the PB0/PB1 fire buttons. On a macOS French layout
//     Option is how you type { } [ ] |, so writing those characters pressed
//     fire. The `keyboard_alt_apple_keys` setting (default on, because the
//     binding is right for most users) is the way out.
//
// The GLFW token values are mirrored as literals so this header stays
// GLFW-free; MainWindow_Input.cpp static_asserts every one of them against
// <GLFW/glfw3.h>, so an upstream renumbering is a compile error rather than
// a chord that silently stops working.

#ifndef POM2_KEY_CHORD_H
#define POM2_KEY_CHORD_H

namespace pom2 {
namespace keychord {

// ── GLFW token mirrors (static_asserted in MainWindow_Input.cpp) ────────
inline constexpr int kKeyA        = 65;      // GLFW_KEY_A
inline constexpr int kKeyZ        = 90;      // GLFW_KEY_Z
inline constexpr int kKeyF        = 70;      // GLFW_KEY_F
inline constexpr int kKeyP        = 80;      // GLFW_KEY_P
inline constexpr int kKeyLeftAlt  = 342;     // GLFW_KEY_LEFT_ALT
inline constexpr int kKeyRightAlt = 346;     // GLFW_KEY_RIGHT_ALT
inline constexpr int kModShift    = 0x0001;  // GLFW_MOD_SHIFT
inline constexpr int kModControl  = 0x0002;  // GLFW_MOD_CONTROL
inline constexpr int kModAlt      = 0x0004;  // GLFW_MOD_ALT

/// True on the build that has Windows' AltGr semantics. A compile-time
/// constant rather than an `#ifdef` inside each policy call, so a test can
/// drive BOTH platforms' behaviour on one host.
#ifdef _WIN32
inline constexpr bool kHostIsWindows = true;
#else
inline constexpr bool kHostIsWindows = false;
#endif

/// The letter the user's KEYBOARD LAYOUT prints on this key, uppercased, or
/// 0 when the key carries no single ASCII letter.
///
/// `name` is what `glfwGetKeyName(key, scancode)` returned (null under
/// Emscripten, where that entry point is an abort() stub and must not be
/// called at all — see MainWindow_Input.cpp). `key` is the GLFW key code,
/// used only for the US-positional fallback.
///
/// The fallback fires ONLY when GLFW has no name for the key. A key that
/// HAS a name which is not a letter (AZERTY's ',' at QWERTY's M position,
/// or any punctuation) yields 0: pretending it is the US letter printed at
/// that position is how Ctrl+',' became a RETURN.
inline char letterFromKeyName(const char* name, int key)
{
    if (name && name[0]) {
        if (name[1] == '\0') {
            const char c = name[0];
            if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
            if (c >= 'A' && c <= 'Z') return c;
        }
        // A name exists and is not a single ASCII letter — a punctuation
        // key, or a multi-byte cap on a non-Latin layout. Not a Ctrl-letter.
        return 0;
    }
    if (key >= kKeyA && key <= kKeyZ)
        return static_cast<char>('A' + (key - kKeyA));
    return 0;
}

/// Windows AltGr: physically the RIGHT Alt, reported by Win32 (and so by
/// GLFW) as CONTROL|ALT together. `rightAltHeld` comes from the caller's own
/// tracking of the GLFW_KEY_RIGHT_ALT press/release edges, because `mods`
/// folds both Alts into one bit.
///
/// Everything that keys off Ctrl or off an Alt chord must exclude this, or
/// ordinary text entry on a German/French/Polish layout fires POM2's
/// shortcuts and types control codes at the guest.
inline bool isAltGr(int mods, bool rightAltHeld, bool hostIsWindows)
{
    return hostIsWindows && rightAltHeld &&
           (mods & kModControl) != 0 && (mods & kModAlt) != 0;
}

/// Is this modifier state a genuine "Ctrl held" for the Ctrl-A..Ctrl-Z
/// injection path?
///
/// Alt disqualifies it on every platform, not only Windows: every Ctrl+Alt
/// chord POM2 owns (Ctrl+Alt+F kiosk, Ctrl+Alt+G grab) is matched earlier
/// and returns, so nothing downstream ever wants a Ctrl-letter with Alt
/// down — while on Windows that combination IS AltGr and must not type
/// $01..$1A at the guest.
inline bool isCtrlLetter(int mods)
{
    return (mods & kModControl) != 0 && (mods & kModAlt) == 0;
}

/// May a Ctrl+Alt chord (kiosk toggle, mouse grab) fire for this event?
/// No when the Alt half is Windows' AltGr — the user is typing a character.
inline bool chordMayFire(int mods, bool rightAltHeld, bool hostIsWindows)
{
    return !isAltGr(mods, rightAltHeld, hostIsWindows);
}

/// May Left/Right Alt drive Open-Apple / Solid-Apple ($C061/$C062 bit 7,
/// which the firmware reads as the PB0/PB1 fire buttons)?
///
/// `settingEnabled` is the `keyboard_alt_apple_keys` setting (default true).
/// Turned off, the Apple keys come only from the on-screen //e keyboard —
/// the escape hatch for a macOS French layout, where Option is how { } [ ]
/// and | are typed and every one of them pressed fire.
///
/// Even with the setting on, a Windows AltGr press is refused: it is a
/// text-entry modifier there, not the Solid-Apple key.
inline bool altDrivesAppleKeys(int mods, bool settingEnabled, bool hostIsWindows)
{
    if (!settingEnabled) return false;
    if (hostIsWindows && (mods & kModControl) != 0) return false;
    return true;
}

}  // namespace keychord
}  // namespace pom2

#endif  // POM2_KEY_CHORD_H
