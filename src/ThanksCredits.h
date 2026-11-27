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
// "Apple, in the beginning" and the authors added on 2026-11-25 come from a
// sourced pass, each name read on the page that credits it: Steven Weyhrich's
// apple2history.org (chapters 2-9 and 13-16) for Apple's people; the header
// of MAME's apple2.cpp, AppleWin's History.txt, the OpenEmulator AUTHORS
// file, the WOZ 2 reference, the LICENSE of izapple2 and Casso, the apple2js
// README (CFFA 2 firmware), the FujiNet KansasFest 2022 slides,
// 6502disassembly.com (Super Serial Card). Two rest on Wikipedia alone: Bill
// Fernandez and Daniel Kottke. Names a source could not be found for were
// left out (the designers of the //c's case, of the IWM, of the mouse card,
// and the people behind several card makers, listed by company instead).
//
// French Touch's real names (Cyril Lambin = fenarinarsa, Arnaud Cocquiere =
// GROUiK) were given by the author on 2026-11-27, from his correspondence
// with them; "Cocquiere" loses its grave accent to the ASCII rule below.
// ASCII only: the window draws these lines centred, one by one.

#ifndef POM2_THANKS_CREDITS_H
#define POM2_THANKS_CREDITS_H

namespace pom2 {

/// One line of the credits. `kind`: 0 a name, 1 a section title, 2 a note
/// under a title (smaller, dim), 3 blank space. A name may carry what the
/// person did after a '|': "Name|role" — the window draws the role dimmer,
/// beside the name. A role is written only where a source gives one; a name
/// alone under a heading is credited for what the heading says.
struct CreditLine {
    int         kind;
    const char* text;
};

inline constexpr CreditLine kThanksCredits[] = {
    {1, "POM2"},
    {2, "an Apple II 8-bit family emulator"},
    {0, "by Arnaud Verhille"},
    {3, ""},
    {2, "exists because of the people below."},
    {2, "Thank you."},
    {3, ""}, {3, ""},

    {1, "Apple, in the beginning"},
    {2, "the people who made these machines"},
    {0, "Steve Wozniak|designed the Apple I and the Apple II"},
    {0, "Steve Jobs|co-founder"},
    {0, "Ronald Wayne|co-founder"},
    {0, "Mike Markkula|first investor and chairman"},
    {0, "Mike Scott|Apple's first president"},
    {0, "Bill Fernandez|first employee"},
    {0, "Daniel Kottke|early employee, board debugging"},
    {3, ""},
    {2, "the Apple II: hardware, case, logo"},
    {0, "Rod Holt|power supply"},
    {0, "Jerry Manock|case"},
    {0, "Rob Janoff|the rainbow logo"},
    {0, "Allen Baum|monitor ROM"},
    {0, "Burrell Smith|Language Card"},
    {3, ""},
    {2, "BASIC, DOS, ProDOS and the firmware"},
    {0, "Randy Wigginton|Applesoft, RWTS"},
    {0, "Bill Gates, Ric Weiland, Monte Davidoff|Microsoft BASIC"},
    {0, "Marc McDonald|the simulator Microsoft BASIC was ported with"},
    {0, "Paul Laughton|Apple DOS"},
    {0, "Cliff Huston|the Disk II drive, Applesoft"},
    {0, "Dick Huston|ProDOS"},
    {0, "Rick Auricchio|DOS, the //e monitor firmware"},
    {0, "Rich Williams|DOS 3.3 tools, the //c firmware"},
    {0, "John Arkley|Autostart ROM, //c Plus"},
    {0, "Bryan Stearns|the //e monitor firmware"},
    {0, "Bill Atkinson|Apple Pascal"},
    {0, "Gary Shannon|Programmer's Aid music routine"},
    {0, "Andy Hertzfeld|Silentype firmware"},
    {0, "Larry Kenyon|Super Serial Card firmware"},
    {0, "Bill Budge|MousePaint"},
    {0, "Natalie Shuttleworth|ProDOS"},
    {3, ""},
    {2, "the Disk II, the //e and the //c"},
    {0, "Wendell Sander|the Disk II controller timing"},
    {0, "Peter Quinn|//e and //c"},
    {0, "Walt Broedner|IOU and MMU"},
    {0, "Ernie Beernink|the //c firmware"},
    {0, "James Huston|the //c firmware"},
    {0, "Bruce Tognazzini|MouseText"},
    {3, ""},
    {2, "the manuals"},
    {0, "Jef Raskin|the first manuals"},
    {0, "Brian Howard|the first manuals"},
    {0, "Chris Espinosa|Apple II Reference Manual"},
    {3, ""},
    {2, "and everyone at Apple who designed, built"},
    {2, "and documented the Apple II family"},
    {3, ""}, {3, ""},

    {1, "Emulators whose code or behaviour POM2 follows"},
    {3, ""},
    {2, "MAME"},
    {0, "The MAME team|the hardware models POM2 ports"},
    {0, "R. Belmont|the MAME Apple II drivers"},
    {0, "Mike Balfour|the original MAME Apple II driver"},
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
    {0, "Nathan Woods|the original MAME Apple II driver"},
    {0, "Olivier Galibert"},
    {0, "Miodrag Milanovic"},
    {0, "benrg"},
    {0, "Antoine Vignau|Apple II Documentation Project, Brutal Deluxe"},
    {0, "Peter Ferrie|thanked by the MAME Apple II driver"},
    {3, ""},
    {2, "AppleWin"},
    {0, "Mike O'Brien|original author, 1994"},
    {0, "Oliver Schmidt|AppleWin 1.10"},
    {0, "William S. Simms"},
    {0, "Michael Pohoreski|developer, the debugger"},
    {0, "Tom Charlesworth|maintainer"},
    {0, "Andrea Odetti"},
    {0, "Nick Westgate|developer"},
    {3, ""},
    {2, "OpenEmulator"},
    {0, "Marc S. Ressl|lead developer"},
    {0, "Zellyn Hunter|apple2shader and notebook"},
    {3, ""},
    {2, "AccurApple, and its logic-analyser traces of a real PAL //e"},
    {0, "Stephane Champailler|AccurApple"},
    {3, ""},
    {2, "apple2js"},
    {0, "Will Scullin|apple2js"},
    {3, ""},
    {2, "web-a2e"},
    {0, "Mike Daley|web-a2e"},
    {0, "Shawn Bullock"},
    {3, ""},
    {2, "Virtual ]["},
    {0, "Gerard Putter|Virtual ]["},
    {3, ""},
    {2, "MicroM8"},
    {0, "Paleotronic|MicroM8"},
    {3, ""},
    {2, "Porte Parole and the MEA8000"},
    {0, "Sebastien Pourcel|PorteParole-MEA8000"},
    {3, ""},
    {2, "and"},
    {0, "greg-kennedy"},
    {0, "Christopher G. Mason"},
    {0, "Ivan Izaguirre|izapple2"},
    {0, "Robert Elmer|Casso"},
    {3, ""},
    {2, "FujiNet"},
    {0, "Thomas Cherryhomes|FujiNet, project lead"},
    {0, "Jeff Piepmeier|FujiNet, Apple II lead"},
    {0, "Joseph \"Mozzwald\" Honold|FujiNet"},
    {3, ""}, {3, ""},

    {1, "Test suites"},
    {0, "Tom Harte|SingleStepTests"},
    {0, "Klaus Dormann|6502 functional tests"},
    {0, "Frank Cringle|zexall"},
    {0, "Sean Young|Z80 documentation"},
    {3, ""}, {3, ""},

    {1, "Documentation, dumps and measurements"},
    {0, "Jim Sather|Understanding the Apple II / IIe"},
    {0, "Linards Ticmanis|video timing"},
    {0, "Steve Chamberlin / BMOW|the Liron ROM dump, Floppy Emu"},
    {0, "markadev"},
    {0, "Rich Dreher|CFFA 2.0 firmware"},
    {0, "Chris Schumann|CFFA 2 firmware"},
    {0, "Dave Lyons|CFFA 2 firmware"},
    {0, "Abdess / RetroBIOS|the firmware archive"},
    {0, "John Keoni Morris|Applesauce, the WOZ format"},
    {0, "John Brooks|thanked by the WOZ specification"},
    {0, "David Brown|thanked by the WOZ specification"},
    {0, "Bill Martens|thanked by the WOZ specification"},
    {0, "Sean McNamara|thanked by the WOZ specification"},
    {0, "Brutal Deluxe"},
    {0, "The Apple II Documentation Project"},
    {0, "Asimov"},
    {0, "The WOZ-a-day archive"},
    {0, "Henry at ReActiveMicro|hardware documentation"},
    {0, "Lukazi|4play, SNES MAX"},
    {0, "Kris Kennaway|ii-pix"},
    {0, "apple2history.org"},
    {0, "Nerdly Pleasures"},
    {0, "mrob"},
    {0, "6502.org"},
    {0, "6502workshop"},
    {3, ""},
    {2, "Le Chat Mauve"},
    {0, "Cyril \"fenarinarsa\" Lambin|French Touch"},
    {0, "Purplesoft"},
    {0, "retronik.silicium.org"},
    {0, "The system-cfg.com forum members"},
    {0, "apple-iigs.info"},
    {3, ""}, {3, ""},

    {1, "Software that keeps POM2 honest"},
    {2, "run on the real machines by their authors"},
    {0, "French Touch"},
    {0, "Arnaud \"GROUiK\" Cocquiere|French Touch"},
    {0, "Cyril \"fenarinarsa\" Lambin|French Touch"},
    {0, "Dubmood"},
    {0, "z00m"},
    {0, "rnR TAD"},
    {0, "S.V. Bulba"},
    {0, "Ivan Roshin"},
    {0, "Vince \"deater\" Weaver|demos"},
    {0, "4am|disk preservation"},
    {0, "Eric Rangell"},
    {0, "Henry Lowe|CardCat"},
    {0, "Roland Gustafsson|disk routines"},
    {3, ""}, {3, ""},

    {1, "Artwork, sounds and libraries"},
    {0, "Oscar Casamitjana|apple2e-emulator artwork"},
    {0, "Scott Lawrence|ImageWriter II recordings"},
    {0, "Omar Cornut|Dear ImGui"},
    {0, "juliettef|IconFontCppHeaders"},
    {0, "David Reid|miniaudio"},
    {0, "Sean Barrett|stb"},
    {0, "The GLFW project"},
    {0, "The libslirp project"},
    {0, "The Emscripten project"},
    {0, "The DejaVu fonts project"},
    {0, "The Font Awesome project"},
    {3, ""}, {3, ""},

    {1, "The cards, and the companies that made them"},
    {2, "POM2 plugs these into its slots"},
    {0, "Sweet Micro Systems|Mockingboard"},
    {0, "Applied Engineering|Phasor, RamWorks, TransWarp"},
    {0, "Videx|Videoterm, UltraTerm"},
    {0, "Microsoft|SoftCard, Premium SoftCard"},
    {0, "Orange Micro|Grappler, Grappler+"},
    {0, "Thunderware|ThunderClock+"},
    {0, "Street Electronics|Echo"},
    {0, "Le Chat Mauve|RVB Graph, Eve, Feline"},
    {0, "Ediciel|Porte Parole"},
    {0, "Passport Designs|MIDI Interface"},
    {0, "Saturn Systems|128K"},
    {0, "PCPI|Applicard"},
    {0, "ALS|The CP/M Card"},
    {0, "California Computer Systems|7710"},
    {0, "Zip Technology|Zip Chip"},
    {0, "a2heaven|FASTChip //e"},
    {0, "Blue Shift|VidHD"},
    {0, "R&D Automation|CFFA"},
    {0, "a2RetroSystems|Uthernet II"},
    {0, "Big Mess o' Wires|Floppy Emu, Yellowstone"},
    {0, "Lukazi|4play, SNES MAX"},
    {0, "profdc9|DAN ][ Controller"},
    {0, "Dallas Semiconductor|No-Slot Clock"},
    {3, ""}, {3, ""},

    {1, "POM1, where it started"},
    {0, "Uncle Bernie|the GEN2 colour graphics card"},
    {0, "Claudio Parmigiani|the P-LAB TMS9918 card and CodeTank"},
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
