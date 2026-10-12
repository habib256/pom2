# Generates src/AppleIIeKeyboardLayout.h from measurements taken directly off
# pic/Keyboard_AppleIIe.jpeg (see the header's provenance comment).
W, H = 2578.0, 908.0
ROWS = {1:(57,173), 2:(222,336), 3:(385,500), 4:(547,661), 5:(713,829)}

# (row, x_start, width, id, label, kind, base, shift)
# kind: C=char  S=special  M=modifier
K = [
 (1, 218,134,"esc","Esc","S","Esc",""),
 (1, 368,131,"1","1 !","C","1","!"),
 (1, 514,133,"2","2 @","C","2","@"),
 (1, 663,131,"3","3 #","C","3","#"),
 (1, 810,130,"4","4 $","C","4","$"),
 (1, 955,132,"5","5 %","C","5","%"),
 (1,1102,131,"6","6 ^","C","6","^"),
 (1,1250,129,"7","7 &","C","7","&"),
 (1,1397,129,"8","8 *","C","8","*"),
 (1,1541,130,"9","9 (","C","9","("),
 (1,1687,131,"0","0 )","C","0",")"),
 (1,1832,132,"minus","- _","C","-","_"),
 (1,1980,131,"equal","= +","C","=","+"),
 (1,2124,221,"del","Delete","S","Del",""),

 (2, 216,202,"tab","Tab","S","Tab",""),
 (2, 445,121,"q","Q","C","q","Q"),
 (2, 591,123,"w","W","C","w","W"),
 (2, 740,122,"e","E","C","e","E"),
 (2, 888,120,"r","R","C","r","R"),
 (2,1034,122,"t","T","C","t","T"),
 (2,1182,121,"y","Y","C","y","Y"),
 (2,1330,120,"u","U","C","u","U"),
 (2,1477,120,"i","I","C","i","I"),
 (2,1623,121,"o","O","C","o","O"),
 (2,1772,120,"p","P","C","p","P"),
 (2,1917,123,"lbracket","[ {","C","[","{"),
 (2,2065,123,"rbracket","] }","C","]","}"),
 (2,2213,135,"return","Return","S","Return",""),   # upper arm of the L

 (3, 214,243,"ctrl","Control","M","Ctrl",""),
 (3, 482,123,"a","A","C","a","A"),
 (3, 628,125,"s","S","C","s","S"),
 (3, 774,128,"d","D","C","d","D"),
 (3, 922,125,"f","F","C","f","F"),
 (3,1071,125,"g","G","C","g","G"),
 (3,1218,125,"h","H","C","h","H"),
 (3,1367,124,"j","J","C","j","J"),
 (3,1513,126,"k","K","C","k","K"),
 (3,1660,126,"l","L","C","l","L"),
 (3,1807,127,"semicolon","; :","C",";",":"),
 (3,1958,124,"quote","' \\\"","C","'","\\\""),
 (3,2103,126,"backquote","` ~","C","`","~"),
 (3,2254, 97,"return","Return","S","Return",""),   # lower leg of the L

 (4, 212,169,"lshift","Shift","M","Shift",""),
 (4, 404,125,"backslash","\\\\ |","C","\\\\","|"),
 (4, 553,124,"z","Z","C","z","Z"),
 (4, 702,123,"x","X","C","x","X"),
 (4, 851,123,"c","C","C","c","C"),
 (4, 998,125,"v","V","C","v","V"),
 (4,1147,122,"b","B","C","b","B"),
 (4,1296,122,"n","N","C","n","N"),
 (4,1445,121,"m","M","C","m","M"),
 (4,1593,121,"comma",", <","C",",","<"),
 (4,1740,122,"period",". >","C",".",">"),
 (4,1886,124,"slash","/ ?","C","/","?"),
 (4,2036,316,"rshift","Shift","M","Shift",""),

 (5, 212,276,"capslock","Caps Lock","M","Caps",""),
 (5, 660,122,"openapple","Open-Apple","M","OA",""),
 (5, 809,798,"space","Space","C"," "," "),
 (5,1637,120,"solidapple","Solid-Apple","M","SA",""),
 (5,1786,121,"left","Left arrow","S","Left",""),
 (5,1935,121,"right","Right arrow","S","Right",""),
 (5,2082,121,"down","Down arrow","S","Down",""),
 (5,2232,123,"up","Up arrow","S","Up",""),
]

# Reset lives in its own recess to the right of the well, mounted LOWER than
# the row-1 caps, so it cannot inherit that row's y band — measured separately
# off the photo as an absolute rect.
EXTRA = [("reset", "Reset", "S", "Reset", "", 2394, 74, 2504, 190)]

out = []
for row, x, w, kid, label, kind, base, shift in K:
    y0, y1 = ROWS[row]
    out.append((kid, label, kind, base, shift,
                x / W, y0 / H, (x + w) / W, y1 / H))
for kid, label, kind, base, shift, x0, y0, x1, y1 in EXTRA:
    out.append((kid, label, kind, base, shift, x0 / W, y0 / H, x1 / W, y1 / H))

with open("src/AppleIIeKeyboardLayout.h", "w") as f:
    f.write('''// VERHILLE Arnaud 2026
//
// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleIIeKeyboardLayout — where every key sits on `pic/Keyboard_AppleIIe.jpeg`
// and what it sends.
//
// GENERATED, and worth saying how, because the numbers are not guesses:
// `tools/gen_keyboard_layout.py` reads the photo, takes a 75th-percentile
// column profile through the middle of each key row (75th, not the median:
// the two-line French/US legends put enough dark glyph pixels in the middle
// of a cap to split it in two under a median) and cuts at the dark valleys
// between caps. That yields 5 row bands and the cap runs inside each, which
// this file stores as fractions of the 2578x908 image — so the hotspots
// track the picture at any window size, and re-cropping the photo means
// re-running the script rather than nudging constants by hand.
//
// The photo is a EUROPEAN (French) //e keyboard: every alphanumeric cap
// carries two legends, French on top and US below ("A Q" is French A / US Q).
// POM2 maps the US legend, which is what the emulated ROM's keyboard decoder
// expects. Two consequences worth knowing before reading the table:
//   * the extra ISO key at the left of row 4 (legends "> |" / "< \\\\") is the
//     US backslash key;
//   * the horizontal arrow caps are left then right. The right-arrow legend
//     was corrected in October 2026; the measured rectangles did not change.
//
// The empty recess between Caps Lock and Open-Apple is left out on purpose —
// it is a bare well on the real machine, with no cap and no switch.

#ifndef POM2_APPLE_IIE_KEYBOARD_LAYOUT_H
#define POM2_APPLE_IIE_KEYBOARD_LAYOUT_H

#include <vector>

namespace pom2 {

/// What a hotspot does when clicked.
enum class KeyKind {
    Char,      ///< Injects `base`, or `shift` when Shift is latched.
    Special,   ///< A named action (Return, arrows, Reset...).
    Modifier   ///< A latch (Shift / Control / Caps / Open- and Solid-Apple).
};

/// Named actions and latches. `Char` keys use `base`/`shift` instead.
enum class KeyAction {
    None, Esc, Tab, Return, Delete, Left, Right, Up, Down, Reset,
    Shift, Control, CapsLock, OpenApple, SolidApple
};

/// One clickable cap. Rect is in IMAGE FRACTIONS (0..1), x0/y0 top-left.
/// Two entries may share an `id` — the Return key is L-shaped and is stored
/// as its two arms, which is also how it highlights as one key.
struct KeyHotspot {
    const char* id;
    const char* label;     ///< Tooltip / status text.
    KeyKind     kind;
    KeyAction   action;    ///< Meaningful for Special and Modifier.
    char        base;      ///< Char keys: unshifted ASCII.
    char        shift;     ///< Char keys: shifted ASCII.
    float       x0, y0, x1, y1;
};

/// The layout. Row order, left to right.
const std::vector<KeyHotspot>& appleIIeKeyboard();

} // namespace pom2

#endif // POM2_APPLE_IIE_KEYBOARD_LAYOUT_H
''')

action_of = {
 "Esc":"Esc","Tab":"Tab","Return":"Return","Del":"Delete","Left":"Left",
 "Right":"Right","Up":"Up","Down":"Down","Reset":"Reset","Shift":"Shift",
 "Ctrl":"Control","Caps":"CapsLock","OA":"OpenApple","SA":"SolidApple",
}
with open("src/AppleIIeKeyboardLayout.cpp", "w") as f:
    f.write('''// VERHILLE Arnaud 2026
//
// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleIIeKeyboardLayout — the generated table. See the header for how the
// coordinates were measured; regenerate with tools/gen_keyboard_layout.py
// rather than editing the numbers here.

#include "AppleIIeKeyboardLayout.h"

namespace pom2 {

const std::vector<KeyHotspot>& appleIIeKeyboard()
{
    static const std::vector<KeyHotspot> kKeys = {
''')
    for kid, label, kind, base, shift, x0, y0, x1, y1 in out:
        if kind == "C":
            k = "KeyKind::Char"; act = "KeyAction::None"
            # A C++ char literal needs the single quote and the backslash
            # escaped; everything else on this keyboard is printable ASCII.
            def lit(ch):
                if ch == "'":  return "'\\''"
                if ch == "\\": return "'\\\\'"
                return "'%s'" % ch
            b = lit(base); s = lit(shift)
        else:
            k = "KeyKind::Special" if kind == "S" else "KeyKind::Modifier"
            act = "KeyAction::" + action_of[base]
            b = "0"; s = "0"
        f.write('        { "%s", "%s", %s, %s, %s, %s,\n'
                '          %.5ff, %.5ff, %.5ff, %.5ff },\n'
                % (kid, label, k, act, b, s, x0, y0, x1, y1))
    f.write('''    };
    return kKeys;
}

} // namespace pom2
''')
print("wrote", len(out), "hotspots")
