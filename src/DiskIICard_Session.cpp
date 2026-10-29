// POM2 — GPL-3.0-or-later
#include "DiskIICard.h"
#include "CpuClock.h"
#include "IWMDevice.h"
#include "SessionIO.h"

#include <memory>
#include <algorithm>
#include <cstdint>
#include <utility>

void DiskIICard::appendSessionIdentity(std::vector<uint8_t>& out) const
{
    out.push_back(1);
    out.push_back(iwmHost_ ? 1 : 0);
    auto prom = [&](bool loaded, const auto& bytes) {
        out.push_back(loaded ? 1 : 0);
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    prom(bootRomLoaded, bootRom);
    prom(bootRom13Loaded, bootRom13);
    prom(p6RomLoaded, p6Rom);
    prom(p6Rom13Loaded, p6Rom13);
}

bool DiskIICard::installSessionIdentity(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != 2 + 4 + bootRom.size() + bootRom13.size() + p6Rom.size() + p6Rom13.size() ||
        bytes[0] != 1 || bytes[1] > 1) return false;
    size_t pos = 2;
    auto prom = [&](bool& loaded, auto& rom) {
        if (bytes[pos] > 1) return false;
        loaded = bytes[pos++] != 0;
        std::copy_n(bytes.data() + pos, rom.size(), rom.begin()); pos += rom.size();
        return true;
    };
    iwmHost_ = bytes[1] != 0;
    const bool ok = prom(bootRomLoaded, bootRom) && prom(bootRom13Loaded, bootRom13) &&
                    prom(p6RomLoaded, p6Rom) && prom(p6Rom13Loaded, p6Rom13);
    lssTable_ = std::any_of(p6Rom.begin(), p6Rom.end(), [](uint8_t b) { return b != 0; });
    return ok;
}

bool DiskIICard::appendSessionState(std::vector<uint8_t>& out, std::string& error) const
{
    std::vector<uint8_t> b, runtime;
    appendSnapshotState(runtime);
    // D2S1 v4's fixed controller header is followed by two optional nibble
    // captures. Bind their path hashes to the private session medium name.
    size_t pos = 83;
    uint64_t identity = 14695981039346656037ULL;
    for (unsigned char c : std::string("(session disk)")) { identity ^= c; identity *= 1099511628211ULL; }
    for (int d = 0; d < kDriveCount; ++d) {
        if (runtime[pos++]) {
            for (int i = 0; i < 8; ++i) runtime[pos + size_t(i)] = uint8_t(identity >> (i * 8));
            pos += 8 + DiskImage::kMediaSnapshotBytes;
        }
    }
    pom2::sessionio::blob(b, runtime);
    for (int d = 0; d < kDriveCount; ++d) {
        std::vector<uint8_t> media;
        if (!images[d].appendSessionMedia(media, error)) return false;
        if (!ejecting_[d].expired() || !images[d].sessionCaptureReady()) {
            error = "Disk II eject/write-back is still pending"; return false;
        }
        pom2::sessionio::blob(b, media);
    }
    out.insert(out.end(), b.begin(), b.end());
    return true;
}

bool DiskIICard::prepareSessionState(const uint8_t* data, size_t len,
                                    SessionRestore& out, std::string& error, bool allowWoz, bool allowWritable) const
{
    for (int d = 0; d < kDriveCount; ++d)
        if (images[d].hasPendingWriteBack() ||
            !ejecting_[d].expired()) {
            error = "Disk II has unsaved changes or a pending eject"; return false;
        }
    pom2::sessionio::Reader r(data, len);
    SessionRestore staged;
    if (!r.blob(staged.runtime, 1024 * 1024)) {
        error = "invalid session Disk II runtime"; return false;
    }
    auto validator = std::make_unique<DiskIICard>(slot_);
    validator->bootRom = bootRom; validator->bootRomLoaded = bootRomLoaded;
    validator->bootRom13 = bootRom13; validator->bootRom13Loaded = bootRom13Loaded;
    validator->p6Rom = p6Rom; validator->p6RomLoaded = p6RomLoaded;
    validator->lssTable_ = lssTable_;
    validator->p6Rom13 = p6Rom13; validator->p6Rom13Loaded = p6Rom13Loaded;
    validator->iwmHost_ = iwmHost_;
    for (int d = 0; d < kDriveCount; ++d) {
        std::vector<uint8_t> media;
        if (!r.blob(media, allowWoz ? 16u * 1024u * 1024u + 4096 : 256u * 1024u)) {
            error = "invalid session Disk II media length"; return false;
        }
        if (!validator->images[d].loadSessionMedia(media.data(), media.size(), error, allowWritable)) return false;
        if (!allowWoz && validator->images[d].isWoz()) {
            error = "WOZ media requires session version 2"; return false;
        }
    }
    if (!r.done()) { error = "trailing session Disk II data"; return false; }
    validator->loadSnapshotState(staged.runtime.data(), staged.runtime.size());
    // Rewind blobs originate in-process; sessions are file input. A canonical
    // layout alone does not prove its clocks are safe: an active LSS with a
    // gigantic catch-up gap would hold the CPU lock for billions of ticks.
    constexpr uint64_t maxClock = uint64_t(INT64_MAX) / 4;
    const uint64_t target = validator->cpuCycleTotal <= maxClock
        ? validator->cpuCycleTotal * 2 : 0;
    bool valid = validator->cpuCycleTotal <= maxClock &&
        validator->lssCycle <= target + 32 &&
        validator->motorOffDelay >= 0 && validator->motorOffDelay <= 2 * POM2_CPU_CLOCK_HZ;
    for (int d = 0; d < kDriveCount; ++d) {
        const auto anchor = validator->revolutionStartLssCycle[d];
        valid = valid && (anchor == kNeverRev ||
            (anchor >= 0 && uint64_t(anchor) <= target + 32));
    }
    if (validator->useBitLss && validator->active != MODE_IDLE &&
        validator->images[validator->activeDrive].isLoaded()) {
        valid = valid && (validator->lssCycle >= target || target - validator->lssCycle <= 400000);
    }
    if (validator->writePosition > 0) {
        valid = valid && validator->writeStartTime >= 0 &&
            uint64_t(validator->writeStartTime) <= validator->lssCycle + 32;
        for (int i = 0; i < validator->writePosition; ++i)
            valid = valid && validator->writeBuffer[i] >= validator->writeStartTime &&
                uint64_t(validator->writeBuffer[i]) <= validator->lssCycle + 32;
    }
    if (!valid) { error = "invalid session Disk II timeline"; return false; }
    std::vector<uint8_t> canonical;
    validator->appendSnapshotState(canonical);
    if (canonical != staged.runtime) {
        error = "invalid or unsupported session Disk II state"; return false;
    }
    for (int d = 0; d < kDriveCount; ++d)
        staged.media[d] = std::make_unique<DiskImage>(std::move(validator->images[d]));
    out = std::move(staged);
    return true;
}

void DiskIICard::applySessionState(SessionRestore&& prepared)
{
    // Caller preflighted every card and the CPU/MEX, under the same lock.
    // Outgoing media are clean; incoming media cannot write to host files.
    struct RestoreScope {
        pom2::IWMDevice* iwm;
        ~RestoreScope() { if (iwm) iwm->setRestoring(false); }
    } scope{iwm_};
    if (iwm_) iwm_->setRestoring(true);
    for (int d = 0; d < kDriveCount; ++d) images[d] = std::move(*prepared.media[d]);
    loadSnapshotState(prepared.runtime.data(), prepared.runtime.size());
}
