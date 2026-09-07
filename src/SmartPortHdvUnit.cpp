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

// SmartPortHdvUnit — thin adapter mapping SmartPortUnit onto the shared
// pom2::Block512Backing store. All the heavy lifting (2IMG parse, dirty
// tracking, write-back, WP) lives in Block512Backing and is exercised by the
// hdv_* / ata_block_device pin tests; this file only forwards.

#include "SmartPortHdvUnit.h"

namespace pom2 {

SmartPortHdvUnit::SmartPortHdvUnit() = default;

SmartPortHdvUnit::~SmartPortHdvUnit()
{
    // Best-effort write-back on destruction (e.g. card unplugged). No-op when
    // write-back is off, the medium is WP, or nothing is dirty.
    (void)backing_.saveDirty();
}

bool SmartPortHdvUnit::readBlock(uint32_t idx, uint8_t* out) const
{
    if (!out) return false;
    return backing_.readBlock(idx, out);
}

bool SmartPortHdvUnit::writeBlock(uint32_t idx, const uint8_t* in)
{
    // Mirrors SmartPort35Unit::writeBlock: the unit contract says a WRITE
    // on a write-protected unit returns false, and write-protected includes
    // "no write-back opt-in" (see the header). The card's dispatch already
    // answers $2B before reaching here; this keeps a direct caller honest.
    if (!in || !backing_.isLoaded() || isWriteProtected()) return false;
    return backing_.writeBlock(idx, in);
}

bool SmartPortHdvUnit::loadImage(const std::string& path)
{
    // Flush pending writes on the outgoing image before swapping, so a
    // mid-session swap doesn't silently drop dirty blocks.
    if (!backing_.saveDirty()) return false;
    return backing_.loadImage(path);
}

bool SmartPortHdvUnit::eject()
{
    // Save-on-eject policy lives here (Block512Backing::eject never auto-
    // saves): flush first (no-op unless write-back is on + dirty + writable),
    // then drop the image.
    if (!backing_.saveDirty()) return false;
    backing_.eject();
    return true;
}

} // namespace pom2
