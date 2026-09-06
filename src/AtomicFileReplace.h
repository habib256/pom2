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

// Cross-platform commit of a sibling temporary file over an existing file.

#ifndef POM2_ATOMIC_FILE_REPLACE_H
#define POM2_ATOMIC_FILE_REPLACE_H

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace pom2 {

/// Push a file's CONTENTS to stable storage.
///
/// `rename` is atomic with respect to a READER — it never exposes a half
/// written file — but it says nothing about DURABILITY. Until the data pages
/// are flushed, a power cut (or a kernel panic, or a yanked USB stick) can
/// land the directory entry while the blocks behind it are still in page
/// cache: the classic outcome is a 0-byte file where the user's disk image
/// used to be, because the metadata commit reached the journal and the data
/// did not. Every write-back path in POM2 replaces the user's ONLY copy of
/// their media — the rest of the disk lives in RAM and dies with the process
/// — so this flush is not optional here.
///
/// Returns false only on a REAL I/O failure (EIO, ENOSPC): the caller should
/// then treat the save as failed and keep its dirty state so the user can
/// retry. A filesystem that simply cannot honour the request (EINVAL /
/// EOPNOTSUPP — some network and virtual filesystems, Emscripten's MEMFS)
/// reports success: failing the user's save over a missing guarantee would
/// be strictly worse than saving without it.
inline bool syncFileContents(const std::filesystem::path& p,
                             std::error_code& ec)
{
#ifdef _WIN32
    // FlushFileBuffers is documented to need GENERIC_WRITE on the handle, so
    // this cannot mirror the POSIX branch's O_RDONLY. That matters because
    // the callers restore the ORIGINAL file's permissions onto the temp copy
    // before committing: a write-protected disk image left the temp
    // FILE_ATTRIBUTE_READONLY, CreateFileW then failed ERROR_ACCESS_DENIED,
    // and the whole write-back failed — on Windows only, for an image that
    // saves fine everywhere else. A handle we cannot open for write is the
    // same class of answer as a volume that does not implement write-through:
    // the flush cannot be PROMISED, and refusing the user's save over a
    // missing guarantee would be strictly worse than saving without it. So
    // an access/sharing refusal reports success, exactly like the
    // ERROR_INVALID_FUNCTION branch below and like POSIX's EROFS/EBADF.
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD openErr = GetLastError();
        if (openErr == ERROR_ACCESS_DENIED || openErr == ERROR_SHARING_VIOLATION) {
            ec.clear();
            return true;
        }
        ec = std::error_code(static_cast<int>(openErr), std::system_category());
        return false;
    }
    const BOOL  ok  = FlushFileBuffers(h);
    const DWORD err = ok ? 0u : GetLastError();
    CloseHandle(h);
    if (ok) { ec.clear(); return true; }
    // The volume does not implement write-through (ERROR_INVALID_FUNCTION is
    // what FlushFileBuffers returns on several network redirectors).
    if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(err), std::system_category());
    return false;
#else
    // O_RDONLY is enough for fsync on Linux/macOS/BSD, and it is what lets
    // this run AFTER the caller has restored the original file's permissions
    // onto the temp copy (which may be read-only).
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = (rc == 0) ? 0 : errno;
    ::close(fd);
    if (rc == 0) { ec.clear(); return true; }
    if (saved == EINVAL || saved == ENOTSUP || saved == EOPNOTSUPP ||
        saved == EBADF  || saved == EROFS) {
        ec.clear();               // cannot promise it → not a write failure
        return true;
    }
    ec = std::error_code(saved, std::generic_category());
    return false;
#endif
}

/// Flush the DIRECTORY ENTRY a freshly renamed file lives in, so the rename
/// itself survives a crash (POSIX requires the containing directory to be
/// fsync'd for that; the data flush above only covers the file's blocks).
///
/// Best-effort by design: a fair number of filesystems refuse fsync on a
/// directory handle, and none of those refusals make the preceding data
/// flush any less valid. On Windows the rename already goes out with
/// MOVEFILE_WRITE_THROUGH, which is the documented equivalent.
inline void syncParentDirectory(const std::filesystem::path& p) noexcept
{
#ifdef _WIN32
    (void)p;
#else
    std::error_code ignored;
    const std::filesystem::path dir =
        p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
#  ifdef O_DIRECTORY
    const int flags = O_RDONLY | O_DIRECTORY;
#  else
    const int flags = O_RDONLY;
#  endif
    const int fd = ::open(dir.c_str(), flags);
    if (fd < 0) return;
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    (void)rc;
    ::close(fd);
#endif
}

/// Make a sibling temporary path safe to create-and-truncate.
///
/// Every write-back path here writes `<target>.tmp` and then renames it over
/// the target. The CALLER validates the target — refuses a path outside the
/// working directory, refuses a symlink, checks the extension — but the temp
/// path is derived afterwards and gets none of that scrutiny, while
/// `ofstream(..., trunc)` follows symlinks like any other open.
///
/// So a file planted at `<target>.tmp` beforehand redirects the write: the
/// bytes land on whatever it points at, and the rename then moves the symlink
/// away, leaving the destroyed target behind with nothing to show what
/// happened. The temp name is ours by construction, so anything already
/// sitting there is either our own debris from a crashed run or somebody
/// else's doing — remove it either way, and refuse to proceed if it will not
/// go, rather than writing through it.
///
/// Returns false only when the path is still unusable afterwards; a missing
/// temp (the normal case) is success.
/// A sibling temporary path that is UNIQUE to this process and this call.
///
/// Every write-back used to derive the temp name from the target alone —
/// `<target>.tmp` / `<target>.pom2tmp`. That name is not ours, it is the
/// name every POM2 on the machine picks: two instances sharing one $HOME (a
/// second window, a kiosk session, a headless run) both open `state.cfg.tmp`
/// with `trunc`, interleave their writes, and whichever renames last
/// publishes a file made of both. Same shape for a disk image opened twice.
/// The pid + counter make the name collide only with ourselves, which we
/// control; `prepareTempPath` below still runs on it, because a NAME being
/// unlikely is not the same as a path being safe.
inline std::filesystem::path tempSiblingPath(const std::filesystem::path& target)
{
    static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return std::filesystem::path(
        target.string() + "." + std::to_string(pid) + "-" +
        std::to_string(counter.fetch_add(1) + 1) + ".pom2tmp");
}

inline bool prepareTempPath(const std::filesystem::path& tmp,
                            std::error_code& ec)
{
    ec.clear();
    std::error_code probe;
    // symlink_status, NOT status: status() follows the link and would report
    // on the victim, which is exactly the thing being hidden here.
    const auto st = std::filesystem::symlink_status(tmp, probe);
    if (probe || st.type() == std::filesystem::file_type::not_found)
        return true;                       // nothing in the way

    if (st.type() == std::filesystem::file_type::regular) {
        // Our own leftover always has link count 1. A regular file HARD-
        // LINKED to the target shares the target's inode, so the caller's
        // ofstream(trunc) through it would destroy the target BEFORE
        // anything is committed — the same redirection the symlink branch
        // below removes, and a violation of the "target untouched on every
        // failure path" contract. remove() only unlinks THIS name.
        std::error_code lc;
        const auto links = std::filesystem::hard_link_count(tmp, lc);
        if (!lc && links > 1) {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            if (rm) { ec = rm; return false; }
        }
        return true;                       // a plain leftover; trunc is fine
    }

    // ONLY a symlink is removed. It is the one entry that redirects the write
    // somewhere else, and the temp name is ours by construction, so nothing
    // legitimate is being destroyed. Everything else non-regular — a
    // directory, a fifo, a device node — is refused rather than deleted:
    // removing it would be a bigger act than this function is entitled to,
    // and the open would fail on its own anyway.
    if (st.type() != std::filesystem::file_type::symlink) {
        ec = std::make_error_code(std::errc::file_exists);
        return false;
    }

    std::error_code rm;
    std::filesystem::remove(tmp, rm);
    if (rm) { ec = rm; return false; }
    return true;
}

/// Resolve a symlinked TARGET to the file it names.
///
/// A rename does not follow links: renaming over `~/.config/POM2/state.cfg`
/// when that is a symlink into a dotfiles repo REPLACES the link with a
/// regular file. The user's setup is quietly dismantled, their repo stops
/// tracking the file, and nothing says so. Nobody symlinks a path in order
/// to have it replaced — the link IS the instruction "write to the thing I
/// point at" — so POM2 follows it rather than refusing (refusing would make
/// a symlinked settings file or disk image unsaveable, which is worse).
///
/// The cost is that the temp file is a sibling of the LINK, not of its
/// target: if they sit on different filesystems the rename now fails loudly
/// with `cross_device_link` instead of silently eating the link. A loud
/// failure the user can act on is the better half of that trade.
inline std::filesystem::path resolveReplaceTarget(const std::filesystem::path& to)
{
    std::error_code ec;
    if (!std::filesystem::is_symlink(std::filesystem::symlink_status(to, ec)) || ec)
        return to;
    const std::filesystem::path real = std::filesystem::weakly_canonical(to, ec);
    return (ec || real.empty()) ? to : real;
}

inline bool replaceFileAtomic(const std::filesystem::path& from,
                              const std::filesystem::path& to,
                              std::error_code& ec)
{
    // Data first: the temp file's contents must be on the medium BEFORE the
    // rename publishes them, or the swap can expose blocks still in flight.
    if (!syncFileContents(from, ec)) return false;
    const std::filesystem::path dest = resolveReplaceTarget(to);
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), dest.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()),
                         std::system_category());
    return false;
#else
    std::filesystem::rename(from, dest, ec);
    if (ec) return false;
    syncParentDirectory(dest); // ...then the directory entry that names them
    return true;
#endif
}

/// Write `bytes` to `path` through the same durable temp + rename commit
/// every other write-back in POM2 uses: sibling temp file, contents synced to
/// the medium, then an atomic rename that publishes them.
///
/// Exists so a caller that has already SERIALISED something into memory can
/// commit it without a second format-aware writer. That split is the point:
/// the AI control server builds a snapshot blob under `stateMutex` (RAM only,
/// microseconds) and then lands it here with the lock released, instead of
/// holding the emulator and the window still across the write and its two
/// fsyncs.
///
/// Returns false with `ec` set; the target is untouched on every failure path.
inline bool writeFileAtomic(const std::filesystem::path& path,
                            const void* bytes, std::size_t length,
                            std::error_code& ec)
{
    ec.clear();
    const std::filesystem::path tmp = tempSiblingPath(path);
    if (!prepareTempPath(tmp, ec)) return false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        if (length)
            f.write(static_cast<const char*>(bytes),
                    static_cast<std::streamsize>(length));
        f.close();
        if (!f) {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    if (!replaceFileAtomic(tmp, path, ec)) {
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        return false;
    }
    return true;
}

} // namespace pom2

#endif // POM2_ATOMIC_FILE_REPLACE_H
