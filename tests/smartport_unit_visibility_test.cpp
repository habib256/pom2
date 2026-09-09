// A SmartPort bay past `unitCount()` is invisible to the guest.
//
// The card has had eight bays since 2026-09-08, but how many it ANSWERS FOR
// is the separate, user-set `unitCount()` (2, 4, 6 or 8; default 2). Bays
// past it "keep their media but are invisible to the guest"
// (SmartPortCard.h), and the SmartPort call engine enforces that: unit 3 with
// a count of 2 is $28 "no device connected".
//
// The legacy streaming registers did not. `$C0n0` unit-select folded the
// written value with `% kMaxUnits` — modulo EIGHT since the bay count grew,
// modulo TWO before — so `LDA #3 / STA $C0n0` reached bay 3 and `$C0n3` then
// read AND COMMITTED 512-byte blocks on a disk the card is hiding: past
// `unitCount()`, past `bayCount()`, so not even the Slot Manager could show
// it. The two paths must agree.

#include "Disk35Image.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

/// A bare 800K ProDOS-shaped image whose every block starts with `tag`.
std::string makeImage(const fs::path& dir, const char* tag)
{
    std::vector<uint8_t> img(819200, 0x00);
    // Keep the "looks like ProDOS at block 2" sniff quiet.
    img[2 * 512 + 0] = 0x00; img[2 * 512 + 1] = 0x00; img[2 * 512 + 4] = 0xF0;
    for (std::size_t b = 0; b < img.size() / 512; ++b)
        if (b != 2) std::memcpy(img.data() + b * 512, tag, 8);
    const fs::path p = dir / (std::string(tag) + ".po");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_sp_unit_visibility";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string hidden = makeImage(dir, "HIDDEN00");

    int rc = 0;
    {
        pom2::SmartPortCard card(5);
        card.setUnitCount(2);                       // the default
        card.setBayType(3, std::string(pom2::SmartPort35Unit::kKindKey));
        std::string err;
        if (!card.mountBay(3, hidden, err)) {
            std::printf("FAIL: could not mount bay 3: %s\n", err.c_str());
            ++g_failures;
        }
        card.setBayWriteBack(3, true);

        check(card.unitCount() == 2 && card.bayCount() == 2,
              "the card answers for two units");

        // 1. Unit-select must not land on a bay outside the count.
        card.deviceSelectWrite(0x0, 3);
        check(card.activeUnit() < static_cast<std::size_t>(card.unitCount()),
              "STA #3,$C0n0 selects a bay inside unitCount() (got " +
              std::to_string(card.activeUnit()) + ")");

        // 2. Nothing of the hidden image comes out of the data register.
        card.deviceSelectWrite(0x1, 0x00);          // block 0 lo
        card.deviceSelectWrite(0x2, 0x00);          // block 0 hi
        char tag[9] = {};
        for (int i = 0; i < 8; ++i)
            tag[i] = static_cast<char>(card.deviceSelectRead(0x3));
        check(std::string(tag) != "HIDDEN00",
              "no byte of the hidden bay is readable through $C0n3");

        // 3. …and nothing goes INTO it.
        card.deviceSelectWrite(0x0, 3);
        card.deviceSelectWrite(0x1, 0x05);          // block 5
        card.deviceSelectWrite(0x2, 0x00);
        for (int i = 0; i < 512; ++i) card.deviceSelectWrite(0x3, 0x5A);
        uint8_t back[512] = {};
        const bool read5 = card.unit(3)->readBlock(5, back);
        check(read5 && back[0] != 0x5A,
              "a 512-byte stream does not commit into the hidden bay "
              "(byte[0] = $" + std::to_string(int(back[0])) + ")");

        // 4. The call engine's answer, unchanged — the two must agree.
        card.deviceSelectWrite(0xE, 0);             // BEGIN
        const uint8_t call[] = { 0x00, 0x03, 0x03, 0x00, 0x00, 0x00 };
        for (uint8_t b : call) card.deviceSelectWrite(0x7, b);
        check(card.deviceSelectRead(0xE) == 0x28,
              "SmartPort STATUS unit 3 is $28 no device connected");

        // 5. Raise the count and the same bay becomes reachable — the gate
        //    is the count, not the bay index.
        card.setUnitCount(4);
        card.deviceSelectWrite(0x0, 3);
        check(card.activeUnit() == 3, "with unitCount() = 4, bay 3 selects");
        card.deviceSelectWrite(0x1, 0x00);
        card.deviceSelectWrite(0x2, 0x00);
        char tag2[9] = {};
        for (int i = 0; i < 8; ++i)
            tag2[i] = static_cast<char>(card.deviceSelectRead(0x3));
        check(std::string(tag2) == "HIDDEN00",
              "…and its block 0 reads back");

        // Do not write the scratch image back on teardown.
        card.setBayWriteBack(3, false);
        rc = g_failures ? 1 : 0;
    }
    fs::remove_all(dir, ec);
    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASS\n", g_failures);
    return rc;
}
