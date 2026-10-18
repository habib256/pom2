// POM2 — GPL-3.0-or-later
// Applesauce MOOF 1.0 / WOZ 2.1: TRKS/FLUX, delta continuation and CRC32.
// https://applesaucefdc.com/moof-reference/ ; /woz/reference2/
#include "Disk35Image.h"
#include "Disk35Bitstream.h"
#include "Disk35Mfm.h"
#include "Block512Backing.h"
#include "Sony35Gcr.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace pom2 {
bool Disk35Image::writeMfmBlock(uint32_t idx, const uint8_t* in) {
    if (!bitstream_ || !moofCells_ || !in) return false;
    const int slot = int(idx / 18), sector = int(idx % 18) + 1;
    const auto& original = (*moofCells_)[size_t(slot)];
    if (original.empty()) return false;
    // Find every CRC-valid copy of the sector, including a field straddling
    // index. Never invent a format for a missing/corrupt sector in a capture.
    auto circular = original;
    const size_t overlap = std::min<size_t>(original.size(), 40000);
    for (size_t i = 0; i < overlap; ++i) circular.push_back(original[i]);
    std::vector<size_t> fields;
    mfm35::decodeMfmTrack(circular, slot / 2, slot % 2,
        [&](int found, const uint8_t*, size_t mark) {
            if (found == sector) fields.push_back(mark % original.size());
        });
    std::sort(fields.begin(), fields.end());
    fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
    if (fields.empty()) return false;
    struct Splice { int64_t start, end; std::vector<int64_t> ticks; };
    std::vector<Splice> splices;
    for (size_t mark : fields) {
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < 3; ++i) crc = mfm35::mfmCrc(crc, 0xA1);
        const uint8_t marker = mfm35::mfmByte(circular, mark);
        crc = mfm35::mfmCrc(crc, marker);
        std::vector<uint8_t> payload(in, in + 512);
        for (auto byte : payload) crc = mfm35::mfmCrc(crc, byte);
        payload.push_back(uint8_t(crc >> 8)); payload.push_back(uint8_t(crc));
        const size_t first = mark + 16;
        Splice splice{int64_t(first * moofBitTiming_), 0, {}};
        size_t cell = first;
        bool previous = marker & 1;
        auto emit = [&](bool bit) {
            if (bit) splice.ticks.push_back(int64_t(cell * moofBitTiming_));
            ++cell;
        };
        for (auto byte : payload) for (int bit = 7; bit >= 0; --bit) {
            const bool data = (byte >> bit) & 1;
            emit(!previous && !data); emit(data); previous = data;
        }
        // The following byte's first clock depends on the new CRC low bit.
        emit(!previous && !original[(cell + 1) % original.size()]);
        splice.end = int64_t(cell * moofBitTiming_);
        splices.push_back(std::move(splice));
    }
    for (const auto& splice : splices)
        if (!writeFlux125ns(slot / 2, slot % 2, splice.start, splice.end,
                           splice.ticks.data(), int(splice.ticks.size()),
                           bitstream_->periods[size_t(slot)])) return false;
    refreshDecodedTrack(slot);
    return true;
}
uint32_t Disk35Image::trackPeriod125ns(int track, int head) const {
    return bitstream_ && track >= 0 && track < 80 && head >= 0 && head < headCount()
        ? bitstream_->periods[size_t(track * 2 + head)] : 0;
}
const std::vector<uint32_t>* Disk35Image::originalFluxTicks(int track, int head) const {
    if (!trackPeriod125ns(track, head)) return nullptr;
    return &bitstream_->transitions[size_t(track * 2 + head)];
}
bool Disk35Image::writeFlux125ns(int track, int head, int64_t start, int64_t end,
                                const int64_t* transitions, int count, uint32_t period) {
    if (!loaded_ || !bitstream_ || isWriteProtected() || track < 0 || track >= 80 ||
        head < 0 || head >= headCount() || end <= start || count < 0 || count > (1 << 20) ||
        (count && !transitions)) return false;
    const size_t slot = size_t(track * 2 + head);
    if (bitstream_->periods[slot]) period = bitstream_->periods[slot];
    if (!period || !moofBitTiming_ || period > uint32_t(moofBitTiming_) * (1u << 20)) return false;
    const uint64_t duration = std::min<uint64_t>(uint64_t(end) - uint64_t(start), period);
    const int64_t begin = end - int64_t(duration);
    const uint32_t origin = uint32_t((begin % period + period) % period);
    std::vector<uint32_t> replacement;
    replacement.reserve(size_t(count));
    for (int i = 0; i < count; ++i)
        if (transitions[i] >= begin && transitions[i] < end)
            replacement.push_back(uint32_t((origin + uint64_t(transitions[i] - begin)) % period));
    std::sort(replacement.begin(), replacement.end());
    replacement.erase(std::unique(replacement.begin(), replacement.end()), replacement.end());
    auto next = bitstream_->transitions[slot];
    next.erase(std::remove_if(next.begin(), next.end(), [&](uint32_t tick) {
        return (uint64_t(tick) + period - origin) % period < duration;
    }), next.end());
    next.insert(next.end(), replacement.begin(), replacement.end());
    std::sort(next.begin(), next.end());
    if (next == bitstream_->transitions[slot] && bitstream_->periods[slot]) return true;
    if (bitstream_.use_count() > 1) bitstream_ = std::make_shared<Disk35Bitstream>(*bitstream_);
    if (moofCells_.use_count() > 1) moofCells_ = std::make_shared<TrackCells>(*moofCells_);
    bitstream_->transitions[slot] = std::move(next);
    bitstream_->periods[slot] = period; bitstream_->changed[slot] = true;
    auto& cells = (*moofCells_)[slot];
    cells.assign(size_t((period + moofBitTiming_ / 2) / moofBitTiming_), 0);
    for (auto tick : bitstream_->transitions[slot]) {
        const size_t cell = size_t((tick + moofBitTiming_ / 2) / moofBitTiming_);
        if (cell < cells.size()) cells[cell] = 1;
    }
    decodedTrackDirty_[slot] = true;
    dirty_ = true; autosave_.noteWrite(); noteMediaWrite();
    return true;
}
void Disk35Image::refreshDecodedTrack(int slot) const {
    if (slot < 0 || slot >= 160 || !decodedTrackDirty_[size_t(slot)] || !moofCells_) return;
    const int track = slot / 2, head = slot % 2;
    const int first = blockIndexFor(track, head, 0);
    if (first < 0) return;
    const bool mfm = kind_ == ImageKind::Moof1440k;
    std::fill(blocks_.begin() + first * 512, blocks_.begin() + (first + (mfm ? 18 : sectorsForTrack(track))) * 512, 0);
    auto cells = (*moofCells_)[size_t(slot)];
    const size_t count = std::min<size_t>(cells.size(), 40000);
    cells.reserve(cells.size() + count);
    for (size_t i = 0; i < count; ++i) cells.push_back(cells[i]);
    if (mfm) {
        mfm35::decodeMfmTrack(cells, track, head, [&](int sector, const uint8_t* data, size_t) {
            std::memcpy(blocks_.data() + (first + sector - 1) * 512, data, 512);
        });
    } else sony35::decodeSectors(sony35::nibblesFromCells(cells), track,
        [&](int tr, int side, int sector, const uint8_t* data) {
            if (side != head) return;
            const int block = blockIndexFor(tr, side, sector);
            if (block >= 0 && size_t(block) < blockCount()) std::memcpy(blocks_.data() + block * 512, data, 512);
        });
    decodedTrackDirty_[size_t(slot)] = false;
}
bool Disk35Image::serializeBitstream(std::vector<uint8_t>& out, std::string& error) const {
    auto fail = [&](const char* why) { error = why; out.clear(); return false; };
    if (!bitstream_) return fail("No captured track data.");
    const auto& raw = bitstream_->raw;
    auto u16 = [&](size_t p) { return uint16_t(raw[p] | (uint16_t(raw[p + 1]) << 8)); };
    auto u32 = [&](size_t p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); };
    size_t info = 0, infoLen = 0, tmap = 0, trks = 0, end = 0, flux = 0, fluxEnd = 0;
    for (size_t p = 12; p < raw.size();) {
        if (raw.size() - p < 8 || u32(p + 4) > raw.size() - p - 8) return fail("Truncated bitstream chunk.");
        const size_t len = u32(p + 4);
        if (!std::memcmp(raw.data() + p, "INFO", 4)) { info = p + 8; infoLen = len; }
        if (!std::memcmp(raw.data() + p, "TMAP", 4) && len >= 160) tmap = p + 8;
        if (!std::memcmp(raw.data() + p, "TRKS", 4) && len >= 1280) { trks = p + 8; end = p + 8 + len; }
        if (!std::memcmp(raw.data() + p, "FLUX", 4) && len >= 160) { flux = p; fluxEnd = p + 8 + len; }
        p += 8 + len;
    }
    const size_t fields = bitstream_->moof ? 40 : 46;
    if (!info || infoLen < fields + 4 || !tmap || !trks || end % 512 ||
        info >= trks || tmap >= trks || (flux && flux < end))
        return fail("Capture layout cannot be rewritten safely.");
    out.assign(raw.begin(), raw.begin() + end);
    auto put16 = [&](size_t p, size_t v) { out[p] = uint8_t(v); out[p + 1] = uint8_t(v >> 8); };
    auto put32 = [&](size_t p, size_t v) { put16(p, v); put16(p + 2, v >> 16); };
    std::array<uint8_t, 160> bits{}, fluxMap{};
    std::copy_n(raw.data() + tmap, 160, bits.begin());
    fluxMap.fill(0xFF);
    if (flux) std::copy_n(raw.data() + flux + 8, 160, fluxMap.begin());
    for (size_t slot = 0; slot < 160; ++slot) {
        const uint8_t index = fluxMap[slot] != 0xFF ? fluxMap[slot] : bits[slot];
        if (index == 0xFF) continue;
        if (index >= 160) return fail("Invalid capture track map.");
        const size_t entry = trks + size_t(index) * 8;
        const size_t offset = size_t(u16(entry)) * 512, length = size_t(u16(entry + 2)) * 512;
        if (offset < trks + 1280 || offset > end || length > end - offset)
            return fail("Track data lies outside its TRKS chunk.");
    }
    constexpr size_t maxBytes = 16u * 1024u * 1024u;
    for (size_t slot = 0; slot < 160; ++slot) {
        if (!bitstream_->changed[slot]) continue;
        std::array<bool, 160> used{};
        for (size_t other = 0; other < 160; ++other) {
            if (other == slot) continue;
            const uint8_t index = fluxMap[other] != 0xFF ? fluxMap[other] : bits[other];
            if (index < 160) used[index] = true;
        }
        const auto free = std::find(used.begin(), used.end(), false);
        if (free == used.end()) return fail("No free capture track descriptor.");
        const uint8_t current = fluxMap[slot] != 0xFF ? fluxMap[slot] : bits[slot];
        const size_t index = current < 160 && !used[current] ? size_t(current) : size_t(free - used.begin());
        std::vector<uint8_t> encoded;
        auto delta = [&](uint32_t ticks) {
            while (ticks >= 255) { encoded.push_back(255); ticks -= 255; }
            encoded.push_back(uint8_t(ticks));
        };
        uint32_t previous = 0;
        for (auto tick : bitstream_->transitions[slot]) { delta(tick - previous); previous = tick; }
        delta(bitstream_->periods[slot] - previous);
        const size_t blocks = (encoded.size() + 511) / 512;
        if (encoded.size() > (1u << 20) || blocks > 65535 || out.size() / 512 > 65535)
            return fail("Capture rewrite exceeds the 16 MiB limit.");
        const size_t entry = trks + index * 8;
        const size_t oldOffset = (size_t(out[entry]) | (size_t(out[entry + 1]) << 8)) * 512;
        const size_t oldBlocks = size_t(out[entry + 2]) | (size_t(out[entry + 3]) << 8);
        bool reuse = index == current && oldOffset >= trks + 1280 && oldOffset <= out.size() &&
                     oldBlocks >= blocks && oldBlocks * 512 <= out.size() - oldOffset;
        for (size_t other = 0; reuse && other < 160; ++other) {
            if (!used[other]) continue;
            const size_t e = trks + other * 8;
            const size_t start = (size_t(out[e]) | (size_t(out[e + 1]) << 8)) * 512;
            const size_t length = (size_t(out[e + 2]) | (size_t(out[e + 3]) << 8)) * 512;
            if (oldOffset < start + length && start < oldOffset + oldBlocks * 512) reuse = false;
        }
        if (reuse) {
            std::copy(encoded.begin(), encoded.end(), out.begin() + oldOffset);
            std::fill(out.begin() + oldOffset + encoded.size(), out.begin() + oldOffset + oldBlocks * 512, 0);
        } else {
            if (blocks * 512 > maxBytes - out.size()) return fail("Capture rewrite exceeds the 16 MiB limit.");
            put16(entry, out.size() / 512); put16(entry + 2, blocks);
            out.insert(out.end(), encoded.begin(), encoded.end());
            out.resize((out.size() + 511) & ~size_t(511));
        }
        put32(entry + 4, encoded.size());
        bits[slot] = 0xFF; fluxMap[slot] = uint8_t(index);
    }
    put32(trks - 4, out.size() - trks);
    // Copy every ancillary chunk byte-for-byte. The old FLUX map is replaced
    // at a block boundary; its entries and aliases survive except written slots.
    if (flux) {
        out.insert(out.end(), raw.begin() + end, raw.begin() + flux);
        out.insert(out.end(), raw.begin() + fluxEnd, raw.end());
    } else out.insert(out.end(), raw.begin() + end, raw.end());
    if (out.size() % 512) {
        const size_t padding = (512 - ((out.size() + 8) % 512)) % 512;
        out.insert(out.end(), {'J','U','N','K',0,0,0,0}); put32(out.size() - 4, padding);
        out.resize(out.size() + padding, 0);
    }
    const size_t fluxBlock = out.size() / 512;
    out.insert(out.end(), {'F','L','U','X',0xF8,1,0,0}); // one block: 8 + 504
    out.insert(out.end(), fluxMap.begin(), fluxMap.end());
    out.resize(out.size() + 344, 0);
    if (out.size() > maxBytes || fluxBlock > 65535) return fail("Capture rewrite exceeds the 16 MiB limit.");
    std::copy(bits.begin(), bits.end(), out.begin() + tmap);
    if (!bitstream_->moof) out[info] = std::max<uint8_t>(3, out[info]);
    put16(info + fields, fluxBlock);
    size_t largestFlux = 0, largest = 0;
    for (size_t i = 0; i < 160; ++i) {
        const size_t entry = trks + i * 8;
        const size_t size = size_t(out[entry + 2]) | (size_t(out[entry + 3]) << 8);
        largest = std::max(largest, size);
        if (std::find(fluxMap.begin(), fluxMap.end(), uint8_t(i)) != fluxMap.end()) largestFlux = std::max(largestFlux, size);
    }
    put16(info + fields + 2, largestFlux);
    put16(info + (bitstream_->moof ? 38 : 44), largest);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 12; i < out.size(); ++i) {
        crc ^= out[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    put32(8, ~crc);
    return true;
}
}
