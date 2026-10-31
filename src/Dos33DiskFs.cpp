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

// DOS 3.3 file system — see Dos33DiskFs.h.

#include "Dos33DiskFs.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace pom2::dos33fs {

namespace {

constexpr int kTracks = 35;
constexpr int kSectors = 16;
constexpr int kVtocTrack = 17;
constexpr int kPairsPerList = 122;
constexpr int kEntriesPerSector = 7;
constexpr int kEntryBytes = 35;
constexpr int kFirstEntry = 0x0B;

/// Position in a ProDOS-order track of DOS 3.3 sector d.
constexpr uint8_t kDosToProdos[16] = {
    0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };

std::string upper(std::string s)
{
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string entryName(const uint8_t* e)
{
    std::string n;
    for (int i = 3; i < 33; ++i) n.push_back(char(e[i] & 0x7F));
    while (!n.empty() && n.back() == ' ') n.pop_back();
    return n;
}

} // namespace

SectorOrder orderForPath(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : upper(path.substr(dot + 1));
    return ext == "PO" ? SectorOrder::Prodos : SectorOrder::Dos;
}

char typeLetter(uint8_t t)
{
    switch (t & 0x7F) {
        case 0x00: return 'T';
        case 0x01: return 'I';
        case 0x02: return 'A';
        case 0x04: return 'B';
        case 0x08: return 'S';
        case 0x10: return 'R';
        case 0x20: return 'a';
        case 0x40: return 'b';
        default:   return '?';
    }
}

uint8_t* Dos33Disk::sector(int track, int sec)
{
    const int pos = order_ == SectorOrder::Dos ? sec : kDosToProdos[sec];
    return image_.data() + (std::size_t(track) * kSectors + std::size_t(pos)) * 256;
}

const uint8_t* Dos33Disk::sector(int track, int sec) const
{
    return const_cast<Dos33Disk*>(this)->sector(track, sec);
}

bool Dos33Disk::open(std::vector<uint8_t> image, SectorOrder order, std::string& error)
{
    if (image.size() != kImageBytes) {
        error = "not a 140 KB 5.25\" image";
        return false;
    }
    image_ = std::move(image);
    order_ = order;
    const uint8_t* v = sector(kVtocTrack, 0);
    if (v[0x34] != kTracks || v[0x35] != kSectors || v[0x36] != 0x00 || v[0x37] != 0x01
        || v[0x01] >= kTracks || v[0x02] >= kSectors || v[0x27] != kPairsPerList) {
        error = "no DOS 3.3 VTOC on track 17";
        return false;
    }
    return true;
}

int Dos33Disk::volume() const { return sector(kVtocTrack, 0)[0x06]; }

bool Dos33Disk::isFree(int track, int sec) const
{
    const uint8_t* v = sector(kVtocTrack, 0);
    const uint8_t byte = v[0x38 + track * 4 + (sec >= 8 ? 0 : 1)];
    return (byte >> (sec & 7)) & 1;
}

void Dos33Disk::setFree(int track, int sec, bool free)
{
    uint8_t& byte = sector(kVtocTrack, 0)[0x38 + track * 4 + (sec >= 8 ? 0 : 1)];
    const uint8_t bit = uint8_t(1u << (sec & 7));
    byte = free ? uint8_t(byte | bit) : uint8_t(byte & ~bit);
}

int Dos33Disk::freeSectors() const
{
    int n = 0;
    for (int t = 0; t < kTracks; ++t)
        for (int s = 0; s < kSectors; ++s) n += isFree(t, s);
    return n;
}

std::vector<FileInfo> Dos33Disk::list() const
{
    std::vector<FileInfo> out;
    const uint8_t* v = sector(kVtocTrack, 0);
    int t = v[0x01], s = v[0x02];
    std::set<int> seen;
    while (t && t < kTracks && s < kSectors && seen.insert(t * 16 + s).second) {
        const uint8_t* c = sector(t, s);
        for (int i = 0; i < kEntriesPerSector; ++i) {
            const uint8_t* e = c + kFirstEntry + i * kEntryBytes;
            if (e[0] == 0 || e[0] == 0xFF) continue;
            FileInfo f;
            f.name = entryName(e);
            f.type = e[2] & 0x7F;
            f.locked = (e[2] & 0x80) != 0;
            f.sectors = e[33] | e[34] << 8;
            out.push_back(f);
        }
        t = c[0x01]; s = c[0x02];
    }
    return out;
}

bool Dos33Disk::findEntry(const std::string& name, Entry& at) const
{
    const std::string want = upper(name);
    const uint8_t* v = sector(kVtocTrack, 0);
    int t = v[0x01], s = v[0x02];
    std::set<int> seen;
    while (t && t < kTracks && s < kSectors && seen.insert(t * 16 + s).second) {
        const uint8_t* c = sector(t, s);
        for (int i = 0; i < kEntriesPerSector; ++i) {
            const uint8_t* e = c + kFirstEntry + i * kEntryBytes;
            if (e[0] == 0 || e[0] == 0xFF) continue;
            if (upper(entryName(e)) == want) { at = { t, s, kFirstEntry + i * kEntryBytes }; return true; }
        }
        t = c[0x01]; s = c[0x02];
    }
    return false;
}

bool Dos33Disk::chain(int t, int s, std::vector<std::pair<int, int>>& lists,
                      std::vector<std::pair<int, int>>& data) const
{
    std::set<int> seen;
    while (t || s) {
        if (t >= kTracks || s >= kSectors || !seen.insert(t * 16 + s).second) return false;
        lists.push_back({ t, s });
        const uint8_t* l = sector(t, s);
        const std::size_t base = std::size_t(l[0x05] | l[0x06] << 8);
        for (int i = 0; i < kPairsPerList; ++i) {
            const int dt = l[0x0C + 2 * i], ds = l[0x0D + 2 * i];
            if (dt >= kTracks || ds >= kSectors) return false;
            if (data.size() < base + std::size_t(i) + 1) data.resize(base + std::size_t(i) + 1, { 0, 0 });
            data[base + std::size_t(i)] = { dt, ds };
        }
        t = l[0x01]; s = l[0x02];
    }
    // Trailing holes are the list's unused pairs, not the file's.
    while (!data.empty() && data.back() == std::pair<int, int>{ 0, 0 }) data.pop_back();
    return true;
}

bool Dos33Disk::readFile(const std::string& name, std::vector<uint8_t>& raw,
                         std::string& error) const
{
    Entry at;
    if (!findEntry(name, at)) { error = name + ": not found"; return false; }
    const uint8_t* e = sector(at.track, at.sec) + at.offset;
    std::vector<std::pair<int, int>> lists, data;
    if (!chain(e[0], e[1], lists, data)) { error = name + ": damaged T/S list"; return false; }
    raw.clear();
    raw.reserve(data.size() * 256);
    for (const auto& [t, s] : data) {
        if (t == 0 && s == 0) { raw.insert(raw.end(), 256, 0); continue; }
        const uint8_t* d = sector(t, s);
        raw.insert(raw.end(), d, d + 256);
    }
    return true;
}

std::vector<uint8_t> Dos33Disk::payload(uint8_t type, const std::vector<uint8_t>& raw,
                                        uint16_t* loadAddress)
{
    auto word = [&](std::size_t o) {
        return o + 1 < raw.size() ? std::size_t(raw[o] | raw[o + 1] << 8) : std::size_t(0);
    };
    switch (type & 0x7F) {
        case 0x00: {
            const auto z = std::find(raw.begin(), raw.end(), uint8_t(0));
            return { raw.begin(), z };
        }
        case 0x01: case 0x02: {
            const std::size_t n = std::min(word(0), raw.size() >= 2 ? raw.size() - 2 : 0);
            return { raw.begin() + 2, raw.begin() + 2 + std::ptrdiff_t(n) };
        }
        case 0x04: {
            if (loadAddress) *loadAddress = uint16_t(word(0));
            const std::size_t n = std::min(word(2), raw.size() >= 4 ? raw.size() - 4 : 0);
            return { raw.begin() + 4, raw.begin() + 4 + std::ptrdiff_t(n) };
        }
        default:
            return raw;
    }
}

std::vector<uint8_t> Dos33Disk::textToDos(const std::vector<uint8_t>& host)
{
    std::vector<uint8_t> out;
    for (std::size_t i = 0; i < host.size(); ++i) {
        uint8_t c = host[i];
        if (c == '\r') {
            if (i + 1 < host.size() && host[i + 1] == '\n') continue;
            c = '\n';
        }
        out.push_back(c == '\n' ? 0x8D : uint8_t(c | 0x80));
    }
    return out;
}

std::vector<uint8_t> Dos33Disk::textFromDos(const std::vector<uint8_t>& dos)
{
    std::vector<uint8_t> out;
    for (uint8_t b : dos) {
        const uint8_t c = b & 0x7F;
        out.push_back(c == 0x0D ? uint8_t('\n') : c);
    }
    return out;
}

bool Dos33Disk::removeFile(const std::string& name, std::string& error)
{
    Entry at;
    if (!findEntry(name, at)) { error = name + ": not found"; return false; }
    uint8_t* e = sector(at.track, at.sec) + at.offset;
    if (e[2] & 0x80) { error = name + ": locked"; return false; }
    std::vector<std::pair<int, int>> lists, data;
    if (!chain(e[0], e[1], lists, data)) { error = name + ": damaged T/S list"; return false; }
    for (const auto& [t, s] : lists) setFree(t, s, true);
    for (const auto& [t, s] : data) if (t || s) setFree(t, s, true);
    // DOS's DELETE: the T/S list track moves to the name's last byte.
    e[32] = e[0];
    e[0] = 0xFF;
    return true;
}

bool Dos33Disk::writeFile(const std::string& name, FileType type,
                          const std::vector<uint8_t>& data, uint16_t loadAddress,
                          std::string& error)
{
    const std::string up = upper(name);
    if (up.empty() || up.size() > 30 || !std::isalpha(static_cast<unsigned char>(up[0]))
        || up.find(',') != std::string::npos) {
        error = name + ": a DOS 3.3 name is 1-30 characters, starting with a letter, no comma";
        return false;
    }
    // The bytes the file will hold.
    std::vector<uint8_t> raw;
    switch (type) {
        case FileType::Text:
            raw = data;
            raw.push_back(0);
            break;
        case FileType::Integer: case FileType::Applesoft:
            raw = { uint8_t(data.size()), uint8_t(data.size() >> 8) };
            raw.insert(raw.end(), data.begin(), data.end());
            break;
        case FileType::Binary:
            raw = { uint8_t(loadAddress), uint8_t(loadAddress >> 8),
                    uint8_t(data.size()), uint8_t(data.size() >> 8) };
            raw.insert(raw.end(), data.begin(), data.end());
            break;
        default:
            raw = data;
            break;
    }
    if (data.size() > 0xFFFF && type != FileType::Text) { error = name + ": too large"; return false; }

    const std::vector<uint8_t> backup = image_;   // all-or-nothing
    auto fail = [&](const std::string& why) { image_ = backup; error = why; return false; };

    Entry at;
    if (findEntry(up, at)) {
        if (sector(at.track, at.sec)[at.offset + 2] & 0x80) return fail(name + ": locked");
        if (!removeFile(up, error)) return fail(error);
    }

    const std::size_t dataSectors = std::max<std::size_t>(1, (raw.size() + 255) / 256);
    const std::size_t listSectors = (dataSectors + kPairsPerList - 1) / kPairsPerList;
    // DOS allocates outward from the catalog track: 18 up, then 16 down.
    std::vector<std::pair<int, int>> free;
    for (int t = 18; t < kTracks; ++t)
        for (int s = kSectors - 1; s >= 0; --s) if (isFree(t, s)) free.push_back({ t, s });
    for (int t = 16; t >= 1; --t)
        for (int s = kSectors - 1; s >= 0; --s) if (isFree(t, s)) free.push_back({ t, s });
    if (free.size() < dataSectors + listSectors) return fail("disk full");

    // A free catalog slot.
    Entry slot{ -1, -1, -1 };
    {
        const uint8_t* v = sector(kVtocTrack, 0);
        int t = v[0x01], s = v[0x02];
        std::set<int> seen;
        while (t && t < kTracks && s < kSectors && seen.insert(t * 16 + s).second && slot.track < 0) {
            const uint8_t* c = sector(t, s);
            for (int i = 0; i < kEntriesPerSector && slot.track < 0; ++i) {
                const uint8_t* e = c + kFirstEntry + i * kEntryBytes;
                if (e[0] == 0 || e[0] == 0xFF) slot = { t, s, kFirstEntry + i * kEntryBytes };
            }
            t = c[0x01]; s = c[0x02];
        }
    }
    if (slot.track < 0) return fail("catalog full");

    std::size_t next = 0;
    std::vector<std::pair<int, int>> lists(free.begin(), free.begin() + std::ptrdiff_t(listSectors));
    next = listSectors;
    for (const auto& [t, s] : lists) {
        setFree(t, s, false);
        std::fill_n(sector(t, s), 256, 0);
    }
    for (std::size_t i = 0; i < dataSectors; ++i) {
        const auto [t, s] = free[next++];
        setFree(t, s, false);
        uint8_t* d = sector(t, s);
        std::fill_n(d, 256, 0);
        const std::size_t off = i * 256;
        if (off < raw.size())
            std::copy_n(raw.begin() + std::ptrdiff_t(off), std::min<std::size_t>(256, raw.size() - off), d);
        uint8_t* l = sector(lists[i / kPairsPerList].first, lists[i / kPairsPerList].second);
        l[0x0C + 2 * (i % kPairsPerList)] = uint8_t(t);
        l[0x0D + 2 * (i % kPairsPerList)] = uint8_t(s);
    }
    for (std::size_t k = 0; k < lists.size(); ++k) {
        uint8_t* l = sector(lists[k].first, lists[k].second);
        const std::size_t base = k * kPairsPerList;
        l[0x05] = uint8_t(base); l[0x06] = uint8_t(base >> 8);
        if (k + 1 < lists.size()) { l[0x01] = uint8_t(lists[k + 1].first); l[0x02] = uint8_t(lists[k + 1].second); }
    }

    uint8_t* e = sector(slot.track, slot.sec) + slot.offset;
    std::fill_n(e, kEntryBytes, 0);
    e[0] = uint8_t(lists[0].first);
    e[1] = uint8_t(lists[0].second);
    e[2] = uint8_t(type);
    for (int i = 0; i < 30; ++i)
        e[3 + i] = uint8_t((i < int(up.size()) ? up[std::size_t(i)] : ' ') | 0x80);
    const std::size_t count = dataSectors + listSectors;
    e[33] = uint8_t(count); e[34] = uint8_t(count >> 8);
    return true;
}

} // namespace pom2::dos33fs
