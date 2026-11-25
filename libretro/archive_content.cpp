// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archive_content.hpp"

#include "AtomicFileReplace.h"
#include "ExchangeMedia.h"
#include "ZipMember.h"
#include "portable_name.hpp"
#include "pom2/core.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

// Nothing here throws (archive_content.hpp): every failure leaves `error`.
// And nothing here trusts the archive (bug hunt 2026-11-25): its names become
// portable leaves before they touch the file system (no folder, no drive
// letter, no device name, CP437 read as CP437), its sizes are capped in
// total, a member that cannot be used is skipped rather than failing the
// disks beside it, and the cache folder is written as the save folder is:
// no planted link is followed.

namespace pom2::libretro {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxMember = 40u << 20;      // a 32 MiB hard disk and its 2IMG header
constexpr std::size_t kMaxArchive = 256u << 20;    // the archive, read whole
constexpr std::size_t kMaxUnpacked = 256u << 20;   // every member, unpacked
constexpr std::size_t kMaxEntries = 4096;          // central-directory names looked at
constexpr std::size_t kMaxDisks = 64;              // disks kept from one archive
constexpr std::size_t kMaxPlaylistLines = 1024;

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

// The extension of a raw name, lowercased, from the bytes: no path is built
// from a name that may not be UTF-8.
std::string suffixOf(const std::string& name)
{
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

enum class Kind { None, Disk, Playlist, Program, Tape, Exchange };

Kind kindOf(const std::string& name)
{
    static const char* const kDisks[] = {".dsk", ".do", ".po", ".nib", ".woz", ".d13", ".2mg", ".2img", ".hdv"};
    const auto suffix = suffixOf(name);
    if (std::find(std::begin(kDisks), std::end(kDisks), suffix) != std::end(kDisks)) return Kind::Disk;
    if (suffix == ".m3u") return Kind::Playlist;
    if (suffix == ".bin" || suffix == ".bas") return Kind::Program;
    if (suffix == ".wav" || suffix == ".aci") return Kind::Tape;
    if (suffix == ".image" || pom2::exchange::hasExchangeExtension(name)) return Kind::Exchange;
    return Kind::None;
}

// A member name's last component, raw: no folders, no parent references.
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

// Case-blind, numbers by value: "Disk 2" before "Disk 10", "boot" before "Data".
bool naturalLess(const std::string& a, const std::string& b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const auto ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            auto na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = ei;
            j = ej;
            continue;
        }
        const auto la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::vector<std::uint8_t> readAll(const fs::path& path, std::string& error)
{
    std::error_code status;
    const auto size = fs::file_size(path, status);
    if (status || size > kMaxArchive) {
        error = status ? "Cannot read archive " + path.u8string() : path.filename().u8string() + " is too large";
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot read archive " + path.u8string();
        return {};
    }
    return {std::istreambuf_iterator<char>(file), {}};
}

// A directory of the cache, never a planted link (as disk_storage.cpp checks
// the save folder: a link would send the unpacked bytes anywhere).
bool safeDirectory(const fs::path& directory, std::string& error)
{
    std::error_code status;
    fs::create_directories(directory, status);
    const auto type = fs::symlink_status(directory, status);
    if (status || !fs::is_directory(type)) {
        error = "Cannot use the archive cache " + directory.u8string();
        return false;
    }
    return true;
}

// The unpacked file, as the archive's: a temporary beside it renamed over it,
// so a planted link at the name is replaced, never followed.
bool writeMember(const fs::path& path, const std::vector<std::uint8_t>& bytes, bool keepExisting,
                 std::string& error)
{
    std::error_code status;
    const auto type = fs::symlink_status(path, status);
    if (keepExisting && fs::is_regular_file(type)) {
        std::error_code links;
        if (fs::hard_link_count(path, links) == 1 && !links) return true;
    }
    // A link planted at the name is removed, not followed: the temporary
    // would otherwise be made beside its target.
    if (fs::is_symlink(type)) fs::remove(path, status);
    const auto temporary = tempSiblingPath(path);
    if (!prepareTempPath(temporary, status)) {
        error = "Cannot unpack to " + path.u8string() + ": " + status.message();
        return false;
    }
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "Cannot unpack to " + path.u8string();
            fs::remove(temporary, status);
            return false;
        }
    }
    fs::rename(temporary, path, status);
    if (status) {
        error = "Cannot unpack to " + path.u8string() + ": " + status.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

// One archive's unpacking: the names it claims, what it wrote, the totals.
struct Unpack {
    fs::path folder;
    bool unchanged = false;                 // the archive is the one unpacked here before
    std::map<std::string, unsigned> next;   // lowercased leaf -> next number
    std::set<std::string> taken;            // lowercased leaves written
    std::size_t unpacked = 0;
    std::string firstFailure;

    // A unique leaf: "GAME.DSK", then "GAME (2).DSK", "GAME (3).DSK"...
    std::string claim(const std::string& leaf)
    {
        const auto dot = leaf.find_last_of('.');
        const auto stem = dot == std::string::npos || dot == 0 ? leaf : leaf.substr(0, dot);
        const auto extension = dot == std::string::npos || dot == 0 ? std::string() : leaf.substr(dot);
        auto& n = next[lower(leaf)];
        std::string name = leaf;
        while (!taken.insert(lower(name)).second)
            name = stem + " (" + std::to_string(++n + 1) + ")" + extension;
        return name;
    }

    // `fresh`: written whatever is there (a playlist, which the load
    // rewrites: kept, the next load would re-read its own output).
    bool write(const std::string& leaf, const std::vector<std::uint8_t>& bytes, std::string& error,
               bool fresh = false)
    {
        unpacked += bytes.size();
        if (unpacked > kMaxUnpacked) {
            error = "the archive unpacks to more than 256 MiB";
            return false;
        }
        // An archive unpacked here before keeps its files: with no save
        // folder a disk is played in place, and its progress is there.
        return writeMember(folder / fs::u8path(leaf), bytes, unchanged && !fresh, error);
    }
};

// A ShrinkIt archive, a DiskCopy image or a .image: the disks it holds,
// written into the folder. A .image that is not DiskCopy 4.2 is a raw block
// image, which the drives take as a .po.
std::vector<fs::path> convertExchange(Unpack& unpack, const std::string& name, const std::vector<std::uint8_t>& bytes,
                                      std::string& error, std::vector<std::string>* rawNames = nullptr)
{
    std::vector<pom2::exchange::Image> images;
    std::string note, failure;
    if (!pom2::exchange::convert(name, bytes, images, note, failure)) {
        const bool rawImage = suffixOf(name) == ".image" && !bytes.empty() && bytes.size() % 512 == 0 &&
                              bytes.size() <= kMaxMember;
        if (!rawImage) {
            error = hostLeafName(name) + ": " + failure;
            return {};
        }
        const auto dot = name.find_last_of('.');
        images.push_back({name.substr(0, dot) + ".po", bytes});
    }
    std::vector<fs::path> paths;
    for (const auto& image : images) {
        const auto leaf = unpack.claim(hostSafeLeaf(image.name));
        if (!unpack.write(leaf, image.bytes, error)) return {};
        paths.push_back(unpack.folder / fs::u8path(leaf));
        if (rawNames) rawNames->push_back(image.name);
    }
    return paths;
}

} // namespace

bool isArchive(const std::filesystem::path& path)
{
    const auto name = path.filename().u8string();
    const auto suffix = suffixOf(name);
    return suffix == ".zip" || suffix == ".gz" || kindOf(name) == Kind::Exchange;
}

std::filesystem::path unpackArchive(const std::filesystem::path& archive, const std::filesystem::path& cacheRoot,
                                    std::string& error, ArchiveNotes* notes)
{
    error.clear();
    if (notes) *notes = {};
    std::error_code status;
    const auto absolute = fs::absolute(archive, status).u8string();
    const auto digest = pom2::Core::sha256(reinterpret_cast<const std::uint8_t*>(absolute.data()),
                                           absolute.size());
    const auto bytes = readAll(archive, error);
    if (!error.empty()) return {};

    Unpack unpack;
    unpack.folder = cacheRoot / digest.substr(0, 24);
    // The folders this code owns are checked for planted links; the one
    // above them is the player's (a symlinked system or saves "pom2" folder
    // is theirs to make).
    if (!safeDirectory(cacheRoot, error) || !safeDirectory(unpack.folder, error)) return {};
    // Which archive was unpacked here: the same bytes keep their files (and
    // any progress made in them), new bytes unpack afresh.
    // The marker is removed before the first file is written and written
    // back last: an unpack cut short never leaves another archive's files
    // certified. Its "2" is this unpacker's format: a later fix to a
    // conversion unpacks again.
    const auto archiveHash = "2 " + pom2::Core::sha256(bytes.data(), bytes.size());
    const auto marker = unpack.folder / ".archive";
    {
        std::error_code links;
        const bool regular = fs::is_regular_file(fs::symlink_status(marker, status)) &&
                             fs::hard_link_count(marker, links) == 1 && !links;
        std::string recorded;
        if (regular) {
            std::ifstream in(marker, std::ios::binary);
            char buffer[128] = {};
            in.read(buffer, sizeof buffer - 1);
            recorded.assign(buffer, static_cast<std::size_t>(in.gcount()));
            recorded = trimmed(recorded.substr(0, recorded.find('\n')));
        }
        unpack.unchanged = regular && recorded == archiveHash;
        if (!unpack.unchanged) {
            fs::remove(marker, status);
            if (fs::exists(fs::symlink_status(marker, status))) {
                error = "Cannot use the archive cache " + unpack.folder.u8string();
                return {};
            }
        }
    }
    // Where libretro v1.2.3 unpacked each file (the member's raw name, a
    // .gz's stored name first): its disk saves are found by those paths.
    const auto legacyPath = [&](const std::string& rawLeaf) {
        try {
            return unpack.folder / fs::u8path(rawLeaf);
        } catch (const std::exception&) {   // Windows, a name that is not UTF-8: v1.2.3 could not load it
            return fs::path();
        }
    };
    const auto noteLegacy = [&](const fs::path& now, const std::string& rawLeaf) {
        if (!notes) return;
        const auto old = legacyPath(rawLeaf);
        if (!old.empty() && old != now) notes->legacy[now] = old;
    };

    std::vector<fs::path> disks, programs, tapes;
    std::vector<std::pair<fs::path, std::string>> playlists;   // written copy, member path
    std::map<std::string, std::string> written;                // member key -> leaf written
    const auto skip = [&unpack](const std::string& failure) {
        if (unpack.firstFailure.empty()) unpack.firstFailure = failure;
    };
    const auto keep = [&](Kind kind, const fs::path& path, const std::string& member) {
        if (kind == Kind::Disk) disks.push_back(path);
        else if (kind == Kind::Playlist) playlists.emplace_back(path, member);
        else if (kind == Kind::Program) programs.push_back(path);
        else if (kind == Kind::Tape) tapes.push_back(path);
    };

    const auto archiveName = archive.filename().u8string();
    if (kindOf(archiveName) == Kind::Exchange) {
        std::vector<std::string> raw;
        disks = convertExchange(unpack, archiveName, bytes, error, &raw);
        if (!error.empty()) return {};
        for (std::size_t i = 0; i < disks.size(); ++i) noteLegacy(disks[i], raw[i]);
    } else if (suffixOf(archiveName) == ".gz") {
        std::vector<std::uint8_t> out;
        std::string name, failure;
        if (!pom2::gunzip(bytes, out, failure, kMaxMember, &name)) {
            error = hostLeafName(archiveName) + ": " + failure;
            return {};
        }
        // The archive's own name first ("Game.dsk.gz" is Game.dsk, renamed or
        // not), then the name stored inside it.
        auto leaf = archive.stem().u8string();
        if (kindOf(leaf) == Kind::None) leaf = leafName(name);
        // No usable name at all (gzip -n, a renamed file): a floppy's size
        // says what it is.
        if (kindOf(leaf) == Kind::None && out.size() == 143360) leaf = archive.stem().u8string() + ".dsk";
        if (kindOf(leaf) == Kind::None && (out.size() == 409600 || out.size() == 819200))
            leaf = archive.stem().u8string() + ".po";
        // v1.2.3 took the stored name first.
        const auto legacyLeaf = leafName(name).empty() ? archive.stem().u8string() : leafName(name);
        const auto kind = kindOf(leaf);
        if (kind == Kind::Exchange) {
            std::vector<std::string> raw;
            disks = convertExchange(unpack, leaf, out, error, &raw);   // "Game.shk.gz"
            if (!error.empty()) return {};
            for (std::size_t i = 0; i < disks.size(); ++i) noteLegacy(disks[i], raw[i]);
        } else if (kind == Kind::None || kind == Kind::Playlist) {
            error = hostLeafName(archiveName) + " holds no Apple II disk image";
            return {};
        } else {
            const auto stored = unpack.claim(hostSafeLeaf(leaf));
            if (!unpack.write(stored, out, error)) return {};
            keep(kind, unpack.folder / fs::u8path(stored), leaf);
            noteLegacy(unpack.folder / fs::u8path(stored), legacyLeaf);
        }
    } else {
        std::vector<pom2::ZipEntry> entries;
        std::string failure;
        if (!pom2::listZipEntries(bytes, entries, failure)) {
            error = hostLeafName(archiveName) + ": " + failure;
            return {};
        }
        if (entries.size() > kMaxEntries) {
            error = hostLeafName(archiveName) + " holds too many files";
            return {};
        }
        // By path, so the same members give the same names however the zip
        // orders them ("GAME.DSK" and "GAME (2).DSK" do not swap on a repack).
        std::stable_sort(entries.begin(), entries.end(), [](const pom2::ZipEntry& a, const pom2::ZipEntry& b) {
            const auto ka = memberKey(a.name), kb = memberKey(b.name);
            return ka != kb ? ka < kb : a.name < b.name;
        });
        for (const auto& entry : entries) {
            const auto leaf = leafName(entry.name);
            const auto kind = kindOf(leaf);
            if (leaf.empty() || leaf.front() == '.' || kind == Kind::None) continue;
            if ((kind == Kind::Disk || kind == Kind::Exchange) && disks.size() >= kMaxDisks) {
                skip(hostLeafName(archiveName) + " holds more than 64 disks: the others were left out");
                continue;
            }
            std::vector<std::uint8_t> out;
            if (!pom2::readZipEntry(bytes, entry, out, failure, kMaxMember)) {
                skip(hostLeafName(leaf) + ": " + failure);   // an extra that cannot be read: the disks still load
                continue;
            }
            if (kind == Kind::Exchange) {
                std::string converted;
                std::vector<std::string> raw;
                auto paths = convertExchange(unpack, leaf, out, converted, &raw);
                if (!converted.empty()) {
                    if (unpack.unpacked > kMaxUnpacked) { error = converted; return {}; }
                    skip(converted);
                    continue;
                }
                if (!paths.empty()) written[memberKey(entry.name)] = paths.front().filename().u8string();
                for (std::size_t i = 0; i < paths.size(); ++i) noteLegacy(paths[i], raw[i]);
                for (auto& path : paths) disks.push_back(std::move(path));
                continue;
            }
            const auto name = unpack.claim(hostSafeLeaf(leaf));
            if (!unpack.write(name, out, error, kind == Kind::Playlist)) return {};
            written[memberKey(entry.name)] = name;
            keep(kind, unpack.folder / fs::u8path(name), entry.name);
            noteLegacy(unpack.folder / fs::u8path(name), leaf);
        }
    }

    // The playlist written for several disks: its name claimed before the
    // cleanup below, and v1.2.3's ("<archive>.m3u") noted for its saves.
    const auto generatedList = unpack.folder / fs::u8path(unpack.claim(hostSafeLeaf(archive.stem().u8string() + ".m3u")));
    noteLegacy(generatedList, archive.stem().u8string() + ".m3u");
    // Files a previous archive at this path left, which this one did not
    // write. Kept: what the core itself leaves beside a disk played in place
    // (a diverted ".guest-" copy, a ".recovered" surface, a temporary of a
    // write in flight, locks and records).
    {
        std::vector<fs::path> stale;
        for (fs::directory_iterator it(unpack.folder, status), end; !status && it != end; it.increment(status)) {
            const auto name = it->path().filename().u8string();
            std::error_code ignored;
            const bool ours = name == ".archive" || unpack.taken.count(lower(name)) ||
                              name.find(".guest-") != std::string::npos || name.find(".recovered") != std::string::npos ||
                              name.find(".pom2tmp") != std::string::npos || suffixOf(name) == ".lock" ||
                              suffixOf(name) == ".source";
            if (!ours && fs::is_regular_file(it->symlink_status(ignored))) stale.push_back(it->path());
        }
        for (const auto& path : stale) fs::remove(path, status);
    }
    if (notes && !unpack.firstFailure.empty()) notes->notice = unpack.firstFailure;
    if (!unpack.unchanged) {
        std::vector<std::uint8_t> record(archiveHash.begin(), archiveHash.end());
        record.push_back('\n');
        writeMember(marker, record, false, error);
        error.clear();   // only an optimisation: a missing marker unpacks again next time
    }

    // The zip's own playlist, when it names disks the zip holds: its entries
    // become the names they were written under, "./" first so that a name
    // starting with '#' or a blank is a disk, not a comment.
    if (!playlists.empty()) {
        const auto& [path, member] = playlists.front();
        const auto slash = member.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? std::string() : member.substr(0, slash + 1);
        std::ifstream in(path, std::ios::binary);
        std::string line, rewritten, pendingInfo;
        std::size_t mapped = 0, lines = 0;
        for (bool first = true; std::getline(in, line) && ++lines <= kMaxPlaylistLines; first = false) {
            if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
            const auto bar = line.find('|');
            const auto entry = trimmed(line.substr(0, bar));
            if (entry.empty()) continue;
            if (entry.compare(0, 8, "#EXTINF:") == 0) { pendingInfo = trimmed(line) + "\n"; continue; }
            if (entry[0] == '#') { rewritten += trimmed(line) + "\n"; continue; }
            const auto found = written.find(memberKey(base + entry));
            if (found == written.end() || kindOf(found->second) != Kind::Disk) {
                pendingInfo.clear();   // a disk the zip does not hold: left out, with its label
                continue;
            }
            rewritten += pendingInfo + "./" + found->second +
                         (bar == std::string::npos ? std::string() : trimmed(line.substr(bar))) + "\n";
            pendingInfo.clear();
            ++mapped;
        }
        in.close();
        if (mapped) {
            if (!writeMember(path, std::vector<std::uint8_t>(rewritten.begin(), rewritten.end()), false, error))
                return {};
            return path;
        }
        // A playlist that names nothing the zip holds: the disks themselves.
    }
    if (disks.empty()) {
        if (!programs.empty() || !tapes.empty()) {
            auto& found = programs.empty() ? tapes : programs;
            std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
                return naturalLess(a.filename().u8string(), b.filename().u8string());
            });
            return found.front();
        }
        error = unpack.firstFailure.empty() ? hostLeafName(archiveName) + " holds no Apple II disk image"
                                            : unpack.firstFailure;
        return {};
    }
    if (disks.size() == 1) return disks.front();
    // Several disks: a playlist in natural order, so Disc Control can swap them.
    std::sort(disks.begin(), disks.end(), [](const fs::path& a, const fs::path& b) {
        return naturalLess(a.filename().u8string(), b.filename().u8string());
    });
    disks.erase(std::unique(disks.begin(), disks.end()), disks.end());
    std::string playlist;
    for (const auto& disk : disks) playlist += "./" + disk.filename().u8string() + "\n";
    if (!writeMember(generatedList, std::vector<std::uint8_t>(playlist.begin(), playlist.end()), false, error))
        return {};
    return generatedList;
}

} // namespace pom2::libretro
