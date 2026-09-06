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

// ProDOSVolume — synthesise a read-only ProDOS volume image (block array)
// from the contents of a host folder.
//
// Layout produced:
//   Block 0       Boot block (zeroed — volume is not directly bootable;
//                 the user boots ProDOS from elsewhere, then the slot 5
//                 host-folder volume appears as a secondary drive).
//   Block 1       Boot block 2 (zeroed).
//   Blocks 2-5    Volume directory (key + 3 extension blocks → 51 entries).
//   Blocks 6+      Contiguous volume bitmap blocks, allocated to cover the
//                  complete synthesised volume.
//   Following     File data + sapling index blocks, allocated sequentially.
//
// Scope (MVP):
//   * Flat directory only — sub-directories of the host folder are skipped.
//   * Up to 51 files (volume directory limit). Excess are skipped.
//   * Files ≤ 128 KB — supports seedling (≤ 512 B) and sapling (1 idx +
//     up to 256 data blocks). Tree files (> 128 KB) are skipped with a
//     warning. Covers virtually every Apple II program.
//   * File type guessed from host extension; defaults to BIN ($06).
//   * Aux type = 0 (no metadata to derive from).
//   * Names sanitised to ProDOS conventions (uppercased ASCII, A-Z/0-9/.,
//     starts with a letter, ≤ 15 chars; collisions get .1/.2/… suffixes).
//
// Output is a `std::vector<uint8_t>` whose size is a multiple of 512 —
// directly consumable by `ProDOSHardDiskCard::loadImageFromBytes(...)`.

#ifndef POM2_PRODOS_VOLUME_H
#define POM2_PRODOS_VOLUME_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

struct ProDOSBuildResult {
    bool        ok = false;
    std::string error;
    std::size_t filesIncluded = 0;
    std::size_t filesSkipped  = 0;     // > 128 KB, name unsanitisable, or overflow
    std::size_t totalBlocks   = 0;
};

/// Synthesise a ProDOS volume from `hostFolder` (the mounted volume is
/// guest-writable RAM; persisting guest writes back to the folder is the
/// separate write-back opt-in — see `decodeVolumeToFolder`). The volume's
/// header carries `volumeName` (truncated/uppercased to a ProDOS-legal
/// 15-char name; pass "HOST" for the typical case). On success,
/// `outImage` holds the volume bytes (multiple of 512). Empty / missing
/// host folders produce a valid empty volume (no files), not an error.
ProDOSBuildResult buildVolumeFromFolder(const std::string& hostFolder,
                                        const std::string& volumeName,
                                        std::vector<std::uint8_t>& outImage);

struct ProDOSDecodeResult {
    bool        ok = false;
    std::string error;
    std::size_t filesWritten  = 0;
    std::size_t filesSkipped  = 0;
    std::size_t dirsCreated   = 0;
    /// Subdirectory entries the walk refused: unsafe name, cyclic/aliased
    /// key_pointer, depth cap, or exhausted directory budget.
    std::size_t dirsSkipped   = 0;
    /// True when a bound fired (directory budget or depth cap), i.e. the
    /// host tree is a PARTIAL image of the volume. The decode still reports
    /// `ok` — the files it did write are valid — but `error` then carries a
    /// human-readable reason for callers that want to surface it.
    bool        aborted       = false;
    /// An instant at or after every file this decode wrote. It is what the
    /// caller must adopt as the volume's new mount stamp: the files the
    /// write-back just created are NEWER than the old stamp, so on the next
    /// flush `preserveNewerThan` would classify POM2's own output as a
    /// host-side edit and preserve the guest's SECOND round of changes away.
    /// Taken as max(clock now, newest mtime actually written) so a server
    /// clock ahead of ours cannot defeat it.
    std::filesystem::file_time_type completedAt{};
};

/// Reverse of `buildVolumeFromFolder`: walk a synthesised volume's directory
/// blocks (2..5) and write every seedling/sapling file back into `hostFolder`
/// using the inverse of the file_type → extension mapping. Files in the
/// folder that are *absent* from the volume are LEFT UNTOUCHED — never
/// deleted. Existing files whose bytes already match the volume are skipped
/// (no write, no mtime bump). Tree files (>128 KB) are skipped with a warn.
/// `hostFolder` is created if missing.
///
/// The image is guest-writable RAM, so its directory graph is untrusted: a
/// subdir entry's key_pointer may alias an ancestor block and turn the walk
/// into a cycle whose fan-out (13 entries × 256 chained blocks per level)
/// makes even a depth-bounded traversal unbounded in visits — every one of
/// which creates a host directory. The walk therefore keeps a per-call set
/// of expanded directory blocks (each is walked at most once) and a global
/// directory budget; both report through `dirsSkipped` / `aborted`.
/// `preserveNewerThan` (optional): the volume snapshot's mount time. A host
/// file whose mtime is LATER was edited on the host while the volume was
/// mounted — the snapshot's copy is stale, so the file is preserved (warned
/// + counted in `filesSkipped`) instead of silently reverted. Pass nullptr
/// for the legacy overwrite-everything behaviour.
ProDOSDecodeResult decodeVolumeToFolder(
    const std::vector<std::uint8_t>& image,
    const std::string& hostFolder,
    const std::filesystem::file_time_type* preserveNewerThan = nullptr);

/// True iff `name` (a directory-entry name decoded from an untrusted volume
/// image) is safe to use as a single host path component. The image is
/// guest-writable RAM, so a hostile/corrupt entry can carry path separators,
/// NUL, "." / ".." — which `decodeVolumeToFolder` would otherwise join to the
/// host folder and escape the jail. A legal ProDOS name is 1..15 chars of
/// [A-Za-z0-9.] and is never "." or ".."; anything else is rejected.
bool isHostSafeProDOSName(const std::string& name);

} // namespace pom2

#endif // POM2_PRODOS_VOLUME_H
