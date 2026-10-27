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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>   // EM_ASM_INT: hostIsWindows()
#endif

namespace pom2 {
namespace keychord {

inline bool routeHostKey(bool uiCaptures, bool globalShortcut, bool release)
{
    // A key can be pressed over the guest, then released over a text field.
    // Losing that edge leaves the IIe any-key-down wire asserted forever.
    return release || !uiCaptures || globalShortcut;
}

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
inline constexpr int kKey2        = 50;      // GLFW_KEY_2
inline constexpr int kKey6        = 54;      // GLFW_KEY_6
inline constexpr int kKeyMinus    = 45;      // GLFW_KEY_MINUS
inline constexpr int kKeyLBracket = 91;      // GLFW_KEY_LEFT_BRACKET
inline constexpr int kKeyBackslash= 92;      // GLFW_KEY_BACKSLASH
inline constexpr int kKeyRBracket = 93;      // GLFW_KEY_RIGHT_BRACKET
inline constexpr int kKeyFirstModifier = 340; // GLFW_KEY_LEFT_SHIFT
inline constexpr int kKeyLastModifier  = 348; // GLFW_KEY_MENU

/// True on the build that has Windows' AltGr semantics. A compile-time
/// constant rather than an `#ifdef` inside each policy call, so a test can
/// drive BOTH platforms' behaviour on one host.
#ifdef _WIN32
inline constexpr bool kHostIsWindows = true;
#else
inline constexpr bool kHostIsWindows = false;
#endif

/// The host's AltGr semantics at run time. The browser build is compiled
/// once for every host, and a browser on Windows delivers AltGr the way
/// Windows does — a synthesised Ctrl plus the right Alt, both flags set on
/// every key typed under it — so there the Windows rule (isAltGr) applies
/// and the Linux one (rightAltIsLevel3) must not. Asked once.
#ifdef __EMSCRIPTEN__
inline bool hostIsWindows()
{
    static const bool windows = EM_ASM_INT({
        const p = (navigator.userAgentData && navigator.userAgentData.platform) ||
                  navigator.platform || navigator.userAgent || "";
        return /win/i.test(p) ? 1 : 0;
    }) != 0;
    return windows;
}
#else
inline constexpr bool hostIsWindows() { return kHostIsWindows; }
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
            // A single ASCII character that is not a letter is a punctuation
            // cap — AZERTY's ',' sits where QWERTY has M, and answering 'M'
            // there typed a RETURN nobody pressed.
            return 0;
        }
        // Multi-byte: a non-Latin cap (Cyrillic, Greek, Hebrew). The layout
        // prints no ASCII letter here, but Ctrl-C still has to break Applesoft
        // (and Ctrl+Shift+P still has to open the palette), so fall through
        // to the US position — which is what the key is physically engraved
        // with underneath. "No ASCII letter" is not "no name".
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
/// Only Windows' AltGr disqualifies it: there Ctrl+Alt IS a text-entry
/// modifier, and AltGr+E on a QWERTZ layout typed $05 instead of "€". Any
/// other Alt is Open-/Solid-Apple, and a //e program reading OA+Ctrl+key
/// wants the control code: refusing every Alt typed a plain `s` for
/// OA+Ctrl+S, the game's sound toggle pressed while holding fire (bug hunt
/// 2026-10-27). POM2's own Ctrl+Alt chords are matched earlier and return.
inline bool isCtrlLetter(int mods, bool rightAltHeld, bool hostIsWindows)
{
    return (mods & kModControl) != 0 && !isAltGr(mods, rightAltHeld, hostIsWindows);
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
inline bool altDrivesAppleKeys(int mods, bool settingEnabled, bool hostIsWindows,
                               bool isRightAlt)
{
    if (!settingEnabled) return false;
    // AltGr is the RIGHT Alt, reported as CONTROL|ALT. The LEFT Alt with Ctrl
    // held is not AltGr on any layout, and refusing it there took Open-Apple
    // away from every Ctrl chord the firmware reads at $C061 — Open-Apple +
    // Ctrl + Reset is the //e cold boot.
    if (hostIsWindows && isRightAlt && (mods & kModControl) != 0) return false;
    return true;
}

/// The character an Open-Apple / Solid-Apple chord types: the key's own,
/// as the user's layout prints it, or 0 for a key that types nothing.
///
/// Needed because GLFW sends NO character for Alt+key — X11 and Win32 call
/// the char callback only for "plain" input (Win32's Alt+letter is a
/// WM_SYSCHAR), and macOS composes Option+S into U+00DF, which the ASCII
/// filter drops. So OA-S, OA-Q, OA-? — every AppleWorks and ProDOS-utility
/// command — set $C061 and latched no key at all (bug hunt 2026-10-23).
///
/// `name` / `key` as for letterFromKeyName. Letters follow Shift; digits and
/// US punctuation take their US shifted symbol (the Apple //e's own keycaps).
/// A multi-byte name falls back to the US position for letters and digits.
inline char appleChordChar(const char* name, int key, bool shift)
{
    char base = 0;
    if (name && name[0] && name[1] == '\0' &&
        static_cast<unsigned char>(name[0]) >= 0x21 &&
        static_cast<unsigned char>(name[0]) < 0x7F) {
        base = name[0];
    } else if (!name || !name[0] || name[1] != '\0') {
        if (key >= kKeyA && key <= kKeyZ) base = static_cast<char>('a' + (key - kKeyA));
        else if (key >= 48 && key <= 57) base = static_cast<char>(key);   // GLFW_KEY_0..9
    }
    // Space has no layout name (glfwGetKeyName returns NULL for it), so
    // OA+Space — fire and jump in one — latched no key at all.
    if (!base && key == 32) return ' ';   // GLFW_KEY_SPACE
    if (!base) return 0;
    if (base >= 'A' && base <= 'Z') base = static_cast<char>(base - 'A' + 'a');
    if (base >= 'a' && base <= 'z')
        return shift ? static_cast<char>(base - 'a' + 'A') : base;
    if (!shift) return base;
    static const char kUnshifted[] = "1234567890-=[]\\;',./`";
    static const char kShifted[]   = "!@#$%^&*()_+{}|:\"<>?~";
    for (int i = 0; kUnshifted[i]; ++i)
        if (kUnshifted[i] == base) return kShifted[i];
    return base;
}

/// The control code a Ctrl+punctuation key makes on the //e keyboard
/// encoder, or -1. Apple IIe Technical Reference, Table 2-3 ("Keys and ASCII
/// codes"): Ctrl with `2 @` → $00, `6 ^` → $1E, `- _` → $1F, `[` → $1B,
/// `\` → $1C, `]` → $1D — with or without Shift; every other key is
/// unchanged by Ctrl. Only Ctrl-A..Z used to reach the guest. `name` /
/// `key` as for letterFromKeyName (the layout's cap, else the US position).
inline int ctrlPunctCode(const char* name, int key)
{
    if (name && name[0] && name[1] == '\0') {
        switch (name[0]) {
        case '2': case '@': return 0x00;
        case '6': case '^': return 0x1E;
        case '-': case '_': return 0x1F;
        case '[': return 0x1B;
        case '\\': return 0x1C;
        case ']': return 0x1D;
        default:  return -1;
        }
    }
    if (name && name[0]) return -1;              // a cap with no such symbol
    switch (key) {
    case kKey2: return 0x00;      case kKey6: return 0x1E;
    case kKeyMinus: return 0x1F;  case kKeyLBracket: return 0x1B;
    case kKeyBackslash: return 0x1C; case kKeyRBracket: return 0x1D;
    default: return -1;
    }
}

/// Linux AltGr. X11 and Wayland report ISO_Level3_Shift as the RIGHT Alt
/// key (GLFW_KEY_RIGHT_ALT) but as Mod5, not Mod1, so the keys typed while it
/// is held carry no ALT bit — a real (Mod1) Alt always sets it, and GLFW then
/// sends no character at all. A non-modifier key seen with Right Alt held
/// and no ALT bit therefore means the Right Alt is AltGr: it must stop being
/// Solid-Apple, and the composed character must reach the guest. (It used to
/// press PB1 — fire — and type the key's base character: German AltGr+Q gave
/// `q`, not `@`.) Windows has its own CONTROL|ALT form (isAltGr).
inline bool rightAltIsLevel3(int key, int mods, bool rightAltHeld, bool hostIsWindows)
{
    return !hostIsWindows && rightAltHeld && (mods & kModAlt) == 0 &&
           !(key >= kKeyFirstModifier && key <= kKeyLastModifier);
}

}  // namespace keychord
}  // namespace pom2

#endif  // POM2_KEY_CHORD_H
