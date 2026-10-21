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

// Memory watchpoints — the tables behind the debugger's write and read
// watches. Kept out of Memory.cpp: it is a concern of its own, and Memory.cpp
// is the god-object the file-size ratchet refuses to grow.
//
// The hot-path halves live elsewhere, on purpose:
//   * memWrite's diversion is the `writable[]` byte this file clears
//     (Memory.h § Write watchpoints) — nothing is tested for on that path;
//   * memRead's gates are the three derived bytes refreshReadFastFlags()
//     recomputes (Memory.h § Read watchpoints), and the report itself is the
//     memReadSlow wrapper in Memory.cpp, which must stay in that TU so the
//     slow body can be force-inlined into it (PERFORMANCE § 8.5).
// What is here is the bookkeeping: arm, disarm, the shadowed permission.

#include "Memory.h"

void Memory::setWriteWatch(uint16_t addr, bool on)
{
    if (on) {
        if (writeWatch_.empty()) writeWatch_.assign(0x10000, 0);
        if (writeWatch_[addr] & kWatchArmed) return;      // keep the count honest
        // The REAL permission, not the byte: a page the aux shadow has
        // already diverted reads false here while being plain RAM.
        writeWatch_[addr] = static_cast<uint8_t>(
            kWatchArmed | ((writable[addr] || auxShadowCovers(addr) ||
                            videoWriteLogCovers(addr))
                               ? kWatchWasWritable : 0));
        // THE diversion: the fast path's own test now fails for this address,
        // which is the whole trick. Below $C000 only — above it every write
        // already reaches memWriteSlow.
        if (addr < 0xC000) writable[addr] = false;
        ++writeWatchCount_;
        return;
    }
    if (writeWatch_.empty() || !(writeWatch_[addr] & kWatchArmed)) return;
    // An address the aux shadow still covers stays diverted (its slow-path
    // permission comes from ramWritable(), which knows about the shadow).
    if (addr < 0xC000 && !auxShadowCovers(addr) && !videoWriteLogCovers(addr))
        writable[addr] = (writeWatch_[addr] & kWatchWasWritable) != 0;
    writeWatch_[addr] = 0;
    if (--writeWatchCount_ == 0) {
        // Un-armed means un-allocated, same rule as Debugger's bitmaps: the
        // table exists only while somebody is debugging.
        writeWatch_.clear();
        writeWatch_.shrink_to_fit();
    }
}

void Memory::setReadWatch(uint16_t addr, bool on)
{
    if (on) {
        if (readWatch_.empty()) readWatch_.assign(0x10000, 0);
        if (readWatch_[addr]) return;                     // keep the count honest
        readWatch_[addr] = 1;
        ++readWatchCount_;
        readDivert_ = true;
        refreshReadFastFlags();   // closes the three fast-path gates
        return;
    }
    if (readWatch_.empty() || !readWatch_[addr]) return;
    readWatch_[addr] = 0;
    if (--readWatchCount_ == 0) clearReadWatches();
}

void Memory::clearReadWatches()
{
    // Flag first, table second: memReadSlow only indexes `readWatch_` when
    // `readDivert_` is set, so this order keeps the pair consistent at every
    // point (callers hold stateMutex anyway — syncDebugHook runs under it).
    readDivert_ = false;
    refreshReadFastFlags();   // reopens the gates
    readWatch_.clear();
    readWatch_.shrink_to_fit();
    readWatchCount_ = 0;
}

void Memory::clearWriteWatches()
{
    if (writeWatch_.empty()) return;
    for (std::size_t a = 0; a < writeWatch_.size(); ++a) {
        if (!(writeWatch_[a] & kWatchArmed)) continue;
        if (a < 0xC000 && !auxShadowCovers(static_cast<uint16_t>(a)) &&
            !videoWriteLogCovers(static_cast<uint16_t>(a)))
            writable[a] = (writeWatch_[a] & kWatchWasWritable) != 0;
    }
    writeWatch_.clear();
    writeWatch_.shrink_to_fit();
    writeWatchCount_ = 0;
}

// ─── Aux shadow — the Le Chat Mauve Eve's CPREG auto-write ───────────────
//
// Memory.h § Aux shadow. The diversion is the same trick as the write
// watch: clear `writable[]` over the page so memWrite's own test sends the
// write to memWriteSlow, where the aux byte is added. The two pages are RAM
// on every profile, so restoring means `true` — except where a watchpoint
// holds the address diverted for its own reasons, in which case the byte is
// left alone (the watch's shadow already records the real permission).
void Memory::setAuxShadow(bool textPage, bool hgrPage, uint8_t byte)
{
    auxShadowByte_ = byte;
    auto apply = [&](bool& flag, bool on, uint16_t lo, uint16_t hi) {
        if (flag == on) return;
        flag = on;
        for (uint32_t a = lo; a <= hi; ++a) {
            if (hasWriteWatch(static_cast<uint16_t>(a))) continue;
            // The video write log may hold the page diverted on its own.
            writable[a] = !on && !videoWriteLogCovers(static_cast<uint16_t>(a));
        }
    };
    apply(auxShadowText_, textPage, 0x0400, 0x07FF);
    apply(auxShadowHgr_,  hgrPage,  0x2000, 0x3FFF);
}
