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

// MachineSnapshot — the single source of truth for "what a POM2 state
// snapshot contains" and how it is applied back.
//
// It serializes / restores exactly the CPU + RAM state that both the
// AI-control `/snapshot/save|load` HTTP handlers and the rewind ring buffer
// (RewindBuffer) need, so the two never drift. The byte layout is the
// SnapshotIO section roster (see SnapshotIO.h):
//
//   CPU   17 bytes  PC A X Y P SP cpuMode + cycle counter + STP halt latch
//                   (16 in v1.0 blobs, which predate the halt latch and are
//                   still accepted)
//   MEM   64 KiB    main RAM (restored through Memory::restoreMainRam, ROM
//                   mirror preserved)
//   MEX   v2 blob   aux RAM + Language-Card RAM + RamWorks banks + paging
//                   soft-switches + DisplayState
//   SLOTn optional  per-card volatile state (e.g. DiskIICard head/LSS) —
//                   written ONLY when captureMachineState(includeSlots=true).
//
// `includeSlots` is the dividing line: the rewind ring buffer passes true so
// a rewind during disk I/O restores the drive head (the media itself is never
// captured — it doesn't change within a rewind window). The AI-control
// `/snapshot` path passes false, keeping its documented "disk/slot state
// deliberately excluded" (CLAUDE.md) file contract — an archival snapshot
// can outlive a media swap, where a stale head position would mismatch.
// restoreMachineState always applies any SLOTn sections it finds (none in an
// AI-control file), so a rewind blob and a file blob load through one path.
// The functions take a SnapshotWriter/Reader so the caller chooses the
// backend (file for /snapshot, in-memory vector for rewind).

#ifndef POM2_MACHINE_SNAPSHOT_H
#define POM2_MACHINE_SNAPSHOT_H

#include <string>

class M6502;
class Memory;

namespace pom2 {

class SnapshotWriter;
class SnapshotReader;

/// Serialize the machine state into `w` (CPU + MEM + MEX, and per-slot
/// SLOTn sections when `includeSlots` is true — see the header note).
void captureMachineState(SnapshotWriter& w, M6502& cpu, Memory& mem,
                         bool includeSlots = false);

struct RestoreResult {
    bool        ok = true;
    std::string error;   // populated only when ok == false
};

/// Apply the CPU / MEM / MEX sections found in `r` to `cpu` + `mem`.
/// Tolerant by design (mirrors the original AI-control loader):
///   • CPU honoured only at the full 16-byte length — a short, crafted
///     section is skipped (the "round 10 #3" over-read hardening).
///   • MEM honoured only at exactly 0x10000 bytes, restored through
///     Memory::restoreMainRam so the ROM mirror is preserved.
///   • MEX bounded to 16 MiB; an oversized section aborts the restore with
///     ok == false so HTTP callers can surface a 400. RewindBuffer feeds its
///     own data and never hits this.
/// Unknown / wrong-length sections are skipped (forward compatibility).
/// With `transactional=true` (the default for files/API input), the complete
/// CPU, memory and slot state is checkpointed and restored if a later section
/// is malformed. Rewind uses false for its trusted in-memory frames to avoid
/// duplicating a full snapshot on every scrub.
RestoreResult restoreMachineState(SnapshotReader& r, M6502& cpu, Memory& mem,
                                  bool transactional = true);

}  // namespace pom2

#endif  // POM2_MACHINE_SNAPSHOT_H
