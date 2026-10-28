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

// pom2_cpm — copy files in and out of an Apple II CP/M disk image
// (SoftCard / ALS 16-sector format, see CpmDiskFs.h). No cpmtools needed.
//
//   pom2_cpm disk.dsk ls
//   pom2_cpm disk.dsk get BTERM.ASM [host-file]
//   pom2_cpm disk.dsk put host/BTERM.COM [CPMNAME.EXT]
//   pom2_cpm disk.dsk rm BTERM.COM
//
// `-u N` selects user area N (default 0). The image is rewritten through
// the same durable temp + rename commit as every POM2 write-back, keeping
// its permissions; a write-protected image (host read-only bit, the notch
// — MediaNotch.h) is refused. Do not edit an image a running POM2 has
// mounted: its floppy autosave would write its own copy back over yours.

#include "AtomicFileReplace.h"
#include "CpmDiskFs.h"
#include "MediaNotch.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::fprintf(stderr,
        "Usage: pom2_cpm [-u USER] IMAGE COMMAND [ARGS]\n"
        "\n"
        "IMAGE is a 140 KB Apple II CP/M disk (.dsk/.do DOS order, .po ProDOS order,\n"
        ".cpm CP/M order).\n"
        "\n"
        "  ls                         list the files (user 0-15)\n"
        "  get NAME [HOSTFILE]        extract NAME (default: same name, cwd)\n"
        "  put HOSTFILE [NAME]        add or replace (default: host file name)\n"
        "  rm NAME                    delete\n"
        "\n"
        "  -u USER                    user area 0-15 (default 0)\n");
}

bool readHostFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

} // namespace

int main(int argc, char** argv)
{
    int user = 0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "-u") {
            if (i + 1 >= argc) { usage(); return 1; }
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > 15) {
                std::fprintf(stderr, "-u expects 0-15\n");
                return 1;
            }
            user = int(v);
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
    pom2::cpmfs::CpmDisk disk;
    std::string error;
    if (!disk.open(std::move(bytes), pom2::cpmfs::orderForPath(imagePath), error)) {
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
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(disk.image().data()),
                    std::streamsize(disk.image().size()));
            if (!f) {
                std::filesystem::remove(tmp, ec);
                std::fprintf(stderr, "%s: write failed\n", tmp.string().c_str());
                return 1;
            }
        }
        if (!pom2::replaceMediaFileAtomic(tmp, target, ec)) {
            std::filesystem::remove(tmp, ec);
            std::fprintf(stderr, "%s: %s\n", imagePath.c_str(), ec.message().c_str());
            return 1;
        }
        return 0;
    };

    if (cmd == "ls" && args.size() == 2) {
        for (const auto& f : disk.list()) {
            std::printf("%2d  %-12s %7u  %3d K%s%s\n", f.user, f.name.c_str(),
                        f.bytes, f.blocks, f.readOnly ? "  R/O" : "",
                        f.system ? "  SYS" : "");
        }
        std::printf("%d K free\n", disk.freeBlocks());
        return 0;
    }
    if (cmd == "get" && (args.size() == 3 || args.size() == 4)) {
        std::vector<uint8_t> data;
        if (!disk.readFile(user, args[2], data, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const std::string out = args.size() == 4 ? args[3] : args[2];
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) { std::fprintf(stderr, "%s: write failed\n", out.c_str()); return 1; }
        return 0;
    }
    if (cmd == "put" && (args.size() == 3 || args.size() == 4)) {
        std::vector<uint8_t> data;
        if (!readHostFile(args[2], data)) {
            std::fprintf(stderr, "%s: cannot read\n", args[2].c_str());
            return 1;
        }
        const std::string name = args.size() == 4
            ? args[3] : std::filesystem::path(args[2]).filename().string();
        if (!disk.writeFile(user, name, data, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    if (cmd == "rm" && args.size() == 3) {
        if (!disk.removeFile(user, args[2], error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return commit();
    }
    usage();
    return 1;
}
