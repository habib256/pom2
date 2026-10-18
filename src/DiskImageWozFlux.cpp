// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiskImage.h"
#include "Block512Backing.h"
#include <algorithm>
#include <cstring>
#include <utility>

void DiskImage::writeWozFlux(int qt, int64_t start, int64_t end, int count,
                             const int64_t* transitions, int64_t revolutionStart)
{
    const int period = trackPeriod(qt);
    if (period <= 0 || end <= start) return;
    const int64_t origin = revolutionStart >= 0 ? revolutionStart : 0;
    const int64_t begin = ((start - origin) % period + period) % period;
    const int64_t duration = std::min<int64_t>(end - start, period);
    const int64_t limitedEnd = start + duration;
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
