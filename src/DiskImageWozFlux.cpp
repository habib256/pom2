// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiskImage.h"
#include "Block512Backing.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

void DiskImage::writeWozFlux(int qt, int64_t start, int64_t end, int count,
                             const int64_t* transitions, int64_t revolutionStart)
{
    const int period = trackPeriod(qt);
    if (period <= 0 || end <= start) return;
    const int64_t origin = revolutionStart >= 0 ? revolutionStart : 0;
    // A window longer than one revolution writes the surface over itself:
    // what stays is the LAST revolution. Clamping the duration from the
    // front kept the first one instead, and folding every later transition
    // onto it (modulo the period) laid revolution two on top of one.
    if (end - start > period) start = end - period;
    const int64_t begin = ((start - origin) % period + period) % period;
    const int64_t duration = end - start;
    const int64_t limitedEnd = end;
    if (begin + duration > period) {
        const int64_t split = start + period - begin;
        writeWozFlux(qt, start, split, count, transitions, revolutionStart);
        writeWozFlux(qt, split, limitedEnd, count, transitions, revolutionStart);
        return;
    }

    const int periodTicks = wozQtFluxPeriodTicks[qt];
    const int firstTick = static_cast<int>(std::min<int64_t>(begin * 4, periodTicks));
    const int lastTick = static_cast<int>(std::min<int64_t>((begin + duration) * 4, periodTicks));
    if (firstTick >= lastTick) return;
    auto& ticks = wozQtFluxTicks[qt];
    std::vector<int> replacement;
    replacement.reserve(static_cast<size_t>(std::max(0, count)));
    const int64_t base = start - begin;
    for (int i = 0; i < count; ++i) {
        // Only this window's own transitions: a split half, or the earlier
        // revolutions of a long window, must not fold in.
        if (transitions[i] < start || transitions[i] >= limitedEnd) continue;
        const int64_t pos = ((transitions[i] - base) % period + period) % period;
        const int64_t tick = pos * 4;
        if (tick >= firstTick && tick < lastTick)
            replacement.push_back(static_cast<int>(tick));
    }
    std::sort(replacement.begin(), replacement.end());
    replacement.erase(std::unique(replacement.begin(), replacement.end()), replacement.end());
    const auto first = std::lower_bound(ticks.begin(), ticks.end(), firstTick);
    const auto last = std::lower_bound(first, ticks.end(), lastTick);
    if (static_cast<size_t>(last - first) == replacement.size() &&
        std::equal(first, last, replacement.begin())) return;
    ticks.erase(first, last);
    ticks.insert(std::lower_bound(ticks.begin(), ticks.end(), firstTick),
                 replacement.begin(), replacement.end());

    // FLUX maps can alias one TRKS entry at several quarter tracks.
    for (int a = 0; a < kQuarterTracks; ++a) {
        if (wozQtFluxHeaderOff[a] != wozQtFluxHeaderOff[qt] ||
            wozQtFluxPeriodTicks[a] == 0) continue;
        if (a != qt) wozQtFluxTicks[a] = ticks;
        auto& events = fluxStream[a];
        auto& bits = bitStream[a];
        events.clear();
        events.reserve(ticks.size());
        std::fill(bits.begin(), bits.end(), 0);
        const int cyc = lssCyclesPerCell();
        for (int tick : ticks) {
            const int cycle = tick / 4;
            events.push_back(cycle);
            const size_t cell = static_cast<size_t>(cycle / cyc);
            if (cell < bits.size()) bits[cell] = 1;
        }
        fluxStreamValid[a] = true;
    }
    wozQtDirty[qt] = true;
    anyDirty = true;
    autosave_.noteWrite();
    pom2::noteMediaWrite();
}

bool DiskImage::growWozFluxSlot(int qt, size_t bytesNeeded)
{
    const size_t blocks = (bytesNeeded + 511) / 512;
    const size_t extra = blocks * 512;
    if (blocks == 0 || blocks > 0xFFFF ||
        extra > 16u * 1024u * 1024u - wozRaw.size()) return false;

    auto read16 = [&](size_t pos) -> size_t {
        return static_cast<size_t>(wozRaw[pos]) |
               (static_cast<size_t>(wozRaw[pos + 1]) << 8);
    };
    auto read32 = [&](size_t pos) -> size_t {
        return read16(pos) | (read16(pos + 2) << 16);
    };
    auto write16 = [&](size_t pos, size_t value) {
        wozRaw[pos] = static_cast<uint8_t>(value);
        wozRaw[pos + 1] = static_cast<uint8_t>(value >> 8);
    };
    auto write32 = [&](size_t pos, size_t value) {
        write16(pos, value);
        write16(pos + 2, value >> 16);
    };

    size_t trksHeader = 0, trksData = 0, trksLength = 0, infoData = 0;
    for (size_t pos = 12; pos <= wozRaw.size() && wozRaw.size() - pos >= 8;) {
        const size_t length = read32(pos + 4);
        if (length > wozRaw.size() - pos - 8) return false;
        if (std::memcmp(wozRaw.data() + pos, "TRKS", 4) == 0) {
            trksHeader = pos; trksData = pos + 8; trksLength = length;
        } else if (std::memcmp(wozRaw.data() + pos, "INFO", 4) == 0 && length >= 50) {
            infoData = pos + 8;
        }
        pos += 8 + length;
    }
    if (trksData == 0 || infoData == 0 || trksLength < 160 * 8 ||
        wozQtFluxHeaderOff[qt] < trksData ||
        wozQtFluxHeaderOff[qt] >= trksData + 160 * 8) return false;
    const size_t insertAt = trksData + trksLength;
    if (infoData >= insertAt || insertAt % 512 != 0 ||
        insertAt / 512 > 0xFFFF ||
        trksLength + extra > UINT32_MAX) return false;

    // Track blocks are absolute file offsets. Extend TRKS at its block
    // boundary, shifting later chunks (notably FLUX) as a whole.
    wozRaw.insert(wozRaw.begin() + insertAt, extra, 0);
    write32(trksHeader + 4, trksLength + extra);
    for (size_t i = 0; i < 160; ++i) {
        const size_t hdr = trksData + i * 8;
        const size_t block = read16(hdr);
        if (block != 0 && block * 512 >= insertAt)
            write16(hdr, block + blocks);
    }
    const size_t fluxBlock = read16(infoData + 46);
    if (fluxBlock != 0 && fluxBlock * 512 >= insertAt)
        write16(infoData + 46, fluxBlock + blocks);
    const size_t largest = read16(infoData + 48);
    if (blocks > largest) write16(infoData + 48, blocks);
    for (int a = 0; a < kQuarterTracks; ++a) {
        if (wozQtByteOff[a] >= insertAt && wozQtByteLen[a] != 0)
            wozQtByteOff[a] += extra;
        if (wozQtFluxHeaderOff[a] == wozQtFluxHeaderOff[qt] &&
            wozQtFluxPeriodTicks[a] != 0) {
            wozQtByteOff[a] = insertAt;
            wozQtByteLen[a] = extra;
        }
    }
    const size_t hdr = wozQtFluxHeaderOff[qt];
    write16(hdr, insertAt / 512);
    write16(hdr + 2, blocks);
    return true;
}

// A quarter-track the image does not carry (TMAP $FF — Applesauce leaves a
// half-used disk's empty tracks out) is unformatted surface, and real media
// takes the write. writeFlux used to drop it: no dirty flag, no error, a
// format verify that failed and a disk that could never be reformatted.
// MAME writes the flux and creates the track on save. A WOZ that cannot grow
// refuses with one warning per quarter-track, not one per flush.
bool DiskImage::allocateWozTrack(int qt)
{
    if (!growWozForTrack(qt)) {
        if (qt >= 0 && qt < kQuarterTracks && !wozAllocFailed_[qt]) {
            wozAllocFailed_[qt] = true;
            lastError = "WOZ: cannot add quarter-track " + std::to_string(qt / 4.0) +
                        " to this image; writes to it are lost";
            pom2::log().warn("Disk II", lastError);
        }
        return false;
    }
    return true;
}

bool DiskImage::growWozForTrack(int qt)
{
    if (!wozFormat || qt < 0 || qt >= kQuarterTracks ||
        wozQtBitCount[qt] != 0 || wozQtFluxPeriodTicks[qt] != 0 ||
        wozRaw.size() < 12) return false;
    const bool woz1 = std::memcmp(wozRaw.data(), "WOZ1", 4) == 0;

    auto read16 = [&](size_t pos) -> size_t {
        return static_cast<size_t>(wozRaw[pos]) |
               (static_cast<size_t>(wozRaw[pos + 1]) << 8);
    };
    auto read32 = [&](size_t pos) -> size_t {
        return read16(pos) | (read16(pos + 2) << 16);
    };
    auto write16 = [&](size_t pos, size_t value) {
        wozRaw[pos] = static_cast<uint8_t>(value);
        wozRaw[pos + 1] = static_cast<uint8_t>(value >> 8);
    };
    auto write32 = [&](size_t pos, size_t value) {
        write16(pos, value);
        write16(pos + 2, value >> 16);
    };
    size_t trksHeader = 0, trksData = 0, trksLength = 0;
    size_t infoData = 0, infoLength = 0, tmapData = 0;
    for (size_t pos = 12; pos <= wozRaw.size() && wozRaw.size() - pos >= 8;) {
        const size_t length = read32(pos + 4);
        if (length > wozRaw.size() - pos - 8) return false;
        if (std::memcmp(wozRaw.data() + pos, "TRKS", 4) == 0) {
            trksHeader = pos; trksData = pos + 8; trksLength = length;
        } else if (std::memcmp(wozRaw.data() + pos, "INFO", 4) == 0) {
            infoData = pos + 8; infoLength = length;
        } else if (std::memcmp(wozRaw.data() + pos, "TMAP", 4) == 0 && length >= 160) {
            tmapData = pos + 8;
        }
        pos += 8 + length;
    }
    if (trksData == 0 || tmapData == 0 || infoData == 0) return false;

    // The new track is as long as the image's own tracks: they carry this
    // drive's speed (and INFO+39's cell width). Nearest populated whole
    // track first; 200 ms of cells at the image's timing when there is none.
    size_t bits = 0;
    for (int d = 0; d < kQuarterTracks && bits == 0; ++d) {
        for (int a : {qt - d, qt + d}) {
            if (a >= 0 && a < kQuarterTracks && (a & 3) == 0 &&
                wozQtBitCount[a] != 0) { bits = wozQtBitCount[a]; break; }
        }
    }
    if (bits == 0) bits = 200000u * 8u / static_cast<size_t>(optimalBitTiming);

    const size_t insertAt = trksData + trksLength;
    size_t extra = 0, capacity = 0;
    int index = -1;
    if (woz1) {
        // WOZ1: fixed 6656-byte slots, which TMAP indexes in order.
        constexpr size_t kSlot = 6656, kBitBytes = 6646;
        bits = std::min(bits, kBitBytes * 8);
        if (trksLength % kSlot != 0 || trksLength / kSlot >= 0xFF) return false;
        index = static_cast<int>(trksLength / kSlot);
        extra = kSlot;
        if (extra > 16u * 1024u * 1024u - wozRaw.size()) return false;
        wozRaw.insert(wozRaw.begin() + insertAt, extra, 0);
        write32(trksHeader + 4, trksLength + extra);
        write16(insertAt + 6646, (bits + 7) / 8);   // bytes_used
        write16(insertAt + 6648, bits);             // bit_count
        write16(insertAt + 6650, 0xFFFF);           // no splice point
        capacity = kBitBytes;
    } else {
        // WOZ2: a free 8-byte header among the 160, data blocks appended at
        // TRKS's block-aligned end — the shape growWozFluxSlot uses.
        if (trksLength < 160 * 8 || insertAt % 512 != 0 ||
            insertAt / 512 > 0xFFFF) return false;
        for (int i = 0; i < 160 && index < 0; ++i) {
            const size_t hdr = trksData + static_cast<size_t>(i) * 8;
            if (read16(hdr) == 0 && read16(hdr + 2) == 0 && read32(hdr + 4) == 0)
                index = i;
        }
        if (index < 0) return false;
        const size_t blocks = ((bits + 7) / 8 + 511) / 512;
        extra = blocks * 512;
        if (blocks > 0xFFFF || extra > 16u * 1024u * 1024u - wozRaw.size() ||
            trksLength + extra > UINT32_MAX) return false;
        wozRaw.insert(wozRaw.begin() + insertAt, extra, 0);
        write32(trksHeader + 4, trksLength + extra);
        // Every block number at or past the insertion moves: other tracks'
        // data and the FLUX chunk (INFO+46).
        for (size_t i = 0; i < 160; ++i) {
            const size_t hdr = trksData + i * 8;
            const size_t block = read16(hdr);
            if (block != 0 && block * 512 >= insertAt)
                write16(hdr, block + blocks);
        }
        if (infoLength >= 50) {
            const size_t fluxBlock = read16(infoData + 46);
            if (fluxBlock != 0 && fluxBlock * 512 >= insertAt)
                write16(infoData + 46, fluxBlock + blocks);
        }
        if (infoLength >= 46 && blocks > read16(infoData + 44))
            write16(infoData + 44, blocks);         // largest_track
        const size_t hdr = trksData + static_cast<size_t>(index) * 8;
        write16(hdr, insertAt / 512);
        write16(hdr + 2, blocks);
        write32(hdr + 4, bits);
        capacity = extra;
    }
    for (int a = 0; a < kQuarterTracks; ++a)
        if (wozQtByteLen[a] != 0 && wozQtByteOff[a] >= insertAt)
            wozQtByteOff[a] += extra;
    // TMAP normally precedes TRKS, but nothing says it must.
    const size_t tmap = tmapData >= insertAt ? tmapData + extra : tmapData;
    wozRaw[tmap + static_cast<size_t>(qt)] = static_cast<uint8_t>(index);

    wozQtByteOff[qt]  = insertAt;
    wozQtByteLen[qt]  = capacity;
    wozQtBitCount[qt] = bits;
    bitStream[qt].assign(bits, 0);    // blank surface: no flux yet
    bitStreamValid[qt]  = true;
    fluxStreamValid[qt] = false;
    fluxStream[qt].clear();
    // The file layout changed even if the write that follows changes no
    // cell: the new TRK has to reach the file.
    wozQtDirty[qt] = true;
    anyDirty = true;
    autosave_.noteWrite();
    pom2::noteMediaWrite();
    return true;
}

bool DiskImage::applyWozFluxWriteBack()
{
    std::vector<std::pair<int, std::vector<uint8_t>>> encodedFlux;
    for (int qt = 0; qt < kQuarterTracks; ++qt) {
        if (!wozQtDirty[qt] || wozQtFluxPeriodTicks[qt] == 0) continue;
        const auto& ticks = wozQtFluxTicks[qt];
        std::vector<uint8_t> bytes;
        int previous = 0;
        auto appendGap = [&](int next) {
            int gap = next - previous;
            while (gap >= 255) {
                bytes.push_back(255);
                gap -= 255;
            }
            bytes.push_back(static_cast<uint8_t>(gap));
            previous = next;
        };
        for (int tick : ticks) appendGap(tick);
        appendGap(wozQtFluxPeriodTicks[qt]); // terminal wrap, no pulse
        if (bytes.size() > wozQtByteLen[qt] &&
            !growWozFluxSlot(qt, bytes.size())) {
            lastError = "WOZ FLUX track " + std::to_string(qt / 4.0) +
                " cannot grow its TRKS allocation; nothing was saved";
            return false;
        }
        const size_t off = wozQtByteOff[qt];
        const size_t capacity = wozQtByteLen[qt];
        const size_t hdr = wozQtFluxHeaderOff[qt];
        if (capacity == 0 || off > wozRaw.size() ||
            capacity > wozRaw.size() - off ||
            hdr == 0 || hdr > wozRaw.size() ||
            wozRaw.size() - hdr < 8 || bytes.size() > capacity) {
            lastError = "WOZ FLUX track " + std::to_string(qt / 4.0) +
                " needs " + std::to_string(bytes.size()) +
                " bytes but its TRKS slot holds " +
                std::to_string(capacity) +
                "; nothing was saved and changes remain in memory";
            return false;
        }
        encodedFlux.emplace_back(qt, std::move(bytes));
    }
    for (const auto& [qt, bytes] : encodedFlux) {
        const size_t off = wozQtByteOff[qt];
        std::copy(bytes.begin(), bytes.end(), wozRaw.begin() + off);
        const size_t hdr = wozQtFluxHeaderOff[qt];
        const uint32_t count = static_cast<uint32_t>(bytes.size());
        for (int b = 0; b < 4; ++b)
            wozRaw[hdr + 4 + b] = static_cast<uint8_t>(count >> (b * 8));
    }

    return true;
}

// MacBinary header CRC (MacBinary II: CRC-16/XMODEM over bytes 0-123).
static uint16_t macBinaryCrc(const uint8_t* p)
{
    uint16_t crc = 0;
    for (int i = 0; i < 124; ++i) {
        crc ^= static_cast<uint16_t>(p[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// A MacBinary envelope around a payload whose size changed (a WOZ 2.1 FLUX
// slot that grew): the header's data-fork length (bytes 83-86) and the
// padding to 128 that opens the trailer must follow it, or the reload cuts
// the WOZ at the old length and refuses it ("chunk length runs past EOF").
// A header CRC that was valid is recomputed. A 2IMG header is left alone.
void DiskImage::refitMacBinaryEnvelope(std::vector<uint8_t>& header,
                                       std::vector<uint8_t>& trailer, std::size_t payload)
{
    if (header.size() != 128 || header[0] != 0x00) return;
    const std::size_t old = (std::size_t(header[83]) << 24) | (std::size_t(header[84]) << 16) |
                            (std::size_t(header[85]) << 8)  |  std::size_t(header[86]);
    if (old == payload) return;
    const bool crcValid = ((uint16_t(header[124]) << 8) | header[125]) == macBinaryCrc(header.data());
    header[83] = static_cast<uint8_t>(payload >> 24);
    header[84] = static_cast<uint8_t>(payload >> 16);
    header[85] = static_cast<uint8_t>(payload >> 8);
    header[86] = static_cast<uint8_t>(payload);
    if (crcValid) {
        const uint16_t crc = macBinaryCrc(header.data());
        header[124] = static_cast<uint8_t>(crc >> 8);
        header[125] = static_cast<uint8_t>(crc);
    }
    const std::size_t oldPad = (128 - old % 128) % 128;
    const std::size_t newPad = (128 - payload % 128) % 128;
    const std::size_t drop = std::min(oldPad, trailer.size());
    trailer.erase(trailer.begin(), trailer.begin() + static_cast<std::ptrdiff_t>(drop));
    trailer.insert(trailer.begin(), newPad, 0x00);
}

// A .nib holds nibbles, not cells, so where the self-sync bytes were has to
// be inferred. MAME's rule (ap2_dsk.cpp a2_nib_format,
// generate_levels_from_nibbles): only "FF ... FF D5" is a sync pattern —
// every $FF of a run that the next bit-7 byte ends with $D5 is 10 cells, any
// other $FF stays 8, and a byte with bit 7 clear (padding MAME drops; kept
// here at 8 cells) neither ends nor joins a run. It used to give every .nib
// byte 8 cells, so a loader counting sync slip saw none (STORAGE-010).
void DiskImage::computeNibSyncWidths(const uint8_t* buf, uint8_t* widths)
{
    int anchor = -1;                    // a bit-7, non-$FF byte to start from
    for (int i = 0; i < kNibblesPerTrack; ++i)
        if (buf[i] != 0xFF && (buf[i] & 0x80)) { anchor = i; break; }
    if (anchor < 0) return;             // no address mark anywhere: no syncs
    std::vector<int> run;
    for (int k = 1; k <= kNibblesPerTrack; ++k) {
        const int idx = (anchor + k) % kNibblesPerTrack;
        const uint8_t b = buf[idx];
        if (b == 0xFF) { run.push_back(idx); continue; }
        if (!(b & 0x80)) continue;      // dropped by MAME: transparent here
        if (b == 0xD5) for (int r : run) widths[r] = 10;
        run.clear();
    }
}
