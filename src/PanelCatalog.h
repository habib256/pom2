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

// PanelCatalog — the ONE list of POM2's dockable panels.
//
// ── Why this file exists ─────────────────────────────────────────────────
// Every panel used to be described in six places at once, and the count is
// the argument: 32 lines to load its visibility, 32 to save it, 38 to offer
// it in the command palette, 38 more to dispatch that command, 37 menu rows,
// and a 28-assignment block that hides "every" panel on the browser build.
// Six hand-kept lists over 38 panels, none of them checkable against the
// others. The predictable failures had all already happened:
//
//   * seven panels were in the palette but had no settings key, so the user
//     opened them and they were gone next launch;
//   * the WASM chrome-light block hid 28 of the 39, silently missing every
//     panel added after it was written;
//   * the Help menu attached ROM Status's tooltip to the Abstraction Levels
//     row (two IsItemHovered blocks after the same MenuItem), so one row
//     showed the wrong tip and the other showed none.
//
// So the panel's *identity* — id, title, where it lives in the menu bar, the
// settings key, the tooltip, the shortcut — is declared here, once, as data.
// `PanelRegistry` binds each entry to its `bool` and its optional runtime
// bits (a label carrying a slot number, an availability predicate), and the
// menus, the palette, the palette's dispatch and the settings round-trip are
// all *derived* from that. Adding a panel is one row here plus one bind call.
//
// This is deliberately a table and not a self-registration mechanism: the
// command palette's own header makes the point for its non-panel commands and
// it holds here too — the value is that you can read the whole UI surface in
// one place. Self-registration scatters it back across 40 translation units.
//
// ── What is NOT here ─────────────────────────────────────────────────────
// The panels' rendering. Each panel still draws itself where it always did;
// this file says what a panel *is*, not what it looks like.

#ifndef POM2_PANEL_CATALOG_H
#define POM2_PANEL_CATALOG_H

#include <cstddef>

namespace pom2 {

/// Where a panel's row appears in the menu bar. The enum order is the order
/// the groups are drawn in, and the catalog below is sorted by it, so reading
/// the table top to bottom reads the menu bar left to right.
enum class PanelGroup {
    File,           ///< File menu
    Machine,        ///< Machine menu
    DevStorage,     ///< Devices ▸ Storage
    DevSound,       ///< Devices ▸ Sound
    DevPorts,       ///< Devices ▸ Ports & cards
    DevInspectors,  ///< Devices ▸ Inspectors & tools
    Display,        ///< Display menu
    View,           ///< View menu
    Tools,          ///< Tools menu
    Help,           ///< Help menu
};

/// Every panel, as a compile-time handle. The ORDER here is irrelevant —
/// correspondence with the table below is by the explicit `PanelId` field in
/// each row, not by position, and `panelInfo()` looks it up. That is the
/// difference between two lists that must be kept in step (the thing this
/// whole file exists to stop) and one list with a typed index into it: a
/// missing or duplicated row is a compile error, not a runtime surprise.
enum class PanelId : std::size_t {
    DiskLibrary, SlotConfig,
    Media, FloppyEmu, Cassette, DiskII, Disk35, Hdv, SmartPort, FujiNet,
    Mockingboard, Phasor, EchoPlus, Mixer,
    Ssc, Ethernet, Printer, ImageWriter, ChatMauve, Joystick, Keyboard,
    Rewind, Mouse, NoSlotClock,
    Crt, Voxel, VoxelSettings,
    MemViewer, Debugger, MemBar, MemBarH, MemGrid,
    HgrPaint, HgrSprite, AiControl,
    Welcome, RomStatus, Abstraction,
    Count,
};

inline constexpr std::size_t kPanelCount = static_cast<std::size_t>(PanelId::Count);

struct PanelInfo {
    /// The compile-time handle. Everything that HOLDS a panel — the
    /// visibility array, `MainWindow::show()`, every call site that used to
    /// name a `bool showXxx` member — uses this, so a typo is a compile
    /// error rather than a panel that silently never opens.
    PanelId id;
    /// Command id: the string the palette dispatches and any future key
    /// binding names. It may not change once shipped, even when the title
    /// does — the enumerator above is for code, this is for config.
    const char* command;
    /// The one label. Menus and the palette both use it, which is the point:
    /// they used to carry different wordings for the same window ("Disk II
    /// drive" in the palette, "Disk II (slot 6)" in the menu).
    const char* title;
    PanelGroup  group;
    /// Settings key for "was it open last time", or nullptr for a panel that
    /// deliberately starts closed every session.
    const char* settingsKey;
    /// Menu accelerator text. Display only — the key itself is routed in
    /// MainWindow::render, not here.
    const char* shortcut;
    /// Menu tooltip. Written for somebody who has not opened the panel yet.
    const char* tip;
    /// Open on a fresh install (no settings file yet). Three panels are: the
    /// two that answer "where do I put my disks" and the printout the printer
    /// cards feed. Everything else starts closed.
    bool defaultOpen = false;
};

// clang-format off
inline constexpr PanelInfo kPanelCatalog[] = {

// ── File ────────────────────────────────────────────────────────────────
{ PanelId::DiskLibrary, "panel.disklibrary", "Disk Library (all formats)", PanelGroup::File,
  "show_disk_library", nullptr,
  "Every disk image POM2 can read, in one browsable list.", /*defaultOpen=*/true },

// ── Machine ─────────────────────────────────────────────────────────────
{ PanelId::SlotConfig, "panel.slotconfig", "Slot Configuration...", PanelGroup::Machine,
  "show_slot_config", nullptr,
  "One card per slot, plugged and unplugged live. Built-in slots on the\n"
  "//c-class profiles are locked and say why.", /*defaultOpen=*/true },

// ── Devices ▸ Storage ───────────────────────────────────────────────────
{ PanelId::Media, "panel.media", "Internal Disks & Media...", PanelGroup::DevStorage,
  "show_media_panel", nullptr,
  "Every internal drive and mountable bay in one place. Mount / Insert /\n"
  "Eject act immediately — the card-per-slot list is Machine \xe2\x86\x92 Slot\n"
  "Configuration." },
{ PanelId::FloppyEmu, "panel.floppyemu", "Floppy Emu (BMOW)", PanelGroup::DevStorage,
  "show_floppy_emu", nullptr,
  "BMOW Floppy Emu: SD-card image browser + OLED, emulated." },
{ PanelId::Cassette, "panel.cassette", "Cassette deck", PanelGroup::DevStorage,
  "show_cassette", nullptr,
  "Load/save tape images (.wav) on II/II+/IIe." },
{ PanelId::DiskII, "panel.diskii", "Disk II (slot 6)", PanelGroup::DevStorage,
  "show_disk_panel", nullptr,
  "5.25\" drive panel: insert / eject / write-protect, drive LEDs." },
{ PanelId::Disk35, "panel.disk35", "Disk 3.5\"", PanelGroup::DevStorage,
  "show_disk35_panel", nullptr,
  "800K 3.5\" drive (SmartPort / //c+ on-board IWM)." },
{ PanelId::Hdv, "panel.hdv", "HDV", PanelGroup::DevStorage,
  "show_hdv_panel", nullptr,
  "ProDOS hard-disk image (.hdv/.2mg): mount / eject / boot." },
{ PanelId::SmartPort, "panel.smartport", "SmartPort Configuration", PanelGroup::DevStorage,
  "show_smartport_panel", nullptr,
  "SmartPort units behind a Liron-class card (3.5\" + HDV volumes)." },
{ PanelId::FujiNet, "panel.fujinet", "FujiNet", PanelGroup::DevStorage,
  "show_fujinet_panel", nullptr,
  "FujiNet relay: transport, attached devices and call counters." },

// ── Devices ▸ Sound ─────────────────────────────────────────────────────
{ PanelId::Mockingboard, "panel.mockingboard", "Mockingboard (VIA + AY state)", PanelGroup::DevSound,
  "show_mockingboard", nullptr,
  "Mockingboard A/C: live 6522 VIA + AY-3-8910 PSG register view." },
{ PanelId::Phasor, "panel.phasor", "Phasor", PanelGroup::DevSound,
  "show_phasor", nullptr,
  "Applied Engineering Phasor: 2\xc3\x97 VIA, 4\xc3\x97 AY, mode soft-switch." },
{ PanelId::EchoPlus, "panel.echoplus", "Echo+", PanelGroup::DevSound,
  "show_echoplus", nullptr,
  "Echo/Cricket SSI263 speech chip state." },
{ PanelId::Mixer, "panel.mixer", "Audio Mixer", PanelGroup::DevSound,
  "show_mixer", nullptr,
  "Per-source volume: speaker, Mockingboard/Phasor, speech, floppy." },

// ── Devices ▸ Ports & cards ─────────────────────────────────────────────
{ PanelId::Ssc, "panel.ssc", "Super Serial", PanelGroup::DevPorts,
  "show_ssc", nullptr,
  "6551 ACIA serial port + telnet bridge (modem / printer)." },
{ PanelId::Ethernet, "panel.ethernet", "Ethernet", PanelGroup::DevPorts,
  "show_ethernet", nullptr,
  "Uthernet I / II state: host transport, MAC, W5100 sockets." },
{ PanelId::Printer, "panel.printer", "Printer", PanelGroup::DevPorts,
  "show_printer", nullptr,
  "Parallel printer card \xe2\x86\x92 text spool (.txt)." },
{ PanelId::ImageWriter, "panel.imagewriter", "ImageWriter II (printout)", PanelGroup::DevPorts,
  "show_imagewriter", nullptr,
  "Rendered ImageWriter II output: pages, colour ribbon, PNG export.", /*defaultOpen=*/true },
{ PanelId::ChatMauve, "panel.chatmauve", "Le Chat Mauve (slot 7)", PanelGroup::DevPorts,
  "show_chatmauve", nullptr,
  "Le Chat Mauve RGB / Eve video card controls." },
{ PanelId::Joystick, "panel.joystick", "Joystick", PanelGroup::DevPorts,
  "show_joystick", nullptr,
  "Analog paddles / joystick mapping + push-buttons." },
{ PanelId::Keyboard, "panel.keyboard", "Apple //e Keyboard", PanelGroup::DevPorts,
  "show_keyboard", nullptr,
  "A photo of the real //e keyboard, clickable. The keys a host keyboard\n"
  "has nowhere to put — Open-Apple, Solid-Apple, the //e's own Reset —\n"
  "are here, with the real legends." },

// ── Devices ▸ Inspectors & tools ────────────────────────────────────────
{ PanelId::Rewind, "panel.rewind", "Rewind (time-travel)", PanelGroup::DevInspectors,
  "show_rewind", "F6",
  "Scrub back through machine state. Hold F6 to rewind live." },
{ PanelId::Mouse, "panel.mouse", "Mouse Inspector", PanelGroup::DevInspectors,
  "show_mouse_inspector", nullptr,
  "Apple II Mouse Card state + host-cursor sync diagnostics." },
{ PanelId::NoSlotClock, "panel.nsclock", "No-Slot Clock (DS1216E)", PanelGroup::DevInspectors,
  "show_nsclock", nullptr,
  "Dallas DS1216E real-time clock hidden under the Monitor ROM." },

// ── Display ─────────────────────────────────────────────────────────────
{ PanelId::Crt, "panel.crt", "CRT Settings (sliders)...", PanelGroup::Display,
  "show_ntsc", nullptr,
  "Scanlines, shadow mask, barrel, phosphor curve, persistence,\n"
  "brightness/contrast/saturation." },
{ PanelId::Voxel, "panel.voxel", "3D voxel view", PanelGroup::Display,
  "show_3d_voxel", nullptr,
  "MicroM8-style cube renderer.\n"
  "Left-drag orbits, middle-drag pans, wheel zooms." },
{ PanelId::VoxelSettings, "panel.voxelset", "3D voxel settings...", PanelGroup::Display,
  "show_voxel_settings", nullptr,
  "Depth, colour pop, fill, anti-alias, mono, per-colour depth." },

// ── View ────────────────────────────────────────────────────────────────
// These five were the panels with no settings key: opening one and finding
// it gone next launch was not a decision, it was six lists disagreeing.
// They persist like the rest now.
{ PanelId::MemViewer, "panel.memviewer", "Memory viewer", PanelGroup::View,
  "show_memviewer", nullptr,
  "Hex + ASCII view of the emulated memory, with a live follow mode." },
{ PanelId::Debugger, "panel.debugger", "Debugger", PanelGroup::View,
  "show_debugger", nullptr,
  "Registers, disassembly, breakpoints, write watchpoints, and step /\n"
  "step-over / run-to-cursor." },
{ PanelId::MemBar, "panel.membar", "Memory Map Bar", PanelGroup::View,
  "show_membar", nullptr,
  "A one-strip map of what is paged where right now." },
{ PanelId::MemBarH, "panel.membarh", "Memory Map Bar (Horizontal)", PanelGroup::View,
  "show_membar_h", nullptr,
  "The same map, laid out along the window's width." },
{ PanelId::MemGrid, "panel.memgrid", "Memory Map Grid", PanelGroup::View,
  "show_memgrid", nullptr,
  "Page-per-cell grid: RAM, ROM, aux, language card, slot ROM." },

// ── Tools ───────────────────────────────────────────────────────────────
{ PanelId::HgrPaint, "panel.hgrpaint", "HGR Paint Editor", PanelGroup::Tools,
  "show_hgr_paint", nullptr,
  "Paint directly into HGR/GR/DHGR video RAM through the real NTSC\n"
  "pipeline (image import included)." },
{ PanelId::HgrSprite, "panel.hgrsprite", "HGR Sprite Editor", PanelGroup::Tools,
  "show_hgr_sprite", nullptr,
  "Draw HGR sprites on a scratch page, grab from / stamp to the live\n"
  "screen, export ca65 .byte tables." },
{ PanelId::AiControl, "panel.aicontrol", "AI Control (HTTP)...", PanelGroup::Tools,
  "show_ai_control", nullptr,
  "Loopback HTTP control server: keys, screenshots, snapshots, reset." },

// ── Help ────────────────────────────────────────────────────────────────
// The one panel that deliberately does NOT persist: a first launch with no
// ROM opens it from the constructor, before settings are read, so a stored
// `false` would silently cancel the greeting the newcomer needs.
{ PanelId::Welcome, "panel.welcome", "Welcome / Quick Start", PanelGroup::Help,
  nullptr, nullptr,
  "Where to put ROMs/disks, keys, and signature features." },
{ PanelId::RomStatus, "panel.romstatus", "ROM Status...", PanelGroup::Help,
  "show_rom_status", nullptr,
  "Every ROM POM2 probes: present or missing, which dump resolved, and\n"
  "what breaks without it." },
{ PanelId::Abstraction, "panel.abstraction", "Abstraction Levels (LLE / HLE)...", PanelGroup::Help,
  "show_abstraction", nullptr,
  "What POM2 emulates as silicon and what it emulates as a service,\n"
  "subsystem by subsystem — plus which level is actually running (a\n"
  "missing ROM dump quietly demotes several of them) and the four\n"
  "boundaries you can move." },
};
// clang-format on

// ── The one check that makes the two halves one list ────────────────────
// Every PanelId has exactly one row, and every row names a real PanelId. A
// forgotten row, a duplicate, or a copy-pasted enumerator is a build failure
// here rather than a menu entry that toggles the wrong window.
constexpr bool panelCatalogIsComplete()
{
    if (sizeof(kPanelCatalog) / sizeof(kPanelCatalog[0]) != kPanelCount) return false;
    for (std::size_t want = 0; want < kPanelCount; ++want) {
        std::size_t seen = 0;
        for (const PanelInfo& p : kPanelCatalog)
            if (static_cast<std::size_t>(p.id) == want) ++seen;
        if (seen != 1) return false;
    }
    return true;
}
static_assert(panelCatalogIsComplete(),
              "every PanelId needs exactly one row in kPanelCatalog");

/// The row for a panel. Linear, constexpr, 38 entries — this runs at compile
/// time wherever the argument is a constant, and once per menu row otherwise.
constexpr const PanelInfo& panelInfo(PanelId id)
{
    for (const PanelInfo& p : kPanelCatalog)
        if (p.id == id) return p;
    return kPanelCatalog[0];   // unreachable: the static_assert above proved it
}

}  // namespace pom2

#endif  // POM2_PANEL_CATALOG_H
