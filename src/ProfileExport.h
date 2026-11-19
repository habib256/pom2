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

// ProfileExport — the debugger's profiler, written as a callgrind file for
// KCachegrind / QCachegrind / callgrind_annotate (UI-017).
//
// [B] Valgrind "Callgrind Format Specification" (valgrind.org/docs/manual/
// cl-format.html): a header (`version: 1`, `positions: instr`, `events:`),
// then cost lines `<address> <cost>…` under the current `ob=` / `fl=` /
// `fn=`. POM2 writes two events — Cycles, Visits — per executed address.
//
// The grouping is the Apple's: `ob=` is the physical bank (main, aux0, lc1,
// rom0, slot6 — DebugAddress.h), so the same address in two banks never
// merges, and `fn=` is the nearest symbol at or below the address in that
// bank (a cc65 `.dbg` import or a symbol file, through DebuggerSymbols) —
// or, with no symbol, the 256-byte page (`page $C6`). Flat costs only: the
// profiler counts per address, it records no call graph.

#ifndef POM2_PROFILE_EXPORT_H
#define POM2_PROFILE_EXPORT_H

#include "Debugger.h"
#include "DebuggerSymbols.h"

#include <string>
#include <vector>

namespace pom2 {

/// A complete callgrind file. `symbols` may be null; `command` names the
/// run (the disk, the program) in the header.
std::string callgrindProfile(const std::vector<Debugger::ProfileEntry>& entries,
                             const DebuggerSymbols* symbols, const std::string& command);

} // namespace pom2

#endif // POM2_PROFILE_EXPORT_H
