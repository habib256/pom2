// POM2 — GPL-3.0-or-later
#include "CffaCard.h"
#include "SessionIO.h"
#include <algorithm>
namespace pom2 {
void CffaCard::appendSessionIdentity(std::vector<uint8_t>& out) const
{
    out.push_back(romLoaded_ ? 1 : 0); out.insert(out.end(), rom_.begin(), rom_.end());
}
bool CffaCard::installSessionIdentity(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != rom_.size() + 1 || bytes[0] > 1) return false;
    romLoaded_ = bytes[0] != 0; std::copy_n(bytes.data() + 1, rom_.size(), rom_.begin()); return true;
}
bool CffaCard::appendSessionState(std::vector<uint8_t>& out, std::string& error) const
{
    sessionio::Blob runtime, media;
    appendSnapshotState(runtime);
    if (!ata_.backing().appendSessionMedia(media, error)) return false;
    sessionio::blob(out, runtime); out.push_back(ata_.sessionAdvancesTaskFile() ? 1 : 0);
    sessionio::blob(out, media); return true;
}
bool CffaCard::prepareSessionState(const std::vector<uint8_t>& bytes, CffaCard& prepared, std::string& error) const
{
    if (!ata_.backing().sessionReplaceable()) { error = "CFFA medium has unsaved changes or pending writes"; return false; }
    sessionio::Reader r(bytes.data(), bytes.size());
    sessionio::Blob runtime, media;
    if (!r.blob(runtime, 9 + AtaBlockDevice::kSnapshotBytes)) {
        error = "invalid CFFA runtime"; return false;
    }
    const auto advance = r.u8();
    if (advance > 1 || runtime.size() != 9 + AtaBlockDevice::kSnapshotBytes ||
        !r.blob(media, (32u << 20) - 24) || !r.done()) { error = "invalid CFFA session"; return false; }
    prepared.loadSnapshotState(runtime.data(), runtime.size());
    sessionio::Blob canonical; prepared.appendSnapshotState(canonical);
    if (canonical != runtime || !prepared.ata_.backing().loadSessionMedia(media.data(), media.size(), error)) {
        if (error.empty()) error = "invalid CFFA register state";
        return false;
    }
    prepared.ata_.setSessionAdvancesTaskFile(advance != 0); return true;
}
void CffaCard::applySessionState(CffaCard& prepared)
{
    sessionio::Blob runtime; prepared.appendSnapshotState(runtime);
    ata_.backing().swapSessionMedia(prepared.ata_.backing());
    loadSnapshotState(runtime.data(), runtime.size());
    ata_.setSessionAdvancesTaskFile(prepared.ata_.sessionAdvancesTaskFile());
}
}
