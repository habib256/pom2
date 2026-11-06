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

// pom2_corpus — the compatibility corpus, headless (VALIDATION-006).
//
// Boots every title of a manifest on its profile through the SDK facade
// (`pom2::Core`, include/pom2/core.hpp — the same machine a library consumer
// gets, single-threaded, cycle-reproducible), plays its script of keys and
// emulated seconds, hashes the frame at each `check`, compares with the
// recorded expectation and prints a pass rate. `--bless` records the frames
// instead. Format and workflow: tests/corpus/README.md.
//
// What a hash is: `Core::renderFrame()`, POM2's composite colour table on the
// CPU, of the last video frame the beam finished — beam-raced splits
// included. It is technical evidence that a frame did or did not change, not
// the picture the application shows (docs/SDK.md § Rendering), and it is a
// function of the firmware as much as of POM2: every expectation carries the
// fingerprint of the dumps that produced it, and a run with other dumps skips
// the title with that reason instead of failing it.
//
// Media are COPIED to a scratch directory before they are mounted, so a guest
// that writes never touches the tree (CLAUDE.md: media write by default).
// A title whose media or firmware is absent is skipped with the reason, never
// failed. Exit: 0 = nothing failed, 1 = a check changed or is unrecorded, a
// setup failed; 2 = usage or manifest error; 77 = no title could run.

#include "CorpusManifest.h"
#include "CpuClock.h"
#include "ResourcePaths.h"
#include "Sha256.h"
#include "SystemProfile.h"

#include <pom2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <process.h>
#endif

// stb's PNG writer for --frames. Its own sprintf is not ours to fix.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;
using pom2::corpus::Device;
using pom2::corpus::Step;
using pom2::corpus::Title;

namespace {

void usage()
{
    std::fprintf(stderr,
        "Usage: pom2_corpus [options]\n"
        "\n"
        "  --manifest FILE   titles to run      (default tests/corpus/corpus.manifest)\n"
        "  --expected FILE   recorded frames    (default tests/corpus/corpus.expected)\n"
        "  --root DIR        media paths are relative to it (default: the current\n"
        "                    directory, i.e. run it from the source tree)\n"
        "  --tag TAG         only the titles carrying TAG (e.g. ci)\n"
        "  --title ID        only that title (repeatable)\n"
        "  --bless           record this run's frames into --expected instead of\n"
        "                    comparing (only the titles that ran are replaced)\n"
        "  --report FILE.md  write the result as a Markdown table (docs/audits/)\n"
        "  --frames DIR      write every checked frame as DIR/<title>__<check>.png\n"
        "  --list            print the titles and exit\n"
        "\n"
        "Exit 0: nothing failed. 1: a frame changed, is unrecorded, or a setup\n"
        "failed. 2: usage or manifest error. 77: no title could run.\n");
}

bool readFile(const fs::path& p, std::vector<std::uint8_t>& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::optional<pom2::SystemProfile> profileFor(const std::string& key)
{
    for (pom2::SystemProfile p : pom2::allProfiles())
        if (pom2::profileKey(p) == key) return p;
    return std::nullopt;
}

pom2::Machine machineFor(pom2::SystemProfile p)
{
    using P = pom2::SystemProfile;
    switch (p) {
    case P::AppleII:
    case P::AppleIIPlus:           return pom2::Machine::AppleIIPlus;
    case P::AppleIIeUnenhanced:
    case P::AppleIIeUnenhancedPAL: return pom2::Machine::AppleIIe;
    case P::AppleIIe:
    case P::AppleIIePAL:           return pom2::Machine::AppleIIeEnhanced;
    case P::AppleIIc:
    case P::AppleIIcPAL:           return pom2::Machine::AppleIIc;
    case P::AppleIIcPlus:          return pom2::Machine::AppleIIcPlus;
    }
    return pom2::Machine::AppleIIPlus;
}

// The firmware a title ran on: every dump loaded, by name and content. The
// short form goes into the expectations; the list goes into the report.
struct Firmware {
    std::string concatenated;
    std::vector<std::string> names;
    void add(const std::string& path, const std::vector<std::uint8_t>& bytes)
    {
        const std::string name = fs::path(path).filename().string();
        names.push_back(name);
        concatenated += name + "=" + pom2::sha256Hex(bytes.data(), bytes.size()) + ";";
    }
    std::string fingerprint() const
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(concatenated.data());
        return pom2::sha256Hex(p, concatenated.size()).substr(0, 12);
    }
};

// First probe that resolves, read whole. "" when none does.
std::string firstResource(const std::vector<std::string>& probes,
                          std::vector<std::uint8_t>& bytes)
{
    for (const std::string& rel : probes) {
        const std::string p = pom2::findResource(rel);
        if (!p.empty() && readFile(p, bytes)) return p;
    }
    return {};
}

enum class Outcome { Pass, Fail, New, Skip };

struct CheckResult {
    std::string name;
    std::string hash;
    enum class State { Match, Changed, Unrecorded } state = State::Unrecorded;
};

struct TitleResult {
    const Title* title = nullptr;
    Outcome outcome = Outcome::Skip;
    std::string reason;
    std::string firmware;
    std::vector<std::string> firmwareFiles;
    std::vector<CheckResult> checks;
    double seconds = 0;   // emulated
    double wall = 0;      // host seconds
};

const char* outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Pass: return "PASS";
    case Outcome::Fail: return "FAIL";
    case Outcome::New:  return "NEW";
    case Outcome::Skip: return "SKIP";
    }
    return "?";
}

struct Options {
    fs::path manifest = "tests/corpus/corpus.manifest";
    fs::path expected = "tests/corpus/corpus.expected";
    fs::path root = ".";
    std::string tag;
    std::vector<std::string> only;
    bool bless = false;
    bool list = false;
    fs::path report;
    fs::path frames;
};

fs::path scratchDir()
{
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = ".";
#if defined(_WIN32)
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(getpid());
#endif
    return base / ("pom2_corpus_" + std::to_string(pid));
}

TitleResult runTitle(const Title& t, const Options& opt,
                     const pom2::corpus::Expectations& expected,
                     const fs::path& scratch)
{
    TitleResult r;
    r.title = &t;
    const auto wallStart = std::chrono::steady_clock::now();
    auto skip = [&](std::string why) { r.outcome = Outcome::Skip; r.reason = std::move(why); return r; };
    auto fail = [&](std::string why) { r.outcome = Outcome::Fail; r.reason = std::move(why); return r; };

    const pom2::SystemProfile profile = *profileFor(t.profile);
    const pom2::ProfileConfig& cfg = pom2::profileConfig(profile);
    const pom2::Machine machine = machineFor(profile);
    const bool iicClass = machine == pom2::Machine::AppleIIc ||
                          machine == pom2::Machine::AppleIIcPlus;
    const bool pal = cfg.videoStandard == VideoStandard::PAL;
    const double clockHz = pom2VideoTiming(cfg.videoStandard).cpuClockHz;

    // ── Media first: an absent disk is the commonest skip, and it costs
    // nothing to find out before the firmware is read.
    struct Mount { const Device* device; fs::path copy; };
    std::vector<Mount> mounts;
    const fs::path titleScratch = scratch / t.id;
    for (const Device& d : t.devices) {
        if (d.media.empty()) continue;
        const fs::path src = opt.root / d.media;
        std::vector<std::uint8_t> bytes;
        if (!readFile(src, bytes))
            return skip("media not present: " + d.media +
                        " (tools/fetch_test_media.py fetches the test media)");
        if (!d.sha256.empty() && pom2::sha256Hex(bytes.data(), bytes.size()) != d.sha256)
            return skip("media differs from the manifest's sha256: " + d.media);
        std::error_code ec;
        fs::create_directories(titleScratch, ec);
        const fs::path copy = titleScratch /
            (d.kind + std::to_string(d.unit) + "_" + src.filename().string());
        std::ofstream out(copy, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) return fail("cannot copy " + d.media + " to " + copy.string());
        mounts.push_back({&d, copy});
    }

    // ── Firmware.
    Firmware fw;
    std::vector<std::uint8_t> romBytes, charBytes;
    const std::string rom = firstResource(cfg.romProbeOrder, romBytes);
    if (rom.empty()) return skip("no system ROM for " + t.profile + " (ROM Status)");
    const std::string chr = firstResource(cfg.charRomProbeOrder, charBytes);
    if (chr.empty()) return skip("no character ROM for " + t.profile);
    fw.add(rom, romBytes);
    fw.add(chr, charBytes);

    pom2::CoreConfig cc;
    cc.machine = machine;
    cc.timing = pal ? pom2::VideoTiming::PAL : pom2::VideoTiming::NTSC;
    pom2::Core core(cc);
    if (!core.loadRom(rom, pom2::profileUsesLowerRomHalf(profile)))
        return fail("loadRom: " + core.lastError());
    if (!core.loadCharacterRom(chr)) return fail("loadCharacterRom: " + core.lastError());

    bool diskII = false, smartPort = false, hdd = false;
    for (const Device& d : t.devices) {
        if (d.kind == "diskii") diskII = true;
        if (d.kind == "smartport") smartPort = true;
        if (d.kind == "hdd") hdd = true;
    }
    if (diskII) {
        std::vector<std::uint8_t> boot, seq;
        // A //c's $C600 is its system ROM. Elsewhere, Apple's PROM when it is
        // there, POM2's own otherwise — and either way the fingerprint says.
        const std::string bootRom = iicClass ? std::string() : firstResource({"roms/disk2.rom"}, boot);
        const std::string seqRom = firstResource({"roms/diskii_p6.rom"}, seq);
        if (!bootRom.empty()) fw.add(bootRom, boot);
        if (!seqRom.empty()) fw.add(seqRom, seq);
        if (!core.attachDiskII(bootRom, seqRom, 6))
            return fail("attachDiskII: " + core.lastError());
    }
    if (smartPort && machine != pom2::Machine::AppleIIcPlus && !core.attachSmartPort(5))
        return fail("attachSmartPort: " + core.lastError());
    if (hdd && !core.attachHardDisk(7))
        return fail("attachHardDisk: " + core.lastError());
    for (const Device& d : t.devices) {
        if (d.kind == "mockingboard") {
            const auto model = d.model == "soundii" ? pom2::MockingboardModel::SoundII
                                                    : pom2::MockingboardModel::AC;
            if (!core.attachMockingboard(d.slot, model))
                return fail("attachMockingboard: " + core.lastError());
        } else if (d.kind == "mouse") {
            const bool appleWin = d.model == "applewin";
            std::vector<std::uint8_t> eprom, mcu;
            const std::string ep = firstResource({"roms/mouse_341-0270-c.bin"}, eprom);
            if (ep.empty()) return skip("no mouse ROM (mouse_341-0270-c.bin)");
            fw.add(ep, eprom);
            if (!appleWin) {
                const std::string m = firstResource({"roms/mouse_341-0269.bin"}, mcu);
                if (m.empty()) return skip("no mouse MCU ROM (mouse_341-0269.bin)");
                fw.add(m, mcu);
            }
            const auto model = appleWin ? pom2::MouseModel::AppleMouseAppleWin
                                        : pom2::MouseModel::AppleMouse;
            if (!core.attachMouse(d.slot, model, fs::path(ep).parent_path().string()))
                return fail("attachMouse: " + core.lastError());
        } else if (d.kind == "rgb") {
            const auto model = d.model == "iic" ? pom2::RgbCardModel::LeChatMauveIIcAdapter
                                                : pom2::RgbCardModel::LeChatMauveFeline;
            if (!core.attachRgbCard(d.slot, model))
                return fail("attachRgbCard: " + core.lastError());
        }
    }
    r.firmware = fw.fingerprint();
    r.firmwareFiles = fw.names;

    // A check recorded with other dumps cannot be compared: say so first.
    for (const Step& s : t.steps) {
        if (s.kind != Step::Kind::Check) continue;
        const auto it = expected.find({t.id, s.text});
        if (!opt.bless && it != expected.end() && it->second.firmware != r.firmware)
        {
            std::string files;
            for (const auto& n : fw.names) files += (files.empty() ? "" : ", ") + n;
            return skip("recorded with firmware " + it->second.firmware +
                        ", this run has " + r.firmware + " (" + files + ")");
        }
    }

    for (const Mount& m : mounts) {
        const int unit = m.device->unit - 1;
        bool ok = false;
        if (m.device->kind == "diskii") ok = core.insertDisk(unit, m.copy.string());
        else if (m.device->kind == "smartport") ok = core.insertSmartPortDisk(unit, m.copy.string());
        else if (m.device->kind == "hdd") ok = core.insertHardDisk(unit, m.copy.string());
        if (!ok) return fail("mounting " + m.device->media + ": " + core.lastError());
    }

    if (t.bootSlot == 0) {
        core.coldBoot();
    } else if (!core.bootSlot(t.bootSlot)) {
        return fail("boot slot " + std::to_string(t.bootSlot) + ": " + core.lastError());
    }

    long long cycles = 0;
    bool anyChanged = false, anyUnrecorded = false;
    for (const Step& s : t.steps) {
        switch (s.kind) {
        case Step::Kind::Run: {
            const long long end = cycles + static_cast<long long>(s.seconds * clockHz);
            // 4096-cycle slices: the chunk the application's worker runs.
            while (cycles < end)
                cycles += core.run(static_cast<int>(std::min<long long>(4096, end - cycles)));
            break;
        }
        case Step::Kind::Key:
            core.queueKey(s.key);
            break;
        case Step::Kind::Text:
            core.pasteText(s.text);
            break;
        case Step::Kind::Check: {
            const pom2::FramebufferView f = core.renderFrame();
            CheckResult c;
            c.name = s.text;
            c.hash = pom2::corpus::hashHex(pom2::corpus::frameHash(f.pixels, f.width, f.height));
            const auto it = expected.find({t.id, s.text});
            if (it == expected.end()) {
                c.state = CheckResult::State::Unrecorded;
                anyUnrecorded = true;
            } else if (it->second.hash == c.hash) {
                c.state = CheckResult::State::Match;
            } else {
                c.state = CheckResult::State::Changed;
                anyChanged = true;
            }
            if (!opt.frames.empty() && f.pixels) {
                std::error_code ec;
                fs::create_directories(opt.frames, ec);
                const fs::path png = opt.frames / (t.id + "__" + s.text + ".png");
                stbi_write_png(png.string().c_str(), f.width, f.height, 4, f.pixels, f.width * 4);
            }
            r.checks.push_back(std::move(c));
            break;
        }
        }
    }
    r.seconds = static_cast<double>(cycles) / clockHz;
    if (core.cpuState().halted) r.reason = "CPU halted (KIL/STP) at the end";
    r.outcome = anyChanged ? Outcome::Fail : anyUnrecorded ? Outcome::New : Outcome::Pass;
    r.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    return r;
}

std::string checkSummary(const TitleResult& r)
{
    int match = 0;
    std::string changed, unrecorded;
    for (const CheckResult& c : r.checks) {
        if (c.state == CheckResult::State::Match) ++match;
        else if (c.state == CheckResult::State::Changed) changed += (changed.empty() ? "" : ", ") + c.name;
        else unrecorded += (unrecorded.empty() ? "" : ", ") + c.name;
    }
    std::string s = std::to_string(match) + "/" + std::to_string(r.checks.size()) + " checks";
    if (!changed.empty()) s += " — changed: " + changed;
    if (!unrecorded.empty()) s += " — unrecorded: " + unrecorded;
    return s;
}

std::string today()
{
    const std::time_t now = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&now));
    return buf;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--manifest") opt.manifest = value();
        else if (a == "--expected") opt.expected = value();
        else if (a == "--root") opt.root = value();
        else if (a == "--tag") opt.tag = value();
        else if (a == "--title") opt.only.push_back(value());
        else if (a == "--bless") opt.bless = true;
        else if (a == "--report") opt.report = value();
        else if (a == "--frames") opt.frames = value();
        else if (a == "--list") opt.list = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    pom2::corpus::Manifest manifest;
    {
        std::ifstream in(opt.manifest);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", opt.manifest.string().c_str()); return 2; }
        std::string error;
        if (!pom2::corpus::parseManifest(in, manifest, error)) {
            std::fprintf(stderr, "%s: %s\n", opt.manifest.string().c_str(), error.c_str());
            return 2;
        }
    }
    for (const Title& t : manifest.titles) {
        if (!profileFor(t.profile)) {
            std::fprintf(stderr, "%s:%d: unknown profile '%s' (canonical keys: ii, ii+, "
                         "iie-u, iie, iic, iic+, iie-u-pal, iie-pal, iic-pal)\n",
                         opt.manifest.string().c_str(), t.line, t.profile.c_str());
            return 2;
        }
    }
    pom2::corpus::Expectations expected;
    {
        std::ifstream in(opt.expected);
        std::string error;
        if (in && !pom2::corpus::parseExpectations(in, expected, error)) {
            std::fprintf(stderr, "%s: %s\n", opt.expected.string().c_str(), error.c_str());
            return 2;
        }
    }

    std::vector<const Title*> selected;
    for (const Title& t : manifest.titles) {
        if (!opt.tag.empty() && !t.hasTag(opt.tag)) continue;
        if (!opt.only.empty() &&
            std::find(opt.only.begin(), opt.only.end(), t.id) == opt.only.end()) continue;
        selected.push_back(&t);
    }
    if (opt.list) {
        for (const Title* t : selected) {
            std::string tags;
            for (const auto& g : t->tags) tags += " " + g;
            std::printf("%-28s %-10s%s\n", t->id.c_str(), t->profile.c_str(), tags.c_str());
        }
        return 0;
    }
    if (selected.empty()) {
        std::fprintf(stderr, "no title selected\n");
        return 2;
    }

    const pom2::SdkInfo sdk = pom2::Core::sdkInfo();
    std::printf("pom2_corpus — POM2 %s (%s), %zu title(s) from %s\n", sdk.version.c_str(),
                sdk.commit.c_str(), selected.size(), opt.manifest.string().c_str());

    const fs::path scratch = scratchDir();
    std::vector<TitleResult> results;
    for (const Title* t : selected) {
        TitleResult r = runTitle(*t, opt, expected, scratch);
        std::error_code ec;
        fs::remove_all(scratch / t->id, ec);
        std::string line = std::string("  ") + outcomeName(r.outcome) + "  " + t->id +
                           "  [" + t->profile + "]";
        if (r.outcome != Outcome::Skip) {
            char timing[800];   // GCC sizes %f for DBL_MAX
            std::snprintf(timing, sizeof timing, "  %.0f s emulated in %.1f s", r.seconds, r.wall);
            line += "  " + checkSummary(r) + timing;
        }
        if (!r.reason.empty()) line += "  — " + r.reason;
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        results.push_back(std::move(r));
    }
    std::error_code ec;
    fs::remove_all(scratch, ec);

    int ran = 0, passed = 0, failed = 0, fresh = 0, skipped = 0, checks = 0, checksOk = 0;
    for (const TitleResult& r : results) {
        if (r.outcome == Outcome::Skip) { ++skipped; continue; }
        ++ran;
        if (r.outcome == Outcome::Pass) ++passed;
        if (r.outcome == Outcome::Fail) ++failed;
        if (r.outcome == Outcome::New) ++fresh;
        for (const CheckResult& c : r.checks) {
            ++checks;
            if (c.state == CheckResult::State::Match) ++checksOk;
        }
    }
    auto pct = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
    char rate[1024];   // GCC sizes %f for DBL_MAX
    std::snprintf(rate, sizeof rate,
                  "Pass rate: %d/%d titles (%.1f%%), %d/%d checks (%.1f%%); "
                  "%d failed, %d unrecorded, %d skipped",
                  passed, ran, pct(passed, ran), checksOk, checks, pct(checksOk, checks),
                  failed, fresh, skipped);

    if (opt.bless) {
        pom2::corpus::Expectations next = expected;
        int recorded = 0;
        for (const TitleResult& r : results) {
            if (r.outcome == Outcome::Skip) continue;
            for (auto it = next.begin(); it != next.end();)
                it = it->first.first == r.title->id ? next.erase(it) : std::next(it);
            for (const CheckResult& c : r.checks) {
                next[{r.title->id, c.name}] = {c.hash, r.firmware};
                ++recorded;
            }
        }
        std::ofstream out(opt.expected, std::ios::binary | std::ios::trunc);
        out << pom2::corpus::formatExpectations(next);
        if (!out) { std::fprintf(stderr, "cannot write %s\n", opt.expected.string().c_str()); return 2; }
        std::printf("Blessed: %d check(s) recorded into %s\n", recorded, opt.expected.string().c_str());
    } else {
        std::printf("%s\n", rate);
    }

    if (!opt.report.empty()) {
        std::ofstream md(opt.report, std::ios::binary | std::ios::trunc);
        md << "# Compatibility corpus — " << today() << "\n\n"
           << "POM2 " << sdk.version << " (`" << sdk.commit << "`), `pom2_corpus --manifest "
           << opt.manifest.generic_string() << (opt.tag.empty() ? "" : " --tag " + opt.tag)
           << "`.\n\n**" << rate << ".**\n\n"
           << "| Title | Profile | Result | Checks | Firmware | Note |\n"
           << "|---|---|---|---|---|---|\n";
        for (const TitleResult& r : results) {
            std::string files;
            for (const auto& n : r.firmwareFiles) files += (files.empty() ? "" : ", ") + n;
            md << "| `" << r.title->id << "` | `" << r.title->profile << "` | "
               << outcomeName(r.outcome) << " | "
               << (r.outcome == Outcome::Skip ? std::string("—") : checkSummary(r)) << " | "
               << (r.firmware.empty() ? std::string("—") : "`" + r.firmware + "` (" + files + ")")
               << " | " << r.reason << " |\n";
        }
        std::printf("Report: %s\n", opt.report.string().c_str());
    }

    if (opt.bless) return 0;
    if (ran == 0) {
        std::printf("SKIP: no title could run (see the reasons above)\n");
        return 77;
    }
    return (failed || fresh) ? 1 : 0;
}
