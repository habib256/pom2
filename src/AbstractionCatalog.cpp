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

// See AbstractionCatalog.h. `docs/lle_vs_hle.md` is the reference for every
// level in the catalog below, and the two must be edited together.

#include "AbstractionCatalog.h"

#include <cstddef>

namespace pom2 {

// ─── The scale ───────────────────────────────────────────────────────────

const char* levelBadge(AbsLevel l)
{
    switch (l) {
        case AbsLevel::L0:   return "L0";
        case AbsLevel::L1:   return "L1";
        case AbsLevel::L2:   return "L2";
        case AbsLevel::H1:   return "H1";
        case AbsLevel::H2:   return "H2";
        case AbsLevel::Host: return "—";
    }
    return "?";
}

const char* levelName(AbsLevel l)
{
    switch (l) {
        case AbsLevel::L0:   return "Silicon";
        case AbsLevel::L1:   return "Chip-faithful";
        case AbsLevel::L2:   return "Real firmware, host device";
        case AbsLevel::H1:   return "Synthetic firmware";
        case AbsLevel::H2:   return "Host function";
        case AbsLevel::Host: return "Host-side machinery";
    }
    return "?";
}

bool levelIsLle(AbsLevel l)
{
    return l == AbsLevel::L0 || l == AbsLevel::L1 || l == AbsLevel::L2;
}

// ─── The catalog ─────────────────────────────────────────────────────────
//
// Mirrors the master table of `docs/lle_vs_hle.md`, with the same verdicts.
// The doc carries the evidence (file:line citations, MAME references); these
// one-liners carry only the conclusion, because a table cell the user has to
// scroll is a table cell they will not read.
//
// Grouped, not in the doc's order: the panel prints a group heading whenever
// the group changes, so each group must be ONE contiguous run — an entry
// filed among another group's rows printed its heading a second time
// (`abstraction_catalog` checks it). Every card the Slot Configuration picker
// offers must resolve to a row through `abstractionEntryForCardKey` (same
// test), or the picker shows it with no level.

const std::vector<AbsEntry>& abstractionCatalog()
{
    static const std::vector<AbsEntry> kCatalog = {
    // ── CPU & memory ─────────────────────────────────────────────────────
    { "cpu", "CPU & memory", "6502 / 65C02 / Rockwell / WDC", AbsLevel::L0,
      "Instruction-stepped; recorded Tom Harte sweep: 178 NMOS opcode files pass; "
      "WDC decimal SBC silicon-exact including interdigit carry",
      "CMOS uses WDC-style opcodes; WAI falls through after 3 cycles, STP halts until reset.", "M6502.h/.cpp" },
    { "z80", "CPU & memory", "Z80 core", AbsLevel::L0,
      "zexdoc + zexall clean; MEMPTR and X/Y flags modelled, not approximated",
      "This is the floor.", "Z80.h/.cpp" },
    { "memory", "CPU & memory", "Memory / MMU / IOU / RamWorks", AbsLevel::L1,
      "Soft switches, aux paging, LC banks, power-on 00 FF pattern, per-cycle "
      "floating bus (vapor lock)",
      "Behaviour is already at L1; only the god-object split is pending.",
      "Memory.h/.cpp" },
    { "softcard", "CPU & memory", "SoftCard Z80 (DMA bus master)", AbsLevel::L1,
      "Real DMA arbitration, 6502 halted per instruction slice; CP/M 2.2 boots "
      "identical to the MAME oracle",
      "Nothing above the arbitration is guest-visible.", "SoftCardZ80.h/.cpp" },
    { "alscpm", "CPU & memory", "ALS CP/M Card (Z80B, own 64 KB)", AbsLevel::L1,
      "Toggle, two banks, Apple window and 60 Hz clock read off ALS's own "
      "CP/M 3.01C; it boots to A> and drives an SSC through the BIOS",
      "No schematic in print: the map is inferred from the software "
      "(CARDS-011); an Apple access is one bus cycle synced to an edge.",
      "AlsCpmCard.h/.cpp" },
    { "softcard2", "CPU & memory", "SoftCard II (Z80, own 64 KB, Am2950 latch)", AbsLevel::L1,
      "Am2950 latches, status bytes and boot mode as izapple2 models them; "
      "Microsoft's CP/M 2.28B boots to A> and lists its disk",
      "No schematic or MAME model: the boot-mode WAIT is an undone and re-run "
      "instruction; the 6 MHz clock is the advertised one.",
      "SoftCardIICard.h/.cpp" },
    { "applicard", "CPU & memory", "PCPI Applicard (Z80, own 64 KB, boot ROM)", AbsLevel::L1,
      "MAME a2applicard latches, ROM overlay, $C0n5 reset and $C0n7 NMI; "
      "PCPI's CP/M 2.2 boots from the real v9 ROM",
      "The CTC socket is empty, as on the stock card.", "ApplicardCard.h/.cpp" },
    { "premium_softcard", "CPU & memory", "Premium SoftCard //e (aux slot, Z80 on aux RAM)", AbsLevel::L1,
      "$C07E register, A15 flip, status byte and the I-register call read "
      "off Microsoft's CP/M 2.25/2.26; both boot and run GBASIC",
      "No schematic: RAM contention between the Z80, the 6502 and the video "
      "is not modelled.", "PremiumSoftCard.h/.cpp" },
    { "saturn", "CPU & memory", "Saturn 128K (eight language cards over INH)", AbsLevel::L1,
      "MAME ramcard128k decode with Saturn's two-access write-enable (a write "
      "counts, manual p. 70; RAMTEST128K passes); takes "
      "$D000-$FFFF over the bus INH line, per direction",
      "Nothing above the switches is guest-visible.", "SaturnCard.h/.cpp" },
    { "slinky", "CPU & memory", "Memory Expansion Card (Slinky, 1 MB)", AbsLevel::L2,
      "MAME a2memexp: the 24-bit auto-incrementing pointer and data port; "
      "Apple's real 341-0344A ROM executes and is the RAM-disk driver",
      "ROM-gated: without the dump the slot is left empty. Only the 1 MB "
      "population is offered — how a partly populated board aliases is in no "
      "source.", "SlinkyCard.h/.cpp" },
    { "transwarp", "CPU & memory", "TransWarp accelerator (Applied Engineering)", AbsLevel::H1,
      "The accelerator as a policy: $C074 speed control, the $F000 ROM-shadow "
      "window and a slow-cycle budget; the multiplier is sampled per "
      "4096-cycle chunk (transwarp_chunk_sampling)",
      "POM2 has one CPU core and one clock: no 65C02 at 3.6 MHz on its own "
      "clock domain.", "TranswarpCard.h/.cpp" },
    { "zipchip", "CPU & memory", "Zip Chip (6502-socket accelerator)", AbsLevel::H1,
      "MAME apple2e.cpp's Zip registers at $C05A-$C05F: the $5A x4 unlock, "
      "enable, slot/speaker slow-down mask and status reads; the //c Plus's "
      "built-in 4 MHz is this same chip",
      "Same reason as the TransWarp: a speed policy on the one CPU core; the "
      "cache is not modelled.", "ZipChip.h/.cpp" },
    { "fastchip", "CPU & memory", "FASTChip //e (6502-socket accelerator)", AbsLevel::H1,
      "The User's Guide's $C06A-$C06F registers: unlock, enable, slot "
      "speeds, the 40-step speed table and the configuration pair",
      "A speed policy on the one CPU core; the slow-down lengths are the Zip "
      "Chip's, since the manual names the settings but gives no times; the "
      "RamWorks / RamFactor emulations are kept inert.", "FastChip.h/.cpp" },

    // ── Video ────────────────────────────────────────────────────────────
    { "display", "Video", "Display (beam-raced reconstruction)", AbsLevel::L1,
      "Per-byte column reconstruction from a cycle-stamped video-event log; "
      "mid-scanline mode splits at 280/560 px",
      "A true per-scanline incremental renderer (MAME style) is the remaining "
      "L0 step — it would fix the unidirectional mid-frame page-split limit.",
      "Apple2Display.h/.cpp" },
    { "oe", "Video", "Composite NTSC (OpenEmulator)", AbsLevel::L1,
      "14.318 MHz 1-bit signal → FIR demodulation (Y @ 2.0, C @ 0.6 MHz) → "
      "YUV→RGB, PAL line-phase alternation",
      "Pure-analog IIR-on-signal is deferred as academic (5-10 d).",
      "NtscPostProcessor.*, OpenGLShader.*" },
    { "lut", "Video", "Artifact-colour LUT modes", AbsLevel::H1,
      "MAME dot-pattern tables or AppleWin's IIR-derived phase/history LUT — "
      "NTSC decoding is precomputed, then looked up during rendering",
      "Not a defect: it is the cheap path, and the OE pipeline beside it is "
      "the low-level one. Switchable above.",
      "Apple2Display.cpp, AppleWinNtsc.h/.cpp" },
    { "chatmauve", "Video", "Le Chat Mauve RGB", AbsLevel::L1,
      "Patent 2-bit mode latch; Feline/IIc LCM HGR + mixed DHGR == AppleWin "
      "RGBMonitor (pinned chatmauve_dot_rules); Eve $C0B0-$C0BF + CPREG "
      "aux-shadow + table IX-1 from Purplesoft; Video-7 keeps 160 chunky",
      "Not the PLA and not a 14 MHz tap: mid-line switches land at the frame "
      "(P6), Eve DASH/COL280 still prose (P3), IIc 80COL is read not inferred "
      "(P5); the RVB Graph's control and colour registers come from its "
      "manual (P4). docs/chatmauve_plan.md",
      "LeChatMauveCard.h/.cpp, Apple2Display.cpp" },
    { "ezcolor", "Video", "E-Z Color (TMS9918A VDP)", AbsLevel::L1,
      "MAME ezcgi decode ($C0n0 data, $C0n1 register, /INT = slot IRQ); "
      "POM1's TMS9918: four modes, sprites with 5th-sprite and collision "
      "flags, the access window, a progressive raster synced to the CPU's "
      "in-flight cycle",
      "The card's composite encoder is not modelled: the picture is the "
      "VDP's RGB palette.", "EzColorCard.h/.cpp, TMS9918.h/.cpp" },
    { "ultraterm", "Video", "Videx UltraTerm (80-160 columns)", AbsLevel::L1,
      "MAME a2ultraterm bus map, VRAM pages and attributes on the HD6845S; "
      "the real firmware's PR#3 drives it",
      "The 6845 frame is painted whole, not raster by raster.",
      "VidexUltratermCard.h/.cpp" },
    { "videoterm", "Video", "Videx Videoterm (80 columns, ][ / ][+)", AbsLevel::L2,
      "MAME a2videoterm: the real Videoterm firmware executes over an HD6845S "
      "register file and 2 KB of banked VRAM, drawn through the card's own "
      "character ROMs; replaces the picture while TEXT + AN0",
      "ROM-gated. The CRTC has no raster timer: the frame is painted whole, "
      "the blink follows emulated time.",
      "VidexVideotermCard.h/.cpp, Hd6845Crtc.h/.cpp" },
    { "vidhd", "Video", "VidHD (IIgs Super Hi-Res on a //e)", AbsLevel::H1,
      "AppleWin VidHD.cpp: the card's contract — the $Cn00-02 signature, "
      "$C022/$C029/$C034/$C035 latched off motherboard writes, SHR painted "
      "whole from the aux bank",
      "The FPGA is not modelled: no raster effects, no IIgs text/border "
      "colours, no extended text modes.", "VidHDCard.h/.cpp" },

    // ── Audio ────────────────────────────────────────────────────────────
    { "speaker", "Audio", "Speaker", AbsLevel::L0,
      "Verbatim MAME spkrdev.cpp: 4x oversample, 64-tap windowed sinc, "
      "0.995-pole DC blocker",
      "This is the floor.", "SpeakerDevice.*" },
    { "cassette", "Audio", "Cassette", AbsLevel::L1,
      "Real $C020 flip-flop and $C060 comparator sign; the guest's own Monitor "
      "loops time real zero-crossings out of a host WAV",
      "The tape itself is analog and lives on the host.", "CassetteDevice.*" },
    { "mockingboard", "Audio", "Mockingboard / Phasor (6522 + AY-3-8910)",
      AbsLevel::L1,
      "T1/T2, IFR/IER, port latches + DDR, CA1 edges; AY counters, LFSR and "
      "envelope generator",
      "Documented skips (shift register, CA2/CB1/CB2 handshake, PB6 pulse "
      "counting) are wired by no POM2 card.",
      "Mockingboard.*, Via6522.h, Ay3_8910.h" },
    { "ssi263", "Audio", "SSI263 speech", AbsLevel::H1,
      "AppleWin-derived registers, A/!R handshake, IRQ modes and PCM durations; "
      "speech uses fixed phoneme samples rather than analog formant synthesis",
      "The real chip is an analog formant synth. AppleWin's blob is the only "
      "extant reference — MAME has no SSI263 at all.",
      "Ssi263.*, Ssi263PhonemeData.*" },
    { "tms5220", "Audio", "Echo+ TMS5220C + 2xAY", AbsLevel::L1,
      "TMS5220C FIFO, LPC interpolation, chirp/noise, lattice filter and DAC; single VIA and two AY8913s",
      "C0n0 commands are accepted immediately; analog board levels and physical hardware captures remain unvalidated.",
      "EchoPlusTMS5220Card.*, EchoPlusTMS5220Snapshot.cpp, Tms5220.*" },
    { "sam", "Audio", "S.A.M. (8-bit DAC speech)", AbsLevel::L1,
      "MAME a2sam: any $C0nX write drives the DAC, stamped with the exact "
      "CPU cycle it lands on; the speech is S.A.M.'s own 6502 code",
      "Complete: no ROM, no readable state — the card's fidelity is the "
      "write timing.", "SamCard.h/.cpp, Dac8Audio.h/.cpp" },
    { "porteparole", "Audio", "Porte Parole (Ediciel, MEA8000 French speech)", AbsLevel::L1,
      "The card's $C0n0 data/status and $C0n1 command from its manual; the "
      "unmodified MEA8000 engine by Sébastien Pourcel (PorteParole-MEA8000), "
      "clocked at 4 MHz; status D7 only, D0-D6 floating",
      "No ROM to run (the driver is on the disk). The optional 2 KB memory "
      "socket and the chip's coefficient quantisation are not modelled.",
      "PorteParoleCard.h/.cpp, Mea8000.h/.cpp" },
    { "floppysnd", "Audio", "Floppy mechanical sounds", AbsLevel::H2,
      "Host sample playback driven by emuCycles-stamped phase strobes",
      "There is nothing on the bus to model — it is literally acoustics.",
      "FloppySoundDevice.*" },

    // ── Storage ──────────────────────────────────────────────────────────
    { "diskimage", "Storage", "DiskImage / WOZ", AbsLevel::L0,
      "Bit-cell / flux-transition store; getNextTransition is verbatim MAME "
      "floppy.cpp",
      "A .dsk has no flux, so its bitstream is RECONSTRUCTED (sync-FF padding "
      ">= 5) — exactly what real hardware infers.",
      "DiskImage.*" },
    { "diskii", "Storage", "Disk II controller", AbsLevel::L0,
      "Real 341-0028-A P6 LSS PROM indexed per LSS cycle, real P5A boot PROM, "
      "per-drive angular position",
      "The legacy 32-cycle nibble gate is the H1 fallback, used only when "
      "roms/diskii_p6.rom is absent.",
      "DiskIICard.*" },
    { "iwm", "Storage", "IWM", AbsLevel::L0,
      "Verbatim MAME machine/iwm.cpp: m_active / m_rw / read-walker / "
      "write-window state machines",
      "Only the sub-CPU-cycle Q3 phase is unmodelled.", "IWMDevice.*" },
    { "sony35", "Storage", "SmartPort hub / Sony 3.5\" drive", AbsLevel::L0,
      "Zoned GCR, LSTRB register strobes, DSKCHG latch polarity per MAME "
      "floppy.cpp",
      "Nothing above it is abstracted.", "SmartPortHub.*, Sony35Drive.*" },
    { "cffa", "Storage", "CFFA 2.0 (IDE)", AbsLevel::L2,
      "The real 4 KB firmware dump EXECUTES over an ATA taskfile model "
      "isomorphic to MAME's cs0_r/cs0_w",
      "The ATA layer skips DMA / IRQ / SMART; CHD backing is out of scope.",
      "CffaCard.*, AtaBlockDevice.*" },
    { "cffa3", "Storage", "CFFA3000 (SmartPort)", AbsLevel::H1,
      "Synthetic SmartPort firmware serves eight disk-image units; the "
      "optional floppy slot uses a separate Disk II card",
      "AVR firmware, dynamic slot RAM, CF/USB menus and slot ghosting are "
      "outside this block-level model.", "Cffa3000Card.h, SmartPortCard.*" },
    { "superdrive", "Storage", "SuperDrive (ProDOS block service)", AbsLevel::H1,
      "A SmartPort block service exposes 400/800K GCR and 1.44 MB MFM images; "
      "the 341-0438-A card ROM is not executed",
      "SWIM registers and the card's 65C02 firmware are not emulated.",
      "SmartPortCard.*" },
    { "hdv", "Storage", "ProDOS HDV card", AbsLevel::H1,
      "Hand-assembled 256 B slot ROM + an invented 4-register streaming port; "
      "block moves are a host memcpy. No GCR, no flux, no ATA",
      "Deliberate: it mounts .hdv/.2mg directly with NO card ROM dump "
      "required. H1 is the feature.",
      "ProDOSHardDiskCard.*" },
    { "smartportcard", "Storage", "SmartPort card (Liron-class)", AbsLevel::L2,
      "The whole 256 B slot page and the 2 KB $C800 bank come from the real "
      "roms/liron.rom; only the service entries are overlaid onto POM2's own "
      "SmartPort handler",
      "The separate Liron card executes controller firmware over an IWM; "
      "the UniDisk drive-side 65C02 is represented by its protocol.",
      "SmartPortCard.*" },
    { "liron", "Storage", "Liron 3.5-inch controller", AbsLevel::L2,
      "The real 4 KB ROM executes over an IWM device and boots through the "
      "SmartPort bus protocol",
      "The UniDisk drive-side 65C02 is represented by a protocol responder.",
      "LironCard.*, SmartPortBusDevice.*" },
    { "iicsp", "Storage", "16 KB //c SmartPort fallback", AbsLevel::H1,
      "ROM 255 lacks SmartPort firmware. Explicit boot exposes POM2's $C500 "
      "stub and its $C800 expansion bank through forced INTCXROM; "
      "iicCardWindow_ gates the expansion window",
      "The 32 KB //c and //c+ instead run their own firmware over the external "
      "SmartPort bus responder; the //c+ boots slot 5 through its reset scan.",
      "Memory.cpp, SmartPortCard.*" },
    { "dan2", "Storage", "DAN ][ Controller — the card's EPROM", AbsLevel::L2,
      "The real 512-byte EPROM executes (both pages, switched by the 8255's "
      "port C bit 0) against an 8255 in mode 2 and the ATmega's command set "
      "(izapple2 dan2.go): status, read, write, SAFEREAD, version",
      "The ATmega is answered at the protocol, not executed; the line is "
      "infinitely fast; one image per SD slot (the volume commands report 1) "
      "and its boot-menu program is not served.", "Dan2Card.h/.cpp" },
    { "dan2_pom2", "Storage", "DAN ][ Controller — POM2's firmware page", AbsLevel::H1,
      "Same card and protocol; the $Cn00 page is POM2's: the EPROM's ID "
      "bytes, a SAFEREAD boot of block 0 and a ProDOS driver at $CnFF. "
      "dan2_card boots ProDOS on both and compares every driver call",
      "No boot menu, one page only.", "Dan2Card.cpp" },
    { "prodosvol", "Storage", "ProDOS host folder", AbsLevel::H1,
      "POM2 FABRICATES a valid ProDOS volume image once; from then on the "
      "guest does genuine block reads through the real filesystem code",
      "The fabrication IS the abstraction — nothing below it is faked.",
      "ProDOSVolume.*" },

    // ── Network & serial ─────────────────────────────────────────────────
    { "ssc", "Network & serial", "Super Serial Card", AbsLevel::L2,
      "6551 ACIA is register-faithful (L1) and Apple's 341-0065-A EPROM "
      "EXECUTES (roms/ssc_341-0065-a.bin, pinned ssc_firmware): DIP banks, "
      "cable lines, $C800 bank",
      "Without the dump the card falls back to the hand-assembled page — "
      "PR#n / IN#n hooks plus a Pascal 1.1 ID block (H1, shown as degraded). "
      "The host side is a telnet bridge or a wiretap, not a DB-25.",
      "SuperSerialCard.h/.cpp" },
    { "ccs7710", "Network & serial", "CCS 7710 (6850) — CCS's 7710-03 PROMs", AbsLevel::L2,
      "The real firmware executes on MAME ccs7710's decode (A0 only) and a "
      "6850 with its status, master reset and interrupt enables",
      "The line is infinitely fast (TDRE always set) and goes to a spool, "
      "not a host port.", "Ccs7710Card.h/.cpp" },
    { "ccs7710_pom2", "Network & serial", "CCS 7710 (6850) — POM2's firmware page", AbsLevel::H1,
      "Same card; the $Cn00 page is POM2's, written to the 7710-03's "
      "behaviour: ID bytes, ACIA set-up, 8-bit output with echo, IN#n with "
      "its flashing cursor, terminal mode and the Ctrl-A / Ctrl-R / Ctrl-T "
      "commands. ccs7710_card runs both and compares line, screens, control",
      "Its own code: the timing of its loops is not the PROM's, so RNDL "
      "($4E/$4F) counts a different number of steps while input waits.",
      "Ccs7710Card.cpp" },
    { "uthernet", "Network & serial", "Uthernet I (CS8900A)", AbsLevel::L1,
      "Verbatim MAME machine/cs8900a.cpp (VICE lineage), packet-level",
      "RX is pull-mode: POM2 has no device_network_interface push bus.",
      "UthernetCard.*, Cs8900aDevice.*" },
    { "uthernet2", "Network & serial", "Uthernet II (W5100)", AbsLevel::L1,
      "Register/socket model per AppleWin + the WIZnet datasheet; each W5100 "
      "socket owns a real host BSD socket",
      "The chip IS a TCP/IP offload engine — host sockets are the faithful "
      "model, not a shortcut. Looks like HLE; is not.",
      "UthernetIICard.*, W5100Device.*" },
    { "workstation", "Network & serial", "Apple II Workstation Card (LocalTalk)", AbsLevel::L0,
      "A second 65C02 runs the card's real dumped firmware over its own "
      "Memory::ForeignBus map, with its own RAM and a Z8530 SCC; boots its "
      "self-test and configures the SCC",
      "The level is high, the reach is zero: LocalTalk has no peer on the "
      "other end of the wire.", "WorkstationCard.h/.cpp, Scc8530Device.h/.cpp" },
    { "fujinet", "Network & serial", "FujiNet relay", AbsLevel::H1,
      "Synthetic slot ROM relays peer-backed calls over SP-over-SLIP; "
      "controller count and the optional HTTP N: service are local",
      "Peer functions stay off-box; local N: is plain HTTP, not full "
      "FujiNet firmware (no TLS, SSH or JSON parser).",
      "FujiNetCard.*, SpOverSlipLink.*" },
    { "netbackend", "Network & serial", "Ethernet host transport", AbsLevel::H2,
      "Null / Loopback / libslirp user-mode NAT",
      "Outbound-only by design: no root, no TAP, no pcap.",
      "NetworkBackend.h, SlirpNetworkBackend.*" },

    // ── Printing ─────────────────────────────────────────────────────────
    { "printercard", "Printing", "Printer card (parallel)", AbsLevel::H1,
      "Synthetic ROM: the PR#n CSWL/CSWH hook, a 4-byte trampoline and a "
      "Pascal 1.1 ID block whose four entries are real (PascalPrinterRom.h); "
      "the data port spools to a std::vector",
      "No PROM dump exists to run — there is no one card this models. "
      "BASIC PR#n and Pascal / ProDOS drivers both bind.",
      "PrinterCard.h/.cpp" },
    { "grappler", "Printing", "Grappler+ (Orange Micro)", AbsLevel::L2,
      "The real 4 KB Orange Micro EPROM EXECUTES; status byte, register "
      "decode, $C800 banking and S1 DIPs line-cited against MAME grappler.cpp",
      "The /STROBE 7-clock pulse is collapsed to instant — the synthetic "
      "printer consumes at latch time, so no observer exists.",
      "GrapplerCard.h/.cpp" },
    { "grappler1", "Printing", "Grappler (1981, Orange Micro)", AbsLevel::L2,
      "MAME grappler.cpp: the real 2 KB eps-1 EPROM executes, one $Cn00 page "
      "per slot, the whole ROM at $C800; status, strobe and ACK latch",
      "ROM-gated. The printer behind it is a Centronics model: instant when "
      "ready, the byte held while not.",
      "GrapplerClassicCard.h/.cpp, CentronicsPrinter.h/.cpp" },
    { "pic", "Printing", "Apple Parallel Interface (341-0057)", AbsLevel::L2,
      "MAME a2pic: the real 512-byte PROM executes, both firmwares selected "
      "by SW1:6; status, ACK latch, autostrobe, IRQ enable",
      "ROM-gated. Not modelled: the strobe-length timer, the polarity "
      "switches, the X3/X5 jumpers.",
      "AppleParallelCard.h/.cpp, CentronicsPrinter.h/.cpp" },
    { "imagewriter", "Printing", "ImageWriter II printer", AbsLevel::H2,
      "Host-side printer: full control language, 4-band ribbon, 8/24-pin bit "
      "images, PNG/PDF export. Not a bus device at all",
      "There is no Apple II hardware here — the printer sat on the far side "
      "of a cable.",
      "ImageWriter.*, ImageWriterPdf.*" },

    // ── Input & clocks ───────────────────────────────────────────────────
    { "mouse", "Input & clocks", "Mouse Card — MAME", AbsLevel::L0,
      "An M68705P3 MCU EXECUTING its real 2 KB mask ROM at 2x CPU clock, plus "
      "an MC6821 PIA and quadrature edge generation",
      "Only the PAL16R4 chip-select sequencer is skipped (firmware-invisible).",
      "MouseCard.*" },
    { "iicmouse", "Input & clocks", "Apple //c IOU mouse", AbsLevel::L1,
      "Unmodified system ROM over IOU axis/edge/IRQ registers",
      "MAME input sampling: one X0/Y0 transition per 65 CPU cycles; no MCU or ROM patch.",
      "IIcMouse.*, Memory.cpp" },
    { "mouseaw", "Input & clocks", "Mouse Card — AppleWin HLE", AbsLevel::H1,
      "Same slot EPROM, but the MCU is a C++ command-byte state machine; the "
      "position is copied from the host delta",
      "Ships BECAUSE the MCU mask ROM is not always available. Smoother and "
      "less correct: no quadrature rate limit.",
      "MouseCardAppleWin.*" },
    { "joystick", "Input & clocks", "Joystick / paddles", AbsLevel::L1,
      "Real $C070 RC discharge timing, sampled at $C064-$C067 bit 7",
      "Nothing below the timing is guest-visible.", "JoystickInput.h/.cpp" },
    { "4play", "Input & clocks", "4play (four digital joysticks)", AbsLevel::L1,
      "Lukazi's card: the digital read registers, one per joystick",
      "Complete: there is no analogue path on the card to model.",
      "FourPlayCard.h/.cpp" },
    { "snesmax", "Input & clocks", "SNES MAX (two SNES gamepads)", AbsLevel::L1,
      "MAME snesmax: latch, the seven-clock pulse and the two serial "
      "shifters, as the pads' own protocol; $C0n0 drives bits 7-6 only, the "
      "rest is the floating bus (AppleWin, by the card's designer)",
      "Complete: nothing below the shift registers is guest-visible.",
      "SnesMaxCard.h/.cpp" },
    { "clock", "Input & clocks", "ProDOS clock card (ThunderClock+)",
      AbsLevel::L2,
      "uPD1990AC bit-bang state machine per MAME upd1990a.cpp, driving the "
      "REAL Thunderware Rev 1.3 EPROM",
      "Already there. The dump even settled the 40-vs-48-bit shift-register "
      "question by disassembly.",
      "ClockCard.h/.cpp" },
    { "nsclock", "Input & clocks", "No-Slot Clock (DS1216E)", AbsLevel::L1,
      "The full 64-bit pattern-match state machine on Memory::interceptRead",
      "Nothing below the pattern match exists.", "NoSlotClock.h/.cpp" },

    // ── Off-axis ─────────────────────────────────────────────────────────
    { "rewind", "Host-side (off-axis)", "Rewind / snapshot", AbsLevel::Host,
      "Keyframes + XOR deltas at frame boundaries — reaching into VIA, AY and "
      "SSI263 state so music and speech survive a rewind",
      "No hardware referent: real Apple IIs do not rewind.",
      "RewindBuffer.*, MachineSnapshot.*" },
    { "turbo", "Host-side (off-axis)", "Disk turbo (~60x) and MAX speed",
      AbsLevel::Host,
      "Pure host pacing — and precisely why emuCycles stamping is mandatory "
      "everywhere: turbo collapses wall-clock gaps to zero",
      "No hardware referent.", "EmulationController.h/.cpp" },
    { "aiserver", "Host-side (off-axis)", "AI control server", AbsLevel::Host,
      "Out-of-band agent channel over loopback HTTP", "No hardware referent.",
      "AiControlServer.h/.cpp" },
    { "crt", "Host-side (off-axis)", "CRT effect stack / 3D voxel / Paint",
      AbsLevel::Host,
      "Presentation and authoring layers above the framebuffer",
      "No hardware referent — a real CRT's glass is not a bus device.",
      "CrtEffectStack.*, Voxel3DRenderer.*, hgrpaint/*" },
    { "bootfromslot", "Host-side (off-axis)", "bootFromSlot shortcut",
      AbsLevel::Host,
      "Usually cold boot plus PC = $Cn00 after validating the JSR trio; "
      "//c+ slot 5 instead resets through the real firmware's SmartPort scan",
      "Labelled a synthetic shortcut in EmulationController.h.",
      "EmulationController.cpp" },
    };
    return kCatalog;
}

const AbsEntry* abstractionEntryForCardKey(std::string_view key)
{
    if (key.empty()) return nullptr;
    std::string_view id = key;
    if      (key == "smartport35" || key == "superdrive") id = "smartportcard";
    else if (key == "printer")        id = "printercard";
    else if (key == "mockingboard_c") id = "mockingboard";
    else if (key == "phasor")         id = "mockingboard";
    else if (key == "echoplus")       id = "ssi263";
    else if (key == "echoplus_tms")   id = "tms5220";
    for (const AbsEntry& e : abstractionCatalog())
        if (id == e.id) return &e;
    return nullptr;
}

bool slotPlanKeyLive(const std::array<std::string, 8>& plan,
                     const std::array<bool, 8>& occupied,
                     std::string_view key)
{
    if (key.empty()) return false;
    for (std::size_t s = 1; s <= 7; ++s)
        if (plan[s] == key && occupied[s]) return true;
    return false;
}

} // namespace pom2
