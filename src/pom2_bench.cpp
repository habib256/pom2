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

// subject.
// Copyright (C) 2026
//
// WHY THIS EXISTS, and why it is NOT pom2_headless.
//
// `pom2_headless` is an interactive telnet console: it starts the worker
// thread, paces to wall-clock, opens an audio device and waits for a human.
// None of that can be profiled or compared run-to-run. This target is the
// opposite — a *closed* run with no threads, no audio device, no sockets and
// no wall-clock pacing:
//
//     one call to run()  →  exactly N frames of `cyclesPerFrame` cycles
//
// so two invocations retire the SAME number of instructions. That property is
// what makes the two uses below work at all:
//
//   1. **callgrind subject** — `Ir` (instructions retired) is stable to the
//      instruction, which compares two builds far more reliably than wall
//      time. See docs/PERFORMANCE.md for the recipe.
//   2. **PGO training driver** — packaging/raspberry/pgo_train.sh runs this
//      binary over several machine profiles and video modes so GCC's profile
//      covers the real spread of hot paths, not just one boot.
//
// It also prints an FNV-1a hash of the final framebuffer (and of RAM), which
// is the cheap way to prove an optimisation is *output-identical*: change the
// code, re-run, compare the hash. A build variant (‑O3 vs PGO+LTO) must never
// move it.
//
// Deliberately NOT wired: ClockCard (reads host time → non-deterministic),
// the SSC listener (sockets), AudioDevice (a second thread), rewind.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>

namespace {

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// Probe the conventional in-repo locations so the bench works whether it is
// launched from the repo root or from build/.
std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates)
        if (fileExists(c)) return c;
    return {};
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628257ull;
    }
    return h;
}

struct ModeName { const char* name; Apple2Display::HiResMode mode; };

// Every integer-deterministic colour pipeline, plus the two FP ones. The FP
// pipelines (OE-CPU demod, AppleWin IIR) are reachable on purpose: they are
// the most expensive per-frame code in POM2 and PGO must see them. Their
// framebuffer hash is host-FP-dependent, so --hash-frame is off by default
// when they are selected (see main()).
const ModeName kModes[] = {
    { "ntsc",       Apple2Display::HiResMode::ColorNTSC          },
    { "medium",     Apple2Display::HiResMode::ColorCompMedium    },
    { "4bit",       Apple2Display::HiResMode::ColorComp4Bit      },
    { "mono",       Apple2Display::HiResMode::MonoWhite          },
    { "green",      Apple2Display::HiResMode::MonoGreen          },
    { "amber",      Apple2Display::HiResMode::MonoAmber          },
    { "oe",         Apple2Display::HiResMode::ColorCompositeOE   },
    { "oecpu",      Apple2Display::HiResMode::ColorCompositeOECpu},
    { "applewin",   Apple2Display::HiResMode::ColorAppleWin      },
    { "chatmauve",  Apple2Display::HiResMode::ChatMauveRGB       },
};

bool modeIsFloatingPoint(const std::string& m)
{
    return m == "oecpu" || m == "applewin";
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Deterministic headless run: N frames with a requested cycle budget,\n"
        "no threads, no audio, no wall-clock pacing. Same arguments → same\n"
        "cycle totals and RAM hash; each frame finishes its last instruction.\n"
        "\n"
        "  --rom <path>        Main ROM: apple2p.rom (apple2e.rom with --iie),\n"
        "                      then apple2.rom; probes roms/ relative paths.\n"
        "  --prom <path>       Disk II PROM.   Default: probe roms/disk2.rom\n"
        "  --disk <path>       .dsk/.do/.po/.nib/.woz to boot. Optional:\n"
        "                      without it the machine runs its ROM banner.\n"
        "  --iie               //e mode (pass apple2e.rom via --rom).\n"
        "  --pal               PAL timing (312 lines, 20313 cycles/frame).\n"
        "  --frames <N>        Frames to run. Default 300.\n"
        "  --cycles-per-frame <N>  Override the frame budget.\n"
        "  --mode <name>       Video pipeline: ntsc medium 4bit mono green\n"
        "                      amber oe oecpu applewin chatmauve.\n"
        "  --no-render         Skip Apple2Display entirely (pure CPU/bus).\n"
        "  --hash-all          Hash EVERY frame, not just the last. Catches a\n"
        "                      regression that only shows on a transient frame,\n"
        "                      but hashing 560x192x4 bytes per frame costs more\n"
        "                      than the emulation does — never use it under\n"
        "                      callgrind or while training PGO.\n"
        "  --hash-frame        Force the framebuffer hash on (see note on\n"
        "                      floating-point pipelines below).\n"
        "  --quiet             One line of output, nothing else.\n"
        "  --read-watch HEX    Arm a read watchpoint on that address — no\n"
        "                      sink, so nothing stops; measures the cost of\n"
        "                      the armed read diversion (PERFORMANCE 8.5).\n"
        "\n"
        "The main-RAM hash ($0000-$BFFF) is always printed and integer-exact;\n"
        "the framebuffer hash is printed for integer pipelines only, because\n"
        "the OE-CPU and AppleWin decoders quantize floating-point math and\n"
        "would differ between hosts (--hash-frame overrides).\n",
        prog);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string romArg, promArg, diskArg, modeArg = "ntsc";
    int  frames         = 300;
    int  cyclesPerFrame = 0;      // 0 = derive from the video standard
    bool iie            = false;
    bool pal            = false;
    bool render         = true;
    bool hashFrameForce = false;
    bool hashAll        = false;
    bool quiet          = false;
    bool dumpText       = false;
    int  readWatch      = -1;        // --read-watch, or none

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--rom"    && i + 1 < argc) romArg  = argv[++i];
        else if (a == "--prom"   && i + 1 < argc) promArg = argv[++i];
        else if (a == "--disk"   && i + 1 < argc) diskArg = argv[++i];
        else if (a == "--mode"   && i + 1 < argc) modeArg = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames  = std::atoi(argv[++i]);
        else if (a == "--cycles-per-frame" && i + 1 < argc)
                                                  cyclesPerFrame = std::atoi(argv[++i]);
        else if (a == "--iie")        iie   = true;
        else if (a == "--pal")        pal   = true;
        else if (a == "--no-render")  render = false;
        else if (a == "--hash-frame") hashFrameForce = true;
        else if (a == "--hash-all")   hashAll = true;
        else if (a == "--quiet")      quiet = true;
        else if (a == "--read-watch" && i + 1 < argc)
                                      readWatch = static_cast<int>(std::strtol(argv[++i], nullptr, 16));
        else if (a == "--dump-text")  dumpText = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
               usage(argv[0]); return 1; }
    }

    if (frames <= 0) { std::fprintf(stderr, "--frames must be > 0\n"); return 1; }

    Apple2Display::HiResMode hiRes = Apple2Display::HiResMode::ColorNTSC;
    bool modeOk = false;
    for (const ModeName& m : kModes)
        if (modeArg == m.name) { hiRes = m.mode; modeOk = true; break; }
    if (!modeOk) {
        std::fprintf(stderr, "unknown --mode '%s'\n", modeArg.c_str());
        return 1;
    }

    const std::string romPath = !romArg.empty() ? romArg : findFirst({
        iie ? "roms/apple2e.rom"       : "roms/apple2p.rom",
        iie ? "../roms/apple2e.rom"    : "../roms/apple2p.rom",
        "roms/apple2.rom", "../roms/apple2.rom", "../../roms/apple2.rom" });
    if (romPath.empty()) {
        std::fprintf(stderr, "no ROM found (pass --rom)\n");
        return 1;
    }

    Memory mem;
    // setIIEMode BEFORE loadAppleIIRom: the loader only splits a 16/32 KB
    // //e dump into the internal $C100-$CFFF I/O ROM when iieMode is already
    // on (same ordering rule as MainWindow::applyProfile step 4/5). With
    // the two swapped the //e booted into an empty $C300, executed BRK ($00)
    // forever, and every "//e" measurement — and PGO training run — was of
    // a BRK loop. `--dump-text` exists to catch exactly that.
    mem.setIIEMode(iie);
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom(%s) failed\n", romPath.c_str());
        return 1;
    }
    mem.setVideoStandard(pal ? VideoStandard::PAL : VideoStandard::NTSC);

    // 17045 / 20313 — the same per-frame budget EmulationController uses, so
    // the bench's "frame" is the emulator's frame and the numbers transfer.
    if (cyclesPerFrame <= 0) cyclesPerFrame = pal ? 20313 : 17045;

    // ── Optional Disk II in slot 6 ────────────────────────────────────────
    // A boot is the single most representative workload there is: it drives
    // the LSS nibble walker, the ROM, and then whatever the disk loads.
    bool booting = false;
    if (!diskArg.empty()) {
        const std::string promPath = !promArg.empty() ? promArg : findFirst({
            "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom" });
        if (promPath.empty()) {
            std::fprintf(stderr, "--disk given but no disk2.rom found\n");
            return 1;
        }
        auto card = std::make_unique<DiskIICard>();
        if (!card->loadBootRom(promPath)) {
            std::fprintf(stderr, "loadBootRom(%s) failed\n", promPath.c_str());
            return 1;
        }
        // Bit-level LSS PROM when present — the legacy 32-cycle gate is a
        // different (and much cheaper) code path, so profiling without it
        // would under-weight the real one.
        for (const char* p : { "roms/diskii_p6.rom", "../roms/diskii_p6.rom",
                               "../../roms/diskii_p6.rom" })
            if (fileExists(p)) { (void)card->loadLssRom(p); break; }
        if (!card->insertDisk(diskArg)) {
            std::fprintf(stderr, "insertDisk(%s) failed: %s\n",
                         diskArg.c_str(), card->getLastError().c_str());
            return 1;
        }
        card->seekTrack0();
        mem.slotBus().plug(6, std::move(card));
        booting = true;
    }

    M6502 cpu(&mem);
    if (readWatch >= 0) mem.setReadWatch(static_cast<uint16_t>(readWatch), true);
    cpu.hardReset();
    mem.slotBus().reset();
    if (booting) cpu.setProgramCounter(0xC600);   // Disk II boot PROM entry

    std::unique_ptr<Apple2Display> disp;
    if (render) {
        disp = std::make_unique<Apple2Display>();
        disp->setHiResMode(hiRes);
    }

    const auto t0 = std::chrono::steady_clock::now();

    uint64_t cycles    = 0;
    uint64_t frameHash = 1469598103934665603ull;
    for (int f = 0; f < frames; ++f) {
        // Same 4 KiB chunking as EmulationController::tickFrame, so the
        // per-slice bookkeeping (and its branch profile) matches the real
        // emulator rather than one giant run() call.
        constexpr int kChunk = 4096;
        for (int done = 0; done < cyclesPerFrame; ) {
            const int want = (cyclesPerFrame - done) < kChunk
                           ? (cyclesPerFrame - done) : kChunk;
            const int got  = cpu.run(want);
            done   += (got > 0 ? got : want);
            cycles += static_cast<uint64_t>(got > 0 ? got : want);
        }
        if (disp) {
            disp->render(mem);
            // Hashing 560x192x4 bytes costs MORE than a frame of emulation
            // (measured: 17 % of a callgrind boot profile when done every
            // frame), so the default hashes the last frame only. --hash-all
            // is for identity checks, where the extra coverage — a
            // regression on one transient frame, a mid-boot page flip —
            // is worth the distortion. Never combine it with profiling.
            if (hashAll || f == frames - 1)
                frameHash = fnv1a(disp->pixels(),
                                  static_cast<size_t>(disp->width()) *
                                  static_cast<size_t>(disp->height()) * 4,
                                  frameHash);
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const uint64_t ramHash = fnv1a(mem.data(), 0xC000);

    if (dumpText) {
        std::printf("PC=$%04X\n", cpu.getProgramCounter());
        for (int r = 0; r < 24; ++r) {
            const uint16_t base = static_cast<uint16_t>(
                0x0400 + (r & 7) * 0x80 + (r >> 3) * 0x28);
            char line[41];
            for (int c = 0; c < 40; ++c) {
                const uint8_t ch = mem.peekMainRam(static_cast<uint16_t>(base + c)) & 0x7F;
                line[c] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '.';
            }
            line[40] = '\0';
            std::printf("|%s|\n", line);
        }
    }
    const double   mhz     = secs > 0 ? (double)cycles / secs / 1e6 : 0.0;
    const double   ratio   = mhz * 1e6 / POM2_CPU_CLOCK_HZ;

    const bool showFrameHash =
        disp && (hashFrameForce || !modeIsFloatingPoint(modeArg));

    if (!quiet) {
        std::printf("pom2_bench: rom=%s%s%s mode=%s%s\n",
                    romPath.c_str(),
                    iie ? " //e" : " ][+",
                    pal ? " PAL" : " NTSC",
                    render ? modeArg.c_str() : "(no render)",
                    booting ? " booting" : "");
    }
    std::printf("frames=%d cycles=%llu wall=%.3fs speed=%.2f MHz (%.1fx) "
                "ram=%016llx",
                frames, (unsigned long long)cycles, secs, mhz, ratio,
                (unsigned long long)ramHash);
    if (showFrameHash)
        std::printf(" fb%s=%016llx", hashAll ? "all" : "",
                    (unsigned long long)frameHash);
    else if (disp)
        std::printf(" fb=<fp-dependent>");
    std::printf("\n");
    return 0;
}
