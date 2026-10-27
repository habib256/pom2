// MediaWritePolicy.h — the one place that says whether a freshly mounted
// medium writes. Part of POM2 (GPL-3.0-or-later).
//
// Media write by default (policy since 2026-09-08): a peripheral writes
// unless the user protects it, and that protection is a visible, editable
// tick in every media panel. Every storage leaf's `writeBackEnabled`
// initialiser and every `*_writeback` settings reader take their default
// from here, so the rule has one spelling.
//
// `POM2_MEDIA_WRITE_DEFAULT=protected` (or `0`) flips the default back to
// write-protected for the process. The test suite sets it for every ctest
// entry (tests/CMakeLists.txt): dozens of tests and probes boot the TRACKED
// images under disks_5.4/, disks_3.5/ and hdv/, and `DiskIICard`,
// `SmartPort35Unit` and `SmartPortHdvUnit` commit dirty media in their
// destructors — under a writable default the first full run rewrote
// disks_3.5/A2DeskTop-1.5-en_800k.2mg with a ProDOS boot's block updates.
// `media_write_default` clears the variable and pins the production default.
#pragma once

#include <cstdlib>

namespace pom2 {

inline bool mediaWritableByDefault()
{
    static const bool writable = [] {
        const char* e = std::getenv("POM2_MEDIA_WRITE_DEFAULT");
        if (!e || !*e) return true;
        return !(e[0] == 'p' || e[0] == 'P' || e[0] == '0' ||
                 e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F');
    }();
    return writable;
}

/// The `*_writeback` value to PERSIST for a drive whose live flag is `live`.
/// `false` there is the legacy opt-out that the next launch migrates into a
/// notch on the disk file (StorageCoordinator::migrateLegacyWriteBack). A
/// process protected by DEFAULT (POM2_MEDIA_WRITE_DEFAULT=protected) has every
/// flag off without the user opting out of anything, and used to save those
/// `false`s: the next normal launch then chmod'ed every mounted image
/// read-only. Such a process records the default, never an opt-out.
inline bool persistedWriteBack(bool live)
{
    return live || !mediaWritableByDefault();
}

} // namespace pom2
