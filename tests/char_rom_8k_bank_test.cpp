// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// 8 KB international //e video ROM — bank-selection equivalence.
//
// MAME's //e character generator region (`gfx1`) is 8 KB = two 4 KB banks, and
// the machine's charset switch picks one. The US `apple2ee` fills BOTH banks
// with the same 4 KB part (342-0265-a.chr at offset 0 and 0x1000); the French
// `apple2eefr` instead ships a single 8 KB part, 342-0274-a.e9, that carries
// two DIFFERENT sets. POM2 handles that by collapsing the 8 KB dump to the
// selected bank and running its ordinary 4 KB normalization on it.
//
// The property pinned here is the one that makes that legal:
//
//     loading bank N of the 8 KB dump must produce EXACTLY the same normalized
//     character ROM as loading the standalone 4 KB dump of that same set.
//
// which is checkable because POM2 already ships both halves separately:
//   bank 0 == roms/apple2e_char_frca.rom   (FR-CA set)
//   bank 1 == roms/apple2e_char.rom        (US set)
//
// Without this, a wrong bank index or an off-by-4096 slice would still "load"
// and simply draw the other country's glyphs — a silent, plausible-looking
// wrong picture rather than a failure.
//
// Soft-skips when the ROMs are absent: they are user-provided and must never
// be a hard test dependency (same rule as the tomharte corpora).

#include "Memory.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks   = 0;

bool have(const char* p) { return std::filesystem::exists(p); }

// Load a char ROM and hand back the NORMALIZED table Memory keeps, which is
// what the renderer actually indexes — comparing raw file bytes would not
// prove the normalization ran on the right half.
std::vector<uint8_t> normalized(const char* path, int bank)
{
    Memory m;
    m.setIIEMode(true);
    if (!m.loadCharRom(path, bank)) return {};
    return m.charRom();
}

void expectSame(const char* eightK, int bank, const char* fourK, const char* what)
{
    const std::vector<uint8_t> a = normalized(eightK, bank);
    const std::vector<uint8_t> b = normalized(fourK, 0);
    ++checks;
    if (a.empty() || b.empty()) {
        std::printf("FAIL %s: a load returned nothing (8K=%zu, 4K=%zu)\n",
                    what, a.size(), b.size());
        ++failures;
        return;
    }
    if (a.size() != 4096 || b.size() != 4096) {
        std::printf("FAIL %s: expected 4096 bytes after normalization, got %zu/%zu\n",
                    what, a.size(), b.size());
        ++failures;
        return;
    }
    if (a != b) {
        size_t first = 0, diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) { if (!diff) first = i; ++diff; }
        std::printf("FAIL %s: %zu/4096 bytes differ, first at %zu (%02X vs %02X)\n",
                    what, diff, first, a[first], b[first]);
        ++failures;
        return;
    }
    std::printf("  ok: %s\n", what);
}

}  // namespace

int main()
{
    std::puts("=== 8K international char ROM bank selection ===");

    const char* k8   = "roms/342-0274-a.e9";
    const char* kFr  = "roms/apple2e_char_frca.rom";
    const char* kUs  = "roms/apple2e_char.rom";

    if (!have(k8) || !have(kFr) || !have(kUs)) {
        std::printf("SKIP: need %s + %s + %s (user-provided ROMs)\n", k8, kFr, kUs);
        return 77;   // ctest SKIP_RETURN_CODE
    }

    expectSame(k8, 0, kFr, "bank 0 == FR-CA 4K dump");
    expectSame(k8, 1, kUs, "bank 1 == US 4K dump");

    // The two banks must not be interchangeable, or the test above would pass
    // for a loader that ignored `bank` entirely.
    {
        ++checks;
        const std::vector<uint8_t> b0 = normalized(k8, 0);
        const std::vector<uint8_t> b1 = normalized(k8, 1);
        if (b0.empty() || b1.empty() || b0 == b1) {
            std::puts("FAIL: bank 0 and bank 1 are identical — `bank` is being ignored");
            ++failures;
        } else {
            std::puts("  ok: banks differ (bank argument is load-bearing)");
        }
    }

    // Out-of-range bank clamps to 0 rather than reading past the buffer.
    {
        ++checks;
        const std::vector<uint8_t> b0 = normalized(k8, 0);
        const std::vector<uint8_t> b9 = normalized(k8, 9);
        if (b9 != b0) { std::puts("FAIL: out-of-range bank did not clamp to 0"); ++failures; }
        else          { std::puts("  ok: out-of-range bank clamps to 0"); }
    }

    // A 4K dump must ignore `bank` entirely (no slicing on a single-bank part).
    {
        ++checks;
        const std::vector<uint8_t> a = normalized(kUs, 0);
        const std::vector<uint8_t> b = normalized(kUs, 1);
        if (a.empty() || a != b) { std::puts("FAIL: 4K dump changed with bank"); ++failures; }
        else                     { std::puts("  ok: 4K dump ignores bank"); }
    }

    if (failures) {
        std::printf("char_rom_8k_bank: FAIL (%d of %d)\n", failures, checks);
        return 1;
    }
    std::printf("char_rom_8k_bank: OK (%d checks)\n", checks);
    return 0;
}
