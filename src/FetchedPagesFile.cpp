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

#include "FetchedPagesFile.h"

#include "AtomicFileReplace.h"
#include "HostFileTag.h"

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace pom2 {

std::vector<std::filesystem::path> saveFetchedPages(const Memory::FetchedPages& pages,
                                                    const std::filesystem::path& directory,
                                                    const std::string& base,
                                                    std::string& error)
{
    std::vector<std::filesystem::path> written;
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto save = [&](bool used, const char* what, const uint8_t* bytes,
                          std::size_t size, uint16_t address) {
        if (!used || !error.empty()) return;
        const std::filesystem::path out =
            directory / (base + "-" + what + hostFileTag(0x06, address));
        if (writeFileAtomic(out, bytes, size, ec)) written.push_back(out);
        else error = out.string() + ": " + ec.message();
    };
    save(pages.usedHiRes,    "HGR",     pages.hiResMain.data(), pages.hiResMain.size(), 0x2000);
    save(pages.usedHiResAux, "HGRAUX",  pages.hiResAux.data(),  pages.hiResAux.size(),  0x2000);
    save(pages.usedText,     "TEXT",    pages.textMain.data(),  pages.textMain.size(),  0x0400);
    save(pages.usedTextAux,  "TEXTAUX", pages.textAux.data(),   pages.textAux.size(),   0x0400);
    return written;
}

} // namespace pom2
