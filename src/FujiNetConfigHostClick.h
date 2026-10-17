#pragma once

#include <cstdint>
#include <optional>

namespace pom2::fujinet_config {

// Apple II CONFIG draws its Host List on text page 1, rows 1..8. A host
// click is translated to the same slot digit followed by Return that CONFIG
// accepts from the keyboard. Validate the screen before consuming a click so
// unrelated Apple II programs keep their normal mouse input.
template <typename ReadMainRam>
std::optional<char> hostKeyAtRow(ReadMainRam readMainRam, int row,
                                 bool textMode, bool page2, bool eightyCol)
{
    if (!textMode || page2 || eightyCol || row < 1 || row > 8)
        return std::nullopt;
    auto cell = [&](int y, int x) -> char {
        const int offset = 0x400 + 0x80 * (y % 8) + 0x28 * (y / 8) + x;
        return static_cast<char>(readMainRam(offset) & 0x7f);
    };
    constexpr char title[] = " Host List";
    bool found = false;
    for (int x = 0; x <= 40 - (static_cast<int>(sizeof(title)) - 1); ++x) {
        bool match = true;
        for (int i = 0; i < static_cast<int>(sizeof(title)) - 1; ++i)
            match &= cell(0, x + i) == title[i];
        found |= match;
    }
    if (!found) return std::nullopt;
    for (int y = 1; y <= 8; ++y)
        if (cell(y, 0) != '0' + y || cell(y, 1) != ' ')
            return std::nullopt;
    return static_cast<char>('0' + row);
}

} // namespace pom2::fujinet_config
