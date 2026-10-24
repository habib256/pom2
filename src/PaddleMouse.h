// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// PaddleMouse — the host mouse as Apple II paddles 0 and 1 (INPUT-003).
//
// An option for a machine with no game controller: the pointer's position
// over the Apple II screen becomes PDL(0) (across) and PDL(1) (down), and
// the left/right buttons PB0/PB1. Absolute, not relative — a paddle is a
// knob with two end stops, and the screen's edges are those stops: the
// value is clamped there, so the pointer leaving the screen holds the knob
// at its end instead of letting it drift.
//
// `range` is the sensitivity: the fraction of the screen, centred, that
// sweeps the full 0..255. 1.0 = edge to edge; 0.5 = the middle half (twice
// as fast), the rest pinned at the stops. Pure arithmetic, so a test drives
// it without a window.

#ifndef POM2_PADDLE_MOUSE_H
#define POM2_PADDLE_MOUSE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pom2::paddlemouse {

inline constexpr double kMinRange = 0.2;   // 5× — past this the knob is a switch

/// `pos` between `lo` and `hi` (screen pixels) → paddle 0..255.
inline uint8_t axisToPaddle(double pos, double lo, double hi, double range)
{
    if (!(hi > lo)) return 128;
    range = std::clamp(range, kMinRange, 1.0);
    const double t = (pos - lo) / (hi - lo);                // 0..1 across the screen
    const double u = std::clamp((t - 0.5) / range + 0.5, 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(u * 255.0));
}

}  // namespace pom2::paddlemouse

#endif  // POM2_PADDLE_MOUSE_H
