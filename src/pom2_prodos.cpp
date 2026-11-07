// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// pom2_prodos — copy files in and out of a ProDOS volume image, IN PLACE:
// the ProDOS counterpart of pom2_dos33 and pom2_cpm (see ProDOSDiskFs.h).
//
//   pom2_prodos disk.po ls [DIR]
//   pom2_prodos disk.po get GAMES/HELLO [host-file]     text converted
//   pom2_prodos disk.po list HELLO [host-file]          a BAS / INT program listed
//   pom2_prodos disk.po put hello.bas [GAMES/HELLO]     type from the host file
//   pom2_prodos disk.po rm GAMES/HELLO                  a file, or an EMPTY directory
//   pom2_prodos disk.po mkdir GAMES
//
// "In place" is the point. POM2 could already build a volume FROM a folder
// (ProDOSVolume.h) and serve a folder AS a volume; neither changes one file
// on an existing disk without rebuilding the rest, and a disk that carries a
// boot block, a system the user installed, or files this tool does not
// understand has to keep every block it did not ask to change.
//
// **The host file says its own type** (HostFileImport.h, the same decision
// pom2_dos33 makes): an AppleSingle wrapper first, else the host name by the
// `NAME#TTAAAA` convention (HostFileTag.h), and a `.bas` that is a typed
// listing is tokenized. `get` names what it writes the same way, so a `put`
// of it restores the type and the aux.
//
// The image is `.po` / `.hdv` (ProDOS block order, any size) or a 140 KB
// `.dsk` / `.do` (DOS sector order — converted on the way in and back). It
// is committed through the same durable temp + rename as every POM2
// write-back; a write-protected image (host read-only bit, the notch —
// MediaNotch.h) is refused. Do not edit an image a running POM2 has mounted:
// its autosave would write over yours.

#include "ApplesoftListing.h"
#include "AtomicFileReplace.h"
#include "HostFileImport.h"
#include "HostFileTag.h"
#include "IntegerBasicListing.h"
#include "MediaNotch.h"
#include "ProDOSDiskFs.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace pom2::prodosfs;

constexpr uint8_t kTypeText = 0x04, kTypeInteger = 0xFA, kTypeApplesoft = 0xFC;

void usage()
{
    std::fprintf(stderr,
        "Usage: pom2_prodos [-t TYPE] [-a AUX] [--raw] [--no-tokenize] [--data-fork-only] IMAGE COMMAND [ARGS]\n"
        "\n"
        "IMAGE is a ProDOS volume: .po / .hdv (block order, any size) or a 140 KB\n"
        ".dsk / .do (DOS sector order).\n"
        "\n"
        "  ls [DIR]                   catalog of the volume, or of a directory\n"
        "  get PATH [HOSTFILE]        extract (a TXT file's line ends converted)\n"
        "  list PATH [HOSTFILE]       a BAS or INT program as a text listing\n"
        "  put HOSTFILE [PATH]        add or replace (type from the host file)\n"
        "  rm PATH                    delete a file, or an empty directory\n"
        "  mkdir PATH                 create a directory\n"
        "\n"
        "  -t TYPE                    put: file type, hex ($06) or TXT BIN BAS INT SYS\n"
        "  -a AUX                     put: aux type (a BIN's load address), hex\n"
        "  --raw                      get/put: no text conversion\n"
        "  --no-tokenize              put: keep an Applesoft TEXT listing as text\n"
        "  --data-fork-only           put: store an AppleSingle's data fork and drop\n"
        "                             its resource fork (refused otherwise)\n"
        "\n"
        "PATH is DIR/NAME from the volume directory. Without HOSTFILE, get names\n"
        "the file after its type — HELLO.bas, NOTES.txt, or GAME#062000 when no\n"
        "extension can carry it — and put reads that back. put also unwraps an\n"
        "AppleSingle file (cc65, CiderPress, Cadius), which states its own type,\n"
        "and tokenizes a .bas file that is a typed listing.\n");
}

bool readHostFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

bool writeHostFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

std::string lowerExtension(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// A ProDOS text file is 7-bit ASCII with CR line ends; a host one has LF
/// (or CR LF).
std::vector<uint8_t> textToProdos(const std::vector<uint8_t>& host)
{
    std::vector<uint8_t> out;
    out.reserve(host.size());
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (host[i] == '\r' && i + 1 < host.size() && host[i + 1] == '\n') continue;
        out.push_back(host[i] == '\n' ? uint8_t('\r') : host[i]);
    }
    return out;
}

std::vector<uint8_t> textFromProdos(const std::vector<uint8_t>& prodos)
{
    std::vector<uint8_t> out = prodos;
    for (uint8_t& b : out) {
        b &= 0x7F;                       // some editors set the high bit
        if (b == '\r') b = '\n';
    }
    return out;
}

const char* typeName(uint8_t type)
{
    switch (type) {
        case 0x00: return "UNK"; case 0x01: return "BAD"; case 0x04: return "TXT";
        case 0x06: return "BIN"; case 0x0F: return "DIR"; case 0x19: return "ADB";
        case 0x1A: return "AWP"; case 0x1B: return "ASP"; case 0xB3: return "S16";
        case 0xEF: return "PAS"; case 0xF0: return "CMD"; case 0xFA: return "INT";
        case 0xFB: return "IVR"; case 0xFC: return "BAS"; case 0xFD: return "VAR";
        case 0xFE: return "REL"; case 0xFF: return "SYS";
        default:   return nullptr;
    }
}

bool parseType(const std::string& text, uint8_t& out)
{
    std::string up = text;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (int t = 0; t < 256; ++t) {
        const char* name = typeName(static_cast<uint8_t>(t));
        if (name && up == name) { out = static_cast<uint8_t>(t); return true; }
    }
    std::string hex = up;
    if (!hex.empty() && hex[0] == '$') hex.erase(0, 1);
    char* end = nullptr;
    const long v = std::strtol(hex.c_str(), &end, 16);
    if (hex.empty() || *end || v < 0 || v > 0xFF) return false;
    out = static_cast<uint8_t>(v);
    return true;
}

/// ProDOS packs a date as YYYYYYYM MMMDDDDD (Technical Reference § B.4.2.2).
std::string dateText(uint16_t date)
{
    if (date == 0) return "         ";
    static const char* const kMonths[] = { "???", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                           "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    const int day = date & 0x1F, month = (date >> 5) & 0x0F, year = (date >> 9) & 0x7F;
    char text[16];
    std::snprintf(text, sizeof text, "%2d-%s-%02d", day, kMonths[month <= 12 ? month : 0], year);
    return text;
}

}  // namespace

int main(int argc, char** argv)
{
    bool raw = false, tokenize = true, typeGiven = false, auxGiven = false;
    bool dataForkOnly = false;
    uint8_t typeArg = 0;
    long    auxArg  = 0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--raw") { raw = true; continue; }
        if (a == "--no-tokenize") { tokenize = false; continue; }
        if (a == "--data-fork-only") { dataForkOnly = true; continue; }
        if (a == "-t" || a == "-a") {
            if (i + 1 >= argc) { usage(); return 1; }
            const std::string v = argv[++i];
            if (a == "-t") {
                if (!parseType(v, typeArg)) {
                    std::fprintf(stderr, "-t expects a type: hex ($06) or TXT BIN BAS INT SYS …\n");
                    return 1;
                }
                typeGiven = true;
            } else {
                std::string hex = v;
                if (!hex.empty() && hex[0] == '$') hex.erase(0, 1);
                char* end = nullptr;
                auxArg = std::strtol(hex.c_str(), &end, 16);
                if (hex.empty() || *end || auxArg < 0 || auxArg > 0xFFFF) {
                    std::fprintf(stderr, "-a expects a hex value, 0..FFFF\n");
                    return 1;
                }
                auxGiven = true;
            }
            continue;
        }
        args.push_back(a);
    }
    if (args.size() < 2) { usage(); return 1; }
    const std::string imagePath = args[0];
    const std::string cmd = args[1];

    std::vector<uint8_t> bytes;
    if (!readHostFile(imagePath, bytes)) {
        std::fprintf(stderr, "%s: cannot read\n", imagePath.c_str());
        return 1;
    }
    // A 140 KB .dsk / .do holds the same blocks in DOS sector order.
    const std::string ext = lowerExtension(imagePath);
    const bool dosOrdered = (ext == ".dsk" || ext == ".do") && bytes.size() == kDiskette140Bytes;
    if (dosOrdered) bytes = dosOrderToBlockOrder(bytes);

    ProDOSDisk disk;
    std::string error;
    if (!disk.open(std::move(bytes), error)) {
        std::fprintf(stderr, "%s: %s\n", imagePath.c_str(), error.c_str());
        return 1;
    }
    // What `put` and `mkdir` create is dated now, as ProDOS dates a file it
    // creates; the library takes no clock of its own (ProDOSDiskFs.h). Left
    // at 0 every file read "<NO DATE>" in a CATALOG (bug hunt 2026-11-07).
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        const bool ok = localtime_s(&local, &now) == 0;
#else
        const bool ok = localtime_r(&now, &local) != nullptr;
#endif
        if (ok)
            disk.setTimestamp(pom2::prodosfs::packDate(local.tm_year + 1900, local.tm_mon + 1,
                                                       local.tm_mday),
                              pom2::prodosfs::packTime(local.tm_hour, local.tm_min));
    }

    // Say the destination cannot take a write BEFORE doing the work
    // (pom2_dos33 has the reasoning); `commit` asks again.
    auto writable = [&]() -> bool {
        if (!pom2::mediaFileIsReadOnly(imagePath)) return true;
        std::fprintf(stderr, "%s: write-protected\n", imagePath.c_str());
        return false;
    };
    auto commit = [&]() -> int {
        if (!writable()) return 1;
        const std::vector<uint8_t> out =
            dosOrdered ? blockOrderToDosOrder(disk.image()) : disk.image();
        const std::filesystem::path target(imagePath);
        const std::filesystem::path tmp = pom2::tempSiblingPath(target);
        std::error_code ec;
        if (!pom2::prepareTempPath(tmp, ec)) {
            std::fprintf(stderr, "%s: %s\n", tmp.string().c_str(), ec.message().c_str());
            return 1;
        }
        if (!writeHostFile(tmp.string(), out)) {
            std::filesystem::remove(tmp, ec);
            std::fprintf(stderr, "%s: write failed\n", tmp.string().c_str());
            return 1;
        }
        if (!pom2::replaceMediaFileAtomic(tmp, target, ec)) {
            std::filesystem::remove(tmp, ec);
            std::fprintf(stderr, "%s: %s\n", imagePath.c_str(), ec.message().c_str());
            return 1;
        }
        return 0;
    };

    if (cmd == "ls" && (args.size() == 2 || args.size() == 3)) {
        std::vector<Entry> entries;
        const std::string dir = args.size() == 3 ? args[2] : std::string();
        if (!disk.list(dir, entries, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("/%s%s%s\n\n", disk.volumeName().c_str(), dir.empty() ? "" : "/", dir.c_str());
        for (const Entry& e : entries) {
            char type[8];
            if (const char* n = typeName(e.fileType)) std::snprintf(type, sizeof type, "%s", n);
            else std::snprintf(type, sizeof type, "$%02X", e.fileType);
            std::printf("%c%-15s %-3s %5u  %s  %7u  A=$%04X\n", e.locked() ? '*' : ' ',
                        e.name.c_str(), type, static_cast<unsigned>(e.blocksUsed),
                        dateText(e.modDate).c_str(), static_cast<unsigned>(e.eof),
                        static_cast<unsigned>(e.auxType));
        }
        std::printf("\nBLOCKS FREE: %u   BLOCKS USED: %u   TOTAL BLOCKS: %u\n",
                    static_cast<unsigned>(disk.freeBlocks()),
                    static_cast<unsigned>(disk.totalBlocks() - disk.freeBlocks()),
                    static_cast<unsigned>(disk.totalBlocks()));
        return 0;
    }

    if ((cmd == "get" || cmd == "list") && (args.size() == 3 || args.size() == 4)) {
        std::vector<uint8_t> file;
        Entry info;
        if (!disk.readFile(args[2], file, &info, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::vector<uint8_t> out;
        if (cmd == "list") {
            if (info.fileType != kTypeApplesoft && info.fileType != kTypeInteger) {
                std::fprintf(stderr, "%s: not a BASIC program (BAS or INT)\n", args[2].c_str());
                return 1;
            }
            const auto peek = [&](uint16_t a) { return a < file.size() ? file[a] : uint8_t(0); };
            pom2::applesoft::ListReport report;
            const std::string text = info.fileType == kTypeApplesoft
                ? pom2::applesoft::list(0, peek, file.size(), &report)
                : pom2::integerbasic::list(0, peek, file.size());
            if (report.untypable > 0)
                std::fprintf(stderr,
                    "%s: warning: %d byte(s) of this program cannot be written as text "
                    "(first in line %d); typing the listing back will not rebuild it. "
                    "Use `get` for an exact copy.\n",
                    args[2].c_str(), report.untypable, report.firstLine);
            out.assign(text.begin(), text.end());
        } else {
            out = (!raw && info.fileType == kTypeText) ? textFromProdos(file) : file;
        }
        if (args.size() == 3 && cmd == "list") {
            std::fwrite(out.data(), 1, out.size(), stdout);
            return 0;
        }
        // The host name carries the type and the aux, so a `put` of what
        // `get` wrote restores both (HostFileTag.h). A listing is text: it
        // is named for what it is, not for the program it came from.
        // A guest name that starts with a period (A2osX's .PROFILE) never
        // becomes a host dotfile: it leaves as `%2ePROFILE#040000`, the
        // spelling the ProDOS host folder writes and `put` reads back
        // (HostFileTag.h, the leading-dot escape).
        const bool dotted = info.name.size() > 1 && info.name.front() == '.';
        std::string dst;
        if (args.size() == 4)   dst = args[3];
        else if (cmd == "list") dst = (dotted ? pom2::hostEscapeLeadingDot(info.name)
                                              : info.name) + ".txt";
        else                    dst = pom2::hostNameForGuestFile(info.name, info.fileType,
                                                                 info.auxType);
        if (!writeHostFile(dst, out)) {
            std::fprintf(stderr, "%s: write failed\n", dst.c_str());
            return 1;
        }
        if (args.size() == 3) std::printf("%s\n", dst.c_str());
        return 0;
    }

    if (cmd == "put" && (args.size() == 3 || args.size() == 4)) {
        if (!writable()) return 1;
        std::vector<uint8_t> data;
        if (!readHostFile(args[2], data)) {
            std::fprintf(stderr, "%s: cannot read\n", args[2].c_str());
            return 1;
        }
        pom2::ImportedFile imported;
        if (!pom2::importHostFile(args[2], data, imported, error, tokenize && !typeGiven,
                                   dataForkOnly)) {
            std::fprintf(stderr, "%s: %s\n", args[2].c_str(), error.c_str());
            return 1;
        }
        if (!imported.note.empty())
            std::fprintf(stderr, "%s: %s\n", args[2].c_str(), imported.note.c_str());
        // -t and -a win over what the file said of itself. An untagged
        // `GAME.bin` is BIN with aux $0000 — NOT a guessed $2000: that is the
        // pair `get` writes under that name (HostFileTag.h), and guessing
        // here would relocate a file across a get / put round trip, the bug
        // pom2_dos33 had until 2026-11-01. Say -a 2000 to mean it.
        const uint8_t type = typeGiven ? typeArg : imported.type;
        const uint16_t aux = auxGiven ? static_cast<uint16_t>(auxArg) : imported.aux;
        std::vector<uint8_t> payload = std::move(imported.data);
        if (!raw && type == kTypeText) payload = textToProdos(payload);
        const std::string path = args.size() == 4 ? args[3] : imported.name;
        if (!disk.writeFile(path, type, aux, payload, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }

    if ((cmd == "rm" || cmd == "mkdir") && args.size() == 3) {
        if (!writable()) return 1;
        const bool ok = cmd == "rm" ? disk.removeFile(args[2], error)
                                    : disk.makeDirectory(args[2], error);
        if (!ok) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }

    usage();
    return 1;
}
