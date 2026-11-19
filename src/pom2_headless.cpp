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
// both modes; `--screen-text` prints Apple text rows after a capture run.

#include "AlsCpmCard.h"
#include "AppleSingle.h"
#include "AtomicFileReplace.h"
#include "AyMusicExport.h"
#include "Cc65DebugInfo.h"
#include "DebuggerSymbols.h"
#include "InstructionTrace.h"
#include "ProfileExport.h"
#include "SoftCardIICard.h"
#include "Apple2Display.h"
#include "TextScreenCapture.h"
#include "ClockCard.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "MediaSource.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "ResourcePaths.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SuperSerialTransport.h"
#include "TestPortCard.h"
#include "Version.h"
#include "WavRecorder.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
        "                        The CPU is an NMOS 6502.\n"
        "  --iie                 //e memory mode, for a 16/32 KiB //e dump; the CPU\n"
        "                        follows the dump (unenhanced NMOS, else 65C02).\n"
        "                        Default ROM then: probe roms/apple2e.rom.\n"
        "  --pal                 The 50 Hz video standard (default NTSC).\n"
        "  --speed <1-4>         The GUI speed menu's x1 / x2 / x4: the frame's\n"
        "                        cycle budget times N (capture mode).\n"
        "  --prom <path>         disk2.rom Disk II PROM.      Default: probe roms/,\n"
        "                        else POM2's own boot PROM (DOS 3.3, ProDOS);\n"
        "                        `--prom pom2` forces POM2's even with a dump\n"
        "  --disk <path>         Disk II image, or a .zip / .gz holding one, or an\n"
        "                        http(s) URL (unpacked / downloaded into the media\n"
        "                        cache). Default: probe disks_5.4/dos33_master.dsk\n"
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
        "  --screen-text         Print Apple text rows (40/80 columns; mixed: bottom 4)\n"
        "  --text-dump           Alias for --screen-text\n"
        "                        To stdout; inverse/flash attributes removed.\n"
        "  --until <addr>        Stop when the CPU is about to execute <addr>\n"
        "                        ($C600, 0xC600 or decimal). The time limit is\n"
        "                        --cycles or --frames; 3600 frames (a minute)\n"
        "                        when neither is given. Exit 4 if it runs out.\n"
        "  --cycles <N>          Run N CPU cycles at most, to the cycle's frame\n"
        "                        slice (a budget: the last instruction may run\n"
        "                        a few cycles past it). Replaces --frames.\n"
        "  --registers           When the run ends, print one line on stdout:\n"
        "                        PC=xxxx A=xx X=xx Y=xx SP=xx P=xx CYCLES=n\n"
        "\n"
        "Exit codes, capture mode: 0 the run ended as asked (frames or cycles\n"
        "  done, or --until reached) · 1 usage or set-up error · 3 the screen\n"
        "  is one flat colour (only without --until/--cycles) · 4 the budget ran\n"
        "  out before --until · 5 the CPU halted (KIL / STP) before the end.\n"
        "  --test-port <slot>    Plug the paravirtual test port (ARCH-017) in that\n"
        "                        slot: $C0n0 log a character (CR/LF ends the line,\n"
        "                        printed on stdout), $C0n1 exit with that status,\n"
        "                        $C0n2 assert (0 = failed: exit 2). Reads are the\n"
        "                        floating bus. Header: tools/testport/pom2_testport.h\n"
        "  --load [addr:]<file>  Load a program into RAM ($0000-$BFFF) once the ROM\n"
        "                        waits for its first key ($FD1B KEYIN), then run it.\n"
        "                        An AppleSingle (cc65's apple2 output) carries its\n"
        "                        own address; a raw binary needs addr. Repeatable.\n"
        "  --run <addr>          Where the loaded program starts. Default: the\n"
        "                        first --load's address.\n"
        "  With --test-port, the exit status is the program's: what it wrote to\n"
        "  $C0n1, or 2 for a failed assertion; 4 if it never exited in the budget\n"
        "  (--frames / --cycles; a minute by default).\n"
        "  --mockingboard <slot> Plug a Mockingboard A/C in that slot.\n"
        "  --vgm <path>          Record the Mockingboard's AY writes, both chips, as\n"
        "                        VGM 1.71 (exact: each write at its 44.1 kHz sample).\n"
        "  --ym <path>           The same, first AY only, as YM5 (one frame a step).\n"
        "  --wav <path>          Record the mixed audio (speaker + Mockingboard) as\n"
        "                        16-bit 44.1 kHz stereo, timed by emulated cycles:\n"
        "                        two runs give the same file.\n"
        "  --trace-file <path>   Write every executed instruction to <path>, one line\n"
        "                        each: PC, physical bank, bytes, disassembly, the\n"
        "                        registers going in, cycles (UI-017).\n"
        "  --trace-format <fmt>  The line, with %%PC %%BANK %%BYTES %%DIS %%A %%X %%Y %%P\n"
        "                        %%SP %%FLAGS %%CYC %%TOTAL %%LABEL %%%% (InstructionTrace.h).\n"
        "  --profile <path>      Profile the run and write a callgrind file for\n"
        "                        KCachegrind / callgrind_annotate: cycles and visits\n"
        "                        per address, grouped by bank and routine.\n"
        "  --symbols <file>      Labels for --profile and %%LABEL: a symbol file\n"
        "                        (name = $0800) or a cc65 .dbg.\n"
        "  With --load, tracing and profiling start when the program does.\n"
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
    int  speedMul    = 1;     // --speed: the GUI speed menu's x1 / x2 / x4
    bool pal         = false; // --pal: the 50 Hz video standard
    bool doSetup     = true;
    bool setupGiven  = false;
    bool textDump    = false;
    bool iie         = false;
    long      untilAddr   = -1;   // --until: stop when PC gets here
    long long cycleBudget = 0;    // --cycles: 0 = no cycle budget
    bool      registers   = false;
    pom2::OsTraceKinds trace;
    int  softcardSlot = 0;   // 0 = none
    int  alsSlot      = 0;   // 0 = none
    int  sc2Slot      = 0;   // 0 = none
    int  testPortSlot = 0;   // --test-port: 0 = none
    // --load: files to put in RAM at the launch point, and --run's address.
    struct LoadSpec { long addr; std::string path; std::vector<uint8_t> bytes; };
    std::vector<LoadSpec> loads;
    long runAddr = -1;
    int  mockingboardSlot = 0;          // --mockingboard: 0 = none
    std::string vgmPath, ymPath, wavPath;
    std::string traceFile, traceFormat, profilePath, symbolsPath;   // UI-017
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
    // An address, the way one is written: $C600, 0xC600 or decimal.
    auto parseAddr = [](std::string text, long& out) -> bool {
        int base = 10;
        if (!text.empty() && text[0] == '$') { text.erase(0, 1); base = 16; }
        else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text.erase(0, 2); base = 16;
        }
        char* end = nullptr;
        const long v = std::strtol(text.c_str(), &end, base);
        if (text.empty() || *end != '\0' || v < 0 || v > 0xFFFF) return false;
        out = v;
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rom" || a == "--prom" || a == "--disk" || a == "--port" ||
            a == "--paste-after" || a == "--setup" || a == "--frames" ||
            a == "--until" || a == "--cycles" ||
            a == "--screenshot" || a == "--softcard" || a == "--type" || a == "--als" ||
            a == "--softcard2" || a == "--speed" || a == "--test-port" ||
            a == "--load" || a == "--run" || a == "--mockingboard" ||
            a == "--vgm" || a == "--ym" || a == "--wav" || a == "--trace-file" ||
            a == "--trace-format" || a == "--profile" || a == "--symbols") {   // --speed: was "unknown arg" (2026-11-17)
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
        else if (a == "--iie") iie = true;
        else if (a == "--pal") pal = true;
        else if (a == "--speed" && i+1 < argc) {
            if (!parseInt("--speed", argv[++i], 1, 4, speedMul)) return 1;
        }
        else if (a == "--text-dump" || a == "--screen-text") textDump = true;
        else if (a == "--trace-mli") trace.mli = true;
        else if (a == "--trace-bdos") trace.bdos = true;
        else if (a == "--trace-sp") trace.smartPort = true;
        else if (a == "--trace-pascal") trace.pascalBios = true;
        else if (a == "--trace-monitor") trace.monitor = true;
        else if (a == "--trace-disk") trace.disk = true;
        else if (a == "--frames" && i+1 < argc) {
            if (!parseInt("--frames", argv[++i], 0, 100000000, frames)) return 1;
        }
        else if (a == "--until" && i+1 < argc) {
            if (!parseAddr(argv[++i], untilAddr)) {
                std::fprintf(stderr, "--until expects an address in $0000..$FFFF, got: %s\n",
                             argv[i]);
                return 1;
            }
        }
        else if (a == "--test-port" && i+1 < argc) {
            if (!parseInt("--test-port", argv[++i], 1, 7, testPortSlot)) return 1;
        }
        else if (a == "--load" && i+1 < argc) {
            // [addr:]path. A path may itself hold a colon (C:\ on Windows), so
            // the prefix only counts when it parses as an address.
            const std::string v = argv[++i];
            LoadSpec spec{ -1, v, {} };
            const auto colon = v.find(':');
            long addr = -1;
            if (colon != std::string::npos && colon > 0
                && parseAddr(v.substr(0, colon), addr)) {
                spec.addr = addr;
                spec.path = v.substr(colon + 1);
            }
            loads.push_back(std::move(spec));
        }
        else if (a == "--mockingboard" && i+1 < argc) {
            if (!parseInt("--mockingboard", argv[++i], 1, 7, mockingboardSlot)) return 1;
        }
        else if (a == "--vgm" && i+1 < argc) vgmPath = argv[++i];
        else if (a == "--ym"  && i+1 < argc) ymPath  = argv[++i];
        else if (a == "--wav" && i+1 < argc) wavPath = argv[++i];
        else if (a == "--trace-file" && i+1 < argc) traceFile = argv[++i];
        else if (a == "--trace-format" && i+1 < argc) traceFormat = argv[++i];
        else if (a == "--profile" && i+1 < argc) profilePath = argv[++i];
        else if (a == "--symbols" && i+1 < argc) symbolsPath = argv[++i];
        else if (a == "--run" && i+1 < argc) {
            if (!parseAddr(argv[++i], runAddr)) {
                std::fprintf(stderr, "--run expects an address in $0000..$FFFF, got: %s\n",
                             argv[i]);
                return 1;
            }
        }
        else if (a == "--cycles" && i+1 < argc) {
            char* end = nullptr;
            errno = 0;   // strtoll saturates at LLONG_MAX: a run that never ends (2026-11-17)
            const long long v = std::strtoll(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v <= 0 || errno == ERANGE) {
                std::fprintf(stderr, "--cycles expects a positive integer, got: %s\n", argv[i]);
                return 1;
            }
            cycleBudget = v;
        }
        else if (a == "--registers") registers = true;
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
    // A stop address, a cycle budget and a register dump are all questions
    // about a run that ENDS, so each of them selects capture mode.
    // A loaded program and the test port are runs that end too.
    const bool launches    = !loads.empty() || runAddr >= 0;
    const bool scripted    = untilAddr >= 0 || cycleBudget > 0 || launches ||
                             testPortSlot != 0;
    const bool recording   = !vgmPath.empty() || !ymPath.empty() || !wavPath.empty() ||
                             !traceFile.empty() || !profilePath.empty();
    // --symbols: read now, so a bad file is a usage error.
    pom2::DebuggerSymbols symbols;
    if (!symbolsPath.empty()) {
        std::ifstream f(symbolsPath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        pom2::cc65dbg::Conversion converted;
        const bool cc65 = pom2::cc65dbg::isDebugFilePath(symbolsPath);
        if (!f.good() && text.empty()) why = "cannot read the file";
        else if (cc65 && !pom2::cc65dbg::toSymbols(text, converted, why)) {}
        else (void)symbols.load(cc65 ? converted.text : text, why);
        if (!why.empty()) { std::fprintf(stderr, "--symbols %s: %s\n", symbolsPath.c_str(), why.c_str()); return 1; }
        std::fprintf(stderr, "[POM2 headless] %zu symbols from %s\n", symbols.size(), symbolsPath.c_str());
    }
    const bool captureMode = frames > 0 || !shotPath.empty() || textDump ||
                             scripted || registers || recording;
    if ((!vgmPath.empty() || !ymPath.empty()) && !mockingboardSlot) {
        std::fprintf(stderr, "--vgm / --ym record a Mockingboard: add --mockingboard <slot>\n");
        return 1;
    }
    // ~5 s, past the boot beep — or a minute when a stop address or a test
    // program is the point and no limit was given: a disk boot takes longer
    // than a banner.
    if (captureMode && frames <= 0)
        frames = (untilAddr >= 0 || launches || testPortSlot) ? 3600 : 300;

    // --load: read every file now, so a bad one is a usage error before the
    // machine exists. An AppleSingle says where it goes (its ProDOS aux type,
    // which is what cc65's apple2 target writes); anything else needs addr.
    for (auto& l : loads) {
        std::ifstream f(l.path, std::ios::binary);
        if (!f) { std::fprintf(stderr, "--load: cannot open %s\n", l.path.c_str()); return 1; }
        std::vector<uint8_t> bytes(0x10001);
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(f.gcount()));
        if (pom2::applesingle::looksLikeAppleSingle(bytes.data(), bytes.size())) {
            pom2::applesingle::File as;
            std::string why;
            if (!pom2::applesingle::decode(bytes, as, why)) {
                std::fprintf(stderr, "--load %s: %s\n", l.path.c_str(), why.c_str());
                return 1;
            }
            if (l.addr < 0) {
                if (!as.hasProdosInfo) {
                    std::fprintf(stderr, "--load %s: an AppleSingle with no ProDOS "
                                 "file info carries no address; write addr:%s\n",
                                 l.path.c_str(), l.path.c_str());
                    return 1;
                }
                l.addr = static_cast<long>(as.auxType & 0xFFFF);
            }
            bytes = std::move(as.data);
        }
        if (l.addr < 0) {
            std::fprintf(stderr, "--load %s: a raw binary needs an address "
                         "(--load addr:%s)\n", l.path.c_str(), l.path.c_str());
            return 1;
        }
        // RAM only: $C000-$CFFF is the I/O page and the ROM sits above it.
        if (bytes.empty() || l.addr + static_cast<long>(bytes.size()) > 0xC000) {
            std::fprintf(stderr, "--load %s: %zu bytes at $%04lX do not fit in "
                         "$0000-$BFFF\n", l.path.c_str(), bytes.size(), l.addr);
            return 1;
        }
        l.bytes = std::move(bytes);
    }
    if (runAddr < 0 && !loads.empty()) runAddr = loads.front().addr;

    const std::string romPath = !romArg.empty() ? romArg : iie
        ? findAsset("roms/apple2e.rom", { "../roms/apple2e.rom", "../../roms/apple2e.rom" })
        : findAsset("roms/apple2.rom", { "../roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = promArg == "pom2" ? std::string()
        : !promArg.empty() ? promArg : findAsset(
        "roms/disk2.rom", { "../roms/disk2.rom", "../../roms/disk2.rom" });
    // This legacy DOS master probe resolves only in a development library.
    // Packages ship named curated images under bundled_disks/ instead;
    // pass --disk explicitly to select one of those for this frontend.
    std::string diskPath = !diskArg.empty() ? diskArg : findAsset(
        "disks_5.4/dos33_master.dsk", { "../disks_5.4/dos33_master.dsk",
                                        "../../disks_5.4/dos33_master.dsk" });

    if (romPath.empty()) {
        std::fprintf(stderr, "missing %s; try --help\n", iie ? "apple2e.rom" : "apple2.rom");
        return 1;
    }
    // A .zip, a .gz or a URL: its local image, as the GUI's positional does
    // (MediaSource.h). It used to reach insertDisk raw and be refused by size.
    if (!diskPath.empty() && pom2::needsMediaResolve(diskPath)) {
        const auto got = pom2::resolveMediaSource(diskPath, pom2::mediaCacheDir());
        if (!got.ok) {
            std::fprintf(stderr, "--disk %s: %s\n", diskPath.c_str(), got.error.c_str());
            return 1;
        }
        std::fprintf(stderr, "[POM2 headless] %s: %s -> %s\n", diskPath.c_str(),
                     got.note.c_str(), got.localPath.c_str());
        diskPath = got.localPath;
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

    // II/II+ mode is enough for DOS 3.3, so that stays the default; --rom changes
    // the file, not the machine's memory mode. --iie changes the mode.
    // --wav mixes OFFLINE: no host device, the sample count follows the
    // emulated cycles, so the file does not depend on the host's timing.
    EmulationController controller(wavPath.empty() ? AudioDevice::OutputMode::Host
                                                   : AudioDevice::OutputMode::Offline);
    // Everything from here to `controller.start()` below runs before the
    // CPU worker exists, so the raw accessor is correct: there is no second
    // thread to race. The one access inside the run loop takes lockState().
    // setIIEMode BEFORE loadAppleIIRom: the loader only splits a //e dump into
    // the internal $C100-$CFFF ROM when the mode is already on (pom2_bench,
    // MainWindow::applyProfile).
    controller.memory().setIIEMode(iie);
    if (!controller.memory().loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }
    // The core powers up as a 65C02, which a ][, a ][+ and an unenhanced //e are
    // not. A //e dump says which it is: [C] $FBC0 is $EA in apple2e_unenh.rom,
    // $E0 in apple2e.rom.
    if (!iie || controller.memory().peekCpuView(0xFBC0) == 0xEA)
        controller.cpu().setCpuMode(M6502::CpuMode::NMOS);

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
    // A card asked for by name takes the slot: the clock is the default only.
    if (z80Slot != ClockCard::kDefaultSlot && mockingboardSlot != ClockCard::kDefaultSlot
        && testPortSlot != ClockCard::kDefaultSlot)
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

    MockingboardCard* mockingboard = nullptr;
    if (mockingboardSlot) {
        if (const SlotPeripheral* there = controller.memory().slotBus().peripheral(mockingboardSlot)) {
            std::fprintf(stderr, "--mockingboard: slot %d holds the %s\n", mockingboardSlot,
                         std::string(there->name()).c_str());
            return 1;
        }
        auto mb = std::make_unique<MockingboardCard>(mockingboardSlot);
        mb->setCpu(&controller.cpu());
        mb->setSampleRate(controller.audio().getActualSampleRate());
        mockingboard = mb.get();
        controller.memory().slotBus().plug(mockingboardSlot, std::move(mb));
    }
    // An offline device takes no source by itself (EmulationController):
    // the speaker and the Mockingboard are what the WAV hears.
    if (!wavPath.empty()) {
        controller.audio().addSource(&controller.speaker());
        if (mockingboard) controller.audio().addSource(mockingboard->audioSource());
    }

    // --test-port: last, into a slot nothing else took.
    pom2::TestPortCard* testPort = nullptr;
    if (testPortSlot) {
        if (const SlotPeripheral* there = controller.memory().slotBus().peripheral(testPortSlot)) {
            std::fprintf(stderr, "--test-port: slot %d holds the %s\n", testPortSlot,
                         std::string(there->name()).c_str());
            return 1;
        }
        auto tp = std::make_unique<pom2::TestPortCard>(testPortSlot);
        // stdout, a line at a time, flushed: the program's own words, kept
        // apart from this tool's [POM2 headless] chatter on stderr.
        tp->setLineSink([](const std::string& line) {
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        });
        testPort = tp.get();
        controller.memory().slotBus().plug(testPortSlot, std::move(tp));
    }

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
        // With --load / --run, --until is a question about the PROGRAM: it is
        // armed when the program starts (launch() below), or the ROM's own
        // boot — COUT at $FDED, say — stopped the run before it began and
        // blamed the launch (bug hunt 2026-11-19).
        if (untilAddr >= 0 && runAddr < 0) {
            // The run-control debugger is what stops a machine at an address;
            // arming it puts the CPU on its debugged loop for this run.
            auto st = controller.lockState();
            controller.debugger().addBreakpoint(static_cast<uint16_t>(untilAddr));
            controller.syncDebugHook();
        }
        // --load / --run: the program goes in once the ROM has finished its
        // cold start and waits for a key — [C] every Apple II Monitor reads
        // the keyboard through KSW, which a cold start points at KEYIN $FD1B.
        // Not at reset: the ROM's cold start (and a disk's boot sector at
        // $0800) would overwrite a program loaded before it, and the program
        // finds the machine a user would — screen, vectors, DOS if booted.
        // --trace-file / --profile: armed at the start of the run, or at the
        // launch of a --load program (the ROM's boot is not the question).
        pom2::InstructionTrace trace;
        trace.setSymbols(symbols);
        // The run is already bounded by --frames / --cycles: no line cap
        // here, the user asked for the whole of it.
        trace.setMaxLines(0);
        bool instrumented = false;
        auto instrument = [&]() -> bool {
            if (instrumented || (traceFile.empty() && profilePath.empty())) return true;
            auto st = controller.lockState();
            if (!traceFile.empty()) {
                std::string why;
                if (!trace.open(std::filesystem::u8path(traceFile), traceFormat, st.cpu(),
                                st.memory(), why)) {
                    std::fprintf(stderr, "--trace-file: %s\n", why.c_str());
                    return false;
                }
                controller.debugger().setInstructionTrace(&trace);
            }
            if (!profilePath.empty()) {
                controller.debugger().clearMeasurements();
                controller.debugger().setProfiling(true);
            }
            controller.syncDebugHook();
            instrumented = true;
            return true;
        };
        constexpr uint16_t kLaunchAt = 0xFD1B;
        bool launchPending = runAddr >= 0;
        bool instrumentFailed = false;
        if (launchPending) {
            auto st = controller.lockState();
            controller.debugger().addBreakpoint(kLaunchAt);
            controller.syncDebugHook();
        }
        auto launch = [&] {
            {
                auto st = controller.lockState();
                if (untilAddr != kLaunchAt) controller.debugger().removeBreakpoint(kLaunchAt);
                controller.debugger().clearHit();
                controller.syncDebugHook();
                for (const auto& l : loads)
                    for (std::size_t k = 0; k < l.bytes.size(); ++k)
                        st.memory().writeRamUnchecked(static_cast<uint16_t>(l.addr + static_cast<long>(k)),
                                                      l.bytes[k]);
                st.cpu().setProgramCounter(static_cast<uint16_t>(runAddr));
                if (untilAddr >= 0) {
                    controller.debugger().addBreakpoint(static_cast<uint16_t>(untilAddr));
                    controller.syncDebugHook();
                }
                std::fprintf(stderr, "[POM2 headless] launched $%04lX after %llu cycles\n", runAddr,
                             static_cast<unsigned long long>(st.memory().getCycleCounter()));
                launchPending = false;
            }
            // A trace file that cannot be created ends the run as a usage
            // error, as it does without --load — it used to be ignored and
            // took --profile down with it (bug hunt 2026-11-19).
            if (!instrument()) instrumentFailed = true;   // takes the lock itself
            // The breakpoint parked the machine (noteDebuggerStop); resume it.
            controller.setMode(EmulationController::Mode::Running);
        };
        if (cycleBudget > 0)
            std::fprintf(stderr, "[POM2 headless] capture: running %lld cycles\n", cycleBudget);
        else
            std::fprintf(stderr, "[POM2 headless] capture: running %d frames\n", frames);

        // The video standard, as the GUI's applyProfile always sets it, with
        // the cards plugged: it is what hands every card the crystal clock
        // (setStandardClock). Without it the Disk II never had one, so this
        // tool could not see a clock-ratio defect the GUI showed (STORAGE-015).
        controller.setVideoStandard(pal ? VideoStandard::PAL : VideoStandard::NTSC);
        controller.setCyclesPerFrame(pom2VideoTiming(controller.getVideoStandard()).cyclesPerFrame);
        // --speed: exactly what the GUI's speed menu does (MainWindow_Chrome
        // speed.2x / speed.4x): the frame's cycle budget times N.
        if (speedMul > 1)
            controller.setCyclesPerFrame(controller.getCyclesPerFrame() * speedMul);
        const uint64_t startCycles = controller.memory().getCycleCounter();
        const int      frameCycles = controller.getCyclesPerFrame();
        if (!launchPending && !instrument()) return 1;

        if (mockingboard && (!vgmPath.empty() || !ymPath.empty())) {
            auto st = controller.lockState();
            mockingboard->startAyCapture(pom2VideoTiming(controller.getVideoStandard()).cyclesPerFrame);
        }
        pom2::WavFileWriter wav;
        if (!wavPath.empty()) {
            std::string why;
            if (!wav.open(std::filesystem::u8path(wavPath), controller.audio().getActualSampleRate(),
                          AudioDevice::kChannels, why)) {
                std::fprintf(stderr, "--wav: %s\n", why.c_str());
                return 1;
            }
        }
        // Samples owed = the emulated time run so far at the device rate,
        // mixed after every frame: the cycles decide, not the host clock.
        // The clock the speaker is GIVEN once the run starts (the standard's,
        // times the speed menu) — not emulatedCpuClockHz() here, which before
        // the first frame still derives from the NTSC frame budget: under
        // --pal or --speed the WAV came out too long and slow (bug hunt
        // 2026-11-19).
        const double wavClock = pom2VideoTiming(controller.getVideoStandard()).cpuClockHz * speedMul;
        uint64_t wavMixed = 0;
        std::vector<float> wavBuf;
        auto mixWavTo = [&](uint64_t cyclesRun) {
            if (!wav.isOpen() || wavClock <= 0.0) return;
            const uint64_t due = static_cast<uint64_t>(
                static_cast<double>(cyclesRun) * wav.sampleRate() / wavClock);
            if (due <= wavMixed) return;
            const int n = static_cast<int>(due - wavMixed);
            wavBuf.assign(static_cast<std::size_t>(n) * AudioDevice::kChannels, 0.0f);
            controller.audio().mixSources(wavBuf.data(), n);
            wav.write(wavBuf.data(), static_cast<std::size_t>(n));
            wavMixed = due;
        };
        bool reached = false, halted = false;
        for (int i = 0;; ++i) {
            if (cycleBudget > 0) {
                const long long used = static_cast<long long>(
                    controller.memory().getCycleCounter() - startCycles);
                if (used >= cycleBudget) break;
                // The last slice is cut to what is left of the budget.
                const long long left = cycleBudget - used;
                controller.setCyclesPerFrame(left < frameCycles ? static_cast<int>(left)
                                                                : frameCycles);
            } else if (i >= frames) {
                break;
            }
            controller.tickFrame();
            mixWavTo(controller.memory().getCycleCounter() - startCycles);
            typeDue(controller.memory().getCycleCounter());
            // The test port ended the run: the program's verdict is final,
            // whatever it does next.
            if (testPort && testPort->finished()) break;
            if (launchPending && controller.debugger().lastHit().valid()
                && controller.debugger().lastHit().pc == kLaunchAt) {
                launch();
                if (instrumentFailed) break;
                continue;
            }
            if (untilAddr >= 0 && controller.debugger().lastHit().valid()) {
                reached = true;
                break;
            }
            // KIL or STP: nothing short of RESET moves this CPU again, so the
            // rest of the budget would be spent learning nothing.
            if (controller.cpu().haltCode() == 1) { halted = true; break; }
        }
        controller.setCyclesPerFrame(frameCycles);
        if (instrumentFailed) return 1;

        const M6502& cpu = controller.cpu();
        const unsigned long long ran = static_cast<unsigned long long>(
            controller.memory().getCycleCounter() - startCycles);
        if (registers)
            std::printf("PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X CYCLES=%llu\n",
                        cpu.getProgramCounter(), cpu.getAccumulator(), cpu.getXRegister(),
                        cpu.getYRegister(), cpu.getStackPointer(), cpu.getStatusRegister(),
                        ran);
        if (instrumented) {
            std::vector<pom2::Debugger::ProfileEntry> profile;
            {
                auto st = controller.lockState();
                controller.debugger().setInstructionTrace(nullptr);
                if (!profilePath.empty()) {
                    profile = controller.debugger().profile(SIZE_MAX);
                    controller.debugger().setProfiling(false);
                }
                controller.syncDebugHook();
            }
            if (trace.isOpen()) {
                const unsigned long long lines = trace.lines();
                const bool capped = trace.stopped() && trace.stopReason().find("cap") != std::string::npos;
                if (!trace.close()) {
                    std::fprintf(stderr, "ERROR: %s: the trace could not be written in full "
                                 "(%s)\n", traceFile.c_str(), "disk full or the file went away");
                    return 1;
                }
                std::fprintf(stderr, "[POM2 headless] wrote %s (%llu lines%s)\n", traceFile.c_str(),
                             lines, capped ? ", stopped at its cap" : "");
            }
            if (!profilePath.empty()) {
                const std::string text = pom2::callgrindProfile(profile, symbols.size() ? &symbols : nullptr,
                                                                "pom2_headless");
                std::error_code ec;
                if (!pom2::writeFileAtomic(std::filesystem::u8path(profilePath), text.data(),
                                           text.size(), ec)) {
                    std::fprintf(stderr, "ERROR: cannot write %s: %s\n", profilePath.c_str(),
                                 ec.message().c_str());
                    return 1;
                }
                std::fprintf(stderr, "[POM2 headless] wrote %s (%zu addresses)\n",
                             profilePath.c_str(), profile.size());
            }
        }
        if (mockingboard && (!vgmPath.empty() || !ymPath.empty())) {
            pom2::aymusic::Capture cap;
            {
                auto st = controller.lockState();
                cap = mockingboard->stopAyCapture();
            }
            const auto save = [](const std::string& path, const std::vector<uint8_t>& bytes,
                                 const char* what, std::size_t writes) {
                std::error_code ec;
                if (!pom2::writeFileAtomic(std::filesystem::u8path(path), bytes.data(),
                                           bytes.size(), ec)) {
                    std::fprintf(stderr, "ERROR: cannot write %s: %s\n", path.c_str(),
                                 ec.message().c_str());
                    return false;
                }
                std::fprintf(stderr, "[POM2 headless] wrote %s (%s, %zu AY writes)\n",
                             path.c_str(), what, writes);
                return true;
            };
            if (!vgmPath.empty() && !save(vgmPath, pom2::aymusic::encodeVgm(cap), "VGM", cap.writes.size()))
                return 1;
            if (!ymPath.empty() && !save(ymPath, pom2::aymusic::encodeYm5(cap, 0), "YM5", cap.writes.size()))
                return 1;
        }
        if (wav.isOpen()) {
            std::string why;
            const uint64_t got = wav.framesWritten();
            if (!wav.close(why)) { std::fprintf(stderr, "ERROR: --wav: %s\n", why.c_str()); return 1; }
            std::fprintf(stderr, "[POM2 headless] wrote %s (%llu frames)\n", wavPath.c_str(),
                         static_cast<unsigned long long>(got));
        }

        int scriptedExit = 0;
        if (testPort && testPort->finished()) {
            std::fprintf(stderr, "[POM2 headless] test port: exit %d after %llu cycles "
                         "(%d assertion(s) passed)\n", testPort->exitStatus(), ran,
                         testPort->assertionsPassed());
            scriptedExit = testPort->exitStatus();
        } else if (launchPending) {
            std::fprintf(stderr, "ERROR: the ROM never waited for a key at $%04X in %llu "
                         "cycles, so the program was not launched\n", kLaunchAt, ran);
            scriptedExit = 4;
        } else if (halted) {
            std::fprintf(stderr, "ERROR: the CPU halted at $%04X after %llu cycles\n",
                         cpu.getProgramCounter(), ran);
            scriptedExit = 5;
        } else if (untilAddr >= 0 && !reached) {
            std::fprintf(stderr,
                "ERROR: $%04lX was not reached in %llu cycles (PC=$%04X)\n",
                untilAddr, ran, cpu.getProgramCounter());
            scriptedExit = 4;
        } else if (untilAddr >= 0) {
            std::fprintf(stderr, "[POM2 headless] reached $%04lX after %llu cycles\n",
                         untilAddr, ran);
        } else if (testPort) {
            std::fprintf(stderr, "ERROR: the program did not exit through the test port "
                         "in %llu cycles (PC=$%04X)\n", ran, cpu.getProgramCounter());
            scriptedExit = 4;
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
            const auto captured = pom2::captureTextScreen(controller.memory());
            std::fwrite(captured.text.data(), 1, captured.text.size(), stdout);
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
        //
        // Not when the run was scripted: a program stopped at an address, or
        // after a counted number of cycles, need not have drawn anything, and
        // the caller asked a different question.
        if (scripted) return scriptedExit;
        if (halted) return scriptedExit;
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
