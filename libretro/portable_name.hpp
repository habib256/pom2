// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
// The name of a disk's private copy, the same on every platform.
#pragma once

#include <string>

namespace pom2::libretro {

/// A name every file system and every frontend takes alike, and the same on
/// every platform, so a save folder synced between a PC and a phone matches:
/// printable ASCII (Latin letters lose their accents, "Énigme" is "Enigme";
/// any other character is "_"), none of <>:"/\|?*, no trailing dot or space
/// (Windows trims those), no device name (CON, NUL, COM1...), at most 64
/// bytes. A name that had to change is followed by " [<8 hex>]", the start
/// of the hash of the name with its accents composed, so a decomposed name
/// (macOS's HFS+ stores "E" + U+0301) and a composed one give one copy.
std::string portableName(const std::string& utf8);

/// The extension kept as it is when it is short and alphanumeric, else "".
std::string portableExtension(const std::string& utf8Extension);

/// A file name from somewhere else (a zip member, a gzip header) as a name
/// to write on this host: UTF-8 when it is valid UTF-8, else read as CP437
/// (a zip whose UTF-8 flag is clear), then portableName + portableExtension.
/// No folder, no drive letter ("D:x.dsk" is "D_x.dsk [...]"), no device name.
std::string hostLeafName(const std::string& raw);

/// The UTF-8 text with its accents composed as NFC composes them (Latin,
/// Cyrillic, Greek, Vietnamese, kana, Hangul): what two spellings of one
/// name are compared by.
std::string composedUtf8(const std::string& utf8);

} // namespace pom2::libretro
