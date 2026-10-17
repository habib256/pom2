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

// Uthernet_ImGui — see the header. Pure rendering: no card pointers, no
// locking, no side effects beyond the returned FrameResult.

#include "Uthernet_ImGui.h"

#include "imgui.h"

#include <cstdio>

namespace pom2 {
namespace {

const ImVec4 kGreen = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kAmber = ImVec4(1.00f, 0.65f, 0.20f, 1.0f);
const ImVec4 kGrey  = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);

void backendLine(const char* backend, bool valid)
{
    ImGui::TextUnformatted("Host transport:");
    ImGui::SameLine();
    ImGui::TextColored(valid ? kGreen : kAmber, "%s", backend);
}

} // namespace

std::string formatMac(const std::array<uint8_t, 6>& mac)
{
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

std::string formatIpv4(uint32_t networkOrder)
{
    const auto* b = reinterpret_cast<const uint8_t*>(&networkOrder);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

const char* w5100StatusName(uint8_t status)
{
    switch (status) {
    case kW5100SnSrClosed:      return "CLOSED";
    case kW5100SnSrInit:        return "INIT";
    case kW5100SnSrListen:      return "LISTEN";
    case kW5100SnSrSynSent:     return "SYNSENT";
    case kW5100SnSrEstablished: return "ESTABLISHED";
    case kW5100SnSrUdp:         return "UDP";
    case kW5100SnSrIpRaw:       return "IPRAW";
    case kW5100SnSrMacRaw:      return "MACRAW";
    default:                    return "?";
    }
}

const char* w5100ProtocolName(uint8_t mode)
{
    switch (mode & kW5100SnMrProtoMask & ~kW5100SnVirtualDns) {
    case kW5100SnMrClosed: return "closed";
    case kW5100SnMrTcp:    return "TCP";
    case kW5100SnMrUdp:    return "UDP";
    case kW5100SnMrIpRaw:  return "IPRAW";
    case kW5100SnMrMacRaw: return "MACRAW";
    default:               return "?";
    }
}

Uthernet_ImGui::FrameResult
Uthernet_ImGui::render(const char* title, bool& open, const Snapshot& snap)
{
    FrameResult result;

    ImGui::SetNextWindowPos (ImVec2(700, 60),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(660, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return result;
    }

    if (!snap.u1Plugged && !snap.u2Plugged) {
        ImGui::TextDisabled(
            "No Ethernet card plugged. Use Hardware → Slot Configuration to "
            "assign\n\"Uthernet I (CS8900A)\" or \"Uthernet II (W5100)\" to a slot.");
        ImGui::Separator();
        ImGui::TextDisabled(
            "Uthernet II is the one to pick for IRC / telnet / FTP: its W5100 is a\n"
            "hardware TCP/IP stack, so POM2 runs it on ordinary host sockets — no\n"
            "libslirp, no root. Uthernet I is a raw NIC and needs libslirp.");
        ImGui::End();
        return result;
    }

    if (ImGui::BeginTabBar("##ethernet_tabs")) {

        // ── Uthernet II ───────────────────────────────────────────────
        if (snap.u2Plugged && ImGui::BeginTabItem("Uthernet II (W5100)")) {
            ImGui::Text("Slot %d", snap.u2Slot);
            ImGui::Text("MAC %s   IP %s",
                        formatMac(snap.u2Mac).c_str(),
                        formatIpv4(snap.u2Ip).c_str());

            // TCP/UDP never touch the backend, so a "none" transport here
            // is not the failure it looks like. Say so.
            ImGui::TextColored(kGreen, "TCP / UDP: host sockets (no backend needed)");
            ImGui::TextUnformatted("MACRAW / IPRAW:");
            ImGui::SameLine();
            ImGui::TextColored(snap.u2BackendValid ? kGreen : kAmber,
                               "%s", snap.u2Backend.c_str());

            ImGui::Text("Traffic: %llu bytes out, %llu in",
                        static_cast<unsigned long long>(snap.u2BytesSent),
                        static_cast<unsigned long long>(snap.u2BytesReceived));

            bool virtualDns = snap.u2VirtualDns;
            if (ImGui::Checkbox("Virtual DNS", &virtualDns)) {
                result.requestVirtualDns = true;
                result.virtualDnsTo = virtualDns;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Emulator extension (not on real silicon): a socket opened with\n"
                    "protocol bit 3 set takes a hostname instead of an IP address.\n"
                    "Software detects it by reading PTIMER as 0. Turning this off\n"
                    "makes the card look like a stock W5100.");
            }

            ImGui::Separator();
            ImGui::TextDisabled("Sockets");

            if (ImGui::BeginTable("##w5100_sockets", 7,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 24.0f);
                ImGui::TableSetupColumn("Proto",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Local",  ImGuiTableColumnFlags_WidthFixed, 56.0f);
                ImGui::TableSetupColumn("Remote");
                ImGui::TableSetupColumn("RX");
                ImGui::TableSetupColumn("TX");
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < snap.u2Sockets.size(); ++i) {
                    const W5100Device::SocketInfo& s = snap.u2Sockets[i];
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", i);

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(w5100ProtocolName(s.mode));

                    ImGui::TableNextColumn();
                    const bool live = s.status != kW5100SnSrClosed;
                    ImGui::TextColored(live ? kGreen : kGrey, "%s",
                                       w5100StatusName(s.status));

                    ImGui::TableNextColumn();
                    if (s.localPort) ImGui::Text("%u", s.localPort);
                    else             ImGui::TextDisabled("—");

                    ImGui::TableNextColumn();
                    if (s.remoteIp || s.remotePort) {
                        ImGui::Text("%s:%u", formatIpv4(s.remoteIp).c_str(),
                                    s.remotePort);
                    } else {
                        ImGui::TextDisabled("—");
                    }

                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", s.rxPending, s.rxCapacity);

                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", s.txPending, s.txCapacity);
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            if (ImGui::Button("Reset card")) result.requestResetU2 = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(closes every socket, clears both rings)");

            ImGui::EndTabItem();
        }

        // ── Uthernet I ────────────────────────────────────────────────
        if (snap.u1Plugged && ImGui::BeginTabItem("Uthernet I (CS8900A)")) {
            ImGui::Text("Slot %d", snap.u1Slot);
            ImGui::Text("MAC %s", formatMac(snap.u1Mac).c_str());
            backendLine(snap.u1Backend.c_str(), snap.u1BackendValid);

            if (!snap.u1BackendValid) {
                ImGui::TextColored(kAmber,
                    snap.slirpCompiledIn
                        ? "This card carries no TCP/IP stack of its own — without a\n"
                          "host transport it will never see a frame. Set the backend\n"
                          "to \"slirp\" and re-plug the card."
                        : "libslirp is not compiled in, so there is no way to bridge\n"
                          "raw Ethernet. Install libslirp-dev and rebuild, or use the\n"
                          "Uthernet II, whose TCP/UDP works without it.");
            }

            ImGui::Separator();
            ImGui::TextColored(snap.u1RxEnabled ? kGreen : kGrey,
                               "Receiver: %s", snap.u1RxEnabled ? "enabled" : "off");
            ImGui::SameLine(220.0f);
            ImGui::TextColored(snap.u1TxEnabled ? kGreen : kGrey,
                               "Transmitter: %s", snap.u1TxEnabled ? "enabled" : "off");
            ImGui::Text("Promiscuous: %s", snap.u1Promiscuous ? "yes" : "no");
            ImGui::Text("PacketPage pointer: $%04X%s",
                        snap.u1PacketPagePtr,
                        (snap.u1PacketPagePtr & 0x8000) ? "  (auto-increment)" : "");

            ImGui::Separator();
            ImGui::Text("Frames sent:      %llu",
                        static_cast<unsigned long long>(snap.u1FramesSent));
            ImGui::Text("Frames received:  %llu",
                        static_cast<unsigned long long>(snap.u1FramesReceived));
            ImGui::Text("Frames filtered:  %llu",
                        static_cast<unsigned long long>(snap.u1FramesFiltered));
            ImGui::Text("Queued for guest: %zu", snap.u1Queued);

            ImGui::Separator();
            if (ImGui::Button("Reset card")) result.requestResetU1 = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(same as the CS8900A SelfCTL reset bit)");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Backend: %s%s", snap.backendChoice.c_str(),
                        snap.slirpCompiledIn ? "" : "  (libslirp not in this build)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Set with the `ethernet_backend` setting: slirp | loopback | none.\n"
            "Takes effect when the card is next plugged (profile switch or\n"
            "Slot Configuration change).\n\n"
            "slirp    user-mode NAT, guest 10.0.2.15, gateway 10.0.2.2,\n"
            "         DNS 10.0.2.3 — outbound only, no root needed\n"
            "loopback everything transmitted comes straight back (self-test)\n"
            "none     no host transport at all");
    }

    ImGui::End();
    return result;
}

} // namespace pom2
