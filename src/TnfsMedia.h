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

#pragma once

// TnfsMedia — fetch a disk image from a TNFS server into a local file.
//
// This is what makes `TnfsClient` reachable. TNFS is how the FujiNet world
// shares Apple II media (`tnfs.fujinet.online` carries a large library), and
// POM2 has had a tested client for it that nothing called.
//
// WHY A FILE, NOT A BUFFER. Handing the bytes straight to a card would mean a
// second mount path beside the one every other image uses, and a write-back
// story with nowhere to write: TNFS is served read-only here. Landing the
// image in a local cache file instead means the rest of POM2 does not learn a
// new concept — `classifyDiskForSlot` picks the drive, the two-phase mount in
// MediaMount.h moves it in off the state mutex, and writes go to the local
// copy, which is the only honest place for them.
//
// URL shape: `tnfs://host[:port]/path/to/image.po`. The scheme is optional,
// so `tnfs.fujinet.online/Apple II/…` works too.

#include <atomic>
#include <cstdint>
#include <string>

namespace pom2 {

/// Bounds on one fetch. Without them the transfer was limited only by the
/// client's PER-REQUEST timeout, and a 32 MB image is ~64 000 requests: a
/// slow or half-dead server produced a black window for the better part of an
/// hour with no way to stop it (the positional-URL fetch runs BEFORE the
/// window exists, so there is not even a UI to close).
struct TnfsFetchLimits {
    /// Whole-transfer deadline. Reached → the fetch fails with a message
    /// naming how far it got; nothing partial is published, because the cache
    /// file only appears through writeFileAtomic at the very end.
    int  deadlineSeconds = 180;
    /// Largest image to pull. Defaults to `kTnfsMaxImageBytes`; 0 = default.
    std::uint32_t maxBytes = 0;
    /// Polled between chunks. Set it from another thread (or a signal
    /// handler) to abandon the transfer. Null = no cancellation.
    const std::atomic<bool>* abort = nullptr;
};

/// Where a fetched image lands, and what it cost.
struct TnfsFetchResult {
    bool        ok = false;
    std::string localPath;      ///< the cache file, ready for the normal mount
    std::uint32_t bytes = 0;
    bool        usedTcp = false;///< diagnostic: false means UDP, see TnfsClient.h
    bool        fromCache = false;
    std::string error;
};

/// A TNFS URL, split. Returns false when `url` is not one.
bool parseTnfsUrl(const std::string& url, std::string& host, std::uint16_t& port,
                  std::string& path);

/// Fetch `url` into `cacheDir`, reusing an existing file of the right size
/// rather than pulling it again. `cacheDir` is created if missing.
///
/// Blocking, and bounded only by the client's own per-request timeout — so
/// call it from the UI thread BETWEEN frames, never with `stateMutex` held.
/// A 140 KB floppy is ~270 round trips; the caller is told the size first so
/// it can say no.
TnfsFetchResult tnfsFetchImage(const std::string& url,
                               const std::string& cacheDir,
                               const TnfsFetchLimits& limits = {});

/// Largest image this will pull. ProDOS's own ceiling — a 32 MB volume is
/// ~64 000 round trips, which is already an unreasonable thing to do over the
/// internet, and anything larger is not a disk POM2 can mount anyway.
inline constexpr std::uint32_t kTnfsMaxImageBytes = 32u * 1024u * 1024u;

} // namespace pom2
