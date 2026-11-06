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

// pom2_dos33 — copy files in and out of a DOS 3.3 disk image, the DOS
// counterpart of pom2_cpm (see Dos33DiskFs.h).
//
//   pom2_dos33 disk.dsk ls
//   pom2_dos33 disk.dsk get HELLO [host-file]      text converted, A/B/I
//                                                  without their header
//   pom2_dos33 disk.dsk list HELLO [host-file]     an A or I program listed
//   pom2_dos33 disk.dsk put HELLO.bas [NAME]       type from the host name; a
//                                                  typed listing is tokenized,
//                                                  an AppleSingle unwrapped
//   pom2_dos33 disk.dsk rm NAME
//
// **The host name carries the type** (`HostFileTag.h`, the convention the
// ProDOS host folder already uses): `get` names the file it writes
// `HELLO.bas`, `NOTES.txt`, or `GAME#062000` when no extension can say it,
// and `put` reads the type and a B file's load address back off that name.
// Before 2026-11-01 `get` dropped a B file's load address on the floor and
// `put` guessed `.txt → T, else B at $2000`, so `get GAME` then `put GAME`
// silently relocated the file to $2000.
//
// `-t T|A|B|I` and `-a ADDR` override what the name says; an explicit
// HOSTFILE / NAME argument is taken verbatim. `--raw` on get keeps the
// file's own bytes (header and DOS text included). The image is committed through the same
// durable temp + rename as every POM2 write-back; a write-protected image
// (host read-only bit, the notch — MediaNotch.h) is refused. Do not edit an
// image a running POM2 has mounted: its autosave would write over yours.

#include "ApplesoftListing.h"
#include "IntegerBasicListing.h"
#include "AtomicFileReplace.h"
#include "Dos33DiskFs.h"
#include "HostFileImport.h"
#include "HostFileTag.h"
#include "MediaNotch.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace pom2::dos33fs;

void usage()
{
    std::fprintf(stderr,
        "Usage: pom2_dos33 [-t T|A|B|I] [-a ADDR] [--raw] IMAGE COMMAND [ARGS]\n"
        "\n"
        "IMAGE is a 140 KB DOS 3.3 disk (.dsk/.do DOS order, .po ProDOS order).\n"
        "\n"
        "  ls                         catalog\n"
        "  get NAME [HOSTFILE]        extract (text converted; A/B/I without header)\n"
        "  list NAME [HOSTFILE]       an A or I BASIC program as a text listing\n"
        "  put HOSTFILE [NAME]        add or replace (type from the host name)\n"
        "  rm NAME                    delete\n"
        "\n"
        "  -t T|A|B|I                 type for put, overriding the host name\n"
        "  -a ADDR                    B file load address, hex, overriding it too\n"
        "  --raw                      get: the file's own bytes, header included\n"
        "  --no-tokenize              put: keep an Applesoft TEXT listing as text\n"
        "\n"
        "put understands three things about the host file: an AppleSingle wrapper\n"
        "(cc65, CiderPress, Cadius) carries the type and load address itself; a\n"
        ".bas file that is a typed listing is tokenized into a real program; and\n"
        "anything else is typed by its name.\n"
        "\n"
        "Without HOSTFILE, get names the file after the type: HELLO.bas,\n"
        "NOTES.txt, or GAME#062000 when no extension can carry it. put reads\n"
        "that back, so a B file keeps its load address across a round trip.\n");
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

/// The catalog entry for `name`, case-insensitively; nullptr when absent.
const FileInfo* infoOf(const std::vector<FileInfo>& files, const std::string& name)
{
    std::string up = name;
    for (auto& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& f : files) {
        std::string n = f.name;
        for (auto& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (n == up) return &f;
    }
    return nullptr;
}

uint8_t typeOf(const std::vector<FileInfo>& files, const std::string& name)
{
    const FileInfo* f = infoOf(files, name);
    return f ? f->type : uint8_t(0xFF);
}

} // namespace

int main(int argc, char** argv)
{
    char typeArg = 0;
    long addr = 0x2000;
    bool addrGiven = false;
    bool raw = false;
    bool tokenize = true;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--raw") { raw = true; continue; }
        if (a == "--no-tokenize") { tokenize = false; continue; }
        if (a == "-t" || a == "-a") {
            if (i + 1 >= argc) { usage(); return 1; }
            const std::string v = argv[++i];
            if (a == "-t") {
                if (v.size() != 1 || std::string("TABI").find(char(std::toupper(v[0]))) == std::string::npos) {
                    std::fprintf(stderr, "-t expects T, A, B or I\n");
                    return 1;
                }
                typeArg = char(std::toupper(v[0]));
            } else {
                char* end = nullptr;
                addr = std::strtol(v.c_str(), &end, 16);
                if (*end || addr < 0 || addr > 0xFFFF) {
                    std::fprintf(stderr, "-a expects a hex address\n");
                    return 1;
                }
                addrGiven = true;
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
    Dos33Disk disk;
    std::string error;
    if (!disk.open(std::move(bytes), orderForPath(imagePath), error)) {
        std::fprintf(stderr, "%s: %s\n", imagePath.c_str(), error.c_str());
        return 1;
    }

    // Say the destination cannot take a write BEFORE doing the work, not
    // after. The idea is `CHECK_WRITABLE` from the Host File Transfer Card
    // (sbabcoc/ReplicApple2Plus, § 4.1), which checks the destination before
    // it even asks which files to transfer — but NOT its implementation,
    // which probes by writing a block back unchanged. POM2 already knows the
    // answer without touching the medium: the notch is the host read-only
    // bit (MediaNotch.h). `commit` asks again, because the bit can change in
    // between and that last gate is the one that matters.
    auto writable = [&]() -> bool {
        if (!pom2::mediaFileIsReadOnly(imagePath)) return true;
        std::fprintf(stderr, "%s: write-protected\n", imagePath.c_str());
        return false;
    };

    auto commit = [&]() -> int {
        if (!writable()) {
            return 1;
        }
        const std::filesystem::path target(imagePath);
        const std::filesystem::path tmp = pom2::tempSiblingPath(target);
        std::error_code ec;
        if (!pom2::prepareTempPath(tmp, ec)) {
            std::fprintf(stderr, "%s: %s\n", tmp.string().c_str(), ec.message().c_str());
            return 1;
        }
        if (!writeHostFile(tmp.string(), disk.image())) {
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

    if (cmd == "ls" && args.size() == 2) {
        std::printf("DISK VOLUME %d\n\n", disk.volume());
        for (const auto& f : disk.list())
            std::printf("%c%c %03d %s\n", f.locked ? '*' : ' ', typeLetter(f.type),
                        f.sectors % 1000, f.name.c_str());
        std::printf("\n%d sectors free\n", disk.freeSectors());
        return 0;
    }
    if ((cmd == "get" || cmd == "list") && (args.size() == 3 || args.size() == 4)) {
        std::vector<uint8_t> file;
        if (!disk.readFile(args[2], file, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const std::vector<FileInfo> catalog = disk.list();
        const FileInfo* info = infoOf(catalog, args[2]);
        const uint8_t type = info ? info->type : uint8_t(0xFF);
        uint16_t loadAddress = 0;
        std::vector<uint8_t> out;
        if (cmd == "list") {
            // A (Applesoft) and I (Integer BASIC) — the ][ Original's BASIC
            // is Integer, and `list` used to refuse it.
            if (type != 0x02 && type != 0x01) {
                std::fprintf(stderr,
                    "%s: not a BASIC program (A or I)\n", args[2].c_str());
                return 1;
            }
            const std::vector<uint8_t> prog = Dos33Disk::payload(type, file);
            const auto peek = [&](uint16_t a) {
                return a < prog.size() ? prog[a] : uint8_t(0);
            };
            pom2::applesoft::ListReport report;
            const std::string text = type == 0x02
                ? pom2::applesoft::list(0, peek, prog.size(), &report)
                : pom2::integerbasic::list(0, peek, prog.size());
            if (report.untypable > 0)
                std::fprintf(stderr,
                    "%s: warning: %d byte(s) of this program cannot be written "
                    "as text (first in line %d); typing the listing back will "
                    "not rebuild it. Use `get` for an exact copy.\n",
                    args[2].c_str(), report.untypable, report.firstLine);
            out.assign(text.begin(), text.end());
        } else if (raw) {
            out = file;
        } else {
            out = Dos33Disk::payload(type, file, &loadAddress);
            if (type == 0x00) out = Dos33Disk::textFromDos(out);
        }
        if (args.size() == 3 && cmd == "list") {
            std::fwrite(out.data(), 1, out.size(), stdout);
            return 0;
        }
        // The host name carries the type, so a `put` of what `get` wrote
        // restores it — a B file's load address included (HostFileTag.h).
        // `--raw` keeps the file's own bytes, header and all, so tagging it
        // would make `put` build a SECOND header over the first.
        std::string dst;
        if (args.size() == 4)            dst = args[3];
        else if (raw || cmd == "list")   dst = args[2];
        else dst = pom2::hostNameForType(info ? info->name : args[2],
                                         pom2::prodosTypeFromDos33(type),
                                         loadAddress);
        if (!writeHostFile(dst, out)) { std::fprintf(stderr, "%s: write failed\n", dst.c_str()); return 1; }
        if (args.size() == 3 && dst != args[2]) std::printf("%s\n", dst.c_str());
        return 0;
    }
    if (cmd == "put" && (args.size() == 3 || args.size() == 4)) {
        if (!writable()) return 1;
        std::vector<uint8_t> data;
        if (!readHostFile(args[2], data)) {
            std::fprintf(stderr, "%s: cannot read\n", args[2].c_str());
            return 1;
        }
        // What the host file becomes — its Apple name, type, aux and bytes —
        // is decided in ONE place for every tool (HostFileImport.h): an
        // AppleSingle wrapper first, else the host name (`#TTAAAA` tag, else
        // the extension — a tagged name keeps its extension, `HELLO.BAS#FC0801`
        // is the DOS file `HELLO.BAS`), and an Applesoft file that is a typed
        // LISTING is tokenized instead of being stored as ASCII under type A.
        pom2::ImportedFile imported;
        if (!pom2::importHostFile(args[2], data, imported, error, tokenize)) {
            std::fprintf(stderr, "%s: %s\n", args[2].c_str(), error.c_str());
            return 1;
        }
        if (!imported.note.empty())
            std::fprintf(stderr, "%s: %s\n", args[2].c_str(), imported.note.c_str());
        data = std::move(imported.data);
        const std::string base = imported.name;
        const uint8_t  prodosType = imported.type;
        const uint16_t prodosAux  = imported.aux;
        std::string name = args.size() == 4 ? args[3] : base;
        // The DOS type: -t wins, else what the host name said. A ProDOS type
        // with no DOS equivalent (and a typeless host file) is binary, as
        // before. S / R / new-A / new-B survive a round trip here although
        // -t cannot ask for them.
        FileType type = FileType::Binary;
        if (typeArg == 'T') type = FileType::Text;
        else if (typeArg == 'A') type = FileType::Applesoft;
        else if (typeArg == 'I') type = FileType::Integer;
        else if (typeArg == 'B') type = FileType::Binary;
        else {
            uint8_t dosType = 0;
            if (pom2::dos33TypeFromProdos(prodosType, dosType))
                type = static_cast<FileType>(dosType);
        }
        if (type == FileType::Text) data = Dos33Disk::textToDos(data);
        // -a wins; otherwise the tag's aux is a B file's load address, and
        // an untagged `.bin` means $0000, which is what the tag would say.
        const uint16_t loadAddress = addrGiven ? uint16_t(addr) : prodosAux;
        if (!disk.writeFile(name, type, data, loadAddress, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    if (cmd == "rm" && args.size() == 3) {
        if (!writable()) return 1;
        if (!disk.removeFile(args[2], error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    usage();
    return 1;
}
