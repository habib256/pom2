#include "FujiNetConfigHostClick.h"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    std::array<uint8_t, 0x800> ram{};
    auto put = [&](int y, int x, char c, bool inverse = false) {
        const int addr = 0x400 + 0x80 * (y % 8) + 0x28 * (y / 8) + x;
        ram[addr] = static_cast<uint8_t>(c) | (inverse ? 0 : 0x80);
    };
    constexpr char title[] = " Host List";
    for (int x = 0; x < static_cast<int>(sizeof(title)) - 1; ++x)
        put(0, 30 + x, title[x]);
    for (int y = 1; y <= 8; ++y) {
        put(y, 0, static_cast<char>('0' + y), y == 3);
        put(y, 1, ' ', y == 3);
    }
    auto key = [&](int row, bool text = true, bool page2 = false,
                   bool eightyCol = false) {
        return pom2::fujinet_config::hostKeyAtRow(
            [&](int addr) { return ram[addr]; }, row, text, page2, eightyCol);
    };
    assert(key(1) == '1');
    assert(key(3) == '3'); // selected row is inverse video
    assert(key(8) == '8');
    assert(!key(0) && !key(9));
    assert(!key(3, false) && !key(3, true, true) &&
           !key(3, true, false, true));
    put(0, 30, 'X');
    assert(!key(3)); // another text program must retain its mouse input
}
