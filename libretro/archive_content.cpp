// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archive_content.hpp"

#include "ExchangeMedia.h"
#include "ZipMember.h"
#include "pom2/core.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <map>
#include <string>
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
    const auto suffix = lower(name.extension().u8string());
    return std::find(std::begin(kExtensions), std::end(kExtensions), suffix) != std::end(kExtensions);
}

// Nothing here throws (archive_content.hpp): every failure leaves `error`.
std::vector<std::uint8_t> readAll(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot read archive " + path.u8string();
        return {};
    }
    return {std::istreambuf_iterator<char>(file), {}};
}

void writeIfChanged(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error)
{
    // The extracted file is the "original" a private copy is made from:
    // leave it alone when it already holds these bytes.
    std::error_code status;
    if (std::filesystem::file_size(path, status) == bytes.size() && !status) {
        std::string unread;
        if (readAll(path, unread) == bytes && unread.empty()) return;
    }
    std::filesystem::create_directories(path.parent_path(), status);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) error = "Cannot unpack to " + path.u8string();
}

// A ShrinkIt archive, a DiskCopy image or a .image: the disks it holds,
// written into `folder`. A .image that is not DiskCopy 4.2 is a raw block
// image, which the drives take as a .po.
std::vector<std::filesystem::path> convertExchange(const std::string& name, const std::vector<std::uint8_t>& bytes,
                                                   const std::filesystem::path& folder, std::string& error)
{
    std::vector<pom2::exchange::Image> images;
    std::string note, failure;
    if (!pom2::exchange::convert(name, bytes, images, note, failure)) {
        const bool rawImage = lower(std::filesystem::u8path(name).extension().u8string()) == ".image"
            && !bytes.empty() && bytes.size() % 512 == 0 && bytes.size() <= kMaxMember;
        if (!rawImage) {
            error = name + ": " + failure;
            return {};
        }
        images.push_back({std::filesystem::u8path(name).stem().u8string() + ".po", bytes});
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& image : images) {
        writeIfChanged(folder / std::filesystem::u8path(image.name), image.bytes, error);
        if (!error.empty()) return {};
        paths.push_back(folder / std::filesystem::u8path(image.name));
    }
    return paths;
}

bool exchangeName(const std::string& leaf)
{
    return pom2::exchange::hasExchangeExtension(leaf) || lower(std::filesystem::u8path(leaf).extension().u8string()) == ".image";
}

// A member name as a safe leaf: no folders, no parent references.
std::string leafName(const std::string& member)
{
    const auto slash = member.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? member : member.substr(slash + 1);
    if (leaf.empty() || leaf == "." || leaf == "..") return {};
    return leaf;
}

// A member path with '/' separators, "." and ".." resolved, lowercased: the
// key a playlist inside the zip finds its disks by.
std::string memberKey(const std::string& path)
{
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path + "/") {
        if (c != '/' && c != '\\') { part += c; continue; }
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        part.clear();
    }
    std::string key;
    for (const auto& p : parts) key += (key.empty() ? "" : "/") + p;
    return lower(key);
}

std::string trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

} // namespace

bool isArchive(const std::filesystem::path& path)
{
    const auto suffix = lower(path.extension().u8string());
    return suffix == ".zip" || suffix == ".gz" || exchangeName(path.filename().u8string());
}

std::filesystem::path unpackArchive(const std::filesystem::path& archive, const std::filesystem::path& cacheRoot,
                                    std::string& error)
{
    error.clear();
    std::error_code status;
    const auto absolute = std::filesystem::absolute(archive, status).u8string();
    const auto digest = pom2::Core::sha256(reinterpret_cast<const std::uint8_t*>(absolute.data()),
                                           absolute.size());
    const auto folder = cacheRoot / digest.substr(0, 24);
    const auto bytes = readAll(archive, error);
    if (!error.empty()) return {};
    const auto noDisk = [&] {
        error = archive.filename().u8string() + " holds no Apple II disk image";
        return std::filesystem::path{};
    };

    std::vector<std::filesystem::path> disks;
    std::vector<std::pair<std::filesystem::path, std::string>> playlists;   // written copy, member path
    std::map<std::string, std::string> written;                             // member key -> leaf written
    if (exchangeName(archive.filename().u8string())) {
        disks = convertExchange(archive.filename().u8string(), bytes, folder, error);
        if (!error.empty()) return {};
    } else if (lower(archive.extension().u8string()) == ".gz") {
        std::vector<std::uint8_t> out;
        std::string name, failure;
        if (!pom2::gunzip(bytes, out, failure, kMaxMember, &name)) {
            error = archive.filename().u8string() + ": " + failure;
            return {};
        }
        auto leaf = leafName(name);
        if (leaf.empty()) leaf = archive.stem().u8string();   // "Game.dsk.gz" -> "Game.dsk"
        if (exchangeName(leaf)) {
            disks = convertExchange(leaf, out, folder, error);   // "Game.shk.gz"
            if (!error.empty()) return {};
        } else {
            if (!loadable(std::filesystem::u8path(leaf))) return noDisk();
            writeIfChanged(folder / std::filesystem::u8path(leaf), out, error);
            if (!error.empty()) return {};
            return folder / std::filesystem::u8path(leaf);
        }
    } else {
        std::vector<pom2::ZipEntry> entries;
        std::string failure;
        if (!pom2::listZipEntries(bytes, entries, failure)) {
            error = archive.filename().u8string() + ": " + failure;
            return {};
        }
        // Two members with one leaf name ("Side A/GAME.DSK", "Side B/GAME.DSK",
        // or GAME.DSK and game.dsk on a case-blind disk) would overwrite each
        // other in the flat folder: the second becomes "GAME (2).DSK".
        std::set<std::string> taken;
        for (const auto& entry : entries) {
            auto leaf = leafName(entry.name);
            if (leaf.empty() || leaf.front() == '.' || !(loadable(std::filesystem::u8path(leaf)) || exchangeName(leaf)))
                continue;
            std::vector<std::uint8_t> out;
            if (!pom2::readZipEntry(bytes, entry, out, failure, kMaxMember)) {
                error = archive.filename().u8string() + ": " + entry.name + ": " + failure;
                return {};
            }
            if (exchangeName(leaf)) {
                auto converted = convertExchange(leaf, out, folder, error);
                if (!error.empty()) return {};
                for (auto& disk : converted) disks.push_back(std::move(disk));
                continue;
            }
            const auto base = std::filesystem::u8path(leaf);
            for (int n = 2; !taken.insert(lower(leaf)).second; ++n)
                leaf = base.stem().u8string() + " (" + std::to_string(n) + ")" + base.extension().u8string();
            const auto path = folder / std::filesystem::u8path(leaf);
            writeIfChanged(path, out, error);
            if (!error.empty()) return {};
            written[memberKey(entry.name)] = leaf;
            if (lower(base.extension().u8string()) == ".m3u") playlists.emplace_back(path, entry.name);
            else disks.push_back(path);
        }
    }
    if (!playlists.empty()) {
        // The zip's own playlist names its disks by their place in the zip
        // ("disks/A.dsk"); the folder is flat, so name them as written.
        const auto& [path, member] = playlists.front();
        const auto slash = member.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? std::string() : member.substr(0, slash + 1);
        std::ifstream in(path, std::ios::binary);
        std::string line, rewritten;
        for (bool first = true; std::getline(in, line); first = false) {
            if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
            const auto bar = line.find('|');
            const auto entry = trimmed(line.substr(0, bar));
            if (entry.empty() || entry[0] == '#') { rewritten += line + "\n"; continue; }
            const auto found = written.find(memberKey(base + entry));
            const auto name = found != written.end() ? found->second : leafName(entry);
            rewritten += name + (bar == std::string::npos ? std::string() : line.substr(bar)) + "\n";
        }
        in.close();
        writeIfChanged(path, std::vector<std::uint8_t>(rewritten.begin(), rewritten.end()), error);
        if (!error.empty()) return {};
        return path;
    }
    if (disks.empty()) return noDisk();
    if (disks.size() == 1) return disks.front();
    // Several disks: a playlist in name order, so Disc Control can swap them.
    std::sort(disks.begin(), disks.end());
    std::string playlist;
    for (const auto& disk : disks) playlist += disk.filename().u8string() + "\n";
    const auto path = folder / std::filesystem::u8path(archive.stem().u8string() + ".m3u");
    writeIfChanged(path, std::vector<std::uint8_t>(playlist.begin(), playlist.end()), error);
    if (!error.empty()) return {};
    return path;
}

} // namespace pom2::libretro
