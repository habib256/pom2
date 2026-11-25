// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
// Every value every libretro core option offers goes through the parser the
// core reads it with. A parser that took "12.5%" for nothing set the analog
// dead zone to the whole stick and every volume to 100 % (bug hunt
// 2026-11-25): no test fed the menu's own values through it.
#include "core_options.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
int failures = 0;
int percents = 0;
void check(bool ok, const std::string& what)
{
    if (!ok) { std::printf("  [FAIL] %s\n", what.c_str()); ++failures; }
}

void visit(const char* key, const char* value, void*)
{
    const std::string text(value);
    if (!text.empty() && text.back() == '%') {
        ++percents;
        const float expected = static_cast<float>(std::strtod(value, nullptr) / 100.0);
        const float got = pom2::libretro::percentValue(text, -1.0f);
        check(got == expected, std::string(key) + " = " + text + " read as " + std::to_string(got));
    }
}
} // namespace

int main()
{
    using pom2::libretro::percentValue;
    pom2::libretro::forEachOptionValue(visit, nullptr);
    check(percents > 30, "the percentage options were found (" + std::to_string(percents) + " values)");
    check(percentValue("12.5%", -1.0f) == 0.125f, "12.5% is an eighth");
    check(percentValue("0%", -1.0f) == 0.0f, "0% mutes");
    check(percentValue("150", -1.0f) == 1.5f, "a bare number from an older configuration");
    check(percentValue("", 0.5f) == 0.5f && percentValue("loud", 0.5f) == 0.5f &&
          percentValue("50%%", 0.5f) == 0.5f && percentValue("nan%", 0.5f) == 0.5f &&
          percentValue("inf", 0.5f) == 0.5f, "a malformed value keeps the default");
    std::printf("libretro core options: %d percentage values, %d failure(s)\n", percents, failures);
    return failures ? 1 : 0;
}
