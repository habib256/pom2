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

// ProfileExport — see the header.

#include "ProfileExport.h"


#include <cstdio>
#include <map>
#include <sstream>

namespace pom2 {

namespace {

// $0000-$BFFF RAM, $C000-$CFFF I/O and slot ROM, $D000-$FFFF ROM / language
// card. A label never names code in another region, nor code more than
// kLabelReach above it: RAM holds the program AND, far above it, DOS and
// its RWTS, which a lone `MAIN = $0800` swallowed (bug hunt round 2).
int regionOf(uint16_t a) { return a < 0xC000 ? 0 : a < 0xD000 ? 1 : 2; }
constexpr int kLabelReach = 0x1000;

// The routine an address belongs to: the nearest label at or below it, in
// the same memory region, among its own bank's labels and the bank-free
// ones (its own bank on a tie). Without the region a program's last symbol
// swallowed every Monitor and Applesoft address above it (bug hunt
// 2026-11-19); with no label in reach the page names it.
std::string routineOf(const DebuggerSymbols* symbols, uint16_t pc, DebugBank bank)
{
    if (symbols) {
        const auto& labels = symbols->labels();
        const std::string* best = nullptr;
        int bestAddr = -1;
        for (const DebugBank want : { bank, DebugBank{} }) {
            // Labels are ordered by address, then bank: walk down from pc.
            auto it = labels.upper_bound(DebugAddress{ pc, DebugBank{ DebugBank::Ignored, 255, 255 } });
            while (it != labels.begin()) {
                --it;
                if (regionOf(it->first.addr) != regionOf(pc)) break;   // below the region
                if (pc - it->first.addr >= kLabelReach) break;        // out of reach
                if (it->first.bank == want) {
                    if (static_cast<int>(it->first.addr) > bestAddr) {
                        bestAddr = it->first.addr;
                        best = &it->second;
                    }
                    break;
                }
            }
        }
        if (best) return *best;
    }
    char page[16];
    std::snprintf(page, sizeof page, "page $%02X", pc >> 8);
    return page;
}

} // namespace

std::string callgrindProfile(const std::vector<Debugger::ProfileEntry>& entries,
                             const DebuggerSymbols* symbols, const std::string& command)
{
    // bank → routine → address → (cycles, visits); std::map keeps every
    // level in a stable order, so the same profile writes the same file.
    struct Cost { uint64_t cycles = 0, visits = 0; };
    std::map<std::string, std::map<std::string, std::map<uint16_t, Cost>>> tree;
    uint64_t totalCycles = 0, totalVisits = 0;
    for (const auto& e : entries) {
        if (!e.visits) continue;
        Cost& c = tree[e.bank.text()][routineOf(symbols, e.pc, e.bank)][e.pc];
        c.cycles += e.cycles;
        c.visits += e.visits;
        totalCycles += e.cycles;
        totalVisits += e.visits;
    }

    std::ostringstream o;
    o << "# callgrind format\n"
      << "version: 1\n"
      << "creator: POM2\n"
      << "cmd: " << (command.empty() ? std::string("Apple II") : command) << "\n"
      << "positions: instr\n"
      << "events: Cycles Visits\n"
      << "summary: " << totalCycles << ' ' << totalVisits << "\n\n";
    for (const auto& [bank, routines] : tree) {
        o << "ob=" << bank << "\n" << "fl=" << bank << "\n";
        for (const auto& [routine, addresses] : routines) {
            o << "fn=" << routine << "\n";
            for (const auto& [pc, c] : addresses) {
                char line[64];
                std::snprintf(line, sizeof line, "0x%04X %llu %llu\n", pc,
                              static_cast<unsigned long long>(c.cycles),
                              static_cast<unsigned long long>(c.visits));
                o << line;
            }
        }
        o << "\n";
    }
    o << "totals: " << totalCycles << ' ' << totalVisits << "\n";
    return o.str();
}

} // namespace pom2
