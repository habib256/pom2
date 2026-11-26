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

// MediaSource — a disk given as an http(s) URL, a .gz, a .zip, a .7z or an exchange
// file (a ShrinkIt archive, a DiskCopy 4.2 image, an A2R flux capture:
// ExchangeMedia.h), turned
// into a plain local image the rest of POM2 mounts as it mounts any file.
//
// The TNFS shape (TnfsMedia.h), for the same reasons: no second mount path,
// and writes go somewhere honest. A URL is downloaded with the host's curl
// (as RomFetch does); a .gz is inflated, a .zip gives up its first disk
// image (stb's inflater, ZipMember.h), and so does a .7z (the LZMA SDK,
// SevenZipArchive.h). The result lands in the media cache
// under a name keyed on its source, and a second mount of the same source
// reuses it — so what the guest writes stays in the cache copy across
// sessions, and the archive or the server is never written to (izapple2's
// "overlay" idea, without a second file format). Everything here is file
// and network I/O: callers run it before taking stateMutex.

#ifndef POM2_MEDIA_SOURCE_H
#define POM2_MEDIA_SOURCE_H

#include <atomic>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// http:// or https:// (case-insensitive).
bool isHttpUrl(const std::string& source);
/// Ends in .gz, .zip or .7z (case-insensitive).
bool isCompressedImage(const std::string& source);
/// A local exchange file a drive cannot take as it is (ExchangeMedia.h):
/// ShrinkIt .shk/.sdk/.bxy/.sea, DiskCopy .dc/.dc42, Applesauce .a2r, and a
/// .image whose header is DiskCopy 4.2's (read from the file).
bool isExchangeMedia(const std::string& source);
/// Any of them: a source resolveMediaSource has work to do on.
inline bool needsMediaResolve(const std::string& source)
{
    return isHttpUrl(source) || isCompressedImage(source) || isExchangeMedia(source);
}

/// `<user data>/media_cache`.
std::filesystem::path mediaCacheDir();

/// The disks unpacked from the same source as `image`, itself included, in
/// name order: what a drive holding `image` can change to. An image this
/// module did not put in `cacheDir` is a set of one.
std::vector<std::string> mediaSetOf(const std::string& image, const std::filesystem::path& cacheDir);
/// The name to show for a disk: a cached image's file name without the key
/// MediaSource put in front of it.
std::string mediaDisplayName(const std::string& image);

/// Where a URL's download is cached (before any archive extension the
/// download adds: a zip or gzip served under a name without one).
std::filesystem::path mediaUrlCachePath(const std::string& url,
                                        const std::filesystem::path& cacheDir);
/// The first bytes of a download are an HTML / XML page (a soft 404, a
/// login wall), not a disk: refused, never cached.
bool mediaBodyIsWebPage(const std::vector<uint8_t>& head);
/// ".zip", ".gz" or ".7z" when the bytes start like one, else nullptr.
const char* mediaArchiveExtension(const std::vector<uint8_t>& head);

/// The refusal shown when `archive` (a .zip, a .7z, a .gz or a URL) was given as a
/// disk and could not be opened: what failed, in the archive's own words —
/// not "this is no disk image", which a .gz that failed its CRC is not.
std::string explainMediaSourceRefusal(const std::string& archive,
                                      const std::string& error);

struct MediaSourceResult {
    bool        ok = false;
    std::string localPath;   ///< the image to mount (== source when nothing to do)
    bool        fromCache = false;
    std::string note;        ///< what was done, for the status line
    std::string error;
};

/// Resolve `source` into `cacheDir`. A source needing nothing comes back as
/// itself. `abort`, when given, is polled during a download.
MediaSourceResult resolveMediaSource(const std::string& source,
                                     const std::filesystem::path& cacheDir,
                                     const std::atomic<bool>* abort = nullptr);

} // namespace pom2

#endif // POM2_MEDIA_SOURCE_H
