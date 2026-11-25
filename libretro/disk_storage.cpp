// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk_storage.hpp"

#include "AtomicFileReplace.h"
#include "portable_name.hpp"
#include "pom2/core.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/stat.h>
#endif

// Nothing here throws (disk_storage.hpp): every std::filesystem call takes an
// error_code, and a failure comes back as a message. RetroArch's web build
// links with exception catching off, where a throw ends the page.

namespace pom2::libretro {
namespace {
namespace fs = std::filesystem;

// The largest ProDOS volume (65535 blocks, 32 MiB) with room for a 2IMG
// header and comment: a hard disk image needs its private copy as a floppy does.
constexpr std::uintmax_t kMaxDiskBytes = 33 * 1024 * 1024;

std::string digest(std::string_view text)
{
    return Core::sha256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

bool ensureDirectory(const fs::path& path, DiskStorageAccess access, std::string& error)
{
    std::error_code status;
    if (access == DiskStorageAccess::CreateIfMissing) fs::create_directory(path, status);
    if (status || !fs::is_directory(fs::symlink_status(path, status))) {
        error = "Cannot use disk save directory " + path.u8string() + (status ? ": " + status.message() : "");
        return false;
    }
    return true;
}

bool ensureDirectories(const fs::path& directory, DiskStorageAccess access, std::string& error)
{
    std::error_code status;
    if (access != DiskStorageAccess::CreateIfMissing || fs::is_directory(directory, status)) return true;
    auto parent = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        parent /= component;
        if (!ensureDirectory(parent, access, error)) return false;
    }
    return true;
}

bool readOriginal(const fs::path& original, std::vector<std::uint8_t>& bytes, std::string& error)
{
    std::ifstream input(original, std::ios::binary | std::ios::ate);
    if (!input) { error = "Cannot read disk " + original.u8string(); return false; }
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxDiskBytes) {
        error = "Disk image is too large: " + original.u8string();
        return false;
    }
    bytes.assign(static_cast<std::size_t>(length), 0);
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size()))) {
        error = "Cannot read disk " + original.u8string();
        return false;
    }
    return true;
}

// A copy at `backing`: absent (false, no error), usable (true), or present
// and not a private regular file (false, error set).
bool usableBacking(const fs::path& backing, std::string& error)
{
    std::error_code status;
    const auto type = fs::symlink_status(backing, status);
    if (type.type() == fs::file_type::not_found) return false;
    std::error_code links;
    if (status || !fs::is_regular_file(type) || fs::hard_link_count(backing, links) != 1 || links) {
        error = "Disk save must be a private regular file: " + backing.u8string();
        return false;
    }
    return true;
}

bool createBacking(const fs::path& backing, const std::vector<std::uint8_t>& bytes, std::string& error)
{
    const auto temporary = tempSiblingPath(backing);
    std::error_code status;
    if (!prepareTempPath(temporary, status)) {
        error = "Cannot prepare disk save " + backing.u8string() + ": " + status.message();
        return false;
    }
    std::ofstream copy(temporary, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        copy.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    copy.close();
    if (copy && publishNewFileAtomic(temporary, backing, status)) return true;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    // Another instance can create the same backing first. Keep its version.
    if (status == std::errc::file_exists && usableBacking(backing, error)) return true;
    if (error.empty())
        error = "Cannot create disk save " + backing.u8string() + (status ? ": " + status.message() : "");
    return false;
}

} // namespace

class DiskSaveLock {
public:
#ifdef _WIN32
    ~DiskSaveLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    ~DiskSaveLock() { if (fd >= 0) ::close(fd); }
    int fd = -1;
#endif
};

std::shared_ptr<DiskSaveLock> lockDiskStorage(const fs::path& backing)
{
    auto lock = backing;
    lock += ".lock";
    auto lease = std::make_shared<DiskSaveLock>();
#ifdef _WIN32
    lease->handle = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lease->handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lease->handle, &info) || GetFileType(lease->handle) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
#else
    lease->fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lease->fd < 0) return {};
    struct stat info{};
    if (::fstat(lease->fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        ::flock(lease->fd, LOCK_EX | LOCK_NB) != 0) return {};
#endif
    return lease;
}

namespace {

// What a private copy was made from: the original's SHA-256, beside the copy.
fs::path sourceRecord(const fs::path& backing)
{
    auto record = backing;
    record += ".source";
    return record;
}

// Binary, and read without trailing blanks: a record written on Windows
// ("hash\r\n") must match on Linux or Android, or a save synced between them
// would be taken for another image (bug hunt 2026-11-25).
std::string recordedSource(const fs::path& backing)
{
    std::ifstream input(sourceRecord(backing), std::ios::binary);
    std::string line;
    std::getline(input, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    return line;
}

void recordSource(const fs::path& backing, const std::string& sourceHash)
{
    std::ofstream output(sourceRecord(backing), std::ios::binary | std::ios::trunc);
    output << sourceHash << '\n';
}

bool renameWithRecord(const fs::path& from, const fs::path& to, std::error_code& status)
{
    fs::rename(from, to, status);
    if (status) return false;
    std::error_code ignored;
    if (fs::exists(sourceRecord(from), ignored)) fs::rename(sourceRecord(from), sourceRecord(to), ignored);
    return true;
}

// Before 2026-11-25 a copy was named by hashes alone:
// pom2/disks/<sha256(content path)>/<sha256(disk path, disk hash)><ext>.
fs::path legacyBacking(const fs::path& disks, const fs::path& content, const fs::path& original,
                       const std::string& sourceHash)
{
    std::error_code status;
    const auto sourceIdentity = fs::canonical(original, status).u8string();
    if (status) return {};
    const auto title = content.empty() ? std::string{} : fs::weakly_canonical(content, status).u8string();
    if (status) return {};
    return disks / digest(title) /
           (digest(sourceIdentity + "\n" + sourceHash) + original.extension().u8string());
}

} // namespace

DiskStorage openDiskStorage(const fs::path& saves, const fs::path& content,
                            const fs::path& original, std::string& error,
                            DiskStorageAccess access, unsigned variant)
{
    error.clear();
    const auto fail = [&error](std::string message) {
        if (error.empty()) error = std::move(message);
        return DiskStorage{};
    };
    if (saves.empty()) return {original};
    constexpr auto writeBits = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    std::error_code status;
    const auto permissions = fs::status(original, status).permissions();
    if (status) return fail("Cannot read disk " + original.u8string() + ": " + status.message());
    if ((permissions & writeBits) == fs::perms::none) return {original};

    std::vector<std::uint8_t> bytes;
    if (!readOriginal(original, bytes, error)) return {};
    const auto sourceHash = Core::sha256(bytes.data(), bytes.size());

    // pom2/disks/<game>/<disk file name>: a frontend finds a game's saves by
    // name (RetroBat asked, emulatorlauncher#1498). <game> is the content's
    // name — "Ultima IV" for Ultima IV.m3u, "Karateka" for Karateka.woz.
    const auto absolute = fs::absolute(saves, status);
    if (status) return fail("Cannot use disk save directory " + saves.u8string() + ": " + status.message());
    const auto root = fs::weakly_canonical(absolute, status);
    if (status) return fail("Cannot use disk save directory " + saves.u8string() + ": " + status.message());
    if (!ensureDirectories(root, access, error)) return {};
    auto disks = root / "pom2";
    if (!ensureDirectory(disks, access, error)) return {};
    disks /= "disks";
    if (!ensureDirectory(disks, access, error)) return {};
    auto gameName = (content.empty() ? original : content).stem().u8string();
    if (gameName.empty() || gameName == "." || gameName == "..") gameName = "disk";
    const auto directory = disks / fs::u8path(portableName(gameName));
    const auto legacy = legacyBacking(disks, content, original, sourceHash);
    std::error_code probe;
    const bool hasLegacy = !legacy.empty() && fs::is_regular_file(fs::symlink_status(legacy, probe));
    // A restore (ExistingOnly) still makes the folder a legacy copy moves to.
    if (!ensureDirectory(directory, hasLegacy ? DiskStorageAccess::CreateIfMissing : access, error)) return {};

    // The .source record keeps two games whose names match from sharing one
    // copy: the second one, or a new revision of the image, gets its own,
    // named with the start of its hash. `variant` keeps apart two disks of ONE
    // playlist with the same file name (two blank SAVE.DSK in two folders).
    const auto diskStem = portableName(original.stem().u8string()) +
                          (variant ? " (" + std::to_string(variant + 1) + ")" : std::string());
    const auto diskExtension = portableExtension(original.extension().u8string());
    auto backing = directory / fs::u8path(diskStem + diskExtension);
    if (usableBacking(backing, error)) {
        const auto recorded = recordedSource(backing);
        if (recorded.empty()) recordSource(backing, sourceHash);   // a copy that lost its record
        else if (recorded != sourceHash)
            backing = directory / fs::u8path(diskStem + " [" + sourceHash.substr(0, 8) + "]" + diskExtension);
    }
    if (!error.empty()) return {};
    const auto moveLegacy = [&] {
        std::error_code ignored;
        auto lock = legacy;
        lock += ".lock";
        fs::remove(lock, ignored);
        fs::remove(legacy.parent_path(), ignored);   // only when empty
    };
    const bool legacyUsable = hasLegacy && usableBacking(legacy, error);
    if (!error.empty()) return {};
    const bool backingUsable = usableBacking(backing, error);
    if (!error.empty()) return {};
    if (legacyUsable && backingUsable) {
        // v1.2.3 kept one copy per content LOCATION; the new name has one per
        // game. When a copy of this game already took the name (the same
        // image opened from another folder or another playlist), neither is
        // lost: the one with progress plays, the other is kept beside it as
        // "<disk> [old <location hash> <image hash>]" (bug hunt 2026-11-25).
        const auto kept = directory / fs::u8path(diskStem + " [old " +
            legacy.parent_path().filename().u8string().substr(0, 8) + " " + sourceHash.substr(0, 8) + "]" +
            diskExtension);
        const bool keptExists = usableBacking(kept, error);
        if (!error.empty()) return {};
        // Never under a copy in use (another drive, another RetroArch): its
        // next save would land on whatever was moved to its name. Left for
        // a later open.
        const auto lease = keptExists ? nullptr : lockDiskStorage(backing);
        if (lease) {
            // Progress first: a copy still holding the original's bytes (made
            // at first boot, never played) loses to one that was played,
            // whatever their times; between two played copies, the newer.
            std::vector<std::uint8_t> legacyBytes, backingBytes;
            std::string unread;
            const bool legacyPlayed = !readOriginal(legacy, legacyBytes, unread) || legacyBytes != bytes;
            const bool backingPlayed = !readOriginal(backing, backingBytes, unread) || backingBytes != bytes;
            std::error_code legacyTime, backingTime;
            const bool legacyNewer =
                fs::last_write_time(legacy, legacyTime) > fs::last_write_time(backing, backingTime) &&
                !legacyTime && !backingTime;
            const bool legacyWins = legacyPlayed && (!backingPlayed || legacyNewer);
            if (legacyWins) {
                if (!renameWithRecord(backing, kept, status) || (fs::rename(legacy, backing, status), status))
                    return fail("Cannot move disk save " + legacy.u8string() + ": " + status.message());
                recordSource(backing, sourceHash);
            } else {
                if (fs::rename(legacy, kept, status), status)
                    return fail("Cannot move disk save " + legacy.u8string() + ": " + status.message());
                recordSource(kept, sourceHash);
            }
            moveLegacy();
        }
    }
    if (!usableBacking(backing, error)) {
        if (!error.empty()) return {};
        if (hasLegacy && usableBacking(legacy, error)) {
            // Keep the player's progress: move the old copy to its new name.
            fs::rename(legacy, backing, status);
            if (status)
                return fail("Cannot move disk save " + legacy.u8string() + " to " + backing.u8string() + ": " +
                            status.message());
            moveLegacy();
            recordSource(backing, sourceHash);
        } else if (!error.empty()) {
            return {};
        } else if (access == DiskStorageAccess::ExistingOnly) {
            return fail("Disk save does not exist: " + backing.u8string());
        } else {
            // The record first: an instance that finds the copy then finds
            // what it was made from, never an empty record to adopt.
            recordSource(backing, sourceHash);
            if (!createBacking(backing, bytes, error)) return {};
        }
    }
    if (fs::equivalent(backing, original, status) || status)
        return fail(status ? "Cannot compare " + backing.u8string() + ": " + status.message()
                           : "Disk save cannot be the original disk image: " + original.u8string());
    return {backing, true};
}

fs::path legacyStoragePath(const fs::path& saves, const fs::path& content, const fs::path& original)
{
    if (saves.empty()) return {};
    std::vector<std::uint8_t> bytes;
    std::string unread;
    if (!readOriginal(original, bytes, unread)) return {};
    std::error_code status;
    const auto root = fs::weakly_canonical(fs::absolute(saves, status), status);
    if (status) return {};
    return legacyBacking(root / "pom2" / "disks", content, original, Core::sha256(bytes.data(), bytes.size()));
}

} // namespace pom2::libretro
