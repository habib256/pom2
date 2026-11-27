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
#include "M6502.h"

#include "CpuClock.h"

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
        // Not a throw: the seam between the two frames (kComposite), which
        // an inspector listing what the PROGRAM did leaves out.
        if (was != is) out.push_back({now, line, kind, is, VideoEvent::kComposite});
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
        out.push_back({e.emuCycle + frame, e.scanline, e.kind, e.value, e.pc});
    }
    return out;
}

namespace {
// Byte column from which an event shows on its line: a switch at horizontal
// cycle h governs column h - kVideoAccessColumnOffset onward (CpuClock.h),
// the whole line when thrown in horizontal blanking.
int eventColumn(const Memory::VideoEvent& e)
{
    const int c = static_cast<int>(e.emuCycle % kLineCycles) - kVideoAccessColumnOffset;
    return c < 0 ? 0 : (c > 40 ? 40 : c);
}
}  // namespace

// Who threw the switch. The CPU keeps no "address of the instruction in
// progress" — a store per instruction on the hottest path of the emulator,
// for a debugger's list (docs/PERFORMANCE.md §§ 8-9) — so it is worked back
// from where the program counter stands DURING the access: past the operand.
// A soft switch lives at $C0xx, and nearly every access to one is an absolute
// instruction, three bytes, whose operand's high byte is $C0 (or $BF, indexed
// up into the page): the instruction starts three bytes back. Anything else
// reaching a switch is a two-byte indirect — `STA ($06),Y`.
// An indirect through a zero-page pointer that is itself at $C0 or $BF
// (`STA ($C0),Y`) has that same byte before the program counter, so the
// OPCODE decides: the 6502's addressing mode is in its low five bits —
// $0C/$0D/$0E absolute, $19 absolute,Y, $1C/$1D/$1E absolute,X; $01 (zp,X),
// $11 (zp),Y, $12 the 65C02's (zp).
// [F] Left undecided only when the bytes read both ways (an absolute opcode
// three back AND an indirect one two back, over a pointer at $C0/$BF): the
// absolute form is reported. Settled by: the CPU latching its opcode
// address, if a use ever pays for it. Pinned by `debugger`.
int32_t Memory::videoEventPc() const
{
    // A card's processor holding the bus (a SoftCard Z80 writing a display
    // switch): the 6502 is parked, and its PC threw nothing.
    if (!cpu || unattributedAccess_ || slots.dmaClaimant()) return VideoEvent::kNoPc;
    const uint16_t after = cpu->getProgramCounter();
    const uint8_t high = peekCpuView(static_cast<uint16_t>(after - 1));
    bool absolute = high == 0xC0 || high == 0xBF;
    if (absolute) {
        const uint8_t mode3 = peekCpuView(static_cast<uint16_t>(after - 3)) & 0x1F;
        const uint8_t mode2 = peekCpuView(static_cast<uint16_t>(after - 2)) & 0x1F;
        // ($0F / $1B / $1F: the NMOS part's undocumented absolutes — SAX, LAX…)
        const bool looksAbsolute = mode3 == 0x0C || mode3 == 0x0D || mode3 == 0x0E ||
                                   mode3 == 0x19 || mode3 == 0x1C || mode3 == 0x1D || mode3 == 0x1E ||
                                   mode3 == 0x0F || mode3 == 0x1B || mode3 == 0x1F;
        const bool looksIndirect = mode2 == 0x01 || mode2 == 0x11 || mode2 == 0x12;
        absolute = looksAbsolute || !looksIndirect;
    }
    return static_cast<uint16_t>(after - (absolute ? 3 : 2));
}

Memory::DisplayState Memory::DisplayFrame::at(int y, int column) const
{
    DisplayState state = start;
    for (const VideoEvent& e : events) {
        if (e.scanline > y || (e.scanline == y && eventColumn(e) > column)) break;
        foldVideoEvent(state, e.kind, e.value);
    }
    return state;
}

bool Memory::DisplayFrame::splitInside(int y) const
{
    for (const VideoEvent& e : events) {
        if (e.scanline > y) break;
        if (e.scanline == y && e.kind != VideoEventKind::Cut) {
            const int c = eventColumn(e);
            if (c > 0 && c < 40) return true;
        }
    }
    return false;
}

std::array<bool, 40> Memory::DisplayFrame::splitColumns(int y) const
{
    std::array<bool, 40> columns{};
    for (const VideoEvent& e : events) {
        if (e.kind == VideoEventKind::Cut || e.scanline >= kVisibleLines) continue;
        if (y >= 0 && e.scanline != y) continue;
        const int c = eventColumn(e);
        if (c > 0 && c < 40) columns[static_cast<std::size_t>(c)] = true;
    }
    return columns;
}

Memory::DisplayFrame Memory::displayFrame()
{
    DisplayFrame frame;
    if (legacyEventBracket_) {       // tests: taking the events would steal them
        frame.start = getDisplayState();
        return frame;
    }
    frame.start  = getDisplayStateAtFrameStart();
    frame.events = takeVideoEvents();
    // Before the first frame closes there is no frame-start snapshot worth
    // the name: the live state is the only description of the screen.
    if (frame.events.empty() && videoPublishedStart_ == 0 && lastVideoFrameStart_ == 0)
        frame.start = getDisplayState();
    return frame;
}

std::array<Memory::DisplayState, 192> Memory::displayStatePerLine()
{
    const DisplayFrame frame = displayFrame();
    std::array<DisplayState, 192> lines;
    for (int y = 0; y < static_cast<int>(kVisibleLines); ++y)
        lines[static_cast<std::size_t>(y)] = frame.at(y, 0);
    return lines;
}

void Memory::captureFetchedPages(FetchedPages& out)
{
    const DisplayFrame frame = displayFrame();
    const uint8_t* const mainRam = data();
    const uint8_t* const auxRam  = iieMode ? videoAuxData() : nullptr;
    std::memcpy(out.textMain.data(),  mainRam + 0x0400, 0x0400);
    std::memcpy(out.hiResMain.data(), mainRam + 0x2000, 0x2000);
    if (auxRam) {
        std::memcpy(out.textAux.data(),  auxRam + 0x0400, 0x0400);
        std::memcpy(out.hiResAux.data(), auxRam + 0x2000, 0x2000);
    }
    out.usedText = out.usedTextAux = out.usedHiRes = out.usedHiResAux = false;

    const bool rewound = videoRewindBegin();
    for (int y = 0; y < static_cast<int>(kVisibleLines); ++y) {
        if (rewound) videoRewindTo(y);
        for (int c = 0; c < 40; ++c) {
            const DisplayState st = frame.at(y, c);
            // 80STORE turns PAGE2 into a bank switch: the scanner stays on page 1.
            const bool page2 = st.page2 && !st.eightyStore;
            const bool text  = st.textMode || (st.mixedMode && y >= 160);
            const bool wide  = st.eightyCol && (text || st.dhgr);
            if (text || !st.hiRes) {
                const uint16_t off = static_cast<uint16_t>(textRowOffset(y >> 3) + c);
                const uint16_t src = static_cast<uint16_t>((page2 ? 0x0800 : 0x0400) + off);
                out.textMain[off] = mainRam[src];
                out.usedText = true;
                if (wide && auxRam) { out.textAux[off] = auxRam[src]; out.usedTextAux = true; }
            } else {
                const uint16_t off = static_cast<uint16_t>(hiResLineOffset(y) + c);
                const uint16_t src = static_cast<uint16_t>((page2 ? 0x4000 : 0x2000) + off);
                out.hiResMain[off] = mainRam[src];
                out.usedHiRes = true;
                if (wide && auxRam) { out.hiResAux[off] = auxRam[src]; out.usedHiResAux = true; }
            }
        }
    }
    if (rewound) videoRewindEnd();
}

uint8_t Memory::fetchedVideoByte(int y, uint16_t address, bool fromAux)
{
    const uint8_t* const auxRam = iieMode ? videoAuxData() : nullptr;
    const bool rewound = videoRewindBegin();
    if (rewound) videoRewindTo(y);
    const uint8_t value = (fromAux && auxRam) ? auxRam[address] : data()[address];
    if (rewound) videoRewindEnd();
    return value;
}
