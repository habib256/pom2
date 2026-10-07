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

// MainWindow_Panels — the panel registry's one binding table, and the four
// views derived from it.
//
// Before this file, a panel existed in six places at once: 32 lines to load
// its visibility, 32 to save it, 38 to offer it in the command palette, 38
// more to dispatch that command, 37 menu rows, and a 28-assignment block that
// hid "every" panel on the browser build. Nothing checked any of them against
// the others, and they had already drifted — seven panels the palette could
// open had no settings key at all, so they never came back after a restart.
//
// Now there is `PanelCatalog.h` (what a panel is), `registerPanels()` below
// (which `bool` it lives in, and what makes it available), and four short
// functions that DERIVE the menus, the palette, the palette's dispatch and the
// settings round-trip from those. Adding a panel is a catalog row plus a bind
// line; forgetting the bind is caught at startup by `undrawn()`.
//
// It lives outside MainWindow.cpp for the reason the file-size ratchet exists:
// the god-object does not get to grow by 250 lines of table.

#include "AudioCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "MainWindow.h"
#include "PrinterCoordinator.h"

#include "EchoPlusCard.h"
#include "FujiNetCard.h"
#include "Logger.h"
#include "RomStatus_ImGui.h"
#include "SystemProfile.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "Debugger_ImGui.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "imgui.h"

#include <string>

namespace {

/// "(slot N)" for a plugged card, "(no card plugged)" for an empty bay. The
/// menus used to spell this out per panel, five times over, each slightly
/// differently.
template <typename Card>
std::string slotSuffix(Card* card)
{
    return card ? " (slot " + std::to_string(card->getSlot()) + ")"
                : std::string(" (no card plugged)");
}

}  // namespace

// ── The binding table ────────────────────────────────────────────────────

void MainWindow::registerPanels()
{
    using P  = pom2::PanelId;
    using RT = pom2::PanelRegistry::Runtime;

    // Most panels need nothing here at all: their title is fixed, they are
    // always available, and their draw call is attached below. Only the ones
    // sitting behind a card have anything to say, and what they say is the
    // same two things — "is it plugged" and "which slot does the label name".
    // The 3.5" drive belongs to a SmartPort card on any //e and to the
    // machine itself on a //c+ — the label has to say which, or the user
    // hunts for a card that is not there.
    panels_.bind(P::Disk35, RT{ [this] {
        return primarySmartPortCard()
            ? "Disk 3.5\" (slot " + std::to_string(primarySmartPortCard()->getSlot()) + ")"
            : std::string("Disk 3.5\" (//c+ on-board)");
    } });
    panels_.bind(P::Hdv, RT{ [this] {
        return "HDV (slot " + std::to_string(primaryHdvCard() ? primaryHdvCard()->getSlot() : 5) + ")";
    } });
    panels_.bind(P::SmartPort, RT{
        [this] { return "SmartPort Configuration" + slotSuffix(primarySmartPortCard()); },
        [this] { return primarySmartPortCard() != nullptr; } });
    panels_.bind(P::FujiNet, RT{
        [this] {
            const auto inv = devicePanelCoordinator_->captureInventory();
            return inv.fujiNetPlugged()
                       ? "FujiNet (slot " + std::to_string(inv.fujiNetSlot) + ")"
                       : std::string("FujiNet");
        },
        [this] {
            return devicePanelCoordinator_->captureInventory().fujiNetPlugged();
        } });
    panels_.bind(P::Phasor, RT{
        [this] {
            const auto slots = audioCoordinator_->captureInventory().phasorSlots;
            return slots.empty()
                ? std::string("Phasor")
                : "Phasor (slot " + std::to_string(slots.back()) + ")";
        },
        [this] {
            return !audioCoordinator_->captureInventory().phasorSlots.empty();
        } });
    panels_.bind(P::EchoPlus, RT{
        [this] {
            const auto slots = audioCoordinator_->captureInventory().echoPlusSlots;
            return slots.empty()
                ? std::string("Echo+")
                : "Echo+ (slot " + std::to_string(slots.back()) + ")";
        },
        [this] {
            return !audioCoordinator_->captureInventory().echoPlusSlots.empty();
        } });
    // Availability and label both come from the coordinator's snapshot: there
    // is no PrinterCard alias to test the address of any more.
    panels_.bind(P::Printer, RT{
        [this] {
            const int slot =
                printerCoordinator_->captureHost(*controller).printerCardSlot;
            return slot >= 0 ? "Printer (slot " + std::to_string(slot) + ")"
                             : std::string("Printer");
        },
        [this] {
            return printerCoordinator_->captureHost(*controller)
                       .printerCardSlot >= 0;
        } });

    // Super Serial is the one card a profile can carry TWICE (the //c's
    // printer and modem ports), so its label lists every slot it occupies.
    panels_.bind(P::Ssc, RT{
        [this] {
            // Hoisted: each call walks the bus.
            const auto ports = serialCards();
            if (ports.empty()) return std::string("Super Serial (no card plugged)");
            if (ports.size() == 1)
                return "Super Serial (slot " +
                       std::to_string(ports[0]->getSlot()) + ")";
            std::string lbl = "Super Serial (slots";
            for (size_t i = 0; i < ports.size(); ++i) {
                lbl += (i == 0) ? " " : ", ";
                lbl += std::to_string(ports[i]->getSlot());
            }
            return lbl + ")";
        },
        [this] { return !serialCards().empty(); } });

    // One entry covers both NICs; the panel tabs between whichever are in.
    panels_.bind(P::Ethernet, RT{
        [this] {
            const auto inv = devicePanelCoordinator_->captureInventory();
            const bool u1 = inv.uthernetSlot >= 0;
            const bool u2 = inv.uthernetIISlot >= 0;
            if (u1 && u2)
                return "Ethernet (I slot " +
                       std::to_string(inv.uthernetSlot) + ", II slot " +
                       std::to_string(inv.uthernetIISlot) + ")";
            if (u2)
                return "Ethernet (Uthernet II slot " +
                       std::to_string(inv.uthernetIISlot) + ")";
            if (u1)
                return "Ethernet (Uthernet I slot " +
                       std::to_string(inv.uthernetSlot) + ")";
            return std::string("Ethernet");
        },
        [this] {
            return devicePanelCoordinator_->captureInventory().ethernetPlugged();
        } });

#ifdef __EMSCRIPTEN__
    // AiControlServer::start() cannot open a listening socket in the browser
    // sandbox, so the panel is absent rather than greyed: a greyed row says
    // "plug something in", and there is nothing to plug in.
    panels_.hideFromUi(P::AiControl);
#endif

    registerPanelDraws();

    // A catalog row nobody draws is a menu entry that opens nothing — silent
    // in every other way, so it is worth a line in the log at startup.
    for (const std::string& id : panels_.undrawn())
        pom2::log().error("UI", "panel declared but nothing draws it: " + id);
}

// ── What each panel draws ────────────────────────────────────────────────
//
// The registry calls these only while the panel is visible, which is why the
// bodies below can be bare calls: the `if (!showXxx) return;` that used to
// open each render function is now the loop's business, in one place.

void MainWindow::registerPanelDraws()
{
    using P = pom2::PanelId;
    auto draw = [this](P id, std::function<void()> fn, bool always = false) {
        panels_.setDraw(id, std::move(fn), always);
    };

    draw(P::DiskLibrary,  [this] { renderDiskLibraryWindow(); });
    draw(P::SlotConfig,   [this] { renderSlotConfigPanel(); });
    draw(P::Media,        [this] { renderMediaPanel(); });
    draw(P::FloppyEmu,    [this] { renderFloppyEmuWindow(); });
    draw(P::Cassette,     [this] { renderCassetteDeckWindow(panelDelta_); });
    draw(P::DiskII,       [this] { renderDiskPanelWindow(); });
    draw(P::Disk35,       [this] { renderDisk35PanelWindow(); });
    draw(P::Hdv,          [this] { renderHdvPanelWindow(); });
    draw(P::SmartPort,    [this] { renderSmartPortPanelWindow(); });
    draw(P::FujiNet,      [this] { renderFujiNetPanelWindow(); });
    draw(P::Mockingboard, [this] { renderMockingboardPanelWindow(); });
    draw(P::Phasor,       [this] { renderPhasorPanelWindow(); });
    draw(P::EchoPlus,     [this] { renderEchoPlusPanelWindow(); });
    draw(P::Mixer,        [this] { renderAudioMixerWindow(); });
    draw(P::Ssc,          [this] { renderSscPanelWindow(); });
    draw(P::Ethernet,     [this] { renderEthernetPanelWindow(); });
    draw(P::Printer,      [this] { renderPrinterPanelWindow(); });
    draw(P::ImageWriter,  [this] { renderImageWriterWindow(); });
    draw(P::ChatMauve,    [this] { renderChatMauvePanelWindow(); });
    draw(P::Joystick,     [this] { renderJoystickPanelWindow(); });
    // The one panel drawn even while closed: it has to release the
    // Open-Apple / Solid-Apple latches on the frame it is shut, or the guest
    // sees a key held forever. It keeps its own guard and acts on that edge.
    draw(P::Keyboard, [this] { renderKeyboardPanel(); }, /*always=*/true);
    draw(P::Rewind,       [this] { renderRewindWindow(panelDelta_); });
    draw(P::Mouse,        [this] { renderMouseInspectorWindow(); });
    draw(P::NoSlotClock,  [this] { renderNoSlotClockPanelWindow(); });
    draw(P::Crt,          [this] { renderNtscSettingsWindow(); });
    draw(P::Voxel,        [] { /* drawn inside renderScreenWindow */ });
    draw(P::VoxelSettings,[this] { renderVoxelSettingsWindow(); });
    draw(P::MemViewer,    [this] { renderMemoryViewerWindow(); });
    draw(P::Debugger,     [this] { debuggerPanel->render(*controller,
                                                         &show(P::Debugger)); });
    draw(P::MemBar,       [this] { renderMemoryBarWindow(); });
    draw(P::MemBarH,      [this] { renderMemoryBarHorizontalWindow(); });
    draw(P::MemGrid,      [this] { renderMemoryGridWindow(); });
    draw(P::HgrPaint,     [this] { renderHgrPaintWindow(); });
    draw(P::HgrSprite,    [this] { renderHgrSpriteWindow(); });
    draw(P::AiControl,    [this] { renderAiControlPanelWindow(); });
    draw(P::Welcome,      [this] { renderWelcomePanelWindow(); });
    // ROM Status builds its panel object on first use — it holds a scan of
    // every ROM path, which a session that never opens it should not pay for.
    draw(P::RomStatus, [this] {
        if (!romStatusPanel)
            romStatusPanel = std::make_unique<pom2::RomStatus_ImGui>();
        romStatusPanel->render(
            &show(P::RomStatus),
            std::string(pom2::profileConfig(activeProfile).displayName));
    });
    draw(P::Abstraction,  [this] { renderAbstractionPanel(); });
}

// ── View 1: menu rows ────────────────────────────────────────────────────

void MainWindow::panelMenuItem(pom2::PanelId id)
{
    if (panels_.hidden(id)) return;
    const pom2::PanelInfo& info = pom2::panelInfo(id);
    const bool enabled = panels_.available(id);
    const std::string label = panels_.title(id);

    if (!enabled) ImGui::BeginDisabled();
    ImGui::MenuItem(label.c_str(), info.shortcut, &panels_.visible(id));
    if (!enabled) ImGui::EndDisabled();
    // AllowWhenDisabled so an unplugged card still explains what it would do
    // — the tooltip is how the user learns the panel exists before owning it.
    if (info.tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", info.tip);
}

void MainWindow::panelMenuGroup(pom2::PanelGroup group)
{
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog)
        if (p.group == group) panelMenuItem(p.id);
}

// ── The command palette and its dispatch ─────────────────────────────────

void MainWindow::forEachPanelCommand(
    const std::function<void(const char*, const std::string&, const char*, bool,
                             bool)>& add) const
{
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        if (panels_.hidden(p.id)) continue;
        add(p.command, panels_.title(p.id), p.shortcut ? p.shortcut : "",
            panels_.available(p.id), panels_.visible(p.id));
    }
}

bool MainWindow::runPanelCommand(const std::string& id)
{
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        if (id != p.command) continue;
        bool& v = panels_.visible(p.id);
        v = !v;
        return true;
    }
    return false;
}

// ── The settings round-trip ──────────────────────────────────────────────

void MainWindow::loadPanelVisibility()
{
    if (!settings) return;
    panels_.forEachPersisted([this](const char* key, bool& flag) {
        flag = settings->getBool(key, flag);
    });
}

void MainWindow::savePanelVisibility()
{
    if (!settings) return;
    panels_.forEachPersisted([this](const char* key, bool& flag) {
        settings->setBool(key, flag);
    });
}

void MainWindow::hideAllPanels()
{
    panels_.hideAll();
}

// ── The render loop ──────────────────────────────────────────────────────

void MainWindow::renderPanels(float deltaSeconds)
{
    // Two panels want the frame delta and the registry's draw signature does
    // not carry one — a member is cheaper than threading a parameter through
    // 38 closures that ignore it.
    panelDelta_ = deltaSeconds;
    panels_.drawAll();
}
