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
//   pom2_dos33 disk.dsk list HELLO [host-file]     an A file as a listing
//   pom2_dos33 disk.dsk put host.txt [NAME]        .txt → T, else B at -a
//   pom2_dos33 disk.dsk rm NAME
//
// `-t T|A|B|I` forces the type on put; `-a ADDR` gives a B file's load
// address (hex, default 2000); `--raw` on get keeps the file's own bytes
// (header and DOS text included). The image is committed through the same
// durable temp + rename as every POM2 write-back; a write-protected image
// (host read-only bit, the notch — MediaNotch.h) is refused. Do not edit an
// image a running POM2 has mounted: its autosave would write over yours.

#include "ApplesoftListing.h"
#include "AtomicFileReplace.h"
#include "Dos33DiskFs.h"
#include "MediaNotch.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
        "  list NAME [HOSTFILE]       an Applesoft (A) file as a text listing\n"
        "  put HOSTFILE [NAME]        add or replace (.txt -> T, else B)\n"
        "  rm NAME                    delete\n"
        "\n"
        "  -t T|A|B|I                 type for put\n"
        "  -a ADDR                    B file load address, hex (default 2000)\n"
        "  --raw                      get: the file's own bytes, header included\n");
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

uint8_t typeOf(const std::vector<FileInfo>& files, const std::string& name)
{
    std::string up = name;
    for (auto& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& f : files) {
        std::string n = f.name;
        for (auto& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (n == up) return f.type;
    }
    return 0xFF;
}

} // namespace

int main(int argc, char** argv)
{
    char typeArg = 0;
    long addr = 0x2000;
    bool raw = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--raw") { raw = true; continue; }
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

    auto commit = [&]() -> int {
        if (pom2::mediaFileIsReadOnly(imagePath)) {
            std::fprintf(stderr, "%s: write-protected\n", imagePath.c_str());
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
        const uint8_t type = typeOf(disk.list(), args[2]);
        std::vector<uint8_t> out;
        if (cmd == "list") {
            if (type != 0x02) {
                std::fprintf(stderr, "%s: not an Applesoft (A) file\n", args[2].c_str());
                return 1;
            }
            const std::vector<uint8_t> prog = Dos33Disk::payload(type, file);
            const std::string text = pom2::applesoft::list(0, [&](uint16_t a) {
                return a < prog.size() ? prog[a] : uint8_t(0);
            }, prog.size());
            out.assign(text.begin(), text.end());
        } else if (raw) {
            out = file;
        } else {
            out = Dos33Disk::payload(type, file);
            if (type == 0x00) out = Dos33Disk::textFromDos(out);
        }
        if (args.size() == 3 && cmd == "list") {
            std::fwrite(out.data(), 1, out.size(), stdout);
            return 0;
        }
        const std::string dst = args.size() == 4 ? args[3] : args[2];
        if (!writeHostFile(dst, out)) { std::fprintf(stderr, "%s: write failed\n", dst.c_str()); return 1; }
        return 0;
    }
    if (cmd == "put" && (args.size() == 3 || args.size() == 4)) {
        std::vector<uint8_t> data;
        if (!readHostFile(args[2], data)) {
            std::fprintf(stderr, "%s: cannot read\n", args[2].c_str());
            return 1;
        }
        const std::filesystem::path host(args[2]);
        std::string name = args.size() == 4 ? args[3] : host.stem().string();
        char t = typeArg;
        if (!t) {
            std::string ext = host.extension().string();
            for (auto& c : ext) c = char(std::toupper(static_cast<unsigned char>(c)));
            t = ext == ".TXT" ? 'T' : 'B';
        }
        FileType type = FileType::Binary;
        if (t == 'T') { type = FileType::Text; data = Dos33Disk::textToDos(data); }
        if (t == 'A') type = FileType::Applesoft;
        if (t == 'I') type = FileType::Integer;
        if (!disk.writeFile(name, type, data, uint16_t(addr), error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    if (cmd == "rm" && args.size() == 3) {
        if (!disk.removeFile(args[2], error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    usage();
    return 1;
}
