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

// CycleMonitor — French Touch's "Monitoring Card" protocol, read by the
// Memory Heatmap (UI-022): four write-only soft switches a program pokes to
// have the debugger time a stretch of its own code.
//
//   write $C067   start a section      write $C066   stop it
//   write $C069   start the IRQ span   write $C068   stop it
//
// [D] AppleWin FT (Fr3nchT0uch/AppleWin, branch `extended_debugger` @
// 52ddd0214ad269fc9838572ec0cbf1c9563b8601): source/Memory.cpp
// `IOWrite_C06x` cases 6-9 call `CMonitoringCard::stopMonitoring(false)` /
// `startMonitoring(false)` / `stopMonitoring(true)` / `startMonitoring(true)`;
// source/MonitoringCard.cpp keeps the elapsed cycles (`g_nCumulativeCycles`
// at the stop minus at the start); source/Debugger/Debugger_Heatmap.cpp draws
// them under the map (sectionBar / irqBar below). There is no card: the
// "card" is those four addresses. French Touch demo code brackets its frame
// work with them, so a debugger that honours them times the demo as FT did.
//
// On the machine itself these writes do nothing: $C060-$C06F is the game-port
// INPUT decode, and POM2's Memory::softSwitchAccess drops every write there
// ([D] MAME apple2.cpp `.nopw()` / apple2e.cpp, as cited at that line; [F]
// not re-read on the silicon for this file — settled by: STA $C067 on a real
// //e changes no paddle, button or cassette reading). So the protocol is read
// by OBSERVING the write, never by acting on it: no state of the machine
// changes, and a READ of $C066 / $C067 stays the paddle read it is. POM2
// claimants checked: the FASTChip's registers are $C06A-$C06F (FastChip.cpp),
// the Zip Chip's $C05A-$C05F, PaddleInputs reads $C064-$C067 and $C068
// mirrors the cassette input ON READ; none of them takes a write to
// $C066-$C069. The heatmap only records while its window is open, and so
// does this.
//
// Where POM2 differs from FT, deliberately:
//   * The stamp is the write's own bus cycle (Memory::accessCycle). [D] FT
//     reads `g_nCumulativeCycles` as it stands — `IOWrite_C06x` does not call
//     `CpuCalcCycles(nExecutedCycles)` first, as AppleWin's own I/O handlers
//     do when they need the time (Memory.cpp:2098 there) — so its count can
//     lag by the cycles run since the last sync.
//   * A stop with no start is counted apart (`orphanStops`), not measured
//     against a stale start ([D] FT subtracts whatever `startCycles` holds).
//   * "IRQ inside the section" sums every IRQ span closed while a section
//     was open. [D] FT's `elapsedInterruptCyclesTotal` is reset by each IRQ
//     START, so it always equals the last span; the heatmap never drew it.
//   * The frame is the machine's (17030 NTSC, 20280 PAL), in VIDEO cycles so
//     an accelerator does not inflate the share. [D] FT divides by
//     `NTSC_GetCyclesPerFrame()` (17030) whatever the machine.
//
// Header-only and GL-free: MemoryHeatmap records with it, the window and the
// test `memory_heatmap` read it.

#ifndef POM2_CYCLE_MONITOR_H
#define POM2_CYCLE_MONITOR_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pom2 {

class CycleMonitor {
public:
    static constexpr uint16_t kSectionStop = 0xC066, kSectionStart = 0xC067;
    static constexpr uint16_t kIrqStop = 0xC068, kIrqStart = 0xC069;
    /// One of the four marks? A single compare: this runs on every write the
    /// heatmap sees while it is open.
    static bool isMark(uint16_t addr) { return static_cast<uint16_t>(addr - kSectionStop) < 4u; }

    /// One kind of span (section or IRQ): the last, the extremes, the mean.
    struct Stats {
        uint64_t last = 0, lastVideo = 0;   // CPU cycles / video cycles
        uint64_t min = 0, max = 0, total = 0;
        uint32_t count = 0;
        double average() const { return count ? static_cast<double>(total) / count : 0.0; }
        void add(uint64_t cycles, uint64_t video)
        {
            last = cycles;
            lastVideo = video;
            min = count ? std::min(min, cycles) : cycles;
            max = std::max(max, cycles);
            total += cycles;
            ++count;
        }
    };
    static constexpr std::size_t kHistory = 120;
    /// The last spans, oldest first (video cycles), for a sparkline.
    struct History {
        std::array<uint32_t, kHistory> v{};
        std::size_t head = 0, size = 0;
        void push(uint64_t x)
        {
            v[head] = static_cast<uint32_t>(std::min<uint64_t>(x, 0xFFFFFFFFu));
            head = (head + 1) % kHistory;
            size = std::min(size + 1, kHistory);
        }
        uint32_t at(std::size_t i) const { return v[(head + kHistory - size + i) % kHistory]; }
    };

    /// A write to one of the four marks at CPU cycle `cycle` (the access's
    /// own cycle) and video cycle `video`.
    void mark(uint16_t addr, uint64_t cycle, uint64_t video)
    {
        ++marks_;
        switch (addr) {
        case kSectionStart:
            section_.open = true;
            section_.start = cycle;
            section_.startVideo = video;
            irqInside_ = 0;
            break;
        case kSectionStop:
            if (!section_.open || cycle < section_.start) { ++orphanStops_; section_.open = false; break; }
            sectionStats_.add(cycle - section_.start, video - section_.startVideo);
            sectionHistory_.push(video - section_.startVideo);
            irqInLastSection_ = irqInside_;
            section_.open = false;
            break;
        case kIrqStart:
            irq_.open = true;
            irq_.start = cycle;
            irq_.startVideo = video;
            break;
        case kIrqStop:
            if (!irq_.open || cycle < irq_.start) { ++orphanStops_; irq_.open = false; break; }
            irqStats_.add(cycle - irq_.start, video - irq_.startVideo);
            irqHistory_.push(video - irq_.startVideo);
            if (section_.open) irqInside_ += cycle - irq_.start;
            irq_.open = false;
            break;
        default:
            --marks_;
            break;
        }
    }
    void reset() { *this = CycleMonitor{}; }

    const Stats& section() const { return sectionStats_; }
    const Stats& irq() const { return irqStats_; }
    const History& sectionHistory() const { return sectionHistory_; }
    const History& irqHistory() const { return irqHistory_; }
    /// CPU cycles of the IRQ spans closed inside the last finished section.
    uint64_t irqInLastSection() const { return irqInLastSection_; }
    bool sectionOpen() const { return section_.open; }
    bool irqOpen() const { return irq_.open; }
    uint64_t marks() const { return marks_; }
    uint64_t orphanStops() const { return orphanStops_; }

    // ── FT's two bars, GL-free ─────────────────────────────────────────────
    /// What Debugger_Heatmap.cpp draws for `video` cycles: the scale (1, 4 or
    /// 8 frames, with a tick at each frame), the share of that scale filled,
    /// and the colour. [D] FT @ 52ddd021, "CPU monitoring": <= 1 frame green
    /// 0x0000DD00, <= 4 frames orange 0x000077FF, else red 0x000000FF, the
    /// bar 256 px for the scale and clamped at 255 px; "CPU IRQ monitoring":
    /// light blue 0x00FF7000 over one frame. Win32 COLORREF is 0x00BBGGRR.
    struct Bar { int frames = 1; float fill = 0.0f; uint8_t r = 0, g = 0, b = 0; };
    static Bar sectionBar(uint64_t video, int frameCycles)
    {
        Bar bar;
        const uint64_t f = static_cast<uint64_t>(std::max(1, frameCycles));
        if (video <= f) { bar.frames = 1; bar.r = 0x00; bar.g = 0xDD; bar.b = 0x00; }
        else if (video <= 4 * f) { bar.frames = 4; bar.r = 0xFF; bar.g = 0x77; bar.b = 0x00; }
        else { bar.frames = 8; bar.r = 0xFF; bar.g = 0x00; bar.b = 0x00; }
        bar.fill = std::min(255.0f / 256.0f,
                            static_cast<float>(video) / static_cast<float>(f * static_cast<uint64_t>(bar.frames)));
        return bar;
    }
    static Bar irqBar(uint64_t video, int frameCycles)
    {
        Bar bar;
        bar.r = 0x00; bar.g = 0x70; bar.b = 0xFF;
        bar.fill = std::min(255.0f / 256.0f, static_cast<float>(video) /
                                                  static_cast<float>(std::max(1, frameCycles)));
        return bar;
    }
    /// Scan lines: 65 cycles a line on every Apple II, NTSC and PAL ([B]
    /// Sather, Understanding the Apple II, ch. 3, as quoted in CpuClock.h).
    static double lines(uint64_t video) { return static_cast<double>(video) / 65.0; }

private:
    struct Open { bool open = false; uint64_t start = 0, startVideo = 0; };
    Open section_, irq_;
    Stats sectionStats_, irqStats_;
    History sectionHistory_, irqHistory_;
    uint64_t irqInside_ = 0, irqInLastSection_ = 0;
    uint64_t marks_ = 0, orphanStops_ = 0;
};

}  // namespace pom2

#endif  // POM2_CYCLE_MONITOR_H
