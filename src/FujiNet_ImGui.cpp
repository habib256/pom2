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

#include "FujiNet_ImGui.h"

#include "IconsFontAwesome6.h"
#include "SpOverSlipLink.h"   // SpDeviceType — the DIB type bytes
#include "StatusLed.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace pom2 {

namespace {

std::string fmtBlocks(uint32_t blocks)
{
    if (blocks == 0) return "—";                 // character device (N:, …)
    char buf[32];
    const double kb = static_cast<double>(blocks) * 0.5;   // 512 B blocks
    if (kb < 1024.0) std::snprintf(buf, sizeof(buf), "%.0f KB", kb);
    else             std::snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0);
    return buf;
}

std::string fmtBytes(uint64_t n)
{
    char buf[32];
    if (n < 1024)             std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    else                      std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    return buf;
}

/// DIB device-type byte. These are FujiNet's OWN allocations from its $1x
/// range (firmware `lib/bus/iwm/iwm.h`, SP_TYPE_BYTE_*), not the generic
/// Apple IIgs SmartPort type list. Anything unknown shows as its number.
const char* deviceTypeName(uint8_t type)
{
    switch (type) {
    case pom2::kSpType35Disk:   return "3.5\" disk";
    case pom2::kSpTypeHardDisk: return "hard disk";
    case pom2::kSpTypeScsi:     return "SCSI";
    case pom2::kSpTypeFuji:     return "FujiNet";
    case pom2::kSpTypeNetwork:  return "network (N:)";
    case pom2::kSpTypeCpm:      return "CP/M";
    case pom2::kSpTypeClock:    return "clock";
    case pom2::kSpTypePrinter:  return "printer";
    case pom2::kSpTypeModem:    return "modem";
    default:                    return "";
    }
}

const int kBauds[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };

} // anon namespace

FujiNet_ImGui::Result FujiNet_ImGui::render(const char* title, bool& open,
                                            const Snapshot& snap)
{
    Result r;
    if (!open) return r;

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) { ImGui::End(); return r; }

    if (!snap.plugged) {
        ImGui::TextWrapped(
            "No FujiNet card is plugged. Add \"FujiNet (SP over SLIP)\" to a "
            "slot in Slot Configuration — slot 7 is the usual choice, because "
            "the autostart scan reaches it before the Disk II in slot 6, so "
            "the machine boots straight into FujiNet's CONFIG.");
        ImGui::End();
        return r;
    }

    // ── Link state ────────────────────────────────────────────────────────
    ImGui::Text("Slot %d", snap.slot);
    ImGui::SameLine();
    statusLed(snap.connected ? MediaStatus::Ok
              : snap.running ? MediaStatus::WriteProtected  // amber: waiting
                             : MediaStatus::Empty,
              snap.connected ? "FujiNet attached"
              : snap.running ? "listening — no FujiNet yet"
                             : "link stopped");
    ImGui::TextUnformatted(snap.state.c_str());

    if (!snap.lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", snap.lastError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // ── Transport ─────────────────────────────────────────────────────────
    // TCP or serial, never both: two peers would mean two SmartPort device
    // number spaces to merge, which no real configuration needs.
    int mode = snap.transport == Transport::Serial ? 1
             : snap.transport == Transport::Tcp    ? 0 : 2;
    ImGui::TextUnformatted("Transport");
    if (ImGui::RadioButton("TCP (FujiNet desktop)", &mode, 0)) {
        r.transportChanged = true; r.transportTo = Transport::Tcp;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Serial (FujiNet board)", &mode, 1)) {
        r.transportChanged = true; r.transportTo = Transport::Serial;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Off", &mode, 2)) {
        r.transportChanged = true; r.transportTo = Transport::Off;
    }

    if (snap.transport == Transport::Tcp) {
        int port = snap.tcpPort;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Listen port", &port, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (port > 0 && port < 65536) {
                r.portChanged = true;
                r.portTo = static_cast<uint16_t>(port);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "POM2 listens on 127.0.0.1; the FujiNet desktop build "
                "connects in. 1985 is the port the FujiNet project uses, so an "
                "existing configuration works unchanged.");
    } else if (snap.transport == Transport::Serial) {
        if (!serialPathPrimed_) {
            std::snprintf(serialPathBuf_.data(), serialPathBuf_.size(), "%s",
                          snap.serialPath.c_str());
            serialPathPrimed_ = true;
        }
        ImGui::SetNextItemWidth(280);
        if (ImGui::InputText("Device", serialPathBuf_.data(), serialPathBuf_.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            r.serialChanged = true;
            r.serialPathTo  = serialPathBuf_.data();
            r.serialBaudTo  = snap.serialBaud;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE "##rescan")) r.requestRescan = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan for serial devices");

        if (!snap.serialDevices.empty()) {
            ImGui::SetNextItemWidth(280);
            if (ImGui::BeginCombo("Detected", "select…")) {
                for (const auto& d : snap.serialDevices) {
                    if (ImGui::Selectable(d.second.c_str())) {
                        std::snprintf(serialPathBuf_.data(), serialPathBuf_.size(),
                                      "%s", d.first.c_str());
                        r.serialChanged = true;
                        r.serialPathTo  = d.first;
                        r.serialBaudTo  = snap.serialBaud;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("No serial device detected.");
        }

        int baudIdx = 4;   // 115200
        for (int i = 0; i < static_cast<int>(sizeof(kBauds) / sizeof(kBauds[0])); ++i)
            if (kBauds[i] == snap.serialBaud) baudIdx = i;
        char baudLabel[16];
        std::snprintf(baudLabel, sizeof(baudLabel), "%d", kBauds[baudIdx]);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("Baud", baudLabel)) {
            for (int i = 0; i < static_cast<int>(sizeof(kBauds) / sizeof(kBauds[0])); ++i) {
                char lbl[16];
                std::snprintf(lbl, sizeof(lbl), "%d", kBauds[i]);
                if (ImGui::Selectable(lbl, i == baudIdx)) {
                    r.serialChanged = true;
                    r.serialPathTo  = serialPathBuf_.data();
                    r.serialBaudTo  = kBauds[i];
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Ignored by a native USB CDC device, but a board with an "
                "FTDI/CP210x bridge in front of the ESP32 does care.");
    }

    ImGui::Separator();

    // ── Timing ────────────────────────────────────────────────────────────
    int timeout = snap.timeoutMs;
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Timeout (ms)", &timeout, 50, 5000)) {
        r.timeoutChanged = true;
        r.timeoutTo      = timeout;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The emulated 6502 is parked inside its JSR for the whole round "
            "trip, so this is how long the machine stalls when the FujiNet "
            "stops answering. Loopback replies in microseconds and a real "
            "board in a few milliseconds; 250 ms leaves ample headroom.");

    // ── Controls ──────────────────────────────────────────────────────────
    if (snap.running) {
        if (ImGui::Button(ICON_FA_STOP " Stop")) r.requestStop = true;
    } else {
        if (ImGui::Button(ICON_FA_PLAY " Start")) r.requestStart = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!snap.connected);
    if (ImGui::Button("Disconnect peer")) r.requestDropPeer = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Only meaningful over TCP: a serial link has no IP to point a browser at.
    ImGui::BeginDisabled(snap.transport != Transport::Tcp || !snap.connected);
    if (ImGui::Button("Web UI")) r.requestOpenWebUi = true;
    ImGui::EndDisabled();
    // The button is disabled over serial, which is precisely the case this
    // tooltip explains — so it has to be readable while disabled.
    if (snap.transport == Transport::Serial &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The FujiNet web UI needs an IP; over serial there "
                          "is none. Use the board's own WiFi address.");

    ImGui::Separator();

    // ── Helper process ────────────────────────────────────────────────────
    // Only meaningful over TCP: a serial FujiNet is a board on a cable, and
    // there is nothing on the host to launch.
    if (snap.transport == Transport::Tcp) {
        ImGui::TextUnformatted("FujiNet program");
        if (!helperPathPrimed_) {
            std::snprintf(helperPathBuf_.data(), helperPathBuf_.size(), "%s",
                          snap.helperPath.c_str());
            helperPathPrimed_ = true;
        }
        ImGui::SetNextItemWidth(320);
        if (ImGui::InputTextWithHint("##helperPath",
                                     snap.helperResolved.empty()
                                         ? "path to a FujiNet desktop build"
                                         : snap.helperResolved.c_str(),
                                     helperPathBuf_.data(), helperPathBuf_.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            r.helperPathChanged = true;
            r.helperPathTo      = helperPathBuf_.data();
        }
        ImGui::SameLine();
        if (snap.helperRunning) {
            if (ImGui::Button("Stop program")) r.requestHelperStop = true;
            ImGui::SameLine();
            if (ImGui::Button("Restart program")) r.requestHelperRestart = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Stop it and start it again.\n"
                    "A FujiNet peer can die mid-session through no fault of "
                    "POM2's — the firmware aborts on some device calls — and "
                    "the guest then reports whatever it was doing rather than "
                    "the truth. Watch the log for\n"
                    "  peer LOST after N s — M call(s) served\n"
                    "and press this.");
        } else {
            ImGui::BeginDisabled(snap.helperPath.empty() &&
                                 snap.helperResolved.empty());
            if (ImGui::Button("Start program")) r.requestHelperStart = true;
            ImGui::EndDisabled();
        }
        if (snap.helperRunning) {
            ImGui::TextDisabled("Running — POM2 stops it when it quits.");
        } else if (snap.helperExitCode > 0) {
            ImGui::TextDisabled("Last run exited with code %d.", snap.helperExitCode);
        } else if (snap.helperPath.empty() && snap.helperResolved.empty()) {
            ImGui::TextDisabled("None found on PATH. Leave empty to auto-detect "
                                "a program named 'fujinet'.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "POM2 does not touch the program's fnconfig.ini — it holds "
                "your WiFi credentials, and its Apple default for Bus-over-IP "
                "is already 127.0.0.1:1985.");
        ImGui::Separator();
    }

    // ── Devices ───────────────────────────────────────────────────────────
    ImGui::Text("SmartPort devices (%d)", static_cast<int>(snap.devices.size()));
    if (snap.devices.empty()) {
        ImGui::TextDisabled(snap.connected
                                ? "Peer attached but reported no devices."
                                : "None — no FujiNet attached.");
    } else if (ImGui::BeginTable("fnDevices", 4,
                                 ImGuiTableFlags_Borders |
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (const auto& d : snap.devices) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", d.unit);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.empty() ? "(unnamed)" : d.name.c_str());
            ImGui::TableNextColumn();
            if (const char* t = deviceTypeName(d.type); t[0])
                ImGui::TextUnformatted(t);
            else
                ImGui::Text("$%02X", d.type);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtBlocks(d.blocks).c_str());
        }
        ImGui::EndTable();
    }

    // ── Host-side integration hints ───────────────────────────────────────
    if (snap.printerTap) {
        if (snap.printerOutranked) {
            ImGui::TextDisabled(
                "Printer unit present, but a parallel printer card outranks it "
                "— POM2's paper shows that card's output, not FujiNet's.");
        } else {
            ImGui::Text("Printer → POM2 paper tray (%s so far)",
                        fmtBytes(snap.printerBytes).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Bytes the guest writes to FujiNet's printer unit are also "
                    "rendered by POM2's ImageWriter. The FujiNet still prints "
                    "its own copy — this is a tap, not a diversion.");
        }
    }
    if (snap.hostClockCard) {
        ImGui::TextDisabled("Note: a ProDOS clock card is also plugged — the "
                            "guest sees two clocks.");
    }

    // ── Counters ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Statistics")) {
        ImGui::Text("Calls: %llu  (answered locally: %llu)",
                    (unsigned long long)snap.calls,
                    (unsigned long long)snap.localCalls);
        ImGui::Text("Timeouts: %llu   Stale responses discarded: %llu",
                    (unsigned long long)snap.timeouts,
                    (unsigned long long)snap.stale);
        ImGui::Text("Sent: %s   Received: %s",
                    fmtBytes(snap.bytesOut).c_str(),
                    fmtBytes(snap.bytesIn).c_str());
        if (snap.stale && ImGui::IsItemHovered())
            ImGui::SetTooltip("A discarded stale response is normal after a "
                              "guest reset — it is the answer to a request "
                              "the machine no longer remembers making.");
    }

    ImGui::End();
    return r;
}

} // namespace pom2
