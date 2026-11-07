// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// pom2_disk — make, inspect and repair 5.25" disk IMAGES. The file-system
// tools (pom2_dos33, pom2_prodos, pom2_cpm) work inside a disk; this one
// works on the disk itself.
//
//   pom2_disk new GAME.dsk --dos33                a formatted, empty DOS 3.3 disk
//   pom2_disk new DATA.po --prodos --name DATA    a formatted, empty ProDOS volume
//   pom2_disk new BIG.hdv --prodos --blocks 65535 …of any size up to 32 MB
//   pom2_disk new RAW.dsk --unformatted           zeros: INIT it in the machine
//   pom2_disk boot DEMO.dsk demo.bin --load 6000  a disk that boots your binary
//   pom2_disk info IMAGE.woz                      what a WOZ says of itself
//   pom2_disk recover BAD.woz GOOD.woz            a healthy copy + what was lost
//
// NOTHING HERE IS APPLE'S. A "new" disk carries no DOS and no ProDOS — POM2
// ships neither — so it is a DATA disk: boot your system disk, then use this
// one. Booting it by mistake prints a line saying so (DiskBuilder.h). `boot`
// is the way to a disk that starts by itself: a 128-byte loader of our own
// and your code, no operating system at all.
//
// The container follows the file name, as everywhere in POM2: .dsk / .do are
// DOS-ordered sectors, .po is ProDOS-ordered, .woz is a bit-cell image (made
// by POM2's own writer), .nib a raw nibble dump. `new` and `boot` never
// replace an existing file unless --force says so.
//
// Exit codes: 0 done · 1 refused or failed · 2 (`recover` and `info` only)
// done, and the image has tracks that do not verify or were lost.

#include "AtomicFileReplace.h"
#include "DiskBuilder.h"
#include "DiskImage.h"
#include "ProDOSDiskFs.h"
#include "WozCompat.h"
#include "WozRecover.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void usage()
{
    std::fprintf(stderr,
        "Usage: pom2_disk COMMAND ...\n"
        "\n"
        "  new IMAGE --dos33 [--volume N]          formatted, empty DOS 3.3 data disk\n"
        "  new IMAGE --prodos [--name NAME] [--blocks N]\n"
        "                                          formatted, empty ProDOS volume\n"
        "                                          (280 blocks = 140 KB by default;\n"
        "                                          1600 = 800 KB; up to 65535)\n"
        "  new IMAGE --unformatted                 never formatted: INIT it yourself\n"
        "  boot IMAGE BINARY [--load ADDR] [--entry ADDR]\n"
        "                                          a disk that boots BINARY with no\n"
        "                                          operating system. ADDR is hex,\n"
        "                                          $0900..$BFFF; load defaults to\n"
        "                                          0900, entry to the load address\n"
        "  info IMAGE.woz                          what a WOZ says of itself, and\n"
        "                                          which sectors verify\n"
        "  recover DAMAGED.woz HEALTHY.woz         repair the container, keep every\n"
        "                                          readable track bit for bit, report\n"
        "                                          what does not verify\n"
        "\n"
        "  --force                                 new/boot/recover: replace IMAGE\n"
        "\n"
        "The container follows the name: .dsk .do (DOS order), .po (ProDOS order),\n"
        ".woz, .nib; .hdv/.2mg are not written (use .po, or .hdv for a block\n"
        "image larger than a floppy). A new disk holds no DOS and no ProDOS: it\n"
        "is a data disk. `boot` makes the one kind that starts by itself.\n"
        "\n"
        "Exit: 0 done, 1 refused or failed, 2 done but the image has tracks that\n"
        "do not verify or were lost (info, recover).\n");
}

bool readHostFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

std::string lowerExtension(const std::string& path)
{
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// Write `bytes` to `path` through the durable temp + rename every POM2
/// write-back uses. Refuses an existing file unless `force`.
bool writeImage(const std::string& path, const std::vector<uint8_t>& bytes, bool force)
{
    std::error_code ec;
    const fs::path target(path);
    if (!force && fs::exists(target, ec)) {
        std::fprintf(stderr, "%s already exists (--force to replace it)\n", path.c_str());
        return false;
    }
    const fs::path tmp = pom2::tempSiblingPath(target);
    if (!pom2::prepareTempPath(tmp, ec)) {
        std::fprintf(stderr, "%s: %s\n", tmp.string().c_str(), ec.message().c_str());
        return false;
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            fs::remove(tmp, ec);
            std::fprintf(stderr, "%s: write failed\n", tmp.string().c_str());
            return false;
        }
    }
    if (!pom2::replaceMediaFileAtomic(tmp, target, ec)) {
        fs::remove(tmp, ec);
        std::fprintf(stderr, "%s: %s\n", path.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

/// A DOS-ordered sector buffer as the container `ext` names. The two bit-
/// level containers go through POM2's own disk model: the sectors are
/// nibblized exactly as a mounted .dsk is, and written out by the same code
/// that saves one — so what this tool makes is what the emulator would.
bool wrap(const std::vector<uint8_t>& dosOrdered, const std::string& ext,
          std::vector<uint8_t>& out, std::string& error)
{
    if (ext == ".dsk" || ext == ".do") { out = dosOrdered; return true; }
    if (ext == ".po") { out = pom2::diskbuild::dosOrderToProdosOrder(dosOrdered); return true; }
    if (ext != ".woz" && ext != ".nib") {
        error = "cannot write a " + (ext.empty() ? std::string("file with no extension") : ext) +
                " — name it .dsk, .do, .po, .woz or .nib";
        return false;
    }
    std::error_code ec;
    const fs::path scratch = pom2::tempSiblingPath(fs::temp_directory_path() / "pom2_disk.dsk");
    {
        std::ofstream f(scratch, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(dosOrdered.data()),
                static_cast<std::streamsize>(dosOrdered.size()));
        if (!f) { error = "cannot write " + scratch.string(); return false; }
    }
    auto image = std::make_unique<DiskImage>();          // 242 KB: not on the stack
    const bool loaded = image->loadFile(scratch.string(), DiskImage::SectorOrder::Dos33);
    fs::remove(scratch, ec);
    if (!loaded) { error = image->getLastError(); return false; }

    std::vector<DiskImage::TrackBuffer> surface(DiskImage::kTracks);
    for (int t = 0; t < DiskImage::kTracks; ++t)
        for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
            surface[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
                image->nibbleAt(t, i);
    if (ext == ".woz") {
        out = DiskImage::buildWozFromSurface(surface, /*thirteenSector=*/false);
    } else {
        out.clear();
        out.reserve(static_cast<std::size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack);
        for (const auto& track : surface) out.insert(out.end(), track.begin(), track.end());
    }
    return true;
}

bool parseHex(const char* text, long& out)
{
    std::string s = text;
    if (!s.empty() && s[0] == '$') s.erase(0, 1);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 16);
    return !s.empty() && *end == '\0' && out >= 0 && out <= 0xFFFF;
}

int commandNew(const std::vector<std::string>& args, bool force)
{
    enum class Kind { None, Dos33, Prodos, Unformatted } kind = Kind::None;
    std::string path, name = "NEW.DISK";
    long volume = 254, blocks = 280;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto value = [&](long lo, long hi, long& out) {
            char* end = nullptr;
            if (i + 1 >= args.size()) return false;
            out = std::strtol(args[++i].c_str(), &end, 10);
            return *end == '\0' && out >= lo && out <= hi;
        };
        if (a == "--dos33") kind = Kind::Dos33;
        else if (a == "--prodos") kind = Kind::Prodos;
        else if (a == "--unformatted") kind = Kind::Unformatted;
        else if (a == "--volume") {
            if (!value(1, 254, volume)) { std::fprintf(stderr, "--volume expects 1..254\n"); return 1; }
        } else if (a == "--blocks") {
            if (!value(16, 65535, blocks)) { std::fprintf(stderr, "--blocks expects 16..65535\n"); return 1; }
        } else if (a == "--name") {
            if (i + 1 >= args.size()) { usage(); return 1; }
            name = args[++i];
        } else if (path.empty() && !a.empty() && a[0] != '-') path = a;
        else { usage(); return 1; }
    }
    if (path.empty() || kind == Kind::None) {
        std::fprintf(stderr, "new: give an image name and one of --dos33, --prodos, --unformatted\n");
        return 1;
    }
    const std::string ext = lowerExtension(path);
    std::vector<uint8_t> out;
    std::string error;

    if (kind == Kind::Prodos) {
        std::vector<uint8_t> volumeBytes =
            pom2::prodosfs::ProDOSDisk::format(static_cast<uint32_t>(blocks), name, error);
        if (volumeBytes.empty()) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        // Block 0 is the boot block; ours says the disk is not a startup
        // disk rather than running zeros (the PROM loads its first half).
        const std::vector<uint8_t> boot = pom2::diskbuild::notBootableSector();
        std::copy(boot.begin(), boot.end(), volumeBytes.begin());
        if (blocks == 280 && ext != ".po" && ext != ".hdv") {
            // A floppy-sized volume in a sector or bit-level container.
            if (!wrap(pom2::diskbuild::prodosOrderToDosOrder(volumeBytes), ext, out, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
        } else if (ext == ".po" || ext == ".hdv") {
            out = std::move(volumeBytes);
        } else {
            std::fprintf(stderr, "a %ld-block ProDOS volume is not a 5.25\" disk: name it .po "
                                 "or .hdv\n", blocks);
            return 1;
        }
    } else if (kind == Kind::Dos33) {
        if (!wrap(pom2::diskbuild::blankDos33(static_cast<uint8_t>(volume)), ext, out, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    } else {
        // "Never formatted" has no representation in a WOZ POM2 would mount
        // (a WOZ with no track is refused) and a .nib of zeros is not a disk.
        if (ext != ".dsk" && ext != ".do" && ext != ".po") {
            std::fprintf(stderr, "an unformatted disk is a .dsk, .do or .po of zeros; mount it "
                                 "with POM2's New Blank (or --blank-disk) and INIT it\n");
            return 1;
        }
        out.assign(pom2::diskbuild::kImageBytes, 0);
    }
    if (!writeImage(path, out, force)) return 1;
    std::printf("%s: %zu bytes\n", path.c_str(), out.size());
    return 0;
}

int commandBoot(const std::vector<std::string>& args, bool force)
{
    std::vector<std::string> names;
    long load = 0x0900, entry = -1;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--load" || a == "--entry") {
            long v = 0;
            if (i + 1 >= args.size() || !parseHex(args[++i].c_str(), v)) {
                std::fprintf(stderr, "%s expects a hex address\n", a.c_str());
                return 1;
            }
            (a == "--load" ? load : entry) = v;
        } else if (!a.empty() && a[0] != '-') names.push_back(a);
        else { usage(); return 1; }
    }
    if (names.size() != 2) { usage(); return 1; }
    std::vector<uint8_t> payload;
    if (!readHostFile(names[1], payload)) {
        std::fprintf(stderr, "%s: cannot read\n", names[1].c_str());
        return 1;
    }
    pom2::diskbuild::DirectBootSpec spec;
    spec.loadAddress  = static_cast<uint16_t>(load);
    spec.entryAddress = static_cast<uint16_t>(entry < 0 ? load : entry);
    std::vector<uint8_t> sectors, out;
    std::string refusal;
    if (!pom2::diskbuild::directBoot(payload, spec, sectors, refusal)) {
        std::fprintf(stderr, "%s: %s\n", names[1].c_str(), refusal.c_str());
        return 1;
    }
    if (!wrap(sectors, lowerExtension(names[0]), out, refusal)) {
        std::fprintf(stderr, "%s\n", refusal.c_str());
        return 1;
    }
    if (!writeImage(names[0], out, force)) return 1;
    std::printf("%s: boots %zu bytes at $%04X, entry $%04X (%zu sector(s))\n", names[0].c_str(),
                payload.size(), spec.loadAddress, spec.entryAddress,
                pom2::diskbuild::directBootSectors(spec.loadAddress, payload.size()));
    return 0;
}

/// The INFO fields a person wants to see. Read at the reference's fixed
/// position; `inspect` has already said whether the file is a WOZ at all.
void printInfoFields(const std::vector<uint8_t>& f)
{
    constexpr std::size_t kInfo = 20;
    if (f.size() < kInfo + 60 || f[3] != '2') return;
    const uint8_t version = f[kInfo];
    std::string creator(reinterpret_cast<const char*>(f.data() + kInfo + 5), 32);
    while (!creator.empty() && creator.back() == ' ') creator.pop_back();
    std::printf("INFO version %d, created by \"%s\"%s\n", version, creator.c_str(),
                f[kInfo + 2] ? ", write-protected" : "");
    if (version >= 2) {
        const uint16_t hardware = static_cast<uint16_t>(f[kInfo + 40] | (f[kInfo + 41] << 8));
        const uint16_t ramK     = static_cast<uint16_t>(f[kInfo + 42] | (f[kInfo + 43] << 8));
        const std::string list = pom2::woz::hardwareList(hardware);
        std::printf("for: %s", list.empty() ? "not stated" : list.c_str());
        if (ramK) std::printf(", needs %uK", static_cast<unsigned>(ramK));
        std::printf("\n");
    }
}

int commandInfo(const std::vector<std::string>& args)
{
    if (args.size() != 1) { usage(); return 1; }
    std::vector<uint8_t> file;
    if (!readHostFile(args[0], file)) {
        std::fprintf(stderr, "%s: cannot read\n", args[0].c_str());
        return 1;
    }
    const pom2::woz::Report report = pom2::woz::inspect(file);
    if (!report.isWoz) {
        std::fprintf(stderr, "%s: not a WOZ file\n", args[0].c_str());
        return 1;
    }
    printInfoFields(file);
    std::fputs(pom2::woz::formatReport(report).c_str(), stdout);
    return report.clean() ? 0 : 2;
}

int commandRecover(const std::vector<std::string>& args, bool force)
{
    if (args.size() != 2) { usage(); return 1; }
    // By FILE, not by spelling: `a.woz` and `./a.woz` (or a hard link, or a
    // symlink) are the same file, and --force would have overwritten it.
    std::error_code sameEc;
    if (fs::path(args[0]) == fs::path(args[1]) ||
        (fs::exists(args[1], sameEc) && fs::equivalent(args[0], args[1], sameEc))) {
        // The damaged file is the evidence; it is never the output.
        std::fprintf(stderr, "recover writes a COPY: give another name for the healthy file\n");
        return 1;
    }
    std::vector<uint8_t> damaged, healthy;
    if (!readHostFile(args[0], damaged)) {
        std::fprintf(stderr, "%s: cannot read\n", args[0].c_str());
        return 1;
    }
    pom2::woz::Report report;
    std::string error;
    if (!pom2::woz::recover(damaged, healthy, report, error)) {
        std::fprintf(stderr, "%s: %s\n", args[0].c_str(), error.c_str());
        return 1;
    }
    if (!writeImage(args[1], healthy, force)) return 1;
    std::fputs(pom2::woz::formatReport(report).c_str(), stdout);
    std::printf("%s: written\n", args[1].c_str());
    return (report.tracksNotVerifying() == 0 && report.tracksDropped() == 0) ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--force") { force = true; continue; }
        args.push_back(a);
    }
    if (args.empty()) { usage(); return 1; }
    const std::string command = args[0];
    args.erase(args.begin());
    if (command == "new")     return commandNew(args, force);
    if (command == "boot")    return commandBoot(args, force);
    if (command == "info")    return commandInfo(args);
    if (command == "recover") return commandRecover(args, force);
    usage();
    return 1;
}
