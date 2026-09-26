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

#include "PrinterRender.h"

#include "AtomicFileReplace.h"

#include <cstddef>
#include <filesystem>
#include <system_error>

// A PRIVATE copy of the PNG encoder. The application's one lives in
// Pom2HgrPaintHost.cpp, which the core library does not link, and every test
// that writes PNGs defines its own; STB_IMAGE_WRITE_STATIC makes this one
// internal to this file so it can collide with neither, and a program linking
// only the core library gets PNG output without supplying an encoder.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace pom2 {

bool savePrinterBytes(const std::string& path, const std::vector<uint8_t>& bytes,
                      std::string& error)
{
    std::error_code ec;
    if (!writeFileAtomic(path, bytes.data(), bytes.size(), ec)) {
        error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    return true;
}

int renderPrinterBytesToPng(const std::vector<uint8_t>& bytes, IwModel model,
                            const std::string& pathPrefix, std::string& error,
                            int dpi)
{
    namespace fs = std::filesystem;
    ImageWriter printer;
    printer.setModel(model);
    if (dpi > 0) printer.setDpi(dpi);
    printer.printBytes(bytes.data(), bytes.size());
    if (!printer.currentPageBlank()) printer.formFeed();

    if (printer.droppedPageCount() != 0) {
        error = "the stream ran to more pages than the printer keeps (" +
                std::to_string(printer.droppedPageCount()) + " dropped)";
        return -1;
    }
    int written = 0;
    std::vector<uint8_t> rgba;
    for (std::size_t i = 0; i < printer.completedPageCount(); ++i) {
        const auto& page = printer.completedPage(i);
        ImageWriter::pageToRgba(page, rgba);
        const fs::path out = pathPrefix + "-" + std::to_string(i + 1) + ".png";
        const fs::path tmp = tempSiblingPath(out);
        std::error_code ec;
        bool ok = prepareTempPath(tmp, ec) &&
                  rgba.size() >= static_cast<std::size_t>(page.w) *
                                     static_cast<std::size_t>(page.h) * 4 &&
                  stbi_write_png(tmp.string().c_str(), page.w, page.h, 4,
                                 rgba.data(), page.w * 4) != 0;
        if (ok) ok = replaceFileAtomic(tmp, out, ec);
        if (!ok) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            error = "cannot write " + out.string() +
                    (ec ? ": " + ec.message() : std::string());
            return -1;
        }
        ++written;
    }
    return written;
}

}  // namespace pom2
