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

// PrinterRender — a printer's byte stream to files, with no GUI.
//
// The ImageWriter panel prints live; a test bench wants the same result from
// a captured stream: the raw bytes on disk (to diff against a real printer's
// capture) and the pages rendered as PNG. This drives a private, offline
// `ImageWriter` — any of its models, the ImageWriter II and the Epson FX-80
// among them — over the whole stream at once and writes each finished page.
// Both writes go through the atomic, durable commit (AtomicFileReplace.h).
//
// Where the bytes come from is the caller's business: a card's spool
// (`SuperSerialCard::drainPrinterSpoolFrom`, `GrapplerCard::drainSpoolFrom`,
// `PrinterCard::drainSpoolFrom`) or `/printer/spool`.

#ifndef POM2_PRINTER_RENDER_H
#define POM2_PRINTER_RENDER_H

#include "ImageWriter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// Write `bytes` to `path`, atomically. False with `error` on failure.
bool savePrinterBytes(const std::string& path, const std::vector<uint8_t>& bytes,
                      std::string& error);

/// Render `bytes` on a fresh `model` printer and write each page as
/// `<pathPrefix>-<N>.png` (N from 1). A page left partly printed at the end
/// is ejected and written too. Returns the number of pages written, or -1
/// with `error` set. `dpi` 0 keeps the printer's default.
int renderPrinterBytesToPng(const std::vector<uint8_t>& bytes, IwModel model,
                            const std::string& pathPrefix, std::string& error,
                            int dpi = 0);

}  // namespace pom2

#endif  // POM2_PRINTER_RENDER_H
