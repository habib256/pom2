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

// MainWindow_MiscPanels — the ImGui bodies for the memory viewer, the
// cassette deck, the HGR/DHGR paint and sprite editors, and the rewind
// timeline. Moved out of MainWindow.cpp verbatim. (The keyboard and welcome
// panels stay in MainWindow.cpp: they load a texture via the stb_image
// instance defined there under STB_IMAGE_STATIC, whose symbols are internal
// to that translation unit.)

#include "MainWindow.h"
#include "DevicePanelCoordinator.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "MouseCoordinator.h"
#include "PrinterCoordinator.h"
#include "SmartPortCard.h"
#include "StorageCoordinator.h"

// stb_image is bundled (single-header public-domain JPEG/PNG decoder) solely
// for the About-dialog Apple ][+ photo and the //e keyboard photo — both
// loaded from this file, which is why the implementation macro lives here and
// nowhere else. STB_IMAGE_STATIC keeps the unused entry points internal; the
// resulting -Wunused-function noise is suppressed locally rather than by
// tagging the third-party header.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "AbstractionLevels_ImGui.h"
#include "CassetteDevice.h"
#include "IconsFontAwesome6.h"
#include "Keyboard_ImGui.h"
#include "AppleIIeKeyboardLayout.h"
#include "Pom2GL.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SystemProfile.h"
#include "Version.h"
#include "DebugCoordinator.h"
#include "EmulationController.h"

#include "CassetteDeck_ImGui.h"
#include "CassetteDevice.h"
#include "MemoryViewer_ImGui.h"
#include "Pom2HgrPaintHost.h"
#include "hgrpaint/HgrPaintEditor.h"
#include "hgrsprite/HgrSpriteEditor.h"
#include "Rewind_ImGui.h"
#include "Settings.h"

#include "imgui.h"
#include <fstream>

void MainWindow::renderMemoryViewerWindow()
{
    // The whole body — window, locked snapshot, and the flush that MUST
    // happen after the lock is released — belongs to DebugCoordinator. The
    // ordering is the reason it is one unit: the write sink re-takes
    // stateMutex to push each poke through Memory::memWrite like a CPU store,
    // and that mutex is non-recursive, so flushing inline would freeze the UI
    // thread while it still holds the lock the worker needs at its next chunk.
    debugCoordinator_->renderMemoryViewer(show(pom2::PanelId::MemViewer));
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!show(pom2::PanelId::Cassette)) return;

    // Build the deck snapshot under stateMutex — cheap enough that holding
    // the emul lock for the time it takes to copy a dozen scalars is fine.
    pom2::CassetteDeck_ImGui::DeckSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const CassetteDevice& d = controller->cassette();
        snap.loadedTape          = d.hasLoadedTape();
        snap.recordedTape        = d.hasRecordedTape();
        snap.playbackActive      = d.isPlaybackActive();
        snap.playbackArmed       = d.isPlaybackArmed();
        snap.rewinding           = d.isRewinding();
        snap.audioAvailable      = d.isAudioAvailable();
        snap.playbackPaused      = d.isPlaybackPaused();
        snap.audioStreamMode     = d.isAudioStreamMode();
        snap.queuedAudioSeconds  = d.getQueuedAudioSeconds();
        snap.playbackPositionSec = d.getPlaybackPositionSeconds();
        snap.playbackTotalSec    = d.getPlaybackTotalSeconds();
        snap.loadedTransitions   = d.getLoadedTransitionCount();
        snap.recordedTransitions = d.getRecordedTransitionCount();
        snap.volume              = d.getVolume();
        snap.loadedTapePath      = d.getLoadedTapePath();
        snap.loadInfo            = d.getLoadInfo();
    }

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    auto result = cassetteDeck->render("Cassette Deck",
                                      show(pom2::PanelId::Cassette),
                                      controller.get(),
                                      snap,
                                      deltaSeconds);

    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 4.0;  // show for 4 seconds
    }
}

void MainWindow::renderHgrPaintWindow()
{
    if (!show(pom2::PanelId::HgrPaint)) return;

    // 64 KB main-RAM (+ IIe aux) snapshot under stateMutex — the editor's
    // per-frame canvas/shadow read source (same idiom as the deck snapshot).
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }

    const float w = hgrpaint::kHiresWidth  * 3.0f + 40.0f;
    const float h = hgrpaint::kHiresHeight * 3.0f + 180.0f;
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Paint Editor", &show(pom2::PanelId::HgrPaint)))
        hgrPaintEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                  : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::renderHgrSpriteWindow()
{
    if (!show(pom2::PanelId::HgrSprite)) return;
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Sprite Editor", &show(pom2::PanelId::HgrSprite)))
        hgrSpriteEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                   : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::driveRewindHold(bool held)
{
    // Edge-detect: hold → step the machine backwards one frame; release →
    // resume live from the rewound point. No-op when recording is off
    // (holdRewind/beginScrub bail out), so it never surprises a non-user.
    if (held)                  rewindPanel_->holdRewind(*controller, 1);
    else if (rewindHeldPrev_)  rewindPanel_->releaseHold(*controller);
    rewindHeldPrev_ = held;
}

void MainWindow::renderRewindWindow(float deltaSeconds)
{
    if (!show(pom2::PanelId::Rewind)) return;
    auto result = rewindPanel_->render("Rewind", show(pom2::PanelId::Rewind), *controller, deltaSeconds);
    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
}
void MainWindow::renderAbstractionPanel()
{
    if (!show(pom2::PanelId::Abstraction)) return;
    if (!abstractionPanel)
        abstractionPanel = std::make_unique<pom2::AbstractionLevels_ImGui>();

    using Panel = pom2::AbstractionLevels_ImGui;
    using Live  = Panel::Live;
    Panel::Snapshot snap;

    // Plug state comes from the live slot map rather than from the dozen
    // `*Card` pointers: one uniform test that covers every catalogued card,
    // including the ones MainWindow keeps no pointer to.
    auto plugged = [&](const char* key) {
        for (int s = 1; s <= 7; ++s)
            if (slotCards[s] == key) return true;
        return false;
    };
    auto row = [&](const char* id, Live live, const char* detail = "") {
        Panel::Row r;
        r.id     = id;
        r.live   = live;
        r.detail = detail;
        snap.rows.push_back(std::move(r));
    };
    // A card that is plugged but running its fallback ROM: still working,
    // still wrong about its level. `actual` is where it really sits.
    auto degradable = [&](const char* id, bool isPlugged, bool atFullLevel,
                          pom2::AbsLevel fallback, const char* why) {
        Panel::Row r;
        r.id = id;
        if      (!isPlugged)   r.live = Live::NotPlugged;
        else if (atFullLevel)  r.live = Live::Active;
        else                 { r.live = Live::Degraded; r.actual = fallback;
                               r.detail = why; }
        snap.rows.push_back(std::move(r));
    };

    // ── Storage ─────────────────────────────────────────────────────────
    // Disk II is the sharpest case in the whole table: no P6 dump and no WOZ
    // mounted means the legacy 32-cycle nibble gate, which reads stock DOS
    // 3.3 fine and loses every bitstream-reading protection. `usingBitLss()`
    // is the honest test — a mounted WOZ forces the L0 path even with no
    // roms/diskii_p6.rom, using the embedded default P6.
    degradable("diskii", primaryDiskII() != nullptr,
               primaryDiskII() && primaryDiskII()->usingBitLss(), pom2::AbsLevel::H1,
               "roms/diskii_p6.rom absent and no WOZ mounted — running the "
               "legacy 32-cycle nibble gate, which cannot decode "
               "bitstream-level copy protection.");
    row("diskimage", primaryDiskII() ? Live::Active : Live::NotPlugged);
    row("cffa", plugged("cffa")        ? Live::Active : Live::NotPlugged);
    row("hdv",  plugged("hdv")         ? Live::Active : Live::NotPlugged);
    degradable("smartportcard", plugged("smartport35"),
               primarySmartPortCard() && primarySmartPortCard()->isLironRomLoaded(),
               pom2::AbsLevel::H1,
               "roms/liron.rom absent — the slot page and the $C800 bank are "
               "POM2's synthetic firmware instead of the real Liron ROM.");
    {
        const bool iicClass =
            activeProfile == pom2::SystemProfile::AppleIIc ||
            activeProfile == pom2::SystemProfile::AppleIIcPlus ||
            activeProfile == pom2::SystemProfile::AppleIIcPAL;
        row("iicsp", iicClass ? Live::Active : Live::NotPlugged,
            iicClass ? "Armed only by an explicit boot from slot 5; every "
                       "reset disarms it." : "");
        row("iwm", activeProfile == pom2::SystemProfile::AppleIIcPlus
                       ? Live::Active : Live::NotPlugged);
        row("sony35", (iicClass || plugged("smartport35"))
                       ? Live::Active : Live::NotPlugged);
    }
    row("prodosvol", Live::NotApplicable);

    // ── Input, clocks, printing ─────────────────────────────────────────
    const auto mouseInventory = mouseCoordinator_->capture();
    row("mouse",   mouseInventory.mamePlugged     ? Live::Active : Live::NotPlugged);
    row("mouseaw", mouseInventory.appleWinPlugged ? Live::Active : Live::NotPlugged);
    degradable("clock", plugged("clock"),
               devicePanelCoordinator_->captureInventory().clockRomFromDump,
               pom2::AbsLevel::H1,
               "roms/thunderclock_u9_v1.3.bin absent — running the synthetic "
               "ProDOS-signature stub, so tools that pull the driver off the "
               "card find nothing.");
    degradable("grappler", plugged("grappler"),
               printerCoordinator_->captureHost(*controller).grapplerRomLoaded,
               pom2::AbsLevel::H1,
               "roms/grappler_plus.bin absent — running buildStubRom(), so "
               "the real Orange Micro firmware is not executing.");
    row("printercard", plugged("printer") ? Live::Active : Live::NotPlugged);
    row("nsclock", controller->noSlotClock().isEnabled()
                       ? Live::Active : Live::NotPlugged);

    // ── Video ───────────────────────────────────────────────────────────
    // Exactly one of the two colour paths is running, which makes this pair
    // the clearest live illustration of the axis in the whole panel.
    {
        const auto hi = display->getHiResMode();
        const bool oe = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                         hi == Apple2Display::HiResMode::ColorCompositeOECpu);
        row("oe",  oe ? Live::Active : Live::NotPlugged,
            oe ? "Signal-level demodulation of the 14.318 MHz 1-bit stream."
               : "Pick a Composite (OpenEmulator) mode in Display to run it.");
        row("lut", oe ? Live::NotPlugged : Live::Active,
            oe ? "" : "Artifact colours are read from a table; no signal is "
                      "synthesised.");
    }
    row("chatmauve", devicePanelCoordinator_->captureInventory().chatMauvePlugged()
                         ? Live::Active : Live::NotPlugged);

    // ── Audio, network, CPU ─────────────────────────────────────────────
    row("mockingboard", (plugged("mockingboard") || plugged("mockingboard_c") ||
                         plugged("phasor")) ? Live::Active : Live::NotPlugged);
    row("ssi263", (plugged("echoplus") || plugged("mockingboard_c"))
                      ? Live::Active : Live::NotPlugged);
    row("tms5220", plugged("echoplus_tms") ? Live::Active : Live::NotPlugged);
    row("ssc",       plugged("ssc")       ? Live::Active : Live::NotPlugged);
    row("uthernet",  plugged("uthernet")  ? Live::Active : Live::NotPlugged);
    row("uthernet2", plugged("uthernet2") ? Live::Active : Live::NotPlugged);
    // The one entry in this group that used to report nothing, so it
    // defaulted to NotApplicable ("always present") — which is exactly the
    // silent-degradation blind spot the panel exists to close. libslirp is
    // an OPTIONAL build dependency: without it `SlirpNetworkBackend`
    // compiles to a stub that always fails, so Uthernet I has no transport
    // at all and Uthernet II is confined to its own hardware stack.
#ifdef POM2_HAVE_SLIRP
    row("netbackend", Live::Active,
        "libslirp linked — user-mode NAT available to both Uthernet cards.");
#else
    row("netbackend", Live::NotPlugged,
        "Built without libslirp: no user-mode NAT. Uthernet I (raw frames) "
        "has no transport; Uthernet II still does TCP/UDP through its own "
        "W5100 hardware stack.");
#endif
    row("fujinet",   plugged("fujinet")   ? Live::Active : Live::NotPlugged);
    row("softcard",  plugged("softcard")  ? Live::Active : Live::NotPlugged);
    row("z80",       plugged("softcard")  ? Live::Active : Live::NotPlugged);

    // ── The switchable boundaries ───────────────────────────────────────
    // Availability is gated on the dump each low side needs, because the
    // whole point of the panel is that a missing dump silently costs you a
    // level — offering a switch that would land on the fallback would repeat
    // the exact mistake it exists to expose.
    auto have = [](const char* rel) {
        return !pom2::findResource(rel).empty();
    };
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::MouseCard;
        t.title        = "Mouse Card";
        t.needsRestart = true;
        t.low.label    = "MAME — the M68705 MCU executes its mask ROM";
        t.low.level    = pom2::AbsLevel::L0;
        t.low.available = have("roms/mouse_341-0270-c.bin") &&
                          have("roms/mouse_341-0269.bin");
        t.low.blockedBy = "both roms/mouse_341-0270-c.bin (slot EPROM) and "
                          "roms/mouse_341-0269.bin (MCU mask ROM) are needed";
        t.low.why      = "Decodes real quadrature edges: at most one edge per\n"
                         "axis per MCU PortB read, so fast host motion is\n"
                         "rate-limited exactly as the hardware limits it.";
        t.high.label   = "AppleWin HLE — the MCU is a C++ state machine";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.available = have("roms/mouse_341-0270-c.bin");
        t.high.blockedBy = "roms/mouse_341-0270-c.bin (slot EPROM) is needed";
        t.high.why     = "Copies the host delta straight into the HLE'd MCU,\n"
                         "so it never drops motion — smoother, and less\n"
                         "correct. Needs a compensating absolute cursor sync\n"
                         "the L0 card does not.";
        const auto mousePlugged = mouseCoordinator_->capture();
        t.selected     = mousePlugged.mamePlugged ? 0
                       : (mousePlugged.appleWinPlugged ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add a mouse in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::BlockStorage;
        t.title        = "ProDOS block storage";
        t.needsRestart = true;
        t.low.label    = "CFFA 2.0 — the real 4 KB firmware executes over ATA";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/cffa20ee02.bin") ||
                          have("roms/cffa20eec02.bin");
        t.low.blockedBy = "roms/cffa20ee02.bin (or the 65C02 variant) is needed";
        t.low.why      = "An ATA taskfile model isomorphic to MAME's\n"
                         "cs0_r/cs0_w, driven by the card's own firmware.\n"
                         "Skips DMA / IRQ / SMART.";
        t.high.label   = "HDV card — synthetic ROM, 4-register port, memcpy";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "H1 IS the feature here: it mounts .hdv/.2mg\n"
                         "directly with no card ROM dump at all. The ProDOS\n"
                         "block corpus has no protection to lose.";
        t.selected     = plugged("cffa") ? 0 : (plugged("hdv") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::PrinterIface;
        t.title        = "Printer interface";
        t.needsRestart = true;
        t.low.label    = "Grappler+ — the real Orange Micro EPROM executes";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/grappler_plus.bin");
        t.low.blockedBy = "roms/grappler_plus.bin is needed";
        t.low.why      = "Status byte, register decode, $C800 banking and the\n"
                         "S1 DIPs, line-cited against MAME grappler.cpp.\n"
                         "What AppleWorks and the graphics dumps expect.";
        t.high.label   = "Printer card — synthetic ROM, PR#n hook only";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "A CSWL/CSWH hook and a 4-byte trampoline. No PROM\n"
                         "dump exists to run, and the Pascal entry block is\n"
                         "deliberately absent, so BASIC PR#n only.";
        t.selected     = plugged("grappler") ? 0 : (plugged("printer") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id         = pom2::AbsToggle::CompositeVideo;
        t.title      = "Colour pipeline";
        t.low.label  = "Composite (OpenEmulator) — demodulate a real signal";
        t.low.level  = pom2::AbsLevel::L1;
        t.low.why    = "The display emits a 14.318 MHz 1-bit luminance\n"
                       "stream; the shader demodulates Y/I/Q off the\n"
                       "subcarrier. Artifact colour is EMERGENT.";
        t.high.label = "Artifact LUT — look the colour up per dot pattern";
        t.high.level = pom2::AbsLevel::H1;
        t.high.why   = "MAME's composite colour tables: the RESULT of NTSC\n"
                       "artifacting, tabulated. Cheap, sharp, and unable to\n"
                       "show anything the table has no entry for.";
        const auto hi = display->getHiResMode();
        t.selected   = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                        hi == Apple2Display::HiResMode::ColorCompositeOECpu)
                           ? 0 : 1;
        t.note       = "Mono modes are neither — they bypass colour entirely.";
        snap.toggles.push_back(std::move(t));
    }

    const Panel::Request req = abstractionPanel->render(&show(pom2::PanelId::Abstraction),
                                                       snap);
    switch (req.toggle) {
        case pom2::AbsToggle::None:
            break;
        case pom2::AbsToggle::MouseCard:
            swapSlotCardVariant(req.option == 0 ? "mouseaw" : "mouse",
                                req.option == 0 ? "mouse" : "mouseaw");
            break;
        case pom2::AbsToggle::BlockStorage:
            swapSlotCardVariant(req.option == 0 ? "hdv" : "cffa",
                                req.option == 0 ? "cffa" : "hdv");
            break;
        case pom2::AbsToggle::PrinterIface:
            swapSlotCardVariant(req.option == 0 ? "printer" : "grappler",
                                req.option == 0 ? "grappler" : "printer");
            break;
        case pom2::AbsToggle::CompositeVideo:
            // No restart: the colour pipeline is a render-path choice, and
            // the machine never sees it. Persisted by the dtor's hi_res_mode
            // write, exactly like a View-menu pick.
            display->setHiResMode(
                req.option == 0 ? Apple2Display::HiResMode::ColorCompositeOE
                                : Apple2Display::HiResMode::ColorNTSC);
            lastColorHiResMode_ = display->getHiResMode();
            break;
    }
}

void MainWindow::ensureKeyboardImageLoaded()
{
    if (kbImageTried_) return;
    kbImageTried_ = true;

    const std::string path = pom2::findResource("pic/Keyboard_AppleIIe.jpeg");
    if (path.empty()) {
        kbImageError_ = "not found in any resource search dir";
        return;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        kbImageError_ = stbi_failure_reason() ? stbi_failure_reason()
                                              : "decode failed";
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // LINEAR both ways: the photo is 2578 px wide and the window is usually
    // narrower, so it is nearly always minified — GL_NEAREST turned the key
    // legends into aliased mush at every size but 1:1.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    kbImageTex_ = tex;
    kbImageW_   = w;
    kbImageH_   = h;
}

void MainWindow::renderKeyboardPanel()
{
    if (!show(pom2::PanelId::Keyboard)) {
        // A latched Open-Apple must not outlive the window that shows it as
        // down: with the panel closed there is nothing to un-latch it with,
        // and the guest would see a key held forever.
        //
        // EDGE-TRIGGERED on the close, not run every frame the window is
        // shut. `keyboardPanel` is never destroyed once built, so the
        // unconditional form kept firing for the rest of the session — and
        // since it wrote $C061/$C062 directly, it also stamped the host's
        // Left/Right Alt back to false 60x/s. Dropping only THIS source and
        // letting `pushAppleKeys()` re-OR is what keeps Alt working.
        if (keyboardPanel && kbPanelWasOpen_) {
            keyboardPanel->releaseAll();
            appleKeys_.releasePanel();
            pushAppleKeys();
        }
        kbPanelWasOpen_ = false;
        return;
    }
    kbPanelWasOpen_ = true;
    ensureKeyboardImageLoaded();
    if (!keyboardPanel)
        keyboardPanel = std::make_unique<pom2::Keyboard_ImGui>();

    const auto ev = keyboardPanel->render(&show(pom2::PanelId::Keyboard), kbImageTex_,
                                          kbImageW_, kbImageH_, kbImageError_);

    // The Apple keys are LEVELS, not events: $C061/$C062 bit 7 reads the
    // switch, so the latch has to be pushed every frame for as long as it is
    // down, exactly like the host's Left/Right Alt in onKey.
    const auto& lat = keyboardPanel->latches();
    appleKeys_.setPanel(lat.openApple, lat.solidApple);
    pushAppleKeys();

    if (!ev.key) return;

    const pom2::KeyHotspot& k = *ev.key;
    bool consumedOneShots = false;

    if (k.kind == pom2::KeyKind::Char) {
        char c = ev.latches.shift ? k.shift : k.base;
        // Caps Lock is a LETTER latch on the //e, not a shift: it uppercases
        // A-Z and leaves the digit row alone (which is why the number keys
        // still need Shift for their symbols on a real machine).
        if (ev.latches.caps && c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        uint8_t code = static_cast<uint8_t>(c);
        if (ev.latches.control) {
            // Ctrl-A..Ctrl-Z = $01..$1A, on either case of the letter.
            const char up = (c >= 'a' && c <= 'z')
                                ? static_cast<char>(c - 'a' + 'A') : c;
            if (up >= 'A' && up <= 'Z')
                code = static_cast<uint8_t>(up - 'A' + 1);
        }
        injectAscii(code);
        consumedOneShots = true;
    } else {
        switch (k.action) {
            case pom2::KeyAction::Esc:    injectAscii(0x1B); break;
            case pom2::KeyAction::Tab:    injectAscii(0x09); break;
            case pom2::KeyAction::Return: injectAscii(0x0D); break;
            // $7F is what the //e's DELETE cap actually generates. It is NOT
            // the $08 the host Backspace injects — that one is the left
            // arrow's code, which is what a II/II+ had instead of a DELETE
            // key. The cap in the photo says Del, so it sends Del.
            case pom2::KeyAction::Delete: injectAscii(0x7F); break;
            case pom2::KeyAction::Left:   injectAscii(0x08); break;
            case pom2::KeyAction::Right:  injectAscii(0x15); break;
            case pom2::KeyAction::Down:   injectAscii(0x0A); break;
            case pom2::KeyAction::Up:     injectAscii(0x0B); break;
            case pom2::KeyAction::Reset:
                // Faithful: RESET alone does nothing on any Apple II — the
                // key is wired through the keyboard encoder's Ctrl line
                // precisely so a stray knock cannot reboot the machine. So
                // the panel refuses too, and says why, rather than quietly
                // being more dangerous than the hardware.
                if (!ev.latches.control) {
                    tapeStatusMessage =
                        "Reset needs Control — latch CONTROL, then click Reset "
                        "(Open-Apple too for a cold boot).";
                    tapeStatusUntil = lastFrameTime + 6.0;
                    break;
                }
                // Open-Apple+Ctrl+Reset is the //e's cold boot; Ctrl+Reset
                // alone is the warm one. Same two verbs as F12 / F11.
                if (ev.latches.openApple) {
                    controller->hardReset();
                    tapeStatusMessage = "Open-Apple + Ctrl + Reset — cold boot";
                } else {
                    controller->softReset();
                    tapeStatusMessage = "Ctrl + Reset";
                }
                tapeStatusUntil  = lastFrameTime + 3.0;
                consumedOneShots = true;
                break;
            default: break;
        }
        if (k.action != pom2::KeyAction::Reset) consumedOneShots = true;
    }

    if (consumedOneShots) keyboardPanel->clearOneShots();
}

void MainWindow::ensureAboutImageLoaded()
{
    if (aboutImageTried_) return;
    aboutImageTried_ = true;

    const std::string path = pom2::findResource("pic/Apple_II_plus.jpg");
    if (path.empty()) return;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) return;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    aboutImageTex_ = tex;
    aboutImageW_   = w;
    aboutImageH_   = h;
}

void MainWindow::renderWelcomePanelWindow()
{
    if (!show(pom2::PanelId::Welcome)) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Welcome to POM2###welcomePanel", &show(pom2::PanelId::Welcome),
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 580.0f);

    // ── No-ROM banner ────────────────────────────────────────────────
    // The single biggest newcomer trip-up: ROMs are user-provided and the
    // machine shows a bare "NO ROM" screen without them. Surface the fix
    // first, in red, only while no ROM is loaded.
    if (!romLoaded_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("No Apple II ROM is loaded yet.");
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "Apple II firmware is copyrighted, so POM2 does not ship it. "
            "Drop your firmware dump into a \"roms/\" folder next to POM2, "
            "then use File → Reload ROM (or relaunch).");
        ImGui::Spacing();

        // Which file the *active* profile wants, and where POM2 looks.
        const auto& cfg = pom2::profileConfig(activeProfile);
        if (!cfg.romProbeOrder.empty()) {
            ImGui::Text("Expected ROM for %.*s:",
                        static_cast<int>(cfg.displayName.size()),
                        cfg.displayName.data());
            for (const auto& cand : cfg.romProbeOrder)
                ImGui::BulletText("%s", cand.c_str());
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("POM2 searches these folders (first hit wins):");
        for (const auto& dir : pom2::resourceSearchDirs())
            ImGui::BulletText("%s", dir.string().c_str());

        ImGui::Spacing();
#ifndef __EMSCRIPTEN__
        if (ImGui::Button("Reload ROM (re-probe folders)")) {
            // Resolve and read with NO lock held. `findResource` stats every
            // candidate in every search directory, and the read that follows
            // is a file the user may have just dropped onto a network share;
            // both used to run inside `lockState()`, which the CPU worker
            // takes every 4096 cycles and the UI thread takes to paint.
            // Re-resolve from the active profile so dropping the
            // profile-specific dump in is picked up without a relaunch.
            std::string newRom;
            for (const auto& cand : pom2::profileConfig(activeProfile).romProbeOrder) {
                std::string r = pom2::findResource(cand);
                if (!r.empty()) { newRom = r; break; }
            }
            if (newRom.empty()) newRom = romPath;  // last-known path
            // The read itself: `Memory::loadAppleIIRom` takes a PATH, so the
            // install below still opens the file — this pass is what makes
            // that second open a warm-cache memcpy instead of a disk (or
            // network) round trip under the lock, and it is also where an
            // unreadable file is discovered, with nothing blocked behind it.
            bool readable = false;
            {
                std::ifstream probe(newRom, std::ios::binary);
                if (probe) {
                    probe.seekg(0, std::ios::end);
                    readable = probe.tellg() > 0;
                }
            }
            bool ok = false;
            if (readable) {
                auto st = controller->lockState();
                ok = st.memory().loadAppleIIRom(newRom.c_str());
                if (ok) romPath = newRom;
            } else {
                romStatus = "cannot read " + newRom;
            }
            if (ok) {
                controller->hardReset();
                romStatus  = std::string("loaded: ") + romPath;
                romLoaded_ = true;
            }
        }
        ImGui::SameLine();
#endif
        ImGui::TextDisabled("(%s)", romStatus.c_str());
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Loading software ─────────────────────────────────────────────
    ImGui::SeparatorText("Loading a disk");
    ImGui::BulletText("Drag a .woz / .dsk / .po / .nib / .hdv / .2mg onto this window.");
    ImGui::BulletText("Or File → Disk Library to browse bundled images.");
    ImGui::BulletText("Or launch from a terminal: POM2 path/to/game.woz");
    ImGui::TextDisabled("POM2 auto-routes each image to Disk II, SmartPort 3.5\" or ProDOS HDV.");

    ImGui::Spacing();
    ImGui::SeparatorText("Suggested media folders");
    ImGui::BulletText("roms/        Apple II firmware dumps");
    ImGui::BulletText("disks_5.4/   5.25\" disk images (.dsk/.woz/.nib)");
    ImGui::BulletText("disks_3.5/   3.5\" disk images (800K)");
    ImGui::BulletText("hdv/         ProDOS hard-disk images (.hdv/.2mg)");

    // ── Keys & signature features ────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Keys");
    ImGui::BulletText("F11  Reset (Ctrl-Reset)        F12  Hard reset");
    ImGui::BulletText("F9   Screenshot                F6   Hold to rewind");
    ImGui::BulletText("Left Alt = Open-Apple          Right Alt = Solid-Apple");
    ImGui::BulletText("Ctrl+Alt+F  Full screen (kiosk) \xe2\x87\x84 windowed  (F10 too)");
    ImGui::BulletText("Ctrl+V pastes clipboard text as keystrokes");

    ImGui::Spacing();
    ImGui::SeparatorText("Try these");
    ImGui::BulletText("Display → 3D voxel view  — MicroM8-style cube renderer.");
    ImGui::BulletText("Devices → Rewind (F6)    — scrub back through machine state.");
    ImGui::BulletText("Display → CRT Settings   — scanlines, phosphor, NTSC look.");
    ImGui::BulletText("Machine → Profile        — switch between ][ / ][+ / //e / //c / //c+.");

    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close")) show(pom2::PanelId::Welcome) = false;
    ImGui::SameLine();
    if (ImGui::Button("About POM2...")) { showAbout = true; }
    ImGui::End();
}

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ensureAboutImageLoaded();
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About POM2", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Photo on the left, all text flowed into a column on the right.
        if (aboutImageTex_ && aboutImageW_ > 0 && aboutImageH_ > 0) {
            // Scale to a sensible width in the dialog while preserving the
            // 800×792 aspect of the original photo (≈ 1:1).
            const float displayW = 220.0f;
            const float displayH = displayW *
                static_cast<float>(aboutImageH_) /
                static_cast<float>(aboutImageW_);
            ImGui::BeginGroup();
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<intptr_t>(aboutImageTex_)),
                         ImVec2(displayW, displayH));
            ImGui::EndGroup();
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        // Constrain wrapped text to a fixed column so it stays beside the photo.
        const float textColumnW = 380.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textColumnW);

        ImGui::Text("POM2 " POM2_VERSION_STRING);
        ImGui::Text("Apple II / II+ / //e / //c / //c+ emulator");
        ImGui::Text("MOS 6502 / 65C02 / Rockwell / WDC, Dear ImGui frontend");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Hardware accuracy comes from verbatim ports of MAME's "
            "device models. Wherever POM2 emulates a chip or a "
            "peripheral, the implementation cites the MAME source "
            "file and line range it follows.");
        ImGui::Spacing();
        ImGui::TextWrapped("Subsystems ported from MAME include:");
        ImGui::BulletText("M6502 / 65C02 dispatch table and timing");
        ImGui::BulletText("IWM (Apple Integrated Woz Machine) for //c+ and 3.5\" SmartPort");
        ImGui::BulletText("AY-3-8910 PSG + 6522 VIA (Mockingboard)");
        ImGui::BulletText("uPD1990AC RTC (ThunderClock+)");
        ImGui::BulletText("M68705P3 + MC6821 PIA (Mouse Card)");
        ImGui::BulletText("WozFDC / Disk II LSS + flux event model");
        ImGui::BulletText("Sony 3.5\" zoned GCR encoder / decoder");
        ImGui::BulletText("RamWorks III aux-slot expander");
        ImGui::BulletText("Floppy mechanical sound samples + cadence");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Thanks to the MAME team for the meticulous reverse "
            "engineering work that makes POM2's parity possible. "
            "MAME is GPL-2.0 / BSD-3-Clause; POM2 is GPL-3.0.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("F11 = Reset (Ctrl-Reset)   F12 = Hard reset");
        ImGui::Text("ESC, arrows, Ctrl-A..Z map straight to the keyboard");

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ImGui::Spacing();
        if (ImGui::Button("Close")) showAbout = false;
    }
    ImGui::End();
}

void MainWindow::renderTapeFileDialogs()
{
    auto pathInput = [](const char* label) {
        // Minimal text-only path widget — POM2 doesn't pull in nativefiledialog.
        // The user types a path; convenience dirs/files can be appended later.
        ImGui::TextUnformatted(label);
    };

    if (cassetteDeck->loadDialogOpen) {
        ImGui::OpenPopup("Load Tape");
        cassetteDeck->loadDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Load Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Tape file path (.aci / .wav / .mp3 / .ogg / .flac)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##LoadPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        // Quick list of cassettes/ directory contents (one click → fill).
        // Through the shared cached listing: a modal re-renders every frame,
        // and this rescanned the folder each time with the THROWING
        // filesystem overloads — a directory that goes away mid-walk threw
        // out of the middle of an ImGui frame. See MainWindow::mediaDirListing.
        {
            const auto& listing = mediaDirListing(
                "cassettes",
                { "cassettes", "../cassettes", "../../cassettes" },
                { ".aci", ".wav", ".mp3", ".ogg", ".flac" },
                /*recursive=*/false);
            if (!listing.dir.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%s/", listing.dir.c_str());
                for (const auto& row : listing.entries) {
                    if (ImGui::Selectable(row.name.c_str()))
                        cassetteDeck->dialogPath = row.path;
                }
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (controller->loadTape(cassetteDeck->dialogPath)) {
                tapeStatusMessage = "Tape loaded: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Load failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (cassetteDeck->saveDialogOpen) {
        ImGui::OpenPopup("Save Tape");
        cassetteDeck->saveDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Save Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Output file path (.aci or .wav)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##SavePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (controller->saveTape(cassetteDeck->dialogPath)) {
                tapeStatusMessage = "Tape saved: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Save failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The transient tapeStatusMessage (disk load / boot / eject / screenshot
    // / paste …) is now surfaced in the bottom status bar (renderStatusBar),
    // right-aligned and auto-expiring via tapeStatusUntil — no separate
    // floating overlay.
}
