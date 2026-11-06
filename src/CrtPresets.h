// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CrtPresets — the CRT glass settings PER MONITOR: a built-in preset for each
// one, and on top of it only the fields the user changed.
//
// There was one set of `ntsc_*` values for every display mode. A green
// monochrome tube and a colour composite set do not share a look, so tuning
// one detuned the other, and the "look presets" the panel once had were
// removed because one click overwrote the whole block (DEV § CRT Settings
// panel UX). The fix for both is the same and it is LAYERS:
//
//     value = the user's override for (this monitor, this field), if any
//           ; else the monitor's preset.
//
// Overrides are sparse: touching the scanlines of the amber monitor stores
// one number, `crt_amber_scanlines`, and changes nothing else anywhere. A
// field can be put back to its preset on its own. The preset can therefore
// improve in a later version without trampling anything the user set.
//
// [D] Casso `Config/CrtResolver.h` + `specs/029-crt-user-overrides` @
//     2d899adf — preset, then sparse per-field overrides stored per monitor.
//     The idea is theirs; the fields, the keys and the values are POM2's.
// [F] The preset VALUES. They are POM2's long-standing defaults for every
//     monitor, plus a glow on the three monochrome ones (lit text on a
//     monochrome tube is soft-edged; on a shadow-mask colour tube much less
//     so). No photograph and no measurement stands behind the numbers.
//     Settled by: photographs of each monitor at a known exposure, or a
//     photometer.
//
// Pure policy, no GL and no settings file: same shape and same reason as
// `AudioBufferPolicy.h` and `KeyChord.h`. The window reads and writes the
// keys this file names.

#ifndef POM2_CRT_PRESETS_H
#define POM2_CRT_PRESETS_H

#include "NtscPostProcessor.h"   // pom2::NtscParams

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pom2 {
namespace crt {

/// One per display mode the user can pick (Apple2Display::HiResMode), named
/// here rather than taken from that enum so this header stays free of the
/// renderer. The window maps one onto the other.
enum class Monitor : std::uint8_t {
    ColorNtsc, ColorMedium, Color4Bit, ChatMauve, CompositeOE, CompositeOECpu,
    MonoWhite, MonoGreen, MonoAmber, AppleWin, Count
};
constexpr std::size_t kMonitors = static_cast<std::size_t>(Monitor::Count);

/// The glass knobs that are layered. `palMode` and `textSharp` are not here:
/// they describe the machine and the demodulator, not a monitor's look, and
/// stay global.
enum class Field : std::uint8_t {
    Brightness, Contrast, Saturation, Hue, Sharpness, Persistence, PhosphorGamma,
    Scanlines, Barrel, CenterLighting, ShadowMask, ShadowMaskStrength,
    LuminanceGain, RgbBandwidth, Bloom, BloomRadius, BloomThreshold, Count
};
constexpr std::size_t kFields = static_cast<std::size_t>(Field::Count);

struct FieldInfo {
    const char* key;      ///< settings suffix: crt_<monitor>_<key>
    const char* legacy;   ///< the old global `ntsc_*` key, or nullptr
    float       lo, hi;   ///< the slider's range; values are clamped to it
};

inline const FieldInfo& info(Field f)
{
    // The ranges are the panel's. A value from a hand-edited file outside
    // them is not harmless: `ntsc_center_lighting=0` is 1/0 in the shader.
    static const FieldInfo k[kFields] = {
        { "brightness",      "ntsc_brightness",       -0.5f, 0.5f  },
        { "contrast",        "ntsc_contrast",          0.5f, 1.5f  },
        { "saturation",      "ntsc_saturation",        0.0f, 2.0f  },
        { "hue",             "ntsc_hue",              -0.5f, 0.5f  },
        { "sharpness",       "ntsc_sharpness",         0.0f, 1.0f  },
        { "persistence",     "ntsc_persistence",       0.0f, 0.95f },
        { "phosphor_gamma",  "ntsc_phosphor_gamma",    0.6f, 2.6f  },
        { "scanlines",       "ntsc_scanlines",         0.0f, 1.0f  },
        { "barrel",          "ntsc_barrel",            0.0f, 0.30f },
        { "center_lighting", "ntsc_center_lighting",   0.5f, 1.0f  },
        { "shadow_mask",     "ntsc_shadow_mask",       0.0f, 3.0f  },
        { "shadow_strength", "ntsc_shadow_strength",   0.0f, 1.0f  },
        { "luminance_gain",  "ntsc_luminance_gain",    1.0f, 2.0f  },
        { "rgb_bandwidth_mhz", "ntsc_rgb_bandwidth_mhz", 0.0f, 8.0f },
        { "bloom",           nullptr,                  0.0f, 1.0f  },
        { "bloom_radius",    nullptr,                  0.5f, 6.0f  },
        { "bloom_threshold", nullptr,                  0.0f, 1.0f  },
    };
    return k[static_cast<std::size_t>(f)];
}

inline const char* monitorKey(Monitor m)
{
    static const char* const k[kMonitors] = {
        "ntsc", "medium", "4bit", "chatmauve", "oe", "oecpu",
        "white", "green", "amber", "applewin",
    };
    return k[static_cast<std::size_t>(m)];
}

inline const char* monitorName(Monitor m)
{
    static const char* const k[kMonitors] = {
        "Color NTSC", "Color (medium)", "Color (4-bit)", "Le Chat Mauve RGB",
        "Composite (OpenEmulator)", "Composite (OpenEmulator, CPU)",
        "Monochrome white", "Monochrome green", "Monochrome amber",
        "AppleWin NTSC",
    };
    return k[static_cast<std::size_t>(m)];
}

inline bool isMonochrome(Monitor m)
{
    return m == Monitor::MonoWhite || m == Monitor::MonoGreen || m == Monitor::MonoAmber;
}

/// `crt_<monitor>_<field>` — where one override lives in the settings file.
inline std::string settingKey(Monitor m, Field f)
{
    return std::string("crt_") + monitorKey(m) + "_" + info(f).key;
}

inline float clampField(Field f, float v)
{
    const FieldInfo& i = info(f);
    if (!(v == v)) return i.lo;                       // NaN from a bad file
    return std::clamp(v, i.lo, i.hi);
}

inline float get(const NtscParams& p, Field f)
{
    switch (f) {
        case Field::Brightness:         return p.brightness;
        case Field::Contrast:           return p.contrast;
        case Field::Saturation:         return p.saturation;
        case Field::Hue:                return p.hue;
        case Field::Sharpness:          return p.sharpness;
        case Field::Persistence:        return p.persistence;
        case Field::PhosphorGamma:      return p.phosphorGamma;
        case Field::Scanlines:          return p.scanlines;
        case Field::Barrel:             return p.barrel;
        case Field::CenterLighting:     return p.centerLighting;
        case Field::ShadowMask:         return static_cast<float>(static_cast<int>(p.shadowMask));
        case Field::ShadowMaskStrength: return p.shadowMaskStrength;
        case Field::LuminanceGain:      return p.luminanceGain;
        case Field::RgbBandwidth:       return p.rgbBandwidthMHz;
        case Field::Bloom:              return p.bloom;
        case Field::BloomRadius:        return p.bloomRadius;
        case Field::BloomThreshold:     return p.bloomThreshold;
        case Field::Count:              break;
    }
    return 0.0f;
}

inline void set(NtscParams& p, Field f, float v)
{
    v = clampField(f, v);
    switch (f) {
        case Field::Brightness:         p.brightness = v; break;
        case Field::Contrast:           p.contrast = v; break;
        case Field::Saturation:         p.saturation = v; break;
        case Field::Hue:                p.hue = v; break;
        case Field::Sharpness:          p.sharpness = v; break;
        case Field::Persistence:        p.persistence = v; break;
        case Field::PhosphorGamma:      p.phosphorGamma = v; break;
        case Field::Scanlines:          p.scanlines = v; break;
        case Field::Barrel:             p.barrel = v; break;
        case Field::CenterLighting:     p.centerLighting = v; break;
        case Field::ShadowMask:
            p.shadowMask = static_cast<NtscParams::ShadowMask>(static_cast<int>(v + 0.5f));
            break;
        case Field::ShadowMaskStrength: p.shadowMaskStrength = v; break;
        case Field::LuminanceGain:      p.luminanceGain = v; break;
        case Field::RgbBandwidth:       p.rgbBandwidthMHz = v; break;
        case Field::Bloom:              p.bloom = v; break;
        case Field::BloomRadius:        p.bloomRadius = v; break;
        case Field::BloomThreshold:     p.bloomThreshold = v; break;
        case Field::Count:              break;
    }
}

/// The built-in look of one monitor. See the [F] note at the top.
inline NtscParams preset(Monitor m)
{
    NtscParams p;                       // POM2's defaults, for every monitor
    switch (m) {
        case Monitor::MonoGreen:
        case Monitor::MonoAmber: p.bloom = 0.30f; break;
        case Monitor::MonoWhite: p.bloom = 0.20f; break;
        default: break;                 // colour tubes: no glow by default
    }
    return p;
}

/// The user's adjustments: for each monitor, the fields they changed.
class Overrides {
public:
    bool  has(Monitor m, Field f) const { return set_[idx(m)][idx(f)]; }
    float value(Monitor m, Field f) const { return value_[idx(m)][idx(f)]; }
    void  set(Monitor m, Field f, float v)
    {
        set_[idx(m)][idx(f)]   = true;
        value_[idx(m)][idx(f)] = clampField(f, v);
    }
    void clear(Monitor m, Field f) { set_[idx(m)][idx(f)] = false; }
    void clear(Monitor m) { set_[idx(m)].fill(false); }
    int  count(Monitor m) const
    {
        int n = 0;
        for (bool b : set_[idx(m)]) n += b ? 1 : 0;
        return n;
    }

    /// What the monitor shows: its preset, with each overridden field laid
    /// over it. `global` supplies the two machine-level flags.
    NtscParams resolve(Monitor m, const NtscParams& global) const
    {
        NtscParams p = preset(m);
        p.palMode   = global.palMode;
        p.textSharp = global.textSharp;
        for (std::size_t f = 0; f < kFields; ++f)
            if (set_[idx(m)][f])
                crt::set(p, static_cast<Field>(f), value_[idx(m)][f]);
        return p;
    }

private:
    static std::size_t idx(Monitor m) { return static_cast<std::size_t>(m); }
    static std::size_t idx(Field f)   { return static_cast<std::size_t>(f); }
    std::array<std::array<bool,  kFields>, kMonitors> set_{};
    std::array<std::array<float, kFields>, kMonitors> value_{};
};

/// Which field a slider is bound to, from the address of the member it
/// edits — so the panel's rows need no second table.
inline Field fieldAt(const NtscParams& p, const float* member)
{
    if (member == &p.brightness)         return Field::Brightness;
    if (member == &p.contrast)           return Field::Contrast;
    if (member == &p.saturation)         return Field::Saturation;
    if (member == &p.hue)                return Field::Hue;
    if (member == &p.sharpness)          return Field::Sharpness;
    if (member == &p.persistence)        return Field::Persistence;
    if (member == &p.phosphorGamma)      return Field::PhosphorGamma;
    if (member == &p.scanlines)          return Field::Scanlines;
    if (member == &p.barrel)             return Field::Barrel;
    if (member == &p.centerLighting)     return Field::CenterLighting;
    if (member == &p.shadowMaskStrength) return Field::ShadowMaskStrength;
    if (member == &p.luminanceGain)      return Field::LuminanceGain;
    if (member == &p.rgbBandwidthMHz)    return Field::RgbBandwidth;
    if (member == &p.bloom)              return Field::Bloom;
    if (member == &p.bloomRadius)        return Field::BloomRadius;
    if (member == &p.bloomThreshold)     return Field::BloomThreshold;
    return Field::Count;
}

}  // namespace crt
}  // namespace pom2

#endif  // POM2_CRT_PRESETS_H
