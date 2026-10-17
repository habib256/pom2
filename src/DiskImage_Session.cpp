// POM2 — GPL-3.0-or-later
#include "DiskImage.h"
#include "SessionIO.h"

#include <cstring>
#include <memory>
#include <utility>

bool DiskImage::appendReadOnlySession(std::vector<uint8_t>& out, std::string& error) const
{
    if (loaded && (!isFileWriteProtected() || hasPendingWriteBack())) {
        error = "sessions require clean, physically write-protected disks";
        return false;
    }
    if (loaded && wozFormat) {
        if (wozRaw.size() < 12 || wozRaw.size() > 16u * 1024u * 1024u) {
            error = "invalid session WOZ source size"; return false;
        }
        out.insert(out.end(), {'D','W','O','1'});
        out.insert(out.end(), wozRaw.begin(), wozRaw.end());
        return true;
    }
    std::vector<uint8_t> b{'D','R','O','1', uint8_t(loaded)};
    if (loaded) {
        b.push_back(uint8_t(sectorsPerTrack_));
        b.push_back(sectorOrder == SectorOrder::ProDOS ? 1 : 0);
        b.push_back(nibFormat ? 1 : 0);
        for (const auto& track : tracks) b.insert(b.end(), track.begin(), track.end());
    }
    out.insert(out.end(), b.begin(), b.end());
    return true;
}

bool DiskImage::loadReadOnlySession(const uint8_t* data, size_t len, std::string& error)
{
    if (data && len >= 4 && !std::memcmp(data, "DWO1", 4)) {
        if (len < 16 || len - 4 > 16u * 1024u * 1024u) {
            error = "invalid session WOZ length"; return false;
        }
        if (hasPendingWriteBack()) { error = "outgoing session disk has pending changes"; return false; }
        auto staged = std::make_unique<DiskImage>();
        staged->writeBackEnabled = false;
        if (!staged->loadWozBytes(std::vector<uint8_t>(data + 4, data + len), "(session disk)")) {
            error = staged->lastError; return false;
        }
        // Host notch protection need not have been set in WOZ INFO. Every
        // embedded medium remains physically protected after restoration.
        staged->fileWriteProtected = true;
        *this = std::move(*staged);
        return true;
    }
    if (!data || len < 5 || std::memcmp(data, "DRO1", 4) || data[4] > 1) {
        error = "invalid session disk header"; return false;
    }
    const bool present = data[4] != 0;
    const size_t expected = present ? 8 + size_t(kTracks) * kNibblesPerTrack : 5;
    if (len != expected || (present &&
        ((data[5] != 13 && data[5] != 16) || data[6] > 1 || data[7] > 1))) {
        error = "invalid session disk length or geometry"; return false;
    }
    // Never flush/replace the only copy of an outgoing dirty medium.
    if (hasPendingWriteBack()) { error = "outgoing session disk has pending changes"; return false; }
    auto staged = std::make_unique<DiskImage>();
    staged->writeBackEnabled = false;
    if (present) {
        staged->loaded = true;
        staged->fileWriteProtected = true;
        staged->sectorsPerTrack_ = data[5];
        staged->sectorOrder = data[6] ? SectorOrder::ProDOS : SectorOrder::Dos33;
        staged->nibFormat = data[7] != 0;
        staged->path = "(session disk)";
        size_t pos = 8;
        for (auto& track : staged->tracks) {
            std::memcpy(track.data(), data + pos, kNibblesPerTrack);
            pos += kNibblesPerTrack;
        }
        staged->invalidateAllBitStreams();
    }
    *this = std::move(*staged);
    return true;
}

size_t DiskImage::sessionStorageBytes() const
{
    size_t bytes = sizeof(DiskImage) + wozRaw.capacity();
    for (const auto& bits : bitStream) bytes += bits.capacity();
    for (const auto& flux : fluxStream) bytes += flux.capacity() * sizeof(int);
    return bytes;
}
