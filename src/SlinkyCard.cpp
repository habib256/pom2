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

// SlinkyCard — see SlinkyCard.h. Every register below cites MAME
// `src/devices/bus/a2bus/a2memexp.cpp` (mamedev/mame master, 2026-10).

#include "SlinkyCard.h"

#include "ByteIO.h"

#include <algorithm>

namespace pom2 {
namespace {
constexpr uint32_t kSnapMagic   = 0x4B4E4C53u;   // 'SLNK' little-endian
constexpr uint16_t kSnapVersion = 1;
constexpr std::size_t kPages = SlinkyCard::kRamBytes / 256;
} // namespace

SlinkyCard::SlinkyCard(int slot)
    : slot_(slot), ram_(kRamBytes, 0xFF)   // device_start: memset 0xFF, :147
{
}

bool SlinkyCard::loadRom(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != kRomBytes) return false;
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    romLoaded_ = true;
    return true;
}

// a2memexp.cpp:155-159 device_reset — the latches clear, the RAM does not.
void SlinkyCard::onReset()
{
    regs_.fill(0);
    wptr_ = liveptr_ = 0;
}

// The three pointer bytes always mirror a pointer, with the Apple card's
// forced high nibble on bits 16-23 (a2memexp.cpp:175-177, 195-197, …).
void SlinkyCard::publishPointer(uint32_t p)
{
    regs_[0] = static_cast<uint8_t>(p & 0xFF);
    regs_[1] = static_cast<uint8_t>((p >> 8) & 0xFF);
    regs_[2] = static_cast<uint8_t>(((p >> 16) & 0xFF) | kBankHiOr);
}

// a2memexp.cpp:166-181 read_c0nx.
uint8_t SlinkyCard::deviceSelectRead(uint8_t low4)
{
    low4 &= 0x0F;
    uint8_t v = regs_[low4];
    if (low4 == 3) {
        v = ram_[liveptr_ & kAddrMask];
        ++liveptr_;
        publishPointer(liveptr_);
    }
    return v;
}

// a2memexp.cpp:188-232 write_c0nx. Note the asymmetry MAME keeps: a write to
// an address byte reloads the live pointer from the latch (`m_wptr`), while
// the data port's increment moves only the live pointer — so the latch keeps
// the START of a transfer.
void SlinkyCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    low4 &= 0x0F;
    switch (low4) {
        case 0: case 1: case 2: {
            const unsigned shift = 8u * low4;
            wptr_ = (wptr_ & ~(0xFFu << shift)) | (static_cast<uint32_t>(v) << shift);
            publishPointer(wptr_);
            liveptr_ = wptr_;
            break;
        }
        case 3:
            ram_[liveptr_ & kAddrMask] = v;
            ++liveptr_;
            publishPointer(liveptr_);
            break;
        default:
            regs_[low4] = v;
            break;
    }
}

// a2memexp.cpp:238-247 read_cnxx: `m_rom[offset + slotno() * 0x100]`.
uint8_t SlinkyCard::slotRomRead(uint8_t low8)
{
    uint8_t v = 0xFF;
    (void)peekSlotRom(low8, v);
    return v;
}

bool SlinkyCard::peekSlotRom(uint8_t low8, uint8_t& out) const
{
    if (!romLoaded_ || slot_ < 1 || slot_ > 7) return false;
    out = rom_[static_cast<std::size_t>(slot_) * 0x100 + low8];
    return true;
}

// a2memexp.cpp:253-260 read_c800: `m_rom[offset + 0x800]`.
uint8_t SlinkyCard::expansionRomRead(uint16_t offset)
{
    uint8_t v = 0xFF;
    (void)peekExpansionRom(offset, v);
    return v;
}

bool SlinkyCard::peekExpansionRom(uint16_t offset, uint8_t& out) const
{
    if (!romLoaded_) return false;
    out = rom_[0x800 + (offset & 0x7FF)];
    return true;
}

// MAME save_item list (a2memexp.cpp:149-152): regs, ram, wptr, liveptr.
// The RAM is written SPARSELY — only the 256-byte pages that differ from
// the power-on $FF fill — so an idle card costs ~30 bytes per rewind frame
// and a busy 1 MB RAM disk at most 1 MB + 2 bytes a page.
void SlinkyCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSnapMagic);
    byteio::putU16(out, kSnapVersion);
    out.insert(out.end(), regs_.begin(), regs_.end());
    byteio::putU32(out, wptr_);
    byteio::putU32(out, liveptr_);
    std::vector<uint16_t> pages;
    for (std::size_t p = 0; p < kPages; ++p) {
        const uint8_t* b = ram_.data() + p * 256;
        if (std::any_of(b, b + 256, [](uint8_t v) { return v != 0xFF; }))
            pages.push_back(static_cast<uint16_t>(p));
    }
    byteio::putU16(out, static_cast<uint16_t>(pages.size()));
    for (uint16_t p : pages) {
        byteio::putU16(out, p);
        out.insert(out.end(), ram_.begin() + p * 256, ram_.begin() + p * 256 + 256);
    }
}

void SlinkyCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // All or nothing: the whole blob is validated before anything is applied,
    // so a short, foreign or corrupt blob leaves the card untouched.
    constexpr std::size_t kHead = 4 + 2 + 16 + 4 + 4 + 2;
    if (!data || len < kHead) return;
    byteio::Reader r(data, len);
    if (r.u32() != kSnapMagic) return;
    if (r.u16() != kSnapVersion) return;
    std::array<uint8_t, 16> regs{};
    for (auto& b : regs) b = r.u8();
    const uint32_t wptr = r.u32();
    const uint32_t live = r.u32();
    const std::size_t count = r.u16();
    if (count > kPages || len < kHead + count * 258) return;
    const uint8_t* body = data + kHead;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned p = body[i * 258] | (body[i * 258 + 1] << 8);
        if (p >= kPages) return;
    }
    regs_ = regs;
    wptr_ = wptr;
    liveptr_ = live;
    std::fill(ram_.begin(), ram_.end(), 0xFF);
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t* e = body + i * 258;
        const unsigned p = e[0] | (e[1] << 8);
        std::copy(e + 2, e + 258, ram_.begin() + p * 256);
    }
}

} // namespace pom2
