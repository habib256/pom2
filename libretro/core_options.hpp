// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <libretro.h>

#include <string_view>

namespace pom2::libretro {

// The core options, in one bilingual table (core_options.cpp): categories and
// translations for frontends with core options v2, the "Label; a|b|c" strings
// for older ones. The first value of every option is its default.
void registerCoreOptions(retro_environment_t environment);

// The frontend's value for `key`, or the option's default when the frontend
// has none (or knows no such key).
std::string_view coreOption(retro_environment_t environment, const char* key);

// Hide the options that cannot matter in the current configuration (the
// mouse's tuning with no mouse, the Mockingboard's with none). Returns true
// when a visibility changed.
bool updateOptionVisibility(retro_environment_t environment);

} // namespace pom2::libretro
