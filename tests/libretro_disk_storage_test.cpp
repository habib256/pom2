// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
// The libretro core's private disk copies: pom2/disks/<game>/<disk>, a
// second game whose names match gets its own copy, and a copy an older core
// named by hashes moves to its new name with the player's progress.
#include "disk_storage.hpp"
#include "pom2/core.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pom2::libretro::DiskStorageAccess;
using pom2::libretro::openDiskStorage;

namespace {

int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string sha(const std::string& text)
{
    return pom2::Core::sha256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

} // namespace

int main()
{
    const auto work = fs::temp_directory_path() / ("pom2_disk_storage_" + std::to_string(std::rand()));
    fs::remove_all(work);
    const auto saves = work / "saves";
    const auto disks = fs::weakly_canonical(fs::absolute(saves)) / "pom2" / "disks";

    // A game's copy is named after the game and the disk.
    const auto karateka = work / "games" / "Karateka.woz";
    write(karateka, "karateka side A");
    const auto first = openDiskStorage(saves, karateka, karateka);
    check(first.privateCopy && first.path == disks / "Karateka" / "Karateka.woz", "pom2/disks/Karateka/Karateka.woz");
    check(read(first.path) == "karateka side A", "the copy holds the original's bytes");
    write(first.path, "karateka with a save");
    check(openDiskStorage(saves, karateka, karateka).path == first.path &&
              read(first.path) == "karateka with a save", "reopening keeps the player's copy");

    // Another game with the same names gets its own copy, named by its hash.
    const auto other = work / "elsewhere" / "Karateka.woz";
    write(other, "a different disk");
    const auto second = openDiskStorage(saves, other, other);
    const auto suffix = " [" + sha("a different disk").substr(0, 8) + "].woz";
    check(second.path == disks / "Karateka" / ("Karateka" + suffix), "a name clash gets its own copy");
    check(read(first.path) == "karateka with a save", "a name clash left the first game's save alone");

    // A playlist names the folder; each disk keeps its file name.
    const auto sideB = work / "u4" / "Ultima IV side B.dsk";
    write(sideB, "ultima side B");
    const auto inPlaylist = openDiskStorage(saves, work / "u4" / "Ultima IV.m3u", sideB);
    check(inPlaylist.path == disks / "Ultima IV" / "Ultima IV side B.dsk", "pom2/disks/Ultima IV/<disk>");

    // ExistingOnly finds a copy and refuses to make one, in a game folder
    // that exists (so it is the copy that is refused, not the folder).
    fs::create_directories(disks / "nothing");
    bool refused = false;
    try {
        openDiskStorage(saves, work / "nothing.woz", karateka, DiskStorageAccess::ExistingOnly);
    } catch (const std::exception& e) { refused = std::string(e.what()).find("does not exist") != std::string::npos; }
    check(refused, "ExistingOnly made a copy");
    check(!fs::exists(disks / "nothing" / "Karateka.woz"), "ExistingOnly left a copy");

    // A record written on Windows (CRLF) still names the same image.
    std::ofstream(fs::path(first.path.string() + ".source"), std::ios::binary) << sha("karateka side A") << "\r\n";
    check(openDiskStorage(saves, karateka, karateka).path == first.path, "a CRLF record is the same image");

    // Names any file system takes, the same on every platform: no accents,
    // no reserved characters, no trailing dot, no device name.
    const auto accented = work / "fr" / fs::u8path("\xc3\x89nigme le retour.dsk");
    write(accented, "enigme");
    const auto portable = openDiskStorage(saves, accented, accented);
    const auto name = portable.path.filename().u8string();
    bool ascii = true;
    for (unsigned char c : portable.path.lexically_relative(disks).u8string()) ascii &= c >= 0x20 && c < 0x7f && c != ':';
    check(ascii && name.rfind("__nigme le retour [", 0) == 0 && name.size() > 4 &&
              name.compare(name.size() - 4, 4, ".dsk") == 0, "an accented name becomes portable ASCII");
#ifndef _WIN32   // Windows cannot create the original either
    const auto device = work / "dev" / "CON.dsk";
    write(device, "con");
    check(openDiskStorage(saves, device, device).path.filename().u8string().rfind("CON [", 0) == 0,
          "a device name is never a file name");
#endif
    const auto dotted = work / "dot" / "Game..dsk";
    write(dotted, "dotted");
    const auto dottedCopy = openDiskStorage(saves, dotted, dotted).path;
    check(dottedCopy.parent_path().filename().u8string().back() != '.', "a game folder never ends in a dot");

    // A copy named by an older core moves to the new name, progress included.
    const auto old = work / "old" / "Lode Runner.dsk";
    write(old, "lode runner");
    const auto content = fs::weakly_canonical(old).u8string();
    const auto legacy = disks / sha(content) /
                        (sha(fs::canonical(old).u8string() + "\n" + sha("lode runner")) + ".dsk");
    write(legacy, "lode runner with high scores");
    const auto moved = openDiskStorage(saves, old, old, DiskStorageAccess::ExistingOnly);
    check(moved.path == disks / "Lode Runner" / "Lode Runner.dsk" &&
              read(moved.path) == "lode runner with high scores", "the legacy copy moved with its progress");
    check(!fs::exists(legacy), "the legacy copy stayed behind");

    // The same image opened from another folder had its own copy in v1.2.3:
    // when the new name is taken, the newer copy plays and the other is kept.
    const auto again = work / "favourites" / "Lode Runner.dsk";
    write(again, "lode runner");
    const auto againLegacy = disks / sha(fs::weakly_canonical(again).u8string()) /
                             (sha(fs::canonical(again).u8string() + "\n" + sha("lode runner")) + ".dsk");
    write(againLegacy, "lode runner, newer progress");
    fs::last_write_time(againLegacy, fs::last_write_time(moved.path) + std::chrono::hours(1));
    const auto merged = openDiskStorage(saves, again, again);
    check(merged.path == moved.path && read(merged.path) == "lode runner, newer progress",
          "the newer copy plays");
    bool keptOlder = false;
    for (const auto& entry : fs::directory_iterator(disks / "Lode Runner"))
        keptOlder |= read(entry.path()) == "lode runner with high scores";
    check(keptOlder, "the other copy is kept beside it");
    check(!fs::exists(againLegacy), "the second legacy copy stayed behind");

    // A copy made just now and never played does not beat older progress.
    const auto fresh = work / "b" / "Choplifter.dsk";
    const auto played = work / "a" / "Choplifter.dsk";
    write(fresh, "choplifter");
    write(played, "choplifter");
    const auto playedLegacy = disks / sha(fs::weakly_canonical(played).u8string()) /
                              (sha(fs::canonical(played).u8string() + "\n" + sha("choplifter")) + ".dsk");
    write(playedLegacy, "choplifter, 64 hostages saved");
    fs::last_write_time(playedLegacy, fs::file_time_type::clock::now() - std::chrono::hours(24));
    const auto freshCopy = openDiskStorage(saves, fresh, fresh);   // new, unplayed
    check(read(openDiskStorage(saves, played, played).path) == "choplifter, 64 hostages saved" &&
              freshCopy.path == disks / "Choplifter" / "Choplifter.dsk", "played progress beats a fresh copy");

    fs::remove_all(work);
    if (failures) return 1;
    std::puts("libretro_disk_storage: ok");
    return 0;
}
