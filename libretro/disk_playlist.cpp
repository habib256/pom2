// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "disk_playlist.hpp"
#include "TwoImgHeader.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace pom2::libretro {
namespace {

std::string extension(const std::filesystem::path& path)
{
    std::string suffix = path.extension().u8string();
    for (char& c : suffix)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return suffix;
}

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\v\f";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::ifstream openFile(const std::filesystem::path& path, const std::string& location)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw std::runtime_error(location + ": expected a regular file: " + path.u8string());

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error(location + ": cannot open " + path.u8string());
    return stream;
}

DiskEntry diskEntry(std::filesystem::path path, std::string_view label,
                    const std::string& location)
{
    const auto suffix = extension(path);
    if (suffix == ".m3u")
        throw std::runtime_error(location + ": nested playlists are unsupported: " + path.u8string());
    if (suffix != ".dsk" && suffix != ".do" && suffix != ".po"
            && suffix != ".nib" && suffix != ".woz" && suffix != ".2mg" && suffix != ".2img"
            && suffix != ".hdv" && suffix != ".d13")
        throw std::runtime_error(location + ": unsupported disk image: " + path.u8string());

    openFile(path, location);
    std::string name = label.empty() ? path.stem().u8string() : std::string(label);
    return {std::move(path), std::move(name)};
}

// Namespace scope, not locals: MSVC will not let the lambda below read a
// local constexpr without capturing it.
constexpr std::uint64_t k35Small = 400 * 1024, k35Large = 800 * 1024, k525 = 140 * 1024;

} // namespace

MediaKind DiskEntry::kind() const
{
    const auto bySize = [](std::uint64_t bytes) {
        if (bytes == k35Small || bytes == k35Large) return MediaKind::SmartPort;
        return bytes > k35Large ? MediaKind::HardDisk : MediaKind::DiskII;
    };
    const auto suffix = extension(path);
    if (suffix == ".hdv") return MediaKind::HardDisk;
    if (suffix == ".2mg" || suffix == ".2img") {
        std::array<std::uint8_t, 32> header{};
        std::ifstream image(path, std::ios::binary);
        if (!image.read(reinterpret_cast<char*>(header.data()), header.size())
                || std::string_view(reinterpret_cast<const char*>(header.data()), 4) != "2IMG")
            return MediaKind::DiskII;
        const std::uint64_t payload = twoImgDataLength(header.data());
        return payload == k525 ? MediaKind::DiskII : bySize(payload);
    }
    if (suffix != ".po") return MediaKind::DiskII;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? MediaKind::DiskII : bySize(bytes);
}

DiskEntry loadDiskImage(const std::filesystem::path& path)
{
    if (path.empty()) throw std::runtime_error("A disk image path is required.");
    const auto source = std::filesystem::absolute(path);
    if (source.u8string().find('\0') != std::string::npos)
        throw std::runtime_error("The content path contains a null byte.");
    return diskEntry(source, {}, "Disk image");
}

std::vector<DiskEntry> loadDiskList(const std::filesystem::path& content)
{
    if (content.empty())
        throw std::runtime_error("A disk image or M3U playlist path is required.");

    const auto source = std::filesystem::absolute(content);
    const auto location = source.u8string();
    if (location.find('\0') != std::string::npos)
        throw std::runtime_error("The content path contains a null byte.");
    if (extension(source) != ".m3u")
        return {diskEntry(source, {}, "Disk image")};

    auto stream = openFile(source, "M3U playlist");
    std::vector<DiskEntry> disks;
    std::string line;
    std::string pendingLabel;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        std::string_view entry = line;
        if (lineNumber == 1 && entry.substr(0, 3) == "\xef\xbb\xbf")
            entry.remove_prefix(3);
        entry = trim(entry);
        const auto entryLocation = location + ":" + std::to_string(lineNumber);
        if (entry.find('\0') != std::string_view::npos)
            throw std::runtime_error(entryLocation + ": unexpected null byte");
        constexpr std::string_view extinf = "#EXTINF:";
        if (entry.substr(0, extinf.size()) == extinf) {
            const auto comma = entry.find(',', extinf.size());
            pendingLabel = comma == std::string_view::npos
                ? std::string_view{} : trim(entry.substr(comma + 1));
            continue;
        }
        if (entry.empty() || entry.front() == '#')
            continue;

        const auto separator = entry.find('|');
        const auto filename = trim(entry.substr(0, separator));
        if (filename.empty())
            throw std::runtime_error(entryLocation + ": missing disk image path");
        const auto label = separator == std::string_view::npos
            ? std::string_view(pendingLabel) : trim(entry.substr(separator + 1));
        auto path = std::filesystem::u8path(filename);
        if (!path.is_absolute())
            path = source.parent_path() / path;
        if (!path.is_absolute())
            throw std::runtime_error(entryLocation + ": cannot resolve disk image path");
        disks.push_back(diskEntry(std::move(path), label, entryLocation));
        pendingLabel.clear();
    }
    if (stream.bad() || !stream.eof())
        throw std::runtime_error(location + ": error reading playlist");
    if (disks.empty())
        throw std::runtime_error(location + ": playlist contains no disk images");
    return disks;
}

} // namespace pom2::libretro
