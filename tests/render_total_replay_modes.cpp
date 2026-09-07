// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Headless renderer for Total Replay across the 8 HiResMode variants.
// Not added to the test suite — a one-off tool driven from the CLI.
//
//   build/POM2_render_modes <out-dir>
//
// Boots Total Replay via SmartPortCard + SmartPortHdvUnit (same as
// total_replay_boot_trace), runs enough cycles for the splash, then for
// each HiResMode renders Apple2Display::pixels() into a PPM.
//
// ColorCompositeOE additionally runs a CPU port of the GLSL shader on
// Apple2Display::signal() so the captured frame reflects what users
// actually see on screen (the regular framebuffer in OE mode is the
// NTSC LUT fallback by design).

#include "Apple2Display.h"
#include "AppleWinNtsc.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "Logger.h"

#include "ProbeOutDir.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;     if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p;  if (fs::exists(up2)) return up2;
    }
    return {};
}

void writePpm(const std::string& path, int w, int h, const uint32_t* rgba)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "PPM open fail: %s\n", path.c_str()); return; }
    f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = rgba[i];
        rgb[i*3 + 0] = static_cast<uint8_t>( p        & 0xFF);
        rgb[i*3 + 1] = static_cast<uint8_t>((p >>  8) & 0xFF);
        rgb[i*3 + 2] = static_cast<uint8_t>((p >> 16) & 0xFF);
    }
    f.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
}

// CPU port of NtscPostProcessor's fragment shader. Same math, single
// pass, scanline-by-scanline. Output: 560 × (192*2) RGBA. No
// persistence (single frame), default NTSC parameters except a touch
// of contrast/saturation so the captured screenshot matches the
// "factory default" look users see on first launch.
void renderCompositeShader(const uint8_t* signal, int sw, int sh,
                           uint32_t* outRgba, float phaseShift = 0.0f)
{
    const int ow = sw;
    const int oh = sh * 2;
    constexpr float PI = 3.14159265358979f;

    auto getSig = [&](int x, int y) -> float {
        if (x < 0 || x >= sw || y < 0 || y >= sh) return 0.0f;
        return signal[y * sw + x] / 255.0f;
    };

    const float brightness  = 0.0f;
    const float contrast    = 1.0f;
    const float saturation  = 1.0f;
    const float hue         = 0.0f;
    const float sharpness   = 0.5f;
    const float scanlines   = 0.25f;
    const float barrel      = 0.05f;

    const float sigmaY = 0.8f;
    const float sigmaC = (1.0f - sharpness) * 1.5f + 1.0f;
    constexpr int N = 8;

    for (int py = 0; py < oh; ++py) {
        for (int px = 0; px < ow; ++px) {
            float u = (px + 0.5f) / float(ow);
            float v = (py + 0.5f) / float(oh);

            // Barrel
            float cuvx = u * 2 - 1, cuvy = v * 2 - 1;
            float r2 = cuvx*cuvx + cuvy*cuvy;
            float buvx = cuvx * (1 + barrel * r2);
            float buvy = cuvy * (1 + barrel * r2);
            float uvx = buvx * 0.5f + 0.5f;
            float uvy = buvy * 0.5f + 0.5f;
            if (uvx < 0 || uvx > 1 || uvy < 0 || uvy > 1) {
                outRgba[py * ow + px] = 0xFF000000u;
                continue;
            }

            float sigX = uvx * sw;
            float sigY = uvy * sh;
            int yInt = static_cast<int>(sigY);

            float Y = 0, I = 0, Q = 0, wYs = 0, wCs = 0;
            for (int i = -N; i <= N; ++i) {
                float fx = sigX + i;
                float s  = getSig(static_cast<int>(fx), yInt);
                float dy = static_cast<float>(i);
                float wY = std::exp(-0.5f * dy*dy / (sigmaY*sigmaY));
                float wC = std::exp(-0.5f * dy*dy / (sigmaC*sigmaC));
                float phase = PI * 0.5f * fx + phaseShift;
                Y   += s * wY;
                I   += s * std::sin(phase) * wC * 2.0f;
                Q   += s * std::cos(phase) * wC * 2.0f;
                wYs += wY;
                wCs += wC;
            }
            Y /= wYs; I /= wCs; Q /= wCs;

            // Hue
            float h = hue * PI;
            float cs = std::cos(h), sn = std::sin(h);
            float Ir = I * cs - Q * sn;
            float Qr = I * sn + Q * cs;

            // YIQ → RGB
            float r = Y + 0.956f*Ir + 0.621f*Qr;
            float g = Y - 0.272f*Ir - 0.647f*Qr;
            float b = Y - 1.106f*Ir + 1.703f*Qr;

            // B/C/S
            r = (r - 0.5f)*contrast + 0.5f + brightness;
            g = (g - 0.5f)*contrast + 0.5f + brightness;
            b = (b - 0.5f)*contrast + 0.5f + brightness;
            float luma = 0.299f*r + 0.587f*g + 0.114f*b;
            r = luma + (r - luma)*saturation;
            g = luma + (g - luma)*saturation;
            b = luma + (b - luma)*saturation;

            r = std::clamp(r, 0.0f, 1.0f);
            g = std::clamp(g, 0.0f, 1.0f);
            b = std::clamp(b, 0.0f, 1.0f);

            // Scanlines (odd output rows)
            if (py & 1) {
                r *= 1 - scanlines;
                g *= 1 - scanlines;
                b *= 1 - scanlines;
            }

            const uint32_t R = static_cast<uint32_t>(r * 255.0f);
            const uint32_t G = static_cast<uint32_t>(g * 255.0f);
            const uint32_t B = static_cast<uint32_t>(b * 255.0f);
            outRgba[py * ow + px] = 0xFF000000u | (B << 16) | (G << 8) | R;
        }
    }
}

struct ModeEntry {
    Apple2Display::HiResMode m;
    const char* tag;
};

} // namespace

int main(int argc, char** argv)
{
    const std::string outDir = pom2test::probeOutDir(argc > 1 ? argv[1] : "");

    Memory mem; M6502 cpu(&mem);
    mem.clearRam(); mem.resetSoftSwitches(); mem.setIIEMode(true);

    const std::string romPath = firstExisting({"roms/apple2e.rom"});
    if (romPath.empty()) { std::fprintf(stderr, "no apple2e.rom\n"); return 1; }
    if (!mem.loadAppleIIRom(romPath.c_str(), false)) {
        std::fprintf(stderr, "load ROM failed\n"); return 1;
    }

    const std::string hdvPath = firstExisting({
        "floppyemu/Total Replay v6.1.hdv",
        "hdv/Total Replay II v1.0-alpha.4.hdv",
    });
    if (hdvPath.empty()) { std::fprintf(stderr, "no Total Replay HDV\n"); return 1; }
    auto card = std::make_unique<pom2::SmartPortCard>(5);
    auto unit = std::make_unique<pom2::SmartPortHdvUnit>();
    if (!unit->loadImage(hdvPath)) {
        std::fprintf(stderr, "HDV load failed: %s\n", unit->lastError().c_str());
        return 1;
    }
    std::fprintf(stderr, "Mounted %s (%u blocks)\n",
                 hdvPath.c_str(), unit->blockCount());
    card->setUnit(0, std::move(unit));
    mem.slotBus().plug(5, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // Walk enough instructions to load ProDOS + Total Replay's splash
    // logo, but stop BEFORE the launcher starts auto-cycling through
    // game previews. The intro splash sits at the moment ProDOS hands
    // control to TR.SYSTEM and the title screen has finished painting.
    // Override via POM2_RENDER_INSTRS=N for experimentation.
    long long kInstrs = 60'000'000;
    if (const char* env = std::getenv("POM2_RENDER_INSTRS")) {
        kInstrs = std::atoll(env);
        if (kInstrs <= 0) kInstrs = 60'000'000;
    }
    for (long long i = 0; i < kInstrs; ++i) cpu.step();

    // Plug a Le Chat Mauve card into slot 7 so the ChatMauveRGB mode
    // takes its native path instead of silently falling back to NTSC.
    // The card itself just decodes whatever's in HGR memory — no
    // firmware ROM needed for rendering.
    auto chat = std::make_unique<LeChatMauveCard>(7);
    LeChatMauveCard* chatRaw = chat.get();
    mem.slotBus().plug(7, std::move(chat));

    Apple2Display display;
    display.setChatMauveCard(chatRaw);
    if (mem.isIIE()) display.setAuxMemory(mem.auxData());

    // Force the renderer to look at HGR page 1 — Total Replay's
    // splash lives there.
    std::fprintf(stderr, "Final PC=$%04X, captured screen state\n",
                 cpu.getProgramCounter());

    static const ModeEntry kModes[] = {
        { Apple2Display::HiResMode::ColorNTSC,        "01_ColorNTSC"        },
        { Apple2Display::HiResMode::ColorCompMedium,  "02_ColorCompMedium"  },
        { Apple2Display::HiResMode::ColorComp4Bit,    "03_ColorComp4Bit"    },
        { Apple2Display::HiResMode::ChatMauveRGB,     "04_ChatMauveRGB"     },
        { Apple2Display::HiResMode::ColorCompositeOE, "05_ColorCompositeOE" },
        { Apple2Display::HiResMode::MonoWhite,        "06_MonoWhite"        },
        { Apple2Display::HiResMode::MonoGreen,        "07_MonoGreen"        },
        { Apple2Display::HiResMode::MonoAmber,        "08_MonoAmber"        },
        { Apple2Display::HiResMode::ColorAppleWin,    "09_ColorAppleWin_Monitor" },
        { Apple2Display::HiResMode::ColorAppleWin,    "10_ColorAppleWin_Tv"      },
        { Apple2Display::HiResMode::ColorAppleWin,    "11_ColorAppleWin_Idealized" },
    };

    namespace fs = std::filesystem;
    fs::create_directories(outDir);

    for (size_t idx = 0; idx < sizeof(kModes) / sizeof(kModes[0]); ++idx) {
        const auto& e = kModes[idx];
        display.setHiResMode(e.m);
        // For the three AppleWin entries, the enum is the same but the
        // sub-mode varies. Pick it from the tag suffix.
        if (e.m == Apple2Display::HiResMode::ColorAppleWin) {
            const std::string tag = e.tag;
            if      (tag.find("_Tv")        != std::string::npos)
                display.setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);
            else if (tag.find("_Idealized") != std::string::npos)
                display.setAppleWinSubMode(Apple2Display::AppleWinSubMode::Idealized);
            else
                display.setAppleWinSubMode(Apple2Display::AppleWinSubMode::Monitor);
        }
        display.render(mem);
        const int w = display.width();
        const int h = display.height();
        const std::string ppm = outDir + "/total_replay_" + e.tag + ".ppm";
        writePpm(ppm, w, h, display.pixels());
        std::fprintf(stderr, "  wrote %s (%dx%d)\n", ppm.c_str(), w, h);

        if (e.m == Apple2Display::HiResMode::ColorCompositeOE
            && display.signalProduced()) {
            const int sw = display.signalWidth();
            const int sh = display.signalHeight();
            // Dump the raw composite signal so a GL harness can feed it to
            // the REAL NtscPostProcessor shader (not this CPU port).
            {
                std::ofstream sf(outDir + "/oe_signal.bin", std::ios::binary);
                sf.write(reinterpret_cast<const char*>(display.signal()),
                         static_cast<std::streamsize>(sw) * sh);
            }
            std::vector<uint32_t> shaderOut(sw * sh * 2);
            // Sweep demod phase shifts to find the one matching the NTSC LUT
            // reference (the artifact-colour-phase calibration).
            const float PIf = 3.14159265358979f;
            const struct { float sh; const char* tag; } phases[] = {
                {0.0f, "_p000"}, {PIf*0.5f, "_p090"},
                {PIf, "_p180"}, {PIf*1.5f, "_p270"},
            };
            for (const auto& ph : phases) {
                renderCompositeShader(display.signal(), sw, sh, shaderOut.data(), ph.sh);
                const std::string sppm = outDir + "/total_replay_"
                                        + e.tag + "_shader" + ph.tag + ".ppm";
                writePpm(sppm, sw, sh * 2, shaderOut.data());
                std::fprintf(stderr, "  wrote %s\n", sppm.c_str());
            }
        }
    }

    // ── AppleWin demod phase sweep ───────────────────────────────────────
    // Rebuild the AppleWin chroma LUT at each subcarrier phase and re-render
    // (the signal is already captured — no reboot). Pick the PPM whose
    // artifact hues match the NTSC reference.
    {
        display.setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        display.setAppleWinSubMode(Apple2Display::AppleWinSubMode::Monitor);
        const float PIf = 3.14159265358979f;
        const struct { float sh; const char* tag; } aw[] = {
            {0.0f, "p000"}, {PIf*0.5f, "p090"}, {PIf, "p180"}, {PIf*1.5f, "p270"},
        };
        for (const auto& a : aw) {
            pom2::AppleWinNtsc::rebuildForPhase(a.sh);
            display.render(mem);
            const std::string p = outDir + "/total_replay_AppleWin_sweep_"
                                 + a.tag + ".ppm";
            writePpm(p, display.width(), display.height(), display.pixels());
            std::fprintf(stderr, "  wrote %s\n", p.c_str());
        }
    }
    return 0;
}
