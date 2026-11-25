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

// ThanksCredits — the people and projects POM2 stands on, as the end
// credits the Help ▸ Thank you window rolls.
//
// The names and their grouping are the author's own list, published with
// POM2 1.0 on Applefritter
// (applefritter.com/content/pom2-10-apple-ii-emulator-c-plus-ntsc-pal), plus
// the credits the repository carries since: THIRD-PARTY.md, the release
// notes and the [A]/[B]/[C]/[D] citations in the sources. A name is here
// because a file in this repository says why; add one the same way. Authors
// are named only where a source names them — a project listed alone is a
// project whose author no document here records.
//
// [E] "Apple, in the beginning" is the exception: no file of this repository
// backs those names. They are the people the published histories of Apple
// credit with the early machines — the founders, the Apple II's power
// supply and case, its monitor ROM, DOS 3.3, the IWM, the //e and the //c —
// written from general knowledge, not from a document checked here.
// Settled by: a pass against Apple's own histories (apple2history.org).
//
// ASCII only: the window draws these lines centred, one by one.

#ifndef POM2_THANKS_CREDITS_H
#define POM2_THANKS_CREDITS_H

namespace pom2 {

/// One line of the credits. `kind`: 0 a name, 1 a section title, 2 a note
/// under a title (smaller, dim), 3 blank space.
struct CreditLine {
    int         kind;
    const char* text;
};

inline constexpr CreditLine kThanksCredits[] = {
    {1, "POM2"},
    {2, "an Apple II family emulator"},
    {0, "by Arnaud Verhille"},
    {3, ""},
    {2, "exists because of the people below."},
    {2, "Thank you."},
    {3, ""}, {3, ""},

    {1, "Apple, in the beginning"},
    {2, "the people who made these machines"},
    {0, "Steve Wozniak"},
    {0, "Steve Jobs"},
    {0, "Ronald Wayne"},
    {0, "Mike Markkula"},
    {0, "Rod Holt"},
    {0, "Jerry Manock"},
    {0, "Allen Baum"},
    {0, "Randy Wigginton"},
    {0, "Chris Espinosa"},
    {0, "Bill Fernandez"},
    {0, "Daniel Kottke"},
    {0, "Paul Laughton"},
    {0, "Wendell Sander"},
    {0, "Peter Quinn"},
    {0, "Walt Broedner"},
    {3, ""},
    {2, "and everyone at Apple who designed, built"},
    {2, "and documented the Apple II family"},
    {3, ""}, {3, ""},

    {1, "Emulators whose code or behaviour POM2 follows"},
    {3, ""},
    {2, "MAME"},
    {0, "The MAME team"},
    {0, "R. Belmont"},
    {0, "Rhett Aultman"},
    {0, "Spiro Trikaliotis"},
    {0, "Vas Crabb"},
    {0, "Fabio Priuli"},
    {0, "Frank Palazzolo"},
    {0, "Aaron Giles"},
    {0, "Jonathan Gevaryahu"},
    {0, "Raphael Nabet"},
    {0, "Couriersud"},
    {0, "Michael Zapf"},
    {0, "Nathan Woods"},
    {0, "Olivier Galibert"},
    {0, "Miodrag Milanovic"},
    {0, "benrg"},
    {3, ""},
    {2, "AppleWin"},
    {0, "William S. Simms"},
    {0, "Michael Pohoreski"},
    {0, "Tom Charlesworth"},
    {0, "Andrea Odetti"},
    {0, "Nick Westgate"},
    {3, ""},
    {2, "OpenEmulator"},
    {0, "Marc S. Ressl"},
    {0, "Zellyn Hunter (apple2shader and notebook)"},
    {3, ""},
    {2, "AccurApple, and its logic-analyser traces of a real PAL //e"},
    {0, "Stephane Champailler"},
    {3, ""},
    {2, "apple2js"},
    {0, "Will Scullin"},
    {3, ""},
    {2, "web-a2e"},
    {0, "Mike Daley"},
    {0, "Shawn Bullock"},
    {3, ""},
    {2, "Virtual ]["},
    {0, "Gerard Putter"},
    {3, ""},
    {2, "MicroM8"},
    {0, "Paleotronic"},
    {3, ""},
    {2, "Porte Parole and the MEA8000"},
    {0, "Sebastien Pourcel"},
    {3, ""},
    {2, "and"},
    {0, "greg-kennedy"},
    {0, "Christopher G. Mason"},
    {0, "izapple2"},
    {0, "Casso"},
    {0, "The FujiNet project"},
    {0, "Thomas Cherryhomes"},
    {3, ""}, {3, ""},

    {1, "Test suites"},
    {0, "Tom Harte (SingleStepTests)"},
    {0, "Klaus Dormann"},
    {0, "Frank Cringle (zexall)"},
    {0, "Sean Young"},
    {3, ""}, {3, ""},

    {1, "Documentation, dumps and measurements"},
    {0, "Jim Sather (Understanding the Apple II / IIe)"},
    {0, "Linards Ticmanis (video timing)"},
    {0, "Steve Chamberlin / BMOW"},
    {0, "markadev"},
    {0, "Rich Dreher (CFFA 2.0 firmware)"},
    {0, "Abdess / RetroBIOS"},
    {0, "John Keoni Morris"},
    {0, "Applesauce"},
    {0, "Brutal Deluxe"},
    {0, "The Apple II Documentation Project"},
    {0, "Asimov"},
    {0, "The WOZ-a-day archive"},
    {0, "Henry at ReActiveMicro"},
    {0, "Lukazi"},
    {0, "Kris Kennaway"},
    {0, "apple2history.org"},
    {0, "Nerdly Pleasures"},
    {0, "mrob"},
    {0, "6502.org"},
    {0, "6502workshop"},
    {3, ""},
    {2, "Le Chat Mauve"},
    {0, "fenarinarsa"},
    {0, "Purplesoft"},
    {0, "retronik.silicium.org"},
    {0, "The system-cfg.com forum members"},
    {0, "apple-iigs.info"},
    {3, ""}, {3, ""},

    {1, "Software that keeps POM2 honest"},
    {2, "run on the real machines by their authors"},
    {0, "French Touch"},
    {0, "GROUiK"},
    {0, "fenarinarsa"},
    {0, "Dubmood"},
    {0, "z00m"},
    {0, "rnR TAD"},
    {0, "S.V. Bulba"},
    {0, "Ivan Roshin"},
    {0, "Vince \"deater\" Weaver"},
    {0, "4am"},
    {0, "Eric Rangell"},
    {0, "Henry Lowe (CardCat)"},
    {0, "Roland Gustafsson"},
    {3, ""}, {3, ""},

    {1, "Artwork, sounds and libraries"},
    {0, "Oscar Casamitjana"},
    {0, "sscanf (apple2e-emulator artwork)"},
    {0, "Scott Lawrence (ImageWriter II recordings)"},
    {0, "Omar Cornut (Dear ImGui)"},
    {0, "juliettef (IconFontCppHeaders)"},
    {0, "David Reid (miniaudio)"},
    {0, "Sean Barrett (stb)"},
    {0, "The GLFW project"},
    {0, "The libslirp project"},
    {0, "The Emscripten project"},
    {0, "The DejaVu fonts project"},
    {0, "The Font Awesome project"},
    {3, ""}, {3, ""},

    {1, "POM1, where it started"},
    {0, "Uncle Bernie"},
    {0, "Claudio Parmigiani"},
    {3, ""}, {3, ""},

    {1, "And you"},
    {2, "who still switch these machines on."},
    {3, ""}, {3, ""}, {3, ""},
    {2, "Every port cites its source where it is used."},
    {2, "The full notices are in THIRD-PARTY.md."},
};

inline constexpr int kThanksCreditCount =
    static_cast<int>(sizeof(kThanksCredits) / sizeof(kThanksCredits[0]));

} // namespace pom2

#endif // POM2_THANKS_CREDITS_H
