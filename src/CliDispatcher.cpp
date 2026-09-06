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

// CliDispatcher.cpp — verb parser + Phase-C runner. Inspired by POM1's
// CliDispatcher with the POM1-card-specific verbs (sid, jukebox, codetank,
// microsd, tms9918, …) removed.

#include "CliDispatcher.h"

#include "CpuClock.h"
#include "Logger.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace pom2 {
namespace {

bool endsWithIcase(const std::string& s, std::string_view suffix)
{
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i]))
            != std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
    }
    return true;
}

/// Parse a 16-bit address. Apple II convention: addresses are HEX.
/// "$0300", "0x0300", and bare "0300"/"2000" all parse as hex ($0300 /
/// $2000). An optional "$" or "0x"/"0X" prefix is accepted and stripped.
/// The ENTIRE token must be valid hex (no trailing garbage, no leading
/// whitespace/sign) or it's a parse error. Range $0000-$FFFF.
/// (Decimal input is intentionally NOT supported — bare tokens are hex so
/// "--run 2000" means $2000, not 2000 decimal.)
bool parseAddr16(const std::string& s, int& out)
{
    if (s.empty()) return false;
    std::string v = s;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.erase(0, 2);
    } else if (v[0] == '$') {
        v.erase(0, 1);
    }
    // Require the first remaining char to be a hex digit so std::stol can't
    // silently skip leading whitespace / accept a sign.
    if (v.empty() || !std::isxdigit(static_cast<unsigned char>(v[0]))) return false;
    try {
        size_t idx = 0;
        long n = std::stol(v, &idx, 16);
        if (idx != v.size()) return false;     // reject trailing garbage
        if (n < 0 || n > 0xFFFF) return false;
        out = static_cast<int>(n);
        return true;
    } catch (...) { return false; }
}

bool parseIntPositive(const std::string& s, int& out)
{
    try {
        size_t idx = 0;
        long n = std::stol(s, &idx, 10);
        // Reject trailing junk, negatives, AND values that would overflow int
        // (std::stol caps at LONG_MAX; the cast below would wrap a huge value
        // to a bogus-but-positive cycles/frame or step count).
        if (idx != s.size() || n < 0 || n > INT_MAX) return false;
        out = static_cast<int>(n);
        return true;
    } catch (...) { return false; }
}

bool splitOnColon(const std::string& s, std::string& l, std::string& r)
{
    auto pos = s.find(':');
    if (pos == std::string::npos) return false;
    l = s.substr(0, pos);
    r = s.substr(pos + 1);
    return !l.empty() && !r.empty();
}

bool parsePresetName(const std::string& raw, CliPreset& out)
{
    std::string s = raw;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "ii"  || s == "apple2"  || s == "appleii")  { out = CliPreset::AppleII;     return true; }
    if (s == "ii+" || s == "iiplus" || s == "apple2plus" ||
        s == "appleiiplus" || s == "ii-plus") { out = CliPreset::AppleIIPlus; return true; }
    if (s == "iie-u" || s == "iie-unenh" || s == "iie-unenhanced" ||
        s == "iieunenhanced" || s == "apple2e-1983" || s == "//e-u")
                                            { out = CliPreset::AppleIIeUnenhanced; return true; }
    if (s == "iie" || s == "apple2e" || s == "appleiie" ||
        s == "//e" || s == "iie-enhanced")  { out = CliPreset::AppleIIe;    return true; }
    if (s == "iic" || s == "apple2c" || s == "appleiic" ||
        s == "//c")                         { out = CliPreset::AppleIIc;    return true; }
    if (s == "iic+" || s == "iicplus" || s == "apple2cplus" ||
        s == "apple2cp" || s == "appleiicplus" ||
        s == "//c+")                        { out = CliPreset::AppleIIcPlus; return true; }
    if (s == "iie-u-pal" || s == "iieupal" || s == "iie-unenh-pal" ||
        s == "//e-u-pal" || s == "apple2e-1983-pal" ||
        s == "frenchtouch")                 { out = CliPreset::AppleIIeUnenhancedPAL; return true; }
    if (s == "iie-pal" || s == "iiepal" || s == "apple2e-pal" ||
        s == "//e-pal")                     { out = CliPreset::AppleIIePAL; return true; }
    if (s == "iic-pal" || s == "iicpal" || s == "apple2c-pal" ||
        s == "//c-pal" || s == "chatmauve") { out = CliPreset::AppleIIcPAL; return true; }
    return false;
}

void printUsage()
{
    std::fprintf(stderr,
        "Usage: POM2 [options] [disk-image]\n"
        "\n"
        "  disk-image                 Mount + boot a disk (.dsk/.do/.po/.nib/\n"
        "                              .woz/.d13/.hdv/.2mg). Slot is auto-picked\n"
        "                              from type. Boots under the saved profile.\n"
        "                              May be a TNFS URL — tnfs://host[:port]/\n"
        "                              path/image.po — which is fetched into a\n"
        "                              local cache and then booted like any\n"
        "                              other image. Cached by host+path, so a\n"
        "                              second run needs no network.\n"
        "  --kiosk                    Full-screen, no menus/panels — just the\n"
        "                              screen. Implies booting [disk-image].\n"
        "                              Ctrl+Alt+F (or F10) or the in-kiosk\n"
        "                              menu exits to the windowed GUI;\n"
        "                              Alt-F4 quits.\n"
        "  --prodos-folder <dir>      Serve a host directory as a ProDOS\n"
        "                              volume and boot it (see the ProDOS\n"
        "                              host-folder card).\n"
        "  --fujinet[=PORT]           Plug a FujiNet relay card and listen on\n"
        "                             127.0.0.1:PORT (default 1985) for a\n"
        "                             FujiNet desktop build to connect.\n"
        "  --fujinet-serial[=DEV]     Same, but talk to a physical FujiNet\n"
        "                             board over USB CDC-ACM. DEV omitted =\n"
        "                             auto-pick when exactly one is present.\n"
        "  --fujinet-slot N           Which slot the card goes in. Without it,\n"
        "                             slot 7 is preferred and POM2 falls back\n"
        "                             to the first free slot if 7 is taken.\n"
        "\n"
        "Phase-A boot options (consumed before MainWindow starts):\n"
        "  -p, --preset <ii|ii+|iie-u|iie|iic|iic+|iie-u-pal|iie-pal|iic-pal>\n"
        "                             System profile to boot into\n"
        "  --ii-plus, --ii+           Force II+ mode (ignore roms/apple2e.rom)\n"
        "  --speed <cycles/frame>     Override CPU pacing (1x = 17045)\n"
        "  --cpu-max                  Run flat-out (~58 MHz emulated)\n"
        "  --ai-control[=PORT]        Start loopback control + screen capture server\n"
        "  --tape <path>              Preload + auto-play tape\n"
        "  --35-disk1 <path>          Mount 800K 3.5\" image in //c+ internal drive\n"
        "  --35-disk2 <path>          Mount 800K 3.5\" image in //c+ external drive\n"
        "  --save-tape <path>         Dump captured tape on shutdown\n"
        "  --save-tape-format <aci|wav>   Force save extension\n"
        "  --snapshot-load <path>     Restore state at boot (Phase C)\n"
        "  --display <mode>           Hi-res render mode at boot. mode = ntsc | chatmauve\n"
        "                              | mono-white | mono-green | mono-amber\n"
        "  --rgb-card-invert-bit7[=on|off]\n"
        "                              Le Chat Mauve / Video-7 Dragon-Wars compat:\n"
        "                              XOR bit 7 of HGR / DHGR-Mixed bytes at decode.\n"
        "\n"
        "Phase-C deferred (run in CLI order after a short settle):\n"
        "  --load <addr>:<path>       Load raw binary at addr\n"
        "  --run <addr>               Jump to addr\n"
        "  --paste <file>             Feed file contents to keyboard (<=4096)\n"
        "  --step <N>                 Single-step N instructions\n"
        "  --trace-brk                (accepted, not yet wired in M6502)\n"
        "  --play / --rec / --rewind  Cassette transport\n"
        "  --snapshot-save <path>     Write snapshot\n"
        "\n"
        "  -h, --help                 Show this and exit\n");
}

} // namespace

std::string resolveSaveTapePath(const std::string& path, CliSaveTapeFormat hint)
{
    if (path.empty()) return path;
    const bool hasAci = endsWithIcase(path, ".aci");
    const bool hasWav = endsWithIcase(path, ".wav");
    if (hasAci || hasWav) return path;
    switch (hint) {
        case CliSaveTapeFormat::Aci: return path + ".aci";
        case CliSaveTapeFormat::Wav: return path + ".wav";
        case CliSaveTapeFormat::NoHint: default: return path + ".aci";
    }
}

std::optional<CliPlan> parseCli(int argc, char* argv[], bool& helpRequestedOut)
{
    helpRequestedOut = false;
    CliPlan plan;

    auto needArg = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            pom2::log().error("CLI", std::string(flag) + " requires an argument");
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            printUsage();
            helpRequestedOut = true;
            return std::nullopt;
        }
        if (a == "-p" || a == "--preset") {
            const char* v = needArg(i, "--preset"); if (!v) return std::nullopt;
            if (!parsePresetName(v, plan.preset)) {
                pom2::log().error("CLI", std::string("unknown preset: ") + v);
                return std::nullopt;
            }
        }
        else if (a == "--cpu-max") {
            plan.cpuMax = true;
        }
        else if (a == "--ai-control" || a.rfind("--ai-control=", 0) == 0) {
            plan.aiControl = true;
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                const int p = std::atoi(a.c_str() + eq + 1);
                if (p <= 0 || p > 65535) {
                    pom2::log().error("CLI", "--ai-control port out of range");
                    return std::nullopt;
                }
                plan.aiControlPort = p;
            }
        }
        else if (a == "--ii-plus" || a == "--ii+") {
            plan.forceIIPlus = true;
        }
        else if (a == "--kiosk") {
            plan.kiosk = true;
        }
        else if (a == "--prodos-folder") {
            const char* v = needArg(i, "--prodos-folder"); if (!v) return std::nullopt;
            plan.prodosFolderPath = v;
        }
        else if (a == "--fujinet" || a.rfind("--fujinet=", 0) == 0) {
            plan.fujiNet = CliPlan::FujiNetTransport::Tcp;
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                const int p = std::atoi(a.c_str() + eq + 1);
                if (p <= 0 || p > 65535) {
                    pom2::log().error("CLI", "--fujinet: port out of range: " +
                                                 a.substr(eq + 1));
                    return std::nullopt;
                }
                plan.fujiNetPort = p;
            }
        }
        else if (a == "--fujinet-serial" || a.rfind("--fujinet-serial=", 0) == 0) {
            plan.fujiNet = CliPlan::FujiNetTransport::Serial;
            const auto eq = a.find('=');
            if (eq != std::string::npos) plan.fujiNetSerialPath = a.substr(eq + 1);
        }
        else if (a == "--fujinet-slot") {
            const char* v = needArg(i, "--fujinet-slot"); if (!v) return std::nullopt;
            const int s = std::atoi(v);
            if (s < 1 || s > 7) {
                pom2::log().error("CLI", std::string("--fujinet-slot must be 1-7, got ") + v);
                return std::nullopt;
            }
            plan.fujiNetSlot         = s;
            plan.fujiNetSlotExplicit = true;
        }
        else if (a == "--display") {
            const char* v = needArg(i, "--display"); if (!v) return std::nullopt;
            const std::string s = v;
            if      (s == "ntsc"        || s == "color-ntsc")  plan.displayMode = CliDisplayMode::ColorNTSC;
            else if (s == "chatmauve"   || s == "chat-mauve"
                  || s == "rgb"         || s == "le-chat-mauve") plan.displayMode = CliDisplayMode::ChatMauveRGB;
            else if (s == "mono-white"  || s == "white")       plan.displayMode = CliDisplayMode::MonoWhite;
            else if (s == "mono-green"  || s == "green"
                  || s == "p31")                                plan.displayMode = CliDisplayMode::MonoGreen;
            else if (s == "mono-amber"  || s == "amber")       plan.displayMode = CliDisplayMode::MonoAmber;
            else {
                pom2::log().error("CLI", std::string("unknown --display mode: ") + v
                    + " (expected ntsc|chatmauve|mono-white|mono-green|mono-amber)");
                return std::nullopt;
            }
        }
        else if (a == "--speed") {
            const char* v = needArg(i, "--speed"); if (!v) return std::nullopt;
            int n; if (!parseIntPositive(v, n) || n <= 0) {
                pom2::log().error("CLI", std::string("invalid --speed: ") + v);
                return std::nullopt;
            }
            // Shared ceiling with the AI server's POST /speed —
            // POM2_MAX_CYCLES_PER_FRAME in CpuClock.h (rationale there).
            // Clamp + warn rather than reject so existing scripted
            // launches keep working.
            if (n > POM2_MAX_CYCLES_PER_FRAME) {
                pom2::log().warn("CLI",
                    "--speed " + std::to_string(n) + " exceeds the " +
                    std::to_string(POM2_MAX_CYCLES_PER_FRAME) +
                    " cycles/frame ceiling — clamped");
                n = POM2_MAX_CYCLES_PER_FRAME;
            }
            plan.executionSpeed = n;
        }
        else if (a == "--rgb-card-invert-bit7"
              || a.rfind("--rgb-card-invert-bit7=", 0) == 0) {
            // Bare flag = enable; `=on`/`=off`/`=true`/`=false`/`=1`/`=0` honoured.
            const auto eq = a.find('=');
            if (eq == std::string::npos) {
                plan.rgbCardInvertBit7 = true;
            } else {
                std::string v = a.substr(eq + 1);
                for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if      (v == "on"  || v == "true"  || v == "1") plan.rgbCardInvertBit7 = true;
                else if (v == "off" || v == "false" || v == "0") plan.rgbCardInvertBit7 = false;
                else {
                    pom2::log().error("CLI", "--rgb-card-invert-bit7 expects on|off, got: " + v);
                    return std::nullopt;
                }
            }
        }
        else if (a == "--35-disk1") {
            const char* v = needArg(i, "--35-disk1"); if (!v) return std::nullopt;
            plan.disk35Internal = v;
        }
        else if (a == "--35-disk2") {
            const char* v = needArg(i, "--35-disk2"); if (!v) return std::nullopt;
            plan.disk35External = v;
        }
        else if (a == "--tape") {
            const char* v = needArg(i, "--tape"); if (!v) return std::nullopt;
            plan.initialTapePath = v;
            plan.initialTapeAutoPlay = true;
        }
        else if (a == "--save-tape") {
            const char* v = needArg(i, "--save-tape"); if (!v) return std::nullopt;
            plan.saveTapePath = v;
        }
        else if (a == "--save-tape-format") {
            const char* v = needArg(i, "--save-tape-format"); if (!v) return std::nullopt;
            std::string s = v;
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if      (s == "aci") plan.saveTapeFormat = CliSaveTapeFormat::Aci;
            else if (s == "wav") plan.saveTapeFormat = CliSaveTapeFormat::Wav;
            else {
                pom2::log().error("CLI", std::string("--save-tape-format expects aci|wav, got: ") + v);
                return std::nullopt;
            }
        }
        else if (a == "--load") {
            const char* v = needArg(i, "--load"); if (!v) return std::nullopt;
            std::string addrStr, path;
            if (!splitOnColon(v, addrStr, path)) {
                pom2::log().error("CLI", std::string("--load expects ADDR:PATH, got: ") + v);
                return std::nullopt;
            }
            int addr;
            if (!parseAddr16(addrStr, addr)) {
                pom2::log().error("CLI", std::string("--load address parse failed: ") + addrStr);
                return std::nullopt;
            }
            CliAction act{};
            act.kind = CliAction::Kind::Load;
            act.addressI = addr;
            act.pathS = path;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--run") {
            const char* v = needArg(i, "--run"); if (!v) return std::nullopt;
            int addr;
            if (!parseAddr16(v, addr)) {
                pom2::log().error("CLI", std::string("--run address parse failed: ") + v);
                return std::nullopt;
            }
            CliAction act{}; act.kind = CliAction::Kind::Run; act.addressI = addr;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--paste") {
            const char* v = needArg(i, "--paste"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::Paste; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--step") {
            const char* v = needArg(i, "--step"); if (!v) return std::nullopt;
            int n;
            if (!parseIntPositive(v, n) || n <= 0) {
                pom2::log().error("CLI", std::string("--step expects positive int, got: ") + v);
                return std::nullopt;
            }
            CliAction act{}; act.kind = CliAction::Kind::Step; act.countI = n;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--trace-brk") {
            CliAction act{}; act.kind = CliAction::Kind::TraceBrk;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--play") {
            CliAction act{}; act.kind = CliAction::Kind::PlayTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--rec") {
            CliAction act{}; act.kind = CliAction::Kind::RecTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--rewind") {
            CliAction act{}; act.kind = CliAction::Kind::RewindTape;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--snapshot-save") {
            const char* v = needArg(i, "--snapshot-save"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::SnapshotSave; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (a == "--snapshot-load") {
            const char* v = needArg(i, "--snapshot-load"); if (!v) return std::nullopt;
            CliAction act{}; act.kind = CliAction::Kind::SnapshotLoad; act.pathS = v;
            plan.deferredActions.push_back(std::move(act));
        }
        else if (!a.empty() && a[0] != '-') {
            // First non-flag argument = positional disk image to mount +
            // boot (5.25" / 3.5" / HDV, auto-routed). A second positional
            // is an error — we only boot one disk.
            if (!plan.bootDiskPath.empty()) {
                pom2::log().error("CLI",
                    "unexpected extra argument: " + a +
                    " (only one disk image may be given)");
                printUsage();
                return std::nullopt;
            }
            plan.bootDiskPath = a;
        }
        else {
            pom2::log().error("CLI", std::string("unknown flag: ") + a);
            printUsage();
            return std::nullopt;
        }
    }

    return plan;
}

} // namespace pom2
