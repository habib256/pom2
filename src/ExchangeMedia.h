// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ExchangeMedia — the exchange formats a drive cannot take as they are,
// turned into images it can: the same shape as a .zip or a .gz in
// MediaSource.h (a local image in a cache, the original never written), and
// the one place both the desktop (MediaSource) and the libretro core
// (archive_content) go through, so the two read a file the same way.
//
//   .shk .sdk .bxy .sea  ShrinkIt (NuFxArchive.h). A disk record becomes
//                        its disk (.po, or .hdv past 800 KB); file records
//                        become files on a fresh ProDOS volume, with their
//                        types — the smallest of 140 KB, 800 KB or a hard
//                        disk that holds them. A ProDOS volume POM2 builds
//                        carries no boot code (ProDOSDisk::format): it is a
//                        data disk, for a drive beside a boot disk.
//   .a2r                 An Applesauce flux capture of a 5.25" disk (A2rFlux.h):
//                        one revolution a quarter track, as a WOZ 2.1 with
//                        FLUX tracks.
//   .dc .dc42 .image     DiskCopy 4.2 (DiskCopy42.h): its sectors as a .po.
//                        A .image is converted only when its header is
//                        DiskCopy's; a raw .image mounts as it is.

#ifndef POM2_EXCHANGE_MEDIA_H
#define POM2_EXCHANGE_MEDIA_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::exchange {

struct Image {
    std::string name;            ///< a file name with the extension that picks the drive
    std::vector<uint8_t> bytes;
};

/// By name only: an extension that is always an exchange format.
bool hasExchangeExtension(const std::string& fileName);

/// By name and first bytes (84 are enough): an exchange file, including a
/// .image whose header is DiskCopy 4.2's.
bool isExchangeFile(const std::string& fileName, const std::vector<uint8_t>& head);

/// Convert. `images` gets every disk the file holds, in archive order (a
/// ShrinkIt archive may hold several); `note` says what was done. False with
/// the reason when nothing mountable comes out.
bool convert(const std::string& fileName, const std::vector<uint8_t>& bytes,
             std::vector<Image>& images, std::string& note, std::string& error);

/// A legal ProDOS name made from any name: letters, digits and periods,
/// starting with a letter, at most 15, upper case. Exposed for the tests.
std::string prodosName(const std::string& name);

} // namespace pom2::exchange

#endif // POM2_EXCHANGE_MEDIA_H
