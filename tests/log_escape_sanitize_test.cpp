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

// Logger: an UNTRUSTED string may not reach the terminal raw (hunt #15).
//
// Every one of POM2's 448 log call sites builds its message by concatenation,
// and what is concatenated in is routinely not ours: a disk path read back out
// of `state.cfg`, a filename read off the host filesystem, the path half of a
// `tnfs://` URL. On POSIX a filename is any byte but '/' and NUL, so
//
//   "a.dsk\n[ERROR] ROM: apple2e.rom checksum FAILED"
//
// is a legal name — and `StorageCoordinator::restoreMediaFromSettings` puts a
// persisted path straight into "persisted path not found: " + path, which
// MainWindow_SlotConfig.cpp hands to pom2::log().warn(). Before the fix that
// one call produced TWO lines on stderr, the second of them a complete,
// convincing POM2 error about a subsystem that never spoke. The ESC forms are
// worse in a different way: "\x1b[2J" clears the user's screen and
// "\x1b]0;…\x07" rewrites the window title, from a file the user only listed.
//
// So: one log call, one line; no C0 control and no DEL on the wire; and every
// byte >= 0x80 preserved, because the messages are full of UTF-8 and mangling
// them would be its own bug.

#include "Logger.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

int main()
{
    // Never let a probe touch the real per-user config: nothing here reads it,
    // but the standing rule is the standing rule.
    const fs::path home = fs::temp_directory_path() / "pom2_log_escape_home";
    fs::remove_all(home);
    fs::create_directories(home / ".config");
    ::setenv("HOME", home.string().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (home / ".config").string().c_str(), 1);

    const fs::path capture = home / "stderr.txt";

    // The three shapes, all of them legal POSIX filenames.
    const std::string ansi   = "\x1b[2J\x1b[31mPWNED\x1b[0m.dsk";
    const std::string forged = "a.dsk\n[ERROR] ROM: apple2e.rom checksum FAILED";
    const std::string title  = "\x1b]0;owned\x07" "b.dsk";
    const std::string utf8   = "Chargé : café — ダメ";
    const std::string tabs   = "a\tb\rc\x7f\x01";
    // Spelled out rather than taken from Logger.h, so this file compiles
    // against the pre-fix header too and FAILS there at run time (a pin that
    // fails to build proves nothing about behaviour).
    constexpr std::size_t kCap = 8192;   // == pom2::kLogMaxMessageBytes
    const std::string huge(kCap + 4096, 'A');
    constexpr int kCalls = 6;

    std::fflush(stderr);
    FILE* redirected = std::freopen(capture.string().c_str(), "w", stderr);
    assert(redirected && "could not redirect stderr");
    pom2::log().error("CLI",     "--load cannot open " + ansi);
    pom2::log().warn ("Storage", "persisted path not found: " + forged);
    pom2::log().info ("Media",   "Write-protected: " + title);
    pom2::log().info ("ROM",     utf8);
    pom2::log().debug("Test",    tabs);
    pom2::log().warn ("Test",    huge);
    std::fflush(stderr);

    std::string text;
    {
        std::ifstream in(capture, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
    }

    // 1. One log call, one line. The forged-newline payload made this 7.
    std::size_t lines = 0;
    for (char c : text) if (c == '\n') ++lines;
    if (lines != kCalls) {
        std::printf("FAIL: %d log calls produced %zu lines\n", kCalls, lines);
        return 1;
    }

    // 2. Not one C0 control byte (bar the line terminators) and no DEL.
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') continue;
        if (c < 0x20 || c == 0x7F) {
            std::printf("FAIL: raw control byte 0x%02X at offset %zu\n",
                        static_cast<unsigned>(c), i);
            return 1;
        }
    }

    // 3. The escape is still legible as text — the user must be able to see
    //    what the name actually was.
    if (text.find("\\x1B[2J") == std::string::npos ||
        text.find("\\n[ERROR] ROM:") == std::string::npos ||
        text.find("\\x1B]0;owned\\x07") == std::string::npos ||
        text.find("a\\tb\\rc\\x7F\\x01") == std::string::npos) {
        std::printf("FAIL: escaped form missing from:\n%s\n", text.c_str());
        return 1;
    }

    // 4. UTF-8 survives byte for byte: the messages are full of it.
    if (text.find(utf8) == std::string::npos) {
        std::printf("FAIL: UTF-8 message was mangled\n");
        return 1;
    }

    // 5. An unbounded value (a 4 MB `state.cfg` string, a pathological name)
    //    is capped rather than scrolled past.
    if (text.size() > kCalls * (kCap + 256)) {
        std::printf("FAIL: message cap not applied (%zu bytes)\n", text.size());
        return 1;
    }
    if (text.find("(truncated)") == std::string::npos) {
        std::printf("FAIL: the over-long message was not marked truncated\n");
        return 1;
    }

    // Say so on stdout — stderr belongs to the capture.
    std::printf("log_escape_sanitize: %d calls, %zu lines, %zu bytes — clean\n",
                kCalls, lines, text.size());
    fs::remove_all(home);
    return 0;
}
