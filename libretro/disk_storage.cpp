// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk_storage.hpp"

#include "AtomicFileReplace.h"
#include "pom2/core.hpp"

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
        throw std::runtime_error("Cannot use disk save directory " + path.string() +
                                 (error ? ": " + error.message() : ""));
}

std::vector<std::uint8_t> readOriginal(const fs::path& original)
{
    std::ifstream input(original, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read disk " + original.string());
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxDiskBytes)
        throw std::runtime_error("Disk image is too large: " + original.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read disk " + original.string());
    return bytes;
}

bool usableBacking(const fs::path& backing)
{
    std::error_code error;
    const auto status = fs::symlink_status(backing, error);
    if (status.type() == fs::file_type::not_found) return false;
    if (error || !fs::is_regular_file(status) || fs::hard_link_count(backing) != 1)
        throw std::runtime_error("Disk save must be a private regular file: " + backing.string());
    return true;
}

void createBacking(const fs::path& backing, const std::vector<std::uint8_t>& bytes)
{
    const auto temporary = tempSiblingPath(backing);
    std::error_code error;
    if (!prepareTempPath(temporary, error))
        throw std::runtime_error("Cannot prepare disk save " + backing.string() + ": " + error.message());
    std::ofstream copy(temporary, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        copy.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    copy.close();
    if (copy && publishNewFileAtomic(temporary, backing, error)) return;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    // Another instance can create the same backing first. Keep its version.
    if (error == std::errc::file_exists && usableBacking(backing)) return;
    throw std::runtime_error("Cannot create disk save " + backing.string() +
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

std::string recordedSource(const fs::path& backing)
{
    std::ifstream input(sourceRecord(backing));
    std::string line;
    std::getline(input, line);
    return line;
}

void recordSource(const fs::path& backing, const std::string& sourceHash)
{
    std::ofstream output(sourceRecord(backing), std::ios::trunc);
    output << sourceHash << '\n';
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
    auto game = (content.empty() ? original : content).stem();
    if (game.empty() || game == "." || game == "..") game = "disk";
    const auto directory = disks / game;
    std::error_code probe;
    const auto legacy = legacyBacking(disks, content, original, sourceHash);
    const bool hasLegacy = fs::is_regular_file(fs::symlink_status(legacy, probe));
    // A restore (ExistingOnly) still makes the folder a legacy copy moves to.
    ensureDirectory(directory, hasLegacy ? DiskStorageAccess::CreateIfMissing : access);

    // The .source record keeps two games whose names match from sharing one
    // copy: the second one, or a new revision of the image, gets its own,
    // named with the start of its hash.
    auto backing = directory / original.filename();
    if (usableBacking(backing)) {
        const auto recorded = recordedSource(backing);
        if (recorded.empty()) recordSource(backing, sourceHash);   // a copy that lost its record
        else if (recorded != sourceHash)
            backing = directory / (original.stem().u8string() + " [" + sourceHash.substr(0, 8) + "]" +
                                   original.extension().u8string());
    }
    if (!usableBacking(backing)) {
        std::error_code error;
        if (hasLegacy && usableBacking(legacy)) {
            // Keep the player's progress: move the old copy to its new name.
            fs::rename(legacy, backing, error);
            if (error)
                throw std::runtime_error("Cannot move disk save " + legacy.string() + " to " + backing.string() +
                                         ": " + error.message());
            auto lock = legacy;
            lock += ".lock";
            fs::remove(lock, error);
            fs::remove(legacy.parent_path(), error);   // only when empty
        } else if (access == DiskStorageAccess::ExistingOnly) {
            throw std::runtime_error("Disk save does not exist: " + backing.string());
        } else {
            createBacking(backing, bytes);
        }
        recordSource(backing, sourceHash);
    }
    if (fs::equivalent(backing, original))
        throw std::runtime_error("Disk save cannot be the original disk image: " + original.string());
    return {backing, true};
}

} // namespace pom2::libretro
