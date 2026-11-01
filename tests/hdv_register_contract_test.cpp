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

// hdv_register_contract — DEV.md § ProDOSHardDiskCard ▸ Register-level
// contract, clause by clause.
//
// `ProDOSHardDiskCard` is POM2's own invention: no MAME file, no datasheet.
// So the written contract IS the reference, and a contract with no test is a
// contract that drifts. This pins the clauses a reimplementation would get
// wrong — above all the NEGATIVE ones, which is the part of the spec that
// nothing else would ever exercise:
//
//   * a write to $0/$1 resets the byte cursor EVEN WHEN the selected block
//     does not change;
//   * a write to $6 (drive) does NOT reset it;
//   * the cursor does NOT advance on a read or write the card declines;
//   * an empty bay's status is EXACTLY $80 — never bit 6 or bit 5, because
//     the ROM reads a clear bit 5 as "the bay is empty";
//   * the block count CLAMPS at $FFFF rather than truncating to 0;
//   * every offset but $2/$3/$4/$5/$6 reads $FF, and every write outside
//     $0/$1/$2/$6 is discarded;
//   * RESET moves the cursors and the latch and NOTHING else: no eject, no
//     flush, no dirty block cleared.

#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

constexpr size_t kBlock = ProDOSHardDiskCard::kBlockBytes;

/// A card with `blocks` blocks of known bytes in drive 1, mounted from
/// memory so no host file is involved.
std::unique_ptr<ProDOSHardDiskCard> cardWith(size_t blocks)
{
    auto card = std::make_unique<ProDOSHardDiskCard>();
    std::vector<uint8_t> img(blocks * kBlock, 0x00);
    for (size_t b = 0; b < blocks; ++b)
        for (size_t i = 0; i < kBlock; ++i)
            img[b * kBlock + i] = uint8_t((b * 31u + i * 7u + 3u) & 0xFF);
    assert(card->loadImageFromBytes(std::move(img), "contract"));
    return card;
}

uint8_t expected(size_t block, size_t offset)
{
    return uint8_t((block * 31u + offset * 7u + 3u) & 0xFF);
}

void selectBlock(ProDOSHardDiskCard& c, uint16_t block)
{
    c.deviceSelectWrite(0x0, uint8_t(block & 0xFF));
    c.deviceSelectWrite(0x1, uint8_t(block >> 8));
}

void testCursorAdvanceAndWrap()
{
    auto c = cardWith(2);
    selectBlock(*c, 1);
    for (size_t i = 0; i < kBlock; ++i)
        CHECK(c->deviceSelectRead(0x2) == expected(1, i));
    // Wraps to the first byte of the SAME block, it does not walk into the
    // next one.
    CHECK(c->deviceSelectRead(0x2) == expected(1, 0));
    CHECK(c->deviceSelectRead(0x2) == expected(1, 1));
}

void testBlockWriteResetsCursorEvenWhenTheBlockIsUnchanged()
{
    auto c = cardWith(2);
    selectBlock(*c, 1);
    CHECK(c->deviceSelectRead(0x2) == expected(1, 0));
    CHECK(c->deviceSelectRead(0x2) == expected(1, 1));

    // Re-write the SAME low byte: the block does not change, the cursor
    // still goes back to 0. It is the write that resets, not the change.
    c->deviceSelectWrite(0x0, 0x01);
    CHECK(c->deviceSelectRead(0x2) == expected(1, 0));

    // And the same through the high byte.
    c->deviceSelectRead(0x2);
    c->deviceSelectWrite(0x1, 0x00);
    CHECK(c->deviceSelectRead(0x2) == expected(1, 0));
}

void testDriveWriteDoesNotResetTheCursor()
{
    auto c = cardWith(2);
    // Drive 2 gets a medium too, so the read below is not declined for
    // being empty — which would hide the cursor's position.
    std::vector<uint8_t> second(2 * kBlock, 0xA5);
    second[0 * kBlock + 3] = 0x11;      // block 0, offset 3
    CHECK(c->loadDriveFromBytes(1, std::move(second), "drive2"));

    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == expected(0, 0));
    CHECK(c->deviceSelectRead(0x2) == expected(0, 1));
    CHECK(c->deviceSelectRead(0x2) == expected(0, 2));   // cursor now 3

    // Switch to drive 2: the cursor stays at 3, on the other medium.
    c->deviceSelectWrite(0x6, 0x80);
    CHECK(c->deviceSelectRead(0x6) == 0x80);
    CHECK(c->deviceSelectRead(0x2) == 0x11 &&
          "a drive write must not reset the byte cursor");

    // Back again, and the first drive's cursor is wherever the shared one
    // now is — the cursor is the CARD's, not the drive's.
    c->deviceSelectWrite(0x6, 0x00);
    CHECK(c->deviceSelectRead(0x6) == 0x00);
    CHECK(c->deviceSelectRead(0x2) == expected(0, 4));
}

void testDeclinedAccessDoesNotMoveTheCursor()
{
    // An empty bay: reads give $FF and the cursor must not move, or the
    // first real transfer after a mount would start mid-block.
    auto c = std::make_unique<ProDOSHardDiskCard>();
    selectBlock(*c, 0);
    for (int i = 0; i < 5; ++i) CHECK(c->deviceSelectRead(0x2) == 0xFF);
    for (int i = 0; i < 5; ++i) c->deviceSelectWrite(0x2, uint8_t(i));

    std::vector<uint8_t> img(kBlock, 0x00);
    for (size_t i = 0; i < kBlock; ++i) img[i] = uint8_t(i & 0xFF);
    CHECK(c->loadImageFromBytes(std::move(img), "late"));
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == 0x00 &&
          "a declined read must leave the cursor alone");
    CHECK(c->deviceSelectRead(0x2) == 0x01);
}

void testWriteProtectLatchesPerBlock()
{
    auto c = cardWith(1);
    c->setHostWriteProtected(true);
    selectBlock(*c, 0);
    // Refused, and the cursor does not move.
    c->deviceSelectWrite(0x2, 0xDE);
    c->deviceSelectWrite(0x2, 0xAD);
    CHECK(c->deviceSelectRead(0x2) == expected(0, 0));
    CHECK((c->deviceSelectRead(0x3) & 0x40) != 0 && "bit 6 = write-protected");

    // Unprotected: the write lands, and the latch holds for the block even
    // if the medium is protected again mid-loop.
    c->setHostWriteProtected(false);
    selectBlock(*c, 0);
    c->deviceSelectWrite(0x2, 0xDE);
    c->setHostWriteProtected(true);
    c->deviceSelectWrite(0x2, 0xAD);          // same block, latch still open
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == 0xDE);
    CHECK(c->deviceSelectRead(0x2) == 0xAD &&
          "the notch must not tear a block whose write already began");

    // A new block asks again, and is refused.
    c->deviceSelectWrite(0x0, 0x00);          // resets the cursor + the latch
    c->deviceSelectWrite(0x2, 0x5A);
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == 0xDE && "the latch is re-asked per block");
}

void testStatusByte()
{
    // Empty bay: EXACTLY $80. Bit 5 must stay clear — the ROM's error tail
    // reads a clear bit 5 as "the bay is empty".
    auto empty = std::make_unique<ProDOSHardDiskCard>();
    CHECK(empty->deviceSelectRead(0x3) == 0x80);
    selectBlock(*empty, 0xFFFF);
    CHECK(empty->deviceSelectRead(0x3) == 0x80 &&
          "an empty bay must never set bit 5, whatever block is selected");

    auto c = cardWith(4);
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x3) == 0x00);
    selectBlock(*c, 3);
    CHECK(c->deviceSelectRead(0x3) == 0x00);          // last valid block
    selectBlock(*c, 4);
    CHECK(c->deviceSelectRead(0x3) == 0x20 && "bit 5 = block past the end");
    selectBlock(*c, 0xFFFF);
    CHECK(c->deviceSelectRead(0x3) == 0x20);
    c->setHostWriteProtected(true);
    CHECK(c->deviceSelectRead(0x3) == 0x60 && "bits 6 and 5 together");

    // An out-of-range transfer is answered, not refused: $FF per byte, and a
    // write is dropped. A firmware that ignores bit 5 must not hang.
    c->setHostWriteProtected(false);
    selectBlock(*c, 100);
    for (int i = 0; i < 4; ++i) CHECK(c->deviceSelectRead(0x2) == 0xFF);
    for (int i = 0; i < 4; ++i) c->deviceSelectWrite(0x2, 0x77);
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == expected(0, 0));
}

void testBlockCountRegisters()
{
    auto empty = std::make_unique<ProDOSHardDiskCard>();
    CHECK(empty->deviceSelectRead(0x4) == 0x00);
    CHECK(empty->deviceSelectRead(0x5) == 0x00);

    auto c = cardWith(300);
    CHECK(c->deviceSelectRead(0x4) == uint8_t(300 & 0xFF));
    CHECK(c->deviceSelectRead(0x5) == uint8_t(300 >> 8));

    // The clamp. A 32 MiB image is exactly 65536 blocks, which truncates to
    // 0 — and ProDOS reads 0 as an empty volume.
    auto big = std::make_unique<ProDOSHardDiskCard>();
    CHECK(big->loadImageFromBytes(std::vector<uint8_t>(65536u * kBlock, 0x00),
                                  "32MiB"));
    CHECK(big->deviceSelectRead(0x4) == 0xFF);
    CHECK(big->deviceSelectRead(0x5) == 0xFF);
}

void testUnclaimedOffsets()
{
    auto c = cardWith(2);
    selectBlock(*c, 1);
    for (uint8_t off : { 0x0, 0x1, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF })
        CHECK(c->deviceSelectRead(off) == 0xFF);

    // And a write outside $0/$1/$2/$6 changes nothing the card exposes.
    const uint8_t status = c->deviceSelectRead(0x3);
    const uint8_t cntLo  = c->deviceSelectRead(0x4);
    const uint8_t first  = c->deviceSelectRead(0x2);   // cursor -> 1
    for (uint8_t off : { 0x3, 0x4, 0x5, 0x7, 0x8, 0xF })
        c->deviceSelectWrite(off, 0xFF);
    CHECK(c->deviceSelectRead(0x3) == status);
    CHECK(c->deviceSelectRead(0x4) == cntLo);
    CHECK(first == expected(1, 0));
    CHECK(c->deviceSelectRead(0x2) == expected(1, 1) &&
          "a discarded write must not move the cursor either");
}

void testSlotRomHasNoSideEffects()
{
    auto c = cardWith(2);
    selectBlock(*c, 1);
    c->deviceSelectRead(0x2);                      // cursor -> 1
    std::vector<uint8_t> first, again;
    for (int i = 0; i < 256; ++i) first.push_back(c->slotRomRead(uint8_t(i)));
    for (int i = 0; i < 256; ++i) again.push_back(c->slotRomRead(uint8_t(i)));
    CHECK(first == again && "the ROM page must read the same every time");
    CHECK(c->deviceSelectRead(0x2) == expected(1, 1) &&
          "reading the ROM page must not touch the byte cursor");
    // No expansion ROM, so the card must not claim the $C800 window.
    CHECK(!c->takesC800());
    CHECK(!c->romLayoutError());
}

void testResetMovesTheCursorsAndNothingElse()
{
    auto c = cardWith(2);
    selectBlock(*c, 1);
    c->deviceSelectWrite(0x6, 0x80);
    c->deviceSelectRead(0x2);
    // Dirty the medium so RESET has something it could wrongly discard.
    c->deviceSelectWrite(0x6, 0x00);
    selectBlock(*c, 0);
    c->deviceSelectWrite(0x2, 0x5A);

    c->onReset();

    CHECK(c->deviceSelectRead(0x6) == 0x00 && "RESET selects drive 1");
    // Block 0, offset 0 — the byte the write above put there, so RESET kept
    // both the medium and the unsaved write.
    CHECK(c->deviceSelectRead(0x2) == 0x5A &&
          "RESET must not eject the medium or drop an unsaved write");
    CHECK(c->deviceSelectRead(0x3) == 0x00 && "still loaded, still writable");
    CHECK(c->isImageLoaded());
    CHECK(c->hasUnsavedChanges() && "RESET must not clear a dirty block");

    // The write latch is clear after RESET: a write now re-asks the notch.
    c->setHostWriteProtected(true);
    selectBlock(*c, 0);
    c->onReset();
    c->deviceSelectWrite(0x2, 0x11);
    selectBlock(*c, 0);
    CHECK(c->deviceSelectRead(0x2) == 0x5A && "the latch is clear after RESET");
}

} // namespace

int main()
{
    testCursorAdvanceAndWrap();
    testBlockWriteResetsCursorEvenWhenTheBlockIsUnchanged();
    testDriveWriteDoesNotResetTheCursor();
    testDeclinedAccessDoesNotMoveTheCursor();
    testWriteProtectLatchesPerBlock();
    testStatusByte();
    testBlockCountRegisters();
    testUnclaimedOffsets();
    testSlotRomHasNoSideEffects();
    testResetMovesTheCursorsAndNothingElse();
    if (g_fail) {
        std::fprintf(stderr, "hdv_register_contract: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("hdv_register_contract: OK\n");
    return 0;
}
