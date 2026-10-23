// POM2 — GPL-3.0-or-later
#include "DiskImage.h"
#include "SessionIO.h"
#include <cstring>
#include <memory>
#include <utility>

bool DiskImage::appendSessionMedia(std::vector<uint8_t>& out, std::string& error) const
{
    if (!sessionCaptureReady()) { error = "Disk II write-back is still pending"; return false; }
    if (!loaded || (isFileWriteProtected() && !anyDirty && !detachedSession_))
        return appendReadOnlySession(out, error);
    // Work on a detached copy: capture neither mutates the source container
    // nor retires its dirty flags, submits I/O or advances the media epoch.
    auto image = std::make_unique<DiskImage>(*this);
    if (wozFormat) {
        if (!image->applyWozFluxWriteBack()) { error = image->lastError; return false; }
        for (int qt = 0; qt < kQuarterTracks; ++qt) {
            if (!wozQtDirty[qt] || wozQtFluxPeriodTicks[qt]) continue;
            // The COPY's offset: a FLUX slot grown by applyWozFluxWriteBack
            // above moved its chunk layout, as it does for saveDirty.
            const size_t bits = wozQtBitCount[qt], offset = image->wozQtByteOff[qt];
            if (!bits || bitStream[qt].size() < bits || offset > image->wozRaw.size() ||
                (bits + 7) / 8 > image->wozRaw.size() - offset) {
                error = "session WOZ track cannot be serialized"; return false;
            }
            for (size_t bit = 0; bit < bits; ++bit) {
                auto& byte = image->wozRaw[offset + bit / 8];
                const uint8_t mask = uint8_t(1u << (7 - bit % 8));
                byte = bitStream[qt][bit] ? uint8_t(byte | mask) : uint8_t(byte & ~mask);
            }
        }
        if (anyDirty) for (int i = 8; i < 12; ++i) image->wozRaw[size_t(i)] = 0;
    }
    image->anyDirty = false;
    image->fileWriteProtected = true;
    std::vector<uint8_t> media, framing;
    if (!image->appendReadOnlySession(media, error)) return false;
    for (const auto& frame : writeFraming) {
        pom2::byteio::putU64(framing, uint64_t(frame.nextCycle));
        pom2::byteio::putU32(framing, uint32_t(frame.nibbleIdx));
        framing.push_back(frame.acc);
        framing.push_back(uint8_t(frame.accBits));
        pom2::byteio::putU64(framing, uint64_t(frame.origin));
        pom2::byteio::putU32(framing, uint32_t(frame.nextCell));
        framing.push_back(uint8_t(frame.heldValid)); framing.push_back(uint8_t(frame.heldBit));
        pom2::byteio::putU32(framing, uint32_t(frame.heldCell));
    }
    out.insert(out.end(), {'D','M','S',1, uint8_t(uint8_t(fileWriteProtected) |
        (uint8_t(hostReadOnly_) << 1) | (uint8_t(writeBackEnabled) << 2))});
    pom2::sessionio::blob(out, media); pom2::sessionio::blob(out, framing);
    return true;
}

bool DiskImage::loadSessionMedia(const uint8_t* data, size_t len, std::string& error, bool allowWritable)
{
    if (!data || len < 4 || std::memcmp(data, "DMS\1", 4)) return loadReadOnlySession(data, len, error);
    if (!allowWritable) { error = "writable media requires session version 5"; return false; }
    if (hasPendingWriteBack()) { error = "outgoing disk has pending changes"; return false; }
    pom2::sessionio::Reader r(data, len); r.pos = 4;
    const uint8_t flags = r.u8();
    std::vector<uint8_t> media, framing;
    if (flags > 7 || !r.blob(media, (16u << 20) + 4) || !r.blob(framing, 4096) || !r.done() ||
        framing.size() != size_t(kTracks) * 32) {
        error = "invalid writable session disk record"; return false;
    }
    auto image = std::make_unique<DiskImage>();
    if (!image->loadReadOnlySession(media.data(), media.size(), error)) return false;
    if (!image->loaded) { error = "writable session disk is empty"; return false; }
    // A container's physical lock may never be cleared by an outer flag.
    if (image->wozFormat && !(flags & 1)) {
        const auto& raw = image->wozRaw;
        for (size_t pos = 12; pos + 8 <= raw.size();) {
            uint32_t count = 0;
            for (int i = 0; i < 4; ++i) count |= uint32_t(raw[pos + 4 + i]) << (8 * i);
            if (count > raw.size() - pos - 8) { error = "invalid session WOZ chunk"; return false; }
            if (count >= 3 && !std::memcmp(raw.data() + pos, "INFO", 4) && raw[pos + 10]) {
                error = "session protection contradicts WOZ header"; return false;
            }
            pos += 8 + count;
        }
    }
    pom2::sessionio::Reader f(framing.data(), framing.size());
    auto u64 = [&] { const auto lo = f.u32(); return uint64_t(lo) | (uint64_t(f.u32()) << 32); };
    auto i32 = [&] { const uint32_t v = f.u32(); return v <= INT32_MAX ? int(v) : int(int64_t(v) - (int64_t(1) << 32)); };
    constexpr int64_t maxClock = INT64_MAX / 4;
    for (auto& frame : image->writeFraming) {
        frame.nextCycle = int64_t(u64()); frame.nibbleIdx = i32(); frame.acc = f.u8();
        frame.accBits = f.u8(); frame.origin = int64_t(u64()); frame.nextCell = i32();
        const uint8_t valid = f.u8(), bit = f.u8(); frame.heldValid = valid; frame.heldBit = bit;
        frame.heldCell = i32();
        if ((frame.nextCycle != INT64_MIN && (frame.nextCycle < 0 || frame.nextCycle > maxClock)) ||
            frame.origin < 0 || frame.origin > maxClock || frame.nibbleIdx < -1 || frame.nibbleIdx >= kNibblesPerTrack ||
            frame.accBits > 7 || frame.nextCell < -1 || frame.nextCell > (1 << 24) ||
            frame.heldCell < -1 || frame.heldCell > (1 << 24) || valid > 1 || bit > 1) {
            error = "invalid session nibble write framing"; return false;
        }
    }
    image->fileWriteProtected = flags & 1; image->hostReadOnly_ = flags & 2;
    image->writeBackEnabled = flags & 4; image->detachedSession_ = true;
    *this = std::move(*image);
    return true;
}
