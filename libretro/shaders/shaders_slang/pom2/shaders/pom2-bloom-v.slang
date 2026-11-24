#version 450

// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pass 2: bloom, vertical Gaussian over the bright-passed horizontal leg.

#include "pom2-bloom.inc"

void main()
{
    if (params.POM2_BLOOM <= 0.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    FragColor = vec4(blur(vTexCoord, vec2(0.0, bloomStepY()), -1.0), 1.0);
}
