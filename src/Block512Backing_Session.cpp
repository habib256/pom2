// POM2 — GPL-3.0-or-later
#include "Block512Backing.h"
#include "ByteIO.h"
#include "MediaAutosave.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace pom2 {
bool Block512Backing::appendSessionMedia(std::vector<uint8_t>& out, std::string& error) const
{
    if (!sessionCaptureReady() || synth_) {
        error = "block sessions require idle file media (host folders are unsupported)"; return false;
    }
    std::vector<uint8_t> bytes{'B','M','S',1,
        uint8_t((wpHeader_ ? 1 : 0) | (hostReadOnly_ ? 2 : 0) | (writeBack_ ? 4 : 0)), uint8_t(loaded_)};
    byteio::putU32(bytes, uint32_t(blockCount()));
    byteio::putU32(bytes, 0);
    uint32_t records = 0;
    for (uint32_t block = 0; block < blockCount(); ++block) {
        auto begin = image_.begin() + block * kBlockBytes;
        if (!dirtyBlocks_[block] && std::all_of(begin, begin + kBlockBytes, [](uint8_t b) { return b == 0; })) continue;
        // Keep the same archive ceiling as browser imports. Dense media may
        // exceed it; refuse before copying rather than produce an unusable file.
        if (bytes.size() + 517 > (32u << 20) - 24) {
            error = "block session exceeds 32 MiB archive limit"; return false;
        }
        byteio::putU32(bytes, block);
        bytes.push_back(dirtyBlocks_[block] ? 1 : 0);
        bytes.insert(bytes.end(), begin, begin + kBlockBytes);
        ++records;
    }
    for (int i = 0; i < 4; ++i) bytes[10 + i] = uint8_t(records >> (8 * i));
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
}

bool Block512Backing::loadSessionMedia(const uint8_t* data, size_t size, std::string& error)
{
    auto invalid = [&]() { error = "invalid sparse block session medium"; return false; };
    if (!sessionReplaceable()) { error = "block medium has pending host writes"; return false; }
    if (!data || size < 14 || size > (32u << 20) - 24 || std::memcmp(data, "BMS\1", 4)) return invalid();
    byteio::Reader r(data, size); r.pos = 4;
    const uint8_t flags = r.u8(), loaded = r.u8();
    const uint32_t blocks = r.u32(), records = r.u32();
    if (flags > 7 || loaded > 1 || blocks > kMaxBlocks || records > blocks ||
        (loaded ? blocks == 0 : blocks != 0) || size - 14 != size_t(records) * 517) return invalid();
    // Validate before allocating or touching the target. Strict order also
    // rejects duplicate blocks and gives one canonical representation.
    uint32_t previous = 0;
    for (uint32_t i = 0; i < records; ++i) {
        const uint32_t block = r.u32(); const uint8_t dirty = r.u8();
        if (block >= blocks || (i && block <= previous) || dirty > 1 ||
            (!dirty && std::all_of(data + r.pos, data + r.pos + kBlockBytes, [](uint8_t b) { return b == 0; }))) return invalid();
        previous = block; r.pos += kBlockBytes;
    }
    std::vector<uint8_t> image(size_t(blocks) * kBlockBytes, 0);
    std::vector<bool> dirty(blocks, false);
    r.pos = 14;
    for (uint32_t i = 0; i < records; ++i) {
        const uint32_t block = r.u32(); dirty[block] = r.u8() != 0;
        std::copy_n(data + r.pos, kBlockBytes, image.begin() + block * kBlockBytes); r.pos += kBlockBytes;
    }
    eject();
    image_ = std::move(image); dirtyBlocks_ = std::move(dirty);
    blockVersions_.assign(blocks, 0);
    anyDirty_ = std::find(dirtyBlocks_.begin(), dirtyBlocks_.end(), true) != dirtyBlocks_.end();
    loaded_ = loaded != 0; detachedSession_ = true;
    wpHeader_ = flags & 1; hostReadOnly_ = flags & 2; writeBack_ = flags & 4;
    dataLength_ = image_.size(); path_ = loaded_ ? "(session disk)" : "";
    mountId_ = loaded_ ? nextMediaCaptureSeq() : 0;
    return true;
}

void Block512Backing::swapSessionMedia(Block512Backing& prepared)
{
    // Preflight admitted only quiescent backings. Preserve the live executor;
    // every persistence entry point independently refuses detached media.
    using std::swap;
    swap(image_, prepared.image_); swap(dirtyBlocks_, prepared.dirtyBlocks_);
    swap(blockVersions_, prepared.blockVersions_); swap(anyDirty_, prepared.anyDirty_);
    swap(wpHeader_, prepared.wpHeader_); swap(hostReadOnly_, prepared.hostReadOnly_);
    swap(writeBack_, prepared.writeBack_); swap(loaded_, prepared.loaded_);
    swap(path_, prepared.path_); swap(mountId_, prepared.mountId_);
    detachedSession_ = true; supportsWriteBack_ = false;
    headerBytes_.clear(); dataOffset_ = 0; dataLength_ = image_.size();
    synth_ = false; hostTextConversion_ = false; hostFolder_.clear();
    hasMountTime_ = false; preserved_.reset();
    ++mountGeneration_; persistenceError_.clear(); nextWriteBack_ = {};
    activityTicks_.store(0, std::memory_order_relaxed);
}
} // namespace pom2
