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

} // namespace pom2
