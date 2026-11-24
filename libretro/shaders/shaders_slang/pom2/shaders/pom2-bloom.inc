// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bloom (halation) taps shared by pom2-bloom-h.slang and pom2-bloom-v.slang,
// the two legs of CrtEffectStack's kBloomFragmentShader (src/CrtEffectStack.cpp).
// The horizontal leg bright-passes its taps, so the vertical leg blurs an
// already bright-passed picture and no pass is spent on the threshold alone.
// [D] libretro glsl-shaders/bloom (hunterk, public domain); the knee and the
//     folding are Casso's (`bloom_h.hlsl` @ 2d899adf).
// The nine weights are row 12 of Pascal's triangle with its two outermost
// terms dropped: 924, 792, 495, 220, 66 over 4070.

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;
    vec4 FinalViewportSize;
    float POM2_BLOOM;
    float POM2_BLOOM_RADIUS;
    float POM2_BLOOM_THRESHOLD;
} params;

#pragma parameter POM2_BLOOM "POM2 bloom (halation)" 0.0 0.0 1.0 0.01
#pragma parameter POM2_BLOOM_RADIUS "POM2 bloom radius (dots of the 560-dot line)" 2.0 0.5 6.0 0.1
#pragma parameter POM2_BLOOM_THRESHOLD "POM2 bloom threshold" 0.5 0.0 1.0 0.01

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = params.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

vec3 tap(vec2 uv, float threshold)
{
    vec3 c = texture(Source, clamp(uv, 0.0, 1.0)).rgb;
    if (threshold < 0.0) return c;
    // Only bright pixels scatter. A hard cut would band as content crosses
    // the threshold, so the low end is a quadratic knee.
    float knee = max(threshold * 0.5, 0.0001);
    float lum  = max(c.r, max(c.g, c.b));
    float soft = clamp(lum - threshold + knee, 0.0, 2.0 * knee);
    float over = max(soft * soft / (4.0 * knee), lum - threshold);
    return c * (over / max(lum, 0.0001));
}

vec3 blur(vec2 uv, vec2 stp, float threshold)
{
    vec3 acc = tap(uv, threshold) * 0.227027;
    acc += (tap(uv + stp,       threshold) + tap(uv - stp,       threshold)) * 0.1945946;
    acc += (tap(uv + stp * 2.0, threshold) + tap(uv - stp * 2.0, threshold)) * 0.1216216;
    acc += (tap(uv + stp * 3.0, threshold) + tap(uv - stp * 3.0, threshold)) * 0.054054;
    acc += (tap(uv + stp * 4.0, threshold) + tap(uv - stp * 4.0, threshold)) * 0.016216;
    return acc;
}

// One tap = POM2_BLOOM_RADIUS dots of the 560-dot line whatever the source's
// own width, and the same distance on the SCREEN down the picture, so the
// halo is round at any window size and in every video mode.
float bloomStepX() { return params.POM2_BLOOM_RADIUS / 560.0; }
float bloomStepY()
{
    return bloomStepX() * params.FinalViewportSize.x / max(params.FinalViewportSize.y, 1.0);
}
