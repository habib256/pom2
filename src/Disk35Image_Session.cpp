// POM2 — GPL-3.0-or-later
#include "Disk35Image.h"
#include "Disk35Bitstream.h"
#include "SessionIO.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace pom2 {
bool Disk35Image::appendSessionMedia(std::vector<uint8_t>& out, std::string& error) const {
    if (!sessionCaptureReady()) { error = "3.5-inch write-back is still pending"; return false; }
    if (!loaded_ || (isFileWriteProtected() && !dirty_ && !detachedSession_))
        return appendReadOnlySession(out, error);
    Disk35Image image = *this;
    image.dirty_ = false; image.fileWriteProtected_ = true;
    std::vector<uint8_t> media;
    if (!image.appendReadOnlySession(media, error)) return false;
    out.insert(out.end(), {'D','M','S',1, uint8_t(uint8_t(fileWriteProtected_) |
        (uint8_t(hostReadOnly_) << 1) | (uint8_t(writeBackEnabled_) << 2))});
    sessionio::blob(out, media);
    return true;
}
bool Disk35Image::loadSessionMedia(const uint8_t* data, size_t size, std::string& error, bool allowWritable) {
    if (!data || size < 4 || std::memcmp(data, "DMS\1", 4)) return loadReadOnlySession(data, size, error);
    if (!allowWritable) { error = "writable media requires session version 5"; return false; }
    if (!sessionReplaceable()) { error = "outgoing 3.5-inch disk has pending changes"; return false; }
    sessionio::Reader r(data, size); r.pos = 4;
    const uint8_t flags = r.u8();
    std::vector<uint8_t> media;
    if (flags > 7 || !r.blob(media, (16u << 20) + 9) || !r.done()) {
        error = "invalid writable 3.5-inch session record"; return false;
    }
    Disk35Image prepared; prepared.mfmCapable_ = mfmCapable_;
    if (!prepared.loadReadOnlySession(media.data(), media.size(), error)) return false;
    if (!prepared.loaded_) { error = "writable session 3.5-inch disk is empty"; return false; }
    if (prepared.bitstream_ && !(flags & 1)) {
        const auto& raw = prepared.bitstream_->raw;
        for (size_t p = 12; p + 8 <= raw.size();) {
            uint32_t count = 0;
            for (int i = 0; i < 4; ++i) count |= uint32_t(raw[p + 4 + i]) << (i * 8);
            if (count > raw.size() - p - 8) { error = "invalid session capture chunk"; return false; }
            if (count >= 3 && !std::memcmp(raw.data() + p, "INFO", 4) && raw[p + 10]) {
                error = "session protection contradicts capture header"; return false;
            }
            p += 8 + count;
        }
    }
    prepared.fileWriteProtected_ = flags & 1; prepared.hostReadOnly_ = flags & 2;
    prepared.writeBackEnabled_ = flags & 4; prepared.detachedSession_ = true;
    *this = std::move(prepared);
    return true;
}
size_t Disk35Image::sessionStorageBytes() const {
    size_t bytes = blocks_.size();
    if (moofCells_) for (const auto& track : *moofCells_) bytes += track.size();
    if (bitstream_) {
        bytes += bitstream_->raw.size();
        for (const auto& track : bitstream_->transitions) bytes += track.size() * sizeof(uint32_t);
    }
    return bytes;
}
bool Disk35Image::appendReadOnlySession(std::vector<uint8_t>& out, std::string& error) const {
    if (!sessionReplaceable() || (loaded_ && !isFileWriteProtected())) {
        error = "sessions require clean, physically write-protected 3.5-inch disks"; return false;
    }
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (loaded_) {
        if (bitstream_) {
            type = bitstream_->moof ? 5 : 4;
            if (std::any_of(bitstream_->changed.begin(), bitstream_->changed.end(), [](bool b) { return b; })) {
                if (!serializeBitstream(payload, error)) return false;
            } else payload = bitstream_->raw;
        } else {
            type = blockCount() == kBlockCount400 ? 1 : blockCount() == kBlockCount1440 ? 3 : 2;
            payload = blocks_;
        }
    }
    out.insert(out.end(), {'D','3','5',1,type});
    sessionio::blob(out, payload);
    return true;
}
bool Disk35Image::loadReadOnlySession(const uint8_t* data, size_t size, std::string& error) {
    if (!sessionReplaceable()) { error = "3.5-inch disk has pending changes"; return false; }
    sessionio::Reader r(data, size);
    if (r.u8() != 'D' || r.u8() != '3' || r.u8() != '5' || r.u8() != 1) {
        error = "invalid session 3.5-inch header"; return false;
    }
    const auto type = r.u8();
    std::vector<uint8_t> payload;
    if (type > 5 || !r.blob(payload, 16u << 20) || !r.done() || (!type && !payload.empty())) {
        error = "invalid session 3.5-inch media"; return false;
    }
    Disk35Image prepared;
    prepared.mfmCapable_ = mfmCapable_;
    if (type >= 4) {
        if (payload.size() < 12 || std::memcmp(payload.data(), type == 5 ? "MOOF" : "WOZ2", 4) ||
            !prepared.loadBitstream(payload, "(session disk)", type == 5)) {
            error = "invalid session 3.5-inch capture: " + prepared.lastError_; return false;
        }
        prepared.kind_ = type == 4 ? ImageKind::Woz35 :
            prepared.blockCount() == kBlockCount400 ? ImageKind::Moof400k :
            prepared.blockCount() == kBlockCount1440 ? ImageKind::Moof1440k : ImageKind::Moof35;
    } else if (type) {
        const size_t bytes = type == 1 ? kBytesPerImage400 : type == 3 ? kBytesPerImage1440 : kBytesPerImage;
        if (payload.size() != bytes || (type == 3 && !mfmCapable_)) {
            error = "invalid session 3.5-inch geometry"; return false;
        }
        prepared.blocks_ = std::move(payload);
        prepared.kind_ = type == 1 ? ImageKind::Raw400k : type == 3 ? ImageKind::Raw1440k : ImageKind::Raw800k;
    }
    prepared.loaded_ = type != 0;
    if (prepared.loaded_) {
        prepared.path_ = "(session disk)";
        prepared.hostReadOnly_ = true;
        prepared.fileWriteProtected_ = true; // archive media can never acquire a host write target
        prepared.lineage_ = nextMediaCaptureSeq();
    }
    *this = std::move(prepared);
    return true;
}
}
