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

// SlotCardCatalog — the single list of card types the user can assign to an
// expansion slot, plus the ROM-presence probes that gate the conditional
// entries (Mouse, CFFA). Extracted from MainWindow_Slots.cpp so BOTH the
// legacy Slot Configuration panel and the consolidated Slot Manager panel
// drive their dropdowns and built-in-name resolution from one source.

#ifndef POM2_SLOT_CARD_CATALOG_H
#define POM2_SLOT_CARD_CATALOG_H

#include "ResourcePaths.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace pom2 {

/// TODO.md's scope ruling, per card: what the project promises about it —
/// a card it stands behind, one it fixes on report, or one it ships as is.
///
/// NOT shown in the Slot Config picker any more (2026-09-12). It sat on
/// every row as "[L0 · LLE · core]" beside the abstraction level, and in a
/// docked panel that tail clipped the card's own name; the level alone
/// ("[LLE]" / "[HLE]") is what a user picking a card acts on. The ruling
/// itself lives in TODO.md § The scope ruling, which is where it is
/// maintained. The enum stays: it is the data behind that ruling.
enum class CardScope { Core, Supported, Frozen };

inline constexpr const char* cardScopeWord(CardScope s)
{
    switch (s) {
        case CardScope::Core:      return "core";
        case CardScope::Frozen:    return "frozen";
        case CardScope::Supported: break;
    }
    return "supported";
}

struct CardType {
    const char* key;
    const char* label;
    /// Defaults to Supported — the ruling's largest bucket. Core and Frozen
    /// are named explicitly below, from TODO.md § The scope ruling.
    CardScope   scope = CardScope::Supported;
};

// Card types the user can pick for any slot. Index 0 is the empty slot.
inline constexpr CardType kCardTypes[] = {
    { "",             "(empty)"           },
    { "diskii",       "Disk II",           CardScope::Core },
    { "hdv",          "ProDOS HDV"        },
    // CFFA 2.0 — MAME-faithful IDE/CompactFlash card: real firmware over an
    // emulated ATA chip (vs the synthetic "hdv"). Needs roms/cffa20ee02.bin
    // (6502) or cffa20eec02.bin (65C02); hidden from the picker when absent.
    { "cffa",         "CFFA 2.0 (IDE)"    },
    // The CFFA3000 uses an AVR and RAM-backed firmware, not the CFFA 2.0
    // ATA registers. Its SmartPort side is provided at block level.
    { "cffa3",        "CFFA3000 (SmartPort)" },
    // SmartPort 3.5" — Apple Disk 3.5 Controller card (the "Liron" /
    // 670-0186). Exposes 2-14 block units (8 by default, 3.5" or HDV) to
    // a //e or II+ through ProDOS/SmartPort services, without a card IWM.
    { "smartport35",  "SmartPort (UniDisk intelligent / HD)",   CardScope::Core },
    { "superdrive",   "Apple IIe SuperDrive (1.44M block interface)" },
    // The same controller as silicon: the real 4 KB Liron EPROM executing
    // over a real IWM, its 3.5" drives answered as intelligent UniDisks on
    // the SmartPort bus (SmartPortBusDevice). Needs roms/liron.rom; slot
    // left empty without it. Boots and serves ProDOS through the firmware's
    // own code — pick it to run the real thing, `smartport35` to be quick.
    { "liron",        "Liron (UniDisk intelligent / HD, real ROM)" },
    { "ssc",          "Super Serial"      },
    // Printer (parallel) — synthetic card that spools COUT bytes to a host
    // spool for export/rendering. A free-slot pick on II / II+ / //e;
    // //c-class printer ports use built-in SSCs. No PROM dump needed.
    { "printer",      "Printer (Parallel)" },
    // Grappler+ (Orange Micro) — parallel printer card with a 4 KB ROM
    // providing graphics dump commands (^I G / ^I H). Requires a ROM
    // dump in roms/grappler_plus.bin; falls back to a stub when missing.
    // See markadev/AppleII-RevEng/Orange-Micro-Grappler+.
    { "grappler",     "Grappler+ (Orange Micro)" },
    // The ORIGINAL Grappler (1981): its own 2 KB ROM, one $Cn page per slot
    // — only slot 1's carries the Pascal signature. Needs
    // roms/grappler_eps-1.bin. MAME `a2bus_grappler_device`.
    { "grappler1",    "Grappler (Orange Micro, 1981)" },
    // Apple II Parallel Interface Card (1979): 512-byte PROM 341-0057, no
    // Pascal signature at all. Needs roms/341-0057.bin. MAME `a2pic`.
    { "pic",          "Apple Parallel Interface" },
    // Videx Videoterm (1980) — the ][ / ][+ 80-column card: its own HD6845S,
    // 2 KB of VRAM and character generator; its picture replaces the Apple's
    // while TEXT and AN0 are on. The v2.4 firmware hard-codes $C0B0, so
    // SLOT 3 only. Needs roms/videx_videoterm_v24_60hz.bin (or _50hz) and
    // roms/videx_videoterm_char_normal.bin. Not offered on a //e-class
    // machine: its 80 columns are built in. MAME `a2videoterm`.
    { "videoterm",    "Videx Videoterm 80-column" },
    // VidHD (Blue Shift, 2018) — the IIgs Super Hi-Res modes on a //e: the
    // card latches $C029 NEWVIDEO (and $C022/$C034/$C035) off the bus and,
    // with SHR on, shows 320/640 x 200 read from aux RAM $2000-$9FFF. No ROM.
    // //e-class machines with slots only (the ][+ needs an aux mirror POM2
    // does not model). Port of AppleWin `source/VidHD.cpp`.
    { "vidhd",        "VidHD (Super Hi-Res)" },
    { "clock",        "Clock (ProDOS)"    },
    // Uthernet I (a2RetroSystems) — CS8900A Ethernet NIC. Raw frames
    // only: the Apple-side stack (IP65, Contiki, ADTPro-ethernet) does
    // the TCP/IP. Needs a host Ethernet transport to see traffic, which
    // in practice means a libslirp-enabled build; the card still plugs
    // and probes without one. MAME `bus/a2bus/uthernet.cpp` port.
    { "uthernet",     "Uthernet I (CS8900A)", CardScope::Frozen },
    // Uthernet II (a2RetroSystems) — WIZnet W5100 hardware TCP/IP stack.
    // Its four TCP/UDP sockets map onto host BSD sockets, so IRC / telnet
    // / FTP clients work with no libslirp and no privileges; only its
    // MACRAW / IPRAW modes need a backend. No MAME device — ported from
    // AppleWin `source/Uthernet2.cpp`.
    { "uthernet2",    "Uthernet II (W5100)" },
    // FujiNet — SmartPort controller relaying peer-backed calls over
    // SP-over-SLIP, with local count/N: services. Its peer can be a FujiNet
    // desktop build on loopback TCP, or a physical ESP32 board over USB
    // CDC-ACM. One card carries every FujiNet function (block storage, the
    // N: network device, clock, printer, modem, CP/M) because on the Apple II
    // they are all SmartPort units. No ROM dump needed — POM2 synthesises the
    // slot ROM. Slot 7 by convention so the autostart scan reaches it before
    // the Disk II in slot 6. II+ / //e only: a //c-class machine's forced
    // INTCXROM masks slot ROM entirely.
    { "fujinet",      "FujiNet (SP over SLIP)" },
    // Microsoft SoftCard — Z80 DMA bus-master card for CP/M. No ROM
    // needed (the card has none; the CP/M boot disk finds it by toggling
    // slot ROM windows). MAME a2bus/a2softcard.cpp port.
    { "softcard",     "SoftCard Z80 (CP/M)" },
    // Advanced Logic Systems "The CP/M Card" — 6 MHz Z80B, 64 KB of its
    // own, CP/M Plus. No ROM either; slot 4 or 7 as ALS's software says.
    { "alscpm",       "ALS CP/M Card (CP/M Plus)" },
    // Microsoft SoftCard II — 6 MHz Z80 on its own 64 KB, talking to the
    // 6502 through an Am2950 latch at $C0n0. No ROM; CP/M 2.28B wants slot 4.
    { "softcard2",    "SoftCard II (CP/M 2.28B)" },
    // PCPI Appli-Card — 6 MHz Z80 with its own 64 KB and a 2 KB boot ROM
    // (roms/applicard_v9.bin, ROM-gated). MAME a2applicard.cpp port.
    { "applicard",    "PCPI Applicard (CP/M 2.2)" },
    // Saturn 128K — eight 16 KB language cards; takes $D000-$FFFF over the
    // bus INH line (Memory::LcInhibitor). MAME ramcard128k.cpp port.
    { "saturn",       "Saturn 128K RAM card" },
    // CCS 7710 — a 6850 ACIA serial card; what the SoftCard II CP/M calls a
    // type 3 card (slot 1 = its buffered printer). POM2's own firmware page
    // unless CCS's two nibble PROMs are in roms/. MAME ccs7710.cpp.
    { "ccs7710",      "CCS 7710 serial (6850)" },
    // E-Z Color Graphics Interface (BYTE, Aug 1982) — a TMS9918A with 16 KB
    // of VRAM and its own monitor (Display ▸ Card Monitor). No ROM. MAME
    // ezcgi.cpp decode; the VDP is POM1's TMS9918.
    { "ezcolor",      "E-Z Color (TMS9918A)" },
    // Videx UltraTerm — 80 to 160 columns, its own video switch; ROM-gated
    // (roms/videx_ultraterm_*). MAME a2ultraterm.cpp port.
    // DAN ][ Controller (ThorstenBr) — an 8255 talking to an ATmega that
    // serves two SD cards as ProDOS volumes. POM2's own page unless the
    // card's EPROM (zlib licence) is roms/dan2_eprom.bin. izapple2 dan2.go.
    { "dan2",         "DAN ][ Controller (2 SD)" },
    { "ultraterm",    "Videx UltraTerm (80-160 col)" },
    { "chatmauve",    "Le Chat Mauve",     CardScope::Core },
    { "mouse",        "Mouse Interface"   },
    // AppleWin-style HLE variant — only needs the slot EPROM (no MCU mask
    // ROM). Different code path from "mouse" (no MC68705 emulation).
    { "mouseaw",      "Mouse (AppleWin HLE)" },
    { "mockingboard", "Mockingboard A/C",  CardScope::Core },
    // Mockingboard "C" Sound II — A/C base + SSI263A speech synth at
    // $C(s)40-$C(s)44. Drives speech in Ultima IV/V, Wasteland, Bard's
    // Tale, Crime Wave, Hudson Hawk, etc. (any title that targets the
    // Sound II variant). A/!R wired to VIA2.CA1 → IRQ-driven phoneme
    // dequeue. Phoneme PCM ported from AppleWin (GPL3 compat).
    { "mockingboard_c", "Mockingboard C (Sound II)" },
    // Phasor (Applied Engineering) — dual-mode successor to Mockingboard.
    // Starts in MB-compat mode (2 active AYs), software-switchable to
    // native (4 AYs, 12 voices, doubled chip clock), with stamped PSG audio.
    { "phasor",       "Phasor (AE)"       },
    // Cricket / Echo-class SSI263 card — standalone SSI263 speech synth
    // at $Cs00-$Cs04. Historically shipped under the "Echo+" label in
    // POM2 settings (key stays "echoplus" for back-compat) but markadev's
    // dumps confirm Street Electronics' actual Echo+ used 2× AY-3-8913
    // + TMS5220, not the SSI263. The SSI263-based product line was the
    // Cricket. Pairs with a Mockingboard A/C in another slot.
    { "echoplus",     "Cricket / Echo (SSI263)" },
    // Real Echo+: TSP5220C speech plus a single VIA driving two AY8913s.
    { "echoplus_tms", "Echo+ (TMS5220 + 2xAY)" },
    // Apple II Workstation Card — LocalTalk/AppleTalk, and a coprocessor:
    // its own 65C02 + 8530 SCC run inside the card. ROM-gated on the 64 KiB
    // 341-0358-A dump; the host handshake at $C0nX is not yet established,
    // so the guest's AppleTalk stack will not complete a transaction.
    { "workstation",  "Apple II Workstation Card (LocalTalk) — boots, does not netboot", CardScope::Frozen },
    // 4play (Lukazi, 2016) — four DIGITAL joysticks, one byte each at
    // $C0nX. The Apple game port is analogue and carries two paddles; this
    // is how an Apple II gets four players.
    { "4play",        "4play — 4 digital joysticks (Lukazi)" },
    { "snesmax",      "SNES MAX — 2 SNES gamepads (Lukazi)" },
    // TransWarp (Applied Engineering, 1986) — 3.58 MHz accelerator. No slot
    // ROM, no $C0nX window and no way to detect it by reading: it is
    // controlled entirely through $C072/$C074, which it snoops off the bus.
    // Speeds the machine's own 6502 to 3.5x (or 1.75x on the half-speed
    // DIP). MAME `bus/a2bus/transwarp.cpp` port.
    { "transwarp",    "TransWarp accelerator (Applied Engineering)" },
    // Apple II Memory Expansion Card (670-0024, "Slinky") — 1 MB behind a
    // 24-bit auto-incrementing address latch at $C0n0-$C0n3; its 341-0344A
    // firmware serves the RAM to ProDOS as a RAM disk. Needs
    // roms/slinky_341-0344a.bin. MAME `bus/a2bus/a2memexp.cpp` port.
    { "slinky",       "Memory Expansion (Slinky RAM disk)" },
    // Don't Ask Software S.A.M. — any write to $C0nX drives an 8-bit DAC;
    // the S.A.M. software synthesises speech on the 6502 and streams the
    // samples. No ROM. MAME `bus/a2bus/a2sam.cpp` port.
    { "sam",          "SAM (8-bit DAC speech)" },
};

/// Human-readable label for a card key (falls back to the key itself).
inline CardScope cardScopeForKey(std::string_view key)
{
    for (const auto& c : kCardTypes)
        if (key == c.key) return c.scope;
    return CardScope::Supported;
}

inline const char* cardLabelForKey(std::string_view key)
{
    if (key == "iicmouse") return "Apple //c IOU mouse (LLE)";
    for (const auto& ct : kCardTypes)
        if (key == ct.key) return ct.label;
    // Caller passed a key not in the catalog — return something printable.
    static thread_local std::string scratch;
    scratch.assign(key);
    return scratch.c_str();
}

// ROM-presence probes. These go through pom2::findResource(), NOT bare
// cwd-relative paths: every other ROM consumer (SlotCardFactory, the ROM
// Status panel, RomFetch's downloader — which writes into userDataDir()
// first) resolves through the resource search path, so a user who fetched
// the Mouse or CFFA dump into the user data directory saw the card greyed
// out as "(ROMs missing)" while the machine could have plugged it fine.
inline bool mouseRomsPresent()
{
    return !findResource("roms/mouse_341-0270-c.bin").empty() &&
           !findResource("roms/mouse_341-0269.bin").empty();
}

/// AppleWin-style Mouse HLE needs only the 2 KB slot EPROM (the MCU side
/// is synthesised in C++). Reuses the same `mouse_341-0270-c.bin` dump.
inline bool mouseAwRomPresent()
{
    return !findResource("roms/mouse_341-0270-c.bin").empty();
}

/// Videx Videoterm: one firmware (60 Hz clone dump or 50 Hz v2.4) plus the
/// normal character set. The inverse set is optional (derived as ~normal).
inline bool videotermRomsPresent()
{
    return (!findResource("roms/videx_videoterm_v24_60hz.bin").empty() ||
            !findResource("roms/videx_videoterm_v24_50hz.bin").empty()) &&
           !findResource("roms/videx_videoterm_char_normal.bin").empty();
}

inline bool cffaRomPresent()
{
    return !findResource("roms/cffa20ee02.bin").empty() ||
           !findResource("roms/cffa20eec02.bin").empty();
}

} // namespace pom2

#endif // POM2_SLOT_CARD_CATALOG_H
