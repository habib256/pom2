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

// MainWindow_Slots — Slot Configuration panel.
//
// Renders an ImGui dialog under Hardware → Slot Configuration that lets
// the user assign one of {Disk II, ProDOS HDV, Super Serial, Clock,
// Le Chat Mauve, Mouse} to each of the 7 expansion slots, or leave a
// slot empty. The selection is persisted to settings as `slot_N_card`
// keys; clicking Apply triggers a controlled restart of the emulation
// thread, which:
//
//   1. Stops the worker (controller->stop()).
//   2. Tears down the SlotBus via `clear()` (each card's onUnplug runs).
//   3. Re-runs `plugSlotsFromSettings()` so the new mapping takes effect.
//   4. Hard-resets the CPU (so PC lands on the new ROM's reset vector).
//   5. Re-starts the worker.
//
// Validation: each card type can only be assigned to one slot at a time.
// Duplicate selections are highlighted in red and Apply stays disabled.
// Mouse Card additionally requires both Apple ROMs to be present —
// otherwise the entry is greyed out in the dropdown.

#include "MainWindow.h"
#include "ProfileSwitch.h"

#include <algorithm>   // std::find / std::max over the connector rows
#include "SlotConfigurationCoordinator.h"
#include "SlotRebuildCoordinator.h"
#include "StorageCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "PrinterCoordinator.h"
#include "MediaMount.h"

// Same heavy-includes-here pattern as MainWindow.cpp — MainWindow.h
// forward-declares the controller / cards / panels.
#include "AiControlServer.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CffaCard.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EmulationController.h"
#include "AbstractionLevels_ImGui.h"
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "PhasorCard.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotCardCatalog.h"
#include "StatusLed.h"
#include "IconsFontAwesome6.h"
#include "Pom2Theme.h"   // palette() for the staged-change accent
#include "MountableMediaCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "FujiNetCard.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <array>
#include <exception>
#include <filesystem>
#include <vector>

// Card catalog + ROM-presence probes now live in SlotCardCatalog.h so the
// Slot Manager panel shares them. Bring the names into this TU unqualified
// to keep the existing panel body unchanged.
using pom2::kCardTypes;
using pom2::mouseRomsPresent;
using pom2::mouseAwRomPresent;
using pom2::cffaRomPresent;

void MainWindow::renderSlotConfigPanel()
{
    if (!show(pom2::PanelId::SlotConfig)) return;

    // 880 px was sized for two columns; one column needs about half that.
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Slot Configuration", &show(pom2::PanelId::SlotConfig))) {
        ImGui::End();
        return;
    }

    // This window is ONE interaction model: staged. It used to carry the media
    // column too, and the two models sat side by side with nothing but a
    // banner to tell them apart — Apply / Revert at the bottom of the left
    // child read as governing the whole window, so mounting a disk on the
    // right and hitting Revert on the left looked like it should undo the
    // mount. The media half now lives in its own window (Devices → Internal
    // Disks & Media), which is what makes Apply / Revert unambiguous.
    ImGui::TextWrapped(
        "Assign a card to each expansion slot. Changes are staged until you "
        "Apply — that restarts the emulator. Mounting media is a separate "
        "window: Devices \xe2\x86\x92 Internal Disks & Media.");
    ImGui::Spacing();

    const auto& profileCfg = pom2::profileConfig(activeProfile);

    ImGui::BeginChild("##slotassign", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    {
        // The window is built from THIS machine's connector inventory
        // (SlotConnectors.h), not from a fixed list of seven slots. Rendering
        // "Slot 1".."Slot 7" on a //c named seven controls that do not exist
        // on the hardware: five were greyed built-ins, one offered a rear
        // adapter under a slot number, and on a //c PAL the last one was dead
        // in every direction. A row exists here only if the connector exists.
        if (!connectorLayoutValid_ || connectorLayoutProfile_ != activeProfile) {
            connectorLayout_        = pom2::buildConnectorLayout(profileCfg);
            connectorLayoutProfile_ = activeProfile;
            connectorLayoutValid_   = true;
        }

        // Label column, measured off the rows that actually HAVE a control.
        // Measuring every label instead let a row carrying no control at all
        // — "Game port (DB-9 + 16-pin)" — set the indent for "Slot 1", so on
        // a docked panel half of every slot row was empty gutter and the
        // combos were clipped to "Mockingboard C (So". Text-only rows are
        // laid out below without this column, so they no longer pay for it
        // or charge anyone else for it.
        float slotGutter = ImGui::CalcTextSize("AUX memory").x;
        for (const auto& section : connectorLayout_)
            for (const auto& row : section.rows) {
                const bool carriesControl =
                    row.slot >= 0 || row.kind == pom2::ConnectorKind::AuxMemory;
                if (!carriesControl) continue;
                slotGutter = std::max(slotGutter,
                                      ImGui::CalcTextSize(row.label.c_str()).x);
            }
        slotGutter += ImGui::GetStyle().ItemSpacing.x * 2.0f;
        // …and capped: a //c's "Serial port 1 (printer, DIN-5)" is long
        // enough to leave a narrow dock with no room for the picker at all.
        // Past 45 % of the width the label wraps under its own control
        // rather than squeezing it.
        slotGutter = std::min(slotGutter,
                              ImGui::GetContentRegionAvail().x * 0.45f);
        auto slotLabel = [slotGutter](const char* text) {
            ImGui::TextUnformatted(text);
            ImGui::SameLine(slotGutter);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };

        // The staged editor value lives in the coordinator, not in a
        // function-local `static`: it is one of the three slot maps, and the
        // one that must NOT be confused with either the effective plan or the
        // live bus. Re-seeded from the plan whenever a profile switch or a
        // settings restart rebuilt it — slotDraftInited_ is reset by both for
        // exactly that purpose, which a `static bool` here could never observe.
        auto& draft = slotConfigCoordinator_->draft();
        if (!slotDraftInited_) {
            slotConfigCoordinator_->resetDraft();
            // EMPTY means "no staged variant change", not "Féline". It used
            // to be seeded with the setting's value once and then compared
            // against the live setting every frame — but the Le Chat Mauve
            // device panel writes `chatmauve_variant` LIVE, so any change made
            // there turned into a phantom "1 staged change" here, and Apply
            // wrote the stale draft back over it. The sentinel makes "the
            // user picked a model in THIS panel" the only thing that stages.
            chatMauveVariantDraft_.clear();
            // Same sentinel discipline for the aux-memory row, and the same
            // reason it must be reset HERE: since the RamWorks size became a
            // STAGED choice rather than an immediate one, a draft left over
            // from the previous machine kept counting in `pendingChangeCount`
            // — which has no iieMode gate — on profiles whose connector
            // layout contains no AuxMemory row at all (a ][+, and every
            // //c-class layout). The panel then badged "1 staged change"
            // pointing at no row, with Apply armed: pressing it wrote
            // `ramworks_banks` for a machine that cannot show it and
            // cold-booted, wiping RAM.
            ramWorksDraft_ = -1;
            slotDraftInited_ = true;
        }
        const std::string cmVariantLive =
            settings->getString("chatmauve_variant", "feline");
        const std::string cmVariantEff =
            chatMauveVariantDraft_.empty() ? cmVariantLive
                                           : chatMauveVariantDraft_;

        const bool mouseAvailable    = mouseRomsPresent();
        const bool mouseAwAvailable  = mouseAwRomPresent();
        const bool cffaAvailable     = cffaRomPresent();

        // Multi-instance cards are never flagged as duplicates. The list is
        // SlotConfigurationCoordinator::isMultiInstance — the same predicate
        // resolve() applies when it builds the effective plan. Built-in slots
        // forced by the profile are exempt too: //c ships TWO SSC-compatible
        // serial ports (printer + modem), both forced, and the picker must
        // not light them up red.
        auto isDuplicate = [&](int slot) -> bool {
            if (slot < 1 || slot > 7)                   return false;
            if (draft[slot].empty())                    return false;
            if (pom2::SlotConfigurationCoordinator::isMultiInstance(draft[slot]))
                return false;
            if (profileCfg.builtInSlots[slot].has_value()) return false;
            for (int s = 1; s <= 7; ++s) {
                if (s == slot) continue;
                if (profileCfg.builtInSlots[s].has_value()) continue;
                if (draft[s] == draft[slot])            return true;
            }
            return false;
        };

        // Why a card cannot be chosen on THIS machine — empty means it can.
        // One place, so the greyed entries in a picker and the "Not available
        // on this machine" group at the bottom can never disagree.
        auto unavailableReason = [&](const std::string& key) -> const char* {
            if (key.empty()) return nullptr;
            if (key == "mouse"   && !mouseAvailable)
                return "ROM dumps missing (roms/mouse_341-0270-c.bin + "
                       "roms/mouse_341-0269.bin)";
            if (key == "mouseaw" && !mouseAwAvailable)
                return "ROM dump missing (the mouse slot EPROM)";
            if (key == "cffa"    && !cffaAvailable)
                return "ROM dump missing (roms/cffa20ee02.bin)";
            if (profileCfg.noPhysicalSlots &&
                key != "chatmauve" && key != "mockingboard" &&
                key != "mockingboard_c")
                return "no expansion bus on this machine — its forced "
                       "INTCXROM masks slot ROM entirely";
            return nullptr;
        };

        // Each card is tagged with its emulation level from the abstraction
        // catalog (the LLE/HLE panel's source of truth — docs/lle_vs_hle.md
        // made live), so the picker says whether you are choosing silicon or
        // a service, and with its scope bucket from TODO.md's ruling.
        auto absEntryFor = [](const std::string& key) -> const pom2::AbsEntry* {
            if (key.empty()) return nullptr;
            const char* id = key.c_str();
            if (key == "smartport35")    id = "smartportcard";
            else if (key == "printer")   id = "printercard";
            else if (key == "mockingboard_c") id = "mockingboard";
            else if (key == "phasor")    id = "mockingboard";
            else if (key == "echoplus")  id = "ssi263";
            else if (key == "echoplus_tms") id = "tms5220";
            for (const auto& e : pom2::abstractionCatalog())
                if (std::string(id) == e.id) return &e;
            return nullptr;
        };
        // Silicon or service, and nothing else. The tag used to read
        // "[L0 · LLE · core]": the L0-L2 sub-levels belong to the Abstraction
        // Levels panel, which is about them, and the scope word to TODO.md's
        // ruling, which is where it is maintained. On every row of a docked
        // picker the pair was clutter that clipped the card's own name.
        auto levelTag = [&](const std::string& key) -> std::string {
            if (key.empty()) return {};
            const auto* e = absEntryFor(key);
            if (!e) return {};
            return std::string("  [") +
                   (pom2::levelIsLle(e->level) ? "LLE" : "HLE") + "]";
        };
        auto cardLabel = [](const std::string& key) -> const char* {
            for (const auto& ct : kCardTypes)
                if (ct.key == key) return ct.label;
            return pom2::cardLabelForKey(key);
        };

        bool anyDuplicate = false;

        // A connector that carries a card the user may choose. `accepts`
        // empty = the whole catalog (a real expansion slot); otherwise only
        // the listed keys, because a DB-15 video connector physically cannot
        // take a Disk II controller.
        auto renderCardPicker = [&](const pom2::Connector& row) {
            const int s = row.slot;
            // A key this connector cannot carry is SHOWN as out-of-place, not
            // rewritten. Rewriting `draft[s]` from the render path invented a
            // staged change the user never made: `Revert` restored the plan's
            // value and the very next frame clobbered it again, so the panel
            // claimed "1 staged change — not applied yet" forever, Apply stayed
            // armed, and each press cold-booted the machine (wiping RAM) while
            // persisting nothing — or, for `chatmauve`, silently wrote "" and
            // deleted the user's adapter.
            //
            // The disagreement is legitimate and permanent, not a stale
            // state.cfg: on a //c `SlotConfigurationCoordinator::resolve()`
            // keeps `chatmauve` in WHATEVER free slot the user parked it,
            // while this table pins the DB-15 to row 7 (the 4c is read on
            // row 3 only since it got its own key).
            const bool outOfPlace =
                !row.accepts.empty() &&
                std::find(row.accepts.begin(), row.accepts.end(),
                          draft[s]) == row.accepts.end();
            const bool dup = isDuplicate(s);
            if (dup) anyDuplicate = true;

            const std::string preview =
                draft[s].empty()
                    ? std::string("(empty)")
                    : std::string(cardLabel(draft[s])) + levelTag(draft[s]) +
                          (outOfPlace ? std::string("  (on another connector)")
                                      : std::string());

            // A staged row is marked where the user is looking.
            const bool staged = (draft[s] != slotCards[s]);
            if (staged) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent));
                ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Staged. Currently plugged: %s",
                                      slotCards[s].empty()
                                          ? "(empty)"
                                          : cardLabel(slotCards[s]));
                ImGui::SameLine(0.0f, 0.0f);
            }
            slotLabel(row.label.c_str());
            char comboId[24];
            std::snprintf(comboId, sizeof(comboId), "##slotcombo%d", s);
            if (dup) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
            if (ImGui::BeginCombo(comboId, preview.c_str())) {
                int shown = 0;
                for (const auto& ct : kCardTypes) {
                    if (!row.accepts.empty() &&
                        std::find(row.accepts.begin(), row.accepts.end(),
                                  ct.key) == row.accepts.end())
                        continue;
                    if (unavailableReason(ct.key)) continue;   // grouped below
                    ++shown;
                    const bool selected = (ct.key == draft[s]);
                    const std::string itemLabel = ct.label + levelTag(ct.key);
                    if (ImGui::Selectable(itemLabel.c_str(), selected))
                        draft[s] = ct.key;
                    if (const auto* ae = absEntryFor(ct.key);
                        ae && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s",
                                          pom2::levelName(ae->level),
                                          ae->modelled);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                // …and everything this machine cannot take, with the reason,
                // below a separator. Listed rather than hidden: a card that
                // silently vanishes reads as a gap in POM2, not as a fact
                // about the machine in front of you.
                bool headerDrawn = false;
                for (const auto& ct : kCardTypes) {
                    if (!row.accepts.empty() &&
                        std::find(row.accepts.begin(), row.accepts.end(),
                                  ct.key) == row.accepts.end())
                        continue;
                    const char* why = unavailableReason(ct.key);
                    if (!why) continue;
                    if (!headerDrawn) {
                        if (shown > 0) ImGui::Separator();
                        ImGui::TextDisabled("Not available on this machine");
                        headerDrawn = true;
                    }
                    ImGui::BeginDisabled();
                    ImGui::Selectable(ct.label, false);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", why);
                }
                ImGui::EndCombo();
            }
            if (dup) ImGui::PopStyleColor();
            if (dup) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "(one only)");
            }
        };

        for (const auto& section : connectorLayout_) {
            ImGui::SeparatorText(section.title.c_str());
            if (!section.blurb.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", section.blurb.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            for (const auto& row : section.rows) {
                const int s = row.slot;
                const bool builtIn =
                    (s >= 1 && s <= 7) &&
                    profileCfg.builtInSlots[static_cast<std::size_t>(s)].has_value();

                // ── A connector whose card is soldered on ────────────────
                // Read-only whatever its kind: the //c's serial ports are
                // ports you plug a cable into, but the 6551 behind them is
                // not a card you can swap.
                if (builtIn) {
                    const auto& bis =
                        *profileCfg.builtInSlots[static_cast<std::size_t>(s)];
                    draft[s] = bis.cardKey;
                    char preview[128];
                    std::snprintf(preview, sizeof(preview), "%s — %s",
                                  cardLabel(bis.cardKey),
                                  row.note.empty() ? bis.label.c_str()
                                                   : row.note.c_str());
                    ImGui::BeginDisabled(true);
                    slotLabel(row.label.c_str());
                    ImGui::TextUnformatted(preview);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Built into %s — cannot be changed.",
                                          std::string(profileCfg.displayName).c_str());
                    continue;
                }

                switch (row.kind) {
                case pom2::ConnectorKind::AuxSlot:
                    // One wrapped line: no control, so no column. Forcing it
                    // into the label gutter clipped "Extended 80-Column Card
                    // (built-in, $C300 firmware)" at the panel edge.
                    ImGui::BeginDisabled(true);
                    ImGui::TextWrapped("%s — %s", row.label.c_str(),
                                       row.note.c_str());
                    ImGui::EndDisabled();
                    continue;

                case pom2::ConnectorKind::AuxMemory: {
                    // RamWorks III aux size. //c-class never reaches here:
                    // those profiles force 1 bank back on (applyProfile step
                    // 4), so a picker there would be a dead control.
                    struct RwTier { int banks; const char* label; };
                    static constexpr RwTier kRwTiers[] = {
                        {   1, "64 KB (stock //e — no RamWorks)" },
                        {   4, "256 KB RamWorks" },
                        {   8, "512 KB RamWorks" },
                        {  16, "1 MB RamWorks" },
                        {  48, "3 MB RamWorks" },
                        { 128, "8 MB RamWorks III" },
                    };
                    // STAGED, like every other row in this window
                    // (2026-09-12). It used to write the setting and cold-boot
                    // the machine the instant you picked a size: the one
                    // immediate control in a staged window, directly above an
                    // Apply button that did not count it — so the window both
                    // claimed nothing had happened and had already wiped RAM.
                    const int liveBanks = settings->getInt("ramworks_banks", 1);
                    const int curBanks  =
                        (ramWorksDraft_ >= 0) ? ramWorksDraft_ : liveBanks;
                    const char* curLabel = kRwTiers[0].label;
                    for (const auto& t : kRwTiers)
                        if (t.banks == curBanks) curLabel = t.label;
                    if (curBanks != liveBanks) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(pom2::palette().accent));
                        ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            const char* wasLabel = kRwTiers[0].label;
                            for (const auto& t : kRwTiers)
                                if (t.banks == liveBanks) wasLabel = t.label;
                            ImGui::SetTooltip("Staged. Currently: %s", wasLabel);
                        }
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    slotLabel(row.label.c_str());
                    ImGui::SetNextItemWidth(320.0f);
                    if (ImGui::BeginCombo("##ramworks", curLabel)) {
                        for (const auto& t : kRwTiers)
                            if (ImGui::Selectable(t.label, t.banks == curBanks))
                                ramWorksDraft_ = t.banks;
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "RamWorks III bank-switched aux RAM ($C071/3/5/7).\n"
                            "Staged: it lands on Apply, which COLD-BOOTS the\n"
                            "machine.\n"
                            "A rewind snapshot only loads back into a machine\n"
                            "with the same aux size.");
                    continue;
                }

                default: break;
                }

                // A port that carries no card at all (game port, cassette):
                // say what it is and where its device lives.
                if (s < 0) {
                    // Same reason as the AUX slot above: label and note on one
                    // wrapped line. "Game port (DB-9 + 16-pin)" plus
                    // "Paddles / joystick — Devices -> Joystick" does not fit
                    // a two-column row in a docked panel, and the note was the
                    // half that fell off the right edge.
                    ImGui::BeginDisabled(true);
                    ImGui::TextWrapped("%s — %s", row.label.c_str(),
                                       row.note.c_str());
                    ImGui::EndDisabled();
                    continue;
                }

                renderCardPicker(row);

                // Which Chat Mauve: the family is ONE catalog key, the model
                // is the `chatmauve_variant` card setting. Not offered on a
                // //c-class machine — there the DB-15 connector fixes it.
                if (draft[s] == "chatmauve") {
                    if (profileCfg.noPhysicalSlots) {
                        slotLabel("");
                        ImGui::TextDisabled(
                            "model: Adaptateur IIc — fixed by the DB-15 connector");
                    } else {
                        using CmVariant = LeChatMauveCard::Variant;
                        CmVariant cur;
                        if (!LeChatMauveCard::parseVariant(cmVariantEff, cur))
                            cur = CmVariant::Feline;
                        slotLabel("  model");
                        char cmId[24];
                        std::snprintf(cmId, sizeof(cmId), "##cmvariant%d", s);
                        if (ImGui::BeginCombo(cmId,
                                              LeChatMauveCard::variantLabel(cur))) {
                            for (int vi = 0; vi < LeChatMauveCard::kVariantCount; ++vi) {
                                const auto v = static_cast<CmVariant>(vi);
                                if (ImGui::Selectable(LeChatMauveCard::variantLabel(v),
                                                      v == cur))
                                    chatMauveVariantDraft_ =
                                        LeChatMauveCard::variantKey(v);
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "Which registers exist and which modes fall back.\n"
                                "Feline / Adaptateur //c: mixed DHGR (Extasie, Arlequin).\n"
                                "Eve: $C0B0-$C0BF, TXT16/CP280/COL280 — NO mixed mode.\n"
                                "Video-7: 160-wide chunky, F/B text.\n"
                                "RVB Graph (II/II+, partial): $C0F0-$C0F3 only.");
                    }
                }

                // Apple sold the mouse for slot 4, and French mouse software
                // takes that literally: Extasie calls the slot-4 firmware
                // entries by self-modified `JSR $C4xx` with no slot scan.
                // Scanning software finds it anywhere — warn, don't forbid.
                if (s != 4 && (draft[s] == "mouse" || draft[s] == "mouseaw")) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                                       "(Extasie & friends want the mouse in slot 4)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Apple's mouse slot is 4. Software that scans the\n"
                            "slots (A2DeskTop, MousePaint) will find it here,\n"
                            "but French titles like Extasie call the slot-4\n"
                            "firmware directly and will not see this card.");
                }
                if (s == 3 && profileCfg.iieMode &&
                    (draft[s] == "grappler" || draft[s] == "printer")) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                                       "(80-col firmware owns slot 3 — use 1)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "On a //e the internal 80-column firmware uses the "
                            "slot-3 screen holes ($0478+3, $057B, $05FB…) for "
                            "its own cursor state.\nA printer card in that slot "
                            "shares them and prints one character per line.\n"
                            "Move it to slot 1 (or 2/4/5/7) — same as on real "
                            "hardware.");
                }
                // Slot 3 on a //e is not merely awkward, it is DEAD for almost
                // every card: with SLOTC3ROM off (the reset default) the
                // motherboard owns $C300-$C3FF outright and slot 3's I/O
                // SELECT never asserts.
                if (s == 3 && profileCfg.iieMode && !draft[s].empty() &&
                    draft[s] != "grappler" && draft[s] != "printer") {
                    const bool isMouse =
                        (draft[s] == "mouse" || draft[s] == "mouseaw");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                       isMouse
                                           ? "(invisible in slot 3 — use 4)"
                                           : "(slot 3 $C300 window is dead)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            isMouse
                            ? "On a //e the internal 80-column firmware owns "
                              "$C300-$C3FF, so a card there has NO $Cs00 page the "
                              "guest can reach.\nSoftware finds the mouse by "
                              "scanning slots for the Apple signature ($Cn05=$38, "
                              "$Cn07=$18, $Cn0B=$01, $Cn0C=$20) — at $C300 it "
                              "reads the 80-column firmware instead and decides "
                              "there is no mouse.\nA2DeskTop, MousePaint and "
                              "MultiScribe then run keyboard-only.\nMove it to "
                              "slot 4 (Apple's own slot for it), or 5/7 — same as "
                              "on real hardware."
                            : "On a //e the internal 80-column firmware owns "
                              "$C300-$C3FF, so slot 3's I/O SELECT never asserts "
                              "and NOTHING in the card's $C300 page is "
                              "reachable.\nThat kills any card that needs it — "
                              "firmware the guest scans for, and registers too: a "
                              "Mockingboard addresses its VIAs through that "
                              "window, so it goes silent there.\nA card that "
                              "only uses its $C0nX soft switches still works.\n"
                              "On real hardware slot 3 belongs to the 80-column "
                              "card.");
                }
            }
            ImGui::Spacing();
        }
        ImGui::Spacing();
        ImGui::Separator();

        if (mouseAvailable) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                               "Mouse ROMs found.");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                               "Mouse ROMs missing — Mouse Interface disabled. "
                               "Add roms/mouse_341-0270-c.bin + "
                               "roms/mouse_341-0269.bin.");
        }
        if (!mouseRomStatus.empty())
            ImGui::TextWrapped("Mouse: %s", mouseRomStatus.c_str());

        ImGui::Spacing();
        if (anyDuplicate) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "One card type per slot — fix duplicates.");
        }

        // How many user-editable slots differ from what is actually plugged.
        // Built-in slots are force-fed into the draft by the rows above, so
        // they can never count as pending.
        // Built-ins never count, the two sentinels mean "nothing staged", and
        // a //c never stages the Chat Mauve model — all of it in one testable
        // place (`SlotConnectors.h`), pinned by `slot_connectors`.
        const int pending = pom2::pendingChangeCount(
            profileCfg, draft, slotCards, chatMauveVariantDraft_, cmVariantLive,
            ramWorksDraft_, settings->getInt("ramworks_banks", 1));

        if (pending > 0) {
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(pom2::palette().accent),
                ICON_FA_CIRCLE_DOT " %d staged change%s — not applied yet",
                pending, pending == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("No staged changes.");
        }

        // Apply is disabled with nothing staged: a button that restarts the
        // emulator should never be a no-op the user can hit by reflex.
        ImGui::BeginDisabled(anyDuplicate || pending == 0);
        char applyLabel[64];
        std::snprintf(applyLabel, sizeof(applyLabel),
                      pending > 0 ? "Apply %d change%s (cold-boots the machine)"
                                  : "Apply (cold-boots the machine)",
                      pending, pending == 1 ? "" : "s");
        if (ImGui::Button(applyLabel)) {
            // Persist ONLY user-editable slots. The panel force-feeds the
            // draft with the profile's built-in cards and force-empties the
            // non-existent connectors on a noPhysicalSlots machine (see the
            // rows above), so persisting all seven here clobbered the
            // user's saved //e-era slot_N_card keys whenever Apply was
            // clicked on a //c-class profile. Same guard as the
            // ~MainWindow shutdown persist path.
            std::array<std::string, 8> previous{};
            std::array<bool, 8> changed{};
            for (int s = 1; s <= 7; ++s) {
                const std::string key = pom2::slotCardSettingKey(profileCfg, s);
                if (!pom2::slotKeyIsUserChoice(profileCfg, s, draft[s],
                                               settings->getString(key, "")))
                    continue;
                previous[s] = settings->getString(key, "");
                changed[s] = true;
                settings->setString(key, draft[s]);
            }
            int  prevRamWorks    = 0;
            bool ramWorksChanged = false;
            if (ramWorksDraft_ >= 0 &&
                ramWorksDraft_ != settings->getInt("ramworks_banks", 1)) {
                prevRamWorks = settings->getInt("ramworks_banks", 1);
                settings->setInt("ramworks_banks", ramWorksDraft_);
                ramWorksChanged = true;
            }
            std::string prevCmVariant;
            bool cmVariantChanged = false;
            if (!profileCfg.noPhysicalSlots &&
                !chatMauveVariantDraft_.empty() &&
                chatMauveVariantDraft_ != cmVariantLive) {
                prevCmVariant = settings->getString("chatmauve_variant", "");
                settings->setString("chatmauve_variant", chatMauveVariantDraft_);
                cmVariantChanged = true;
            }
            if (!settings->save()) {
                for (int s = 1; s <= 7; ++s) {
                    if (changed[s]) settings->setString(
                        pom2::slotCardSettingKey(profileCfg, s), previous[s]);
                }
                if (cmVariantChanged)
                    settings->setString("chatmauve_variant", prevCmVariant);
                if (ramWorksChanged)
                    settings->setInt("ramworks_banks", prevRamWorks);
                tapeStatusMessage = "Slot changes not applied — settings could not be saved.";
                tapeStatusUntil = lastFrameTime + 8.0;
                pom2::log().warn("Slots", tapeStatusMessage);
            } else if (!restartEmulationFromSettings()) {
                // The live machine was deliberately left intact. Restore the
                // persisted mapping too, otherwise the refused draft would be
                // applied silently on the next launch.
                for (int s = 1; s <= 7; ++s) {
                    if (changed[s]) settings->setString(
                        pom2::slotCardSettingKey(profileCfg, s), previous[s]);
                }
                if (cmVariantChanged)
                    settings->setString("chatmauve_variant", prevCmVariant);
                if (ramWorksChanged)
                    settings->setInt("ramworks_banks", prevRamWorks);
                if (!settings->save())
                    pom2::log().error("Slots",
                        "Could not persist the previous slot mapping after a refused rebuild.");
            } else {
                // restartEmulationFromSettings also captured live media paths
                // after the first save; make those refreshed values durable.
                if (!settings->save()) {
                    tapeStatusMessage =
                        "Slots applied, but updated settings could not be saved.";
                    tapeStatusUntil = lastFrameTime + 8.0;
                    pom2::log().warn("Slots", tapeStatusMessage);
                }
                slotConfigCoordinator_->resetDraft();
                chatMauveVariantDraft_.clear();   // staged change consumed
                ramWorksDraft_ = -1;
            }
        }
        ImGui::EndDisabled();
        if (pending > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Cold-boots the emulated machine with the new cards —\n"
                "RAM is wiped, exactly as if you had powered it off,\n"
                "swapped the cards and powered it back on. Anything\n"
                "running or loaded in memory is gone.\n"
                "Mounted media is preserved where the card still exists.\n"
                "Only affects the slot list above — anything mounted from\n"
                "Internal Disks & Media has already taken effect.");
        ImGui::SameLine();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::Button("Revert")) {
            slotConfigCoordinator_->resetDraft();
            chatMauveVariantDraft_.clear();
            ramWorksDraft_ = -1;
        }
        ImGui::EndDisabled();
        if (pending > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard the %d staged slot change%s.\n"
                              "Does not touch mounted media.",
                              pending, pending == 1 ? "" : "s");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ─── Internal Disks & Media ─────────────────────────────────────────────
// Split out of Slot Configuration on 2026-07-28. Everything here is
// IMMEDIATE — Mount / Insert / Eject act on the running machine — which is
// the opposite of the staged model next door, and the reason the two no
// longer share a window.
void MainWindow::renderMediaPanel()
{
    if (!show(pom2::PanelId::Media)) return;

    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Internal Disks & Media", &show(pom2::PanelId::Media))) {
        ImGui::End();
        return;
    }

    const auto& profileCfg = pom2::profileConfig(activeProfile);

    ImGui::BeginChild("##slotmedia", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        ImGui::SeparatorText("Internal disks & mountable ports");
        ImGui::TextDisabled("Mount / Insert / Eject take effect immediately.");
        ImGui::TextDisabled("Which card sits in which slot: Machine \xe2\x86\x92 "
                            "Slot Configuration.");
        ImGui::Spacing();

        // Shared media status LED (grey/green/yellow/red). Kept as a local
        // alias so the existing per-row call sites read unchanged.
        auto dot = [](bool loaded, bool wp) { pom2::statusLed(loaded, wp); };

        // Persistent InputText buffers, keyed [slot][bay/drive]. Primed once
        // from the live path; re-primed (to the new live value) after eject.
        // Fourteen bays: a Liron's chain carries up to fourteen units, a
        // SmartPort card eight.
        constexpr int kPanelBays = 14;
        static std::array<std::array<std::array<char, 512>, kPanelBays>, 8> mBuf{};
        static std::array<std::array<bool, kPanelBays>, 8> mPrimed{};
        static std::array<std::array<std::array<char, 512>, 2>, 8> dBuf{};
        static std::array<std::array<bool, 2>, 8> dPrimed{};
        // Re-prime everything after a slot rebuild: these statics survive
        // applyProfile / restartEmulationFromSettings, and a stale primed
        // path shown against a rebuilt card (SmartPort → CFFA, or a profile
        // switch) is one enabled Mount button away from inserting the OLD
        // card's image into the NEW card.
        static uint32_t seedGen = 0;
        if (seedGen != mediaPanelSeedGen_) {
            seedGen = mediaPanelSeedGen_;
            for (auto& row : mPrimed) row.fill(false);
            for (auto& row : dPrimed) row.fill(false);
        }

        bool any = false;
        // Unlocked on purpose, and one of the few places `memory()` is the
        // right accessor. What is read here is the bus *topology* (which slot
        // holds which card), and that is UI-thread-confined: every writer —
        // plugSlotsFromSettings, applyProfile, the slot-config rebuild — runs
        // on this thread. The worker only ever reads it, from memRead
        // dispatch. Taking `lockState()` for the reference would protect
        // nothing (it is released before the loop below uses `bus`) while
        // reading as though it did. Per-card *state* is a different matter,
        // and each bay snapshot below does take the lock.
        SlotBus& bus = controller->memory().slotBus();

        // Label rows from the LIVE bus, not from the plan. They are not the
        // same thing: a card auto-provisioned for a boot is plugged without
        // being configured, and a configured card whose ROM is missing is
        // configured without being plugged. Reading the plan here meant the
        // first showed a blank label and the second showed a card that was
        // not there.
        //
        // Unlocked on purpose, and consistent with the `bus` reference above:
        // captureLive reads only topology (which slot holds which card, and
        // each card's const name), which is UI-thread-confined. The header's
        // "caller must hold the state lock" is written for callers that are
        // not the thread which owns every writer; this one is.
        const auto liveSlots = slotConfigCoordinator_->captureLive(
            controller->memory().slotBus());
        for (int s = 1; s <= 7; ++s) {
            SlotPeripheral* p = bus.peripheral(s);
            if (!p) continue;
            const bool builtIn = profileCfg.builtInSlots[s].has_value();

            // ── Cards with mountable bays (SmartPort / CFFA / HDV) ────────
            if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(p)) {
                any = true;
                ImGui::PushID(2000 + s);
                ImGui::Text("Slot %d — %s%s", s,
                            pom2::cardLabelForKey(liveSlots.keys[s]),
                            builtIn ? " (built-in)" : "");

                // The chain length, on a card that lets the user pick it (the
                // Liron: 2/4/6/8 units on its SmartPort bus). The SmartPort
                // card has its own panel, with the same choice.
                if (const auto choices = media->bayCountChoices(); !choices.empty()) {
                    ImGui::SameLine();
                    const int cur = media->bayCount();
                    ImGui::SetNextItemWidth(70.0f);
                    if (ImGui::BeginCombo("units", std::to_string(cur).c_str())) {
                        for (int n : choices) {
                            if (ImGui::Selectable(std::to_string(n).c_str(), n == cur) &&
                                n != cur) {
                                const auto r = storageCoordinator_->setMediaBayCount(
                                    *controller, *settings, s, n);
                                if (r.ok) mPrimed[s].fill(false);
                                tapeStatusMessage = "Slot " + std::to_string(s) +
                                    (r.ok ? ": " + std::to_string(n) +
                                            " units — seen at the next boot "
                                            "(ProDOS 8 2.4+ remaps units 2+ onto "
                                            "other slots)"
                                          : ": " + r.error);
                                tapeStatusUntil = lastFrameTime + (r.ok ? 4.0 : 6.0);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "Units on the card's SmartPort chain — 3.5\" or hard-disk "
                            "images. 0/1 are drive 1/2 of slot %d;\nProDOS 8 2.4+ shows "
                            "units 2+ under other slots with no disk device (14 devices "
                            "at most).\nThe firmware counts the chain at boot: reboot to "
                            "see a change.", s);
                }

                int nb = media->bayCount();
                if (nb > kPanelBays) nb = kPanelBays;
                bool bootable = false;
                for (int b = 0; b < nb; ++b) {
                    // Snapshot the bay state under the lock — the worker
                    // mutates it during block I/O (loaded/dirty flags,
                    // lastError strings). Same snapshot-under-lock rule as
                    // the Disk II / HDV panels; the lock is NOT held across
                    // the rendering below (the Mount/Eject buttons take it
                    // themselves).
                    pom2::MediaBayInfo info;
                    {
                        std::lock_guard<std::mutex> lk(controller->stateMutex());
                        info = media->bayInfo(b);
                    }
                    ImGui::PushID(b);
                    ImGui::Indent();

                    dot(info.loaded, info.writeProtected);
                    if (info.supportsTypeSelect || nb > 1) ImGui::Text("Unit %d", b);
                    else                                   ImGui::TextUnformatted("Image");
                    if (info.loaded) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s, %u blocks%s)",
                            info.kindLabel.empty() ? "media" : info.kindLabel.c_str(),
                            info.blockCount, info.writeProtected ? ", WP" : "");
                    } else if (!info.kindLabel.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", info.kindLabel.c_str());
                    }

                    // Type selector (SmartPort units only).
                    if (info.supportsTypeSelect) {
                        const auto opts = media->bayTypeOptions(b);
                        const char* curLabel = "(empty)";
                        for (const auto& o : opts)
                            if (o.first == info.typeKey) { curLabel = o.second.c_str(); break; }
                        ImGui::SetNextItemWidth(150);
                        if (ImGui::BeginCombo("Type", curLabel)) {
                            for (const auto& o : opts) {
                                const bool sel = (o.first == info.typeKey);
                                if (ImGui::Selectable(o.second.c_str(), sel) &&
                                    o.first != info.typeKey) {
                                    // Through the coordinator, like Mount and
                                    // Eject beside it: it re-resolves the card
                                    // by slot under the lock and owns the
                                    // bay-key rules — the hand-rolled
                                    // `persistMediaBay` this replaces wrote
                                    // `hdv_path` with no auto-provision or
                                    // host-folder guard, so ticking a box on a
                                    // synthesised /HOST/ volume overwrote the
                                    // user's real HDV path.
                                    const auto r =
                                        storageCoordinator_->setMediaBayType(
                                            *controller, *settings, s, b,
                                            o.first);
                                    if (r.ok) {
                                        mPrimed[s][b] = false;
                                    } else {
                                        tapeStatusMessage =
                                            "Slot " + std::to_string(s) +
                                            ": " + r.error;
                                        tapeStatusUntil = lastFrameTime + 4.0;
                                    }
                                }
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    }

                    const bool typeAllows =
                        !info.supportsTypeSelect || !info.typeKey.empty();

                    char* buf = mBuf[s][b].data();
                    if (!mPrimed[s][b]) {
                        std::snprintf(buf, mBuf[s][b].size(), "%s", info.path.c_str());
                        mPrimed[s][b] = true;
                    }
                    ImGui::SetNextItemWidth(300);
                    ImGui::InputText("##path", buf, mBuf[s][b].size());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(buf[0] == '\0' || !typeAllows);
                    if (ImGui::Button("Mount")) {
                        // Two-phase mount (the coordinator reads the image with
                        // no lock held, then adopts it and persists the bay
                        // keys). mountBay() under stateMutex stalled the CPU
                        // worker and the window for the whole read — 25.8 ms
                        // for a 32 MiB HDV, more than a PAL frame.
                        // Resolved BEFORE the call: the coordinator re-resolves
                        // the card by slot, so `p` is only known-live up to here.
                        const bool isHdv =
                            dynamic_cast<ProDOSHardDiskCard*>(p) != nullptr;
                        const auto r = storageCoordinator_->mountMediaBay(
                            *controller, *settings, s, b, buf);
                        if (r.ok && isHdv) {
                            hdvPath   = buf;
                            hdvStatus = std::string("loaded: ") + buf;
                        }
                        tapeStatusMessage = r.ok
                            ? ("Slot " + std::to_string(s) + ": mounted " + buf)
                            : ("Slot " + std::to_string(s) + ": mount failed: " +
                               r.error);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!info.loaded);
                    if (ImGui::Button("Eject")) {
                        // Same reason the Mount button above moved: ejectBay()
                        // under stateMutex ran the save-on-eject rewrite with
                        // the machine and the window both frozen behind it.
                        // The coordinator splits that write out of the lock
                        // and owns the bay-key persistence.
                        const auto r = storageCoordinator_->ejectMediaBay(
                            *controller, *settings, s, b);
                        if (r.ok) mPrimed[s][b] = false;
                        tapeStatusMessage = "Slot " + std::to_string(s) +
                            (r.ok ? ": ejected" : ": eject failed: " + r.error);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();

                    if (info.supportsWriteBack) {
                        // The notch (MediaNotch.h): the mounted file's own
                        // read-only bit, so the protection travels with the
                        // image rather than staying on the bay.
                        bool protect = info.writeProtected;
                        ImGui::BeginDisabled(!typeAllows || !info.loaded);
                        if (ImGui::Checkbox("Write-protected (the notch on this medium)", &protect)) {
                            const auto r = storageCoordinator_->setMediaNotch(
                                *controller, info.path, protect);
                            tapeStatusMessage = "Slot " + std::to_string(s) +
                                (r.ok ? (protect ? ": WRITE-PROTECTED" : ": WRITABLE")
                                      : ": " + r.error);
                            tapeStatusUntil = lastFrameTime + 4.0;
                        }
                        ImGui::EndDisabled();

                        // Standing warning, not just an eject-time one. The
                        // status bar's eject menu already asks before pulling
                        // a bay with unsaved blocks, but the session that
                        // loses work never opens it: mount, play, quit. And
                        // unlike a floppy — whose isWriteProtected() folds in
                        // `!writeBackEnabled`, so the guest is told no — a
                        // block device deliberately presents a fully writable
                        // volume with write-back off (ProDOSHardDiskCard::
                        // writeDataByte: "a real hard disk is read/write to
                        // ProDOS"). The guest's save therefore SUCCEEDS, the
                        // file never changes, and nothing anywhere says so.
                        // That is the gap this line closes.
                        if (info.loaded && !info.writeBackEnabled) {
                            ImGui::TextColored(
                                ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                                info.hasUnsavedChanges
                                    ? "WRITE-PROTECTED — this volume has "
                                      "already been written to, and none of "
                                      "it will reach the file"
                                    : "WRITE-PROTECTED — nothing written here "
                                      "will reach the file");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    "A block device still reports itself "
                                    "writable to ProDOS, so the guest's save "
                                    "appears to succeed —\nthe blocks live "
                                    "in memory and are dropped on eject or "
                                    "quit.\nUntick Write-protected above to "
                                    "let them be saved.");
                        } else if (info.loaded) {
                            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                                               "WRITABLE — saved in the background");
                        }
                    }

                    if (info.loaded && !info.persistenceState.empty())
                        ImGui::Text("Host file: %s", info.persistenceState.c_str());
                    if (!info.persistenceError.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                           "Save failed: %s", info.persistenceError.c_str());
                    if (!info.lastError.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                           "Error: %s", info.lastError.c_str());

                    if (b == 0 && info.loaded) bootable = true;
                    ImGui::Unindent();
                    ImGui::PopID();
                }

                ImGui::BeginDisabled(!bootable);
                if (ImGui::SmallButton("Boot slot")) {
                    controller->bootFromSlot(s);
                    tapeStatusMessage = "Booting slot " + std::to_string(s);
                    tapeStatusUntil = lastFrameTime + 3.0;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::Separator();
            }
            // ── Internal Disk II drives (5.25") ───────────────────────────
            else if (auto* d2 = dynamic_cast<DiskIICard*>(p)) {
                any = true;
                ImGui::PushID(3000 + s);
                ImGui::Text("Slot %d — %s%s", s,
                            pom2::cardLabelForKey(liveSlots.keys[s]),
                            builtIn ? " (built-in)" : "");

                bool bootable = false;
                for (int drv = 0; drv < DiskIICard::kDriveCount; ++drv) {
                    // Snapshot under the lock (worker mutates load state /
                    // path during inserts from other panels); not held
                    // across rendering — the buttons lock themselves.
                    bool loaded;
                    std::string path;
                    {
                        std::lock_guard<std::mutex> lk(controller->stateMutex());
                        loaded = d2->isDiskLoaded(drv);
                        if (!dPrimed[s][drv]) path = d2->getDiskPath(drv);
                    }
                    if (drv == 0 && loaded) bootable = true;
                    ImGui::PushID(drv);
                    ImGui::Indent();
                    dot(loaded, false);
                    ImGui::Text("Drive %d", drv + 1);

                    char* buf = dBuf[s][drv].data();
                    if (!dPrimed[s][drv]) {
                        std::snprintf(buf, dBuf[s][drv].size(), "%s",
                                      path.c_str());
                        dPrimed[s][drv] = true;
                    }
                    ImGui::SetNextItemWidth(300);
                    ImGui::InputText("##d2path", buf, dBuf[s][drv].size());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(buf[0] == '\0');
                    if (ImGui::Button("Insert")) {
                        // Through the coordinator so this panel persists the
                        // SAME keys the File menu and the restore path use.
                        // The hand-rolled version wrote `disk_path_slot<N>`
                        // and skipped drive 2 entirely, on a comment claiming
                        // "drive 2 mounts are session-only" — untrue since
                        // `diskIIPathSettingKey` gained the `_drive2` suffix
                        // and `restoreMediaFromSettings` began looping BOTH
                        // drives. A drive-2 insert here was therefore lost on
                        // restart while the same insert from the File menu
                        // survived.
                        const auto r = storageCoordinator_->mountDiskII(
                            *controller, *settings, s, drv, buf,
                            /*seekTrackZero=*/true);
                        const bool ok = r.ok;
                        tapeStatusMessage = ok
                            ? ("Slot " + std::to_string(s) + " drive " +
                               std::to_string(drv + 1) + ": inserted")
                            : ("Slot " + std::to_string(s) + ": insert failed: " +
                               r.error);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!loaded);
                    if (ImGui::Button("Eject")) {
                        // Same reason as the Insert button above: the
                        // coordinator clears the key that MATCHES the drive
                        // (`_drive2` for drive 2). The hand-rolled version
                        // cleared nothing for drive 2, so a disk ejected here
                        // was remounted on the next launch unless a clean quit
                        // happened to rewrite the key from live state first.
                        const auto r = storageCoordinator_->ejectDiskII(
                            *controller, *settings, s, drv);
                        const bool ok = r.ok;
                        if (ok) dPrimed[s][drv] = false;
                        tapeStatusMessage = "Slot " + std::to_string(s) +
                            " drive " + std::to_string(drv + 1) +
                            (ok ? ": ejected" : ": eject failed: " + r.error);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::Unindent();
                    ImGui::PopID();
                }

                ImGui::BeginDisabled(!bootable);
                if (ImGui::SmallButton("Boot slot")) {
                    controller->bootFromSlot(s);
                    tapeStatusMessage = "Booting slot " + std::to_string(s);
                    tapeStatusUntil = lastFrameTime + 3.0;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::Separator();
            }
        }

        if (!any)
            ImGui::TextDisabled("No storage cards plugged.");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ─── Emulation restart ──────────────────────────────────────────────────

std::string MainWindow::firstExistingPath(const std::vector<std::string>& candidates)
{
    // pom2::findFirstResource probes each candidate against CWD, the
    // build/-relative `../` `../../` roots (dev), and the executable-
    // relative / FHS-install roots (portable bundle, AppImage, /usr/bin).
    // See ResourcePaths.h.
    return pom2::findFirstResource(candidates);
}

M6502::CpuMode MainWindow::resolveCpuMode(M6502::CpuMode profileDefault) const
{
    return pom2::resolveCpuModeSetting(*settings, profileDefault);   // ProfileSwitch.h
}

float MainWindow::floppyMotorPitchForProfile(pom2::SystemProfile p)
{
    switch (p) {
        case pom2::SystemProfile::AppleIIc:
        case pom2::SystemProfile::AppleIIcPlus:
        case pom2::SystemProfile::AppleIIcPAL:
            return 1.4f;       // Sony internal drive ≈ 40% faster spin-up
        default:
            return 1.0f;       // original Disk II Shugart — native rate
    }
}

void MainWindow::setGlfwWindow(GLFWwindow* w)
{
    window = w;
    // Catch up the title once the handle is available — the constructor
    // may have resolved a non-default profile before main.cpp could hand
    // us the window, so the initial title from glfwCreateWindow wouldn't
    // reflect the active machine otherwise.
    if (window) {
        const auto& cfg = pom2::profileConfig(activeProfile);
        std::string title = "POM2 " POM2_VERSION_STRING " — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());

        // Reopen at the geometry the last windowed session ended with.
        // Skipped in kiosk (main() already created the window full-screen)
        // and when nothing was ever persisted, in which case main()'s
        // default size stands. A saved position is clamped back onto a
        // monitor so a window saved on a since-disconnected screen can't
        // reopen off-screen.
        if (!kiosk_ && loadWindowGeometryFromSettings()) {
            // Validate against the WHOLE virtual desktop, not just the
            // primary monitor: a monitor to the left of primary has
            // NEGATIVE virtual-screen X and one to the right has X beyond
            // the primary width, so a primary-only clamp dragged every
            // secondary-display window back to the centre of screen 1 on
            // each launch, with no way to make it stick.
            int mc = 0;
            GLFWmonitor** mons = glfwGetMonitors(&mc);
            bool onSomeMonitor = false;
            for (int i = 0; i < mc && !onSomeMonitor; ++i) {
                int mx = 0, my = 0, mw = 0, mh = 0;
                glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
                // "Visible enough to grab": the title bar's left corner
                // must sit inside this monitor's work area.
                if (savedWinX_ >= mx - 32 && savedWinX_ <= mx + mw - 64 &&
                    savedWinY_ >= my - 32 && savedWinY_ <= my + mh - 64)
                    onSomeMonitor = true;
            }
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (vm) {
                if (savedWinW_ > vm->width)  savedWinW_ = vm->width;
                if (savedWinH_ > vm->height) savedWinH_ = vm->height;
            }
            if (!onSomeMonitor && mon) {
                // Saved on a since-disconnected screen — recentre on
                // primary rather than reopening off-screen. Around the
                // primary's WORK AREA, whose origin is a virtual-desktop
                // coordinate: the old form centred a video-mode SIZE as
                // though the primary always started at (0,0), so on a layout
                // whose primary sits to the right of another monitor the
                // "recentred" window opened on the neighbour — or under the
                // menu bar, since a work area also excludes the system
                // chrome the raw mode includes.
                int px = 0, py = 0, pw = 0, ph = 0;
                glfwGetMonitorWorkarea(mon, &px, &py, &pw, &ph);
                if (pw > 0 && ph > 0) {
                    if (savedWinW_ > pw) savedWinW_ = pw;
                    if (savedWinH_ > ph) savedWinH_ = ph;
                    savedWinX_ = px + (pw - savedWinW_) / 2;
                    savedWinY_ = py + (ph - savedWinH_) / 2;
                }
            }
            glfwSetWindowSize(window, savedWinW_, savedWinH_);
            glfwSetWindowPos (window, savedWinX_, savedWinY_);
            if (savedWinMaximized_) glfwMaximizeWindow(window);
        }
    }
}

void MainWindow::stopSlotNetworkWorkers()
{
    // Same shape as NetworkCoordinator's Stop button: resolve the cards
    // under a brief lock, join their workers once it is gone. The CPU
    // worker is already stopped on every caller; the AI server is still
    // attached until beginLocked, so this must not run while holding
    // stateMutex (the mutex is non-recursive and the join can take
    // seconds).
    //
    // Never throws: the callers (`applyProfile`, `restartEmulationFromSettings`)
    // have already committed the profile and stopped the worker, and main()
    // has no catch — an escaping exception is a `std::terminate` with no log
    // line. Each card's stop is tried even if a sibling's threw.
    std::vector<pom2::FujiNetTransport*> links;
    std::vector<SuperSerialCard*> serials;
    try {
        links.reserve(SlotBus::kSlotCount);
        serials.reserve(SlotBus::kSlotCount);
        auto st = controller->lockState();
        auto& bus = st.memory().slotBus();
        for (int slot = 0; slot < SlotBus::kSlotCount; ++slot) {
            if (auto* fn = dynamic_cast<pom2::FujiNetCard*>(bus.peripheral(slot)))
                links.push_back(&fn->transportLink());
            if (auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot)))
                serials.push_back(ssc);
        }
    } catch (...) {
        pom2::log().warn("Slots", "host worker sweep failed; "
                         "destructors will join under the lock");
        return;
    }
    for (auto* link : links) {
        try {
            link->stop();
        } catch (const std::exception& e) {
            pom2::log().warn("Slots",
                std::string("FujiNet worker stop failed: ") + e.what());
        } catch (...) {
            pom2::log().warn("Slots", "FujiNet worker stop failed");
        }
    }
    for (auto* ssc : serials) {
        try {
            ssc->stopListening();
        } catch (const std::exception& e) {
            pom2::log().warn("Slots",
                std::string("SSC listener stop failed: ") + e.what());
        } catch (...) {
            pom2::log().warn("Slots", "SSC listener stop failed");
        }
    }
}

void MainWindow::abandonSlotRebuild(const char* what, const std::string& why)
{
    try {
        auto st = controller->lockState();
        slotRebuildCoordinator_->abandonLocked(st);
    } catch (const std::exception& e) {
        pom2::log().warn("Slots", std::string("abandoning the rebuild "
                         "failed too: ") + e.what());
    } catch (...) {
        pom2::log().warn("Slots", "abandoning the rebuild failed too");
    }
    tapeStatusMessage = std::string(what) + " failed: " + why +
                        " — machine paused; apply again once fixed";
    tapeStatusUntil = lastFrameTime + 12.0;
    pom2::log().warn("Slots", tapeStatusMessage);
}

void MainWindow::applyProfile(pom2::SystemProfile p)
{
    try {
        applyProfileTransaction(p);
    } catch (const std::exception& e) {
        abandonSlotRebuild("Profile switch", e.what());
    } catch (...) {
        abandonSlotRebuild("Profile switch", "unknown error");
    }
}

bool MainWindow::restartEmulationFromSettings()
{
    try {
        return restartEmulationTransaction();
    } catch (const std::exception& e) {
        abandonSlotRebuild("Slot rebuild", e.what());
    } catch (...) {
        abandonSlotRebuild("Slot rebuild", "unknown error");
    }
    return false;
}

void MainWindow::applyProfileTransaction(pom2::SystemProfile p)
{
    // The machine half lives in ProfileSwitch.cpp (TODO G5-6), where a test
    // can drive it; what stays here is what only the window knows. Each hook
    // runs at the point the inline code used to do the same thing — the
    // step numbers in ProfileSwitch.h are the ones these comments used.
    const auto& cfg = pom2::profileConfig(p);
    pom2::ProfileSwitchHooks hooks;
    hooks.flushMedia = [this](std::string& err) { return flushSlotMedia(err); };
    hooks.afterFlush = [this, p] {
        // Stop FujiNet / SSC host workers before the cards they talk to go.
        slotRebuildCoordinator_->prepareAfterFlush();
        slotRebuildCoordinator_->stopHostWorkers();
        // 0. Commit the active profile NOW — plugSlotsFromSettings (step 7)
        //    reads it to pick the built-in slots and the ROM identities.
        activeProfile = p;
    };
    hooks.beginLocked = [this](pom2::StateAccess& st) {
        slotRebuildCoordinator_->beginLocked(st);
    };
    hooks.charRomOverride = [this]() -> std::string {
        return charRomLocale != pom2::CharRomLocale::ProfileDefault
                   ? pom2::resolveCharRomPath(charRomLocale)
                   : std::string();
    };
    hooks.charRomBank = pom2::charRomBank(charRomLocale);
    hooks.plugSlots = [this](pom2::StateAccess& st, bool iieMode) {
        display->setAuxMemory(iieMode ? st.memory().auxData() : nullptr);
        plugSlotsFromSettings(st);
        // The Slot Config draft and the media panels' input fields describe
        // the machine that just left.
        slotDraftInited_ = false;
        ++mediaPanelSeedGen_;
    };
    hooks.afterPlug = [this, &cfg] {
        // 7a. Deferred: the link start() and the SSC bind can block.
        (void)startDeferredFujiNetLinks();
        startDeferredSscListeners();
        // 7b. A profile that SHIPS an on-board Le Chat Mauve (the //c PAL's
        //     adapter) comes up in its RGB mode.
        bool builtinRgb = false;
        for (int s = 1; s <= 7; ++s)
            if (cfg.builtInSlots[s].has_value() &&
                cfg.builtInSlots[s]->cardKey == "chatmauve")
                builtinRgb = true;
        if (builtinRgb &&
            devicePanelCoordinator_->captureInventory().chatMauvePlugged())
            display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    };
    hooks.publishLocked = [this](pom2::StateAccess& st) {
        slotRebuildCoordinator_->publishLocked(st);
    };

    const auto r = pom2::switchProfile(*controller, *storageCoordinator_, *settings,
                                       p, !settingsReadOnly(), hooks);
    if (!r.applied) {
        tapeStatusMessage = "Profile switch refused — save failed: " + r.error;
        tapeStatusUntil = lastFrameTime + 8.0;
        pom2::log().warn("Profile", tapeStatusMessage);
        return;
    }
    romStatus  = r.romStatus;
    romLoaded_ = r.romLoaded;
    if (r.romLoaded) romPath = r.romPath;
    charRomPath = r.charRomPath;
    diskSavedCyclesPerFrame = cfg.defaultCyclesPerFrame;
    // The internal //c drive is a Sony mechanism that spins up faster.
    controller->floppySound525().setMotorPitch(floppyMotorPitchForProfile(p));

    if (window) {
        std::string title = "POM2 " POM2_VERSION_STRING " — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());
    }
    pom2::log().info("Profile",
        std::string("Active = ") + std::string(cfg.displayName) +
        ", ROM = " + (r.romPath.empty() ? "<missing>" : r.romPath) +
        ", CPU = " + (r.cpuIsCmos ? "65C02" : "NMOS"));
    aiServer->setProfileLabel(std::string(cfg.displayName));
}

bool MainWindow::restartEmulationTransaction()
{
    // 0. Snapshot LIVE media into settings BEFORE teardown. Menu Insert/Eject
    //    and the HDV/CFFA library mounts update the live cards but NOT the
    //    settings keys (those are written only at shutdown), so without this a
    //    Slot-Config "Apply" rebuilds from stale keys and silently drops the
    //    mounted disk/HDV/CFFA. plugSlotsFromSettings restores FROM settings,
    //    so persisting the live state here preserves it.
    //
    //    The coordinator owns the exclusions that go with it: a session-only
    //    auto-provisioned HDV slot and a synthesised "[host folder] " volume
    //    must not reach hdv_path, or they come back as a real mount next time.
    //
    //    The loop this replaces read isDiskLoaded()/getDiskPath() with their
    //    default arguments, so drive 2's path was never synced and Apply
    //    dropped whatever was mounted in it.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const auto snapshot = storageCoordinator_->captureRebuildSnapshot(
            controller->memory().slotBus());
        storageCoordinator_->persistRebuildSettings(*settings, snapshot);
    }

    const auto previousMode = controller->getMode();
    controller->stop();
    std::string flushErr;
    if (!flushSlotMedia(flushErr)) {
        tapeStatusMessage = "Slot rebuild refused — save failed: " + flushErr;
        tapeStatusUntil = lastFrameTime + 8.0;
        pom2::log().warn("Slots", tapeStatusMessage);
        controller->setMode(previousMode);
        return false;
    }
    // The rebuild is now committed. Its session-local auto-plugged media will
    // be destroyed below, so their shutdown-persistence markers no longer
    // describe a live card.
    // Same commit point as applyProfile: the flush above succeeded, so the
    // rewind ring — whose SLOTn sections describe the card set being torn
    // down, and which would be incoherent restored onto the rebuilt set — and
    // the session-only provisioning markers are invalidated once, by the
    // transaction rather than by two hand-kept copies.
    slotRebuildCoordinator_->prepareAfterFlush();

    slotRebuildCoordinator_->stopHostWorkers();

    // 2. Tear down all cards and clear our raw pointers. Holding the
    //    state mutex isn't strictly necessary now that the worker is
    //    stopped, but it's cheap insurance against any UI thread that
    //    might be peeking — AND it serialises with the AI control
    //    server's handlers (which take the same mutex around card
    //    pointer reads). aiServer->detach() must happen under this
    //    lock to safely null disk6_/hdv5_ before slotBus.clear()
    //    destroys their pointees.
    {
        auto st = controller->lockState();
        // Same transaction as applyProfile's — one implementation, so the two
        // rebuild paths cannot drift on the detach order.
        slotRebuildCoordinator_->beginLocked(st);
    }

    // 3-4 run under stateMutex — same rationale as applyProfile steps
    // 5-7: the AI control server's handlers still run against
    // controller/memory (detach() nulled only its card pointers), so the
    // SlotBus rebuild + remounts must be atomic w.r.t. their lock.
    {
    auto st = controller->lockState();

    // 3. Re-run plugSlotsFromSettings() with the freshly-saved keys.
    plugSlotsFromSettings(st);
    // Re-seed the Slot Config draft from the rebuilt slotCards[] next
    // render, and re-prime the Media panel's path buffers from the new cards.
    slotDraftInited_ = false;
    ++mediaPanelSeedGen_;

    // 4. Media is restored by `plugSlotsFromSettings` itself — its phase 2
    //    calls `StorageCoordinator::restoreMediaFromSettings` against the
    //    finished topology, which covers BOTH Disk II drives, the per-slot
    //    and legacy keys, the HDV/CFFA cards and the SmartPort units.
    //
    //    A hand-rolled copy of the Disk II half stood here and ran right
    //    after it, so every 5.25" image was decoded twice on each Apply —
    //    under the lock, which is where a nibble decode costs the most — and
    //    a second copy of the `_drive2` key rule was kept alive next to the
    //    one the coordinator owns. Whatever the two disagreed about, the
    //    later one won silently.

    // RamWorks size lives in settings, not on the SlotBus. applyProfile
    // step 4 grows the backing BEFORE the wipe so `clearRam()` paints
    // every bank with the 00/FF pattern. Doing it after `coldBoot()`
    // (hunt #21's first pass) left $C073 working but banks 1+ as zeros.
    {
        if (activeProfile == pom2::SystemProfile::AppleIIe ||
            activeProfile == pom2::SystemProfile::AppleIIeUnenhanced ||
            activeProfile == pom2::SystemProfile::AppleIIePAL ||
            activeProfile == pom2::SystemProfile::AppleIIeUnenhancedPAL) {
            const int banks = settings->getInt("ramworks_banks", 1);
            st.memory().setRamWorksBanks(
                static_cast<uint32_t>(banks > 0 ? banks : 1));
        } else if (pom2::profileConfig(activeProfile).iieMode) {
            st.memory().setRamWorksBanks(1);
        }
    }

    }   // end stateMutex scope over steps 3-4

    // 4a. Same as applyProfile's step 7a: the FujiNet transports and the SSC
    //     listeners are opened with the lock released and the worker still
    //     stopped.
    (void)startDeferredFujiNetLinks();
    startDeferredSscListeners();

    // 5. COLD BOOT + restart worker. `coldBoot()`, not `hardReset()`: the
    //    card set just changed, and hardReset preserves RAM — so everything
    //    the guest had built around the OLD hardware survived into the new
    //    machine. DOS 3.3 stays hooked to a slot whose Disk II is gone,
    //    ProDOS keeps a device table describing cards that no longer exist,
    //    a player keeps poking a Mockingboard that was unplugged, and the
    //    warm `resetSoftSwitchesWarm()` even leaves a II/II+'s display and
    //    Language Card banks as they were. The user asked for different
    //    hardware; on a real machine that means opening the lid and powering
    //    back on, which is exactly `coldBoot`: `clearRam()` with the MAME
    //    00/FF pattern, the FULL `resetSoftSwitches()`, and a hard CPU
    //    reset. It matches what `applyProfile` (step 4 + step 11) has always
    //    done for a profile switch — the same event, one rebuild smaller.
    //
    //    Route through the controller rather than `cpu().hardReset()` +
    //    `slotBus().reset()`: the controller path additionally disarms
    //    `iicSmartPortArmed_` (via `Memory::setIicSmartPortArmed(false)`)
    //    and resets the speaker / IWM / SmartPort hub. Pre-fix: on
    //    //c-class, the $C500 firmware punch stayed armed after
    //    `bootFromSlot(5)`, so the post-Apply reset vector was fetched while
    //    the punch was live → //c F8 autostart re-booted SmartPort instead
    //    of leaving the user at the BASIC prompt the Apply was meant to
    //    give them. `coldBoot()` disarms it too.
    controller->coldBoot();
    controller->start();

    // 6. Re-attach the AI control server with the freshly rebuilt card
    //    pointers — the slot-bus tear-down above invalidated whatever
    //    primaryDiskII()/primaryHdvCard() the server was holding. Held under stateMutex
    //    so any handler that observed the detached null sees the new
    //    pointers atomically with respect to its own lock acquisition.
    {
        // Publish under the machine lock so no AI request can observe a
        // partially rebuilt machine, and through the transaction so it
        // cannot happen while the bus is still being repopulated.
        auto st = controller->lockState();
        slotRebuildCoordinator_->publishLocked(st);
    }

    pom2::log().info("Slots",
                     "Cold-booted with the new slot mapping (RAM wiped).");
    return true;
}

// ─── GUI ↔ kiosk runtime transition ──────────────────────────────────────
//
// Kiosk is NOT a different machine: it is exclusive full-screen + the
// chrome-free render path + "never write settings". The emulated CPU,
// memory and slot cards are untouched, so the switch needs no snapshot
// round-trip — flipping the flag and moving the GLFW window is enough,
// and nothing about the running program is disturbed (a game keeps
// playing across the transition, mid-frame).

void MainWindow::saveWindowGeometryToSettings()
{
    if (savedWinW_ <= 0 || !settings) return;
    settings->setInt ("window_x", savedWinX_);
    settings->setInt ("window_y", savedWinY_);
    settings->setInt ("window_w", savedWinW_);
    settings->setInt ("window_h", savedWinH_);
    settings->setBool("window_maximized", savedWinMaximized_);
}

bool MainWindow::loadWindowGeometryFromSettings()
{
    if (!settings) return false;
    const int w = settings->getInt("window_w", 0);
    const int h = settings->getInt("window_h", 0);
    if (w <= 0 || h <= 0) return false;
    savedWinX_ = settings->getInt("window_x", 0);
    savedWinY_ = settings->getInt("window_y", 0);
    savedWinW_ = w;
    savedWinH_ = h;
    savedWinMaximized_ = settings->getBool("window_maximized", false);
    return true;
}

void MainWindow::captureWindowGeometryNow()
{
    if (!window || kiosk_ || settingsReadOnly()) return;
    // A MAXIMIZED window reports the maximized rect. Do NOT un-maximize to
    // measure: on X11 glfwRestoreWindow only posts a _NET_WM_STATE message
    // and returns, so the very next query still reads the maximized rect —
    // and we would have un-maximized the user's window for nothing. Record
    // the flag and KEEP whatever non-maximized geometry we already had
    // (from an earlier capture or from settings), so re-maximizing on
    // restore lands correctly and un-maximizing afterwards gives a sane
    // floating size instead of a screen-sized rectangle.
    const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
    savedWinMaximized_ = maximized;
    if (!maximized) {
        glfwGetWindowPos(window, &savedWinX_, &savedWinY_);
        glfwGetWindowSize(window, &savedWinW_, &savedWinH_);
    }
    saveWindowGeometryToSettings();
}
