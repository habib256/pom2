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
#include "ByteIO.h"

#include <algorithm>
#include <climits>

namespace {
constexpr uint64_t kLineCycles     = 65;   // NTSC and PAL alike
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

void Memory::appendSdkVideoState(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    std::lock_guard<std::mutex> lock(stateMutex);
    const auto putDisplay = [&](const DisplayState& state) {
        for (bool value : {state.textMode, state.mixedMode, state.page2, state.hiRes,
                          state.eightyCol, state.an3, state.altChar, state.dhgr,
                          state.eightyStore, state.an0, state.an2})
            putU8(out, value ? 1 : 0);
    };
    putU64(out, lastVideoFrameStart_);
    putU64(out, videoPublishedStart_);
    putU64(out, videoLogValidFrom_);
    putU32(out, static_cast<uint32_t>(videoQuietFrames_));
    putU8(out, videoLogArmed_ ? 1 : 0);
    putDisplay(displayAtFrameStart_);
    putDisplay(publishedFrameStart_);
    const auto putEvents = [&](const std::vector<VideoEvent>& events) {
        putU32(out, static_cast<uint32_t>(events.size()));
        for (const auto& event : events) {
            putU64(out, event.emuCycle);
            putU16(out, event.scanline);
            putU8(out, static_cast<uint8_t>(event.kind));
            putU8(out, event.value ? 1 : 0);
        }
    };
    putEvents(videoEvents_);
    putEvents(publishedEvents_);
    putU32(out, static_cast<uint32_t>(videoLog_.size()));
    for (const auto& entry : videoLog_) {
        putU64(out, entry.cycle);
        putU16(out, entry.addr);
        putU8(out, entry.oldValue);
        putU8(out, entry.newValue);
        putU8(out, entry.toAux ? 1 : 0);
    }
}

bool Memory::loadSdkVideoState(const uint8_t* data, size_t size, bool apply)
{
    pom2::byteio::Reader r(data, size);
    if (!data || !r.has(24 + 4 + 1 + 22)) return false;
    const auto frameStart = r.u64(), publishedStart = r.u64(), validFrom = r.u64();
    const auto quietFrames = r.u32();
    const auto armed = r.u8();
    constexpr uint64_t maxClock = uint64_t{1} << 61;
    if (armed > 1 || quietFrames > kVideoLogQuietFrames + 1 ||
        frameStart >= maxClock || publishedStart >= maxClock || validFrom >= maxClock)
        return false;
    const auto getDisplay = [&](DisplayState& state) {
        for (auto* field : {&state.textMode, &state.mixedMode, &state.page2, &state.hiRes,
                            &state.eightyCol, &state.an3, &state.altChar, &state.dhgr,
                            &state.eightyStore, &state.an0, &state.an2}) {
            const auto value = r.u8();
            if (value > 1) return false;
            *field = value != 0;
        }
        return true;
    };
    DisplayState active, published;
    if (!getDisplay(active) || !getDisplay(published)) return false;
    const auto getEvents = [&](std::vector<VideoEvent>& events) {
        if (!r.has(4)) return false;
        const auto count = r.u32();
        if (count > kVideoLogCap || !r.has(size_t(count) * 12)) return false;
        if (apply) events.reserve(count);
        uint64_t previous = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto cycle = r.u64();
            const auto line = r.u16();
            const auto kind = r.u8(), value = r.u8();
            if (cycle < previous || cycle >= maxClock || line > kVisibleLines ||
                kind >= static_cast<uint8_t>(VideoEventKind::Cut) || value > 1) return false;
            if (apply) events.push_back({cycle, line, static_cast<VideoEventKind>(kind), value != 0});
            previous = cycle;
        }
        return true;
    };
    std::vector<VideoEvent> activeEvents, publishedEvents;
    if (!getEvents(activeEvents) || !getEvents(publishedEvents) || !r.has(4)) return false;
    const auto count = r.u32();
    if (count > kVideoLogCap || (!armed && count) || size - r.pos != size_t(count) * 13)
        return false;
    std::vector<VideoWrite> writes;
    if (apply) writes.reserve(count);
    uint64_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto cycle = r.u64();
        const auto address = r.u16();
        const auto oldValue = r.u8(), newValue = r.u8(), auxiliary = r.u8();
        if (cycle < previous || cycle >= maxClock || auxiliary > 1 ||
            !((address >= 0x0400 && address < 0x0c00) ||
              (address >= 0x2000 && address < 0x6000))) return false;
        if (apply) writes.push_back({cycle, address, oldValue, newValue, auxiliary != 0});
        previous = cycle;
    }
    if (apply) {
        std::lock_guard<std::mutex> lock(stateMutex);
        setVideoWriteLog(armed != 0);
        displayAtFrameStart_ = active;
        publishedFrameStart_ = published;
        videoEvents_ = std::move(activeEvents);
        publishedEvents_ = std::move(publishedEvents);
        videoLog_ = std::move(writes);
        lastVideoFrameStart_ = frameStart;
        videoPublishedStart_ = publishedStart;
        videoLogValidFrom_ = validFrom;
        videoQuietFrames_ = static_cast<int>(quietFrames);
    }
    return true;
}

uint8_t* Memory::videoWriteTarget(const VideoWrite& w)
{
    // An aux entry describes RamWorks bank 0 (noteVideoWrite logs nothing
    // else), and bank 0 is not `aux` while another bank is selected — it is
    // the backing store's first slot, where the scanner reads it.
    if (!w.toAux) return mem.data();
    return ramWorksAwayFromBank0() ? ramWorksBacking_.data() : aux.data();
}

void Memory::noteVideoWrite(uint16_t addr, uint8_t value)
{
    const uint64_t now = videoAccessCycle();
    const bool toAux = iieMode && iieWriteToAux(addr);
    auto push = [&](bool intoAux, uint8_t v) {
        // An aux write to a RamWorks bank other than 0 is not what the
        // scanner reads (videoAuxData), so it changes nothing on screen.
        if (intoAux && ramWorksAwayFromBank0()) return;
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
    // The Eve's CPREG auto-write lands in aux beside a main write — on a
    // //e-class machine only, as in memWriteSlow.
    if (iieMode && !toAux && auxShadowCovers(addr)) push(true, auxShadowByte_);
}

uint64_t Memory::nextVblCpuCycle(uint64_t cpuNow) const
{
    // The grid advanceCyclesVideo keeps: frame base = v - v % frame, VBL
    // from scanline 192 — in video cycles, mapped back to the CPU's.
    const uint64_t frame = 65u * static_cast<uint64_t>(
        pom2VideoTiming(videoStandard_.load(std::memory_order_relaxed)).scanlinesPerFrame);
    const uint64_t v = videoCycleAt(cpuNow);
    uint64_t edge = v - (v % frame) + 192u * 65u;
    if (edge <= v) edge += frame;
    const uint64_t c = cpuCycleForVideo(edge);
    return c > cpuNow ? c : cpuNow + 1;
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
        else if (!auxShadowCovers(addr) && !heatSink_) writable[a] = true;   // RAM pages
    }
    videoLog_.clear();
    videoLogValidFrom_ = videoNow();
    vrActive_ = false;
}

// A clock jump (rewind, snapshot or session load): the soft-switch log and
// the write log both describe a timeline that no longer exists.
void Memory::resetVideoEventLogForClockJump()
{
    // Not a jump: the caller re-executed up to this very clock, and the log
    // describes it (Memory::setKeepVideoLogOnRestore — the debugger's Step
    // Back). Honoured here because a restore reaches this from two places.
    if (keepVideoLogOnRestore_) return;
    videoEvents_.clear();
    publishedEvents_.clear();
    resetVideoWriteLogForClockJump();
    constexpr uint64_t kCyclesPerScanline = 65;   // as in advanceCycles
    const uint64_t kCyclesPerFrame =
        kCyclesPerScanline *
        static_cast<uint64_t>(
            pom2VideoTiming(videoStandard_.load()).scanlinesPerFrame);
    lastVideoFrameStart_ = videoNow() - (videoNow() % kCyclesPerFrame);
}

void Memory::resetVideoWriteLogForClockJump()
{
    // The log's cycles belong to the old timeline: undoing them would write
    // bytes of a future that no longer happens into RAM.
    videoLog_.clear();
    videoLogValidFrom_ = videoNow();
    vrActive_ = false;
}

void Memory::onVideoFramePublished(uint64_t frameStart, bool hadEvents)
{
    videoPublishedStart_ = frameStart;
    if (hadEvents || beamView_) {      // the beam view needs beam-time RAM
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
    bool stale = !videoLog_.empty() && videoLog_.back().cycle > videoNow() + 16;
    // The cut computation below needs every entry inside or after the
    // published frame, in clock order. A clock that jumped under the log
    // (rewind, snapshot) breaks both, and a write logged after the jump
    // hides it from the test above.
    for (std::size_t i = 0; !stale && i < videoLog_.size(); ++i)
        stale = videoLog_[i].cycle < frameStart ||
                (i && videoLog_[i].cycle < videoLog_[i - 1].cycle);
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
        videoLogValidFrom_ = videoNow();
        return false;
    }

    // Beam view: the picture is rolled around the beam's line B — lines
    // B..191 were scanned in the published frame, lines 0..B-1 in the frame
    // in progress — so a cut becomes a KEY in scan order: key(y) = y - B for
    // the old lines, y + 192 - B for the new ones (rollKey). A write the beam
    // has not reached yet gets kNever and stays out of the picture.
    const bool rolling = beamViewActive();
    const uint64_t frameLen = kLineCycles * static_cast<uint64_t>(
        pom2VideoTiming(videoStandard_.load(std::memory_order_relaxed)).scanlinesPerFrame);
    constexpr uint16_t kNever = 0x7FFF;
    uint16_t beam = 0;
    if (rolling)
        beam = static_cast<uint16_t>(std::min<uint64_t>(
            kVisibleLines, (videoNow() - lastVideoFrameStart_) / kLineCycles));
    vrRollBase_ = rolling ? static_cast<int>(beam) : -1;

    // The line from which the beam shows each write. A write during line L's
    // fetch shows on L only if its byte had not been fetched yet.
    const std::size_t n = videoLog_.size();
    vrCutOf_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const VideoWrite& w = videoLog_[i];
        uint64_t rel = w.cycle - frameStart;          // trimmed: cycle >= start
        const bool inProgress = rolling && rel >= frameLen;
        if (inProgress) rel -= frameLen;
        const uint64_t line = rel / kLineCycles;
        const uint64_t h    = rel % kLineCycles;
        uint16_t cut = kVisibleLines;
        if (line < kVisibleLines) {
            cut = static_cast<uint16_t>(line);
            // Already fetched on this line: the store first shows on the
            // next one. Same column rule as a soft switch (CpuClock.h).
            if (fetchColumn(w.addr) < static_cast<int>(h) - kVideoAccessColumnOffset)
                ++cut;
        }
        if (rolling) {
            if (inProgress)
                cut = cut < beam ? static_cast<uint16_t>(cut + kVisibleLines - beam) : kNever;
            else
                cut = cut < beam ? uint16_t{0} : static_cast<uint16_t>(cut - beam);
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
        uint16_t c = vrCutOf_[i];
        if (c < 1 || c >= kVisibleLines) continue;
        if (rolling)      // key back to the line it starts on
            c = c < kVisibleLines - beam ? static_cast<uint16_t>(c + beam)
                                         : static_cast<uint16_t>(c - (kVisibleLines - beam));
        if (c < 1 || (!rolling && !vrCuts_.empty() && vrCuts_.back() == c)) continue;
        if (cutFilter && !(*cutFilter)(videoLog_[i].addr, videoLog_[i].toAux)) continue;
        vrCuts_.push_back(c);
    }
    if (rolling) {
        // RAM also changes hands AT the beam: the line above it is the newest
        // on the tube, the beam's own line the oldest.
        if (n && beam >= 1 && beam < kVisibleLines) vrCuts_.push_back(beam);
        std::sort(vrCuts_.begin(), vrCuts_.end());
        vrCuts_.erase(std::unique(vrCuts_.begin(), vrCuts_.end()), vrCuts_.end());
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
    if (vrRollBase_ >= 0 && line != INT_MAX) {     // line → scan-order key
        line = std::clamp(line, 0, static_cast<int>(kVisibleLines) - 1);
        line = line >= vrRollBase_ ? line - vrRollBase_
                                   : line + static_cast<int>(kVisibleLines) - vrRollBase_;
    }
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
    vrRollBase_ = -1;
}

// ─── //c VBLINT, read inside the instruction ────────────────────────────
//
// `vblIrqPending` is set by advanceCyclesVideo(), which runs AFTER the
// instruction, together with the IRQ line. So a `LDA $C019` whose read
// cycle landed on or after the VBL edge returned 0, the interrupt was taken
// right after it, and a handler that acknowledges (`BIT $C070`) cleared the
// flag before the next read: a poller with the interrupt enabled never saw
// bit 7. French Touch's DIGIDREAM SID Voice Edition (DIX `DD2/main.a`,
// "wait for start of VBL (IIc specific) and INT occurs", `LDA VERTBLANK /
// BPL`) froze on every //c — the browser's default machine (2026-11-07).
//
// On the silicon the LDA that straddles the edge reads the flag SET: the
// edge latches VBLINT at once, the 6502 sampled /IRQ on the instruction's
// penultimate cycle and still completes the read, N survives the interrupt
// and the BPL falls through. The loop's phase walks across the edge a cycle
// a frame (20280 mod 7 = 1), so it exits within a few frames. [D] MAME's
// cycle-stepped m65c02 has the same order for free (apple2e.cpp @ 5346a8ce
// :1397-1400 raises VBLINT inside the instruction). POM2's core samples IRQ
// at the instruction's end (M6502.cpp, "interrupt sampling is
// instruction-granular"), which is unchanged: only the READ looks ahead to
// the access cycle. Pinned by `vbl_ioudis_annunciator` §5 and the corpus
// title `dix-digidream-sid-iic`.
bool Memory::vblintEdgeInFlight() const
{
    if (!vblIrqMask || !vblWasActive || vblFrameCycles_ == 0) return false;
    const uint64_t edge = vblFrameBase_ + 192 * 65;   // first VBL line, video cycles
    // One cycle ahead of the access, like $C019 on the //e: the CPU's PHASE 0
    // read sees the counters already advanced ([A] arekkusu42's BEAMPOS on
    // the //c; Memory.cpp $C019 for the full note).
    const uint64_t va   = videoAccessCycle() + 1;
    return va >= edge && va - vblFrameBase_ < vblFrameCycles_;
}
