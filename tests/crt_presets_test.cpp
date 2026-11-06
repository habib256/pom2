// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CrtPresets — the layering of the CRT glass settings per monitor: preset,
// then the fields the user changed. Pure policy, no GL.

#include "CrtPresets.h"

#include <cstdio>
#include <set>
#include <string>

using namespace pom2;
using crt::Field;
using crt::Monitor;

namespace {
int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)
}  // namespace

int main()
{
    const NtscParams defaults;

    // Every preset is POM2's defaults on a colour monitor: an upgrade does
    // not move a picture nobody tuned. Monochrome differs by its glow only.
    for (std::size_t m = 0; m < crt::kMonitors; ++m) {
        const auto mon = static_cast<Monitor>(m);
        const NtscParams p = crt::preset(mon);
        for (std::size_t f = 0; f < crt::kFields; ++f) {
            const auto field = static_cast<Field>(f);
            if (field == Field::Bloom && crt::isMonochrome(mon)) {
                CHECK(crt::get(p, field) > 0.0f);
                continue;
            }
            CHECK(crt::get(p, field) == crt::get(defaults, field));
        }
    }

    // get/set round-trip on every field, and a set is clamped to the range.
    for (std::size_t f = 0; f < crt::kFields; ++f) {
        const auto field = static_cast<Field>(f);
        const crt::FieldInfo& i = crt::info(field);
        NtscParams p;
        crt::set(p, field, i.hi);
        CHECK(crt::get(p, field) == i.hi);
        crt::set(p, field, i.hi + 100.0f);
        CHECK(crt::get(p, field) == i.hi);
        crt::set(p, field, i.lo - 100.0f);
        CHECK(crt::get(p, field) == i.lo);
        // Setting one field moves no other.
        NtscParams q;
        crt::set(q, field, i.lo);
        for (std::size_t g = 0; g < crt::kFields; ++g)
            if (g != f)
                CHECK(crt::get(q, static_cast<Field>(g)) ==
                      crt::get(defaults, static_cast<Field>(g)));
    }
    // A NaN from a damaged file becomes a legal value, not a black screen.
    {
        NtscParams p;
        const float nan = std::stof("nan");
        crt::set(p, Field::CenterLighting, nan);
        CHECK(p.centerLighting >= 0.5f && p.centerLighting <= 1.0f);
    }

    // The settings keys are distinct and spell monitor then field.
    {
        std::set<std::string> keys;
        for (std::size_t m = 0; m < crt::kMonitors; ++m)
            for (std::size_t f = 0; f < crt::kFields; ++f)
                keys.insert(crt::settingKey(static_cast<Monitor>(m), static_cast<Field>(f)));
        CHECK(keys.size() == crt::kMonitors * crt::kFields);
        CHECK(crt::settingKey(Monitor::MonoAmber, Field::Scanlines) == "crt_amber_scanlines");
        CHECK(crt::settingKey(Monitor::CompositeOE, Field::BloomRadius) == "crt_oe_bloom_radius");
    }

    // The layering itself.
    {
        crt::Overrides ov;
        NtscParams global;
        global.palMode = true;                         // machine-level, not layered
        CHECK(ov.count(Monitor::MonoAmber) == 0);
        const NtscParams before = ov.resolve(Monitor::MonoAmber, global);
        CHECK(before.scanlines == crt::preset(Monitor::MonoAmber).scanlines);
        CHECK(before.palMode);

        ov.set(Monitor::MonoAmber, Field::Scanlines, 0.6f);
        CHECK(ov.has(Monitor::MonoAmber, Field::Scanlines));
        CHECK(ov.count(Monitor::MonoAmber) == 1);
        const NtscParams amber = ov.resolve(Monitor::MonoAmber, global);
        CHECK(amber.scanlines == 0.6f);
        // …one field, on one monitor: the rest of amber is still its preset,
        CHECK(amber.bloom == crt::preset(Monitor::MonoAmber).bloom);
        CHECK(amber.persistence == defaults.persistence);
        // and no other monitor moved at all.
        CHECK(ov.resolve(Monitor::MonoGreen, global).scanlines == defaults.scanlines);
        CHECK(ov.resolve(Monitor::CompositeOE, global).scanlines == defaults.scanlines);

        // An override equal to the preset is still an override: the user's
        // choice survives a later change of the preset.
        ov.set(Monitor::MonoGreen, Field::Bloom, crt::preset(Monitor::MonoGreen).bloom);
        CHECK(ov.has(Monitor::MonoGreen, Field::Bloom));

        // The shadow mask is a field like the others.
        ov.set(Monitor::ChatMauve, Field::ShadowMask, 2.0f);
        CHECK(ov.resolve(Monitor::ChatMauve, global).shadowMask ==
              NtscParams::ShadowMask::ApertureGrille);

        // Reset one field, then the whole monitor.
        ov.clear(Monitor::MonoAmber, Field::Scanlines);
        CHECK(!ov.has(Monitor::MonoAmber, Field::Scanlines));
        CHECK(ov.resolve(Monitor::MonoAmber, global).scanlines == defaults.scanlines);
        ov.set(Monitor::MonoAmber, Field::Barrel, 0.1f);
        ov.set(Monitor::MonoAmber, Field::Hue, 0.2f);
        ov.clear(Monitor::MonoAmber);
        CHECK(ov.count(Monitor::MonoAmber) == 0);
        CHECK(ov.count(Monitor::MonoGreen) == 1);      // untouched
    }

    // A slider's member finds its field; an unknown address finds none.
    {
        NtscParams p;
        CHECK(crt::fieldAt(p, &p.scanlines) == Field::Scanlines);
        CHECK(crt::fieldAt(p, &p.bloomThreshold) == Field::BloomThreshold);
        float stray = 0.0f;
        CHECK(crt::fieldAt(p, &stray) == Field::Count);
        // Every float field has a member (the mask is the one int).
        int found = 0;
        const float* members[] = {
            &p.brightness, &p.contrast, &p.saturation, &p.hue, &p.sharpness,
            &p.persistence, &p.phosphorGamma, &p.scanlines, &p.barrel,
            &p.centerLighting, &p.shadowMaskStrength, &p.luminanceGain,
            &p.rgbBandwidthMHz, &p.bloom, &p.bloomRadius, &p.bloomThreshold };
        for (const float* m : members) found += crt::fieldAt(p, m) != Field::Count;
        CHECK(found == static_cast<int>(crt::kFields) - 1);
    }

    if (failures) { std::printf("crt_presets: %d check(s) failed\n", failures); return 1; }
    std::printf("crt_presets: OK\n");
    return 0;
}
