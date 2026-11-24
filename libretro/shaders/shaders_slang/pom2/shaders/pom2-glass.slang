#version 450

// POM2 Apple II Emulator — RetroArch slang port of the desktop glass pass.
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pass 3: the tube. A port of CrtEffectStack's glass fragment shader
// (src/CrtEffectStack.cpp, kFragmentShader), the pass the desktop runs over
// every display mode: barrel geometry, sharpness, hue, brightness / contrast /
// saturation, phosphor gamma, scanlines, shadow mask, bloom, centre lighting,
// luminance gain and phosphor persistence, in that order. The reasons behind
// each step are written there; this file keeps only what the port changes.
//
// What the port changes:
// - It renders at the viewport size (scale_type viewport), as the desktop
//   renders at the on-screen size, so the scanline and mask patterns are
//   anti-aliased against real screen pixels.
// - Persistence reads this pass's own previous output (POM2GlassFeedback),
//   once per frame RetroArch presents. The desktop raises the retention to
//   the emulated frames a host present spans (crtRetention); RetroArch
//   presents one frame per emulated frame, so the knob is used as is.

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
    vec4 OutputSize;
    vec4 POM2CableSize;
    uint FrameCount;
    float POM2_BRIGHTNESS;
    float POM2_CONTRAST;
    float POM2_SATURATION;
    float POM2_HUE;
    float POM2_SHARPNESS;
    float POM2_PERSISTENCE;
    float POM2_PHOSPHOR_GAMMA;
    float POM2_SCANLINES;
    float POM2_BARREL;
    float POM2_CENTER_LIGHTING;
    float POM2_SHADOW_MASK;
    float POM2_SHADOW_STRENGTH;
    float POM2_LUMINANCE_GAIN;
    float POM2_BLOOM;
} params;

// Defaults and ranges: NtscParams (src/NtscPostProcessor.h) and the panel's
// ranges (src/CrtPresets.h, crt::info).
#pragma parameter POM2_BRIGHTNESS "POM2 brightness" 0.0 -0.5 0.5 0.01
#pragma parameter POM2_CONTRAST "POM2 contrast" 1.0 0.5 1.5 0.01
#pragma parameter POM2_SATURATION "POM2 saturation" 1.0 0.0 2.0 0.01
#pragma parameter POM2_HUE "POM2 hue" 0.0 -0.5 0.5 0.01
#pragma parameter POM2_SHARPNESS "POM2 sharpness (0.5 = neutral)" 0.5 0.0 1.0 0.01
#pragma parameter POM2_PERSISTENCE "POM2 phosphor persistence" 0.4 0.0 0.95 0.01
#pragma parameter POM2_PHOSPHOR_GAMMA "POM2 phosphor gamma" 1.0 0.6 2.6 0.05
#pragma parameter POM2_SCANLINES "POM2 scanlines" 0.25 0.0 1.0 0.01
#pragma parameter POM2_BARREL "POM2 barrel (curvature)" 0.02 0.0 0.3 0.005
#pragma parameter POM2_CENTER_LIGHTING "POM2 centre lighting (1 = flat)" 1.0 0.5 1.0 0.01
#pragma parameter POM2_SHADOW_MASK "POM2 mask: off, triad, aperture grille, dot" 0.0 0.0 3.0 1.0
#pragma parameter POM2_SHADOW_STRENGTH "POM2 mask strength" 0.5 0.0 1.0 0.01
#pragma parameter POM2_LUMINANCE_GAIN "POM2 luminance gain" 1.0 1.0 2.0 0.01
#pragma parameter POM2_BLOOM "POM2 bloom (halation)" 0.0 0.0 1.0 0.01

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
layout(set = 0, binding = 2) uniform sampler2D Source;             // bloom, vertical leg
layout(set = 0, binding = 3) uniform sampler2D POM2Cable;          // the picture after the cable
layout(set = 0, binding = 4) uniform sampler2D POM2GlassFeedback;  // this pass, previous frame

#define uSrcSize (params.POM2CableSize.xy)
#define uOutSize (params.OutputSize.xy)

// Catmull-Rom cubic weight (4-tap per axis).
float cubicWeight(float x)
{
    x = abs(x);
    if (x < 1.0) return x * x * (1.5 * x - 2.5) + 1.0;
    if (x < 2.0) return x * (x * (-0.5 * x + 2.5) - 4.0) + 2.0;
    return 0.0;
}

vec3 sampleSrc(vec2 uv)
{
    uv = clamp(uv, 0.0, 1.0);
    vec2 mag2 = uOutSize / uSrcSize;

    // Horizontal minification: box-average the covered source columns
    // (a 560-wide frame in a viewport narrower than 560 pixels).
    if (mag2.x < 0.995) {
        const int kBox = 8;
        float foot = (1.0 / mag2.x) / uSrcSize.x;
        vec3 acc = vec3(0.0);
        for (int i = 0; i < kBox; ++i) {
            float o = ((float(i) + 0.5) / float(kBox) - 0.5) * foot;
            acc += texture(POM2Cable, vec2(clamp(uv.x + o, 0.0, 1.0), uv.y)).rgb;
        }
        return acc / float(kBox);
    }

    float mag = max(mag2.x, mag2.y);
    if (mag <= 1.25)
        return texture(POM2Cable, uv).rgb;

    vec2 coord = uv * uSrcSize - 0.5;
    vec2 f = fract(coord);
    coord = floor(coord);
    vec3 col = vec3(0.0);
    float wsum = 0.0;
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            vec2 offs = vec2(float(i), float(j));
            vec2 samp = (coord + offs + 0.5) / uSrcSize;
            float w = cubicWeight(offs.x - f.x) * cubicWeight(offs.y - f.y);
            col += texture(POM2Cable, clamp(samp, 0.0, 1.0)).rgb * w;
            wsum += w;
        }
    }
    return col / max(wsum, 1e-4);
}

void main()
{
    // Barrel distortion, with a soft analytic-AA border.
    vec2 cuv = vTexCoord * 2.0 - 1.0;
    float r2 = dot(cuv, cuv);
    vec2 buv = cuv * (1.0 + params.POM2_BARREL * r2);
    vec2 uv  = buv * 0.5 + 0.5;
    vec2  edge     = min(uv, 1.0 - uv);
    vec2  edgeFw   = max(fwidth(uv), vec2(1e-4));
    float edgeMask = clamp(min(edge.x / edgeFw.x, edge.y / edgeFw.y), 0.0, 1.0);
    vec3 rgb = sampleSrc(uv);

    // Sharpness: 0.5 = passthrough, above sharpens, below softens.
    {
        float amt = (params.POM2_SHARPNESS - 0.5) * 2.0;
        if (amt != 0.0) {
            vec2 t = 1.0 / uSrcSize;
            vec3 blurred = (
                sampleSrc(uv + vec2(-t.x, 0.0)) +
                sampleSrc(uv + vec2( t.x, 0.0)) +
                sampleSrc(uv + vec2(0.0, -t.y)) +
                sampleSrc(uv + vec2(0.0,  t.y))) * 0.25;
            rgb = clamp(rgb + amt * (rgb - blurred), 0.0, 1.0);
        }
    }

    // Hue: chroma rotation through BT.601 YUV, OpenEmulator's decoder matrix.
    if (params.POM2_HUE != 0.0) {
        float Y = dot(rgb, vec3( 0.299,    0.587,    0.114));
        float U = dot(rgb, vec3(-0.14713, -0.28886,  0.436));
        float V = dot(rgb, vec3( 0.615,   -0.51499, -0.10001));
        float a  = params.POM2_HUE * 3.14159265;
        float cs = cos(a), sn = sin(a);
        float Ur = U * cs - V * sn;
        float Vr = U * sn + V * cs;
        rgb = vec3(Y                 + 1.139883 * Vr,
                   Y - 0.394642 * Ur - 0.580622 * Vr,
                   Y + 2.032062 * Ur);
    }

    // Brightness / contrast / saturation.
    rgb = (rgb - 0.5) * params.POM2_CONTRAST + 0.5 + params.POM2_BRIGHTNESS;
    float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
    rgb = mix(vec3(luma), rgb, clamp(params.POM2_SATURATION, 0.0, 4.0));
    rgb = clamp(rgb, 0.0, 1.0);

    // Phosphor response curve.
    if (params.POM2_PHOSPHOR_GAMMA != 1.0)
        rgb = pow(max(rgb, vec3(0.0)), vec3(params.POM2_PHOSPHOR_GAMMA));

    // Scanlines: sin^2 of the beam, 1 at a row's centre and 0 at the seams
    // (OpenEmulator's glass shader), faded out where they would alias.
    float outRow = uv.y * (uSrcSize.y * 2.0);
    float rowFw  = max(fwidth(outRow), 1e-4);
    float scanAA = clamp(1.0 - (rowFw - 0.5) / 0.5, 0.0, 1.0);
    float beam   = 0.5 - 0.5 * cos(3.14159265 * outRow);
    rgb *= 1.0 - params.POM2_SCANLINES * (1.0 - beam) * scanAA;

    // Shadow mask, pinned to the 560-dot line: a property of the glass.
    int maskType = int(params.POM2_SHADOW_MASK + 0.5);
    if (maskType != 0 && params.POM2_SHADOW_STRENGTH > 0.0) {
        const float kMaskUnitsX = 1120.0;
        float oxBase = uv.x * kMaskUnitsX;
        float maskFw = max(fwidth(oxBase), 1e-4);
        float maskAA = clamp(1.0 - (maskFw - 1.0) / 2.0, 0.0, 1.0);
        float ox = oxBase;
        if (maskType == 3)
            ox += (mod(floor(outRow * 0.5), 2.0) < 1.0) ? 0.0 : 1.5;
        float strength = params.POM2_SHADOW_STRENGTH * maskAA;
        int phase = int(mod(floor(ox), 3.0));
        // Lottes dark/light triplet: keeps the triad's average luminance.
        const float maskDark = 0.5, maskLight = 1.5;
        vec3 mask = vec3(maskDark);
        if      (phase == 0) mask.r = maskLight;
        else if (phase == 1) mask.g = maskLight;
        else                 mask.b = maskLight;
        vec3 atten = mix(vec3(1.0), mask, strength);
        if (maskType == 1 || maskType == 3) {
            float vrow = mod(floor(outRow), 3.0);
            if (vrow < 1.0) atten *= mix(1.0, 0.7, strength);
        }
        rgb *= atten;
    }

    // Bloom: light scattered in the faceplate, after scanlines and mask.
    if (params.POM2_BLOOM > 0.0)
        rgb += texture(Source, clamp(uv, 0.0, 1.0)).rgb * params.POM2_BLOOM;

    // Centre lighting (OpenEmulator order: after the mask).
    {
        vec2 lighting = cuv * (1.0 / params.POM2_CENTER_LIGHTING - 1.0);
        rgb *= exp(-dot(lighting, lighting));
    }

    rgb *= params.POM2_LUMINANCE_GAIN;
    rgb *= edgeMask;

    // Persistence, last, on the final colour. No history on the first frame:
    // max(rgb, -0.5/256) == rgb.
    float retention = params.FrameCount == 0u ? 0.0 : clamp(params.POM2_PERSISTENCE, 0.0, 0.98);
    vec3 prev = texture(POM2GlassFeedback, vTexCoord).rgb;
    rgb = max(rgb, prev * retention - 0.5 / 256.0);

    FragColor = vec4(rgb, 1.0);
}
