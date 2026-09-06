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

// MainWindow_DevicePanels — the ImGui bodies for the non-audio device panels:
// Ethernet (Uthernet I/II), the Super Serial card, the parallel printer, the
// ImageWriter tray and the AI control server panel. Moved out of
// MainWindow.cpp verbatim; none touch the anonymous-namespace helpers that
// keep the storage panels there. See the file-size ratchet.

#include "MainWindow.h"
#include "DevicePanelCoordinator.h"
#include "EmulationController.h"

#include "SystemProfile.h"
#include "FujiNetCardFactory.h"
#include "FujiNet_ImGui.h"
#include "NetworkCoordinator.h"
#include "SerialPort.h"
#include "SpTransport.h"
#include "PrinterScreenDump.h"
#include "AiControlServer.h"
#include "AtomicFileReplace.h"
#include "FujiNetCard.h"
#include "ImageWriter_ImGui.h"
#include "Logger.h"
#include "PrinterFeedCursor.h"
#include "PrinterHistory.h"
#include "ResourcePaths.h"
#include "SlirpNetworkBackend.h"
#include "Uthernet_ImGui.h"
#include "GrapplerCard.h"
#include "ImageWriter.h"
#include "PrinterCard.h"
#include "PrinterCoordinator.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

#include "imgui.h"

// Ethernet (Uthernet I / II). Snapshot-under-lock → render → dispatch
// actions under the lock again, the LeChatMauve_ImGui pattern. The card
// pointers are non-owning (SlotBus owns the cards) and are nulled on
// every re-plug, so they are only ever dereferenced inside the lock.
void MainWindow::renderEthernetPanelWindow()
{
    if (!show(pom2::PanelId::Ethernet)) return;
    if (!ethernetPanel) ethernetPanel = std::make_unique<pom2::Uthernet_ImGui>();

    // Snapshot both NICs under one lock with the cards resolved inside it,
    // then apply whatever the frame asked for in a second one. The backend
    // choice and the slirp availability are host-side and need no lock.
    auto snap = devicePanelCoordinator_->captureEthernet();
    snap.slirpCompiledIn = pom2::slirpAvailable();
    snap.backendChoice   = settings->getString("ethernet_backend", "slirp");

    const auto action =
        ethernetPanel->render("Ethernet###ethernetPanel", show(pom2::PanelId::Ethernet), snap);

    devicePanelCoordinator_->applyEthernet(action);
}

void MainWindow::renderSscPanelWindow()
{
    if (!show(pom2::PanelId::Ssc)) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Super Serial###sscPanel", &show(pom2::PanelId::Ssc))) {
        ImGui::End();
        return;
    }

    // One panel hosts every plugged SSC under a tab bar. //c boots with
    // two (printer + modem); other profiles typically run zero or one.
    // Per-slot port-input state lives in a static map so each tab keeps
    // its own draft port number across frames.
    // Renders ONE tab from an immutable snapshot and returns what the user
    // asked for. It touches no card: the panel used to read eight fields and
    // perform four mutations straight through a card pointer, unlocked, from
    // a tab body that a slot rebuild could invalidate mid-frame.
    using Serial = pom2::DevicePanelCoordinator::SerialSnapshot;
    using SerialCmd = pom2::DevicePanelCoordinator::SerialCommand;
    auto renderOne = [&](const Serial& ssc) -> SerialCmd {
        SerialCmd cmd;
        const int slot = ssc.slot;
        cmd.slot = slot;
        static std::map<int, int> portDrafts;
        auto it = portDrafts.find(slot);
        if (it == portDrafts.end()) {
            portDrafts[slot] = ssc.port ? ssc.port
                : SuperSerialCard::kDefaultPort;
            it = portDrafts.find(slot);
        }
        int& portDraft = it->second;

        const bool listening = ssc.listening;
        const bool connected = ssc.connected;

        ImGui::Text("Status: %s%s",
            listening ? "listening" : "stopped",
            connected ? " — client connected" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("(slot %d)", slot);

        ImGui::Separator();
        ImGui::PushID(slot);
#ifdef __EMSCRIPTEN__
        // The host telnet bridge is compiled out under WASM
        // (SuperSerialCard::startListening() is a no-op — no TCP sockets in
        // the browser sandbox, see SuperSerialCard.cpp). The ACIA itself is
        // still emulated, so PR#n output still flows into the TX counter /
        // recent traffic below. Grey out the listener controls rather than
        // hide them, so the panel reads honestly.
        (void)listening; (void)connected;
        ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        ImGui::SameLine();
        ImGui::Button("Start listener");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Telnet bridge unavailable in the browser build.");
#else
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        if (portDraft < 1)     portDraft = 1;
        if (portDraft > 65535) portDraft = 65535;

        ImGui::SameLine();
        if (!listening) {
            if (ImGui::Button("Start listener")) {
                // Whether the bind succeeded is not known here — the
                // coordinator reports it back after applying.
                cmd.requestStart = true;
                cmd.port = static_cast<uint16_t>(portDraft);
            }
        } else {
            if (ImGui::Button("Stop listener")) cmd.requestStop = true;
        }

        if (listening) {
            ImGui::TextWrapped("Connect from a host terminal:");
            ImGui::TextWrapped("  telnet 127.0.0.1 %d", ssc.port);
            ImGui::TextWrapped("In the Apple II:  PR#%d  (or IN#%d for input)",
                slot, slot);
        } else {
            ImGui::TextDisabled("Click Start, then telnet to the port to "
                                "bridge I/O between your host shell and "
                                "the Apple II.");
        }
#endif

        ImGui::Separator();
        bool raw = ssc.rawMode;
        if (ImGui::Checkbox("Raw mode (8-bit binary)", &raw)) {
            cmd.requestRawMode = true;
            cmd.rawMode = raw;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: stock telnet — IAC ($FF) negotiation\n"
                              "swallowed + CR/LF normalised to CR.\n"
                              "On: every byte forwarded verbatim. Use for\n"
                              "XMODEM / Kermit / ADTPro / any binary protocol.");
        }

        bool tap = ssc.printerTap;
        if (ImGui::Checkbox("Feed ImageWriter printer", &tap)) {
            cmd.requestPrinterTap = true;
            cmd.printerTap = tap;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mirror every transmitted byte into the\n"
                              "host-side ImageWriter II (the //c's real\n"
                              "printer port is this serial card — slot 1\n"
                              "taps by default). A plugged parallel\n"
                              "Printer card or Grappler+ takes priority.");
        }

        ImGui::Separator();
        ImGui::Text("RX (telnet → A2): %llu B",
            static_cast<unsigned long long>(ssc.bytesRx));
        ImGui::Text("TX (A2 → telnet): %llu B",
            static_cast<unsigned long long>(ssc.bytesTx));

        if (ImGui::CollapsingHeader("Recent traffic")) {
            ImGui::TextDisabled("Last bytes the Apple II printed via PR#%d:",
                                slot);
            ImGui::TextWrapped("%s", ssc.recentTxText.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Last bytes the host typed:");
            ImGui::TextWrapped("%s", ssc.recentRxText.c_str());
        }
        ImGui::PopID();
        // Falling off the end of a value-returning lambda is UB. It has been
        // doing so since this panel got its snapshot/command boundary, and it
        // worked only because NRVO happened to build `cmd` in the caller's
        // return slot — the panel's start/stop, port, raw-mode and printer-tap
        // commands all rode on that. -Werror is what turned it up.
        return cmd;
    };

    // One acquisition for every tab's data, taken before any of them render.
    const auto ports = devicePanelCoordinator_->captureSerialCards();
    pom2::DevicePanelCoordinator::SerialCommand cmd;
    if (ports.size() == 1) {
        cmd = renderOne(ports[0]);
    } else if (ImGui::BeginTabBar("##sscTabs")) {
        // //c convention: sl1 = printer port, sl2 = modem port. Other
        // profiles just label by slot number.
        const bool isIIcLayout = (ports.size() == 2) &&
            (ports[0].slot == 1) && (ports[1].slot == 2);
        for (size_t i = 0; i < ports.size(); ++i) {
            const int slot = ports[i].slot;
            std::string tab;
            if (isIIcLayout) tab = (i == 0) ? "Printer port (sl1)"
                                            : "Modem port (sl2)";
            else             tab = "Slot " + std::to_string(slot);
            if (ImGui::BeginTabItem(tab.c_str())) {
                cmd = renderOne(ports[i]);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    // Applied once, after every tab has rendered: the coordinator re-resolves
    // the card under the machine lock, so a slot rebuild between the snapshot
    // and here means the command is dropped rather than written to a freed
    // card. A failed bind is reported back rather than guessed at.
    if (!cmd.empty()) {
        const auto r = devicePanelCoordinator_->applySerial(cmd);
        if (r.startAttempted && !r.startSucceeded) {
            tapeStatusMessage = "SSC slot " + std::to_string(cmd.slot) +
                                ": bind failed (port busy?)";
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
    }

    ImGui::End();
}

void MainWindow::renderPrinterPanelWindow()
{
    if (!show(pom2::PanelId::Printer)) return;
    // One acquisition for the whole panel: plugged state, slot, byte count,
    // truncation flag and the spool text. This used to read the spool through
    // a bare alias while the CPU thread appended to it — the comment below
    // claimed the lock was held everywhere it was touched, and it was not.
    const auto printerPanel =
        printerCoordinator_->capturePrinterPanel(*controller);
    if (!printerPanel.plugged) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    const std::string title = "Printer (slot " +
        std::to_string(printerPanel.slot) + ")###printerPanel";
    if (!ImGui::Begin(title.c_str(), &show(pom2::PanelId::Printer))) {
        ImGui::End();
        return;
    }

    const size_t nBytes = printerPanel.bytesWritten;
    ImGui::Text("Spool: %zu byte%s", nBytes, nBytes == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("— PR#%d from BASIC sends output here",
                        printerPanel.slot);
    if (printerPanel.spoolTruncated) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
            "Preview/save retains only the newest %zu bytes; older output "
            "was already streamed to the virtual printer.",
            PrinterCard::kMaxSpoolBytes);
    }

    ImGui::Separator();

    // Auto-suggest a timestamped path on first open so the user can hit
    // Save without typing anything. printerSavePath persists across saves
    // within a session — the user can edit it freely.
    if (printerSavePath.empty()) {
        const std::time_t t = std::time(nullptr);
        // localtime_r, not localtime: the latter hands back a shared static
        // `tm`, and ClockCard converts times of its own on the CPU thread (a
        // ProDOS timestamp under stateMutex). Same split as NoSlotClock.
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        printerSavePath = (pom2::userDataDir() / "printouts" /
                           (std::string("spool-") + stamp + ".txt")).string();
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", printerSavePath.c_str());
#ifdef __EMSCRIPTEN__
    // No persistent host filesystem in the browser build: a "Save as .txt"
    // would land in MEMFS and vanish on reload, with no download path. Grey
    // the controls out — the live spool preview below still works (PR#n
    // output is captured), and "Clear spool" still resets the buffer.
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-110);
    ImGui::InputText("##printerPath", buf, sizeof(buf));
    ImGui::SameLine();
    ImGui::Button("Save as .txt", ImVec2(100, 0));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Saving to a file is unavailable in the browser build.");
#else
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("##printerPath", buf, sizeof(buf))) {
        printerSavePath = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as .txt", ImVec2(100, 0))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::path(printerSavePath);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        // Unique per process AND per call, then scrutinised: a fixed
        // `<target>.pom2tmp` is the name every POM2 instance derives, and a
        // symlink or hard link planted there redirects this trunc onto
        // whatever it points at. Same discipline as every other write-back
        // (AtomicFileReplace.h) — this was the last caller skipping it.
        const fs::path tmp = pom2::tempSiblingPath(p);
        std::error_code prepEc;
        const bool tmpUsable = pom2::prepareTempPath(tmp, prepEc);
        std::error_code permEc;
        const fs::perms oldPerms = fs::status(p, permEc).permissions();
        const bool havePerms = !permEc;
        std::ofstream out;
        if (tmpUsable)
            out.open(tmp, std::ios::binary | std::ios::trunc);
        if (!tmpUsable) {
            printerLastSaveStatus = "Save failed: temp path unusable " +
                                    tmp.string() + ": " + prepEc.message();
        } else if (!out) {
            printerLastSaveStatus = "Save failed: cannot open " +
                                    p.string();
        } else {
            const std::string& text = printerPanel.spoolText;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            out.close();
            if (!out) {
                printerLastSaveStatus = "Save failed: short write to " +
                                        tmp.string();
                fs::remove(tmp, ec);
            } else {
                if (havePerms) {
                    fs::permissions(tmp, oldPerms, ec);
                    ec.clear();
                }
                if (!pom2::replaceFileAtomic(tmp, p, ec)) {
                    printerLastSaveStatus = "Save failed: cannot replace " +
                                            p.string() + ": " + ec.message();
                    fs::remove(tmp, ec);
                } else {
                    printerLastSaveStatus = "Saved " +
                        std::to_string(text.size()) + " bytes → " + p.string();
                }
            }
        }
    }

    if (!printerLastSaveStatus.empty()) {
        ImGui::TextDisabled("%s", printerLastSaveStatus.c_str());
    }
#endif

    if (ImGui::Button("Clear spool")) {
        printerCoordinator_->clearPrinterPanelSpool(*controller,
                                                    printerPanel.slot);
        printerLastSaveStatus.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(host-side buffer only — does NOT touch the Apple II)");

    ImGui::Separator();
    ImGui::TextDisabled("Preview (high bit stripped, CR → LF):");

    // Taken with the rest of the panel state, under one lock.
    const std::string& preview = printerPanel.spoolText;
    ImGui::BeginChild("##printerPreview", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (preview.empty()) {
        ImGui::TextDisabled("(empty — try `PR#%d : PRINT \"HELLO\"` from BASIC)",
                            printerPanel.slot);
    } else {
        ImGui::TextUnformatted(preview.data(),
                               preview.data() + preview.size());
    }
    ImGui::EndChild();

    ImGui::End();
}

SuperSerialCard* MainWindow::printerTapSsc() const
{
    // serialCards() is sorted by slot ascending, so the first tapped card is
    // the lowest slot — the //c printer port when that profile is active.
    for (auto* ssc : serialCards())
        if (ssc && ssc->printerTap()) return ssc;
    return nullptr;
}

void MainWindow::pumpPostScript(const std::vector<uint8_t>& fresh)
{
    // Ask the interpreter for exactly the sheet the tray holds, so the page
    // that comes back blits 1:1 with no scaling.
    // Alongside the printouts, not in the system temp dir: the files are
    // the user's own job, they are removed as soon as the render is done,
    // and a scratch path inside POM2's own data dir is one the user can
    // find if an interpreter ever leaves something behind.
    postScriptSpooler_.setScratchDir(
        (pom2::userDataDir() / "printouts" / "spool").string());
    postScriptSpooler_.setPageGeometry(imageWriter->currentPage().dpi,
                                       imageWriter->paperWidthIn(),
                                       imageWriter->paperLengthIn());

    if (!fresh.empty()) {
        postScriptSpooler_.feed(fresh.data(), fresh.size());
        postScriptIdle_ = 0.0;
    } else if (postScriptSpooler_.pendingBytes() > 0 &&
               !postScriptSpooler_.busy()) {
        // A driver that ends its job by simply stopping. A serial line has no
        // end-of-file to notice, so the idle IS the signal.
        postScriptIdle_ += static_cast<double>(ImGui::GetIO().DeltaTime);
        if (postScriptIdle_ >= kPostScriptIdleFlush) {
            postScriptIdle_ = 0.0;
            postScriptSpooler_.flushNow();
        }
    }

    pom2::PsRenderResult page;
    if (postScriptSpooler_.takeResult(page)) {
        if (page.ok) {
            imageWriter->adoptRenderedPage(page.gray.data(), page.w, page.h);
            // `showpage` is what finished this sheet, so eject it — that is
            // what puts it on the completed stack, into the PDF export and
            // into the print history, exactly like a form feed does for the
            // matrix heads.
            imageWriter->formFeed();
            // …and a job may have said `showpage` more than once. Every
            // further sheet the interpreter drew gets the same treatment,
            // in order, so a multi-page document arrives whole instead of
            // as its first page plus a note.
            for (const auto& extra : page.more) {
                imageWriter->adoptRenderedPage(extra.gray.data(),
                                               extra.w, extra.h);
                imageWriter->formFeed();
            }
            if (page.extraPages > 0) {
                tapeStatusMessage =
                    "LaserWriter: printed " +
                    std::to_string(page.extraPages + 1) + " pages";
                tapeStatusUntil = lastFrameTime + 6.0;
            }
        } else {
            tapeStatusMessage = "LaserWriter: " + page.error;
            tapeStatusUntil   = lastFrameTime + 8.0;
            pom2::log().warn("LaserWriter", page.error);
        }
    }

    // The mechanism still ticks: an ejected page has to reach the stack, and
    // the panel's own animations answer to it.
    imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));
}

void MainWindow::pumpImageWriter()
{
    // The printer is downstream of the interface card: every byte the card
    // spooled since the last frame is handed to the ImageWriter, in order.
    // Cheap enough to run unconditionally (the panel need not be open —
    // a printout should be waiting when the user opens the tray).
    if (!imageWriter) return;

    // Archive first, so this runs on EVERY path — the "no source this frame"
    // branch below returns early, and a job already inside the printer's
    // buffer keeps ejecting sheets after its card is unplugged. Running at
    // the top costs one frame of lag and never misses a page.
    archiveNewPrinterPages();

    // One call resolves every candidate source under the machine lock,
    // applies the physical priority (PrinterCard > Grappler+ > FujiNet unit >
    // SSC tap), advances the feed cursor across a source change or a cleared
    // spool, and hands back an OWNED byte batch. The cursor handover rules
    // still live in printerFeedCursor() (PrinterFeedCursor.h explains why
    // re-seating at 0 was wrong); they are now applied inside the coordinator
    // instead of being re-stated per source here. Pinned by testSpoolSeam.
    auto batch = printerCoordinator_->drainImageWriter(*controller);
    std::vector<uint8_t> fresh = std::move(batch.bytes);

    if (!batch.haveSource()) {
        // No source this frame (card unplugged, or the tap switched off).
        // The mechanism keeps running: a job already in the printer's input
        // buffer must still reach paper even with nothing feeding it —
        // unplugging the card does not un-print the page.
        //
        // Release BUSY on the way out: the release block at the bottom of
        // this function is the ONLY writer of the Grappler's BUSY line, and
        // an early return that skips it forever leaves a back-pressured
        // guest spinning on the ACK bit.
        printerCoordinator_->setGrapplerBusy(*controller, false);
        imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));
        return;
    }

    // The LaserWriter's PostScript mode forks the stream here rather than in
    // the printer: there is no parser to route to, the job goes to an
    // external interpreter, and the finished page comes back as a raster.
    if (imageWriter->modelProfile().lineage == pom2::IwLineage::PostScript) {
        // Same early-return rule as above: with BUSY latched by a long
        // dot-matrix job, switching the model to the LaserWriter took this
        // branch every frame and the release block below never ran — the
        // guest hung in the firmware's ACK poll ($CD89) forever. The
        // PostScript path has no input-buffer model to hold the line for.
        printerCoordinator_->setGrapplerBusy(*controller, false);
        pumpPostScript(fresh);
        return;
    }

    if (!fresh.empty()) {
        imageWriter->queueBytes(fresh.data(), fresh.size());
        if (imageWriter->tracing())
            imageWriter->traceEvent("card delivered %zu byte%s (queue now %zu)",
                                    fresh.size(), fresh.size() == 1 ? "" : "s",
                                    imageWriter->pendingBytes());
    }
    // Print what the mechanism had time for this frame. ImGui's DeltaTime
    // is the host frame time, which is what a real print head answers to
    // (the guest can be paused, turbo'd or rewound — the paper still
    // moves at 250 cps).
    imageWriter->tick(static_cast<double>(ImGui::GetIO().DeltaTime));

    // Report the printer's input buffer back up the cable. A stock
    // ImageWriter II buffers 2 KB and stops acknowledging bytes until the
    // head catches up; the Grappler firmware spins on that ACK bit before
    // every byte, so a guest printing a long job now waits for the paper
    // instead of blasting a whole page into a host queue.
    {
        // Only hold the line when the user asked for the real handshake:
        // a Print Shop page is ~100 KB of dot columns, which at 250 cps
        // keeps the guest blocked for minutes. Faithful, and
        // indistinguishable from a hang unless you know to expect it.
        const bool busy =
            printerBackPressure &&
            imageWriter->pendingBytes() > pom2::ImageWriter::kInputBufferBytes;
        const auto update = printerCoordinator_->setGrapplerBusy(*controller, busy);
        if (update.grapplerPlugged && update.changed) {
            if (imageWriter->tracing())
                imageWriter->traceEvent(
                    "BUSY=%d — the Apple II %s (queue %zu / %zu buffer)",
                    busy ? 1 : 0,
                    busy ? "is now waiting on the printer"
                         : "may send again",
                    imageWriter->pendingBytes(),
                    pom2::ImageWriter::kInputBufferBytes);
        }
    }
}

void MainWindow::renderImageWriterWindow()
{
    if (!show(pom2::PanelId::ImageWriter) || !imageWriter || !imageWriterPanel) return;

    pom2::ImageWriter_ImGui::HostInfo host;

    // There is one ImageWriter and up to three things that can feed it, so
    // pumpImageWriter() arbitrates: parallel cards outrank the SSC tap,
    // and PrinterCard outranks Grappler+. Slot Config lets a PrinterCard
    // and a Grappler+ coexist (different catalog keys, so its duplicate
    // check does not object), and the loser then feeds nothing at all —
    // silently, with paper that just stays blank. Name the losers.
    // The coordinator arbitrates and reports BOTH which source won and which
    // plugged sources are therefore feeding nothing — including the FujiNet
    // printer unit, which the hand-rolled list here never named, so a
    // FujiNet-only setup showed an unconnected printer instead of its source.
    const auto printerHost = printerCoordinator_->captureHost(*controller);
    const std::vector<std::string>& ignored = printerHost.ignoredSources;

    using SourceKind = pom2::PrinterCoordinator::SourceKind;
    host.haveSource = printerHost.source != SourceKind::None;
    switch (printerHost.source) {
        case SourceKind::PrinterCard:
            host.sourceLabel = "fed by Printer card, slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case SourceKind::Grappler:
            host.sourceLabel = "fed by Grappler+, slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case SourceKind::FujiNet:
            host.sourceLabel = "fed by FujiNet printer unit, slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case SourceKind::SuperSerial:
            host.sourceLabel = "fed by Super Serial (printer port), slot " +
                               std::to_string(printerHost.sourceSlot);
            break;
        case SourceKind::None:
            break;
    }

    if (printerHost.grapplerPlugged) {
        // The Grappler's S1 printer-type switches decide which dialect of
        // escape codes its firmware emits — an Epson-configured card feeds
        // this ImageWriter Epson graphics commands, which come out as
        // noise. Surface it where the damage shows up.
        using PT = GrapplerCard::PrinterType;
        for (int i = 0; i <= 6; ++i) {
            const auto t = static_cast<PT>(i);
            host.cardDipOptions.push_back(
                { GrapplerCard::printerTypeName(t), i });
        }
        host.cardDipValue = printerHost.grapplerPrinterType;
        host.cardDipRecommended = static_cast<int>(PT::AppleDotMatrix);
        host.backPressure = printerBackPressure;
        host.onBackPressureChanged =
            [this](bool v) { printerBackPressure = v; };
        // Re-resolves the card under the lock before writing: this callback
        // runs later in the frame, and the slot can have been rebuilt.
        host.onCardDipChanged = [this](int v) {
            printerCoordinator_->setGrapplerPrinterType(*controller, v);
        };
    }

    if (host.haveSource && !ignored.empty()) {
        host.sourceLabel += "  (not feeding: ";
        for (size_t i = 0; i < ignored.size(); ++i)
            host.sourceLabel += (i ? ", " : "") + ignored[i];
        host.sourceLabel += ")";
    }
    host.saveDir = (pom2::userDataDir() / "printouts").string();

    // ── Print history (printer plan phase E) ─────────────────────────────
    // The panel lists and asks; the store stays here. Rebuilt each frame,
    // which is cheap — it is a few dozen rows of already-loaded metadata.
    if (printerHistory && printerHistory->isOpen()) {
        std::string writeErr;
        if (!printerHistory->pollWriteFailures(writeErr))
            pom2::log().warn("PrinterHistory", writeErr);
        host.historyDir = printerHistory->dir();
        host.history.reserve(printerHistory->size());
        for (const auto& p : printerHistory->pages()) {
            pom2::ImageWriter_ImGui::HostInfo::HistoryRow row;
            row.file    = p.file;
            row.savedAt = p.savedAt;
            row.printer = pom2::ImageWriter::modelName(
                static_cast<pom2::IwModel>(p.model));
            row.ribbon  = pom2::ImageWriter::ribbonName(
                static_cast<pom2::ImageWriter::Ribbon>(p.ribbon));
            row.job     = p.job;
            row.w       = p.w;
            row.h       = p.h;
            row.paperW  = p.paperW;
            row.paperL  = p.paperL;
            host.history.push_back(std::move(row));
        }
        host.loadHistoryPage = [this](const std::string& file,
                                      std::vector<uint8_t>& rgba,
                                      int& w, int& h) {
            for (const auto& p : printerHistory->pages()) {
                if (p.file != file) continue;
                std::string err;
                return printerHistory->loadRgba(p, rgba, w, h, err);
            }
            return false;
        };
        host.onDeleteHistoryPage = [this](const std::string& file) {
            for (const auto& p : printerHistory->pages()) {
                if (p.file != file) continue;
                std::string err;
                if (!printerHistory->erase(p, err))
                    pom2::log().warn("PrinterHistory", err);
                return;
            }
        };
        host.onClearHistory = [this]() {
            std::string err;
            if (!printerHistory->clear(err))
                pom2::log().warn("PrinterHistory", err);
        };
    }
#ifdef __EMSCRIPTEN__
    host.canSaveFiles = false;   // MEMFS writes vanish on reload
#endif

    imageWriterPanel->render(&show(pom2::PanelId::ImageWriter), *imageWriter, host);
}

void MainWindow::renderAiControlPanelWindow()
{
    if (!show(pom2::PanelId::AiControl)) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Control (HTTP)", &show(pom2::PanelId::AiControl))) {
        ImGui::End();
        return;
    }

    const bool running = aiServer->isRunning();
    ImGui::Text("Status: %s", running ? "listening" : "stopped");
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("on 127.0.0.1:%u", aiServer->getPort());
    }
    ImGui::Text("Requests served: %llu",
                static_cast<unsigned long long>(aiServer->requestsServed()));
    {
        const std::string addr = aiServer->lastClientAddr();
        if (!addr.empty()) ImGui::Text("Last client: %s", addr.c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("TCP port", &aiPortInput, 0, 0);
    if (aiPortInput < 1)     aiPortInput = 1;
    if (aiPortInput > 65535) aiPortInput = 65535;

    char tokenBuf[128];
    std::snprintf(tokenBuf, sizeof(tokenBuf), "%s", aiTokenInput.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Auth token (empty = open)", tokenBuf, sizeof(tokenBuf))) {
        aiTokenInput = tokenBuf;
        aiServer->setAuthToken(aiTokenInput);
    }

    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button("Start")) {
            // Re-attach in case slot cards were rebuilt by the slot config
            // panel since the last start — pointers may have moved.
            aiServer->attach(controller.get(), display.get(), primaryDiskII(), primaryHdvCard());
            aiServer->setAuthToken(aiTokenInput);
            if (!aiServer->start(static_cast<uint16_t>(aiPortInput))) {
                tapeStatusMessage = "AI Control: bind failed (port busy?)";
                tapeStatusUntil   = lastFrameTime + 4.0;
            }
        }
    } else {
        if (ImGui::Button("Stop")) aiServer->stop();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Drive POM2 from an AI agent (or curl/Postman) via HTTP/1.1.");
    ImGui::Spacing();
    ImGui::TextDisabled("Example:");
    ImGui::TextWrapped("  curl http://127.0.0.1:%d/status", aiPortInput);
    ImGui::TextWrapped(
        "  curl -d '{\"text\":\"PRINT 1+1\\r\"}' http://127.0.0.1:%d/keyboard",
        aiPortInput);
    ImGui::TextWrapped(
        "  curl http://127.0.0.1:%d/screen.ppm -o screen.ppm",
        aiPortInput);
    if (!aiTokenInput.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Send 'X-POM2-Token: <token>' header on each request.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Endpoints: /status /reset /cpu /mem /keyboard /disk "
                        "/eject /snapshot/save /snapshot/load /speed /screen.ppm");

    ImGui::End();
}
bool MainWindow::plugFujiNetFromCli(int& slot, bool slotExplicit, bool serial,
                                    const std::string& serialDevice,
                                    int tcpPort, std::string& errOut)
{
    if (slot < 1 || slot > 7) { errOut = "slot must be 1-7"; return false; }
    if (pom2::profileConfig(activeProfile).noPhysicalSlots) {
        errOut = "the active //c-class profile has no physical expansion slots";
        return false;
    }

    {
    auto  st  = controller->lockState();
    auto& bus = st.memory().slotBus();

    if (bus.peripheral(slot) != nullptr && !slotExplicit) {
        // The user never named a slot — 7 is only POM2's preference, and its
        // own first-run default puts a Le Chat Mauve there, so refusing here
        // would make the documented bare `--fujinet` fail on a stock install.
        // Fall back the way docs/fujinet_plan.md specifies. Downwards from 7:
        // the autostart scan walks slots high to low, so the highest free slot
        // is the one most likely to be reached before the Disk II in 6.
        int free = 0;
        for (int s = 7; s >= 1 && free == 0; --s)
            if (bus.peripheral(s) == nullptr) free = s;
        if (free == 0) {
            errOut = "every slot is occupied — free one, or name it with "
                     "--fujinet-slot";
            return false;
        }
        pom2::log().info("CLI", "--fujinet: slot " + std::to_string(slot) +
                                    " holds " +
                                    std::string(bus.peripheral(slot)->name()) +
                                    ", using free slot " + std::to_string(free));
        slot = free;
    }

    if (bus.peripheral(slot) != nullptr) {
        errOut = "slot " + std::to_string(slot) + " already holds " +
                 std::string(bus.peripheral(slot)->name()) +
                 " — pick a free slot with --fujinet-slot";
        return false;
    }

    // Plugged with its transport CLOSED, then opened below with the lock
    // released: a TCP listen or a tty open under `stateMutex` blocks the CPU
    // worker and the window for as long as the host takes to answer.
    if (!plugFujiNetUnlocked(st, slot, serial, serialDevice, tcpPort, errOut,
                             /*startNow=*/false))
        return false;
    }   // release the state lock before touching the network

    pendingFujiNetSlots_.push_back(slot);
    if (!startDeferredFujiNetLinks(&errOut)) {
        // The CLI reports a transport it could not open as a failure, and did
        // so before the card existed. Undo the plug so the two answers agree.
        auto st = controller->lockState();
        (void)st.memory().slotBus().unplug(slot);
        return false;
    }

    // Remember it so every later slot rebuild reproduces it — see the header.
    cliFujiNetSlot_       = slot;
    cliFujiNetSerial_     = serial;
    cliFujiNetSerialPath_ = serialDevice;
    cliFujiNetPort_       = tcpPort;
    return true;
}

bool MainWindow::plugFujiNetUnlocked(const pom2::StateAccess& st,
                                     int slot, bool serial,
                                     const std::string& serialDevice,
                                     int tcpPort, std::string& errOut,
                                     bool startNow)
{
    auto& bus = st.memory().slotBus();
    auto card = pom2::makeFujiNetCard(slot);
    card->setMemory(&st.memory());
    card->setCpu(&st.cpu());
    auto& link = card->transportLink();
    if (serial)
        link.setSerialMode(serialDevice, pom2::SerialPort::kDefaultBaud);
    else
        link.setTcpMode(static_cast<uint16_t>(tcpPort));

    // Only the CLI path starts the transport here, and it does so with the
    // lock NOT yet taken (see plugFujiNetFromCli); the rebuild path passes
    // false and opens it afterwards, because a listen()/open(tty) under
    // `stateMutex` stalls the CPU worker and the window for as long as the
    // host takes to answer.
    if (startNow) {
        std::string err;
        if (!link.start(err)) { errOut = err; return false; }
    }

    bus.plug(slot, std::move(card));
    // Session-only (CLI --fujinet / drag-and-drop): the live bus shows it,
    // the plan does not claim it.
    return true;
}

bool MainWindow::startDeferredFujiNetLinks(std::string* errOut)
{
    if (pendingFujiNetSlots_.empty()) return true;
    std::vector<int> slots;
    slots.swap(pendingFujiNetSlots_);
    bool allStarted = true;
    for (int slot : slots) {
        // Topology read: every writer of the SlotBus is this thread, and the
        // CPU worker is stopped across every rebuild that queues one of
        // these, so the pointer stays valid for the start() below — which is
        // the whole point of doing it out here.
        auto* card = dynamic_cast<pom2::FujiNetCard*>(
            controller->memory().slotBus().peripheral(slot));
        if (!card) continue;
        std::string err;
        if (card->transportLink().start(err)) continue;
        allStarted = false;
        if (errOut && errOut->empty()) *errOut = err;
        pom2::log().warn("FujiNet", "link not started: " + err);
    }
    return allStarted;
}

void MainWindow::archiveNewPrinterPages()
{
    if (!imageWriter || !printerHistory || !printerHistory->isOpen()) return;

    const uint64_t ejected = static_cast<uint64_t>(imageWriter->sheetsEjected());
    if (ejected <= printerArchivedSheets_) return;

    // How many of those sheets are still reachable. The stack is capped, so a
    // burst of form feeds between two frames can push pages off it before we
    // ever see them — archive what is there and resynchronise rather than
    // pretending we captured everything.
    const uint64_t missed = ejected - printerArchivedSheets_;
    const size_t   have   = imageWriter->completedPageCount();
    const size_t   take   = static_cast<size_t>(std::min<uint64_t>(missed, have));
    const uint64_t irrecoverablyDropped = missed - take;
    size_t accepted = 0;

    for (size_t i = have - take; i < have; ++i) {
        std::string err;
        if (!printerHistory->addPage(imageWriter->completedPage(i),
                                     static_cast<int>(imageWriter->model()),
                                     static_cast<int>(imageWriter->ribbon()),
                                     imageWriter->paperWidthIn(),
                                     imageWriter->paperLengthIn(), err)) {
            pom2::log().warn("PrinterHistory", err);
            break;
        }
        ++accepted;
    }
    if (take < missed) {
        pom2::log().warn("PrinterHistory",
            "archived " + std::to_string(take) + " of " +
            std::to_string(missed) + " ejected sheets — the rest had already "
            "fallen off the printer's page stack");
    }
    // Advance only past sheets actually archived (plus sheets already fallen
    // off the bounded live stack). A transient index failure is retried next
    // frame instead of silently discarding the failed page and the rest of
    // the batch.
    printerArchivedSheets_ += irrecoverablyDropped + accepted;
}

void MainWindow::dumpScreenToPrinter()
{
    if (!imageWriter) return;

    // Snapshot the framebuffer under BOTH locks, exactly as saveScreenshot
    // does — `pixels()` can lazily run the composite demod, which writes
    // frame80 and races the AI control server's /screen handler otherwise.
    // Lock order stateMutex → demodMutex, never the other way.
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        w = display->width();
        h = display->height();
        if (w > 0 && h > 0) {
            const uint32_t* src = display->pixels();
            px.assign(src, src + static_cast<size_t>(w) * h);
        }
    }
    if (px.empty()) return;

    // The dump goes in as BYTES, through the printer's own ESC G parser —
    // never painted onto the page. So it obeys the ribbon, the pacing and the
    // paper, lands in the tray and the PDF export, and cannot drift from what
    // a period driver would have produced. See PrinterScreenDump.h.
    // Each head has its own graphics grammar, so the dump has to be built
    // for the one actually fitted — the Epson wants ESC * with a binary count
    // and bit 7 as the top dot, the C. Itoh family ESC G with four ASCII
    // digits and bit 0.
    std::vector<uint8_t> stream;
    const pom2::IwModelProfile& head = imageWriter->modelProfile();
    if (head.lineage == pom2::IwLineage::EscP) {
        // ...and within the Epson lineage, `ESC *` is FX-generation. An
        // MX-80 has only `ESC K`, and would print an `ESC *` dump's data
        // bytes as text.
        const auto cmd = (head.escPFeatures & pom2::kEscPGraphicsStar)
                             ? pom2::EpsonDumpCmd::Star72
                             : pom2::EpsonDumpCmd::K60;
        pom2::buildScreenDumpEpson(px.data(), w, h, w,
                                   printerDumpOptions_, stream, cmd);
    } else
        pom2::buildScreenDumpImageWriter(px.data(), w, h, w,
                                         printerDumpOptions_, stream);
    if (stream.empty()) return;

    imageWriter->queueBytes(stream.data(), stream.size());
    show(pom2::PanelId::ImageWriter) = true;  // the user asked to print; show the paper
}

void MainWindow::renderFujiNetPanelWindow()
{
    if (!show(pom2::PanelId::FujiNet)) return;

    // One acquisition for the snapshot, one for whatever the frame asked for.
    // What this replaces bound `auto& link = card->transportLink()` OUTSIDE
    // any lock and then wrote through that reference inside SIX separate
    // critical sections — a slot rebuild between any two of them left the rest
    // writing to a freed link — and applied the timeout change with no lock at
    // all.
    auto snap = networkCoordinator_->captureFujiNetPanel(*controller);

    // Both of these take the machine lock themselves, so they must stay
    // outside any guard: stateMutex is non-recursive.
    //
    // Outranked iff the arbitration picked something OTHER than this tap.
    snap.printerOutranked =
        snap.printerTap &&
        printerCoordinator_->captureHost(*controller).source !=
            pom2::PrinterCoordinator::SourceKind::FujiNet;
    snap.hostClockCard =
        devicePanelCoordinator_->captureInventory().clockPlugged();

    const auto r = fujiNetPanel->render("FujiNet", show(pom2::PanelId::FujiNet), snap);
    if (!snap.plugged) return;

    // The web-UI button has no portable "open a URL" helper in POM2, so the
    // coordinator surfaces the address on the status line instead.
    networkCoordinator_->applyFujiNetPanel(*controller, r);
}
