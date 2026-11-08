// POM2 — GPL-3.0-or-later
// Also compiled as a standalone WASM regression: rejection must not depend
// on native exception handling, which the browser build does not enable.
#include "Mea8000.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

int main()
{
    pom2::Mea8000 chip;
    std::vector<uint8_t> before;
    chip.appendSnapshot(before);
    const std::vector<std::pair<std::size_t, double>> cases = {
        {0, 4}, {0, -1}, {0, 0.5}, {1, -1}, {2, 513}, {3, 5},
        {4, 480}, {5, 4000000}, {6, 13}, {7, 4294967296.0},
        {8, 2}, {9, -1}, {10, 0.5}, {11, 256}, {12, -1},
        {13, 1.5}, {14, 256}, {15, 511}, {16, -1}, {17, 2},
        {18, -1}, {19, 1}, {21, 3501}, {37, 1e30},
        {0, std::numeric_limits<double>::quiet_NaN()},
        {50, std::numeric_limits<double>::infinity()}
    };
    for (const auto& entry : cases) {
        auto bad = before;
        uint64_t bits; std::memcpy(&bits, &entry.second, 8);
        for (int b = 0; b < 8; ++b) bad[4 + entry.first * 8 + b] = uint8_t(bits >> (8*b));
        if (chip.loadSnapshot(bad.data(), bad.size())) {
            std::fprintf(stderr, "FAIL: accepted invalid field %zu\n", entry.first); return 1;
        }
        std::vector<uint8_t> after; chip.appendSnapshot(after);
        if (after != before) { std::fprintf(stderr, "FAIL: non-atomic rejection\n"); return 1; }
    }
    if (!chip.loadSnapshot(before.data(), before.size())) return 1;
    std::printf("PASS: %zu malformed MEA8000 snapshots rejected atomically\n", cases.size());
    return 0;
}
