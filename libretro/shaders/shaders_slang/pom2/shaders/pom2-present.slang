#version 450

// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Last pass: copies the glass pass to the screen 1:1. It exists because the
// glass pass reads its own previous output for the phosphor persistence, and
// RetroArch keeps a feedback texture only for a pass that is not the last.

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);
}
