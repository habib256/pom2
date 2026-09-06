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

// Smoke test for FloppyEmuDevice (the pure BMOW Floppy Emu model): mode
// label/key round-trip, per-mode file-format filtering, SD-card directory
// navigation bounded to the root, and favdisks.txt parsing (automount
// directive + relative path resolution).

#include "FloppyEmuDevice.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pom2::FloppyEmuDevice;
using pom2::FloppyEmuMode;

namespace {

bool hasEntry(const std::vector<FloppyEmuDevice::Entry>& v, const std::string& n)
{
    return std::any_of(v.begin(), v.end(),
                       [&](const auto& e){ return e.name == n; });
}

void touch(const fs::path& p)
{
    std::ofstream f(p, std::ios::binary);
    f << "x";
}

bool testModeRoundTrip()
{
    for (auto m : FloppyEmuDevice::allModes()) {
        FloppyEmuMode back;
        if (!FloppyEmuDevice::modeFromKey(FloppyEmuDevice::modeKey(m), back) ||
            back != m) {
            std::printf("FAIL: mode key round-trip for %s\n",
                        FloppyEmuDevice::modeLabel(m));
            return false;
        }
    }
    FloppyEmuMode dummy;
    if (FloppyEmuDevice::modeFromKey("bogus", dummy)) {
        std::printf("FAIL: accepted bogus mode key\n"); return false;
    }
    std::printf("OK : mode label/key round-trip\n");
    return true;
}

bool testFormatFilter()
{
    FloppyEmuDevice d;
    d.setMode(FloppyEmuMode::Disk525);
    if (!d.acceptsFile("a.woz") || !d.acceptsFile("B.NIB") ||
        !d.acceptsFile("c.dsk") || d.acceptsFile("d.hdv")) {
        std::printf("FAIL: 5.25 filter\n"); return false;
    }
    d.setMode(FloppyEmuMode::SmartportHD);
    if (!d.acceptsFile("x.hdv") || !d.acceptsFile("y.2mg") ||
        !d.acceptsFile("z.po") || d.acceptsFile("w.woz")) {
        std::printf("FAIL: smartport filter\n"); return false;
    }
    d.setMode(FloppyEmuMode::Disk35);
    if (!d.acceptsFile("a.po") || d.acceptsFile("a.nib")) {
        std::printf("FAIL: 3.5 filter\n"); return false;
    }
    std::printf("OK : per-mode format filter\n");
    return true;
}

bool testNavigation()
{
    const fs::path root = fs::temp_directory_path() / "pom2_femu_sd";
    fs::remove_all(root);
    fs::create_directories(root / "GAMES");
    touch(root / "PRODOS.po");          // accepted (smartport)
    touch(root / "README.txt");         // ignored
    touch(root / "GAMES" / "TR.2mg");   // accepted in subdir

    FloppyEmuDevice d;
    d.setMode(FloppyEmuMode::SmartportHD);
    d.setSdRoot(root.string());

    if (!d.sdPresent()) { std::printf("FAIL: sdPresent\n"); return false; }
    if (!d.atRoot())    { std::printf("FAIL: not at root after setSdRoot\n"); return false; }

    auto top = d.listing();
    if (hasEntry(top, ".."))         { std::printf("FAIL: '..' shown at root\n"); return false; }
    if (!hasEntry(top, "GAMES"))     { std::printf("FAIL: subdir missing\n"); return false; }
    if (!hasEntry(top, "PRODOS.po")) { std::printf("FAIL: image missing\n"); return false; }
    if (hasEntry(top, "README.txt")) { std::printf("FAIL: non-image listed\n"); return false; }
    // Directories sort before files.
    if (!top.empty() && !top.front().isDir) {
        std::printf("FAIL: dirs not first\n"); return false;
    }

    // Enter GAMES.
    FloppyEmuDevice::Entry gamesEntry;
    for (auto& e : top) if (e.name == "GAMES") gamesEntry = e;
    d.enterDir(gamesEntry);
    if (d.atRoot()) { std::printf("FAIL: still at root after enter\n"); return false; }
    auto sub = d.listing();
    if (!hasEntry(sub, ".."))      { std::printf("FAIL: '..' missing in subdir\n"); return false; }
    if (!hasEntry(sub, "TR.2mg"))  { std::printf("FAIL: subdir image missing\n"); return false; }

    // Go up — back at root.
    d.goUp();
    if (!d.atRoot()) { std::printf("FAIL: goUp didn't reach root\n"); return false; }
    // goUp at root is a no-op (can't escape the SD card).
    d.goUp();
    if (!d.atRoot()) { std::printf("FAIL: escaped above root\n"); return false; }

    // R6-#1: a symlink in the SD root must NOT be followed — directory_iterator
    // + is_directory() dereference links, which would let an in-root symlink
    // browse files anywhere on the host (sandbox escape).
    {
        const fs::path outside = fs::temp_directory_path() / "pom2_femu_outside";
        fs::create_directories(outside);
        touch(outside / "SECRET.po");
        std::error_code ec;
        fs::create_symlink(outside, root / "ESCAPE", ec);
        if (!ec) {   // skip if the platform/FS lacks symlink support
            auto lst = d.listing();
            if (hasEntry(lst, "ESCAPE")) {
                std::printf("FAIL: symlink ESCAPE listed — SD sandbox escape\n");
                return false;
            }
        }
        fs::remove_all(outside);
    }

    fs::remove_all(root);
    std::printf("OK : SD navigation bounded to root\n");
    return true;
}

bool testFavorites()
{
    const std::string base = (fs::temp_directory_path() / "pom2_femu_sd").string();
    // `favdisks.txt` lives INSIDE the emulated SD card, so the guest can
    // write it. The listing has always clamped navigation to the SD root;
    // this path used an absolute line verbatim and let `../..` be collapsed
    // out by lexically_normal and then followed — so one line in a text file
    // the guest controls put an arbitrary host file (or the user's whole
    // home) one click away from being mounted. Both escapes are now dropped
    // rather than rewritten: a favourite that does not name something on the
    // card is not a favourite. (Bug hunt 2026-09-06 #H8.)
    const std::string content =
        "automount 2\n"
        "# a comment line\n"
        "GAMES/TR.2mg\n"
        "/abs/PRODOS.po\n"
        "../../etc/passwd\n"
        "GAMES/../SYS.po\n";
    auto fav = FloppyEmuDevice::parseFavorites(content, base);
    if (!fav.present)        { std::printf("FAIL: favorites not present\n"); return false; }
    if (fav.automount != 2)  { std::printf("FAIL: automount=%d (want 2)\n", fav.automount); return false; }
    // TR.2mg and SYS.po survive; the absolute path and the `../..` climb out
    // of the root do not.
    if (fav.entries.size() != 2) {
        std::printf("FAIL: %zu favorites (want 2)\n", fav.entries.size()); return false;
    }
    if (fav.entries[0].name != "TR.2mg") {
        std::printf("FAIL: fav[0] name %s\n", fav.entries[0].name.c_str()); return false;
    }
    // Relative path resolved against base...
    if (fav.entries[0].fullPath.find(base) == std::string::npos) {
        std::printf("FAIL: relative favorite not resolved against SD root\n"); return false;
    }
    // ...and an interior `..` that stays inside the card is fine.
    if (fav.entries[1].fullPath != (fs::path(base) / "SYS.po").string()) {
        std::printf("FAIL: in-root favorite altered: %s\n",
                    fav.entries[1].fullPath.c_str()); return false;
    }
    for (const auto& e : fav.entries) {
        if (e.fullPath.find(base) != 0) {
            std::printf("FAIL: favorite escaped the SD root: %s\n",
                        e.fullPath.c_str());
            return false;
        }
    }
    // R6-#4: a favorite whose file doesn't exist must report size 0, not the
    // (uint64_t)-1 garbage from an ignored file_size error code. Neither
    // favorite exists in this test's context.
    if (fav.entries[1].sizeBytes != 0) {
        std::printf("FAIL: missing favorite size = %llu (want 0)\n",
                    static_cast<unsigned long long>(fav.entries[1].sizeBytes));
        return false;
    }
    std::printf("OK : favdisks.txt parse (automount + paths + sandbox)\n");
    return true;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= testModeRoundTrip();
    ok &= testFormatFilter();
    ok &= testNavigation();
    ok &= testFavorites();
    return ok ? 0 : 1;
}
