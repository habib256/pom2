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

// Settings round-trip regression test (round 10 #4/#5).
//
// The line-oriented key=value store must round-trip ARBITRARY string values:
//   #4 a value containing '#' was truncated on reload (load stripped after
//      the first '#' anywhere) — silently breaking disk paths like
//      "/home/u/My#Disks/game.dsk".
//   #5 a value containing a newline split into two lines, the second dropped.
// Both are fixed: '#' is a comment only at line start, and values are
// escaped (\\, \n, \r) on save / unescaped on load.
//
// Extended 2026-07-30 with the TYPED accessors, which this file previously only
// spot-checked as raw strings — and floats did not in fact round-trip:
// `ostringstream` defaults to 6 significant digits, so 1.0f/3.0f wrote as
// "0.333333" and read back as a DIFFERENT float. Every persisted float (all
// five volumes, ui_scale, and the ~15 NTSC/CRT + voxel shader parameters) thus
// shifted on the first save/load cycle. setFloat now emits the shortest width
// that round-trips, capped at max_digits10.
//
// Also pins BOUNDARY whitespace: load() trims each line (to drop a CRLF '\r'
// artifact), so a value with leading/trailing space or tab only survives
// because escapeValue encodes those positions specially.
//
// Drives save()→load() through a real file by pointing HOME at a temp dir.

#include "Settings.h"
#include "ResourcePaths.h"
#include "SettingsList.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / "pom2_settings_rt_home";
    fs::remove_all(home);
    fs::create_directories(home);
    ::setenv("HOME", home.string().c_str(), 1);
    // HOME alone does not pin the config directory on Linux/BSD:
    // userConfigDir() prefers XDG_CONFIG_HOME whenever it is set, and a
    // GitHub runner sets it. Left alone, this test resolved to the machine's
    // REAL ~/.config/POM2 — which it then truncates with a 4 MB junk file to
    // exercise the oversize guard — and the "does not cache" check below
    // compared a sandbox path against a machine path and aborted. Both
    // variables point into the sandbox for the whole run.
#if !defined(_WIN32) && !defined(__APPLE__)
    ::setenv("XDG_CONFIG_HOME", (home / "config").string().c_str(), 1);
#endif

    const std::string kHash   = "/home/u/My#Disks/game.dsk";   // '#' mid-value
    const std::string kLeadHash = "#literal-hash-start";        // '#' at value start
    const std::string kNewline = "line1\nline2\nline3";         // embedded newlines
    const std::string kBack    = "weird\\path\\name";           // backslashes
    const std::string kPlain   = "PlainValue123";

    {
        pom2::Settings s;
        s.setString("disk_path", kHash);
        s.setString("lead",      kLeadHash);
        s.setString("note",      kNewline);
        s.setString("back",      kBack);
        s.setString("plain",     kPlain);
        s.setInt   ("num",       4242);
        s.setBool  ("flag",      true);
        assert(s.save());
    }

    {
        pom2::Settings s;
        assert(s.load());
        assert(s.getString("disk_path") == kHash   && "'#' mid-value must survive");
        assert(s.getString("lead")      == kLeadHash&& "'#'-leading value must survive");
        assert(s.getString("note")      == kNewline && "embedded newlines must survive");
        assert(s.getString("back")      == kBack    && "backslashes must survive");
        assert(s.getString("plain")     == kPlain);
        // Sanity: the ints/bools still parse.
        assert(s.getString("num")  == "4242");
        assert(s.getString("flag") == "true" || s.getString("flag") == "1");
    }

    // ── Boundary whitespace: load() trims, so only escaping saves these ──
    {
        const std::vector<std::string> ws = {
            " ", "  ", "\t", " \t ", " lead", "trail ", " both ", "  two  ",
            "\tleadtab", "trailtab\t",
        };
        pom2::Settings w;
        for (size_t i = 0; i < ws.size(); ++i) w.setString("w" + std::to_string(i), ws[i]);
        assert(w.save());
        pom2::Settings r;
        assert(r.load());
        for (size_t i = 0; i < ws.size(); ++i) {
            const std::string k = "w" + std::to_string(i);
            if (r.getString(k, "<<missing>>") != ws[i]) {
                std::printf("FAIL: boundary-whitespace value %s did not round-trip "
                            "(want %zu bytes, got %zu)\n", k.c_str(), ws[i].size(),
                            r.getString(k, "").size());
                return 1;
            }
        }
    }

    // ── Typed accessors, floats especially ──────────────────────────────
    {
        const std::vector<int> ints = { 0, 1, -1, 127, -128, 32767, -32768,
                                        2147483647, -2147483647 };
        // 1/3 and 2/3 are the cases the 6-digit default got wrong; the rest
        // cover the exponent forms and everyday slider values.
        const std::vector<float> floats = { 0.0f, 1.0f, -1.0f, 0.5f, 0.75f,
                                            1.0f / 3.0f, 2.0f / 3.0f,
                                            1e-6f, 1e6f, -2.5e-3f, 0.1f, 0.2f };
        pom2::Settings s;
        for (size_t i = 0; i < ints.size(); ++i)   s.setInt  ("i" + std::to_string(i), ints[i]);
        for (size_t i = 0; i < floats.size(); ++i) s.setFloat("f" + std::to_string(i), floats[i]);
        s.setBool("bt", true);
        s.setBool("bf", false);
        assert(s.save());

        pom2::Settings r;
        assert(r.load());
        int bad = 0;
        for (size_t i = 0; i < ints.size(); ++i) {
            const std::string k = "i" + std::to_string(i);
            const int got = r.getInt(k, -999999);
            if (got != ints[i]) {
                std::printf("FAIL: int %s want %d got %d\n", k.c_str(), ints[i], got);
                ++bad;
            }
        }
        for (size_t i = 0; i < floats.size(); ++i) {
            const std::string k = "f" + std::to_string(i);
            const float got = r.getFloat(k, -1e30f);
            if (got != floats[i]) {
                std::printf("FAIL: float %s want %.9g got %.9g (stored \"%s\")\n",
                            k.c_str(), double(floats[i]), double(got),
                            r.getString(k, "?").c_str());
                ++bad;
            }
        }
        if (r.getBool("bt", false) != true || r.getBool("bf", true) != false) {
            std::printf("FAIL: bool round-trip\n");
            ++bad;
        }
        if (bad) { fs::remove_all(home); return 1; }
    }

    // ── Partial parses are NOT values (round 11 #39) ─────────────────────
    // state.cfg's header invites hand-editing, and `std::stoi`/`std::stof`
    // stop at the first character they cannot use and report success on what
    // they got: "6503 # typo" was read as the CPU clock 6503, "4M" as 4, and
    // "1.5x" as 1.5f. A value that is not entirely a number must fall back to
    // the caller's default — the rule CliDispatcher::parseIntPositive already
    // applies to command-line numbers.
    {
        pom2::Settings s;
        s.setString("bad_int_comment", "6503 # was 1022727");
        s.setString("bad_int_unit",    "4M");
        s.setString("bad_int_trail",   "12abc");
        s.setString("bad_float_unit",  "1.5x");
        s.setString("bad_float_pct",   "80%");
        s.setString("empty_val",       "");
        // Still accepted: a clean number, and one padded with blanks (load()
        // trims the line, but an escaped boundary space survives).
        s.setString("good_int",        "1022727");
        s.setString("good_int_pad",    " 42 ");
        s.setString("good_float",      "0.25");
        assert(s.save());

        pom2::Settings r;
        assert(r.load());
        int bad = 0;
        auto wantInt = [&](const char* k, int def, int want) {
            const int got = r.getInt(k, def);
            if (got != want) {
                std::printf("FAIL: getInt(%s) want %d got %d\n", k, want, got);
                ++bad;
            }
        };
        auto wantFloat = [&](const char* k, float def, float want) {
            const float got = r.getFloat(k, def);
            if (got != want) {
                std::printf("FAIL: getFloat(%s) want %g got %g\n",
                            k, double(want), double(got));
                ++bad;
            }
        };
        wantInt("bad_int_comment", 1022727, 1022727);
        wantInt("bad_int_unit",    -7,      -7);
        wantInt("bad_int_trail",   -7,      -7);
        wantInt("empty_val",       -7,      -7);
        wantInt("good_int",        -7,      1022727);
        wantInt("good_int_pad",    -7,      42);
        wantFloat("bad_float_unit", 3.0f,   3.0f);
        wantFloat("bad_float_pct",  3.0f,   3.0f);
        wantFloat("good_float",    -1.0f,   0.25f);
        // A missing key is still the default, and getString is untouched by
        // any of this (the raw text is what a path setting needs).
        wantInt("no_such_key", 5, 5);
        assert(r.getString("bad_int_unit") == "4M");
        if (bad) { fs::remove_all(home); return 1; }
    }

    // ── The Joystick panel's binding survives a restart (round 11 #40) ────
    // Only `joystick_square_gate` was ever written, so the pad the user
    // picked, the deadzone they dialled and the two invert flags came back
    // as defaults every launch. `joystick_host` also doubles as the
    // "the user decided" marker: -1 means "(none)" and must not be
    // auto-re-bound, which is why MainWindow reads it with a -2 sentinel
    // rather than a plain -1 default.
    {
        pom2::Settings s;
        s.setInt  ("joystick_host",       -1);
        s.setFloat("joystick_deadzone",   0.175f);
        s.setBool ("joystick_invert_x",   true);
        s.setBool ("joystick_invert_y",   false);
        s.setBool ("joystick_square_gate", false);
        assert(s.save());

        pom2::Settings r;
        assert(r.load());
        assert(r.getInt  ("joystick_host",       -2) == -1);
        assert(r.getFloat("joystick_deadzone",   0.10f) == 0.175f);
        assert(r.getBool ("joystick_invert_x",   false) == true);
        assert(r.getBool ("joystick_invert_y",   true)  == false);
        assert(r.getBool ("joystick_square_gate", true) == false);
        // Absent on a fresh install → the sentinel survives, and the
        // auto-binder stays in charge.
        pom2::Settings fresh;
        assert(fresh.getInt("joystick_host", -2) == -2);
    }

    // A damaged or hostile line-oriented state file must be rejected before
    // getline can grow a multi-megabyte string during application startup.
    {
        pom2::Settings pathProbe;
        std::ofstream huge(pathProbe.getStorePath(),
                           std::ios::binary | std::ios::trunc);
        assert(huge);
        huge.seekp(4 * 1024 * 1024);
        huge.put('x');
        huge.close();
        pom2::Settings rejected;
        assert(!rejected.load());
    }

    // ── Where the file goes is ONE decision ──────────────────────────────
    // Settings and the ImGui layout used to resolve their directory with two
    // hand-copied platform dances, and under Emscripten they had drifted:
    // neither knew about the IDBFS mount, so the browser build wrote both to
    // a filesystem that does not survive a reload. Both now call
    // pom2::userConfigDir(); this pins that the store still lands under it.
    {
        pom2::Settings probe;
        const fs::path dir  = pom2::userConfigDir();
        const fs::path file = fs::path(probe.getStorePath());
        assert(!dir.empty() && "a writable HOME must yield a config dir");
        assert(fs::is_directory(dir) && "userConfigDir creates its directory");
        assert(file.parent_path() == dir &&
               "state.cfg lives in userConfigDir()");
        assert(file.filename() == "state.cfg");
        // And it follows the environment rather than a cached first answer —
        // the ImGui ini path is resolved from a different call site at a
        // different moment in startup.
        const fs::path other = home / "elsewhere";
        fs::create_directories(other);
#if defined(__APPLE__)
        ::setenv("HOME", other.string().c_str(), 1);
#elif !defined(_WIN32)
        ::setenv("XDG_CONFIG_HOME", (other / "config").string().c_str(), 1);
#endif
#ifndef _WIN32
        assert(pom2::userConfigDir() != dir &&
               "userConfigDir must not cache its first answer");
        ::setenv("HOME", home.string().c_str(), 1);
#  if !defined(__APPLE__)
        ::setenv("XDG_CONFIG_HOME", (home / "config").string().c_str(), 1);
#  endif
        assert(pom2::userConfigDir() == dir);
#endif
    }

    // ── An unchanged save writes nothing ─────────────────────────────────
    // The browser build has no "on exit" moment (main() never returns under
    // simulate_infinite_loop), so it persists the whole session on a 10 s
    // heartbeat. That is only affordable because a save whose content matches
    // the last one is skipped. Proven by deleting the file underneath: a save
    // that still writes would recreate it.
    {
        pom2::Settings s;
        s.setString("alpha", "one");
        assert(s.save());
        const fs::path file(s.getStorePath());
        assert(fs::exists(file));
        fs::remove(file);
        assert(s.save() && "an unchanged save reports success");
        assert(!fs::exists(file) && "an unchanged save does not write");
        // A real change writes again.
        s.setString("alpha", "two");
        assert(s.save());
        assert(fs::exists(file));
        // Including a change that only REMOVES nothing and adds a key —
        // equality is over the whole store, not over one value.
        fs::remove(file);
        s.setBool("beta", true);
        assert(s.save());
        assert(fs::exists(file) && "a new key counts as a change");
    }

    // ── The list encoding, now that two translation units share it ───────
    // MainWindow.cpp reads these values and MainWindow_Session.cpp writes
    // them; before SettingsList.h they were two copies of one separator.
    {
        const std::vector<std::string> paths = {
            "/home/u/My Disks/game one.dsk",     // spaces
            "/home/u/a,b;c:d.dsk",               // every punctuation a naive
                                                 // separator would have used
            "/home/u/My#Disks/x.dsk",            // '#'
        };
        const std::string packed = pom2::joinSettingList(paths);
        assert(pom2::splitSettingList(packed) == paths);
        // Through the store, where escaping also applies.
        pom2::Settings s;
        s.setString("library_recents", packed);
        assert(s.save());
        pom2::Settings back;
        assert(back.load());
        assert(pom2::splitSettingList(back.getString("library_recents")) == paths);
        // Empty list, and a trailing separator from a hand-edited file.
        assert(pom2::joinSettingList({}).empty());
        assert(pom2::splitSettingList("").empty());
        assert(pom2::splitSettingList(std::string("a") + pom2::kSettingListSep)
               == std::vector<std::string>{"a"});
    }

    // ── KEYS, which were never escaped ──────────────────────────────────
    // load() decides where a record starts and where it splits BEFORE it
    // unescapes anything, so two characters in a KEY corrupted the store
    // while the same characters in a VALUE were fine:
    //   '='   the split is on the first '=', so "a=b" landed as key "a",
    //         value "b=v" — a different setting, silently.
    //   '#'   at position 0 made the whole line a comment; the key vanished.
    // (Bug hunt 2026-09-06 #H24.)
    {
        const std::string kEqKey   = "weird=key";
        const std::string kHashKey = "#hashkey";
        pom2::Settings s;
        s.setString(kEqKey,   "eqvalue");
        s.setString(kHashKey, "hashvalue");
        s.setString("plain",  "kept");
        assert(s.save());

        pom2::Settings back;
        assert(back.load());
        assert(back.getString(kEqKey,   "MISSING") == "eqvalue");
        assert(back.getString(kHashKey, "MISSING") == "hashvalue");
        assert(back.getString("plain",  "MISSING") == "kept");
        // ...and nothing invented a key from the mangled halves.
        assert(back.getString("weird", "MISSING") == "MISSING");
    }

    // ── A UTF-8 BOM must not eat the first key ──────────────────────────
    // What a Windows editor leaves behind when a user hand-edits the file
    // the header invites them to edit. Three bytes glued to the first key,
    // one setting lost, no message.
    {
        pom2::Settings seed;
        seed.setString("aaa_first", "one");
        seed.setString("zzz_last",  "two");
        assert(seed.save());

        const fs::path file(seed.getStorePath());
        std::string all;
        {
            std::ifstream in(file, std::ios::binary);
            all.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
        }
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "\xEF\xBB\xBF" << all;
        }
        pom2::Settings back;
        assert(back.load());
        assert(back.getString("aaa_first", "MISSING") == "one");
        assert(back.getString("zzz_last",  "MISSING") == "two");
    }

    // ── The store holds the AI control token: 0600, not 0644 ────────────
    // And a failed save must not leave its temp file behind: the name is
    // unique per call now, so debris would accumulate one file per attempt.
    {
        pom2::Settings s;
        s.setString("ai_control_token", "s3cret");
        assert(s.save());
        const fs::path file(s.getStorePath());
#if !defined(_WIN32)
        const auto perms = fs::status(file).permissions();
        assert((perms & fs::perms::group_all) == fs::perms::none);
        assert((perms & fs::perms::others_all) == fs::perms::none);
        assert((perms & fs::perms::owner_read) != fs::perms::none);
        assert((perms & fs::perms::owner_write) != fs::perms::none);
#endif
        // Make the commit fail without permission tricks: a directory at the
        // store's own path makes the rename fail.
        fs::remove(file);
        fs::create_directory(file);
        s.setString("something_new", "x");
        assert(!s.save());
        int debris = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(file.parent_path(), ec)) {
            const std::string nm = e.path().filename().string();
            if (nm.size() > 8 && nm.compare(nm.size() - 8, 8, ".pom2tmp") == 0)
                ++debris;
        }
        assert(debris == 0);
        fs::remove_all(file);
    }

    fs::remove_all(home);
    std::printf("OK settings_roundtrip (#-in-value, newline, backslash, "
                "boundary whitespace, int/float/bool round-trip, partial "
                "parses rejected, joystick binding keys, config dir, "
                "unchanged-save skip, list encoding)\n");
    return 0;
}
