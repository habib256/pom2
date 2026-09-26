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

#include "PrinterPortControl.h"

#include "AppleParallelCard.h"
#include "CentronicsPrinter.h"
#include "GrapplerCard.h"
#include "GrapplerClassicCard.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SuperSerialCard.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

namespace pom2 {
namespace {

std::vector<std::pair<std::string, std::string>> splitOptions(
    const std::string& options, std::string& error)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::string item;
    std::istringstream in(options);
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == item.size()) {
            error = "expected key=value, got \"" + item + "\"";
            return {};
        }
        out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    }
    if (out.empty() && error.empty()) error = "no options given";
    return out;
}

bool parseOnOff(const std::string& v, bool& out)
{
    if (v == "on" || v == "1" || v == "true")   { out = true;  return true; }
    if (v == "off" || v == "0" || v == "false") { out = false; return true; }
    return false;
}

bool parseByte(const std::string& v, uint8_t& out)
{
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 0);   // 0x.. or decimal
    if (end == v.c_str() || *end != '\0' || n < 0 || n > 0xFF) return false;
    out = static_cast<uint8_t>(n);
    return true;
}

const char* onOff(bool b) { return b ? "true" : "false"; }

std::string hex2(uint8_t v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\"$%02X\"", v);
    return buf;
}

}  // namespace

PrinterPortState describePrinterPort(SlotBus& bus, int slot)
{
    PrinterPortState st;
    st.slot = slot;
    SlotPeripheral* p = (slot >= 1 && slot < SlotBus::kSlotCount)
                            ? bus.peripheral(slot) : nullptr;
    if (!p) { st.card = "none"; return st; }

    if (auto* ssc = dynamic_cast<SuperSerialCard*>(p)) {
        st.card = "ssc";
        st.firmware = ssc->firmwareLoaded();
        st.builtInPort = ssc->builtInPort();
        st.dsw1 = ssc->dipSwitch1();
        st.dsw2 = ssc->dipSwitch2();
        st.mode = SuperSerialCard::modeKey(ssc->mode());
        st.cable = SuperSerialCard::cableKey(ssc->cable());
        st.tap = ssc->printerTap();
        const auto lines = ssc->inputLines();
        st.dcd = lines.dcd;
        st.dsr = lines.dsr;
        st.cts = lines.cts;
        return st;
    }
    if (auto* g = dynamic_cast<GrapplerCard*>(p)) {
        st.card = "grappler";
        st.romLoaded = g->isRomLoaded();
        st.online = g->online();
        st.paperOut = g->paperOut();
        st.printerConnected = g->printerConnected();
        st.busy = g->printerBusy();
        st.printerType = static_cast<uint8_t>(g->printerType());
        st.lineBits = g->lineBits();
        return st;
    }
    if (auto* cp = p->centronicsPrinter()) {
        st.card = dynamic_cast<AppleParallelCard*>(p) ? "pic"
                : dynamic_cast<GrapplerClassicCard*>(p) ? "grappler1"
                : std::string(p->name());
        st.romLoaded = true;           // these cards do not plug without it
        st.online = cp->online();
        st.paperOut = cp->paperOut();
        st.printerConnected = cp->connected();
        st.busy = cp->busy();
        st.lineBits = static_cast<uint8_t>((cp->busyLine() ? 0x08 : 0) |
                                           (cp->peLine() ? 0x04 : 0) |
                                           (cp->selectLine() ? 0x02 : 0));
        return st;
    }
    st.card = std::string(p->name());
    return st;
}

std::string printerPortJson(const PrinterPortState& s)
{
    std::ostringstream o;
    o << "{\"slot\":" << s.slot << ",\"card\":\"";
    for (char c : s.card) {
        if (c == '"' || c == '\\') o << '\\';
        o << c;
    }
    o << "\"";
    if (s.card == "ssc") {
        o << ",\"firmware\":" << onOff(s.firmware)
          << ",\"built_in_port\":" << onOff(s.builtInPort)
          << ",\"dsw1\":" << hex2(s.dsw1)
          << ",\"dsw2\":" << hex2(s.dsw2)
          << ",\"mode\":\"" << s.mode << "\""
          << ",\"cable\":\"" << s.cable << "\""
          << ",\"tap\":" << onOff(s.tap)
          << ",\"lines\":{\"dcd\":" << onOff(s.dcd)
          << ",\"dsr\":" << onOff(s.dsr)
          << ",\"cts\":" << onOff(s.cts) << "}";
    } else if (s.card == "grappler" || s.card == "grappler1" || s.card == "pic") {
        o << ",\"rom\":" << onOff(s.romLoaded)
          << ",\"online\":" << onOff(s.online)
          << ",\"paper_out\":" << onOff(s.paperOut)
          << ",\"printer_connected\":" << onOff(s.printerConnected)
          << ",\"busy\":" << onOff(s.busy)
          << ",\"line_bits\":" << hex2(s.lineBits);
        if (s.card == "grappler")
            o << ",\"printer_type\":" << static_cast<int>(s.printerType);
    }
    o << "}";
    return o.str();
}

bool applyPrinterPortOptions(SlotBus& bus, int slot, const std::string& options,
                             std::string& error)
{
    error.clear();
    if (slot < 1 || slot >= SlotBus::kSlotCount) {
        error = "slot must be 1-7";
        return false;
    }
    SlotPeripheral* p = bus.peripheral(slot);
    auto* ssc = dynamic_cast<SuperSerialCard*>(p);
    auto* g = dynamic_cast<GrapplerCard*>(p);
    pom2::CentronicsPrinter* cp = p ? p->centronicsPrinter() : nullptr;
    if (!ssc && !g && !cp) {
        error = "slot " + std::to_string(slot) + " holds no printer card";
        return false;
    }
    const auto opts = splitOptions(options, error);
    if (!error.empty()) return false;

    // Validate everything into a list of actions, then run them. The SSC's
    // `mode` goes first so explicit dsw1/dsw2 win over what it writes.
    std::vector<std::function<void()>> first, then;
    for (const auto& kv : opts) {
        const std::string& key = kv.first;
        const std::string& value = kv.second;
        auto bad = [&] {
            error = "bad value for " + key + ": \"" + value + "\"";
            return false;
        };
        if (ssc) {
            if (key == "mode") {
                SuperSerialCard::Mode m;
                if (!SuperSerialCard::parseModeKey(value, m)) return bad();
                if (ssc->builtInPort()) {
                    error = "a //c serial port has no mode switches";
                    return false;
                }
                first.push_back([ssc, m] { ssc->setMode(m); });
            } else if (key == "dsw1" || key == "dsw2") {
                uint8_t b;
                if (!parseByte(value, b)) return bad();
                if (ssc->builtInPort()) {
                    error = "a //c serial port has no DIP switches";
                    return false;
                }
                const bool one = key == "dsw1";
                then.push_back([ssc, b, one] {
                    ssc->setDipSwitches(one ? b : ssc->dipSwitch1(),
                                        one ? ssc->dipSwitch2() : b);
                });
            } else if (key == "cable") {
                SuperSerialCard::Cable c;
                if (!SuperSerialCard::parseCableKey(value, c)) return bad();
                then.push_back([ssc, c] { ssc->setCable(c); });
            } else if (key == "tap") {
                bool on;
                if (!parseOnOff(value, on)) return bad();
                then.push_back([ssc, on] { ssc->setPrinterTap(on); });
            } else {
                error = "unknown Super Serial Card option: " + key;
                return false;
            }
        } else {
            bool on;
            if (key == "online") {
                if (!parseOnOff(value, on)) return bad();
                if (g) then.push_back([g, on] { g->setOnline(on); });
                else   then.push_back([cp, on] { cp->setOnline(on); });
            } else if (key == "paper") {
                if (value != "ok" && value != "out") return bad();
                const bool out = value == "out";
                if (g) then.push_back([g, out] { g->setPaperOut(out); });
                else   then.push_back([cp, out] { cp->setPaperOut(out); });
            } else if (key == "printer") {
                if (value != "connected" && value != "none") return bad();
                const bool c = value == "connected";
                if (g) then.push_back([g, c] { g->setPrinterConnected(c); });
                else   then.push_back([cp, c] { cp->setConnected(c); });
            } else if (key == "busy") {
                if (!parseOnOff(value, on)) return bad();
                if (g) then.push_back([g, on] { g->setPrinterBusy(on); });
                else   then.push_back([cp, on] { cp->setBusy(on); });
            } else if (key == "type" && g) {
                uint8_t t;
                if (!parseByte(value, t) || t > 7) return bad();
                then.push_back([g, t] {
                    g->setPrinterType(static_cast<GrapplerCard::PrinterType>(t));
                });
            } else {
                error = "unknown printer-card option: " + key;
                return false;
            }
        }
    }
    for (auto& f : first) f();
    for (auto& f : then) f();
    return true;
}

bool parsePrinterPortSpec(const std::string& spec, int& slot,
                          std::string& options, std::string& error)
{
    const auto colon = spec.find(':');
    if (colon != 1 || spec[0] < '1' || spec[0] > '7') {
        error = "expected SLOT:key=value,... with SLOT 1-7, got \"" + spec + "\"";
        return false;
    }
    slot = spec[0] - '0';
    options = spec.substr(colon + 1);
    if (options.empty()) {
        error = "no options after \"" + spec.substr(0, 2) + "\"";
        return false;
    }
    return true;
}

}  // namespace pom2
