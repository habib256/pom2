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

// AbstractionLevels_ImGui — implementation. See the header for why this
// window exists; `docs/lle_vs_hle.md` is the reference for every level in
// the catalog below, and the two must be edited together.

#include "AbstractionLevels_ImGui.h"

#include "Pom2Theme.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

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

namespace {

// Colour per level. Deliberately NOT a good/bad ramp: H1 is the right answer
// for a card with no public ROM dump, and painting it red would be a lie
// about POM2's own decision rule. Green→blue→cyan walks *down* the stack,
// amber marks "the boundary is the service", grey means off-axis.
ImU32 levelColour(AbsLevel l)
{
    const Palette& p = palette();
    switch (l) {
        case AbsLevel::L0:   return p.ok;
        case AbsLevel::L1:   return p.info;
        case AbsLevel::L2:   return p.accent;
        case AbsLevel::H1:   return p.warn;
        case AbsLevel::H2:   return p.textDim;
        case AbsLevel::Host: return p.textDim;
    }
    return p.text;
}

// A filled chip rather than coloured text: the level is the column the eye
// scans, and at a glance a chip separates from prose in a way a coloured
// word does not. Drawn by hand because ImGui has no badge primitive.
void levelChip(AbsLevel l)
{
    const ImU32 col = levelColour(l);
    const char* txt = levelBadge(l);
    const ImVec2 sz = ImGui::CalcTextSize(txt);
    const ImVec2 pad(ImGui::GetStyle().FramePadding.x, 1.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + sz.x + pad.x * 2.0f, p0.y + sz.y + pad.y * 2.0f);

    ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(f.x, f.y, f.z, 0.18f)),
                      3.0f);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(
                            ImVec4(f.x, f.y, f.z, 0.55f)),
                3.0f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), col, txt);
    ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

} // namespace

// ─── The catalog ─────────────────────────────────────────────────────────
//
// Mirrors the master table of `docs/lle_vs_hle.md`, in the same order and
// with the same verdicts. The doc carries the evidence (file:line citations,
// MAME references); these one-liners carry only the conclusion, because a
// table cell the user has to scroll is a table cell they will not read.

const std::vector<AbsEntry>& abstractionCatalog()
{
    static const std::vector<AbsEntry> kCatalog = {
    // ── CPU & memory ────────────────────────────────────────────────────
    { "cpu", "CPU & memory", "6502 / 65C02 / Rockwell / WDC", AbsLevel::L0,
      "Per-cycle; 100% of the 178 documented NMOS opcodes vs Tom Harte 65x02; "
      "WDC decimal SBC silicon-exact including interdigit carry",
      "This is the floor.", "M6502.h/.cpp" },
    { "z80", "CPU & memory", "Z80 core", AbsLevel::L0,
      "zexdoc + zexall clean; MEMPTR and X/Y flags modelled, not approximated",
      "This is the floor.", "Z80.h/.cpp" },
    { "softcard", "CPU & memory", "SoftCard Z80 (DMA bus master)", AbsLevel::L1,
      "Real DMA arbitration, 6502 halted per instruction slice; CP/M 2.2 boots "
      "identical to the MAME oracle",
      "Nothing above the arbitration is guest-visible.", "SoftCardZ80.h/.cpp" },
    { "memory", "CPU & memory", "Memory / MMU / IOU / RamWorks", AbsLevel::L1,
      "Soft switches, aux paging, LC banks, power-on 00 FF pattern, per-cycle "
      "floating bus (vapor lock)",
      "Behaviour is already at L1; only the god-object split is pending.",
      "Memory.h/.cpp" },

    // ── Video ───────────────────────────────────────────────────────────
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
      "MAME's composite colour tables indexed per dot pattern — the RESULT of "
      "NTSC artifacting, tabulated, with no signal in between",
      "Not a defect: it is the cheap path, and the OE pipeline beside it is "
      "the low-level one. Switchable above.",
      "Apple2Display.cpp, AppleWinNtsc.h/.cpp" },
    { "chatmauve", "Video", "Le Chat Mauve RGB", AbsLevel::L1,
      "Patent 2-bit mode latch; Feline/IIc LCM HGR + mixed DHGR == AppleWin "
      "RGBMonitor (pinned chatmauve_dot_rules); Eve $C0B0-$C0BF + CPREG "
      "aux-shadow + table IX-1 from Purplesoft; Video-7 keeps 160 chunky",
      "Not the PLA and not a 14 MHz tap: mid-line switches land at the frame "
      "(P6), Eve DASH/COL280 still prose (P3), IIc 80COL is read not inferred "
      "(P5), RVB Graph absent (P4). docs/chatmauve_plan.md",
      "LeChatMauveCard.h/.cpp, Apple2Display.cpp" },

    // ── Audio ───────────────────────────────────────────────────────────
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
      "Registers, A/!R handshake, IRQ modes and phoneme DURATION are "
      "chip-exact (L1); the sound itself is a canned PCM blob per phoneme",
      "The real chip is an analog formant synth. AppleWin's blob is the only "
      "extant reference — MAME has no SSI263 at all.",
      "Ssi263.*, Ssi263PhonemeData.*" },
    { "tms5220", "Audio", "Echo+ TMS5220 (scaffold)", AbsLevel::H1,
      "Stub register decode at $Cs00-$Cs0F, enough for driver detection",
      "The LPC10 decoder (chirp ROM + K-parameter interpolation) and the "
      "AY-3-8913 synth are not written yet — ~3-5 d.",
      "EchoPlusTMS5220Card.*" },
    { "floppysnd", "Audio", "Floppy mechanical sounds", AbsLevel::H2,
      "Host sample playback driven by emuCycles-stamped phase strobes",
      "There is nothing on the bus to model — it is literally acoustics.",
      "FloppySoundDevice.*" },

    // ── Storage ─────────────────────────────────────────────────────────
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
      "The ATA layer skips DMA / IRQ / SMART; CHD backing is phase 2.",
      "CffaCard.*, AtaBlockDevice.*" },
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
      "Full Liron LLE needs the IWM bit-shifter AND the UniDisk drive-side "
      "65C02 — out of scope.",
      "SmartPortCard.*" },
    { "iicsp", "Storage", "//c-class on-board SmartPort", AbsLevel::H1,
      "A $C500-$C5FF hole punched through the //c's forced INTCXROM, armed "
      "only by an explicit GUI/CLI boot — plus the stub's $C800 expansion "
      "bank, gated by the iicCardWindow_ execution-flow heuristic. ProDOS 8 "
      "2.4.3 boots and runs end-to-end",
      "A real //c masks all slot ROM, and MAME models no 3.5\" on a plain //c. "
      "An HLE only works once it is complete — bus etiquette included "
      "(docs/lle_vs_hle.md, the case study).",
      "Memory.cpp, SmartPortCard.*" },
    { "prodosvol", "Storage", "ProDOS host folder", AbsLevel::H1,
      "POM2 FABRICATES a valid ProDOS volume image once; from then on the "
      "guest does genuine block reads through the real filesystem code",
      "The fabrication IS the abstraction — nothing below it is faked.",
      "ProDOSVolume.*" },

    // ── Network & serial ────────────────────────────────────────────────
    { "ssc", "Network & serial", "Super Serial Card", AbsLevel::H1,
      "6551 ACIA is register-faithful (L1); the slot ROM is synthetic — PR#n / "
      "IN#n hooks plus a Pascal 1.1 ID block",
      "The chip is right; the firmware is a stub because no dump is bundled. "
      "The real 341-0065-A is publicly dumped — this is a sourcing job.",
      "SuperSerialCard.h/.cpp" },
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
    { "fujinet", "Network & serial", "FujiNet relay", AbsLevel::H1,
      "Synthetic 256 B slot ROM whose only job is to trap into the host; every "
      "SmartPort call is forwarded verbatim over SP-over-SLIP",
      "Nothing below the protocol exists to model — the device is real and "
      "off-box.",
      "FujiNetCard.*, SpOverSlipLink.*" },
    { "netbackend", "Network & serial", "Ethernet host transport", AbsLevel::H2,
      "Null / Loopback / libslirp user-mode NAT",
      "Outbound-only by design: no root, no TAP, no pcap.",
      "NetworkBackend.h, SlirpNetworkBackend.*" },

    // ── Printing ────────────────────────────────────────────────────────
    { "printercard", "Printing", "Printer card (parallel)", AbsLevel::H1,
      "Synthetic ROM whose entire job is the PR#n CSWL/CSWH hook plus a 4-byte "
      "trampoline; the data port spools to a std::vector",
      "No PROM dump exists to run. The Pascal entry block is deliberately "
      "absent, so Pascal drivers cannot bind — BASIC PR#n only.",
      "PrinterCard.h/.cpp" },
    { "grappler", "Printing", "Grappler+ (Orange Micro)", AbsLevel::L2,
      "The real 4 KB Orange Micro EPROM EXECUTES; status byte, register "
      "decode, $C800 banking and S1 DIPs line-cited against MAME grappler.cpp",
      "The /STROBE 7-clock pulse is collapsed to instant — the synthetic "
      "printer consumes at latch time, so no observer exists.",
      "GrapplerCard.h/.cpp" },
    { "imagewriter", "Printing", "ImageWriter II printer", AbsLevel::H2,
      "Host-side printer: full control language, 4-band ribbon, 8/24-pin bit "
      "images, PNG/PDF export. Not a bus device at all",
      "There is no Apple II hardware here — the printer sat on the far side "
      "of a cable.",
      "ImageWriter.*, ImageWriterPdf.*" },

    // ── Input & clocks ──────────────────────────────────────────────────
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

    // ── Off-axis ────────────────────────────────────────────────────────
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
      "Cold boot plus a forced PC = $Cn00 after validating the JSR trio — no "
      "real firmware scan happens",
      "Labelled a synthetic shortcut in EmulationController.h.",
      "EmulationController.cpp" },
    };
    return kCatalog;
}

// ─── The window ──────────────────────────────────────────────────────────

AbstractionLevels_ImGui::Request
AbstractionLevels_ImGui::render(bool* open, const Snapshot& snap)
{
    Request req;
    if (!open || !*open) return req;

    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_LAYER_GROUP " Abstraction Levels (LLE / HLE)"
                      "###abstractionLevels", open)) {
        ImGui::End();
        return req;
    }

    const Palette& pal = palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    // ── Legende : les cinq niveaux, tout le discours en infobulle ───────
    // La fenetre etait deux paragraphes, une section de commutateurs, trois
    // filtres et une table de prose. Elle est maintenant UNE table ; le
    // raisonnement complet vit dans docs/lle_vs_hle.md et les infobulles.
    {
        static const AbsLevel kAll[] = { AbsLevel::L0, AbsLevel::L1,
                                         AbsLevel::L2, AbsLevel::H1,
                                         AbsLevel::H2 };
        static const char* kTips[] = {
            "Silicon. Internal state machine + cycle timing modelled; a real\n"
            "ROM/firmware dump executes on top and cannot tell the difference.",
            "Chip-faithful. Full register/protocol model at bus timing;\n"
            "firmware-invisible internals deliberately skipped.",
            "Real firmware, host device. The card's real ROM executes, but\n"
            "what it drives is a host implementation.",
            "Synthetic firmware. POM2 hand-assembles a slot ROM: the guest\n"
            "6502 code is real, the register protocol behind it is invented.",
            "Host function. No guest-visible hardware at all."
        };
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine(0.0f, 10.0f);
            levelChip(kAll[i]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTips[i]);
        }
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextDisabled("LLE = the chip's pins, HLE = the service. "
                            "Full doc: docs/lle_vs_hle.md");
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Filter...",
                             search_, sizeof(search_));

    // Lignes vivantes indexees une fois par frame.
    std::unordered_map<std::string, const Row*> live;
    for (const Row& r : snap.rows) live[r.id] = &r;

    // Les quatre frontieres commutables, indexees par id de catalogue :
    // quand une ligne appartient a une paire, son bouton vit DANS la ligne.
    struct PairSide { const ToggleState* t; int idx; };
    std::unordered_map<std::string, PairSide> pair;
    for (const ToggleState& t : snap.toggles) {
        const char* lo = nullptr; const char* hi = nullptr;
        switch (t.id) {
            case AbsToggle::MouseCard:      lo = "mouse";    hi = "mouseaw";     break;
            case AbsToggle::BlockStorage:   lo = "cffa";     hi = "hdv";         break;
            case AbsToggle::PrinterIface:   lo = "grappler"; hi = "printercard"; break;
            case AbsToggle::CompositeVideo: lo = "oe";       hi = "lut";         break;
            case AbsToggle::None:           break;
        }
        if (lo) { pair[lo] = { &t, 0 }; pair[hi] = { &t, 1 }; }
    }

    auto matches = [&](const AbsEntry& e) {
        if (!search_[0]) return true;
        auto hay = std::string(e.subsystem) + " " + e.modelled + " " +
                   e.group + " " + e.files;
        std::string needle(search_);
        auto lower = [](std::string v) {
            for (char& c : v) c = static_cast<char>(std::tolower(
                                      static_cast<unsigned char>(c)));
            return v;
        };
        return lower(hay).find(lower(needle)) != std::string::npos;
    };

    int degraded = 0;

    if (ImGui::BeginTable("##abs", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Level",  ImGuiTableColumnFlags_WidthFixed,
                                3.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Now",    ImGuiTableColumnFlags_WidthFixed,
                                7.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const char* group = nullptr;
        for (const AbsEntry& e : abstractionCatalog()) {
            auto it = live.find(e.id);
            const Row* r = (it != live.end()) ? it->second : nullptr;
            if (!matches(e)) continue;
            if (r && r->live == Live::Degraded) ++degraded;

            if (!group || std::strcmp(group, e.group) != 0) {
                group = e.group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(u32(pal.accentDim), "%s", group);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            levelChip(e.level);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.subsystem);
            if (ImGui::IsItemHovered()) {
                // Toute la prose de l'ancienne table vit ici.
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(30.0f * ImGui::GetFontSize());
                ImGui::TextColored(u32(levelColour(e.level)), "%s — %s",
                                   levelBadge(e.level), levelName(e.level));
                ImGui::Separator();
                ImGui::TextWrapped("%s", e.modelled);
                ImGui::Spacing();
                ImGui::TextWrapped("Why not lower: %s", e.whyNot);
                ImGui::Spacing();
                ImGui::TextColored(u32(pal.textDim), "%s", e.files);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(2);
            if (!r || r->live == Live::NotApplicable ||
                r->live == Live::Active) {
                ImGui::TextColored(u32(pal.ok), ICON_FA_CIRCLE_CHECK " live");
            } else if (r->live == Live::Degraded) {
                // La raison d'etre de cette colonne : un chemin degrade
                // n'est PAS une panne, c'est pour ca qu'il faut le dire.
                ImGui::TextColored(u32(pal.warn),
                                   ICON_FA_TRIANGLE_EXCLAMATION " %s now",
                                   levelBadge(r->actual));
            } else {
                ImGui::TextColored(u32(pal.textDim), "not plugged");
            }
            if (r && !r->detail.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r->detail.c_str());

            // ── Le bouton de bascule, quand l'autre niveau existe ───────
            ImGui::TableSetColumnIndex(3);
            auto pit = pair.find(e.id);
            if (pit != pair.end()) {
                const ToggleState& t   = *pit->second.t;
                const int          idx = pit->second.idx;
                const ToggleOption& me = (idx == 0) ? t.low : t.high;
                const bool switchable  = (t.selected >= 0);
                if (t.selected == idx) {
                    ImGui::TextColored(u32(pal.ok), ICON_FA_CHECK " in use");
                    if (ImGui::IsItemHovered() && !me.why.empty())
                        ImGui::SetTooltip("%s", me.why.c_str());
                } else {
                    ImGui::PushID(e.id);
                    ImGui::BeginDisabled(!me.available || !switchable);
                    char label[96];
                    std::snprintf(label, sizeof(label),
                                  ICON_FA_RIGHT_LEFT " use %s",
                                  levelBadge(me.level));
                    if (ImGui::SmallButton(label)) {
                        req.toggle = t.id;
                        req.option = idx;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled)) {
                        // AllowWhenDisabled : l'explication du grisage est
                        // exactement ce qu'on veut pouvoir lire.
                        if (!switchable)
                            ImGui::SetTooltip("Nothing to switch — neither "
                                              "side is plugged. Add the card "
                                              "in Slot Configuration first.");
                        else if (!me.available && !me.blockedBy.empty())
                            ImGui::SetTooltip("Unavailable — %s",
                                              me.blockedBy.c_str());
                        else
                            ImGui::SetTooltip("%s%s", me.why.c_str(),
                                              t.needsRestart
                                              ? "\n\nSwitching RESTARTS the "
                                                "machine." : "");
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }

    if (degraded > 0) {
        ImGui::TextColored(u32(pal.warn),
                           ICON_FA_TRIANGLE_EXCLAMATION
                           " %d running above its catalogued level "
                           "(a ROM dump is missing — see ROM Status).",
                           degraded);
    }

    ImGui::End();
    return req;
}

} // namespace pom2
