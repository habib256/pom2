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

// Memory — the video write log (Memory.h § Video write log). One concern:
// what the video RAM held when the beam fetched each band of the last
// published frame.
//
// The display paints a frame AFTER the machine ran it, from live RAM. On the
// real machine the scanner fetches RAM during the scan, 40 bytes per line
// from cycle 25 of each 65-cycle line (Sather; the floating bus in Memory.cpp
// uses the same geometry). So while armed, every CPU write into the video
// pages is logged {cycle, old byte, new byte}, and the display walks RAM back
// and forth through the log as it paints its bands top to bottom:
//
//   videoRewindBegin  validate, undo every write since the frame started,
//                     sort the writes by the line from which the beam shows
//                     them (their "cut")
//   videoRewindTo(y)  apply / undo writes so RAM is what line y fetched
//   videoRewindEnd    re-apply everything: RAM is exactly live again
//
// The painters are untouched: they read `mem.data()` / `videoAuxData()` as
// before, and those arrays hold beam-time bytes while a band paints. All of
// it runs under the state lock the display already holds to paint, so the
// CPU never sees the rewound RAM.

#include "Memory.h"
#include "M6502.h"

#include <algorithm>
#include <climits>

namespace {
constexpr uint64_t kLineCycles     = 65;   // NTSC and PAL alike
constexpr uint64_t kFetchStart     = 25;   // first visible cycle of a line
constexpr uint16_t kVisibleLines   = 192;

// Byte column a video address is fetched at, on whatever line shows it: text
// and hi-res both lay a line out as 40 bytes inside a 128-byte group, three
// lines to a group, and the last 8 bytes are screen holes (never fetched).
int fetchColumn(uint16_t addr)
{
    const int off = addr & 0x7F;
    return off < 120 ? off % 40 : 40;
}
}  // namespace

uint8_t* Memory::videoWriteTarget(const VideoWrite& w)
{
    return w.toAux ? aux.data() : mem.data();
}

void Memory::noteVideoWrite(uint16_t addr, uint8_t value)
{
    const uint64_t now = cycleCounter +
        (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0);
    const bool toAux = iieMode && iieWriteToAux(addr);
    auto push = [&](bool intoAux, uint8_t v) {
        // An aux write to a RamWorks bank other than 0 is not what the
        // scanner reads (videoAuxData), so it changes nothing on screen.
        if (intoAux && ramWorksBanks_ > 1 && ramWorksBank_ != 0) return;
        if (videoLog_.size() >= kVideoLogCap) {
            // Runaway (a stalled UI): drop the log, valid again from here.
            videoLog_.clear();
            videoLogValidFrom_ = now + 1;
            return;
        }
        const uint8_t old = intoAux ? aux[addr] : mem[addr];
        // A write that leaves the byte as it was is invisible at any time,
        // so it needs no band of its own (a loop re-poking the same text).
        if (old == v) return;
        videoLog_.push_back({now, addr, old, v, intoAux});
    };
    push(toAux, value);
    // The Eve's CPREG auto-write lands in aux beside a main write.
    if (!toAux && auxShadowCovers(addr)) push(true, auxShadowByte_);
}

void Memory::setVideoWriteLog(bool on)
{
    if (on == videoLogArmed_) return;
    videoLogArmed_ = on;   // first, so videoWriteLogCovers agrees below
    for (uint32_t a = 0x0400; a < 0x6000; ++a) {
        if (a >= 0x0C00 && a < 0x2000) continue;
        const auto addr = static_cast<uint16_t>(a);
        if (hasWriteWatch(addr)) continue;   // its shadow keeps the permission
        if (on)                         writable[a] = false;
        else if (!auxShadowCovers(addr)) writable[a] = true;   // RAM pages
    }
    videoLog_.clear();
    videoLogValidFrom_ = cycleCounter;
    vrActive_ = false;
}

void Memory::onVideoFramePublished(uint64_t frameStart, bool hadEvents)
{
    videoPublishedStart_ = frameStart;
    if (hadEvents) {
        videoQuietFrames_ = 0;
        setVideoWriteLog(true);
    } else if (videoLogArmed_ && ++videoQuietFrames_ > kVideoLogQuietFrames) {
        setVideoWriteLog(false);
        return;
    }
    // Writes from before the published frame can no longer matter.
    const auto firstKept = std::find_if(videoLog_.begin(), videoLog_.end(),
        [frameStart](const VideoWrite& w) { return w.cycle >= frameStart; });
    videoLog_.erase(videoLog_.begin(), firstKept);
}

bool Memory::videoRewindBegin(const std::function<bool(uint16_t, bool)>* cutFilter)
{
    vrActive_ = false;
    vrCuts_.clear();
    if (!videoLogArmed_ || videoLogValidFrom_ > videoPublishedStart_) return false;
    const uint64_t frameStart = videoPublishedStart_;
    // A rewind / snapshot load moved the clock back under the log, or RAM
    // was rewritten behind it: the newest logged byte of every address must
    // still be what RAM holds, or undoing the log would invent a picture.
    bool stale = !videoLog_.empty() && videoLog_.back().cycle > cycleCounter + 16;
    if (!stale) {
        vrSeen_.assign(0xC000, 0);
        for (auto it = videoLog_.rbegin(); it != videoLog_.rend(); ++it) {
            uint8_t& seen = vrSeen_[(it->toAux ? 0x6000u : 0u) + it->addr];
            if (seen) continue;
            seen = 1;
            if (videoWriteTarget(*it)[it->addr] != it->newValue) { stale = true; break; }
        }
    }
    if (stale) {
        videoLog_.clear();
        videoLogValidFrom_ = cycleCounter;
        return false;
    }

    // The line from which the beam shows each write. A write during line L's
    // fetch shows on L only if its byte had not been fetched yet.
    const std::size_t n = videoLog_.size();
    vrCutOf_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const VideoWrite& w = videoLog_[i];
        const uint64_t rel  = w.cycle - frameStart;   // trimmed: cycle >= start
        const uint64_t line = rel / kLineCycles;
        const uint64_t h    = rel % kLineCycles;
        uint16_t cut = kVisibleLines;
        if (line < kVisibleLines) {
            cut = static_cast<uint16_t>(line);
            if (h >= kFetchStart &&
                static_cast<uint64_t>(fetchColumn(w.addr)) < h - kFetchStart)
                ++cut;
        }
        vrCutOf_[i] = cut;
    }
    // Stable by cut: one address's writes keep their log order, because its
    // cuts never decrease along the log (same column, later or equal line).
    vrOrder_.resize(n);
    for (std::size_t i = 0; i < n; ++i) vrOrder_[i] = static_cast<uint32_t>(i);
    std::stable_sort(vrOrder_.begin(), vrOrder_.end(),
        [this](uint32_t a, uint32_t b) { return vrCutOf_[a] < vrCutOf_[b]; });
    for (uint32_t i : vrOrder_) {
        const uint16_t c = vrCutOf_[i];
        if (c < 1 || c >= kVisibleLines || (!vrCuts_.empty() && vrCuts_.back() == c))
            continue;
        if (cutFilter && !(*cutFilter)(videoLog_[i].addr, videoLog_[i].toAux)) continue;
        vrCuts_.push_back(c);
    }
    // Back to the frame start: undo newest first.
    for (auto it = videoLog_.rbegin(); it != videoLog_.rend(); ++it)
        videoWriteTarget(*it)[it->addr] = it->oldValue;
    vrApplied_ = 0;
    vrActive_ = true;
    return true;
}

void Memory::videoRewindTo(int line)
{
    if (!vrActive_) return;
    const std::size_t n = vrOrder_.size();
    while (vrApplied_ < n && vrCutOf_[vrOrder_[vrApplied_]] <= line) {
        const VideoWrite& w = videoLog_[vrOrder_[vrApplied_++]];
        videoWriteTarget(w)[w.addr] = w.newValue;
    }
    while (vrApplied_ > 0 && vrCutOf_[vrOrder_[vrApplied_ - 1]] > line) {
        const VideoWrite& w = videoLog_[vrOrder_[--vrApplied_]];
        videoWriteTarget(w)[w.addr] = w.oldValue;
    }
}

void Memory::videoRewindEnd()
{
    videoRewindTo(INT_MAX);
    vrActive_ = false;
}
