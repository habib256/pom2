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

// pom2_prodos on a .2mg (ProDOSImageFile.h): a file written into the volume
// inside a 2IMG lands in its payload, and EVERYTHING around it — header,
// comment chunk — comes back byte for byte; a DOS-order 2IMG is converted
// both ways; a nibble one, a damaged one are refused; the lock bit is seen;
// Sweet-16's zero length is repaired. The plain .po / .dsk paths keep their
// behaviour.

#include "ProDOSDiskFs.h"
#include "ProDOSImageFile.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pom2::prodosfs;

namespace {

void put32(std::vector<uint8_t>& b, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

// [B] 2IMG spec: magic, creator, header length, version, format, flags,
// blocks, data offset/length, comment offset/length.
std::vector<uint8_t> twoImg(const std::vector<uint8_t>& payload, uint32_t format, uint32_t flags,
                            const std::string& comment, bool zeroLength = false,
                            const char* creator = "PM2X")
{
    std::vector<uint8_t> b(64, 0);
    std::memcpy(b.data(), "2IMG", 4);
    std::memcpy(b.data() + 4, creator, 4);
    b[8] = 64; b[10] = 1;
    put32(b, 0x0C, format);
    put32(b, 0x10, flags);
    put32(b, 0x14, format == 1 ? static_cast<uint32_t>(payload.size() / 512) : 0);
    put32(b, 0x18, 64);
    put32(b, 0x1C, zeroLength ? 0 : static_cast<uint32_t>(payload.size()));
    put32(b, 0x20, static_cast<uint32_t>(64 + payload.size()));
    put32(b, 0x24, static_cast<uint32_t>(comment.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    b.insert(b.end(), comment.begin(), comment.end());
    return b;
}

void testProdosOrder()
{
    std::string err;
    const auto vol = ProDOSDisk::format(280, "TWOIMG", err);
    assert(!vol.empty());
    const std::string comment = "a comment chunk after the data";
    const auto file = twoImg(vol, 1, 0, comment);

    ImageFile layout;
    std::vector<uint8_t> blocks;
    assert(readImageFile(file, ".2mg", layout, blocks, err));
    assert(layout.kind == ImageFile::Kind::TwoImg && !layout.locked && blocks == vol);

    ProDOSDisk disk;
    assert(disk.open(blocks, err));
    const std::vector<uint8_t> data = { 'H', 'I', '\r' };
    assert(disk.writeFile("NOTE", 0x04, 0, data, err));
    const auto out = writeImageFile(layout, disk.image());
    assert(out.size() == file.size());
    assert(std::memcmp(out.data(), file.data(), 64) == 0 && "the header is kept");
    assert(std::string(out.end() - static_cast<long>(comment.size()), out.end()) == comment && "the chunk is kept");
    assert(out != file && "the payload changed");

    ImageFile again; std::vector<uint8_t> b2;
    assert(readImageFile(out, ".2MG-any-ext", again, b2, err));
    ProDOSDisk back;
    assert(back.open(b2, err));
    std::vector<uint8_t> read; Entry info;
    assert(back.readFile("NOTE", read, &info, err) && read == data);
    std::printf("  ok: ProDOS-order 2IMG: the file lands, header and chunk byte for byte\n");
}

void testDosOrderAndPlain()
{
    std::string err;
    const auto vol = ProDOSDisk::format(280, "DOSORD", err);
    const auto dosBytes = blockOrderToDosOrder(vol);
    ImageFile layout; std::vector<uint8_t> blocks;
    assert(readImageFile(twoImg(dosBytes, 0, 0, ""), ".2mg", layout, blocks, err));
    assert(layout.twoImgDosOrder && blocks == vol);
    const auto out = writeImageFile(layout, blocks);
    assert(std::vector<uint8_t>(out.begin() + 64, out.end()) == dosBytes && "converted back to DOS order");

    // The plain paths: a .dsk is DOS order, a .po is the blocks.
    assert(readImageFile(dosBytes, ".dsk", layout, blocks, err) && layout.kind == ImageFile::Kind::DosOrder && blocks == vol);
    assert(writeImageFile(layout, blocks) == dosBytes);
    assert(readImageFile(vol, ".po", layout, blocks, err) && layout.kind == ImageFile::Kind::Blocks && blocks == vol);
    std::printf("  ok: DOS-order 2IMG both ways; .dsk and .po unchanged\n");
}

void testRefusalsAndFlags()
{
    std::string err;
    const auto vol = ProDOSDisk::format(280, "FLAGS", err);
    ImageFile layout; std::vector<uint8_t> blocks;
    assert(!readImageFile(twoImg(vol, 2, 0, ""), ".2mg", layout, blocks, err) && err.find("nibble") != std::string::npos);
    auto bad = twoImg(vol, 1, 0, "");
    put32(bad, 0x1C, static_cast<uint32_t>(vol.size() + 1));
    assert(!readImageFile(bad, ".2mg", layout, blocks, err) && err.find("outside") != std::string::npos);
    assert(!readImageFile(std::vector<uint8_t>(bad.begin(), bad.begin() + 40), ".2mg", layout, blocks, err));
    assert(readImageFile(twoImg(vol, 1, 0x80000000u, ""), ".2mg", layout, blocks, err) && layout.locked);
    // Sweet-16 ('WOOF') writes a zero length: block count × 512 (TwoImgHeader.h).
    assert(readImageFile(twoImg(vol, 1, 0, "", true, "WOOF"), ".2mg", layout, blocks, err) && blocks == vol);
    // A size change is refused rather than written over the chunks.
    assert(readImageFile(twoImg(vol, 1, 0, "c"), ".2mg", layout, blocks, err));
    blocks.resize(blocks.size() + 512);
    assert(writeImageFile(layout, blocks).empty());
    std::printf("  ok: nibble, damaged and cut 2IMG refused; lock seen; WOOF repaired; no resize\n");
}

} // namespace

int main()
{
    std::printf("prodos_image_file:\n");
    testProdosOrder();
    testDosOrderAndPlain();
    testRefusalsAndFlags();
    std::printf("prodos_image_file: all passed\n");
    return 0;
}
