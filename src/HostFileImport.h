// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HostFileImport — what a host file BECOMES when it is put on an Apple
// volume: its Apple name, its type, its aux value and its bytes.
//
// One function, because three things have to be decided in one order and the
// two tools that put files on disks (pom2_dos33, pom2_prodos) must decide
// them the same way — the hazard HostFileTag.h records, one level up:
//
//   1. an AppleSingle wrapper says the type and aux itself (cc65's linker,
//      CiderPress and Cadius write one), and its data fork is the file (one
//      with a resource fork is refused unless the caller drops it). It
//      wins over the host name: the wrapper travels WITH the bytes, a name
//      is whatever the file was last saved as.
//   2. otherwise the host name says it, by the convention HostFileTag.h owns
//      (`NAME#TTAAAA`, else the extension).
//   3. an Applesoft file that is a TEXT LISTING is tokenized. `HELLO.bas`
//      can be either the program as the disk stores it (what `get` writes)
//      or the listing someone typed in an editor; only the second is text,
//      and a stored program always holds $00 line terminators, so the two
//      cannot be confused. Without this a listing went onto the disk as a
//      file of type A whose bytes were ASCII — it catalogued, and RUN gave
//      ?SYNTAX ERROR or worse.
//
// Text files (line endings, DOS 3.3's high bit) are NOT converted here: each
// file system has its own form, and its tool owns it.

#ifndef POM2_HOST_FILE_IMPORT_H
#define POM2_HOST_FILE_IMPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

inline constexpr std::uint8_t  kProdosTypeApplesoft = 0xFC;
inline constexpr std::uint16_t kApplesoftLoadAddress = 0x0801;

struct ImportedFile {
    std::vector<std::uint8_t> data;
    std::string   name;             ///< the Apple name the host name implies (tag / known extension off)
    std::uint8_t  type = 0;         ///< ProDOS file type
    std::uint16_t aux  = 0;         ///< ProDOS aux type (a BIN's load address)
    bool          auxStated = false;///< false: nothing said the aux, the caller's default applies
    std::string   note;             ///< what was done to the bytes, for the tool to print; may be empty
};

/// True when `data` reads as a typed listing rather than a stored program:
/// printable ASCII and line ends only, and a digit first.
bool looksLikeBasicListing(const std::vector<std::uint8_t>& data);

/// `hostPath`: only its file name is used. `tokenizeListings` false keeps an
/// Applesoft text listing as the bytes it is. False (with `error`) when an
/// AppleSingle wrapper is malformed, carries a resource fork and
/// `dropResourceFork` is false, or a listing does not tokenize — the message
/// names the line.
bool importHostFile(const std::string& hostPath, const std::vector<std::uint8_t>& bytes,
                    ImportedFile& out, std::string& error, bool tokenizeListings = true,
                    bool dropResourceFork = false);

}  // namespace pom2

#endif  // POM2_HOST_FILE_IMPORT_H
