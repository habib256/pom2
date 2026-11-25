// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk_storage.hpp"

#include "AtomicFileReplace.h"
#include "pom2/core.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/stat.h>
#endif

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

void ensureDirectory(const fs::path& path, DiskStorageAccess access)
{
    std::error_code error;
    if (access == DiskStorageAccess::CreateIfMissing) fs::create_directory(path, error);
    if (error || !fs::is_directory(fs::symlink_status(path, error)))
        throw std::runtime_error("Cannot use disk save directory " + path.u8string() +
                                 (error ? ": " + error.message() : ""));
}

std::vector<std::uint8_t> readOriginal(const fs::path& original)
{
    std::ifstream input(original, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read disk " + original.u8string());
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxDiskBytes)
        throw std::runtime_error("Disk image is too large: " + original.u8string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read disk " + original.u8string());
    return bytes;
}

bool usableBacking(const fs::path& backing)
{
    std::error_code error;
    const auto status = fs::symlink_status(backing, error);
    if (status.type() == fs::file_type::not_found) return false;
    if (error || !fs::is_regular_file(status) || fs::hard_link_count(backing) != 1)
        throw std::runtime_error("Disk save must be a private regular file: " + backing.u8string());
    return true;
}

void createBacking(const fs::path& backing, const std::vector<std::uint8_t>& bytes)
{
    const auto temporary = tempSiblingPath(backing);
    std::error_code error;
    if (!prepareTempPath(temporary, error))
        throw std::runtime_error("Cannot prepare disk save " + backing.u8string() + ": " + error.message());
    std::ofstream copy(temporary, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        copy.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    copy.close();
    if (copy && publishNewFileAtomic(temporary, backing, error)) return;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    // Another instance can create the same backing first. Keep its version.
    if (error == std::errc::file_exists && usableBacking(backing)) return;
    throw std::runtime_error("Cannot create disk save " + backing.u8string() +
                             (error ? ": " + error.message() : ""));
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

// A name every file system and every frontend takes alike, and the same on
// every platform, so a save folder synced between a PC and a phone matches:
// printable ASCII, none of <>:"/\|?*, no trailing dot or space (Windows
// trims those), no device name (CON, NUL, COM1...), at most 96 bytes. The
// copy's path also reaches narrow std::string APIs, which on Windows take
// only the ANSI code page (bug hunt 2026-11-25). A name that had to change
// keeps its uniqueness with the start of its own hash.
std::string portableName(const std::string& name)
{
    std::string out;
    for (const unsigned char c : name)
        out += (c < 0x20 || c >= 0x7f || std::strchr("<>:\"/\\|?*", c)) ? '_' : static_cast<char>(c);
    const auto trimEnd = [&out] {
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    };
    trimEnd();
    if (out.size() > 96) out.resize(96);
    trimEnd();
    std::string device = out.substr(0, out.find('.'));
    for (auto& c : device) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* const kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
        "LPT8", "LPT9"};
    const bool reserved = std::any_of(std::begin(kDevices), std::end(kDevices),
                                      [&device](const char* d) { return device == d; });
    if (out == name && !reserved && !out.empty()) return out;
    return (out.empty() ? std::string("disk") : out) + " [" + digest(name).substr(0, 8) + "]";
}

std::string portableExtension(const fs::path& original)
{
    std::string ext;
    for (const unsigned char c : original.extension().u8string())
        if (std::isalnum(c) || c == '.') ext += static_cast<char>(c);
    return ext.size() > 1 && ext.size() <= 8 ? ext : std::string();
}

void renameWithRecord(const fs::path& from, const fs::path& to)
{
    fs::rename(from, to);
    std::error_code error;
    if (fs::exists(sourceRecord(from), error)) fs::rename(sourceRecord(from), sourceRecord(to), error);
}

void ensureDirectories(const fs::path& directory, DiskStorageAccess access)
{
    if (access != DiskStorageAccess::CreateIfMissing || fs::is_directory(directory)) return;
    auto parent = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        parent /= component;
        ensureDirectory(parent, access);
    }
}

// Before 2026-11-25 a copy was named by hashes alone:
// pom2/disks/<sha256(content path)>/<sha256(disk path, disk hash)><ext>.
fs::path legacyBacking(const fs::path& disks, const fs::path& content, const fs::path& original,
                       const std::string& sourceHash)
{
    const auto sourceIdentity = fs::canonical(original).u8string();
    const auto title = content.empty() ? std::string{} : fs::weakly_canonical(content).u8string();
    return disks / digest(title) /
           (digest(sourceIdentity + "\n" + sourceHash) + original.extension().u8string());
}

} // namespace

DiskStorage openDiskStorage(const fs::path& saves, const fs::path& content,
                            const fs::path& original, DiskStorageAccess access)
{
    if (saves.empty()) return {original};
    constexpr auto writeBits = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    if ((fs::status(original).permissions() & writeBits) == fs::perms::none)
        return {original};

    const auto bytes = readOriginal(original);
    const auto sourceHash = Core::sha256(bytes.data(), bytes.size());

    // pom2/disks/<game>/<disk file name>: a frontend finds a game's saves by
    // name (RetroBat asked, emulatorlauncher#1498). <game> is the content's
    // name — "Ultima IV" for Ultima IV.m3u, "Karateka" for Karateka.woz.
    const auto root = fs::weakly_canonical(fs::absolute(saves));
    ensureDirectories(root, access);
    auto disks = root / "pom2";
    ensureDirectory(disks, access);
    disks /= "disks";
    ensureDirectory(disks, access);
    auto gameName = (content.empty() ? original : content).stem().u8string();
    if (gameName.empty() || gameName == "." || gameName == "..") gameName = "disk";
    const auto directory = disks / fs::u8path(portableName(gameName));
    std::error_code probe;
    const auto legacy = legacyBacking(disks, content, original, sourceHash);
    const bool hasLegacy = fs::is_regular_file(fs::symlink_status(legacy, probe));
    // A restore (ExistingOnly) still makes the folder a legacy copy moves to.
    ensureDirectory(directory, hasLegacy ? DiskStorageAccess::CreateIfMissing : access);

    // The .source record keeps two games whose names match from sharing one
    // copy: the second one, or a new revision of the image, gets its own,
    // named with the start of its hash.
    const auto diskStem = portableName(original.stem().u8string());
    const auto diskExtension = portableExtension(original);
    auto backing = directory / fs::u8path(diskStem + diskExtension);
    if (usableBacking(backing)) {
        const auto recorded = recordedSource(backing);
        if (recorded.empty()) recordSource(backing, sourceHash);   // a copy that lost its record
        else if (recorded != sourceHash)
            backing = directory / fs::u8path(diskStem + " [" + sourceHash.substr(0, 8) + "]" + diskExtension);
    }
    const auto moveLegacy = [&] {
        std::error_code error;
        auto lock = legacy;
        lock += ".lock";
        fs::remove(lock, error);
        fs::remove(legacy.parent_path(), error);   // only when empty
    };
    if (hasLegacy && usableBacking(legacy) && usableBacking(backing)) {
        // v1.2.3 kept one copy per content LOCATION; the new name has one per
        // game. When a copy of this game already took the name (the same
        // image opened from another folder or another playlist), neither is
        // lost: the newer one plays, the other is kept beside it as
        // "<disk> [old <location hash>]" (bug hunt 2026-11-25).
        const auto kept = directory / fs::u8path(diskStem + " [old " +
            legacy.parent_path().filename().u8string().substr(0, 8) + "]" + diskExtension);
        std::error_code error;
        const bool legacyNewer = fs::last_write_time(legacy, error) > fs::last_write_time(backing, error);
        try {
            if (usableBacking(kept)) {
                // Moved once before; nothing to decide twice.
            } else if (legacyNewer) {
                renameWithRecord(backing, kept);
                fs::rename(legacy, backing);
                recordSource(backing, sourceHash);
            } else {
                fs::rename(legacy, kept);
                recordSource(kept, sourceHash);
            }
        } catch (const fs::filesystem_error& failure) {
            throw std::runtime_error("Cannot move disk save " + legacy.u8string() + ": " + failure.what());
        }
        moveLegacy();
    }
    if (!usableBacking(backing)) {
        if (hasLegacy && usableBacking(legacy)) {
            // Keep the player's progress: move the old copy to its new name.
            std::error_code error;
            fs::rename(legacy, backing, error);
            if (error)
                throw std::runtime_error("Cannot move disk save " + legacy.u8string() + " to " +
                                         backing.u8string() + ": " + error.message());
            moveLegacy();
        } else if (access == DiskStorageAccess::ExistingOnly) {
            throw std::runtime_error("Disk save does not exist: " + backing.u8string());
        } else {
            createBacking(backing, bytes);
        }
        recordSource(backing, sourceHash);
    }
    if (fs::equivalent(backing, original))
        throw std::runtime_error("Disk save cannot be the original disk image: " + original.u8string());
    return {backing, true};
}

} // namespace pom2::libretro
