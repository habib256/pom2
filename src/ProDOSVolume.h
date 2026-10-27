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

// ProDOSVolume — synthesise a guest-writable ProDOS volume image (block array)
// from the contents of a host folder.
//
// Layout produced:
//   Block 0       Boot block (zeroed — volume is not directly bootable;
//                 the user boots ProDOS from elsewhere, then accesses the
//                 host-folder volume through its configured HDV slot/drive).
//   Block 1       Boot block 2 (zeroed).
//   Blocks 2-5    Volume directory (key + 3 extension blocks → 51 entries).
//   Blocks 6+      Contiguous volume bitmap blocks, allocated to cover the
//                  complete synthesised volume.
//   Following     Subdirectories, file data and sapling/tree index blocks.
//
// Scope:
//   * Recursive subdirectories (bounded depth and symlink-cycle detection).
//   * Up to 51 root entries (the ProDOS root directory limit).
//   * Seedling, sapling and tree files, up to the 24-bit EOF limit (16 MiB-1).
//   * File type guessed from extension, or explicit NAME#TTAAAA metadata.
//     Applesoft defaults to aux $0801; other untagged files default to 0.
//   * Names sanitised to ProDOS conventions (uppercased ASCII, A-Z/0-9/.,
//     starts with a letter, ≤ 15 chars; collisions get .1/.2/… suffixes).
//
// Output is a `std::vector<uint8_t>` whose size is a multiple of 512 —
// directly consumable by `ProDOSHardDiskCard::loadImageFromBytes(...)`.

#ifndef POM2_PRODOS_VOLUME_H
#define POM2_PRODOS_VOLUME_H

#include "ProDOSHostNameMap.h"   // takeFolderScanStart / takeFolderNameMap

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pom2 {

struct ProDOSBuildResult {
    bool        ok = false;
    std::string error;
    std::size_t filesIncluded = 0;
    std::size_t filesSkipped  = 0;     // > 24-bit EOF, unsafe name, or directory overflow
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
                                        std::vector<std::uint8_t>& outImage,
                                        bool convertText = false);

struct ProDOSDecodeResult {
    bool        ok = false;
    std::string error;
    std::size_t filesWritten  = 0;
    std::size_t filesSkipped  = 0;
    /// The subset of `filesSkipped` that is DATA LOSS: an entry the guest
    /// wrote and this decode could not put anywhere on the host (a name the
    /// host cannot carry, a destination that resolves outside the served
    /// folder, an unreadable key_pointer, a file past the size ceiling).
    /// Deliberate skips — a host file preserved because the user edited it
    /// after the mount — are NOT counted here.
    ///
    /// Non-zero makes the decode FAIL (`ok == false`, `error` naming the
    /// entries). It used to return success: the guest's save reported "5
    /// file(s) written", the sixth was gone, and the block stayed clean so
    /// nothing ever retried. A save that did not save everything is a failed
    /// save, and the user has to be able to see which file to rename.
    std::size_t filesUnsaved  = 0;
    /// Host changed since mount: guest copy stored under .pom2-conflicts.
    std::size_t filesConflicted = 0;
    std::vector<std::string> conflictNames;
    /// The first few names behind `filesUnsaved`, for the error text.
    std::vector<std::string> unsavedNames;
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
    /// True once the walk ran, so `completedAt` is real. A decode that stops
    /// before writing anything (folder gone, not creatable) leaves it false:
    /// comparing `completedAt` against a default value is no signal, since
    /// what a default file_clock time means differs between libc++ and
    /// libstdc++ (the 2026-10-21 guard did not hold on Linux).
    bool completedAtValid = false;
};

/// Reverse of `buildVolumeFromFolder`: walk a synthesised volume's directory
/// blocks (2..5) and write every seedling/sapling/tree file into `hostFolder`
/// using the inverse of the file_type → extension mapping. Files in the
/// folder that are *absent* from the volume are LEFT UNTOUCHED — never
/// deleted. Existing files whose bytes already match the volume are skipped
/// (no write, no mtime bump). Seedling, sapling AND tree files are decoded —
/// both directions support all three storage types, including guest-grown
/// files. `hostFolder` is created if missing.
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
/// mounted. The host file is preserved; a differing volume copy is stored in
/// `.pom2-conflicts/`, with numbered names for distinct later copies. Both
/// outcomes are counted in `filesSkipped`. Pass nullptr for the legacy
/// overwrite-everything behaviour.
///
/// `stickyPreserved` (optional): host files already preserved earlier in the
/// same mount. They stay preserved whatever their mtime, and every file this
/// pass preserves is added. Without it the caller's restamp (see
/// `completedAt`) moved past the user's edit, and the NEXT flush reverted it
/// to the mount-time copy (bug hunt 2026-09-17).
ProDOSDecodeResult decodeVolumeToFolder(
    const std::vector<std::uint8_t>& image,
    const std::string& hostFolder,
    const std::filesystem::file_time_type* preserveNewerThan = nullptr,
    std::set<std::string>* stickyPreserved = nullptr,
    bool convertText = false,
    const HostNameMap* mountNames = nullptr);

/// True iff `name` (a directory-entry name decoded from an untrusted volume
/// image) is safe to use as a single host path component. The image is
/// guest-writable RAM, so a hostile/corrupt entry can carry path separators,
/// NUL, "." / ".." — which `decodeVolumeToFolder` would otherwise join to the
/// host folder and escape the jail. A legal ProDOS name is 1..15 chars of
/// [A-Za-z0-9.] and is never "." or ".."; anything else is rejected.
bool isHostSafeProDOSName(const std::string& name);

/// True iff `name` is one a well-formed ProDOS directory entry can hold:
/// 1..15 characters of [A-Za-z0-9.], never "." or "..".
///
/// The line between "a file the guest really has" and "bytes no ProDOS ever
/// wrote". `isHostSafeProDOSName` is strictly narrower — it additionally
/// refuses what the HOST cannot take (a DOS device name, a leading dot) — and
/// the write-back uses the gap between the two: a legal name the host refuses
/// is lost user data and fails the save (`ProDOSDecodeResult::filesUnsaved`),
/// while an illegal one is a crafted or corrupt entry that is merely skipped.
/// Failing on the latter would let one hostile image jam every future save.
bool isProDOSLegalName(const std::string& name);

} // namespace pom2

#endif // POM2_PRODOS_VOLUME_H
