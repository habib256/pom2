// MediaNotch.h — the write-protect notch, on the disk and nowhere else
//
// A real 5.25" is write-enabled by a notch cut in its sleeve and
// write-protected by a sticker over that notch; a 3.5" has a sliding tab.
// Either way the protection travels WITH THE DISK — put it in another
// drive and it is still protected. POM2 models that with the one thing that
// travels with an image file: its host write permission. Every loader
// (`DiskImage`, `Disk35Image`, `Block512Backing`) already mounted a
// read-only file as write-protected; since 2026-09-08 the media panels and
// the Disk Library toggle exactly that bit, and the per-card / per-drive
// "write-back" flags are no longer a user-facing setting (they remain the
// process-wide default under `MediaWritePolicy.h`, which is how the test
// suite runs protected).
//
// Both helpers are file I/O: call them with `stateMutex` RELEASED.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace pom2 {

/// True when the host refuses to open `path` for writing — the read-only
/// bit, a read-only attribute on Windows, a read-only volume, or a file
/// owned by someone else. This is the probe every loader uses, so the
/// answer here is what the mounted image will report.
inline bool mediaFileIsReadOnly(const std::string& path)
{
    std::ofstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
    return !probe;
}

/// Put the sticker on (`protect` = true: clear every write bit) or take it
/// off (add the owner's write bit). Returns false with `error` set when the
/// host refuses — a read-only volume, a file the user does not own — in
/// which case nothing changed.
inline bool setMediaNotch(const std::string& path, bool protect, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "not a file: " + path;
        return false;
    }
    const fs::perms bits = protect
        ? (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)
        : fs::perms::owner_write;
    fs::permissions(path, bits,
                    protect ? fs::perm_options::remove : fs::perm_options::add, ec);
    if (ec) {
        error = "cannot " + std::string(protect ? "write-protect " : "write-enable ") +
                path + ": " + ec.message();
        return false;
    }
    if (mediaFileIsReadOnly(path) != protect) {
        error = std::string(protect ? "write-protect" : "write-enable") +
                " had no effect on " + path +
                (protect ? "" : " (read-only volume, or not your file?)");
        return false;
    }
    return true;
}

}  // namespace pom2
