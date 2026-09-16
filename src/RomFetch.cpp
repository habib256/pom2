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

#include "RomFetch.h"
#include "ZipMember.h"

#include "AtomicFileReplace.h"
#include "ChildProcess.h"
#include "Logger.h"
#include "Pom2Build.h"
#include "ResourcePaths.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;

namespace pom2 {

namespace {

// MAME keeps the unenhanced //e firmware as two 8 KB chips; POM2 probes the
// single 16 KB image, which is the CD ROM followed by the EF ROM.
const char* kApple2eUnenhChips[] = {
    "342-0134-a.64",
    nullptr,
};

// Every entry is a LOOSE file under bios/Apple/Apple II, except two that
// only exist inside a MAME romset: the mouse slot eprom (a2mouse.zip) and
// the unenhanced //e firmware (apple2e.zip, two chips concatenated).
// RetroBIOS PR #75 (merged 2026-09-14)
// added the //c / //c+ firmware, the card ROMs and the European character
// generators POM2 was missing, and published loose copies of the dumps this
// file used to extract from a2cffa02 / a2grapplerplus / a2superdrive — so
// the common path no longer needs `unzip` or `tar` on the host at all.
const std::vector<RomFetchEntry>& catalogStorage()
{
    static const std::vector<RomFetchEntry> k = {
        // ── Machine firmware ──────────────────────────────────────────
        { "roms/apple2o.rom", "Apple ][ Original", 12288,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2o.rom",
          nullptr, nullptr, 0u, nullptr,
          "68d9db6bb4c305d40c3fa89fa0f2d7b7f71516a9431e3138859f23bc5bddb2d1", 0u },
        // apple2-asoft-auto.rom, not RetroBIOS's apple2.rom: same length,
        // different firmware (the Integer/autostart mix). The digest below
        // is POM2's own generic fallback — Applesoft + autostart — so the
        // old URL was rejected on every run.
        { "roms/apple2.rom", "Apple ][ generic fallback (Applesoft autostart)", 12288,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2-asoft-auto.rom",
          nullptr, nullptr, 0u, nullptr,
          "fc3e9d41e9428534a883df5aa10eb55b73ea53d2fcbb3ee4f39bed1b07a82905", 0u },
        { "roms/apple2p.rom", "Apple ][+", 20480,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2p.rom",
          nullptr, nullptr, 0u, nullptr,
          // RetroBIOS now publishes the combined 20 KB dump next to the
          // six-chip apple2p.zip, and it is byte for byte the one POM2
          // ships — so this entry finally has a digest to gate on instead
          // of downloading a DIFFERENT (12 KB) II+ image than the local
          // file it was comparing against (bug hunt #10).
          "92c4bef609920842ea472d21b661a0d35dbda6cd90963b8b734a205e22d84108",
          // …and a tree that already holds the 12 KB six-chip image POM2
          // used to assemble from that zip still counts as complete: it is
          // a legitimate II+ firmware, so do not overwrite it.
          12288u },
        { "roms/apple2e.rom", "Apple //e Enhanced", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e.rom",
          nullptr, nullptr, 0u, nullptr,
          "c17bc38c75ba96c33a30c688a1efd60144811073423533fe7f8453cdd9457aab", 0u },
        // NOT RetroBIOS's loose AppleIIe.rom: that is a DIFFERENT 16 KB //e
        // dump, so the digest below — which is the firmware POM2 ships and
        // boots — rejected every download this entry ever made. MAME keeps
        // the unenhanced //e as two 8 KB chips; the image POM2 probes is
        // 342-0135-B ($C000-$DFFF) then 342-0134-A ($E000-$FFFF).
        { "roms/apple2e_unenh.rom", "Apple //e Unenhanced (two 8 KB chips)", 16384,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e.zip",
          "342-0135-b.64", kApple2eUnenhChips, 0u, nullptr,
          "1fb812584c6633fa16b77b20915986ed1178d1e6fc07a647f7ee8d4e6ab9d40b", 0u },
        { "roms/apple2c-32Kv0.rom", "Apple //c (ROM 0/3/4, 32 KB)", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2c-32Kv0.rom",
          nullptr, nullptr, 0u, nullptr,
          "d65bbf97058b9cdce8eaebf74124048e3b4e42dca1bd42be316747bbc27dc124", 0u },
        { "roms/apple2c-16K.rom", "Apple //c (ROM 255, 16 KB)", 16384,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2c-16K.rom",
          nullptr, nullptr, 0u, nullptr,
          "3ca1c8a27bf111aa0f4dc9372c5e6404457adb9a458ce32ae4b03a7e989e0c76", 0u },
        { "roms/3420033a.256", "Apple //c UniDisk 3.5 (342-0033-A)", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/3420033a.256",
          nullptr, nullptr, 0u, nullptr,
          "678d5c9d374664d2ee94ba9c9564d26a917b31116b07c46df30a3e82c640885b", 0u },
        { "roms/apple2cp.rom", "Apple //c Plus (ROM X4)", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2cp.rom",
          nullptr, nullptr, 0u, nullptr,
          "5a62070f6a0b07784681d4df4bf2ce88b2809bec0cbaa65fcb963e804ed60374", 0u },

        // ── Character generators ──────────────────────────────────────
        { "roms/apple2_char.rom", "II/II+ character ROM", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2-character.rom",
          nullptr, nullptr, 0u, nullptr,
          "08f5d22230481019844492dde0a29a018cb193712a9e4a43770a3870608f28de", 0u },
        { "roms/apple2e_char.rom", "//e Enhanced character ROM", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e-character.rom",
          nullptr, nullptr, 0u, nullptr,
          "52c3b87900ac939f6525402cab1ccfd8f8259290fc6df54da48fb4c98ae3ed0f", 0u },
        // Same bytes, second name: CharRomCatalog's "US Enhanced" picker
        // entry probes apple2e_char_us.rom, so a tree that only got
        // apple2e_char.rom lost that row from the dropdown. Likewise the
        // //e UNENHANCED profile's first char probe is apple2e_char_2k.rom,
        // not the apple2e_char_us_unenh.rom this catalogue already fetched.
        { "roms/apple2e_char_us.rom", "//e US Enhanced (342-0265-A)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e-character.rom",
          nullptr, nullptr, 0u, nullptr,
          "52c3b87900ac939f6525402cab1ccfd8f8259290fc6df54da48fb4c98ae3ed0f", 0u },
        { "roms/apple2e_char_us_unenh.rom", "//e Unenhanced character ROM", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2eu-character.rom",
          nullptr, nullptr, 0u, nullptr,
          "ed5bdd4afa509134e85f1d020685af7ff50e279226eb869a17825b471cc1634c", 0u },
        { "roms/apple2e_char_2k.rom", "//e Unenhanced char ROM (profile probe)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2eu-character.rom",
          nullptr, nullptr, 0u, nullptr,
          "ed5bdd4afa509134e85f1d020685af7ff50e279226eb869a17825b471cc1634c", 0u },

        // ── International character sets (the picker's locale rows) ───
        { "roms/apple2e_char_fr.rom", "//e Français (4 KB)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_fr.rom",
          nullptr, nullptr, 0u, nullptr,
          "52d8e46b487a617c288caaf12ab7ad6cb81030b6ab92419f9a2e19c43d765566", 0u },
        { "roms/apple2e_char_frca.rom", "//e FR Canadien Enhanced", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_frca.rom",
          nullptr, nullptr, 0u, nullptr,
          "56479cbec4767651c62a4d0f61e6ed103395216f2b86eddba74d85495dd71cc4", 0u },
        { "roms/apple2e_char_uk.rom", "//e UK Enhanced (342-0273-A)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_uk.rom",
          nullptr, nullptr, 0u, nullptr,
          "d55bd5391485cd78c68c71408f09bd088e82a73d179a0a25820067a0c1554acd", 0u },
        { "roms/apple2e_char_uk_unenh.rom", "//e UK Unenhanced (341-0160-A)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_uk_unenh.rom",
          nullptr, nullptr, 0u, nullptr,
          "f2199fbfb473e98a8c3fb7e7c3312faaf76b4b010b02c655b662e5f87dc997f3", 0u },
        { "roms/apple2e_char_de.rom", "//e Deutsch (341-0161-A)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_de.rom",
          nullptr, nullptr, 0u, nullptr,
          "b92576b4fa994384501078ff99963b53cfa98eff4bb641eb61649d484a469315", 0u },
        { "roms/apple2e_char_de_improved.rom", "//e Deutsch Improved", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_de_improved.rom",
          nullptr, nullptr, 0u, nullptr,
          "0e32ab131e3932fc61c0a5f0f5509b4c37ab8db6239027f5955d47feba56d39f", 0u },
        // The genuine 342-0274-A: ONE 8 KB part holding both the FR and the
        // US bank, which is why CharRomCatalog offers it as two rows.
        { "roms/342-0274-a.e9", "//e Français 342-0274-A (8 KB, both banks)", 8192,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/342-0274-a.e9",
          nullptr, nullptr, 0u, nullptr,
          "d4d29771658372c6ab5d18ff3bdab6b3b2b02b8de30ed6b021efb8a415192432", 0u },
        { "roms/apple2e_char_ft_blockascii.rom",
          "//e French Touch Block ASCII (custom)", 8192,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/apple2e_char_ft_blockascii.rom",
          nullptr, nullptr, 0u, nullptr,
          "c69c8e1eb26923c5df03d3507fdf86c5f768283a924a5fffdd80377cf2339321", 0u },
        { "roms/Videx Lower Case Chip ROM.bin",
          "Videx LOWER CASE CHIP (II/II+)", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/Videx%20Lower%20Case%20Chip%20ROM.bin",
          nullptr, nullptr, 0x00F68076u, "sha1 447874fe0850c8add3fd5b13fa98f6648fe6f999",
          "6a516bcd96382597af7a498fb1da7547490c830db0f9d22d541fb065b9cd048b", 0u },

        // ── Disk II ───────────────────────────────────────────────────
        { "roms/disk2.rom", "Disk II boot PROM 16-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/boot-16.rom",
          nullptr, nullptr, 0u, nullptr,
          "de1e3e035878bab43d0af8fe38f5839c527e9548647036598ee6fe7ec74d2a7d", 0u },
        { "roms/diskii_p6.rom", "Disk II P6 sequencer 16-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/state-machine-16.rom",
          nullptr, nullptr, 0u, nullptr,
          "e5e30615040567c1e7a2d21599681f8dac820edbdcda177b816a64d74b3a12f2", 0u },
        { "roms/disk2_13.rom", "Disk II boot PROM 13-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/boot-13.rom",
          nullptr, nullptr, 0u, nullptr,
          "2d2599521fc5763d4e8c308c2ee7c5c4d5c93785b8fb9a4f7d0381dfd5eb60b6", 0u },
        { "roms/diskii_p6_13.rom", "Disk II P6 sequencer 13-sector", 256,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/DiskII/state-machine-13.rom",
          nullptr, nullptr, 0u, nullptr,
          "383c55f5333cbfde39dd41528186631e5aca2eddfec30716344f03b6a4d44655", 0u },

        // ── Cards ─────────────────────────────────────────────────────
        { "roms/liron.rom", "Liron / Apple II 3.5 SmartPort controller", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/liron.rom",
          nullptr, nullptr, 0u, nullptr,
          "42c1ae66c6bec932669239599eb8989a5364752d56fa5c8997bb23d8dfc0b657", 0u },
        { "roms/cffa20ee02.bin", "CFFA 2.0 (6502)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/cffa20ee02.bin",
          nullptr, nullptr, 0x3ECAFCE5u, "dreher.net Run6_CDROM.zip",
          "3d7f6918f9af3828f5a6299299a817df0d354ee62e547bc0238755de66162a88", 0u },
        { "roms/cffa20eec02.bin", "CFFA 2.0 (65C02)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/cffa20eec02.bin",
          nullptr, nullptr, 0xFB3726F8u, "dreher.net Run6_CDROM.zip",
          "a08cb25ad80054649ee5fcb620e01ed56f025acb28c753bc28dd187a1d149cab", 0u },
        { "roms/thunderclock_u9_v1.3.bin", "ThunderClock+ slot ROM v1.3", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/thunderclock_u9_v1.3.bin",
          nullptr, nullptr, 0u, nullptr,
          "ecca793179427fdd193373080bf23e49a95743bd7b5e0b218c747663938b6c03", 0u },
        { "roms/ae_transwarp_1.4.bin", "TransWarp ROM v1.4 (Applied Engineering)", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/ae_transwarp_1.4.bin",
          nullptr, nullptr, 0xAFE37F55u, "MAME warprom",
          "bcbb1a3bab083b0c2988b502f5e024ff4346fab5dbf09276b32724693bcbd0e9", 0u },
        { "roms/grappler_plus.bin", "Grappler+ EPROM 3.2", 4096,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/grappler_plus.bin",
          nullptr, nullptr, 0u, nullptr,
          "9be4dea7722ad8928aee64c2ae708f365a6d66c50a63bff8a3c1a4e7596d44c6", 0u },
        { "roms/mouse_341-0269.bin", "Mouse card 6805 MCU", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/mouse_341-0269.bin",
          nullptr, nullptr, 0u, nullptr,
          "66480812edad5f6bd70349949cf8bd8fbafee2dc8bf396541d4e78bac2629ec0", 0u },
        // The only entry left that needs unzip/tar: RetroBIOS has no loose
        // copy of the mouse SLOT eprom, only MAME's a2mouse romset.
        { "roms/mouse_341-0270-c.bin", "Mouse card slot EPROM", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Arcade/MAME/a2mouse.zip",
          "341-0270-c.4b", nullptr, 0u, nullptr,
          "7f8e50c0e8a409991264201f9300f109b872b68e09c43f3e6c26af35fd8b89af", 0u },

        // ── Reference dumps (no POM2 card reads them yet) ─────────────
        { "roms/341-0438-a.bin", "Apple 3.5\" SuperDrive controller", 32768,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/341-0438-a.bin",
          nullptr, nullptr, 0xC73FF25Bu, "MAME a2superdrive",
          "084587835c1d2c92f5708a9745fdd43ba32136fcdb861a12ee7d690de22dc172", 0u },
        { "roms/341-0358-A.bin", "Apple II Workstation Card firmware", 65536,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/341-0358-a.bin",
          nullptr, nullptr, 0x63819DCBu, "sha1 59c8e8c88bac5c31ada1306b412edfcf5912a720",
          "ce785b913b9fcec6db0fab89b2d94f07fcd07664a822531dcbba3a5ca70ca3bf", 0u },
        { "roms/342-0326-a.f12", "//e international keyboard decode ROM (FR)", 2048,
          "https://raw.githubusercontent.com/Abdess/retrobios/main/bios/Apple/Apple%20II/342-0326-a.f12",
          nullptr, nullptr, 0xF04970A9u, "MAME apple2eefr",
          "4ae7ff693a486b155a70a8833047b3d4363464dace6760ed64bfb68db233ed4b", 0u },
    };
    return k;
}

// ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────────
// Small enough to keep here rather than grow a dependency; the only caller
// is the install gate below and its test. Not a hot path — the biggest input
// is 32 KB, once per fetched file.
struct Sha256Ctx {
    std::uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    std::uint64_t len = 0;
    std::uint8_t  buf[64]{};
    std::size_t   fill = 0;
};

inline std::uint32_t ror32(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void sha256Block(Sha256Ctx& c, const std::uint8_t* p)
{
    static const std::uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<std::uint32_t>(p[i * 4    ]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) <<  8) |
                static_cast<std::uint32_t>(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror32(w[i-15],  7) ^ ror32(w[i-15], 18) ^ (w[i-15] >>  3);
        const std::uint32_t s1 = ror32(w[i- 2], 17) ^ ror32(w[i- 2], 19) ^ (w[i- 2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], hh = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        const std::uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const std::uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const std::uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g;  c.h[7] += hh;
}

bool dirIsWritable(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
#if defined(_WIN32)
    const fs::path probe = dir / ".pom2_write_probe";
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
#else
    return ::access(dir.string().c_str(), W_OK) == 0;
#endif
}

bool runHostTool(const std::string& exe,
                 const std::vector<std::string>& args,
                 const std::string& cwd,
                 int timeoutMs,
                 std::string& err,
                 const RomFetchCancel& cancelled = {})
{
#if !POM2_HAS_CHILD_PROCESS
    (void)exe; (void)args; (void)cwd; (void)timeoutMs; (void)cancelled;
    err = "helper programs are not available in this build";
    return false;
#else
    ChildProcess p;
    if (!p.start(exe, args, cwd, err)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    while (p.isRunning()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > timeoutMs) {
            p.stop(500);
            err = exe + " timed out";
            return false;
        }
        // The 30 ms poll is also where a quit gets answered: without this the
        // panel's destructor waited out curl's full --max-time.
        if (cancelled && cancelled()) {
            p.stop(500);
            err = exe + " cancelled";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (p.lastExitCode() != 0) {
        err = exe + " exited " + std::to_string(p.lastExitCode());
        return false;
    }
    return true;
#endif
}

bool downloadUrl(const std::string& url, const fs::path& dest, std::string& err,
                 const RomFetchCancel& cancelled = {})
{
    const std::string curl = ChildProcess::findOnPath("curl");
    if (curl.empty()) {
        err = "curl was not found on PATH — install it to fetch ROMs";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".part";
    fs::remove(tmp, ec);
    if (!runHostTool(curl, curlDownloadArgs(url, tmp.string()),
                     {}, 120000, err, cancelled)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        err = "could not publish download: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

fs::path extractedMemberPath(const fs::path& dir, const std::string& member)
{
    // unzip -j and tar both drop a member with slashes as its basename
    // when we ask them to flatten; we only ever extract flat names, but
    // keep the last component so a future nested member still resolves.
    const auto pos = member.find_last_of("/\\");
    const std::string base =
        (pos == std::string::npos) ? member : member.substr(pos + 1);
    return dir / base;
}

// `cancelled` is forwarded to the child, like the download is: without it the
// Cancel button could not interrupt an unpack, and the panel sat unresponsive
// for up to 30 s per member on a slow or wedged unzip.
bool extractZipMember(const fs::path& zip, const std::string& member,
                      const fs::path& destDir, std::string& err,
                      const RomFetchCancel& cancelled = {})
{
    std::error_code ec;
    fs::create_directories(destDir, ec);

    const std::string unzip = ChildProcess::findOnPath("unzip");
    if (!unzip.empty()) {
        return runHostTool(unzip,
                           { "-o", "-q", "-j", zip.string(), member, "-d",
                             destDir.string() },
                           {}, 30000, err, cancelled);
    }

    const std::string tar = ChildProcess::findOnPath("tar");
    if (!tar.empty()) {
        return runHostTool(tar,
                           { "-xf", zip.string(), "-C", destDir.string(),
                             member },
                           {}, 30000, err, cancelled);
    }

    err = "neither unzip nor tar was found — cannot unpack a MAME romset";
    return false;
}

bool readAll(const fs::path& path, std::vector<std::uint8_t>& out, std::string& err);

/// One member's bytes. In-process first (stored/deflate — every archive
/// RetroBIOS serves); a host tool only for what that cannot read. The
/// in-process reader is not an optimisation: the host fallback was `tar`,
/// and GNU tar does not read zip, so a Linux box without unzip failed every
/// romset (bug hunt 2026-09-16).
bool unpackZipMember(const fs::path& zip, const std::string& member,
                     const fs::path& extractDir,
                     std::vector<std::uint8_t>& bytes, std::string& err,
                     const RomFetchCancel& cancelled)
{
    std::vector<std::uint8_t> archive;
    if (!readAll(zip, archive, err)) return false;
    bool unsupported = false;
    if (readZipMember(archive, member, bytes, err, kMaxUnpackedZipBytes,
                      &unsupported))
        return true;
    if (!unsupported) return false;
    return extractZipMember(zip, member, extractDir, err, cancelled) &&
           readAll(extractedMemberPath(extractDir, member), bytes, err);
}

/// Largest thing this module will ever read into memory. The biggest catalog
/// entry is 32 KB and the biggest zip a few hundred; the cap exists because
/// the file is whatever the network handed us.
constexpr std::size_t kMaxReadBytes = 64u * 1024u * 1024u;

bool readAll(const fs::path& path, std::vector<std::uint8_t>& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "could not read " + path.string();
        return false;
    }
    f.seekg(0, std::ios::end);
    // `tellg()` returns -1 on a stream that cannot seek (a fifo left behind
    // where the download should be) and the old cast turned that into
    // SIZE_MAX: `out.resize(SIZE_MAX)` throws length_error/bad_alloc, and the
    // throw escaped through the fetch thread — leaving `fetchRunning_` stuck
    // true, so the panel's Download button stayed disabled for the rest of
    // the session with no message anywhere.
    const std::streampos end = f.tellg();
    if (end < 0) {
        err = "could not measure " + path.string();
        return false;
    }
    const auto n = static_cast<std::uintmax_t>(end);
    if (n > kMaxReadBytes) {
        err = path.filename().string() + " is " + std::to_string(n) +
              " bytes — refusing to load it";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(n));
    if (n && !f.read(reinterpret_cast<char*>(out.data()),
                     static_cast<std::streamsize>(n))) {
        err = "short read of " + path.string();
        return false;
    }
    return true;
}

/// CRC-32 (IEEE, reflected) — the same law RomStatus_ImGui displays, so the
/// panel's verdict and this gate cannot disagree.
std::uint32_t crc32Bytes(const std::vector<std::uint8_t>& bytes)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// Install `bytes` as `dest`, but only if they are the dump we asked for.
///
/// Size was the only gate. The catalogue carries a CRC32 for the entries POM2
/// has a documented reference for, and it was displayed in the ROM Status
/// panel and used for nothing else — so a download that was the right LENGTH
/// and the wrong FILE (a mirror serving another revision, an error page
/// padded out) landed in the user's roms/ and surfaced days later as "it
/// doesn't boot". Verify before publishing, refuse on mismatch, and say what
/// was expected. (Bug hunt 2026-09-06 #H9.)
/// Where a replaced dump goes. OUTSIDE roms/ on purpose: the packaging
/// manifest ships that directory's working tree as-is, and a user's old ROM
/// has no business in a release package.
fs::path replacedRomsDir()
{
    return userDataDir() / "roms-replaced";
}

/// Copy `dest` aside before it is replaced. The planner re-verifies present
/// files and replaces the ones that are not the dump the catalogue names —
/// which is right for a damaged dump and wrong for a legitimate VARIANT the
/// catalogue has no digest for (a 16 KB //e firmware, another //c revision,
/// an older EPROM). POM2 cannot tell those apart, so it never destroys
/// either: the old file is kept, and the replacement does not happen at all
/// if it cannot be (bug hunt 2026-09-16).
bool backupBeforeReplace(const fs::path& dest, std::string& backupOut, std::string& err)
{
    backupOut.clear();
    std::error_code ec;
    if (!fs::exists(dest, ec)) return true;          // nothing to keep
    const fs::path dir = replacedRomsDir();
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot keep the old " + dest.filename().string() +
              " (" + dir.string() + ": " + ec.message() + ") — not replaced";
        return false;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    for (int n = 0; n < 100; ++n) {
        const fs::path target = dir / (dest.filename().string() + "." + stamp +
            (n ? ("-" + std::to_string(n)) : std::string()));
        ec.clear();
        // copy_options::none refuses an existing target: never overwrite a
        // backup either.
        if (fs::copy_file(dest, target, fs::copy_options::none, ec)) {
            backupOut = target.string();
            return true;
        }
        if (!fs::exists(target)) break;              // a real failure
    }
    err = "cannot keep the old " + dest.filename().string() +
          (ec ? (": " + ec.message()) : std::string()) + " — not replaced";
    return false;
}

bool commitBytes(const fs::path& dest, const RomFetchEntry& entry,
                 const std::vector<std::uint8_t>& bytes, std::string& err,
                 std::string& backupOut)
{
    if (entry.expectedSize && bytes.size() != entry.expectedSize) {
        err = dest.filename().string() + " is " + std::to_string(bytes.size()) +
              " bytes, expected " + std::to_string(entry.expectedSize);
        return false;
    }
    if (entry.expectedCrc) {
        const std::uint32_t got = crc32Bytes(bytes);
        if (got != entry.expectedCrc) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "%s has CRC32 %08X, expected %08X%s%s",
                          dest.filename().string().c_str(), got,
                          entry.expectedCrc,
                          entry.crcLabel ? " — " : "",
                          entry.crcLabel ? entry.crcLabel : "");
            err = buf;
            return false;
        }
    }
    // CRC32 detects damage; it does not identify a file. A 32-bit
    // non-cryptographic checksum is trivially collidable ON PURPOSE, so
    // "right size, right CRC" is not evidence that these are the bytes POM2
    // vouches for — and this file is about to be installed as the machine's
    // firmware. SHA-256 is, wherever the catalogue has one.
    if (entry.expectedSha256 && *entry.expectedSha256) {
        const std::string got = sha256Hex(bytes.data(), bytes.size());
        if (got != entry.expectedSha256) {
            err = dest.filename().string() + " has SHA-256 " + got +
                  ", expected " + entry.expectedSha256 +
                  " — this is not the dump POM2 asked for";
            return false;
        }
    }
    // Already exactly these bytes (another POM2 got there first, or the file
    // was fine all along): nothing to replace, nothing to keep.
    {
        std::error_code xec;
        std::vector<std::uint8_t> have;
        std::string readErr;
        if (fs::exists(dest, xec) && readAll(dest, have, readErr) && have == bytes) {
            backupOut.clear();
            return true;
        }
    }
    // Only now, with the download proven to be the right file: keep what it
    // is about to replace.
    if (!backupBeforeReplace(dest, backupOut, err)) return false;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (!writeFileAtomic(dest, bytes.data(), bytes.size(), ec)) {
        err = "could not write " + dest.string() +
              (ec ? (": " + ec.message()) : std::string());
        return false;
    }
    return true;
}

std::string defaultPresent(const char* destRel)
{
    return findResource(destRel);
}

}  // namespace

const std::vector<RomFetchEntry>& romFetchCatalog()
{
    return catalogStorage();
}

std::string sha256Hex(const std::uint8_t* data, std::size_t n)
{
    Sha256Ctx c;
    c.len = static_cast<std::uint64_t>(n) * 8u;
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) sha256Block(c, data + i);
    std::uint8_t tail[128]{};
    const std::size_t rem = n - i;
    if (rem) std::memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    const std::size_t blocks = (rem + 1 + 8 > 64) ? 2u : 1u;
    const std::size_t end = blocks * 64;
    for (int b = 0; b < 8; ++b)
        tail[end - 1 - static_cast<std::size_t>(b)] =
            static_cast<std::uint8_t>((c.len >> (8 * b)) & 0xFFu);
    for (std::size_t b = 0; b < blocks; ++b) sha256Block(c, tail + b * 64);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t v : c.h) {
        for (int b = 3; b >= 0; --b) {
            const std::uint8_t byte = static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFu);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0Fu]);
        }
    }
    return out;
}

std::vector<std::string> curlDownloadArgs(const std::string& url,
                                          const std::string& outPath)
{
    // `-fsSL` alone was the whole policy, and it followed a redirect from
    // https:// to http:// without a word — a downgrade any network position
    // can force, after which the "verified" bytes are whatever the wire says.
    // `--proto` pins the FIRST request to HTTPS, `--proto-redir` pins every
    // hop after it, and `--max-filesize` stops a mirror answering a 256-byte
    // PROM with a terabyte: the old 64 MB cap was applied when READING the
    // file back, i.e. after it had already landed on the user's disk.
    return { "-fsSL",
             "--proto",       "=https",
             "--proto-redir", "=https",
             "--max-filesize", std::to_string(kMaxUnpackedZipBytes),
             "--retry", "2", "--max-time", "90",
             "-o", outPath, url };
}

bool zipUnpackedSizeWithinCap(const std::vector<std::uint8_t>& zip,
                              std::uintmax_t& totalOut,
                              std::string& err)
{
    totalOut = 0;
    // Walk the central directory, which is the only place a zip states the
    // uncompressed size of every member up front. Find the End Of Central
    // Directory record by scanning back from the tail (the comment field is
    // variable-length, so there is no fixed offset).
    if (zip.size() < 22) { err = "archive is too small to be a zip"; return false; }
    const std::size_t maxBack = std::min<std::size_t>(zip.size(), 22u + 65535u);
    std::size_t eocd = 0;
    bool found = false;
    for (std::size_t back = 22; back <= maxBack; ++back) {
        const std::size_t off = zip.size() - back;
        if (zip[off] == 0x50 && zip[off+1] == 0x4B &&
            zip[off+2] == 0x05 && zip[off+3] == 0x06) { eocd = off; found = true; break; }
    }
    if (!found) { err = "no zip central directory (not an archive?)"; return false; }

    auto le16 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(zip[o]) |
              (static_cast<std::uint32_t>(zip[o+1]) << 8);
    };
    auto le32 = [&](std::size_t o) {
        return  static_cast<std::uint32_t>(zip[o])        |
               (static_cast<std::uint32_t>(zip[o+1]) << 8) |
               (static_cast<std::uint32_t>(zip[o+2]) << 16)|
               (static_cast<std::uint32_t>(zip[o+3]) << 24);
    };
    const std::uint32_t count = le16(eocd + 10);
    std::size_t         cd    = le32(eocd + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (cd + 46 > zip.size() ||
            !(zip[cd] == 0x50 && zip[cd+1] == 0x4B &&
              zip[cd+2] == 0x01 && zip[cd+3] == 0x02)) {
            err = "malformed zip central directory";
            return false;
        }
        totalOut += le32(cd + 24);                       // uncompressed size
        if (totalOut > kMaxUnpackedZipBytes) {
            err = "archive expands to more than " +
                  std::to_string(kMaxUnpackedZipBytes) +
                  " bytes — refusing to unpack it";
            return false;
        }
        cd += 46 + le16(cd + 28) + le16(cd + 30) + le16(cd + 32);
    }
    return true;
}

std::filesystem::path writableRomsDir()
{
    // A download has to land where findResource() looks FIRST, or a wrong
    // file in an earlier root keeps being the one the machine loads. The
    // per-user data dir is the first root whenever it is per-user, and it is
    // ours to create — so it is the answer then, whether or not its roms/
    // exists yet. Created by fetchMissingRoms, not here: this also feeds a
    // tooltip, and hovering it used to create the directory.
    if (userDataDirIsPerUser()) return userDataDir() / "roms";
    for (const auto& base : resourceSearchDirs()) {
        // The empty root is the working directory, and it is a root like the
        // others (it used to be skipped, so a writable ./roms was never used).
        const fs::path roms = base.empty() ? fs::path("roms") : base / "roms";
        std::error_code ec;
        if (fs::is_directory(roms, ec) && dirIsWritable(roms)) {
            const fs::path abs = fs::absolute(roms, ec);
            return ec ? roms : abs;
        }
    }
    return userDataDir() / "roms";
}

std::vector<const RomFetchEntry*> romsToFetch(
    const std::function<bool(const char* destRel)>& present)
{
    std::vector<const RomFetchEntry*> out;
    for (const auto& e : catalogStorage()) {
        if (!present(e.destRel)) out.push_back(&e);
    }
    return out;
}

std::vector<const RomFetchEntry*> romsToFetch()
{
    std::vector<const RomFetchEntry*> out;
    for (const auto& e : catalogStorage()) {
        const std::string resolved = defaultPresent(e.destRel);
        if (resolved.empty()) { out.push_back(&e); continue; }
        // Present is not the same as CORRECT. The ROM Status panel already
        // paints a wrong-size / wrong-digest dump red, but "Download missing"
        // used a bare findResource() probe and reported "everything is
        // already present" — so the one button that could replace the bad
        // file refused to run. Re-verify with the same gate commitBytes
        // applies to a fresh download; an entry with no reference digest and
        // no expected size stays "present" on the strength of its existence,
        // which is all POM2 knows about it.
        std::vector<std::uint8_t> have;
        std::string err;
        if (!readAll(resolved, have, err) || !localDumpAcceptable(e, have))
            out.push_back(&e);
    }
    return out;
}

bool installFetchedRom(const fs::path& dest, const RomFetchEntry& entry,
                       const std::vector<std::uint8_t>& bytes,
                       std::string& err, std::string& backupOut)
{
    return commitBytes(dest, entry, bytes, err, backupOut);
}

bool localDumpAcceptable(const RomFetchEntry& e, const std::vector<std::uint8_t>& have)
{
    // The alternate size is a DIFFERENT dump that is legitimate too — the
    // 12 KB six-chip II+ image beside the 20 KB one the catalogue fetches.
    // The digests below describe the fetched dump, so they cannot apply to
    // it: checking them anyway flagged every 12 KB II+ tree as broken and
    // "Download missing" replaced a firmware that boots (bug hunt 2026-09-16,
    // the same regression bug hunt #10 had fixed, re-opened when the II+
    // entry gained a SHA-256).
    if (e.altPresentSize && have.size() == e.altPresentSize &&
        have.size() != e.expectedSize)
        return true;
    if (e.expectedSize && have.size() != e.expectedSize) return false;
    if (e.expectedSha256 && *e.expectedSha256 &&
        sha256Hex(have.data(), have.size()) != e.expectedSha256)
        return false;
    if (e.expectedCrc && crc32Bytes(have) != e.expectedCrc) return false;
    return true;
}

namespace {

constexpr const char* kFetchDirPrefix = ".retrobios-fetch";

fs::path uniqueFetchDir(const fs::path& destRoot)
{
    static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return destRoot / (std::string(kFetchDirPrefix) + "-" + std::to_string(pid) +
                       "-" + std::to_string(counter.fetch_add(1) + 1));
}

void sweepStaleFetchDirs(const fs::path& destRoot)
{
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (fs::directory_iterator it(destRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(kFetchDirPrefix, 0) != 0) continue;
        std::error_code sec;
        if (!it->is_directory(sec) || sec) continue;
        const auto mtime = fs::last_write_time(it->path(), sec);
        if (sec || mtime > cutoff) continue;
        fs::remove_all(it->path(), sec);
    }
}

}  // namespace

RomFetchResult fetchMissingRoms(const fs::path& destRoot,
                                const RomFetchProgress& progress,
                                const RomFetchCancel& cancelled)
{
    RomFetchResult r;
    r.destDir = destRoot.string();

#if defined(__EMSCRIPTEN__)
    r.error   = "ROM download is not available in the browser build";
    r.summary = r.error;
    return r;
#else
    if (ChildProcess::findOnPath("curl").empty()) {
        r.error   = "curl was not found on PATH — install it to fetch ROMs";
        r.summary = r.error;
        return r;
    }

    const auto missing = romsToFetch();
    r.skipped = static_cast<int>(catalogStorage().size() - missing.size());
    if (missing.empty()) {
        r.summary = "Every RetroBIOS dump POM2 can use is already present.";
        return r;
    }

    std::error_code ec;
    fs::create_directories(destRoot, ec);
    if (!fs::is_directory(destRoot, ec)) {
        r.error   = "cannot create " + destRoot.string();
        r.summary = r.error;
        return r;
    }

    // One scratch directory PER RUN. The name used to be fixed, and every run
    // removed it at start and at end — so a second POM2 on the same $HOME
    // pressing Download deleted the first one's half-written files from under
    // its curl (bug hunt 2026-09-16). Leftovers of a crashed run are swept by
    // age instead: a live fetch is minutes old, a day-old one is debris.
    sweepStaleFetchDirs(destRoot);
    const fs::path scratch = uniqueFetchDir(destRoot);
    fs::create_directories(scratch, ec);

    std::unordered_map<std::string, fs::path> zipCache;
    const int total = static_cast<int>(missing.size());
    int done = 0;

    auto tick = [&](const char* label) {
        if (progress) progress(done, total, label);
    };

    for (const RomFetchEntry* e : missing) {
        if (cancelled && cancelled()) {
            if (r.error.empty()) r.error = "cancelled";
            break;
        }
        tick(e->label);
        std::string err;
        std::string backup;
        const fs::path dest = destRoot / fs::path(e->destRel).filename();

        bool ok = false;
        if (!e->zipMember) {
            const fs::path raw = scratch / fs::path(e->destRel).filename();
            ok = downloadUrl(e->url, raw, err, cancelled);
            if (ok) {
                std::vector<std::uint8_t> bytes;
                ok = readAll(raw, bytes, err) &&
                     commitBytes(dest, *e, bytes, err, backup);
            }
        } else {
            fs::path zipPath;
            auto it = zipCache.find(e->url);
            if (it == zipCache.end()) {
                zipPath = scratch / ("pack-" + std::to_string(zipCache.size()) + ".zip");
                if (!downloadUrl(e->url, zipPath, err, cancelled)) {
                    zipPath.clear();
                } else {
                    // Zip-bomb gate, BEFORE `unzip` runs: a few hundred
                    // downloaded kilobytes can expand to gigabytes, and the
                    // 64 MB cap this module had was applied when reading the
                    // EXTRACTED member back — long after the disk was full.
                    // The central directory states every member's
                    // uncompressed size up front, so the check costs one
                    // read of the archive we just fetched.
                    std::vector<std::uint8_t> zipBytes;
                    std::uintmax_t unpacked = 0;
                    if (!readAll(zipPath, zipBytes, err) ||
                        !zipUnpackedSizeWithinCap(zipBytes, unpacked, err)) {
                        std::error_code rmEc;
                        fs::remove(zipPath, rmEc);
                        zipPath.clear();
                    } else {
                        zipCache.emplace(e->url, zipPath);
                    }
                }
            } else {
                zipPath = it->second;
            }

            if (!zipPath.empty()) {
                const fs::path extractDir = scratch / ("x-" + std::to_string(done));
                std::vector<std::uint8_t> bytes;
                ok = unpackZipMember(zipPath, e->zipMember, extractDir, bytes,
                                     err, cancelled);
                if (ok && e->zipConcat) {
                    for (const char* const* m = e->zipConcat; *m; ++m) {
                        std::vector<std::uint8_t> more;
                        if (!unpackZipMember(zipPath, *m, extractDir, more, err,
                                             cancelled)) {
                            ok = false;
                            break;
                        }
                        bytes.insert(bytes.end(), more.begin(), more.end());
                    }
                }
                if (ok)
                    ok = commitBytes(dest, *e, bytes, err, backup);
            }
        }

        if (ok) {
            // Saved is not USED: a root searched before `destRoot` that holds
            // this name (and could not be the destination — not writable)
            // still wins findResource, and it holds the wrong dump, or this
            // entry would not have been fetched. Say so instead of reporting
            // a success the machine will not see.
            const std::string resolved = findResource(e->destRel);
            std::error_code eqEc;
            if (!resolved.empty() && !fs::equivalent(resolved, dest, eqEc)) {
                ok = false;
                err = std::string("saved ") + dest.string() + ", but " +
                      resolved + " is found first and is not the expected "
                      "dump — remove or replace it";
            }
        }
        if (ok) {
            ++r.saved;
            log().info("ROM", std::string("fetched ") + e->destRel +
                       " from RetroBIOS");
            if (!backup.empty()) {
                ++r.replaced;
                log().info("ROM", std::string("the previous ") +
                           fs::path(e->destRel).filename().string() +
                           " was not the expected dump; kept as " + backup);
            }
        } else {
            ++r.failed;
            log().warn("ROM", std::string("RetroBIOS fetch of ") + e->destRel +
                       " failed: " + err);
            if (r.error.empty()) r.error = err;
        }
        ++done;
        tick(e->label);
    }

    fs::remove_all(scratch, ec);

    char buf[192];
    if (r.failed == 0) {
        std::snprintf(buf, sizeof(buf),
                      "Saved %d ROM%s to %s (%d already present).",
                      r.saved, r.saved == 1 ? "" : "s",
                      destRoot.string().c_str(), r.skipped);
        if (r.replaced > 0) {
            // Say it where the user is looking: a file they had is not the
            // one in roms/ any more.
            char more[256];
            std::snprintf(more, sizeof(more),
                          " %d replaced a different file, kept in %s.",
                          r.replaced, replacedRomsDir().string().c_str());
            r.summary = std::string(buf) + more;
            return r;
        }
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Saved %d, failed %d, skipped %d — %s",
                      r.saved, r.failed, r.skipped, r.error.c_str());
    }
    r.summary = buf;
    return r;
#endif
}

}  // namespace pom2
