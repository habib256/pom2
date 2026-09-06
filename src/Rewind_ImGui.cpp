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

// Rewind_ImGui — see Rewind_ImGui.h.

#include "Rewind_ImGui.h"

#include "CpuClock.h"          // VideoTiming / pom2VideoTiming
#include "EmulationController.h"
#include "RewindBuffer.h"
#include "imgui.h"

#include <cstdint>
#include <mutex>

namespace pom2 {
namespace {

/// Cycles per second the machine is *actually* running at, for turning an
/// emuCycles span into the seconds figures on the timeline.
///
/// This used to be a hardcoded 1022727.0. That is the NTSC nominal, and it
/// is wrong on two of POM2's profiles:
///   * **//c Plus** carries a 4x Zip-style accelerator — 68180 cycles per
///     60 Hz frame, ~4.09 MHz. Dividing its cycle stamps by the NTSC clock
///     reported a 30-second ring as "120.0 s", disagreeing by a factor of
///     four with the "history (s)" slider sitting right beside it.
///   * **PAL** runs 20313 cycles at 50 Hz (~1.0156 MHz).
///
/// The worker's own budget is the honest source: cyclesPerFrame x refresh
/// is exactly what it spends per wall-clock second, accelerator included.
double effectiveCpuHz(EmulationController& ctrl)
{
    const VideoTiming& t = pom2VideoTiming(ctrl.getVideoStandard());
    const double hz = static_cast<double>(ctrl.getCyclesPerFrame()) *
                      static_cast<double>(t.refreshHz);
    return hz > 0.0 ? hz : static_cast<double>(t.cpuClockHz);
}

}  // namespace

void Rewind_ImGui::syncScrub(EmulationController& ctrl)
{
    // The controller owns the scrub; this flag is a view of it. Any resume
    // that did not come through this panel — toolbar Play, Machine > Run, the
    // `machine.run` palette command, the kiosk menu — ended the scrub behind
    // our back. Without this resync `scrubbing_` stayed true forever: the next
    // drag skipped beginScrubIfNeeded's park (early-out below), so it seeked a
    // RUNNING machine and the slider visibly did nothing, and a released F6
    // hold called rewindEndAndResume with a stale cursor, teleporting the
    // machine back into a timeline the user had already left.
    if (scrubbing_ && !ctrl.rewindScrubbing()) scrubbing_ = false;
}

void Rewind_ImGui::beginScrubIfNeeded(EmulationController& ctrl)
{
    // rewindBeginScrub() stops the worker and waits for it to park before it
    // touches the ring. That wait runs on the UI thread and is BOUNDED —
    // EmulationController::waitUntilParked() polls at most 200 x 1 ms and
    // returns whether the worker parked or not — so a wedged worker cannot
    // hang the window; it degrades to one ~200 ms hitch on the frame that
    // starts a scrub. In practice the worker parks within one frame budget
    // (<= ~16 ms at 60 Hz, sooner under turbo), and this runs once per scrub,
    // not once per drag frame (the early-out below).
    syncScrub(ctrl);
    if (scrubbing_) return;
    if (ctrl.rewindBeginScrub()) {
        scrubbing_ = true;
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        cursor_ = ctrl.rewind().empty() ? 0 : ctrl.rewind().size() - 1;
    }
}

void Rewind_ImGui::seekTo(EmulationController& ctrl, long index)
{
    if (index < 0) index = 0;
    const size_t got = ctrl.rewindSeek(static_cast<size_t>(index));
    if (got != RewindBuffer::kNoFrame) cursor_ = got;
}

void Rewind_ImGui::holdRewind(EmulationController& ctrl, size_t frames)
{
    beginScrubIfNeeded(ctrl);
    if (scrubbing_) seekTo(ctrl, static_cast<long>(cursor_) - static_cast<long>(frames));
}

void Rewind_ImGui::releaseHold(EmulationController& ctrl)
{
    syncScrub(ctrl);
    if (!scrubbing_) return;
    ctrl.rewindEndAndResume(cursor_);
    scrubbing_ = false;
}

Rewind_ImGui::FrameResult Rewind_ImGui::render(const char* title, bool& open,
                                               EmulationController& ctrl,
                                               float /*deltaSeconds*/)
{
    FrameResult res;

    syncScrub(ctrl);

    // Read ring stats once, under the lock.
    bool     enabled = false;
    size_t   count = 0, bytes = 0, maxFrames = 0;
    uint64_t oldest = 0, newest = 0, cursorCycle = 0;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        RewindBuffer& rb = ctrl.rewind();
        enabled   = rb.enabled();
        count     = rb.size();
        bytes     = rb.bytes();
        maxFrames = rb.maxFrames();
        oldest    = rb.oldestCycle();
        newest    = rb.newestCycle();
        if (scrubbing_ && cursor_ < count) cursorCycle = rb.infoAt(cursor_).cycle;
    }
    if (scrubbing_ && count == 0) scrubbing_ = false;   // ring was cleared
    const bool running    = ctrl.getMode() == EmulationController::Mode::Running;
    const bool haveFrames = count > 0;

    ImGui::SetNextWindowSize(ImVec2(540, 132), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) { ImGui::End(); return res; }

    // ── Record toggle + buffer stats ─────────────────────────────────────
    bool rec = enabled;
    if (ImGui::Checkbox("Record", &rec)) {
        // setEnabled is no longer just an atomic flip: re-enabling restarts
        // the delta chain (sinceKeyframe_ = 0, prevBlob_.clear() — the
        // timeline-splice fix), and the worker's capture() reads and
        // move-assigns prevBlob_ under stateMtx. Unserialised, the clear
        // races that vector access (UB), or slips after a capture and the
        // first resumed frame deltas against the stale pre-pause blob —
        // the exact splice bug the clear was added to fix. Same lock the
        // history slider below already takes.
        if (rec) {
            std::lock_guard<std::mutex> lk(ctrl.stateMutex());
            ctrl.rewind().setEnabled(true);
            res.statusMessage = "Rewind: recording on";
        } else {
            if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
            std::lock_guard<std::mutex> lk(ctrl.stateMutex());
            ctrl.rewind().setEnabled(false);
            res.statusMessage = "Rewind: recording off";
        }
    }
    ImGui::SameLine();
    const double cpuHz   = effectiveCpuHz(ctrl);
    const double spanSec = newest >= oldest ? (newest - oldest) / cpuHz : 0.0;
    ImGui::Text("%zu frames  ·  %.1f s  ·  %.1f MB",
                count, spanSec, static_cast<double>(bytes) / (1024.0 * 1024.0));
    ImGui::SameLine();
    // History length in seconds. The ring counts *frames*, so the
    // conversion is the profile's refresh rate, not a hardcoded 60 — on the
    // 50 Hz PAL profiles that constant made the slider read 20 % short.
    // Takes effect immediately; shrinking evicts the oldest frames. Disabled
    // while scrubbing because front-eviction would shift every frame index
    // and desync `cursor_`.
    const int  refreshHz = pom2VideoTiming(ctrl.getVideoStandard()).refreshHz;
    int histSec = static_cast<int>(maxFrames /
                                   static_cast<size_t>(refreshHz));
    if (histSec < 1) histSec = 1;
    ImGui::SetNextItemWidth(150.0f);
    ImGui::BeginDisabled(scrubbing_);
    if (ImGui::SliderInt("history (s)", &histSec, 5, 120)) {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        ctrl.rewind().setMaxFrames(static_cast<size_t>(histSec) *
                                   static_cast<size_t>(refreshHz));
    }
    ImGui::EndDisabled();
    if (!enabled && !haveFrames)
        ImGui::TextDisabled("Enable Record, run for a moment, then scrub the timeline below.");

    // ── Timeline ──────────────────────────────────────────────────────────
    const int maxIdx = haveFrames ? static_cast<int>(count - 1) : 0;
    int sliderVal = scrubbing_ ? static_cast<int>(cursor_) : maxIdx;
    ImGui::BeginDisabled(!haveFrames);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##timeline", &sliderVal, 0, maxIdx, "")) {
        beginScrubIfNeeded(ctrl);
        seekTo(ctrl, sliderVal);
    }
    ImGui::EndDisabled();

    if (scrubbing_) {
        const double back = newest >= cursorCycle ? (newest - cursorCycle) / cpuHz : 0.0;
        ImGui::Text("Scrubbing  -%.2f s   (frame %zu / %zu)", back, cursor_ + 1, count);
    } else {
        ImGui::TextDisabled("Live (newest)");
    }

    // ── Transport ─────────────────────────────────────────────────────────
    ImGui::BeginDisabled(!haveFrames);
    if (ImGui::Button("|<"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, 0); }
    ImGui::SameLine();
    ImGui::Button("<< hold");                          // press-and-hold = live rewind
    if (ImGui::IsItemActive() && haveFrames) {
        beginScrubIfNeeded(ctrl);
        seekTo(ctrl, static_cast<long>(cursor_) - rewindSpeed_);
    }
    ImGui::SameLine();
    if (ImGui::Button("<|"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, static_cast<long>(cursor_) - 1); }
    ImGui::SameLine();
    if (ImGui::Button("|>"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, static_cast<long>(cursor_) + 1); }
    ImGui::SameLine();
    if (ImGui::Button("resume here")) {
        if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
        else ctrl.setMode(EmulationController::Mode::Running);
        res.statusMessage = "Rewind: resumed live";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (running) {
        if (ImGui::Button("Pause")) ctrl.setMode(EmulationController::Mode::Stopped);
    } else {
        if (ImGui::Button("Play ")) {
            if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
            else ctrl.setMode(EmulationController::Mode::Running);
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    const char* speeds[] = { "1x", "2x", "4x" };
    int speedIdx = (rewindSpeed_ >= 4) ? 2 : (rewindSpeed_ >= 2 ? 1 : 0);
    if (ImGui::Combo("##rwspeed", &speedIdx, speeds, 3))
        rewindSpeed_ = (speedIdx == 2) ? 4 : (speedIdx == 1 ? 2 : 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold-rewind speed");

    ImGui::End();
    return res;
}

}  // namespace pom2
