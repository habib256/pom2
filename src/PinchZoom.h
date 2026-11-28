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

#pragma once

// Two-finger pinch zoom — the POLICY (owner request 2026-11-28: "zoom on a
// touchpad or on a tablet with two fingers").
//
// Every host says "pinch" in its own words: macOS an NSEventTypeMagnify that
// GLFW never forwards, a Windows precision touchpad a Ctrl+wheel, a Windows
// touch screen a WM_GESTURE GID_ZOOM, a browser a `wheel` with ctrlKey (Chrome,
// Firefox, Edge), a `gesturechange` (Safari) or two touches. The platform
// sources (PinchZoomHost*.{cpp,mm}, wasm/pinch_zoom.js) turn each into ONE
// portable event — a natural-log scale delta and, when the source knows it,
// the focal point in window coordinates — and push it here. This header
// decides the rest, and it is GLFW-free and ImGui-free so a headless test
// pins it (`pinch_zoom_policy`), the same shape as MouseGrab.h / KeyChord.h:
//
//   • accumulation: every event of a frame sums into one gesture, the focal
//     point is the last one reported, and a gesture no view took is dropped
//     at the next frame — it never leaks into a later frame or panel;
//   • the sign: > 0 zooms IN. The platform convention (spread = in) is the
//     default, because the OS already gives that sign; the setting
//     `pinch_zoom_inverted` flips it for whoever wants the opposite;
//   • clamping: one event, and one frame, move at most ×4 either way, and a
//     NaN / infinite source value is refused;
//   • Ctrl+wheel → the same delta: one notch = ×1.25, the step the memory
//     heatmap's own wheel already uses;
//   • the target: the gesture goes to the view drawn in the ImGui window
//     under the focal point (or one of its ancestors) whose zoomable rect
//     contains that point, and to nobody else — the caller resolves the
//     window under the point (ImGui's own hit-test, z-order aware) and passes
//     its id chain; a rect alone would hand the gesture to a panel hidden
//     behind a floating window.
//
// Thread: main (UI) thread only. GLFW callbacks, AppKit event monitors, the
// Win32 window procedure and Emscripten's DOM callbacks all run there, so no
// lock is taken.

#include <cmath>
#include <cstdint>
#include <vector>

namespace pom2 {
namespace pinch {

struct Point { float x = 0.0f, y = 0.0f; };

struct Rect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool contains(Point p) const { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
};

/// ln(1.25): one Ctrl+wheel notch. The memory heatmap zooms by 1.25 a notch;
/// a pinch that a host reports as a wheel must feel the same.
inline constexpr float kWheelNotchLog = 0.22314355f;
/// ln(4): the most one event, and one frame, may zoom either way. A stalled
/// frame (the UI thread blocked behind a disk mount) may deliver a burst of
/// gesture events at once; without a bound the view would leap.
inline constexpr float kMaxLog = 1.3862944f;

inline float clampLog(float v)
{
    if (!std::isfinite(v)) return 0.0f;
    return v < -kMaxLog ? -kMaxLog : (v > kMaxLog ? kMaxLog : v);
}

/// Ctrl+wheel notches (ImGui's units: +1 = one notch away from the user,
/// which every browser and OS reads as "zoom in") → log scale.
inline float wheelToLog(float notches) { return clampLog(notches * kWheelNotchLog); }

/// A browser `wheel` with ctrlKey. Chrome, Edge and Firefox report a
/// trackpad pinch that way, in pixels (deltaMode 0), and scale their own page
/// by exp(-deltaY / 100) for it; that mapping is kept. A mouse's Ctrl+wheel
/// arrives the same way at ±100 px a notch on several hosts, so one event is
/// capped at ±25 px — one notch reads as about one ×1.28 step, while a pinch
/// (a few pixels an event) is untouched. Lines (1) and pages (2) are notches.
inline float domCtrlWheelToLog(double deltaY, int deltaMode)
{
    if (!std::isfinite(deltaY)) return 0.0f;
    if (deltaMode == 0) {
        const double px = deltaY < -25.0 ? -25.0 : (deltaY > 25.0 ? 25.0 : deltaY);
        return clampLog(static_cast<float>(-px / 100.0));
    }
    const double notches = deltaMode == 1 ? -deltaY / 3.0 : -deltaY;
    return wheelToLog(static_cast<float>(notches));
}

/// macOS `-[NSEvent magnification]`: the scale changes by (1 + m).
inline float magnificationToLog(double m)
{
    if (!std::isfinite(m)) return 0.0f;
    if (m <= -0.95) return -kMaxLog;
    return clampLog(static_cast<float>(std::log1p(m)));
}

/// A distance (or scale) ratio, new / old — WM_GESTURE GID_ZOOM, Safari's
/// gesturechange scale, two touches in the browser.
inline float ratioToLog(double ratio)
{
    if (!std::isfinite(ratio) || ratio <= 0.0) return 0.0f;
    return clampLog(static_cast<float>(std::log(ratio)));
}

/// Is this wheel a zoom? ImGui's Ctrl held — and only that: ImGui never
/// scrolls a window while its Ctrl is down, so the wheel can be a zoom
/// without ALSO scrolling. On macOS ImGui swaps the two
/// (ConfigMacOSXBehaviors), so its Ctrl is the Command key there: Cmd+wheel
/// zooms, while the physical Control key (ImGui's Super, with which ImGui
/// does scroll, and which macOS keeps for its own screen zoom) does not.
inline bool wheelIsZoom(bool ctrlHeld, float wheelY) { return ctrlHeld && wheelY != 0.0f; }

/// One frame's gesture as a view sees it.
struct Gesture {
    float logScale = 0.0f;   ///< ln(factor); > 0 zooms in (inversion applied)
    Point focal{};           ///< window coordinates (ImGui screen space)
    bool  fit = false;       ///< macOS smart magnify (two-finger double tap)
    float factor() const { return std::exp(logScale); }
    bool  active() const { return logScale != 0.0f || fit; }
};

/// Turns a continuous pinch into rungs of a discrete zoom ladder. A rung
/// fires each time the accumulated scale crosses `logPerStep`; the remainder
/// carries to the next frame, and a reversal starts from zero so a
/// leftover from the other direction never cancels the first rung.
struct LadderAccumulator {
    float acc = 0.0f;
    int feed(float logScale, float logPerStep = kWheelNotchLog)
    {
        if (logScale == 0.0f || logPerStep <= 0.0f) return 0;
        if ((logScale > 0.0f) != (acc > 0.0f) && acc != 0.0f) acc = 0.0f;
        acc += logScale;
        const int steps = static_cast<int>(acc / logPerStep);   // toward zero
        acc -= static_cast<float>(steps) * logPerStep;
        return steps;
    }
};

class Router {
public:
    // ── settings ──────────────────────────────────────────────────────────
    void setInverted(bool on) { inverted_ = on; }
    bool inverted() const { return inverted_; }

    // ── sources (main thread) ─────────────────────────────────────────────
    /// A zoom step at the pointer (Ctrl+wheel, macOS magnify).
    void push(float logScale) { pushImpl(logScale, false, Point{}); }
    /// A zoom step about a point the source knows (two touches).
    void pushAt(float logScale, Point focal) { pushImpl(logScale, true, focal); }
    /// "Back to fit" (macOS smart magnify), at the pointer.
    void pushFit() { queuedFit_ = true; }
    /// A touch pinch has begun over a mouse press the FIRST finger already
    /// made (the browser's GLFW turns one touch into the left button). The
    /// glue then releases that button away from every widget, so lifting the
    /// fingers clicks nothing: a pinch is never also a click, a brush dot
    /// that grows into a stroke, or a selection.
    void cancelPress() { pressCancel_ = true; }
    /// Taken once by the glue; not tied to a frame's gesture, so a cancel
    /// is never lost to beginFrame dropping an untaken pinch.
    bool takePressCancel() { const bool v = pressCancel_; pressCancel_ = false; return v; }

    // ── frame ─────────────────────────────────────────────────────────────
    /// Opens a frame: what was queued since the last call becomes this
    /// frame's gesture, and the previous one is dropped whether or not a
    /// view took it. `pointer` stands in for the focal point of events that
    /// carried none.
    void beginFrame(Point pointer)
    {
        frame_ = Gesture{};
        frame_.logScale = clampLog(inverted_ ? -queuedLog_ : queuedLog_);
        frame_.fit      = queuedFit_;
        frame_.focal    = queuedHasFocal_ ? queuedFocal_ : pointer;
        taken_ = false;
        hitChain_.clear();
        queuedLog_ = 0.0f;
        queuedFit_ = false;
        queuedHasFocal_ = false;
    }

    bool pending() const { return frame_.active() && !taken_; }
    Point focal() const { return frame_.focal; }

    /// The window under the focal point, then its parents up to the root
    /// (child → … → root). Set once per frame by the ImGui glue.
    void setHitChain(std::vector<std::uint64_t> chain) { hitChain_ = std::move(chain); }

    /// A zoomable view asks for this frame's gesture. `window` is the id of
    /// the window it draws in, `rect` its zoomable area. Answers at most once
    /// a frame, to the view under the focal point.
    bool take(std::uint64_t window, Rect rect, Gesture& out)
    {
        if (!pending() || !rect.contains(frame_.focal)) return false;
        bool inChain = false;
        for (std::uint64_t id : hitChain_) if (id == window) { inChain = true; break; }
        if (!inChain) return false;
        taken_ = true;
        out = frame_;
        return true;
    }

private:
    void pushImpl(float logScale, bool hasFocal, Point focal)
    {
        const float v = clampLog(logScale);
        if (v == 0.0f) return;
        queuedLog_ = clampLog(queuedLog_ + v);
        if (hasFocal) { queuedFocal_ = focal; queuedHasFocal_ = true; }
    }

    bool  inverted_ = false;
    bool  pressCancel_ = false;
    float queuedLog_ = 0.0f;
    bool  queuedFit_ = false;
    bool  queuedHasFocal_ = false;
    Point queuedFocal_{};
    Gesture frame_{};
    bool  taken_ = false;
    std::vector<std::uint64_t> hitChain_;
};

/// The process's router (main thread). Function-local static: no static
/// initialisation order between the platform hooks and the UI.
inline Router& router()
{
    static Router r;
    return r;
}

} // namespace pinch
} // namespace pom2
