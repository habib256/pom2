// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ExchangeMedia.h"

#include "A2rFlux.h"
#include "DiskBuilder.h"
#include "DiskCopy42.h"
#include "NuFxArchive.h"
#include "ProDOSDiskFs.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace pom2::exchange {
namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& name)
{
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(name.substr(dot));
}

std::string stemOf(const std::string& name)
{
    const auto slash = name.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
    const auto dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot > 0) leaf.erase(dot);
    return leaf.empty() ? std::string("DISK") : leaf;
}

// A host file name from a guest one: no path separators, nothing a host
// file system refuses.
std::string hostLeaf(const std::string& name)
{
    std::string out;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x20 || u >= 0x7F || std::string("/\\:*?\"<>|").find(c) != std::string::npos ? '_' : c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out.empty() ? std::string("DISK") : out;
}

const char* diskExtension(std::size_t bytes)
{
    // [B] ProDOS block images: 280 blocks a 5.25" disk, 1600 an 800 KB 3.5";
    //     larger ones are hard disks.
    return bytes > 1600u * 512u ? ".hdv" : ".po";
}

bool convertDiskCopy(const std::string& fileName, const std::vector<uint8_t>& bytes,
                     std::vector<Image>& images, std::string& note, std::string& error)
{
    std::vector<uint8_t> blocks;
    if (!diskcopy42::unwrap(bytes, blocks, error)) return false;
    images.push_back({hostLeaf(stemOf(fileName)) + ".po", std::move(blocks)});
    const auto name = diskcopy42::diskName(bytes);
    note = "DiskCopy 4.2 image" + (name.empty() ? std::string() : " \"" + name + "\"");
    return true;
}

// The files of a ShrinkIt archive on one ProDOS volume of `blocks` blocks.
// False with `full` set when they do not fit.
bool buildVolume(const std::vector<const nufx::Record*>& files, uint32_t blocks,
                 const std::string& volume, const std::string& folder,
                 std::vector<uint8_t>& image, bool& full, std::string& error)
{
    full = false;
    image = prodosfs::ProDOSDisk::format(blocks, volume, error);
    if (image.empty()) return false;
    prodosfs::ProDOSDisk disk;
    if (!disk.open(std::move(image), error)) return false;

    std::set<std::string> taken;      // full upper-case paths already on the volume
    auto unique = [&](const std::string& parent, std::string leaf) {
        std::string path = parent.empty() ? leaf : parent + "/" + leaf;
        for (int n = 2; taken.count(path); ++n) {
            const std::string suffix = "." + std::to_string(n);
            std::string base = leaf.substr(0, 15 - std::min<std::size_t>(15, suffix.size()));
            path = (parent.empty() ? "" : parent + "/") + base + suffix;
        }
        taken.insert(path);
        return path;
    };
    std::set<std::string> dirs;
    // Guest directory path -> volume path, so each component is renamed once.
    std::vector<std::pair<std::string, std::string>> dirMap;
    auto directory = [&](const std::vector<std::string>& comps, std::size_t count, std::string& out) {
        std::string guest, path;
        for (std::size_t i = 0; i < count; ++i) {
            guest += "/" + lower(comps[i]);
            auto it = std::find_if(dirMap.begin(), dirMap.end(), [&](const auto& m) { return m.first == guest; });
            if (it != dirMap.end()) { path = it->second; continue; }
            path = unique(path, prodosName(comps[i]));
            if (!disk.makeDirectory(path, error)) return false;
            dirMap.emplace_back(guest, path);
        }
        out = path;
        return true;
    };

    for (const auto* rec : files) {
        std::vector<std::string> comps;
        std::string part;
        for (char c : rec->path + "/") {
            if (c == '/') { if (!part.empty()) comps.push_back(part); part.clear(); }
            else part.push_back(c);
        }
        if (comps.empty()) comps.push_back("FILE");
        if (!folder.empty()) comps.insert(comps.begin(), folder);
        std::string parent;
        if (!directory(comps, rec->isDirectory ? comps.size() : comps.size() - 1, parent)) {
            full = error == "disk full";
            return false;
        }
        if (rec->isDirectory) continue;
        const std::string path = unique(parent, prodosName(comps.back()));
        if (!disk.writeFile(path, static_cast<uint8_t>(rec->fileType), static_cast<uint16_t>(rec->auxType),
                            rec->data, error)) {
            full = error == "disk full";
            return false;
        }
    }
    image = disk.image();
    // A 5.25" volume booted by mistake says so: POM2's own "NOT A STARTUP
    // DISK" sector in the first half of block 0, the half the Disk II PROM
    // loads (DiskBuilder.h). Larger volumes boot through a block device and
    // keep ProDOSDisk::format's empty block 0.
    if (blocks == 280) {
        const auto sector = diskbuild::notBootableSector();
        std::copy(sector.begin(), sector.end(), image.begin());
    }
    return true;
}

bool convertShrinkIt(const std::string& fileName, const std::vector<uint8_t>& bytes,
                     std::vector<Image>& images, std::string& note, std::string& error)
{
    std::vector<nufx::Record> records;
    if (!nufx::readArchive(bytes, records, error)) return false;
    std::vector<const nufx::Record*> files;
    int disks = 0, forks = 0;
    for (const auto& rec : records) {
        if (rec.isDisk) {
            ++disks;
            const std::string stem = rec.path.empty() ? stemOf(fileName) : stemOf(rec.path);
            images.push_back({hostLeaf(stem) + diskExtension(rec.data.size()), rec.data});
            continue;
        }
        if (rec.hasResourceFork) ++forks;
        files.push_back(&rec);
    }
    int fileCount = 0;
    for (const auto* f : files) fileCount += f->isDirectory ? 0 : 1;
    if (fileCount) {
        const std::string volume = prodosName(stemOf(fileName));
        // [B] ProDOS TRM § B.2.2: the volume directory is four blocks, 51
        //     entries, and cannot grow; a subdirectory can. An archive with
        //     more names at its top level goes into one folder.
        std::set<std::string> top;
        for (const auto* f : files) top.insert(prodosName(f->path.substr(0, f->path.find('/'))));
        const std::string folder = top.size() > 51 ? volume : std::string();
        std::vector<uint8_t> image;
        bool built = false;
        // The smallest disk that holds them: a 5.25" disk, an 800 KB one,
        // then hard disks up to ProDOS's 65 535 blocks.
        for (uint32_t blocks : {280u, 1600u, 4096u, 8192u, 16384u, 32768u, 65535u}) {
            bool full = false;
            if (buildVolume(files, blocks, volume, folder, image, full, error)) { built = true; break; }
            if (!full) return false;
        }
        if (!built) { error = "the archive's files do not fit on a ProDOS volume"; return false; }
        images.push_back({hostLeaf(stemOf(fileName)) + diskExtension(image.size()), std::move(image)});
    }
    if (images.empty()) { error = "the ShrinkIt archive holds no disk and no file"; return false; }

    note = "ShrinkIt archive:";
    if (disks) note += " " + std::to_string(disks) + (disks == 1 ? " disk" : " disks");
    if (disks && fileCount) note += " and";
    if (fileCount)
        note += " " + std::to_string(fileCount) + (fileCount == 1 ? " file" : " files")
              + " on a ProDOS data volume (no boot code: boot a ProDOS disk beside it)";
    if (forks)
        note += "; " + std::to_string(forks) + " GS/OS resource fork" + (forks == 1 ? "" : "s") + " left out";
    return true;
}

} // namespace

std::string prodosName(const std::string& name)
{
    // [B] ProDOS TRM § 2.1: a letter, then letters, digits or periods; 15 at most.
    std::string out;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c & 0x7F);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::toupper(u)));
        else if (!out.empty() && out.back() != '.') out.push_back('.');
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), 'A');
    if (out.size() > 15) out.resize(15);
    while (out.back() == '.') out.pop_back();
    return out;
}

bool hasExchangeExtension(const std::string& fileName)
{
    const std::string ext = extensionOf(fileName);
    for (const char* e : {".shk", ".sdk", ".bxy", ".sea", ".dc", ".dc42", ".a2r"})
        if (ext == e) return true;
    return false;
}

bool isExchangeFile(const std::string& fileName, const std::vector<uint8_t>& head)
{
    if (hasExchangeExtension(fileName)) return true;
    if (extensionOf(fileName) != ".image" || head.size() < diskcopy42::kHeaderBytes) return false;
    // The header only: the size check needs the whole file, so a .image is
    // taken when its $0100 signature and data size are DiskCopy's.
    const uint32_t data = (uint32_t(head[64]) << 24) | (uint32_t(head[65]) << 16) | (uint32_t(head[66]) << 8) | head[67];
    return head[82] == 0x01 && head[83] == 0x00 && head[0] <= 63
        && (data == 409600 || data == 819200 || data == 737280 || data == 1474560);
}

bool convert(const std::string& fileName, const std::vector<uint8_t>& bytes,
             std::vector<Image>& images, std::string& note, std::string& error)
{
    images.clear();
    note.clear();
    if (nufx::looksLikeNuFx(bytes)) return convertShrinkIt(fileName, bytes, images, note, error);
    if (a2r::looksLikeA2r(bytes)) {
        std::vector<uint8_t> woz;
        if (!a2r::toWoz(bytes, woz, note, error)) return false;
        images.push_back({hostLeaf(stemOf(fileName)) + ".woz", std::move(woz)});
        return true;
    }
    if (diskcopy42::looksLikeDiskCopy42(bytes)) return convertDiskCopy(fileName, bytes, images, note, error);
    const std::string ext = extensionOf(fileName);
    if (ext == ".a2r")
        error = "not an A2R 2 or A2R 3 flux capture";
    else if (ext == ".dc" || ext == ".dc42" || ext == ".image")
        error = "not a DiskCopy 4.2 image (its header or its size does not match)";
    else
        error = "not a ShrinkIt archive";
    return false;
}

} // namespace pom2::exchange
