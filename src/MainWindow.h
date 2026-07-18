// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_MAIN_WINDOW_H
#define POM2_MAIN_WINDOW_H

// MainWindow.h kept lean: only the headers strictly required by the
// public/private member types declared here. Card / panel / controller
// implementations are pulled in by MainWindow.cpp via include of their
// own headers. Changing a card or panel header recompiles its own TU
// (and MainWindow.cpp), not every TU that touches MainWindow.h.
//
// `M6502.h` stays because `M6502::CpuMode` appears in the
// `resolveCpuMode` signature below — nested enums can't be forward-
// declared. `SystemProfile` is reachable via an opaque enum-class
// forward decl, no SystemProfile.h needed here.

#include "M6502.h"
#include "Apple2Display.h"  // HiResMode (toolbar color/mono toggle remembers submode)
#include "Mat4.h"           // pom2::OrbitCamera member (3D voxel view)

#include "imgui.h"  // ImU32 / ImVec2 used in struct MemRegion + member types

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

class Apple2Display;
class ClockCard;
class DiskIICard;
class EmulationController;
class JoystickInput;
class LeChatMauveCard;
class EchoPlusCard;
class EchoPlusTMS5220Card;
class GrapplerCard;
class MockingboardCard;
class MouseCard;
class MouseCardAppleWin;
class PhasorCard;
class PrinterCard;
class ProDOSHardDiskCard;
class SlotPeripheral;
class SuperSerialCard;

namespace pom2 {
    class AiControlServer;
    class CassetteDeck_ImGui;
    class Rewind_ImGui;
    class Disk35Controller_ImGui;
    class NtscPostProcessor;
    class CrtEffectStack;
    class Voxel3DRenderer;
    struct NtscParams;
    class DiskController_ImGui;
    class DiskLibrary_ImGui;
    class CffaCard;
    class ProDOSBlockCard;
    class HdvController_ImGui;
    class JoystickPanel_ImGui;
    class LeChatMauve_ImGui;
    class Settings;
    class SmartPortCard;
    class SmartPort_ImGui;
    class FloppyEmuDevice;
    class FloppyEmu_ImGui;
    class Toolbar_ImGui;
    enum class SystemProfile;
    enum class CharRomLocale : uint8_t;
}
class MemoryViewer_ImGui;
class Pom2HgrPaintHost;
namespace hgrpaint { class HgrPaintEditor; }
namespace hgrsprite { class HgrSpriteEditor; }

class MainWindow
{
public:
    /// `forceIIPlus`: when true, skip the auto-probe for `roms/apple2e.rom`
    /// even if it is present and load `roms/apple2.rom` instead. Used by
    /// the `--ii-plus` CLI flag to fall back to II+ mode for software
    /// (e.g. Copy II Plus 8.3) that boots cleanly under the legacy ROM.
    explicit MainWindow(bool forceIIPlus = false);
    ~MainWindow();

    /// Apply a system profile via full cold-reset. Stops the CPU worker,
    /// wipes RAM + soft switches, reloads the profile's ROM + char ROM,
    /// re-plugs the slot cards (preserving currently-mounted disk/HDV
    /// paths so the user can boot the same media under a different
    /// Apple II model), flips the CPU type to the profile default
    /// (overridable via the `cpu_mode_override` setting), and lets the
    /// CPU restart from the new reset vector. Persists the chosen
    /// profile to settings so subsequent launches default to it.
    void applyProfile(pom2::SystemProfile p);
    pom2::SystemProfile currentProfile() const { return activeProfile; }

    /// Defined out-of-line in MainWindow.cpp — `controller` is a
    /// `unique_ptr<EmulationController>` here, so dereferencing it
    /// inline would require EmulationController.h. Keeping the body in
    /// the .cpp lets every translation unit that includes MainWindow.h
    /// stay clear of the EmulationController include cone.
    EmulationController& emul();
    Apple2Display&       displayRef();

    /// Read/write the Le Chat Mauve "Dragon Wars compatibility" toggle
    /// from main()'s Phase-B CLI handler. No-ops (returns false) when no
    /// LeChatMauveCard is plugged at boot. Persists to Settings so the
    /// next session picks it up automatically.
    bool setChatMauveInvertBit7(bool v);

    void setGlfwWindow(GLFWwindow* w);
    void render();

    // Hooks installed by main.cpp into GLFW callbacks.
    void onChar(unsigned int codepoint);
    void onKey (int key, int scancode, int action, int mods);
    /// GLFW cursor-position callback. Window-relative coordinates.
    /// Routes to the Apple II Mouse Card when the cursor is inside
    /// the Apple II Screen widget; otherwise no-op (ImGui handles it).
    void onMouseMove  (double x, double y);
    void onMouseButton(int button, int action);

    /// GLFW file-drop callback (installed by main.cpp). Routes the first
    /// recognised disk image among `paths` through `insertAndBootImage`
    /// (auto-routes Disk II / SmartPort 3.5" / ProDOS HDV). Unrecognised
    /// or extra files are ignored; a status-bar message reports the result.
    void onFileDrop(int count, const char** paths);

    // HDV helper — cold-boot through the slot-5 ProDOS block-device ROM.
    // Public so main() can drive it from `POM2_AUTO_BOOT_HDV` for headless
    // trace capture without going through the File menu.
    void bootHdvImage();

    /// Mount `path` into the slot matching its type (5.25" Disk II / 800K
    /// 3.5" / ProDOS HDV, via `classifyDiskForSlot`) under the currently
    /// active profile + slot config, then cold-boot from it. Returns true
    /// on success; on failure `errOut` carries the reason. Drives both the
    /// Disk Library "insert + boot" buttons and the CLI positional-disk /
    /// `--kiosk` launcher. Must run on the UI thread (takes the state lock
    /// internally between frames).
    bool insertAndBootImage(const std::string& path, std::string& errOut);

#ifdef __EMSCRIPTEN__
    /// Browser UX: toolbar reset should relaunch the boot image that was
    /// passed in through the WASM page arguments (Total Replay by default).
    void setBrowserResetBootImage(const std::string& path) { browserResetBootImage_ = path; }
#endif

    /// Kiosk mode: chrome-free full-screen — render() draws only the Apple
    /// II screen (no menu bar, toolbar, panels or dialogs). Set by main()
    /// from the `--kiosk` CLI flag before the first render.
    void setKioskMode(bool k) { kiosk_ = k; }
    bool kioskMode() const { return kiosk_; }

    // Apple II memory map region — used by the bar / grid widgets in
    // MainWindow_MemoryMaps.cpp. Public so file-local helpers in that TU
    // can take a `const std::vector<MemRegion>&`.
    struct MemRegion { uint16_t start, end; ImU32 color; const char* label; };

private:
    // Owning members held by unique_ptr so the include of each subsystem
    // header stays in MainWindow.cpp. Constructor / destructor defined
    // out-of-line for the same reason (unique_ptr<T> destruction needs
    // T to be a complete type).
    std::unique_ptr<EmulationController>          controller;
    std::unique_ptr<Apple2Display>                display;
    // Toolbar color↔mono toggle remembers the last mode on each side, so
    // round-tripping preserves the user's specific submode (e.g. Mono Green
    // ↔ Color 4-bit) rather than snapping to a single default each way.
    Apple2Display::HiResMode lastColorHiResMode_ = Apple2Display::HiResMode::ColorNTSC;
    Apple2Display::HiResMode lastMonoHiResMode_  = Apple2Display::HiResMode::MonoWhite;
    std::unique_ptr<MemoryViewer_ImGui>           memViewer;
    std::unique_ptr<pom2::Settings>               settings;
    std::unique_ptr<pom2::CassetteDeck_ImGui>     cassetteDeck;
    std::unique_ptr<pom2::Rewind_ImGui>           rewindPanel_;
    // One Disk II controller-panel per plugged DiskIICard (option C
    // 2026-05-15: DiskII can now appear in multiple slots, so the user
    // can wire DiskII slot 6 + DiskII slot 4 = 4 drives 5.25"). The
    // vector is rebuilt by `plugSlotsFromSettings`; index N corresponds
    // to `diskCards[N]`. `diskPanel` keeps a reference to the *primary*
    // (lowest-slot) panel so legacy menu wiring stays unchanged.
    std::vector<std::unique_ptr<pom2::DiskController_ImGui>> diskPanels;
    pom2::DiskController_ImGui*                              diskPanel = nullptr;
    std::unique_ptr<pom2::Disk35Controller_ImGui> disk35Panel;
    std::unique_ptr<pom2::DiskLibrary_ImGui>      diskLibrary;
    std::unique_ptr<pom2::HdvController_ImGui>    hdvPanel;
    std::unique_ptr<pom2::SmartPort_ImGui>        smartPortPanel;
    // BMOW Floppy Emu: the device model (mode/SD/favorites) + its OLED panel.
    std::unique_ptr<pom2::FloppyEmuDevice>        floppyEmu;
    std::unique_ptr<pom2::FloppyEmu_ImGui>        floppyEmuPanel;
    std::unique_ptr<pom2::JoystickPanel_ImGui>    joystickPanel;
    std::unique_ptr<pom2::LeChatMauve_ImGui>      chatMauvePanel;
    std::unique_ptr<pom2::Toolbar_ImGui>          toolbar;
    // Portable HGR Paint editor (src/hgrpaint/, shared verbatim with POM1)
    // + its POM2 host seam (pokes / offscreen NTSC render / file I/O).
    std::unique_ptr<Pom2HgrPaintHost>             hgrPaintHost;
    std::unique_ptr<hgrpaint::HgrPaintEditor>     hgrPaintEditor;
    // Sprite editor (src/hgrsprite/) — reuses the same host seam.
    std::unique_ptr<hgrsprite::HgrSpriteEditor>   hgrSpriteEditor;
    // All plugged Disk II cards, sorted by slot ascending. `diskCard`
    // (below) is the primary alias = `diskCards.empty() ? nullptr :
    // diskCards.front()`. Most legacy code paths use `diskCard` directly
    // (auto-turbo, AI control attach, menu Eject); the panel render loop
    // iterates `diskCards`.
    std::vector<DiskIICard*>     diskCards;
    DiskIICard*                  diskCard = nullptr;       // non-owning, owned by SlotBus
    ProDOSHardDiskCard*          hdvCard = nullptr;        // non-owning, owned by SlotBus
    pom2::CffaCard*              cffaCard = nullptr;       // non-owning, owned by SlotBus
    LeChatMauveCard*             chatMauveCard = nullptr;  // non-owning, owned by SlotBus
    // Plural alias: the //c built-in lineup ships TWO SSC-compatible
    // serial ports (printer + modem). `sscCard` is the primary alias =
    // `sscCards.empty() ? nullptr : sscCards.front()` (kept for legacy
    // callers like SnapshotIO + AI control). Multi-instance code paths
    // (menu, panel tabs, settings persistence) iterate `sscCards`.
    std::vector<SuperSerialCard*> sscCards;
    SuperSerialCard*             sscCard = nullptr;        // non-owning, owned by SlotBus
    ClockCard*                   clockCard = nullptr;      // non-owning, owned by SlotBus
    MouseCard*                   mouseCard = nullptr;      // non-owning, owned by SlotBus
    // AppleWin-style HLE mouse — alternative to MouseCard (only one of
    // the two is plugged at a time). Both implement the same setHostMouse
    // API so the UI input layer is variant-agnostic.
    MouseCardAppleWin*           mouseAwCard = nullptr;    // non-owning, owned by SlotBus
    MockingboardCard*            mockingboardCard = nullptr; // non-owning, owned by SlotBus
    PhasorCard*                  phasorCard       = nullptr; // non-owning, owned by SlotBus
    EchoPlusCard*                echoPlusCard     = nullptr; // non-owning, owned by SlotBus
    EchoPlusTMS5220Card*         echoPlusTmsCard  = nullptr; // non-owning, owned by SlotBus
    pom2::SmartPortCard*         smartPortCard    = nullptr; // non-owning, owned by SlotBus
    PrinterCard*                 printerCard      = nullptr; // non-owning, owned by SlotBus
    GrapplerCard*                grapplerCard     = nullptr; // non-owning, owned by SlotBus
    /// Status of the Mouse Card ROM probe — used by the Slot
    /// Configuration UI to indicate whether 'mouse' is selectable.
    /// "" = not yet probed, "loaded: <paths>" = ready, otherwise the
    /// failure message.
    std::string                  mouseRomStatus;

    /// Canonical per-slot card-type strings, as resolved by
    /// `plugSlotsFromSettings()` (and persisted on shutdown). Index 0 is
    /// the language-card slot and is always empty here.
    /// Values: "", "diskii", "hdv", "ssc", "clock", "chatmauve", "mouse".
    std::array<std::string, 8> slotCards{};

    std::unique_ptr<JoystickInput> joystick;
    GLFWwindow*                    window = nullptr;

    unsigned int screenTexture = 0;     // GL texture name (lazy)
    int          screenTextureWidth  = 0;
    int          screenTextureHeight = 0;
    // OpenEmulator-style composite NTSC shader pipeline. Lazily
    // initialised on the first frame the user selects ColorCompositeOE;
    // stays alive across mode toggles so persistence state isn't lost
    // when the user briefly flips to another mode and back.
    std::unique_ptr<pom2::NtscPostProcessor> ntscFx;
    // Universal CRT effect stack (Phase 3): applies the same scanline /
    // shadow-mask / barrel / persistence / BCS layers to the framebuffer of
    // ANY colour mode (not just ColorCompositeOE). Opt-in via the menu;
    // shares the NtscParams driven by the CRT Settings panel. Lazily
    // initialised. Always runs for non-OE pipelines (OE bakes its own
    // effects in its demod shader).
    std::unique_ptr<pom2::CrtEffectStack> crtFx;
    // 3D voxel view (MicroM8 "Voxel Cube"): rebuilds the presented framebuffer
    // as an upright 4:3 slab of equal-depth instanced cubes, viewed by an orbit
    // camera (left-drag / wheel). Orthogonal to the colour pipeline — works on
    // any HiResMode. Lazily initialised; off (persisted under `show_3d_voxel`).
    std::unique_ptr<pom2::Voxel3DRenderer> voxel3d_;
    pom2::OrbitCamera voxelCam_;
    bool         show3dVoxel_ = false;
    bool         showVoxelSettings_ = false;
    bool         showNtscSettings = false;
    // Master ON/OFF for the whole CRT effect stack (the button at the top of
    // the CRT Settings window). When off, every pipeline presents its raw
    // framebuffer — the colour demod still runs, only the CRT glass is
    // bypassed. Persisted under `crt_effects_enabled`.
    bool         crtEffectsEnabled = true;

    // Presentation aspect (Phase 6). The Apple II's 280×192 active area was
    // shown on a 4:3 CRT → its pixels are NOT square. Square = 1:1 logical
    // pixels (crisp emulator default); Crt43 = true 4:3 monitor shape;
    // Integer = square snapped to an integer multiple (no scaling shimmer).
    enum class AspectMode { Square, Crt43, Integer };
    AspectMode   aspectMode = AspectMode::Square;
    bool         showMemViewer = false;
    bool         showMemoryBar      = false;   // tall vertical map
    bool         showMemoryBarH     = false;   // wide-short horizontal variant
    bool         showMemoryGrid     = false;   // 16×16 page grid
    // Default UI layout: Apple II Screen on the left, HDV top-right,
    // Disk II bottom-right. Every other panel (Emulation controls,
    // Cassette deck, Memory viewers, Joystick, Le Chat Mauve) starts
    // hidden — toggle from the Debug / Hardware menus.
    bool         showCassetteDeck = false;
    bool         showHgrPaintEditor = false;
    bool         showHgrSpriteEditor = false;
    // Per-frame 64 KB main-RAM (+ aux) snapshots handed to the HGR Paint
    // editor as its canvas/shadow read source (see renderHgrPaintWindow).
    std::vector<uint8_t> hgrPaintMem_;
    std::vector<uint8_t> hgrPaintAux_;
    bool         showRewindBar    = false;
    bool         rewindHeldPrev_  = false;   // hold-to-rewind edge tracker (F6 + toolbar)
    // Per-card disk panels (Disk II / Disk 3.5" / HDV) are off by
    // default since 2026-05-15 — the unified `Disk Library` panel
    // covers the 1-click insert+boot path for all three formats with
    // a single window. Users open the per-card panels on demand from
    // Devices menu when they need the deep state (track number, motor
    // LED, write-back checkbox, etc.).
    bool         showDiskPanel   = false;
    bool         showDisk35Panel = false;
    bool         showHdvPanel    = false;
    bool         showSmartPortPanel = false;
    bool         showJoystickPanel = false;
    bool         showChatMauvePanel = false;
    bool         showSscPanel       = false;
    // Dallas DS1216E "No-Slot Clock" diagnostics panel — pattern-matcher
    // counter + clock-readout bit counter so the user can verify ProDOS
    // walked the magic key successfully.
    bool         showNoSlotClockPanel = false;
    // Printer panel — view spool buffer, save as .txt, clear.
    bool         showPrinterPanel   = false;
    // Pending path the user has typed into the "Save spool as…" text box.
    // Auto-suggested with a timestamped filename under ./printouts/ on
    // first open; reused across save clicks within the same session.
    std::string  printerSavePath;
    // Last save outcome — shown under the Save button, persists until the
    // user saves again or closes the panel. Empty = nothing saved yet.
    std::string  printerLastSaveStatus;
    // Recipient for "print with e-mail" — persisted (printer_email_to)
    // so the address survives across sessions.
    std::string  printerEmailTo;
    // Memory-viewer edits queued during memViewer->render() (which runs
    // under stateMutex) and flushed through memWrite after the lock is
    // released — the write callback must never re-lock (self-deadlock).
    std::vector<std::pair<uint16_t, uint8_t>> pendingMemViewerWrites;
    // Mockingboard live state panel — shows VIA T1 / IFR / IER and the
    // two AY-3-8910 register banks. Primary use: diagnose silent
    // IRQ-driven music drivers (Ultima IV, Nox Archaist) by seeing
    // whether the music handler is actually writing AY registers.
    bool         showMockingboardPanel = false;
    // Phasor live state panel — same layout as Mockingboard but
    // widened to 4 AY-3-8913 banks, with a mode banner (MB / Phasor /
    // EchoPlus) and clockScale at the top.
    bool         showPhasorPanel       = false;
    // Echo+ panel — single SSI263 register dump + current phoneme +
    // A/!R + duration countdown.
    bool         showEchoPlusPanel     = false;
    // Audio mixer — master + per-channel sliders/mute (Speaker, Cassette,
    // Mockingboard, Disk 5.25", Disk 3.5"). Replaces the volume sliders
    // that used to live in the Status panel. Persisted as `show_mixer`.
    bool         showAudioMixer     = false;
    // Mouse Inspector — live readout of host cursor position, Apple II
    // Screen widget rect, MouseCard 8-bit counter + sub-pixel accumulator,
    // AppleWin HLE firmware state (iX/iY/clamps/mode), and the firmware
    // screen holes. Off by default; toggled from Panels menu; persisted as
    // `show_mouse_inspector`. Helper for future cursor-alignment tuning.
    bool         showMouseInspector = false;
    // Optional CSV log path for the Mouse Inspector. Empty = not logging.
    // `mouseInspectorLogStream` is opened when logging starts and closed
    // when it stops; flushed after every row so a crash mid-tune still
    // leaves a usable trace on disk.
    std::string  mouseInspectorLogPath;
    std::unique_ptr<std::ofstream> mouseInspectorLogStream;
    double       mouseInspectorLastLogTime = 0.0;
    // Slot Configuration: left = per-slot card assignment (built-ins greyed),
    // right = internal disks + mountable ports of plugged storage cards.
    // Persisted as `show_slot_config`. (Absorbed the old Slot Manager.)
    bool         showSlotConfigPanel = false;
    // BMOW Floppy Emu panel. Off by default; Devices → Floppy Emu.
    // Persisted as `show_floppy_emu`. `floppyEmuFavActive_` tracks the
    // Favorites-vs-File-Explorer toggle; `floppyEmuStatus` is the last
    // mount/eject/mode message shown under the OLED.
    bool         showFloppyEmu       = false;
    bool         floppyEmuFavActive_ = false;
    std::string  floppyEmuStatus;
    bool         showAiControlPanel  = false;
    // Unified disk browser (3-tab panel: 5.25/3.5/HDV). On by default
    // since 2026-05-15 — replaces the per-card library lists as the
    // primary way to browse + mount images. Toggled from File menu.
    // Persisted as `show_disk_library`.
    bool         showDiskLibrary     = true;
    // Initialised in the constructor body from SuperSerialCard::kDefaultPort
    // so we don't have to drag SuperSerialCard.h into this header.
    int          sscPortInput       = 0;

    // ── AI Control server (HTTP/1.1 on 127.0.0.1) ────────────────────────
    // Lifetime owned here; attach()'d after EmulationController is wired,
    // start()'d if the persisted setting was on at last shutdown. Exposed
    // via the Hardware menu's AI Control panel.
    std::unique_ptr<pom2::AiControlServer> aiServer;
    // Initialised in the constructor body from AiControlServer::kDefaultPort
    // for the same reason as sscPortInput above.
    int          aiPortInput   = 0;
    std::string  aiTokenInput;

    // Disk II insert dialog state moved to DiskController_ImGui (it
    // owns its own UX surface). MainWindow keeps only the ROM probe.
    std::string diskRomPath  = "roms/disk2.rom";
    std::string diskRomStatus;
    // Auto-turbo while the Disk II motor is spinning. Real Apple II boot
    // takes 10-15 s at 1 MHz; bumping to ~60 MHz emulated for the duration
    // of the read drops it to <1 s. Off by default — preserves authentic
    // speed and the floppy mechanical sounds (the turbo collapses
    // wall-clock to zero, which mutes the seek/spindle audio).
    bool        diskTurboWhileMotor = false;
    // Speed to restore when turbo disengages. NTSC 1× at construction;
    // applyProfile re-seeds it with the profile's defaultCyclesPerFrame
    // (20313 PAL, 68180 //c+) so the restore never underclocks.
    int         diskSavedCyclesPerFrame = 17045;
    bool        diskTurboActive = false;

    // ProDOS hard disk / HDV state. The default mounted image is whatever
    // the constructor finds first under hdv/ (alphabetical) — no auto-boot.
    // Mount-dialog state lives in `hdvPanel`.
    std::string hdvPath;
    std::string hdvStatus;

    // Cassette load/save dialog state moved to CassetteDeck_ImGui.
    std::string tapeStatusMessage;
    double      tapeStatusUntil = 0.0;
    double      lastFrameTime   = 0.0;

    // Paste-from-file dialog state.
    bool        showPasteFileDialog = false;
    std::string pasteDialogPath;
    bool        pasteAutoUppercase  = false;

    // Slot number of the DiskII the Insert-disk popup currently routes to.
    // Latched when any panel sets `insertDialogOpen` true; cleared when
    // the popup closes. Lets the popup survive panel pointer churn (rare
    // profile-switch races).
    int         diskDialogTargetSlot = -1;

    // Boot-time ROM probing.
    std::string romPath = "roms/apple2.rom";
    std::string charRomPath = "roms/apple2_char.rom";
    std::string romStatus = "no ROM loaded";

    // User-selected character-generator ROM locale. ProfileDefault means
    // "use the active profile's charRomProbeOrder"; anything else
    // overrides and bypasses the probe. Persisted as `char_rom_locale`
    // so the user's choice survives restarts and applies even before
    // the toolbar shows up. Default-initialised in the constructor
    // (header stays light — forward-declared enum can't be initialised
    // inline).
    pom2::CharRomLocale charRomLocale;

    // Active system profile. Tracked separately so the Presets menu can
    // mark the live entry with a checkmark and the title bar reflects
    // it. Initialised in the constructor body to AppleIIPlus (default
    // for II+ probe); user-driven Presets menu clicks + CLI --preset go
    // through `applyProfile()` which keeps this in sync.
    pom2::SystemProfile activeProfile;

    // Slot Config panel working-copy sync flag. Reset to false by
    // applyProfile()/restartEmulationFromSettings() so the next render
    // re-seeds the draft from the freshly-rebuilt slotCards[]; otherwise the
    // panel shows — and Apply persists — the previous profile's stale slots.
    bool slotDraftInited_ = false;

    bool showAbout = false;
    // Welcome / Quick Start panel. Opened from Help → Welcome, and
    // auto-opened on first launch when no main ROM was found so a
    // newcomer sees where to drop firmware/disks instead of a bare
    // "NO ROM" screen. `romLoaded_` mirrors the last ROM-load result so
    // the panel can show the no-ROM guidance prominently.
    bool showWelcomePanel = false;
    bool romLoaded_       = false;
    // Apple ][+ photo shown in the About dialog. Loaded lazily on first
    // open via stb_image; texture freed in ~MainWindow. `tried_` blocks
    // repeated reload attempts if the file is missing.
    unsigned aboutImageTex_ = 0;
    int      aboutImageW_   = 0;
    int      aboutImageH_   = 0;
    bool     aboutImageTried_ = false;

    // Kiosk mode (set by `--kiosk`): render() draws only the Apple II
    // screen, full-viewport, with no menu bar / toolbar / panels.
    bool kiosk_ = false;

    // ── Kiosk in-game menu (gamepad-driven, keyboard-free) ──────────────
    // An overlay exclusive to kiosk mode. Two entry points:
    //   • START (or F10) → the two-zone Start menu: a GAMES list (the disk
    //     images next to the booted disk + any extra ROM folders) and an
    //     ACTIONS column (Restart / Keyboard / ROM folders / Quit). LEFT/
    //     RIGHT swaps focus between the two zones, UP/DOWN moves within the
    //     focused zone, FIRE validates it. The machine is PAUSED while this
    //     is up (like inserting a disk on a real machine at rest).
    //   • SELECT (or K) → the Keyboard band: a 2D grid of Apple II keys sent
    //     live to the running game (NOT paused) via Memory::queueKey.
    // From the Start menu, "ROM folders" opens a gamepad directory browser
    // to add/remove extra scan folders (persisted outside state.cfg so the
    // kiosk's read-only main config is never touched).
    enum class KioskPage { List, Keys, Quit, Browse, RomDirs };
    enum class KioskZone { Games, Actions };
    static constexpr int kKioskActionCount = 4;   // Restart/Keyboard/ROMs/Quit

    bool        kioskMenuOpen_  = false;
    KioskPage   kioskPage_      = KioskPage::List;
    KioskZone   kioskZone_      = KioskZone::Games;
    int         kioskActSel_    = 0;                // 0..kKioskActionCount-1
    int         kioskKeySel_    = 0;                // key-grid cell
    int         kioskRomDirSel_ = 0;                // ROM-folders page cursor
    int         kioskBrowseSel_ = 0;                // directory-browser cursor

    std::vector<std::string> kioskDiskPaths_;   // absolute image paths (games)
    std::vector<std::string> kioskDiskLabels_;  // display file names
    int                      kioskDiskSel_ = 0;
    std::string              kioskStatus_;      // last mount / info line
    std::string              kioskMountedPath_; // disk in the boot drive (● mark)

    std::vector<std::string> kioskRomDirs_;     // extra scan folders (persisted)
    bool                     kioskRomDirsLoaded_ = false;  // lazy-load guard

    // Directory browser scratch (populated while KioskPage::Browse is up).
    std::string              kioskBrowseDir_;
    std::vector<std::string> kioskBrowseSubdirs_;
    std::vector<std::string> kioskBrowseShortcutPaths_;
    std::vector<std::string> kioskBrowseShortcutLabels_;

    // Temporal auto-repeat for held navigation (see JoystickInput::UiNav).
    bool   kioskNavHeld_ = false;
    double kioskNavNextT_ = 0.0;   // ImGui::GetTime() of next allowed step
    // Whether we actively parked the worker (Mode::Stopped) for the menu, so
    // we only resume (Mode::Running) a machine we paused ourselves.
    bool   kioskPausedByMenu_ = false;
    // Menu → game input isolation across the close (see
    // pollJoystickAndPushToMemory): the poll samples kioskMenuOpen_ a frame
    // behind updateKioskMenu, and Circle/Cross double as menu B/A and Apple
    // PB0/PB1 — swallow the shared buttons until the pad is fully released.
    bool   kioskMenuWasOpen_ = false;
    bool   kioskSwallowPad_  = false;

    // One-shot dedup for the "pad bound / gamepad-mapped" diagnostic log.
    int  loggedJoyHost_    = -2;   // -2 = never logged
    bool loggedJoyGamepad_ = false;

    // In-game D-pad → Apple II arrow-key auto-repeat (IIe-style: fire on press,
    // then repeat while held). Index 0..3 = up/down/left/right.
    bool   padArrowHeld_[4]  = { false, false, false, false };
    double padArrowNextT_[4] = { 0.0, 0.0, 0.0, 0.0 };

    /// Poll gamepad/keyboard and drive the whole kiosk menu state machine
    /// (open/close, page/zone navigation, activation). Once per frame in
    /// kiosk mode. Also keeps the pause state in sync with the active page.
    void updateKioskMenu();
    /// Draw the active kiosk page (list / keyboard / quit / browse / romdirs)
    /// when the menu is open.
    void renderKioskMenu();

    /// Open the Start menu on the GAMES zone and rescan the disk list.
    void openKioskStartMenu();
    /// (Re)build kioskDiskPaths_ from the booted disk's folder + extra ROM
    /// folders, sorted by name-proximity to the mounted disk. Also called on
    /// the RomDirs → List transition so folder edits show up immediately.
    /// No-op-safe with no Disk II / no folder.
    void kioskRescanDisks();
    /// Swap the highlighted disk into the boot Disk II drive without reboot.
    void kioskMountSelected();
    /// Validate the focused zone's item (mount a disk, or run an action).
    void kioskActivateFocused();
    /// Send the highlighted key-grid cell to the running machine (live).
    void kioskInjectSelectedKey();
    /// The Disk II card the kiosk selector targets: slot 6 if present, else
    /// the primary card. nullptr when the config has no Disk II at all.
    DiskIICard* kioskBootDiskCard();

    // Pause helper: park (Mode::Stopped) / resume (Mode::Running) the worker
    // only when we're the one that paused it. `want==true` → paused.
    void kioskSetPaused(bool want);

    // ── ROM-folders manager + directory browser helpers ────────────────
    void kioskScanBrowse(const std::string& dir);     // fill subdirs for `dir`
    void kioskComputeShortcuts();                      // /, Home, mounts
    void kioskLoadRomDirs();                            // read persisted list
    void kioskSaveRomDirs();                            // write persisted list
    bool kioskPruneRomDirs();                          // drop vanished folders

    // Slot of the HDV card auto-plugged by ensureHdvCardForBoot for a CLI
    // `POM2 <image.hdv>` boot (-1 = none). Session-local: NOT persisted, so
    // ~MainWindow must skip writing slot_N_card / hdv_path for this slot.
    int autoProvisionedHdvSlot_ = -1;

    // Same contract for the SmartPort card auto-plugged by the Floppy Emu
    // panel's ensureSmartPort (-1 = none): session-local, never persisted.
    int autoProvisionedSmartPortSlot_ = -1;

#ifdef __EMSCRIPTEN__
    std::string browserResetBootImage_;
#endif

    // ── Mouse Card host-input plumbing (Phase 5) ─────────────────────
    // Apple II Screen widget rect, window-relative. Updated every
    // frame by `renderScreenWindow()` so the GLFW cursor-pos callback
    // (which fires async to the render loop) can decide whether to
    // route motion to the Mouse Card.
    ImVec2 screenRectMin{ 0, 0 };
    ImVec2 screenRectMax{ 0, 0 };
    // Running 8-bit Apple II "mouse units" counter — mirrors MAME's
    // IPT_MOUSE_X/Y (0..0xFF wrapping). The MCU firmware computes
    // deltas with 8-bit subtraction.
    uint8_t mouseAppleX = 0;
    uint8_t mouseAppleY = 0;
    // Last GLFW cursor position; used to compute per-frame deltas.
    double  lastMouseHostX = 0;
    double  lastMouseHostY = 0;
    bool    mouseInited    = false;
    bool    mouseButtonHeld = false;
    // Title-bar-only drag state for the Apple II Screen window. The
    // window is opened with `ImGuiWindowFlags_NoMove` so ImGui never
    // starts a window-move from the content area (clicks pass through
    // to the Mouse Card). When the user mouses-down on the title bar
    // we latch this flag and apply `io.MouseDelta` to the window pos
    // ourselves until the button is released.
    bool    screenDraggingByTitleBar = false;
    // Sub-tick accumulator: dx host pixels × (display.width()/widget_w)
    // = Apple-coord delta. We'd lose fractional motion if we truncated
    // each event, so carry the remainder across calls. Per-event delta
    // is clamped to ±127 (MCU's 8-bit signed wrap range) BEFORE we
    // subtract from the accumulator, so >127-tick events keep their
    // residual for the next event.
    double  mouseSubAppleX = 0.0;
    double  mouseSubAppleY = 0.0;

    // ── Absolute closed-loop cursor sync (host ⇄ guest) ────────────────
    // The Mouse Card is a purely *relative* quadrature device, so a host
    // delta fed open-loop drifts away from the guest cursor in absolute
    // terms (clamp-edge losses + scale mismatch). When the AppleMouse
    // firmware is active (mode byte $07F8+s bit 0) we instead drive the
    // guest *toward* the host cursor's absolute position-in-widget, read
    // back from the firmware screen holes ($0478+s/$0578+s low/high X,
    // $04F8+s/$05F8+s low/high Y — layout per the Apple II Mouse FAQ).
    // To avoid quadrature windup (host cursor events fire far faster than
    // the app polls READMOUSE, so the holes are stale between polls) we
    // edge-trigger: a correction is injected only once the holes have
    // moved, i.e. once the app has consumed the previous batch. Sentinel
    // -1 forces the first correction.
    int  lastSyncHoleX  = -1;
    int  lastSyncHoleY  = -1;
    bool mouseSyncActive = false;   // true while in absolute mode (mouse on)

    /// Populate the SlotBus from `slot_1_card`..`slot_7_card` settings,
    /// instantiating each card with its slot number. Falls back to legacy
    /// defaults (DiskII=6, HDV=5, SSC=2, Clock=4, ChatMauve=7) when a
    /// slot key is absent. Validates uniqueness — duplicate card-type
    /// requests log a warning and skip the second instance. Populates the
    /// raw `*Card` pointer fields and the `slotCards[]` index.
    void plugSlotsFromSettings();

    // ── Disk insert+boot routing (shared by Disk Library UI + CLI) ───────
    // Promoted from file-local lambdas in renderDiskLibraryWindow so
    // insertAndBootImage() can reuse the exact same routing. Each takes
    // the state lock internally.
    /// Route a 3.5" image to the //c+ on-board hub or a SmartPort card
    /// unit `driveIdx`, auto-creating a SmartPort35Unit if needed.
    bool routeMount35 (int driveIdx, const std::string& path, std::string& errOut);
    /// Route an HDV image to the ProDOSHardDiskCard or a SmartPort card's
    /// unit 0, auto-creating a SmartPortHdvUnit if needed. `bootSlotOut`
    /// receives the slot to boot from.
    bool routeMountHdv(const std::string& path, int& bootSlotOut, std::string& errOut);
    /// The active HDV-class block device for the HDV Library / turbo / eject —
    /// prefers the MAME-faithful CffaCard when plugged, else the synthetic
    /// ProDOSHardDiskCard. nullptr when neither is present.
    /// NOTE: this is the *primary* (single-target) accessor kept for the
    /// legacy menu/library paths; for anything that must touch EVERY block
    /// card (eject-all, auto-turbo, the Slot Manager) enumerate via
    /// `blockCards()` so a second block card isn't silently ignored.
    pom2::ProDOSBlockCard* hdvDevice() const;
    /// Enumerate ALL plugged ProDOS block-device cards (synthetic HDV +
    /// MAME-faithful CFFA), sorted by slot ascending, by walking the
    /// SlotBus. The bus — not our raw `*Card` pointers — is the source of
    /// truth, so this stays correct when several block cards coexist.
    std::vector<pom2::ProDOSBlockCard*> blockCards() const;
    /// Enumerate ALL plugged SmartPort (Liron-class) slot cards, sorted by
    /// slot ascending. Same rationale as `blockCards()`.
    std::vector<pom2::SmartPortCard*> smartPortCards() const;
    /// Ensure the config can host an HDV image: returns the slot of an
    /// existing HDV or SmartPort card, or plugs a fresh ProDOSHardDiskCard
    /// into a free slot (preferring slot 7) and returns that. Used by the
    /// CLI/kiosk launcher so `POM2 game.hdv` boots even when the saved slot
    /// config has only Disk II cards. Returns -1 if no free slot exists.
    /// NOT persisted — the saved slot configuration is left untouched.
    int  ensureHdvCardForBoot();

    void renderMenuBar();
    void renderStatusBar();
    void renderScreenWindow();
    /// Draw the Apple II framebuffer texture into the current ImGui
    /// window's content region, scaled + centred (letterboxed) to preserve
    /// the 4:3 aspect. Shared by the normal screen window and the kiosk
    /// full-viewport path. Updates screenRectMin/Max for Mouse Card routing.
    void drawScreenImage();
    /// Kiosk render path: a single borderless full-viewport window showing
    /// only the screen. No menu bar / toolbar / panels.
    void renderKiosk();
    void renderMemoryViewerWindow();
    // Memory map visualisations — implemented in MainWindow_MemoryMaps.cpp.
    std::vector<MemRegion> buildMemoryRegions();
    void renderMemoryBarWindow();
    void renderMemoryBarHorizontalWindow();
    void renderMemoryGridWindow();
    void renderCassetteDeckWindow(float deltaSeconds);
    void renderHgrPaintWindow();
    void renderHgrSpriteWindow();
    void renderRewindWindow(float deltaSeconds);
    // Drive hold-to-rewind from the combined input sources (F6 key + toolbar
    // button). Edge-detected against rewindHeldPrev_ — call once per frame.
    void driveRewindHold(bool held);
    void renderTapeFileDialogs();
    void renderPasteFileDialog();
    void updateAutoTurbo();
    void renderDiskPanelWindow();
    void renderDiskFileDialog();
    void renderDiskLibraryWindow();
    void renderDisk35PanelWindow();
    void renderDisk35FileDialog();
    void renderHdvPanelWindow();
    void renderHdvFileDialog();
    void renderSmartPortPanelWindow();
    void renderChatMauvePanelWindow();
    void renderMockingboardPanelWindow();
    void renderPhasorPanelWindow();
    void renderEchoPlusPanelWindow();
    void renderSscPanelWindow();
    void renderPrinterPanelWindow();
    void renderNoSlotClockPanelWindow();
    void renderJoystickPanelWindow();
    /// Mouse Inspector — diagnostic panel for cursor-alignment tuning.
    /// Live host cursor position + Apple II Screen widget rect + Mouse
    /// Card running counter + AppleWin HLE firmware state + screen holes.
    /// Optional CSV log to `mouseInspectorLogPath` for offline review.
    void renderMouseInspectorWindow();
    void renderAudioMixerWindow();
    void renderNtscSettingsWindow();
    void renderVoxelSettingsWindow();
    void renderAiControlPanelWindow();
    void pollJoystickAndPushToMemory();
    void renderAboutDialog();
    void renderWelcomePanelWindow();
    /// Lazy-loads `pic/Apple_II_plus.jpg` into `aboutImageTex_` on the
    /// first About-dialog open. Silent no-op if the asset can't be
    /// resolved via ResourcePaths (kiosk builds, missing pic/ folder).
    void ensureAboutImageLoaded();
    /// Slot Configuration panel (two columns): left = per-slot card
    /// assignment (built-ins greyed), right = internal disks + mountable
    /// ports of plugged storage cards. Implemented in MainWindow_Slots.cpp.
    void renderSlotConfigPanel();
    /// Persist a media bay's state with the right key scheme for its card
    /// type (SmartPort per-unit / CFFA per-slot / synthetic HDV global key).
    /// Called under stateMutex right after a mount/eject/type/write-back.
    void persistMediaBay(int slot, int bay, SlotPeripheral* p);
    /// BMOW Floppy Emu panel — OLED-style SD browser + emulation-mode select.
    /// Routes the selected image into the matching controller card
    /// (DiskIICard / SmartPortCard units) per the device's current mode.
    void renderFloppyEmuWindow();
    /// Tear down the SlotBus and re-run plugSlotsFromSettings(). Called
    /// by the Slot Configuration panel's Apply button. Stops the
    /// emulation worker around the rebuild so card destructors / new
    /// constructors don't race against a running CPU thread.
    void restartEmulationFromSettings();

    /// Pick the first existing file path in `candidates`. Empty string
    /// when none exists. Used by applyProfile to probe ROM candidates.
    static std::string firstExistingPath(const std::vector<std::string>& candidates);

    /// Resolve the CPU mode to use given the profile default + the
    /// `cpu_mode_override` setting. `auto` (default) → profile.defaultCpu;
    /// `nmos` → NMOS; `65c02` → CMOS. Logged to console on each apply.
    M6502::CpuMode resolveCpuMode(M6502::CpuMode profileDefault) const;

    /// Pick the motor-sound pitch multiplier for a profile. //c / //c+
    /// use a Sony internal 5.25" drive that spins up faster and at a
    /// higher pitch than the original Shugart-based Disk II — bumping
    /// the motor pitch on those profiles approximates that without
    /// dragging in a second sample set. All other profiles → 1.0
    /// (native MAME Disk II samples).
    static float floppyMotorPitchForProfile(pom2::SystemProfile p);

    // Paste helpers — feed text into the keyboard buffer.
    void pasteFromClipboard();
    void pasteFromFile(const std::string& path);

    // Write the current display framebuffer to ./screenshot_NNN.ppm. The
    // sequence number auto-increments to avoid overwriting prior captures
    // within the same session.
    void saveScreenshot();

    // Eject every loaded image (Disk II, HDV, SmartPort units, 3.5").
    // Shared by the Disk Library header-row button.
    void ejectAllDisks();

    void uploadScreenTexture();

    // Translate a GLFW key/codepoint into an ASCII byte and feed it to
    // Memory::queueKey(). Memory's strobe handling is hardware-faithful;
    // we just hand it the character.
    void injectAscii(uint8_t ascii);
};

#endif // POM2_MAIN_WINDOW_H
