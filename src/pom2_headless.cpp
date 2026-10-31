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

// A console-only build that brings up the same core (Memory + M6502 +
// SlotBus + DiskIICard + SuperSerialCard) without GLFW or ImGui. The
// SSC's TCP listener is the user-facing terminal: telnet to the port,
// type Apple II keystrokes, see PRINT output.
//
// Boot/post-boot keyboard injection: a real disk boots into Applesoft
// without knowing the SSC exists, so the emulator pastes a small setup
// sequence (`PR#2 / IN#2`) once the disk has had time to spin up. After
// that, every byte the telnet client sends is fed via the SSC into the
// Apple II's input vector, and every PRINTed byte comes back out on the
// SSC's TX path.
//
// Designed for headless self-test: launch in the background, telnet in,
// type DOS 3.3 commands, observe behaviour. Exits cleanly on SIGINT.
//
// CP/M: `--softcard <slot>` plugs the Microsoft SoftCard (Z80) — or `--als
// <slot>` the ALS CP/M Card (CP/M Plus), or `--softcard2 <slot>` the
// SoftCard II (Z80 on its own 64 KB) — and drops the
// BASIC setup paste; the SSC then stays a serial line (no keyboard sink), so
// a CP/M program's RDR:/PUN: traffic is what the telnet client sees. The
// console is typed with `--type <sec>:<text>`, at an emulated second, in
// both modes; `--text-dump` prints the text page after a capture run.

#include "AlsCpmCard.h"
#include "SoftCardIICard.h"
#include "Apple2Display.h"
#include "ClockCard.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "ResourcePaths.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SuperSerialTransport.h"
#include "Version.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int /*sig*/) { g_quit = true; }

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// Resolve a bundled asset. Tries POM2's real resolver first — the one the GUI
// uses, which knows the INSTALLED layouts (`<exe>/../share/POM2/...`, the macOS
// `Contents/Resources`, `$XDG_DATA_HOME`) — then falls back to the historic
// CWD-relative probes for a plain source checkout.
//
// Going through findResource is what lets the release jobs run the boot capture
// against the PACKAGED binary: inside an AppImage the ROMs live at
// usr/share/POM2/roms, which no amount of "../roms" ever finds. That also makes
// the smoke test the package's asset resolution, not just its CPU core.
std::string findAsset(const std::string& rel,
                      std::initializer_list<const char*> legacy = {})
{
    const std::string viaResolver = pom2::findResource(rel);
    if (!viaResolver.empty() && fileExists(viaResolver)) return viaResolver;
    for (const char* c : legacy) if (fileExists(c)) return c;
    return {};
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "  --rom <path>          System ROM (normally 12/16 KiB for II/II+).\n"
        "                        Default: probe roms/apple2.rom; no profile switch.\n"
        "  --prom <path>         disk2.rom Disk II PROM.      Default: probe roms/,\n"
        "                        else POM2's own boot PROM (DOS 3.3, ProDOS);\n"
        "                        `--prom pom2` forces POM2's even with a dump\n"
        "  --disk <path>         Disk II image. Default: probe disks_5.4/dos33_master.dsk\n"
        "  --port <N>            SSC TCP port. Default 6502\n"
        "  --paste-after <sec>   Emulated seconds before autopasting\n"
        "                        the SSC setup sequence. Default 6.\n"
        "  --setup <s>           Override the setup paste. Default \"PR#2\\rIN#2\\r\"\n"
        "                        (IN#2 LAST — its CR must be the final byte the\n"
        "                        keyboard sees; see the comment at the default).\n"
        "                        \\r \\n \\t \\\\ are unescaped.\n"
        "  --no-setup            Don't autopaste anything.\n"
        "  --softcard <slot>     Plug a Microsoft SoftCard (Z80) in that slot\n"
        "                        (1, 3, 4, 5 or 7; 4 replaces the clock card).\n"
        "                        Implies --no-setup unless --setup is given, and\n"
        "                        keeps the SSC a serial line, not a keyboard.\n"
        "  --type <sec>:<text>   Type <text> on the keyboard at emulated second\n"
        "                        <sec>. Repeatable; same escapes as --setup.\n"
        "                        Example: --type 12:'DIR\\r'\n"
        "  --als <slot>          Plug an ALS CP/M Card (Z80B, CP/M Plus) instead;\n"
        "                        slot 4 or 7 as ALS says. Same rules as --softcard.\n"
        "  --softcard2 <slot>    Plug a Microsoft SoftCard II (6 MHz Z80, own\n"
        "                        64 KB) instead, slot 4 for CP/M 2.28B. That\n"
        "                        CP/M is a //e build (80 columns): on this ][+\n"
        "                        machine every other column is lost.\n"
        "                        Same rules as --softcard.\n"
        "\n"
        "Capture mode (no telnet listener, no threads, deterministic):\n"
        "  --frames <N>          N > 0: run N emulated frames, then exit.\n"
        "                        N = 0: interactive unless --screenshot is given.\n"
        "  --screenshot <path>   Write the screen as a binary PPM (P6) after\n"
        "                        those frames. Exits non-zero if the capture is\n"
        "                        a single flat colour (exit 3). Defaults to 300\n"
        "                        frames when --frames is absent or zero.\n"
        "  --text-dump           After the frames, print the 40-column text page\n"
        "                        to stdout (24 lines, inverse/flash shown as is).\n"
        "  --trace-mli           Print every ProDOS MLI call and its result\n"
        "                        to stderr (puts the CPU on its debugged loop).\n"
        "  --trace-bdos          Print every CP/M BDOS call a card Z80 makes.\n"
        "  --trace-sp            Print every SmartPort call and its result.\n"
        "  --trace-pascal        Print the Apple Pascal BIOS calls (entries).\n"
        "  --trace-monitor       Print the Monitor's output lines and GETLNs.\n"
        "  --trace-disk          Print the Disk II's seeks, motor and writes.\n"
        "  --version             Print the version and exit.\n"
        "  -h, --help            Show this and exit.\n"
        "\n"
        "Once running (without --frames), telnet to 127.0.0.1:<port> to interact.\n",
        prog);
}

// Binary PPM (P6). Chosen over PNG because it needs no encoder: the point of
// this format is that a CI step can parse it in three lines of Python.
bool writePpm(const std::string& path, int w, int h, const uint32_t* rgba)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = rgba[i];
        rgb[i*3 + 0] = static_cast<uint8_t>( p        & 0xFF);
        rgb[i*3 + 1] = static_cast<uint8_t>((p >>  8) & 0xFF);
        rgb[i*3 + 2] = static_cast<uint8_t>((p >> 16) & 0xFF);
    }
    f.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(f);
}

// How much of the frame differs from its top-left pixel, and how many distinct
// colours it holds. Zero differing pixels = one flat colour = the machine never
// drew anything, which is the failure this capture mode exists to catch (a ROM
// that failed to load, a CPU that never ran, a display that produced nothing).
//
// Deliberately content-agnostic: it makes no claim about WHAT is on screen.
// Note the counts are of COLOURS, not bytes — POM2's Apple II text screen is
// legitimately two colours, so a byte-value count would have almost no room
// between "booted" and "blank".
struct FrameStats { size_t colours = 0; size_t differing = 0; };

FrameStats frameStats(int w, int h, const uint32_t* rgba)
{
    FrameStats s;
    std::set<uint32_t> seen;
    const uint32_t first = (w > 0 && h > 0) ? rgba[0] : 0u;
    for (int i = 0; i < w * h; ++i) {
        seen.insert(rgba[i]);
        if (rgba[i] != first) ++s.differing;
    }
    s.colours = seen.size();
    return s;
}

std::string unescape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            switch (in[i + 1]) {
                case 'r': out.push_back('\r'); ++i; break;
                case 'n': out.push_back('\n'); ++i; break;
                case 't': out.push_back('\t'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                default:  out.push_back(in[i]);
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string romArg, promArg, diskArg, setupOverride, shotPath;
    int  port        = SuperSerialCard::kDefaultPort;
    int  pasteAfter  = 6;     // emulated seconds before pasting setup
    int  frames      = 0;     // >0 = capture mode (see below)
    bool doSetup     = true;
    bool setupGiven  = false;
    bool textDump    = false;
    pom2::OsTraceKinds trace;
    int  softcardSlot = 0;   // 0 = none
    int  alsSlot      = 0;   // 0 = none
    int  sc2Slot      = 0;   // 0 = none
    // --type: text to type at an emulated second, in the order given.
    struct TypedKeys { int atSecond; std::string text; bool sent; };
    std::vector<TypedKeys> typed;
    // A flag written last, with its value missing, used to fall through every
    // `&& i+1 < argc` arm to the final else and be reported as an "unknown
    // arg" — a misdiagnosis that sends the reader looking for a typo in a
    // flag that is spelled correctly. Recognise the name first, then complain
    // about the missing value.
    auto needsValue = [&](const std::string& flag) {
        std::fprintf(stderr, "%s requires an argument\n", flag.c_str());
    };
    // Integer options: reject what atoi() would silently turn into 0.
    // `--frames abc` used to DISABLE capture mode (frames = 0) and start a
    // telnet listener instead, and `--port 70000` bound port 4464 while the
    // banner printed 70000 — the narrowing to uint16_t happens at the bind.
    auto parseInt = [&](const char* flag, const char* text, int lo, int hi,
                        int& out) -> bool {
        char* end = nullptr;
        const long v = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || v < lo || v > hi) {
            std::fprintf(stderr, "%s expects an integer in %d..%d, got: %s\n",
                         flag, lo, hi, text);
            return false;
        }
        out = static_cast<int>(v);
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rom" || a == "--prom" || a == "--disk" || a == "--port" ||
            a == "--paste-after" || a == "--setup" || a == "--frames" ||
            a == "--screenshot" || a == "--softcard" || a == "--type" || a == "--als" ||
            a == "--softcard2") {
            if (i + 1 >= argc) { needsValue(a); usage(argv[0]); return 1; }
        }
        if      (a == "--rom"   && i+1 < argc) romArg   = argv[++i];
        else if (a == "--prom"  && i+1 < argc) promArg  = argv[++i];
        else if (a == "--disk"  && i+1 < argc) diskArg  = argv[++i];
        else if (a == "--port"  && i+1 < argc) {
            if (!parseInt("--port", argv[++i], 1, 65535, port)) return 1;
        }
        else if (a == "--paste-after" && i+1 < argc) {
            if (!parseInt("--paste-after", argv[++i], 0, 100000, pasteAfter)) return 1;
        }
        else if (a == "--setup" && i+1 < argc) { setupOverride = argv[++i]; setupGiven = true; }
        else if (a == "--softcard" && i+1 < argc) {
            if (!parseInt("--softcard", argv[++i], 1, 7, softcardSlot)) return 1;
            if (softcardSlot == 2 || softcardSlot == 6) {
                std::fprintf(stderr, "--softcard: slot %d holds the %s\n", softcardSlot,
                             softcardSlot == 2 ? "Super Serial Card" : "Disk II");
                return 1;
            }
        }
        else if (a == "--als" && i+1 < argc) {
            if (!parseInt("--als", argv[++i], 1, 7, alsSlot)) return 1;
            if (alsSlot == 2 || alsSlot == 6) {
                std::fprintf(stderr, "--als: slot %d holds the %s\n", alsSlot,
                             alsSlot == 2 ? "Super Serial Card" : "Disk II");
                return 1;
            }
        }
        else if (a == "--softcard2" && i+1 < argc) {
            if (!parseInt("--softcard2", argv[++i], 1, 7, sc2Slot)) return 1;
            if (sc2Slot == 2 || sc2Slot == 6) {
                std::fprintf(stderr, "--softcard2: slot %d holds the %s\n", sc2Slot,
                             sc2Slot == 2 ? "Super Serial Card" : "Disk II");
                return 1;
            }
        }
        else if (a == "--type" && i+1 < argc) {
            const std::string v = argv[++i];
            const auto colon = v.find(':');
            int at = 0;
            if (colon == std::string::npos
                || !parseInt("--type", v.substr(0, colon).c_str(), 0, 100000, at)) {
                std::fprintf(stderr, "--type expects <seconds>:<text>, got: %s\n", v.c_str());
                return 1;
            }
            typed.push_back({ at, unescape(v.substr(colon + 1)), false });
        }
        else if (a == "--text-dump") textDump = true;
        else if (a == "--trace-mli") trace.mli = true;
        else if (a == "--trace-bdos") trace.bdos = true;
        else if (a == "--trace-sp") trace.smartPort = true;
        else if (a == "--trace-pascal") trace.pascalBios = true;
        else if (a == "--trace-monitor") trace.monitor = true;
        else if (a == "--trace-disk") trace.disk = true;
        else if (a == "--frames" && i+1 < argc) {
            if (!parseInt("--frames", argv[++i], 0, 100000000, frames)) return 1;
        }
        else if (a == "--screenshot" && i+1 < argc) shotPath = argv[++i];
        else if (a == "--no-setup") doSetup = false;
        else if (a == "--version") {
            // The release workflow greps this to prove the package's version
            // is the tag's — a binary that announces a stale number makes every
            // bug report unattachable to a release.
            // kVersionString already carries the leading 'v'. Same shape as
            // the GUI banner ("POM2: v0.8"), which the release workflow greps.
            std::printf("POM2 headless: %s\n", pom2::kVersionString);
            return 0;
        }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }
    // Capture mode: run a fixed number of frames and photograph the screen.
    // This is the SMOKE the release packages run — it proves the packaged
    // binary found its bundled roms/, executed 6502 code and drew a frame,
    // which `--help` cannot. Deterministic by construction: no worker thread
    // (tickFrame drives the CPU inline), no sleeps, no TCP listener.
    if ((softcardSlot != 0) + (alsSlot != 0) + (sc2Slot != 0) > 1) {
        std::fprintf(stderr, "--softcard, --als and --softcard2: one Z80 card at a time\n");
        return 1;
    }
    const int z80Slot = softcardSlot ? softcardSlot : alsSlot ? alsSlot : sc2Slot;
    if (z80Slot && !setupGiven) doSetup = false;
    const bool captureMode = frames > 0 || !shotPath.empty() || textDump;
    if (captureMode && frames <= 0) frames = 300;   // ~5 s: past the boot beep

    const std::string romPath = !romArg.empty() ? romArg : findAsset(
        "roms/apple2.rom", { "../roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = promArg == "pom2" ? std::string()
        : !promArg.empty() ? promArg : findAsset(
        "roms/disk2.rom", { "../roms/disk2.rom", "../../roms/disk2.rom" });
    // This legacy DOS master probe resolves only in a development library.
    // Packages ship named curated images under bundled_disks/ instead;
    // pass --disk explicitly to select one of those for this frontend.
    const std::string diskPath = !diskArg.empty() ? diskArg : findAsset(
        "disks_5.4/dos33_master.dsk", { "../disks_5.4/dos33_master.dsk",
                                        "../../disks_5.4/dos33_master.dsk" });

    if (romPath.empty()) {
        std::fprintf(stderr, "missing apple2.rom; try --help\n");
        return 1;
    }
    // Outside capture mode the disk IS the point (the telnet console drives
    // DOS 3.3), so its absence stays fatal. In capture mode the machine boots
    // the ROM to the Applesoft prompt on its own. Packages omit the legacy
    // DOS master; requiring it would make their no-argument smoke fail.
    // No disk2.rom is not fatal: the card boots with POM2's own PROM.
    if (!captureMode && diskPath.empty()) {
        std::fprintf(stderr, "missing disk; try --help\n");
        return 1;
    }
    std::fprintf(stderr,
        "[POM2 headless] rom=%s prom=%s disk=%s port=%d\n",
        romPath.c_str(), promPath.empty() ? "(POM2 boot PROM)" : promPath.c_str(),
        diskPath.c_str(), port);

    // II/II+ mode is enough for DOS 3.3. We deliberately avoid loading
    // apple2e.rom here even if present so the headless target stays
    // simple; --rom changes the file, not the machine's memory mode.
    EmulationController controller;
    // Everything from here to `controller.start()` below runs before the
    // CPU worker exists, so the raw accessor is correct: there is no second
    // thread to race. The one access inside the run loop takes lockState().
    if (!controller.memory().loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    DiskIICard* diskRaw = nullptr;
    // A Disk II with an EMPTY drive is worse than no Disk II at all for the
    // capture smoke: the II+ autostart ROM hands control to the boot PROM,
    // which spins forever waiting for a disk that is never coming, and the
    // capture is the uninitialised text page. With no controller the same ROM
    // falls through to Applesoft and draws its banner — which is what we want
    // to photograph. Packages omit the legacy default disk, so this is the
    // normal path without an explicit --disk there.
    const bool plugDiskII = !captureMode || !diskPath.empty();
    if (plugDiskII) {
        auto disk = std::make_unique<DiskIICard>();
        if (!promPath.empty() && !disk->loadBootRom(promPath)) {
            std::fprintf(stderr, "PROM load failed\n");
            return 1;
        }
        // Optional: load the bit-level LSS PROM if available. Falls back to
        // the legacy 32-cycle gate when missing.
        {
            const std::string lss = findAsset(
                "roms/diskii_p6.rom", { "../roms/diskii_p6.rom",
                                        "../../roms/diskii_p6.rom" });
            if (!lss.empty()) (void)disk->loadLssRom(lss);
        }
        // …and the 13-sector pair (341-0009 boot + 341-0010 LSS), exactly as
        // SlotCardFactory does for the GUI. Without them the card cannot flip
        // to serving13_, so every DOS 3.1/3.2/3.2.1 image mounted here hung
        // at the boot PROM — and the card's own warning blamed
        // `roms/disk2_13.rom` for being missing while it ships in the tree.
        {
            const std::string boot13 = findAsset(
                "roms/disk2_13.rom", { "../roms/disk2_13.rom",
                                       "../../roms/disk2_13.rom" });
            if (!boot13.empty()) (void)disk->loadBootRom13(boot13);
            const std::string lss13 = findAsset(
                "roms/diskii_p6_13.rom", { "../roms/diskii_p6_13.rom",
                                           "../../roms/diskii_p6_13.rom" });
            if (!lss13.empty()) (void)disk->loadLssRom13(lss13);
        }
        if (!diskPath.empty() && !disk->insertDisk(diskPath)) {
            std::fprintf(stderr, "insertDisk failed: %s\n", disk->getLastError().c_str());
            return 1;
        }
        diskRaw = disk.get();
        controller.memory().slotBus().plug(6, std::move(disk));
    }

    // No TCP listener in capture mode: it is the one part of this binary that
    // can fail for reasons that have nothing to do with the package (a busy
    // port on a shared CI runner), and nothing in capture mode talks to it.
    if (!captureMode) {
        auto ssc = std::make_unique<SuperSerialCard>(SuperSerialCard::kDefaultSlot);
        // Apple's EPROM when it is there, as SlotCardFactory does for the GUI.
        // CP/M's BIOS drives the card through its Pascal entries in the $C800
        // half, which the hand-assembled fallback page does not have.
        for (const char* rel : { "roms/ssc_341-0065-a.bin", "roms/341-0065-a.bin",
                                 "roms/SSC.rom" }) {
            const std::string rom = findAsset(rel);
            if (rom.empty()) continue;
            std::ifstream f(rom, std::ios::binary);
            std::vector<uint8_t> bytes(SuperSerialCard::kFirmwareBytes + 1);
            f.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
            bytes.resize(static_cast<std::size_t>(f.gcount()));
            if (ssc->loadFirmware(bytes)) break;
        }
        // pasteKeyStream, not pasteText — the clipboard filter drops the
        // control bytes a terminal has to be able to send (see
        // MainWindow_SlotConfig.cpp's copy of this sink).
        // Under CP/M the SSC is the RDR:/PUN: line, not a second keyboard.
        if (!z80Slot) {
            ssc->setKeyboardSink(
                [&mem = controller.memory()](uint8_t b) {
                    const char buf[1] = { static_cast<char>(b) };
                    mem.pasteKeyStream(buf, 1);
                });
        }
        // The card cannot build its own transport — see SuperSerialCard.
        ssc->setTransport(pom2::makeSuperSerialTcpTransport(
            *ssc, SuperSerialCard::kDefaultSlot));
        if (!ssc->startListening(static_cast<uint16_t>(port))) {
            std::fprintf(stderr, "SSC listener failed (port busy?)\n"); return 1;
        }
        controller.memory().slotBus().plug(SuperSerialCard::kDefaultSlot, std::move(ssc));
    }

    // ThunderClock+-compatible RTC in slot 4 — uPD1990AC bit-bang chip
    // emulation; ProDOS auto-detects and links its driver to it.
    if (z80Slot != ClockCard::kDefaultSlot)
        controller.memory().slotBus().plug(ClockCard::kDefaultSlot,
            std::make_unique<ClockCard>(ClockCard::kDefaultSlot));

    // The SoftCard is a bus master: EmulationController::runCpuSlice hands
    // it the slice while it owns the bus (SlotBus::dmaClaimant).
    if (softcardSlot) {
        auto z80 = std::make_unique<SoftCardZ80>();
        z80->setMemory(&controller.memory());
        z80->setCpu(&controller.cpu());
        controller.memory().slotBus().plug(softcardSlot, std::move(z80));
    }
    if (alsSlot) {
        auto als = std::make_unique<AlsCpmCard>();
        als->setMemory(&controller.memory());
        als->setCpu(&controller.cpu());
        controller.memory().slotBus().plug(alsSlot, std::move(als));
    }
    // The SoftCard II is no bus master: its Z80 runs beside the 6502, paced
    // by the slot bus's advanceCycles.
    if (sc2Slot)
        controller.memory().slotBus().plug(sc2Slot, std::make_unique<SoftCardIICard>());

    // Hard reset loads the motherboard reset vector; reset the slot bus
    // as well so DiskIICard can re-arm its trace flags. Autostart firmware
    // initializes the machine before scanning slots and entering $C600;
    // the original monitor needs the explicit fallback below.
    controller.cpu().hardReset();
    controller.memory().slotBus().reset();
    if (diskRaw) diskRaw->seekTrack0();
    // Let the ROM's own Autostart cold-start do the booting when it HAS one.
    // It sets up the page-3 vectors, the text window and the screen clear
    // that a DOS greeting relies on, and it jumps to $C600 by itself.
    // Poking PC=$C600 unconditionally, as this used to, skips all of that:
    // DOS 3.3's boot1 survives it, DOS 3.2's does not — every .d13 here left
    // the screen on the raw power-on pattern (17 262 lit pixels, byte for
    // byte the same on two different masters) while the same disk booted to
    // `]` in tests/dos32_boot_trace, which lets the ROM start itself.
    //
    // hardReset() has just put PC on the reset vector, so that vector IS the
    // test: an Autostart ROM points it into its own cold-start ($FA62 on the
    // II+), the original Apple ][ monitor (apple2o.rom) points it at $FF59 —
    // no autostart, no boot, and there the poke is the only way in. With no
    // Disk II plugged (capture mode without a selected disk), $C600 holds
    // nothing, so never poke.
    if (diskRaw && controller.cpu().getProgramCounter() >= 0xFF00)
        controller.cpu().setProgramCounter(0xC600);
    controller.setMode(EmulationController::Mode::Running);

    // --type: send every entry whose second has come. pasteKeyStream, not
    // pasteText, so a ^C or ^Z in the script reaches the guest.
    auto typeDue = [&](uint64_t cycles) {
        const int seconds = static_cast<int>(cycles / 1'022'727);
        for (auto& t : typed) {
            if (t.sent || seconds < t.atSecond) continue;
            controller.memory().pasteKeyStream(t.text.data(), t.text.size());
            t.sent = true;
        }
    };

    // Before the worker exists, like the rest of the setup.
    if (trace.mli || trace.bdos || trace.smartPort || trace.pascalBios || trace.monitor || trace.disk) {
        controller.debugger().osCalls().setSink([](const std::string& line) {
            std::fprintf(stderr, "%s\n", line.c_str());
        });
        controller.setOsCallTrace(trace);
    }

    // ─── Capture mode ──────────────────────────────────────────────────────
    // tickFrame() runs one frame's cycles INLINE — the same single-threaded
    // path the WASM build uses. No worker thread and no wall-clock pacing, so
    // the run is deterministic and takes as long as the host needs rather than
    // N/60 seconds: 300 frames land in well under a second on any runner.
    if (captureMode) {
        std::fprintf(stderr, "[POM2 headless] capture: running %d frames\n", frames);
        for (int i = 0; i < frames; ++i) {
            controller.tickFrame();
            typeDue(controller.memory().getCycleCounter());
        }

        Apple2Display display;
        if (controller.memory().isIIE())
            display.setAuxMemory(controller.memory().auxData());
        display.render(controller.memory());
        const int w = display.width();
        const int h = display.height();
        const FrameStats st = frameStats(w, h, display.pixels());
        std::fprintf(stderr,
            "[POM2 headless] capture: %dx%d, %zu colours, %zu/%d pixels differ "
            "from the background\n",
            w, h, st.colours, st.differing, w * h);

        if (textDump) {
            for (int r = 0; r < 24; ++r) {
                const uint16_t base = uint16_t(0x0400 + (r % 8) * 0x80 + (r / 8) * 0x28);
                char line[41];
                for (int c = 0; c < 40; ++c) {
                    const char ch = char(controller.memory().memRead(uint16_t(base + c)) & 0x7F);
                    line[c] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
                }
                line[40] = 0;
                std::printf("%s\n", line);
            }
        }
        if (!shotPath.empty()) {
            if (!writePpm(shotPath, w, h, display.pixels())) {
                std::fprintf(stderr, "ERROR: cannot write %s\n", shotPath.c_str());
                return 1;
            }
            std::fprintf(stderr, "[POM2 headless] wrote %s\n", shotPath.c_str());
        }
        // A single flat colour means the machine never drew anything — a ROM
        // that failed to load, a CPU that never ran, a display that produced
        // nothing. That is precisely the class of failure a packaged binary
        // hits and `--help` sails straight past, so it is an ERROR here, not a
        // remark. Distinct exit code: a CI log should say which check failed.
        if (st.differing == 0) {
            std::fprintf(stderr,
                "ERROR: capture is a single flat colour — the machine did not boot\n");
            return 3;
        }
        return 0;
    }

    controller.start();

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // Boot wait loop. Each iteration sleeps 100 ms, tracks emulated
    // seconds via cycleCounter, and once enough time has elapsed sends
    // the setup paste once (so the SSC slot is wired to BASIC's CSW/KSW).
    bool pasted = !doSetup;
    // Order matters: PR#2 patches the CHARACTER OUTPUT vector (CSWL/CSWH)
    // first, so the BASIC prompt and any echo go straight to the SSC. The
    // following IN#2 patches the INPUT vector (KSWL/KSWH); after that
    // moment, BASIC's GETLN reads from the SSC instead of the keyboard
    // latch — and the rest of our paste queue would be ignored, so the
    // CR after `IN#2` MUST be the last thing in the buffer.
    const std::string setup = unescape(setupOverride.empty()
        ? std::string("PR#2\rIN#2\r")
        : setupOverride);

    std::fprintf(stderr,
        "[POM2 headless] running. telnet 127.0.0.1 %d to interact.\n", port);
    if (doSetup) {
        std::fprintf(stderr,
            "[POM2 headless] setup paste %u chars at t≈%ds.\n",
            static_cast<unsigned>(setup.size()), pasteAfter);
    }

    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // The cycle counter is plain (non-atomic) machine state that the CPU
        // worker writes on every Memory::advanceCycles, so sampling it needs
        // stateMutex — same as MainWindow::renderStatusBar's achieved-clock
        // readout and AiControlServer's /status. Reading it bare here was a
        // real data race (caught by a ThreadSanitizer run of this binary);
        // benign on x86-64, but it is still UB and the wrong example to set.
        // `pasteText` below takes Memory's own kbMutex, so it stays outside.
        uint64_t cycles = 0;
        {
            auto st = controller.lockState();
            cycles = st.memory().getCycleCounter();
        }
        const int  seconds = static_cast<int>(cycles / 1'022'727);
        if (!pasted && seconds >= pasteAfter) {
            std::fprintf(stderr,
                "[POM2 headless] t=%ds — pasting setup.\n", seconds);
            controller.memory().pasteText(setup);
            pasted = true;
        }
        typeDue(cycles);
    }

    std::fprintf(stderr, "[POM2 headless] shutting down.\n");
    controller.stop();
    return 0;
}
