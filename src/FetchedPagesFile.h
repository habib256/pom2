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

// FetchedPagesFile — writing `Memory::FetchedPages` to the host: one file per
// video page the frame used, named by the convention HostFileTag.h owns
// (`NAME#06AAAA`: a BIN with its load address), so the Paint editor, CiderPress
// and any Apple II tool load them where they belong.
//
//   <base>-HGR#062000      hi-res, main     8 KB
//   <base>-HGRAUX#062000   hi-res, aux      8 KB   (double hi-res)
//   <base>-TEXT#060400     text / lo-res    1 KB
//   <base>-TEXTAUX#060400  text / lo-res, aux 1 KB (80 columns, double lo-res)
//
// File work only: call it with no machine lock held. Pinned by `beam_view`.

#ifndef POM2_FETCHED_PAGES_FILE_H
#define POM2_FETCHED_PAGES_FILE_H

#include "Memory.h"

#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

/// Writes the pages `pages` marks as used into `directory` (created when
/// missing). Returns the files written; on a failure returns what was written
/// before it and sets `error`.
std::vector<std::filesystem::path> saveFetchedPages(const Memory::FetchedPages& pages,
                                                    const std::filesystem::path& directory,
                                                    const std::string& base,
                                                    std::string& error);

} // namespace pom2

#endif // POM2_FETCHED_PAGES_FILE_H
