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

#include "HostFileTag.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

// Host extensions stripped off a name (the type comes from them instead).
const char* const kStripExts[] = {
    ".bas", ".bin", ".sys", ".txt", ".int",
    ".dsk", ".po",  ".do",  ".hdv", ".2mg"
};

} // namespace

std::string hostFileTag(std::uint8_t type, std::uint16_t aux)
{
    char tag[8];
    std::snprintf(tag, sizeof tag, "#%02X%04X", type, aux);
    return tag;
}

bool stripHostFileTag(std::string& name, std::uint8_t& type, std::uint16_t& aux)
{
    const std::size_t hash = name.rfind('#');
    if (hash == std::string::npos || name.size() - hash != 7) return false;
    for (std::size_t i = hash + 1; i < name.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
    type = static_cast<std::uint8_t>(std::stoul(name.substr(hash + 1, 2), nullptr, 16));
    aux  = static_cast<std::uint16_t>(std::stoul(name.substr(hash + 3, 4), nullptr, 16));
    name.erase(hash);
    return true;
}

std::uint8_t prodosTypeFromExtension(const std::string& ext)
{
    // Common ProDOS file types (cf. Apple ProDOS 8 Technical Reference):
    //   $00 typeless / unknown   $04 TXT   $06 BIN
    //   $FA INT (Integer BASIC)  $FC BAS (Applesoft)   $FF SYS
    std::string e = ext;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".bas") return 0xFC;
    if (e == ".bin") return 0x06;
    if (e == ".sys") return 0xFF;
    if (e == ".txt") return 0x04;
    if (e == ".int") return 0xFA;
    // Extensionless host files → typeless ($00) so the decode adds NO
    // extension and the name round-trips verbatim ("GAME" → "GAME", not
    // "GAME.bin"). Unknown extensions keep their dot in the Apple name, so
    // their type is informational → plain BIN.
    if (e.empty() || e == ".") return 0x00;
    return 0x06;
}

const char* extensionForProdosType(std::uint8_t t)
{
    switch (t) {
        case 0x00: return "";       // typeless → no extension
        case 0x04: return ".txt";
        case 0x06: return ".bin";
        case 0xFA: return ".int";
        case 0xFC: return ".bas";
        case 0xFF: return ".sys";
        default:   return nullptr;
    }
}

std::size_t strippedExtensionLength(const std::string& name)
{
    for (const char* xe : kStripExts) {
        const std::size_t xn = std::strlen(xe);
        if (name.size() <= xn) continue;
        std::string tail = name.substr(name.size() - xn);
        for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == xe) return xn;
    }
    return 0;
}

// An extension names a file TYPE and nothing else, so `PIC.bin` can only
// carry BIN with aux 0 back into the next mount. The ProDOS decode used to
// compose one regardless — every type it had no extension for became
// `.bin`, and every aux became 0 — and the loss was not even confined to
// files the guest changed: a host `PIC#062000` came back out as a SECOND
// file `PIC.bin` on every flush, because nothing on disk was called that.
// The next mount then served `PIC` and `PIC.1`. A guest `BSAVE X,A$2000`
// lost its load address the same way, and an AppleWorks document ($1A) came
// back as BIN, which AppleWorks then no longer listed.
//
// So the extension is used only when the parse would derive exactly this
// (type, aux) pair back from it, and the tag carries everything else. A
// dotted Apple name keeps its own suffix and the type is read off that
// suffix; `.bas` is exchangeable with aux $0801 because that is what the
// ProDOS build assigns and what BASIC.SYSTEM writes.
std::string hostNameForType(const std::string& name, std::uint8_t type,
                            std::uint16_t aux)
{
    // Which aux an extension can carry. $0000 for every type, EXCEPT
    // Applesoft: an untagged `.bas` is imported with aux $0801, the load
    // address BASIC.SYSTEM writes, so `.bas` carries $0801 and NOT $0000 —
    // spelling a BAS file of aux 0 `HELLO.bas` handed it back as $0801 on
    // the next mount (caught by `host_file_tag`, 2026-11-01, when the
    // convention got a test of its own: the composed name must parse back).
    const bool auxPlain = (type == 0xFC) ? (aux == 0x0801) : (aux == 0);
    // An Apple name ending in an extension the parse strips (`HELLO.BAS`,
    // `NOTES.TXT`) goes out TAGGED, so the next import keeps the whole name;
    // written plain it came back as HELLO and `RUN HELLO.BAS` failed.
    if (strippedExtensionLength(name) != 0)
        return name + hostFileTag(type, aux);
    if (name.find('.') != std::string::npos) {
        if (auxPlain &&
            prodosTypeFromExtension(fs::path(name).extension().string()) == type)
            return name;
    } else if (const char* ext = extensionForProdosType(type); ext && auxPlain) {
        return name + ext;
    }
    return name + hostFileTag(type, aux);
}

// DOS 3.3 catalog type → ProDOS file type.
//
//   T $00 → TXT $04      I $01 → INT $FA
//   A $02 → BAS $FC      B $04 → BIN $06
//
// Those four are the equivalences ProDOS's own CONVERT uses, and they are
// already what `prodosTypeFromExtension` assigns to `.txt` / `.int` /
// `.bas` / `.bin`, so one tag means the same thing on a DOS disk, in a
// ProDOS host folder and in the HGR Paint editor.
//
// **NOT sourced:** S $08, R $10, new-A $20 and new-B $40 have no
// equivalence POM2 has checked against a primary document. They are given
// ProDOS types $F2, $FE (REL), $F3 and $F4 purely so that a `get` of such a
// file round-trips through a `put`; the choice is POM2's own, nothing but
// `pom2_dos33` reads it back, and it should not be quoted as a convention.
std::uint8_t prodosTypeFromDos33(std::uint8_t dosType)
{
    switch (dosType) {
        case 0x00: return 0x04;   // T → TXT
        case 0x01: return 0xFA;   // I → INT
        case 0x02: return 0xFC;   // A → BAS
        case 0x04: return 0x06;   // B → BIN
        case 0x08: return 0xF2;   // S → POM2's own choice
        case 0x10: return 0xFE;   // R → REL
        case 0x20: return 0xF3;   // new A → POM2's own choice
        case 0x40: return 0xF4;   // new B → POM2's own choice
        default:   return 0x00;
    }
}

bool dos33TypeFromProdos(std::uint8_t prodosType, std::uint8_t& dosType)
{
    switch (prodosType) {
        case 0x04: dosType = 0x00; return true;
        case 0xFA: dosType = 0x01; return true;
        case 0xFC: dosType = 0x02; return true;
        case 0x06: dosType = 0x04; return true;
        case 0xF2: dosType = 0x08; return true;
        case 0xFE: dosType = 0x10; return true;
        case 0xF3: dosType = 0x20; return true;
        case 0xF4: dosType = 0x40; return true;
        default:   return false;
    }
}


// ------------------------------------------------- the leading-dot escape

std::string hostEscapeLeadingDot(const std::string& name)
{
    return "%2e" + name.substr(name.empty() ? 0 : 1);
}

bool stripHostLeadingDotEscape(std::string& hostName)
{
    if (hostName.size() < 4 || hostName[0] != '%' || hostName[1] != '2' ||
        (hostName[2] != 'e' && hostName[2] != 'E'))
        return false;
    hostName.erase(0, 3);
    return true;
}

} // namespace pom2
