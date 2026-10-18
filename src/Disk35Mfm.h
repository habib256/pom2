// POM2 — GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2::mfm35 {
inline uint16_t mfmCrc(uint16_t crc, uint8_t byte)
{
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int bit = 0; bit < 8; ++bit)
        crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
    return crc;
}

inline uint16_t mfmWord(const std::vector<uint8_t>& cells, std::size_t pos)
{
    uint16_t word = 0;
    for (int bit = 0; bit < 16; ++bit)
        word = static_cast<uint16_t>((word << 1) | cells[pos + bit]);
    return word;
}

inline uint8_t mfmByte(const std::vector<uint8_t>& cells, std::size_t pos)
{
    uint8_t byte = 0;
    for (int bit = 1; bit < 16; bit += 2)
        byte = static_cast<uint8_t>((byte << 1) | cells[pos + bit]);
    return byte;
}

template<class Sector>
void decodeMfmTrack(const std::vector<uint8_t>& cells, int track, int head,
                    Sector&& sector)
{
    if (cells.size() < 16 * 530) return;
    // A1 with a deliberately missing MFM clock is 0x4489. Three such
    // words precede both ID and data marks; scanning bit by bit tolerates
    // arbitrary index positions in captured MOOF tracks.
    int pendingSector = -1;
    for (std::size_t pos = 0; pos + 16 * 4 < cells.size(); ++pos) {
        if (mfmWord(cells, pos) != 0x4489 ||
            mfmWord(cells, pos + 16) != 0x4489 ||
            mfmWord(cells, pos + 32) != 0x4489) continue;
        const std::size_t markPos = pos + 48;
        const uint8_t mark = mfmByte(cells, markPos);
        if (mark == 0xFE && markPos + 16 * 7 <= cells.size()) {
            uint8_t id[6];
            for (int i = 0; i < 6; ++i) id[i] = mfmByte(cells, markPos + 16 * (i + 1));
            uint16_t crc = 0xFFFF;
            for (int i = 0; i < 3; ++i) crc = mfmCrc(crc, 0xA1);
            crc = mfmCrc(crc, mark);
            for (int i = 0; i < 4; ++i) crc = mfmCrc(crc, id[i]);
            pendingSector = (id[0] == track && id[1] == head && id[2] >= 1 &&
                             id[2] <= 18 && id[3] == 2 &&
                             id[4] == uint8_t(crc >> 8) &&
                             id[5] == uint8_t(crc)) ? id[2] : -1;
        } else if ((mark == 0xFB || mark == 0xF8) && pendingSector > 0 &&
                   markPos + 16 * 515 <= cells.size()) {
            uint8_t data[512];
            uint16_t crc = 0xFFFF;
            for (int i = 0; i < 3; ++i) crc = mfmCrc(crc, 0xA1);
            crc = mfmCrc(crc, mark);
            for (int i = 0; i < 512; ++i) {
                data[i] = mfmByte(cells, markPos + 16 * (i + 1));
                crc = mfmCrc(crc, data[i]);
            }
            const uint8_t high = mfmByte(cells, markPos + 16 * 513);
            const uint8_t low = mfmByte(cells, markPos + 16 * 514);
            if (high == uint8_t(crc >> 8) && low == uint8_t(crc))
                sector(pendingSector, data, markPos);
            pendingSector = -1;
        }
        pos += 47; // the three sync words cannot start another field
    }
}

}
