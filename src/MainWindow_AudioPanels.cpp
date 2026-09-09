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

// MainWindow_AudioPanels — the ImGui bodies for the audio device panels:
// the consolidated mixer, Le Chat Mauve RGB, Mockingboard, Phasor and the
// Echo+/Cricket speech card. Moved out of MainWindow.cpp verbatim (no logic
// change) for the reason the file-size ratchet exists — the god-object does
// not get to keep 840 lines of panel bodies. None of these touch the
// anonymous-namespace helpers that keep the storage panels in MainWindow.cpp.

#include "MainWindow.h"
#include "DevicePanelCoordinator.h"
#include "AudioCoordinator.h"
#include "EmulationController.h"

#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "FloppyEmuDevice.h"
#include "FloppySoundDevice.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "PrinterSoundDevice.h"
#include "ProDOSBlockCard.h"
#include "Settings.h"
#include "SmartPortCard.h"
#include "SpeakerDevice.h"

#include "imgui.h"

// ─── Audio Mixer ─────────────────────────────────────────────────────────
//
// Consolidated mixer panel: one row per source (Master, Speaker, Cassette,
// Mockingboard if plugged, Disk 5.25", Disk 3.5" if its sample bank
// loaded). Sliders + mute checkboxes write directly into the underlying
// atomics on each source / on AudioDevice — no UI-side cache, so cross-
// thread read-back stays consistent. Replaces the two sliders that used
// to live in the Status panel.

void MainWindow::renderAudioMixerWindow()
{
    if (!show(pom2::PanelId::Mixer)) return;

    // The default size has to cover a full row — label column + volume
    // slider + meter + Mute + pan — or the rightmost control lands outside
    // the window with no way to reach it. Scale it, because everything the
    // row is built from (font, padding) scales with the UI zoom while a
    // literal pixel size would not.
    const float uiSc = uiScale_ * dpiScale_;
    ImGui::SetNextWindowPos (ImVec2(80, 80),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(540 * uiSc, 320 * uiSc), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Mixer", &show(pom2::PanelId::Mixer))) {
        ImGui::End();
        return;
    }

    // Row geometry. The columns used to hang off hard-coded 110/160/40/90 px
    // offsets, which do NOT follow uiScale_/dpiScale_ while the text and
    // padding around them do: the pan slider already sat ~100 px past the
    // content edge at this panel's own default size, and every zoom step
    // made it worse. Derive the label column and the meter from the font
    // (which does scale) and let the two sliders share whatever width the
    // window actually offers, so the row stays whole at any scale and in
    // any docked width.
    const float labelColW = 8.0f * ImGui::GetFontSize();

    // `panSrc` is the AudioSource whose stereo placement this row edits,
    // or null for a row that has no placement to offer: the master bus
    // (it IS the stereo field) and the AY cards, which pan themselves
    // per chip from the card's wiring — see AudioDevice.h.
    auto channelRow = [labelColW](const char* label, float& vol, bool& mute,
                         float peak, const char* idSuffix, bool dim,
                         AudioSource* panSrc = nullptr, float clicksArg = -1.0f) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float em     = ImGui::GetFontSize();
        const float avail  = ImGui::GetContentRegionAvail().x;
        // A label longer than the column (the "(samples missing)" rows)
        // pushes its own row right instead of being overdrawn by the slider.
        const float labelW = std::max(labelColW,
                                      ImGui::CalcTextSize(label).x + st.ItemSpacing.x);
        const float meterW = 3.0f * em;
        const float muteW  = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x
                           + ImGui::CalcTextSize("Mute").x;
        const float fixedW = labelW + meterW + muteW
                           + (panSrc ? 4.0f : 3.0f) * st.ItemSpacing.x;
        // Floor at a still-draggable width: below that the user has shrunk
        // the panel past what the controls need and clipping is on them.
        const float freeW  = std::max(6.0f * em, avail - fixedW);
        const float panW   = panSrc ? std::min(6.5f * em, freeW * 0.40f) : 0.0f;
        const float volW   = freeW - panW;

        if (dim) ImGui::BeginDisabled();
        ImGui::PushID(idSuffix);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(volW);
        const std::string slid = std::string("##v") + idSuffix;
        ImGui::SliderFloat(slid.c_str(), &vol, 0.0f, 2.0f, "%.2f");
        // Tiny activity meter: lets the user confirm at a glance that
        // the channel is actually producing samples (addresses the
        // "doesn't seem connected to the mixer" feedback). Green for
        // safe levels, yellow approaching clip, red at clip.
        ImGui::SameLine();
        const float clamped = std::min(1.0f, std::max(0.0f, peak));
        ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
        if      (clamped >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
        else if (clamped >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(clamped, ImVec2(meterW, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::string muteId = std::string("Mute##") + idSuffix;
        ImGui::Checkbox(muteId.c_str(), &mute);
        // Discontinuities per second in THIS source's own output — the
        // crackle detector (AudioSource::clicksPerSecond). A sampled or
        // synthesised source should read nothing here; the 1-bit speaker
        // legitimately steps on every toggle. Shown only when non-zero so
        // a clean mixer stays quiet.
        {
            const float clicks = clicksArg >= 0.0f ? clicksArg
                : (panSrc ? panSrc->clicksPerSecond.load(std::memory_order_relaxed) : 0.0f);
            if (clicks > 0.5f) {
                ImGui::SameLine();
                const ImVec4 c = clicks > 20.0f ? ImVec4(0.90f, 0.20f, 0.20f, 1.0f)
                                                : ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
                ImGui::TextColored(c, "%.0f clicks/s", static_cast<double>(clicks));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frame-to-frame jumps above %.2f full-scale in this\n"
                                      "source's own output, per second. A crackle you hear\n"
                                      "comes from the row that shows a number here.",
                                      static_cast<double>(AudioSource::kClickThreshold));
            }
        }
        // Stereo placement for a mono source. Centre (0) is unity on
        // both channels, so leaving it alone reproduces the pre-stereo
        // mix exactly.
        if (panSrc) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(panW);
            float pan = panSrc->pan.load(std::memory_order_relaxed);
            const std::string panId = std::string("##p") + idSuffix;
            // Build the whole readout ourselves: ImGui takes this string
            // as a printf FORMAT applied to the value, and one with no
            // conversion specifier is printed literally — which is how
            // the "centre" case works, and it saves showing a signed
            // number the user would have to decode. Keep it free of any
            // '%' for that reason: a literal one would have to be
            // doubled or snprintf reads past the end of the format.
            char fmt[24];
            if (pan < -0.005f)
                std::snprintf(fmt, sizeof fmt, "L %.2f", -pan);
            else if (pan > 0.005f)
                std::snprintf(fmt, sizeof fmt, "R %.2f",  pan);
            else
                std::snprintf(fmt, sizeof fmt, "centre");
            if (ImGui::SliderFloat(panId.c_str(), &pan, -1.0f, 1.0f, fmt))
                panSrc->pan.store(pan, std::memory_order_relaxed);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                panSrc->pan.store(0.0f, std::memory_order_relaxed);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stereo placement — right-click to centre");
        }
        ImGui::PopID();
        if (dim) ImGui::EndDisabled();
    };

    // ── Master ─────────────────────────────────────────────────────────
    AudioDevice& dev = controller->audio();
    float masterVol = dev.getMasterVolume();
    bool  masterMute = dev.isMasterMuted();
    // The master row carries its own clicks/s: the per-source tally is
    // pre-sum and pre-gain, so clipping and every mixer-side cut (a card
    // plugged, a pan flip, the suspend) show up ONLY here.
    channelRow("Master", masterVol, masterMute, dev.getMasterPeak(),
               "master", false, nullptr, dev.getMasterClicksPerSecond());
    if (masterVol != dev.getMasterVolume()) dev.setMasterVolume(masterVol);
    if (masterMute != dev.isMasterMuted()) dev.setMasterMuted(masterMute);
    // The bus is stereo, so the master needs a meter per channel — the
    // single bar above shows the louder side, which cannot tell a
    // hard-panned card from a centred one.
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("L / R");
        ImGui::SameLine(labelColW);
        // Two bars spanning roughly the volume slider above them — same
        // font-relative unit, so they stay aligned under UI scaling.
        const float barW = 5.6f * ImGui::GetFontSize();
        const auto bar = [barW](float v) {
            const float c = std::min(1.0f, std::max(0.0f, v));
            ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
            if      (c >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
            else if (c >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            ImGui::ProgressBar(c, ImVec2(barW, 0.0f), "");
            ImGui::PopStyleColor();
        };
        bar(dev.getMasterPeakL());
        ImGui::SameLine();
        bar(dev.getMasterPeakR());
        ImGui::SameLine();
        bool mono = dev.isMonoDownmix();
        if (ImGui::Checkbox("Mono", &mono)) dev.setMonoDownmix(mono);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Fold the stereo bus down to a centred image.\n"
                "The Mockingboard pans AY1 left and AY2 right on real\n"
                "hardware, so a single-chip tune plays from one speaker;\n"
                "tick this for mono gear or a centred mix.");
        }
    }
    ImGui::Separator();

    // ── Speaker ────────────────────────────────────────────────────────
    SpeakerDevice& spk = controller->speaker();
    float spkVol = spk.getVolume();
    bool  spkMute = spk.isMuted();
    channelRow("Speaker", spkVol, spkMute,
               spk.lastBufferPeak.load(std::memory_order_relaxed),
               "spk", false, &spk);
    if (spkVol != spk.getVolume()) spk.setVolume(spkVol);
    if (spkMute != spk.isMuted()) spk.setMuted(spkMute);

    // ── Cassette ───────────────────────────────────────────────────────
    CassetteDevice& tape = controller->cassette();
    float tapeVol = tape.getVolume();
    bool  tapeMute = tape.isMuted();
    channelRow("Cassette", tapeVol, tapeMute,
               tape.lastBufferPeak.load(std::memory_order_relaxed),
               "tape", false, &tape);
    if (tapeVol != tape.getVolume()) tape.setVolume(tapeVol);
    if (tapeMute != tape.isMuted()) tape.setMuted(tapeMute);

    // ── Slot audio cards (Mockingboard / Phasor / Echo+) ──────────────
    // One row per LIVE card, not one per alias. Two Mockingboard variants can
    // sit on the bus at once (catalog `mockingboard` = AC and
    // `mockingboard_c` = Sound II are different keys, so the duplicate check
    // does not object); the old `if (mockingboardCard)` showed only whichever
    // was plugged last and the other had no control at all.
    using AC = pom2::AudioCoordinator;
    for (const auto& card : audioCoordinator_->captureMixerCards()) {
        const char* name = "Card";
        const char* idTag = "card";
        switch (card.kind) {
            case AC::CardKind::Mockingboard:    name = "Mockingbd"; idTag = "mb";     break;
            case AC::CardKind::Phasor:          name = "Phasor";    idTag = "phasor"; break;
            case AC::CardKind::EchoPlus:        name = "Echo+";     idTag = "echop";  break;
            case AC::CardKind::EchoPlusTms5220: name = "Echo+ TMS"; idTag = "echotms";break;
        }
        const std::string lbl =
            std::string(name) + " (S" + std::to_string(card.slot) + ")";
        // Slot-qualified so two cards of the same type keep distinct ImGui ids.
        const std::string id = std::string(idTag) + std::to_string(card.slot);
        float vol = card.volume;
        bool mute = card.muted;
        channelRow(lbl.c_str(), vol, mute, card.peak, id.c_str(), false,
                   nullptr, card.clicks);
        if (vol != card.volume || mute != card.muted) {
            // Re-resolves the card under the lock before writing — the row was
            // drawn from a snapshot, and the slot can have been rebuilt since.
            audioCoordinator_->applyMixerCard(
                AC::MixerCardCommand{ card.kind, card.slot, vol, mute });
        }
    }

    // ── Disk 5.25" ─────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs525 = controller->floppySound525();
        const bool dim = !fs525.isLoaded();
        float vol = fs525.getVolume();
        bool  mute = fs525.isMuted();
        channelRow(dim ? "Disk 5.25\" (samples missing)" : "Disk 5.25\"",
                   vol, mute,
                   fs525.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs525", dim, &fs525);
        if (!dim) {
            if (vol != fs525.getVolume()) fs525.setVolume(vol);
            if (mute != fs525.isMuted()) fs525.setMuted(mute);
        }
    }

    // ── Disk 3.5" ──────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs35 = controller->floppySound35();
        const bool dim = !fs35.isLoaded();
        float vol = fs35.getVolume();
        bool  mute = fs35.isMuted();
        channelRow(dim ? "Disk 3.5\" (samples missing)" : "Disk 3.5\"",
                   vol, mute,
                   fs35.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs35", dim, &fs35);
        if (!dim) {
            if (vol != fs35.getVolume()) fs35.setVolume(vol);
            if (mute != fs35.isMuted()) fs35.setMuted(mute);
        }
    }

    // ── Printer (synthesised head / platen / carriage) ─────────────────
    // Always shown, like Speaker and Cassette: the source is host-side and
    // not owned by any card, so there is no slot to gate it on. Before this
    // row existed the level and mute persisted in state.cfg but had no
    // in-app control at all — silencing a too-loud printer meant quitting
    // and hand-editing the file.
    {
        float vol  = printerSound->volume();
        bool  mute = printerSound->muted();
        channelRow("Printer", vol, mute,
                   printerSound->lastBufferPeak.load(std::memory_order_relaxed),
                   "prn", false, printerSound.get());
        if (vol != printerSound->volume()) printerSound->setVolume(vol);
        if (mute != printerSound->muted()) printerSound->setMuted(mute);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Master is post-mix; per-channel knobs are pre-mix.");
    ImGui::TextDisabled("Bars show last-buffer peak with ~100 ms release.");
    ImGui::TextDisabled("AY cards pan themselves per chip (MAME wiring).");
    ImGui::End();
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!show(pom2::PanelId::ChatMauve)) return;

    // One acquisition for the snapshot, one for whatever the frame asked for.
    // This panel used to take the bare stateMutex SIX times per frame — once
    // to build the snapshot and once per toggle — through an alias that a
    // slot rebuild could have invalidated between any two of them.
    const auto snap = devicePanelCoordinator_->captureChatMauve();

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel->render("Le Chat Mauve###chatMauvePanel",
                                        show(pom2::PanelId::ChatMauve), snap);

    // Re-resolves the card under the lock and applies every requested change
    // in one critical section, then persists the toggles after unlocking.
    devicePanelCoordinator_->applyChatMauve(result);
}

// ─── Mockingboard live state panel ───────────────────────────────────────
//
// Diagnostic window for the Mockingboard A/C. Shows both 6522 VIAs' T1
// counter / IFR / IER + slot IRQ state, and both AY-3-8910 register
// banks (R0..R15). Primary use case: figuring out why an IRQ-driven
// music driver is silent. Three observable cases:
//
//   1. AY registers all stay 0 — the music handler isn't running at
//      all. Check IFR/IER + irqAsserted on the VIA. If T1 ticks but
//      IRQ never asserts, the IER is wrong or the driver hasn't
//      enabled T1 yet.
//   2. AY registers move every few frames — the driver is running and
//      the AY synth is producing samples; if you hear nothing, look at
//      AudioDevice (volume/mute) or the channel mixer R7.
//   3. AY registers load once and freeze — the install ran but only
//      one IRQ landed. Likely the handler isn't re-arming T1 or the
//      ack path is broken.
//
// The panel takes the controller state mutex for each snapshot and
// reads via the card's existing test/debug accessors
// (`peekViaRegister`, `getAyRegister`, `isIrqAsserted`).
void MainWindow::renderMockingboardPanelWindow()
{
    if (!show(pom2::PanelId::Mockingboard)) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mockingboard (VIA + AY state)",
                      &show(pom2::PanelId::Mockingboard),
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // One acquisition for the whole panel: plugged state, slot, level, the two
    // 6522+AY pairs and the Sound II SSI263. The card is resolved from the
    // live SlotBus inside that lock, so the panel can no longer read through
    // an alias that a slot rebuild has already destroyed. It also replaces
    // TWO separate lock_guard(stateMutex()) blocks — the chip registers and
    // the SSI263 state could previously come from different machine states.
    const auto mb = audioCoordinator_->captureMockingboard();
    if (!mb.plugged) {
        ImGui::TextDisabled("No Mockingboard plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }
    const auto& via = mb.via;
    const bool slotIrq = mb.slotIrq;

    // Slot IRQ indicator and volume readout.
    ImGui::Text("Slot IRQ line: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        mb.volume, mb.muted ? "(MUTED)" : "");
    ImGui::Separator();

    // Two-column display, one per 6522+AY pair.
    if (ImGui::BeginTable("##mb_chips", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 + AY #0 ($Cn00-$Cn0F)");
        ImGui::TableSetupColumn("VIA #2 + AY #1 ($Cn80-$Cn8F)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            // Telemetry first — these counters tell you instantly
            // whether the music driver is even talking to this VIA.
            // viaWrites grows on every guest STA $CnXX (Mockingboard
            // slot-ROM window); ayWrites grows only when LATCH→WRITE
            // strobes successfully complete and a register lands in
            // the AY. Both staying near zero mid-game = the driver
            // isn't running or its IRQ handler is short-circuiting
            // before the AY phase.
            ImGui::Text("VIA writes : %u", v.viaWrites);
            ImGui::Text("AY writes  : %u  (register-store WRITE strobes)",
                        v.ayWrites);
            ImGui::Text("AY resets  : %u  (!RESET pulses, wipes all regs)",
                        v.ayResets);
            ImGui::TextDisabled("AY cmd:  LATCH=%u WRITE=%u INACT=%u READ=%u",
                                v.cmdLatch, v.cmdWrite, v.cmdInactive,
                                v.cmdRead);
            ImGui::Separator();
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
            ImGui::Separator();
            ImGui::TextDisabled("AY-3-8910 registers:");
            // Two columns inside the cell: 8 regs each.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row;
                const int r1 = row + 8;
                ImGui::Text("R%-2d %3u $%02X    R%-2d %3u $%02X",
                            r0, v.ay[r0], v.ay[r0],
                            r1, v.ay[r1], v.ay[r1]);
            }
            // Friendly labels for the regs that decide audibility.
            ImGui::Separator();
            const uint16_t periodA = v.ay[0] | ((v.ay[1] & 0x0F) << 8);
            const uint16_t periodB = v.ay[2] | ((v.ay[3] & 0x0F) << 8);
            const uint16_t periodC = v.ay[4] | ((v.ay[5] & 0x0F) << 8);
            ImGui::Text("Ch A period $%03X  vol $%X", periodA, v.ay[8] & 0x1F);
            ImGui::Text("Ch B period $%03X  vol $%X", periodB, v.ay[9] & 0x1F);
            ImGui::Text("Ch C period $%03X  vol $%X", periodC, v.ay[10] & 0x1F);
            ImGui::Text("Mixer $%02X (tone %c%c%c noise %c%c%c)",
                        v.ay[7],
                        (v.ay[7] & 0x01) ? '.' : 'A',
                        (v.ay[7] & 0x02) ? '.' : 'B',
                        (v.ay[7] & 0x04) ? '.' : 'C',
                        (v.ay[7] & 0x08) ? '.' : 'A',
                        (v.ay[7] & 0x10) ? '.' : 'B',
                        (v.ay[7] & 0x20) ? '.' : 'C');
        }
        ImGui::EndTable();
    }

    // ── Sound II SSI263 section (only if this variant has one) ─────────
    const auto& ssiSnap = mb.ssi;
    if (mb.hasSsi) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                           "SSI263 (Sound II — speech @ $Cs40-$Cs44)");
        if (ssiSnap.powerDown) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                               "  CTL=1  →  POWER DOWN (silent)");
        }
        ImGui::TextColored(ssiSnap.aRequest
                             ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                             : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  A/!R: %s  →  VIA1.IFR.CA1 if PCR.0=0",
                           ssiSnap.aRequest ? "REQUEST" : "running");
        ImGui::Text("  Phoneme: $%02X (%d)  remaining: %d cyc",
                    ssiSnap.currentPhoneme, ssiSnap.currentPhoneme,
                    ssiSnap.phonemeRemainingCycles);
        ImGui::Text("  Writes since reset: %u  |  IRQ enable: %s",
                    ssiSnap.phonemeWriteCount,
                    ssiSnap.irqEnabled ? "yes" : "no");
        ImGui::Text("  Regs: $00=%02X  $01=%02X  $02=%02X  $03=%02X  $04=%02X",
                    ssiSnap.regs[0], ssiSnap.regs[1], ssiSnap.regs[2],
                    ssiSnap.regs[3], ssiSnap.regs[4]);
    }

    ImGui::End();
}

// ─── Phasor ──────────────────────────────────────────────────────────────

void MainWindow::renderPhasorPanelWindow()
{
    if (!show(pom2::PanelId::Phasor)) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Phasor (mode + 2×VIA + 4×AY)",
                      &show(pom2::PanelId::Phasor),
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // One acquisition for VIAs + AYs + mode + level, with the card resolved
    // from the live SlotBus inside it.
    const auto ph = audioCoordinator_->capturePhasor();
    if (!ph.plugged) {
        ImGui::TextDisabled("No Phasor plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }
    const auto& via = ph.via;
    const auto& ay  = ph.ay;
    const auto mode = static_cast<PhasorCard::Mode>(ph.mode);
    const int  clockScale = ph.clockScale;
    const bool slotIrq    = ph.slotIrq;

    // ── Mode banner ─────────────────────────────────────────────────────
    const char* modeLabel =
        (mode == PhasorCard::PH_Phasor)       ? "PHASOR NATIVE"  :
        (mode == PhasorCard::PH_EchoPlus)     ? "ECHO+"          :
        (mode == PhasorCard::PH_Mockingboard) ? "MOCKINGBOARD"   :
                                                "(unknown)";
    const ImVec4 modeColor =
        (mode == PhasorCard::PH_Phasor)       ? ImVec4(0.30f, 0.85f, 0.45f, 1.0f) :
        (mode == PhasorCard::PH_EchoPlus)     ? ImVec4(0.40f, 0.65f, 0.95f, 1.0f) :
                                                ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    ImGui::TextColored(modeColor, "Mode: %s", modeLabel);
    ImGui::SameLine();
    ImGui::TextDisabled("  (clock ×%d, %d AYs active)",
                        clockScale,
                        (mode == PhasorCard::PH_Mockingboard) ? 2 : 4);
    ImGui::Text("Slot IRQ line: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        ph.volume, ph.muted ? "(MUTED)" : "");
    {
        // Device-select page for this slot is $C0n0..$C0nF where
        // n = 8 + slot (e.g. slot 4 → $C0C0..$C0CF). The mode soft-
        // switch responds to read OR write of those 16 addresses.
        const int devHi = 0x8 + ph.slot;
        ImGui::TextDisabled(
            "Mode soft-switch: read/write $C0%X8 → MB, $C0%XD → Phasor",
            devHi, devHi);
    }
    ImGui::Separator();

    // ── VIA telemetry (2 cols) ─────────────────────────────────────────
    if (ImGui::BeginTable("##ph_vias", 2,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 ($Cs00-$Cs0F) → AY0 / AY1");
        ImGui::TableSetupColumn("VIA #2 ($Cs80-$Cs8F) → AY2 / AY3");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            ImGui::Text("VIA writes : %u", v.writes);
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    // ── AY-3-8913 register banks (4 cols) ─────────────────────────────
    if (ImGui::BeginTable("##ph_ays", 4,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("AY0 (VIA1 pri)");
        ImGui::TableSetupColumn("AY1 (VIA1 sec)");
        ImGui::TableSetupColumn("AY2 (VIA2 pri)");
        ImGui::TableSetupColumn("AY3 (VIA2 sec)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& a = ay[c];
            // In MB-compat mode the secondary AYs (1, 3) are unreachable
            // — flag the cell so the user understands why the bank
            // stays at zero even with a music driver running.
            const bool unreachableInMb =
                (mode == PhasorCard::PH_Mockingboard) && ((c & 1) != 0);
            if (unreachableInMb) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.4f, 1.0f),
                                   "(MB-compat: silent)");
            }
            ImGui::Text("writes %u", a.writes);
            ImGui::Text("resets %u", a.resets);
            ImGui::Separator();
            // Compact reg dump: 8 rows × 2 regs per cell.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row, r1 = row + 8;
                ImGui::Text("R%-2d $%02X  R%-2d $%02X",
                            r0, a.regs[r0], r1, a.regs[r1]);
            }
            ImGui::Separator();
            const uint16_t periodA = a.regs[0] | ((a.regs[1] & 0x0F) << 8);
            const uint16_t periodB = a.regs[2] | ((a.regs[3] & 0x0F) << 8);
            const uint16_t periodC = a.regs[4] | ((a.regs[5] & 0x0F) << 8);
            ImGui::Text("A $%03X v$%X", periodA, a.regs[8]  & 0x1F);
            ImGui::Text("B $%03X v$%X", periodB, a.regs[9]  & 0x1F);
            ImGui::Text("C $%03X v$%X", periodC, a.regs[10] & 0x1F);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ─── Echo+ ───────────────────────────────────────────────────────────────

void MainWindow::renderEchoPlusPanelWindow()
{
    if (!show(pom2::PanelId::EchoPlus)) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Echo+ (SSI263 speech)", &show(pom2::PanelId::EchoPlus),
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const auto ep = audioCoordinator_->captureEchoPlus();
    if (!ep.plugged) {
        ImGui::TextDisabled("No Echo+ plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }
    const auto& s = ep.chip;
    const bool slotIrq = ep.slotIrq;
    ImGui::Text("Slot %d  |  Volume: %.2f %s",
                ep.slot, ep.volume, ep.muted ? "(MUTED)" : "");
    ImGui::TextColored(slotIrq
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "Slot IRQ: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::Separator();

    // ── Live chip state ───────────────────────────────────────────────
    if (s.powerDown) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                           "CTL=1  →  POWER DOWN (silent)");
    } else {
        ImGui::Text("CTL=0  →  RUN");
    }
    const char* modeStr =
        (s.mode == pom2::Ssi263::MODE_IRQ_DISABLED)                    ? "00 IRQ disabled" :
        (s.mode == pom2::Ssi263::MODE_FRAME_IMMEDIATE_INFLECTION)      ? "01 Frame imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION)    ? "10 Phon. imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION) ? "11 Phon. trans. infl." :
                                                                          "??";
    ImGui::Text("Mode: %s  |  IRQ enable: %s",
                modeStr, s.irqEnabled ? "yes" : "no");
    ImGui::TextColored(s.aRequest
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "A/!R: %s", s.aRequest ? "REQUEST (phoneme done)"
                                              : "running");
    ImGui::Text("Current phoneme: $%02X (%d)",
                s.currentPhoneme, s.currentPhoneme);
    ImGui::Text("Duration remaining: %d cycles (≈ %.1f ms)",
                s.phonemeRemainingCycles,
                s.phonemeRemainingCycles / 1022.727);
    ImGui::Text("Phoneme writes since reset: %u",
                s.phonemeWriteCount);
    ImGui::Separator();

    // ── Register dump ──────────────────────────────────────────────────
    ImGui::TextDisabled("SSI263 registers ($Cs00-$Cs04):");
    const char* labels[5] = {
        "$00 DURPHON", "$01 INFLECT", "$02 RATEINF",
        "$03 CTTRAMP", "$04 FILFREQ"
    };
    for (int r = 0; r < 5; ++r) {
        ImGui::Text("%s = $%02X (%3u)", labels[r], s.regs[r], s.regs[r]);
    }
    ImGui::Separator();

    // ── Status footer ──────────────────────────────────────────────────
    ImGui::TextWrapped(
        "Audio: live — 62-phoneme PCM blob (ported from AppleWin) "
        "resampled from 22050 Hz to the host rate, scaled by the AMP "
        "register. Power-down (CTL=1) or FILFREQ=$FF squelches output.");

    ImGui::End();
}

// ─── Disk II ─────────────────────────────────────────────────────────────

void MainWindow::updateAutoTurbo()
{
    // Auto-turbo: while a disk is actively streaming, crank the CPU to ~60x
    // emulated so loads don't crawl at 1 MHz. Two activity sources:
    //
    //   • DiskII (5.25"): the motor line. Multi-instance friendly — one card
    //     spinning up is enough to enter turbo; all must be idle to leave it.
    //   • ProDOS hard disk (ProDOSHardDiskCard): no motor line, so the byte-
    //     loop firmware streams blocks at the plain CPU rate and HD games
    //     (e.g. Nox Archaist) load far slower than a 5.25" game that gets the
    //     motor-on turbo. Treat recent HDV data-port activity as the same
    //     "busy" signal; the card decays it over a few frames so a multi-block
    //     load stays in turbo end-to-end, then drops back for gameplay.
    //
    // Called every frame from render() (NOT from renderDiskPanelWindow, which
    // early-returns when its window is hidden — the default).
    bool anyMotorOn = false;
    for (auto* c : diskIICards()) {
        if (c && c->isMotorOn()) { anyMotorOn = true; break; }
    }
    // Decay + poll EVERY block card (HDV + CFFA can coexist) so a load on
    // either keeps turbo engaged. tickActivityDecay() must run on each card
    // (not short-circuit) so their independent decay counters all advance.
    bool hdvBusy = false;
    const auto blocks = blockCards();
    for (auto* dev : blocks) {
        dev->tickActivityDecay();
        if (dev->isBusy()) hdvBusy = true;
    }
    // SmartPort units carry the same hysteretic counter but are NOT
    // ProDOSBlockCards, so `blockCards()` never sees them. Until this loop
    // existed their counters were bumped and never bled off, and — more
    // than a stuck LED — SmartPort media sat outside disk turbo entirely.
    // That is the //c / //c+ boot path for 3.5" and HDV, so the machines
    // most dependent on it were the ones loading at 1 MHz while an HDV card
    // in a //e got ~60×.
    const auto smartPorts = smartPortCards();
    for (auto* sp : smartPorts) {
        for (size_t u = 0; u < pom2::SmartPortCard::kMaxUnits; ++u) {
            if (auto* unit = sp->unit(u)) {
                unit->tickActivityDecay();
                if (unit->isBusy()) hdvBusy = true;
            }
        }
    }
    const bool anyBusy       = anyMotorOn || hdvBusy;
    const bool turboEligible =
        diskTurboWhileMotor &&
        (!diskIICards().empty() || !blocks.empty() || !smartPorts.empty());
    // The override is COMPOSED by the controller, not stashed here. This used
    // to save `getCyclesPerFrame()` into a MainWindow member before writing
    // 1 M, and restore that member when the drive stopped — so anything that
    // changed the machine's speed DURING the burst was silently reverted:
    // a profile switch to PAL (20313) or a //c+ (68180) mid-load came back as
    // whatever was current when the motor spun up, and a speed-menu change
    // while loading never took effect at all. `setTurboOverride` /
    // `clearTurboOverride` keep the base in the controller, so the exit
    // restores the CURRENT base whoever set it (EmulationController.h).
    constexpr int kDiskTurboCyclesPerFrame = 1'000'000;   // ~60× emulated
    if (turboEligible) {
        if (anyBusy && !diskTurboActive) {
            controller->setTurboOverride(kDiskTurboCyclesPerFrame);
            diskTurboActive = true;
        } else if (!anyBusy && diskTurboActive) {
            controller->clearTurboOverride();
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        controller->clearTurboOverride();
        diskTurboActive = false;
    }
}
