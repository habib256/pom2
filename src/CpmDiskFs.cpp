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

#include "CpmDiskFs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <utility>

namespace pom2::cpmfs {

namespace {

constexpr uint8_t kDeleted = 0xE5;
constexpr int kRecordsPerExtent = 128;
constexpr int kRecordsPerBlock  = kBlockBytes / 128;
constexpr int kBlocksPerExtent  = 16;
constexpr int kDirBlocks        = 2;   // 64 entries × 32 bytes

// Free: $E5, or 32 zero bytes — a directory sector the formatter never
// wrote (ALS 3.01C Disks 2 and 3 end in one). CP/M would read such an entry
// as a user-0 file with a name of NULs and no blocks; nothing can open it.
bool unused(const uint8_t* e)
{
    if (e[0] == kDeleted) return true;
    for (int i = 0; i < 32; ++i) if (e[i]) return false;
    return true;
}

int extentNumber(const uint8_t* e) { return (e[12] & 0x1F) + 32 * (e[14] & 0x3F); }

std::string displayName(const uint8_t* e)
{
    std::string base, ext;
    for (int i = 1; i <= 8; ++i)  base.push_back(char(e[i] & 0x7F));
    for (int i = 9; i <= 11; ++i) ext.push_back(char(e[i] & 0x7F));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    return ext.empty() ? base : base + "." + ext;
}

bool sameName(const uint8_t* e, const std::string& name11)
{
    for (int i = 0; i < 11; ++i)
        if ((e[1 + i] & 0x7F) != uint8_t(name11[size_t(i)])) return false;
    return true;
}

} // namespace

SectorOrder orderForPath(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "po") return SectorOrder::Prodos;
    if (ext == "cpm") return SectorOrder::Cpm;
    return SectorOrder::Dos;
}

bool CpmDisk::toDirName(const std::string& name, std::string& out11)
{
    // CCP's delimiters (CP/M 2.2 CCP `delim`) plus the wildcards: a file
    // named with one of them can be written but never typed again.
    static const char* kBad = "<>.,;:=?*[]%|()/\\ ";
    const auto dot = name.find('.');
    std::string base = name.substr(0, dot);
    std::string ext  = dot == std::string::npos ? std::string() : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
    out11.assign(11, ' ');
    auto put = [&](const std::string& s, size_t at) {
        for (size_t i = 0; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x21 || c > 0x7E || std::strchr(kBad, c)) return false;
            out11[at + i] = char(std::toupper(c));
        }
        return true;
    };
    return put(base, 0) && put(ext, 8);
}

uint8_t* CpmDisk::sector(int track, int dosSector)
{
    const int pos = order_ == SectorOrder::Prodos ? kDosToProdos[dosSector]
                  : order_ == SectorOrder::Cpm    ? kDosToCpm[dosSector]
                  : dosSector;
    return image_.data() + (size_t(track) * 16 + size_t(pos)) * 256;
}

const uint8_t* CpmDisk::sector(int track, int dosSector) const
{
    return const_cast<CpmDisk*>(this)->sector(track, dosSector);
}

// Block b = 4 consecutive logical sectors of the data area. The track
// wraps modulo 35: the 60K system's blocks 128-139 are tracks 0-2.
void CpmDisk::readBlock(int block, uint8_t* out) const
{
    for (int i = 0; i < 4; ++i) {
        const int n = block * 4 + i;
        const int track = (kReservedTracks + n / 16) % 35;
        std::memcpy(out + i * 256, sector(track, kSkew[n % 16]), 256);
    }
}

void CpmDisk::writeBlock(int block, const uint8_t* in)
{
    for (int i = 0; i < 4; ++i) {
        const int n = block * 4 + i;
        const int track = (kReservedTracks + n / 16) % 35;
        std::memcpy(sector(track, kSkew[n % 16]), in + i * 256, 256);
    }
}

void CpmDisk::loadDirectory()
{
    uint8_t buf[kDirBlocks * kBlockBytes];
    for (int b = 0; b < kDirBlocks; ++b) readBlock(b, buf + b * kBlockBytes);
    std::memcpy(dir_, buf, sizeof dir_);
}

void CpmDisk::storeDirectory()
{
    uint8_t buf[kDirBlocks * kBlockBytes];
    std::memcpy(buf, dir_, sizeof dir_);
    for (int b = 0; b < kDirBlocks; ++b) writeBlock(b, buf + b * kBlockBytes);
}

std::vector<uint8_t> CpmDisk::imageInOrder(SectorOrder order) const
{
    CpmDisk out;
    out.image_.assign(image_.size(), 0);
    out.order_ = order;
    for (int t = 0; t < 35; ++t)
        for (int s = 0; s < 16; ++s)
            std::memcpy(out.sector(t, s), sector(t, s), 256);
    return out.image_;
}

bool CpmDisk::open(std::vector<uint8_t> image, SectorOrder order, std::string& error)
{
    if (image.size() != kImageBytes) {
        error = "not a 140 KB 5.25\" image (" + std::to_string(image.size()) + " bytes)";
        return false;
    }
    image_ = std::move(image);
    order_ = order;
    loadDirectory();
    // An entry is either free ($E5) or a user number 0-31 followed by eleven
    // printable name bytes (attribute bits masked) and block numbers inside
    // the disk. A DOS 3.3 or ProDOS disk fails this on its first entry.
    for (const auto& e : dir_) {
        if (unused(e)) continue;
        bool ok = e[0] <= 31 && e[15] <= 0x80;
        for (int i = 1; ok && i <= 11; ++i) {
            const uint8_t c = e[i] & 0x7F;
            ok = c >= 0x20 && c < 0x7F;
        }
        for (int i = 16; ok && i < 32; ++i) ok = e[i] <= kMaxReadBlock;
        if (!ok) {
            error = "no CP/M directory on track 3 (wrong sector order, or not a CP/M disk)";
            image_.clear();
            return false;
        }
    }
    return true;
}

std::vector<bool> CpmDisk::usedBlocks() const
{
    std::vector<bool> used(kMaxReadBlock + 1, false);
    for (int b = 0; b < kDirBlocks; ++b) used[size_t(b)] = true;
    for (const auto& e : dir_) {
        if (unused(e)) continue;
        for (int i = 16; i < 32; ++i)
            if (e[i]) used[e[i]] = true;
    }
    return used;
}

int CpmDisk::freeBlocks() const
{
    const auto used = usedBlocks();
    return int(std::count(used.begin(), used.begin() + kDataBlocks, false));
}

std::vector<int> CpmDisk::entriesOf(int user, const std::string& name11) const
{
    std::vector<int> out;
    for (int i = 0; i < kDirEntries; ++i)
        if (!unused(dir_[i]) && dir_[i][0] == user && sameName(dir_[i], name11))
            out.push_back(i);
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        return extentNumber(dir_[a]) < extentNumber(dir_[b]);
    });
    return out;
}

std::vector<FileInfo> CpmDisk::list() const
{
    std::vector<FileInfo> files;
    std::map<std::pair<int, std::string>, size_t> index;
    std::map<size_t, int> lastExtent;
    for (const auto& e : dir_) {
        if (unused(e) || e[0] > 15) continue;   // free, label, system pseudo-file
        const std::string name = displayName(e);
        const auto key = std::make_pair(int(e[0]), name);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, files.size()).first;
            FileInfo f;
            f.user = e[0];
            f.name = name;
            f.readOnly = (e[9] & 0x80) != 0;
            f.system = (e[10] & 0x80) != 0;
            files.push_back(f);
            lastExtent[it->second] = -1;
        }
        FileInfo& f = files[it->second];
        for (int i = 16; i < 32; ++i) if (e[i]) ++f.blocks;
        const int x = extentNumber(e);
        if (x > lastExtent[it->second]) {
            lastExtent[it->second] = x;
            f.bytes = uint32_t(x * kRecordsPerExtent + e[15]) * 128u;
        }
    }
    return files;
}

bool CpmDisk::readFile(int user, const std::string& name,
                       std::vector<uint8_t>& out, std::string& error) const
{
    std::string n11;
    if (!toDirName(name, n11)) { error = "invalid CP/M file name: " + name; return false; }
    const auto entries = entriesOf(user, n11);
    if (entries.empty()) {
        error = name + ": no such file in user " + std::to_string(user);
        return false;
    }
    const uint8_t* last = dir_[entries.back()];
    const size_t records = size_t(extentNumber(last)) * kRecordsPerExtent + last[15];
    out.assign(records * 128, 0x1A);
    uint8_t blk[kBlockBytes];
    for (int idx : entries) {
        const uint8_t* e = dir_[idx];
        const size_t base = size_t(extentNumber(e)) * kRecordsPerExtent * 128;
        for (int j = 0; j < kBlocksPerExtent; ++j) {
            if (!e[16 + j]) continue;          // a hole reads as ^Z
            const size_t at = base + size_t(j) * kBlockBytes;
            if (at >= out.size()) break;
            readBlock(e[16 + j], blk);
            std::memcpy(out.data() + at, blk, std::min<size_t>(kBlockBytes, out.size() - at));
        }
    }
    return true;
}

bool CpmDisk::removeFile(int user, const std::string& name, std::string& error)
{
    std::string n11;
    if (!toDirName(name, n11)) { error = "invalid CP/M file name: " + name; return false; }
    const auto entries = entriesOf(user, n11);
    if (entries.empty()) {
        error = name + ": no such file in user " + std::to_string(user);
        return false;
    }
    for (int idx : entries) dir_[idx][0] = kDeleted;
    storeDirectory();
    return true;
}

bool CpmDisk::writeFile(int user, const std::string& name,
                        const std::vector<uint8_t>& data, std::string& error)
{
    std::string n11;
    if (user < 0 || user > 15) { error = "user number must be 0-15"; return false; }
    if (!toDirName(name, n11)) { error = "invalid CP/M file name: " + name; return false; }

    // Work on copies so a disk-full or directory-full refusal changes nothing.
    uint8_t dir[kDirEntries][32];
    std::memcpy(dir, dir_, sizeof dir);
    for (auto& e : dir)
        if (!unused(e) && e[0] == user && sameName(e, n11)) e[0] = kDeleted;

    std::vector<bool> used(kMaxReadBlock + 1, false);
    for (int b = 0; b < kDirBlocks; ++b) used[size_t(b)] = true;
    for (const auto& e : dir)
        if (!unused(e))
            for (int i = 16; i < 32; ++i) if (e[i]) used[e[i]] = true;

    const size_t records = (data.size() + 127) / 128;
    const size_t blocks  = (records + kRecordsPerBlock - 1) / kRecordsPerBlock;
    const size_t extents = std::max<size_t>(1, (blocks + kBlocksPerExtent - 1) / kBlocksPerExtent);

    std::vector<int> freeList;
    for (int b = kDirBlocks; b < kDataBlocks; ++b) if (!used[size_t(b)]) freeList.push_back(b);
    if (freeList.size() < blocks) {
        error = "disk full: " + name + " needs " + std::to_string(blocks) + " KB, "
              + std::to_string(freeList.size()) + " KB free";
        return false;
    }
    std::vector<int> slots;
    for (int i = 0; i < kDirEntries; ++i) if (unused(dir[i])) slots.push_back(i);
    if (slots.size() < extents) {
        error = "directory full (64 entries)";
        return false;
    }

    std::vector<uint8_t> padded(blocks * kBlockBytes, 0x1A);
    if (!data.empty()) std::memcpy(padded.data(), data.data(), data.size());
    for (size_t b = 0; b < blocks; ++b)
        writeBlock(freeList[b], padded.data() + b * kBlockBytes);

    for (size_t x = 0; x < extents; ++x) {
        uint8_t* e = dir[slots[x]];
        std::memset(e, 0, 32);
        e[0] = uint8_t(user);
        std::memcpy(e + 1, n11.data(), 11);
        e[12] = uint8_t(x & 0x1F);
        e[14] = uint8_t(x >> 5);
        const size_t first = x * kRecordsPerExtent;
        e[15] = uint8_t(std::min<size_t>(kRecordsPerExtent, records - std::min(records, first)));
        for (int j = 0; j < kBlocksPerExtent; ++j) {
            const size_t b = x * kBlocksPerExtent + size_t(j);
            if (b < blocks) e[16 + j] = uint8_t(freeList[b]);
        }
    }
    std::memcpy(dir_, dir, sizeof dir_);
    storeDirectory();
    return true;
}

} // namespace pom2::cpmfs
