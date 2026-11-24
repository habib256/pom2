// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace pom2::libretro {

// The keyboard read as a device, for netplay. RetroArch's netplay sends and
// replays a port's RETRO_DEVICE_KEYBOARD state, sampled with input_state
// every frame (network/netplay/netplay_frontend.c @ v1.22.2); the keyboard
// CALLBACK's events never reach the other machine, nor a rollback. So a port
// set to the keyboard device reads these keys that way instead.

/// The RETROK codes read each frame: everything an Apple II keyboard has.
const unsigned* polledKeys(std::size_t& count);

/// The character a US keyboard types with `keycode`, shifted or not; 0 for
/// a key that types none. Netplay carries key positions, not characters, so
/// both machines read one layout.
std::uint32_t usCharacter(unsigned keycode, bool shift);

} // namespace pom2::libretro
