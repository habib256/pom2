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

// ZipMember.h — read one member out of a zip archive held in memory.
//
// RomFetch unpacks MAME romsets. It used to shell out to `unzip`, falling
// back to `tar` — but GNU tar, the one a Linux box without unzip has, does
// not read zip at all, so the fallback failed every romset there (bug hunt
// 2026-09-16). Stored and deflated members cover every zip RetroBIOS
// serves; the inflater is stb_image's, which POM2 already bundles.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// Extract `member` (its exact name in the archive) from `zip` into `out`.
/// Returns false with `err` set when the member is absent, encrypted, uses a
/// method other than stored/deflate, claims more than `maxBytes`, or fails
/// its CRC-32. `unsupported` is set in the method/encryption case only, so
/// a caller can hand that archive to a host tool instead.
bool readZipMember(const std::vector<std::uint8_t>& zip,
                   const std::string& member,
                   std::vector<std::uint8_t>& out,
                   std::string& err,
                   std::size_t maxBytes,
                   bool* unsupported = nullptr);

}  // namespace pom2
