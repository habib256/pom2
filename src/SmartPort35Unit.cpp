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

#include "SmartPort35Unit.h"

namespace pom2 {

SmartPort35Unit::SmartPort35Unit() = default;

SmartPort35Unit::~SmartPort35Unit()
{
    // Best-effort write-back on destruction (e.g. card unplugged).
    // No-op when write-back is off or nothing is dirty.
    (void)img_.saveDirty();
}

uint32_t SmartPort35Unit::blockCount() const
{
    return img_.isLoaded() ? img_.blockCount() : 0u;
}

bool SmartPort35Unit::readBlock(uint32_t idx, uint8_t* out) const
{
    if (!img_.isLoaded() || !out) return false;
    return img_.readBlock(idx, out);
}

bool SmartPort35Unit::writeBlock(uint32_t idx, const uint8_t* in)
{
    if (!img_.isLoaded() || img_.isWriteProtected() || !in) return false;
    return img_.writeBlock(idx, in);
}

bool SmartPort35Unit::loadImage(const std::string& path)
{
    // Auto-save the outgoing image's dirty blocks so a user-driven
    // swap doesn't quietly lose mid-session writes (same UX as
    // DiskIICard::insertDisk).
    if (!img_.saveDirty()) {
        lastError_ = img_.lastError();
        return false;
    }
    Disk35Image replacement;
    replacement.setWriteBackEnabled(img_.isWriteBackEnabled());
    replacement.setMfmCapable(img_.isMfmCapable());
    if (!replacement.loadFile(path)) {
        lastError_ = replacement.lastError();
        return false;
    }
    img_ = std::move(replacement);
    lastError_.clear();
    return true;
}

bool SmartPort35Unit::adoptImage(Block512Backing::PreparedImage&& p)
{
    if (!p.valid) { lastError_ = "no image to adopt"; return false; }
    if (!img_.loadPrepared(std::move(p.bytes), p.path, !p.hostWritable)) {
        lastError_ = img_.lastError();
        if (lastError_.empty()) lastError_ = "3.5-inch mount failed";   // never "unsupported"
        return false;
    }
    lastError_.clear();
    return true;
}

bool SmartPort35Unit::eject()
{
    // Save-on-eject, same policy as SmartPortHdvUnit::eject():
    // Disk35Image::eject() clears blocks_ + dirty_ unconditionally, so
    // skipping the flush silently destroyed every un-flushed guest write
    // of the session ("Write-back (save on eject)" checkbox lied).
    // saveDirty() is already a guarded no-op when write-back is off or
    // nothing is dirty.
    if (!img_.saveDirty()) {
        lastError_ = img_.lastError();
        return false;
    }
    lastError_ = img_.lastError();
    img_.eject();
    return true;
}

} // namespace pom2
