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

// Memory — the beam view (Memory.h § Beam view). One concern: describing the
// picture on the tube at THIS cycle instead of the last completed frame.
//
// The display normally paints the PUBLISHED frame: its start state, its
// soft-switch events, and RAM walked back to what each line fetched. That is
// right at full speed and useless to someone stopped at a breakpoint in the
// middle of a frame, or running at a thousandth of the speed: the picture
// then lags by up to a whole frame — twenty seconds at 0.1 %.
//
// On a tube, at cycle T of a frame, the lines above the beam show the frame
// in progress and the lines from the beam down still show the previous one.
// The beam view hands the display exactly that, as ONE synthetic frame in the
// same vocabulary:
//
//   start state   the frame in progress's (displayAtFrameStart_)
//   events        the frame in progress's, up to now
//                 + at the beam, the switches that turn the current state
//                   into what the PREVIOUS frame had at that same position
//                 + the previous frame's events from that position on
//   video RAM     MemoryVideoLog.cpp, rolled around the beam's line
//
// So no painter and no pipeline knows about it. [F] Two approximations, both
// at the beam itself and both smaller than an instruction: POM2 advances by
// whole instructions, so "now" is an instruction boundary; and video RAM
// changes hands at the START of the beam's line, not at the beam's byte.
// Settled by: a cycle-stepped core. Pinned by `beam_view`.

#include "Memory.h"

#include <cstring>


namespace {
constexpr uint64_t kLineCycles   = 65;
constexpr uint16_t kVisibleLines = 192;
}  // namespace

void Memory::foldVideoEvent(DisplayState& state, VideoEventKind kind, bool value)
{
    switch (kind) {
        case VideoEventKind::TextMode:    state.textMode    = value; break;
        case VideoEventKind::MixedMode:   state.mixedMode   = value; break;
        case VideoEventKind::Page2:       state.page2       = value; break;
        case VideoEventKind::HiRes:       state.hiRes       = value; break;
        case VideoEventKind::EightyCol:   state.eightyCol   = value; break;
        case VideoEventKind::Dhgr:        state.dhgr        = value; break;
        case VideoEventKind::An3:         state.an3         = value; break;
        case VideoEventKind::EightyStore: state.eightyStore = value; break;
        case VideoEventKind::AltChar:     state.altChar     = value; break;
        case VideoEventKind::An2:         state.an2         = value; break;
        case VideoEventKind::Cut:         break;   // a band edge only
    }
}

void Memory::setBeamView(bool on)
{
    if (on == beamView_) return;
    beamView_ = on;
    // Beam-time RAM is half of the picture: arm the write log now rather
    // than at the next frame with a soft-switch event (there may be none).
    if (on) setVideoWriteLog(true);
}

bool Memory::beamViewActive() const
{
    if (!beamView_ || legacyEventBracket_) return false;
    const uint64_t frame = kLineCycles * static_cast<uint64_t>(
        pom2VideoTiming(videoStandard_.load(std::memory_order_relaxed)).scanlinesPerFrame);
    const uint64_t now = videoNow();
    return lastVideoFrameStart_ == videoPublishedStart_ + frame &&
           now >= lastVideoFrameStart_ && now - lastVideoFrameStart_ < frame;
}

std::vector<Memory::VideoEvent> Memory::beamViewEventsLocked() const
{
    const uint64_t frame = kLineCycles * static_cast<uint64_t>(
        pom2VideoTiming(videoStandard_.load(std::memory_order_relaxed)).scanlinesPerFrame);
    const uint64_t now = videoNow();
    const uint64_t rel = now - lastVideoFrameStart_;

    std::vector<VideoEvent> out;
    out.reserve(videoEvents_.size() + publishedEvents_.size() + 10);
    DisplayState current = displayAtFrameStart_;
    for (const VideoEvent& e : videoEvents_) {
        if (e.scanline >= kVisibleLines) continue;     // VBL: next frame's
        out.push_back(e);
        foldVideoEvent(current, e.kind, e.value);
    }
    // In vertical blanking the frame in progress is complete on the tube.
    if (rel >= kLineCycles * kVisibleLines) return out;

    // What the previous frame was showing at this same position.
    DisplayState previous = publishedFrameStart_;
    for (const VideoEvent& e : publishedEvents_)
        if (e.emuCycle - videoPublishedStart_ < rel)
            foldVideoEvent(previous, e.kind, e.value);

    const auto line = static_cast<uint16_t>(rel / kLineCycles);
    const auto change = [&](VideoEventKind kind, bool was, bool is) {
        if (was != is) out.push_back({now, line, kind, is});
    };
    change(VideoEventKind::TextMode,    current.textMode,    previous.textMode);
    change(VideoEventKind::MixedMode,   current.mixedMode,   previous.mixedMode);
    change(VideoEventKind::Page2,       current.page2,       previous.page2);
    change(VideoEventKind::HiRes,       current.hiRes,       previous.hiRes);
    change(VideoEventKind::EightyCol,   current.eightyCol,   previous.eightyCol);
    change(VideoEventKind::Dhgr,        current.dhgr,        previous.dhgr);
    change(VideoEventKind::An3,         current.an3,         previous.an3);
    change(VideoEventKind::EightyStore, current.eightyStore, previous.eightyStore);
    change(VideoEventKind::AltChar,     current.altChar,     previous.altChar);
    change(VideoEventKind::An2,         current.an2,         previous.an2);

    // The rest of the previous frame, one frame later on the clock so the
    // list stays in order; its position on the line (cycle mod 65) is kept.
    for (const VideoEvent& e : publishedEvents_) {
        if (e.scanline >= kVisibleLines) continue;
        if (e.emuCycle - videoPublishedStart_ < rel) continue;
        out.push_back({e.emuCycle + frame, e.scanline, e.kind, e.value});
    }
    return out;
}

std::array<Memory::DisplayState, 192> Memory::displayStatePerLine()
{
    std::array<DisplayState, 192> lines;
    if (legacyEventBracket_) {       // tests: taking the events would steal them
        lines.fill(getDisplayState());
        return lines;
    }
    DisplayState state = getDisplayStateAtFrameStart();
    const std::vector<VideoEvent> events = takeVideoEvents();
    // Before the first frame closes there is no frame-start snapshot worth
    // the name: the live state is the only description of the screen.
    if (events.empty() && videoPublishedStart_ == 0 && lastVideoFrameStart_ == 0)
        state = getDisplayState();
    std::size_t next = 0;
    for (uint16_t y = 0; y < kVisibleLines; ++y) {
        // An event in horizontal blanking governs the whole line; one inside
        // the window splits it, and this reports the line's first byte.
        while (next < events.size() &&
               (events[next].scanline < y ||
                (events[next].scanline == y &&
                 events[next].emuCycle % kLineCycles < kLineCycles - 40))) {
            foldVideoEvent(state, events[next].kind, events[next].value);
            ++next;
        }
        lines[y] = state;
    }
    return lines;
}

void Memory::captureFetchedHiRes(uint8_t* out8k)
{
    const std::array<DisplayState, 192> lines = displayStatePerLine();
    std::memcpy(out8k, data() + 0x2000, 0x2000);       // the holes, and a base
    const bool rewound = videoRewindBegin();
    for (int y = 0; y < static_cast<int>(kVisibleLines); ++y) {
        if (rewound) videoRewindTo(y);
        const DisplayState& st = lines[static_cast<std::size_t>(y)];
        // 80STORE turns PAGE2 into a bank switch: the scanner stays on page 1.
        const uint16_t page = (st.page2 && !st.eightyStore) ? 0x4000 : 0x2000;
        const uint16_t off  = hiResLineOffset(y);
        std::memcpy(out8k + off, data() + page + off, 40);
    }
    if (rewound) videoRewindEnd();
}
