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

// The compatibility corpus (VALIDATION-006): what `pom2_corpus` reads and
// writes, with no machine in it, so the format is pinned by a test that boots
// nothing (`corpus_manifest`).
//
// Two files. The MANIFEST declares titles — a profile, the devices and media,
// and a script of `run` / `key` / `type` / `check` steps. The EXPECTATIONS
// file records, per title and check, the hash of the frame the check saw and
// the firmware fingerprint of the run that recorded it. A check passes when
// the same firmware produces the same frame; a different firmware is a skip
// with a reason, never a failure, because the frame is a function of the
// dumps as much as of POM2. Syntax: tests/corpus/README.md.

#ifndef POM2_CORPUS_MANIFEST_H
#define POM2_CORPUS_MANIFEST_H

#include <cstdint>
#include <istream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pom2::corpus {

/// One device line. `kind` is diskii / smartport / hdd (media) or
/// mockingboard / mouse / rgb (cards). `unit` is the Apple's drive number
/// (1 or 2) for a media line, `slot` the card's slot for a card line.
struct Device {
    std::string kind;
    int         slot = 0;
    int         unit = 0;
    std::string model;     ///< card variant, "" = the default
    std::string media;     ///< path relative to the corpus root
    std::string sha256;    ///< expected SHA-256 of `media`, "" = unchecked
    int         line = 0;
    bool        deferred = false; ///< mounted by a swap step, not at boot
};

struct Step {
    enum class Kind { Run, Key, Text, Check, Swap };
    Kind          kind = Kind::Run;
    std::size_t   deviceIndex = 0; ///< Swap: entry in Title::devices
    double        seconds = 0;   ///< Run: emulated seconds
    std::uint8_t  key = 0;       ///< Key: the 7-bit code
    std::string   text;          ///< Text: typed as pasted; Check: its name
    int           line = 0;
};

struct Title {
    std::string              id;
    std::string              profile;   ///< canonical key (`iie-pal`, …)
    std::vector<std::string> tags;
    std::vector<Device>      devices;
    int                      bootSlot = 0;   ///< 0 = cold boot, the ROM's own scan
    std::vector<Step>        steps;
    int                      line = 0;

    bool hasTag(const std::string& tag) const;
};

struct Manifest {
    std::vector<Title> titles;
};

/// Parse a manifest. On failure `error` names the line and nothing else of
/// `out` should be trusted. Rejects: an unknown keyword, a device or step
/// before any `title`, a duplicate title id or check name, a malformed
/// number, a unit other than 1-2, a slot outside 1-7, an unknown key name,
/// a title with no `check`.
bool parseManifest(std::istream& in, Manifest& out, std::string& error);

/// A recorded check: the frame hash and the firmware it was taken with.
struct Expected {
    std::string hash;
    std::string firmware;
};
/// Keyed by (title id, check name).
using Expectations = std::map<std::pair<std::string, std::string>, Expected>;

bool parseExpectations(std::istream& in, Expectations& out, std::string& error);
/// The file `--bless` writes: a header comment, then one sorted line per
/// check, so a re-bless diffs line by line.
std::string formatExpectations(const Expectations& expectations);

/// `RETURN`, `LEFT`, `ESC`, … a single printable character, or `$hh`.
bool parseKeyToken(const std::string& token, std::uint8_t& out);

/// FNV-1a 64 over the frame's size and its pixels (0xAABBGGRR, as
/// `pom2::FramebufferView` packs them, hashed byte by byte, little end
/// first, so the value does not depend on the host's byte order).
std::uint64_t frameHash(const std::uint32_t* pixels, int width, int height);
std::string hashHex(std::uint64_t h);

}  // namespace pom2::corpus

#endif  // POM2_CORPUS_MANIFEST_H
