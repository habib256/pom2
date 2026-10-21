// POM2 — GPL-3.0-or-later
#ifndef POM2_TWO_IMG_HEADER_H
#define POM2_TWO_IMG_HEADER_H

#include <cstdint>
#include <cstring>

namespace pom2 {

/// The payload length a 2IMG header declares (bytes 28-31, little-endian).
/// Sweet-16 writes its images (creator 'WOOF') with 0 there; CiderPress
/// repairs any non-nibble one to block count × 512 (`TwoImg.cpp`: "fix
/// broken 'WOOF' images from Sweet-16"), and so does every loader here —
/// refusing them left a whole emulator's output unmountable. `hdr` holds at
/// least 32 bytes.
inline uint32_t twoImgDataLength(const uint8_t* hdr)
{
    auto rd32 = [hdr](int o) {
        return static_cast<uint32_t>(hdr[o]) | (static_cast<uint32_t>(hdr[o + 1]) << 8) |
               (static_cast<uint32_t>(hdr[o + 2]) << 16) | (static_cast<uint32_t>(hdr[o + 3]) << 24);
    };
    const uint32_t len = rd32(28);
    if (len == 0 && std::memcmp(hdr + 4, "WOOF", 4) == 0 && rd32(12) != 2 &&
        rd32(20) <= 0xFFFFFFFFu / 512u)   // blocks × 512 stays in 32 bits
        return rd32(20) * 512u;
    return len;
}

} // namespace pom2

#endif // POM2_TWO_IMG_HEADER_H
