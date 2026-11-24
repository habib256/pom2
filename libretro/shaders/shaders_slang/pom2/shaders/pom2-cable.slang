#version 450

// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pass 0: the analog RGB cable (NtscParams::rgbBandwidthMHz). The same
// windowed-sinc low-pass as CrtEffectStack's bandwidth pre-pass
// (src/CrtEffectStack.cpp, kBandwidthFragmentShader): horizontal only, per
// channel, on the SOURCE sample grid. A 560-wide frame is sampled at
// 14.318 MHz and a 280-wide one at 7.16 MHz, so one MHz figure is a different
// fraction of Nyquist in each mode, exactly as one physical cable behaves.
// 0 MHz, or a cutoff at or above Nyquist, passes the frame through.

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 SourceSize;
    float POM2_RGB_BANDWIDTH;
} params;

#pragma parameter POM2_RGB_BANDWIDTH "POM2 analog RGB bandwidth (MHz, 0 = off)" 0.0 0.0 8.0 0.1

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

const int   kTaps = 8;
const float kPi   = 3.14159265;
// The 560-dot line is sampled at the colour-reference dot clock.
const float kDotClockMHz560 = 14.31818;

void main()
{
    float fsMHz  = params.SourceSize.x * (kDotClockMHz560 / 560.0);
    float cutoff = params.POM2_RGB_BANDWIDTH / max(fsMHz, 1e-4);   // cycles/sample
    if (params.POM2_RGB_BANDWIDTH <= 0.0 || cutoff >= 0.5) {
        FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);
        return;
    }

    float texel = params.SourceSize.z;
    float fc2   = 2.0 * cutoff;

    // k = 0: sinc(0) = 1, hann(0) = 1.
    vec3  acc  = texture(Source, vTexCoord).rgb * fc2;
    float wsum = fc2;
    for (int k = 1; k <= kTaps; ++k) {
        float x = fc2 * float(k);
        float h = fc2 * (sin(kPi * x) / (kPi * x))
                      * (0.5 + 0.5 * cos(kPi * float(k) / float(kTaps + 1)));
        float dx = float(k) * texel;
        acc += (texture(Source, vec2(clamp(vTexCoord.x - dx, 0.0, 1.0), vTexCoord.y)).rgb +
                texture(Source, vec2(clamp(vTexCoord.x + dx, 0.0, 1.0), vTexCoord.y)).rgb) * h;
        wsum += 2.0 * h;
    }
    // Normalised by the weights' own sum: a flat field stays exactly flat.
    FragColor = vec4(acc / max(wsum, 1e-4), 1.0);
}
