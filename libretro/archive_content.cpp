// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archive_content.hpp"

#include "ZipMember.h"
#include "pom2/core.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace pom2::libretro {
namespace {

constexpr std::size_t kMaxMember = 40u << 20;   // a 32 MiB hard disk and its 2IMG header

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

bool loadable(const std::filesystem::path& name)
{
    static const char* const kExtensions[] = {".dsk", ".do", ".po", ".nib", ".woz", ".d13", ".2mg",
                                              ".2img", ".hdv", ".m3u", ".bin", ".bas"};
    const auto suffix = lower(name.extension().string());
    return std::find(std::begin(kExtensions), std::end(kExtensions), suffix) != std::end(kExtensions);
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read archive " + path.u8string());
    return {std::istreambuf_iterator<char>(file), {}};
}

void writeIfChanged(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    // The extracted file is the "original" a private copy is made from:
    // leave it alone when it already holds these bytes.
    std::error_code error;
    if (std::filesystem::file_size(path, error) == bytes.size() && !error && readAll(path) == bytes) return;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Cannot unpack to " + path.u8string());
}

// A member name as a safe leaf: no folders, no parent references.
std::string leafName(const std::string& member)
{
    const auto slash = member.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? member : member.substr(slash + 1);
    if (leaf.empty() || leaf == "." || leaf == "..") return {};
    return leaf;
}

} // namespace

bool isArchive(const std::filesystem::path& path)
{
    const auto suffix = lower(path.extension().string());
    return suffix == ".zip" || suffix == ".gz";
}

std::filesystem::path unpackArchive(const std::filesystem::path& archive, const std::filesystem::path& cacheRoot)
{
    const auto absolute = std::filesystem::absolute(archive).u8string();
    const auto digest = pom2::Core::sha256(reinterpret_cast<const std::uint8_t*>(absolute.data()),
                                           absolute.size());
    const auto folder = cacheRoot / digest.substr(0, 24);
    const auto bytes = readAll(archive);
    std::string error;

    if (lower(archive.extension().string()) == ".gz") {
        std::vector<std::uint8_t> out;
        std::string name;
        if (!pom2::gunzip(bytes, out, error, kMaxMember, &name))
            throw std::runtime_error(archive.filename().u8string() + ": " + error);
        auto leaf = leafName(name);
        if (leaf.empty()) leaf = archive.stem().u8string();   // "Game.dsk.gz" -> "Game.dsk"
        if (!loadable(leaf)) throw std::runtime_error(archive.filename().u8string() + " holds no Apple II disk image");
        writeIfChanged(folder / leaf, out);
        return folder / leaf;
    }

    std::vector<pom2::ZipEntry> entries;
    if (!pom2::listZipEntries(bytes, entries, error))
        throw std::runtime_error(archive.filename().u8string() + ": " + error);
    std::vector<std::filesystem::path> disks, playlists;
    for (const auto& entry : entries) {
        const auto leaf = leafName(entry.name);
        if (leaf.empty() || leaf.front() == '.' || !loadable(leaf)) continue;
        std::vector<std::uint8_t> out;
        if (!pom2::readZipEntry(bytes, entry, out, error, kMaxMember))
            throw std::runtime_error(archive.filename().u8string() + ": " + entry.name + ": " + error);
        writeIfChanged(folder / leaf, out);
        (lower(std::filesystem::path(leaf).extension().string()) == ".m3u" ? playlists : disks)
            .push_back(folder / leaf);
    }
    if (!playlists.empty()) return playlists.front();
    if (disks.empty()) throw std::runtime_error(archive.filename().u8string() + " holds no Apple II disk image");
    if (disks.size() == 1) return disks.front();
    // Several disks: a playlist in name order, so Disc Control can swap them.
    std::sort(disks.begin(), disks.end());
    std::string playlist;
    for (const auto& disk : disks) playlist += disk.filename().u8string() + "\n";
    const auto path = folder / (archive.stem().u8string() + ".m3u");
    writeIfChanged(path, std::vector<std::uint8_t>(playlist.begin(), playlist.end()));
    return path;
}

} // namespace pom2::libretro
