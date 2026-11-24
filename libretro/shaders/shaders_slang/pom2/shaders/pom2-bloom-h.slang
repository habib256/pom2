#version 450

// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pass 1: bloom, bright-pass + horizontal Gaussian, on the source grid.

#include "pom2-bloom.inc"

void main()
{
    if (params.POM2_BLOOM <= 0.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    FragColor = vec4(blur(vTexCoord, vec2(bloomStepX(), 0.0), params.POM2_BLOOM_THRESHOLD), 1.0);
}
