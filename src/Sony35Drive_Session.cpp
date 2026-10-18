// POM2 — GPL-3.0-or-later
#include "Sony35Drive.h"
#include "SessionIO.h"
#include <cstring>

namespace pom2 {
void Sony35Drive::appendSessionWriteState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), {'S','W','S',1, uint8_t(writeActive_), uint8_t(writeTrack_), uint8_t(writeHead_)});
    byteio::putU64(out, uint64_t(writeCursorTick_));
    byteio::putU32(out, uint32_t(writeCursorCell_));
    // Raw GCR images project only complete sectors back to blocks. The cache
    // can therefore contain a partially written field not yet in the image.
    const bool cache = (writeActive_ || writeCursorTick_ != INT64_MIN) && !cells_.empty();
    byteio::putU32(out, uint32_t(cache ? cachedTrack_ : -1));
    byteio::putU32(out, uint32_t(cache ? cachedHead_ : -1));
    byteio::putU64(out, uint64_t(cache ? cachedCyclesPerRev_ : 0));
    if (cache) sessionio::blob(out, cells_);
    else sessionio::blob(out, {});
}
bool Sony35Drive::loadSessionWriteState(const uint8_t* data, size_t len, bool commit)
{
    if (!data || len < 39 || std::memcmp(data, "SWS\1", 4)) return false;
    sessionio::Reader r(data, len); r.pos = 4;
    const uint8_t active = r.u8(), track = r.u8(), head = r.u8();
    auto u64 = [&] { const auto low = r.u32(); return uint64_t(low) | (uint64_t(r.u32()) << 32); };
    const int64_t tick = int64_t(u64());
    const uint32_t cell = r.u32(), cachedTrack = r.u32(), cachedHead = r.u32();
    const uint64_t period = u64();
    const uint32_t cells = r.u32();
    if (!r.need(cells) || cells > (1u << 20) || r.pos + cells != len || active > 1 || track >= 80 || head >= 2 ||
        (tick != INT64_MIN && (tick < 0 || tick > INT64_MAX / 4)) || cell > (1u << 20) ||
        (cells && (cachedTrack >= 80 || cachedHead >= 2 || !period || period > 40000000 || period * 32 < cells)) ||
        (!cells && (cachedTrack != UINT32_MAX || cachedHead != UINT32_MAX || period))) return false;
    for (size_t i = r.pos; i < len; ++i) if (data[i] > 1) return false;
    if (!commit) return true;
    writeActive_ = active; writeTrack_ = track; writeHead_ = head;
    writeCursorTick_ = tick; writeCursorCell_ = int(cell);
    invalidateCache();
    if (cells) {
        cachedTrack_ = int(cachedTrack); cachedHead_ = int(cachedHead);
        cachedCyclesPerRev_ = int64_t(period); cachedCellsPerRev_ = int(cells);
        cells_.assign(data + r.pos, data + len);
        rebuildTransitionsFromCells();
    }
    return true;
}
}
