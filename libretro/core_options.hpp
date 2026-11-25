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

/// A percentage option's value ("12.5%", "100%", or a bare number from an
/// older configuration) as a fraction, or `fallback` when it is not one.
/// Never throws (RetroArch's web build cannot catch).
float percentValue(std::string_view text, float fallback);

/// Every value every option offers, for the test that feeds each through
/// the parsers: {key, value}.
void forEachOptionValue(void (*visit)(const char* key, const char* value, void* context), void* context);

} // namespace pom2::libretro
