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

// AI control server — the printer-detection endpoints (docs/printer-detection.md):
//
//   GET  /printer-port[?slot=N]      the printer side of one slot, or of every
//                                    slot holding a Super Serial Card or a
//                                    Grappler+ (PrinterPortControl)
//   POST /printer-port               {"slot":N,"set":"k=v,k=v"} — same option
//                                    language as --printer-port
//   GET  /slot-log?slot=N            drain slot N's bus-access log
//   POST /slot-log                   {"slot":N,"enable":1[,"capacity":C]} or
//                                    {"slot":N,"enable":0}
//   GET  /printer/spool?slot=N[&from=K]
//                                    the bytes the card in slot N has sent to
//                                    its printer since byte K (hex), and the
//                                    cursor for the next call

#include "AiControlJson.h"
#include "AiControlServer.h"
#include "CentronicsPrinter.h"

#include "EmulationController.h"
#include "GrapplerCard.h"
#include "Memory.h"
#include "PrinterCard.h"
#include "PrinterPortControl.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace pom2 {

using namespace aijson;

namespace {

bool slotInRange(long slot) { return slot >= 1 && slot <= 7; }

}  // namespace

void AiControlServer::handlePrinterPort(socket_t fd, const Request& req)
{
    if (!ctrl_) { sendJsonError(fd, 503, "no controller"); return; }

    if (req.method == "GET") {
        const std::string q = queryParam(req.query, "slot");
        const long only = q.empty() ? 0 : parseQueryNumber(q);
        if (!q.empty() && !slotInRange(only)) {
            sendJsonError(fd, 400, "slot must be 1-7");
            return;
        }
        std::ostringstream o;
        {
            auto st = ctrl_->lockState();
            SlotBus& bus = st.memory().slotBus();
            if (only) {
                o << "{\"port\":" << printerPortJson(describePrinterPort(bus, static_cast<int>(only))) << "}";
            } else {
                o << "{\"ports\":[";
                bool first = true;
                for (int s = 1; s <= 7; ++s) {
                    const auto state = describePrinterPort(bus, s);
                    if (state.card != "ssc" && state.card != "grappler" &&
                        state.card != "grappler1" && state.card != "pic")
                        continue;
                    if (!first) o << ",";
                    first = false;
                    o << printerPortJson(state);
                }
                o << "]}";
            }
        }
        sendJsonOk(fd, o.str());
        return;
    }
    if (req.method != "POST") { sendJsonError(fd, 405, "GET or POST"); return; }

    long slot = 0;
    if (!jsonGetInt(req.body, "slot", slot) || !slotInRange(slot)) {
        sendJsonError(fd, 400, "slot must be 1-7");
        return;
    }
    const std::string set = jsonGetString(req.body, "set");
    std::string error;
    std::string json;
    {
        auto st = ctrl_->lockState();
        SlotBus& bus = st.memory().slotBus();
        if (!applyPrinterPortOptions(bus, static_cast<int>(slot), set, error)) {
            // fall through to the error reply below, outside the lock
        } else {
            json = printerPortJson(describePrinterPort(bus, static_cast<int>(slot)));
        }
    }
    if (json.empty()) { sendJsonError(fd, 400, error); return; }
    sendJsonOk(fd, "{\"port\":" + json + "}");
}

void AiControlServer::handleSlotLog(socket_t fd, const Request& req)
{
    if (!ctrl_) { sendJsonError(fd, 503, "no controller"); return; }

    if (req.method == "POST") {
        long slot = 0, enable = 0, capacity = 65536;
        if (!jsonGetInt(req.body, "slot", slot) || !slotInRange(slot)) {
            sendJsonError(fd, 400, "slot must be 1-7");
            return;
        }
        if (!jsonGetInt(req.body, "enable", enable) ||
            !jsonOptionalIntOk(req.body, "capacity", capacity) ||
            capacity < 1 || capacity > (1L << 22)) {
            sendJsonError(fd, 400, "enable must be 0/1, capacity 1-4194304");
            return;
        }
        {
            auto st = ctrl_->lockState();
            SlotBus& bus = st.memory().slotBus();
            if (enable)
                bus.enableAccessLog(static_cast<int>(slot),
                                    static_cast<std::size_t>(capacity));
            else
                bus.disableAccessLog(static_cast<int>(slot));
        }
        sendJsonOk(fd, std::string("{\"slot\":") + std::to_string(slot) +
                       ",\"enabled\":" + (enable ? "true" : "false") + "}");
        return;
    }
    if (req.method != "GET") { sendJsonError(fd, 405, "GET or POST"); return; }

    const long slot = parseQueryNumber(queryParam(req.query, "slot"));
    if (!slotInRange(slot)) { sendJsonError(fd, 400, "slot must be 1-7"); return; }
    std::vector<SlotBus::SlotAccess> entries;
    bool enabled = false;
    std::uint64_t dropped = 0;
    {
        auto st = ctrl_->lockState();
        SlotBus& bus = st.memory().slotBus();
        enabled = bus.accessLogEnabled(static_cast<int>(slot));
        entries = bus.takeAccessLog(static_cast<int>(slot));
        dropped = bus.accessLogDropped(static_cast<int>(slot));
    }
    std::ostringstream o;
    o << "{\"slot\":" << slot << ",\"enabled\":" << (enabled ? "true" : "false")
      << ",\"dropped\":" << dropped
      << ",\"writes\":";
    std::size_t writes = 0;
    for (const auto& e : entries) writes += e.write ? 1 : 0;
    o << writes << ",\"entries\":[";
    char buf[96];
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        std::snprintf(buf, sizeof(buf),
                      "%s{\"cycle\":%llu,\"addr\":\"$%04X\",\"value\":\"$%02X\",\"op\":\"%c\"}",
                      i ? "," : "", static_cast<unsigned long long>(e.cycle),
                      e.addr, e.value, e.write ? 'w' : 'r');
        o << buf;
    }
    o << "]}";
    sendJsonOk(fd, o.str());
}

void AiControlServer::handlePrinterSpool(socket_t fd, const Request& req)
{
    if (!ctrl_) { sendJsonError(fd, 503, "no controller"); return; }
    if (req.method != "GET") { sendJsonError(fd, 405, "GET only"); return; }
    const long slot = parseQueryNumber(queryParam(req.query, "slot"));
    if (!slotInRange(slot)) { sendJsonError(fd, 400, "slot must be 1-7"); return; }
    const std::string fromText = queryParam(req.query, "from");
    const long from = fromText.empty() ? 0 : parseQueryNumber(fromText);
    if (from < 0) { sendJsonError(fd, 400, "from must be a byte offset"); return; }

    std::vector<uint8_t> bytes;
    std::size_t next = 0;
    std::string card;
    {
        auto st = ctrl_->lockState();
        SlotPeripheral* p = st.memory().slotBus().peripheral(static_cast<int>(slot));
        const auto f = static_cast<std::size_t>(from);
        if (auto* ssc = dynamic_cast<SuperSerialCard*>(p)) {
            card = "ssc";
            next = ssc->drainPrinterSpoolFrom(f, bytes);
        } else if (auto* g = dynamic_cast<GrapplerCard*>(p)) {
            card = "grappler";
            next = g->drainSpoolFrom(f, bytes);
        } else if (auto* pc = dynamic_cast<PrinterCard*>(p)) {
            card = "printer";
            next = pc->drainSpoolFrom(f, bytes);
        } else if (auto* cp = p ? p->centronicsPrinter() : nullptr) {
            card = "parallel";
            next = cp->drainSpoolFrom(f, bytes);
        }
    }
    if (card.empty()) {
        sendJsonError(fd, 404, "slot holds no printer-capable card");
        return;
    }
    sendJsonOk(fd, "{\"slot\":" + std::to_string(slot) + ",\"card\":\"" + card +
                   "\",\"next\":" + std::to_string(next) +
                   ",\"bytes\":\"" + bytesToHex(bytes.data(), bytes.size()) + "\"}");
}

}  // namespace pom2
