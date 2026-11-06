// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HostFileImport — see the header.

#include "HostFileImport.h"

#include "AppleSingle.h"
#include "ApplesoftTokenizer.h"
#include "HostFileTag.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace pom2 {

bool looksLikeBasicListing(const std::vector<std::uint8_t>& data)
{
    bool digitFirst = false, seenInk = false;
    for (const std::uint8_t b : data) {
        const bool lineEnd = b == '\n' || b == '\r';
        // A stored program is full of $00 terminators and token bytes >= $80;
        // one of either and this is not something a person typed.
        if (!lineEnd && b != '\t' && (b < 0x20 || b > 0x7E)) return false;
        if (!seenInk && b != ' ' && b != '\t' && !lineEnd) {
            seenInk    = true;
            digitFirst = b >= '0' && b <= '9';
        }
    }
    return digitFirst;
}

bool importHostFile(const std::string& hostPath, const std::vector<std::uint8_t>& bytes,
                    ImportedFile& out, std::string& error, bool tokenizeListings)
{
    ImportedFile file;
    const std::filesystem::path host(hostPath);
    std::string base = host.filename().string();

    // The name first: it gives the Apple name in every case, and the type
    // when nothing better does. A tagged name keeps its extension —
    // `HELLO.BAS#FC0801` is the file `HELLO.BAS`.
    std::uint8_t  namedType = 0;
    std::uint16_t namedAux  = 0;
    const bool tagged = stripHostFileTag(base, namedType, namedAux);
    if (!tagged) {
        namedType = prodosTypeFromExtension(host.extension().string());
        base.resize(base.size() - strippedExtensionLength(base));
    }
    file.name      = base;
    file.type      = namedType;
    file.aux       = namedAux;
    file.auxStated = tagged;
    file.data      = bytes;

    if (applesingle::looksLikeAppleSingle(bytes.data(), bytes.size())) {
        applesingle::File wrapped;
        if (!applesingle::decode(bytes, wrapped, error)) {
            error = "AppleSingle: " + error;
            return false;
        }
        file.data = std::move(wrapped.data);
        file.note = "AppleSingle unwrapped";
        if (wrapped.hasProdosInfo) {
            file.type      = static_cast<std::uint8_t>(wrapped.fileType);
            file.aux       = static_cast<std::uint16_t>(wrapped.auxType);
            file.auxStated = true;
            char text[40];
            std::snprintf(text, sizeof text, " (type $%02X, aux $%04X)", file.type, file.aux);
            file.note += text;
        }
        // `prog.as` / `PROG.AS` is the wrapper's own extension, not part of
        // the Apple name.
        const std::string ext = host.extension().string();
        if (!tagged && (ext == ".as" || ext == ".AS") && file.name.size() > 3)
            file.name.resize(file.name.size() - 3);
    }

    if (tokenizeListings && file.type == kProdosTypeApplesoft &&
        looksLikeBasicListing(file.data)) {
        const std::uint16_t load = file.auxStated ? file.aux : kApplesoftLoadAddress;
        std::vector<std::uint8_t> program;
        std::vector<applesoft::TokenizeError> errors;
        const std::string text(file.data.begin(), file.data.end());
        if (!applesoft::tokenize(text, program, errors, load)) {
            error = "not a usable Applesoft listing";
            for (std::size_t i = 0; i < errors.size() && i < 3; ++i)
                error += "\n  line " + std::to_string(errors[i].sourceLine) + ": " +
                         errors[i].message;
            if (errors.size() > 3)
                error += "\n  … and " + std::to_string(errors.size() - 3) + " more";
            return false;
        }
        file.data      = std::move(program);
        file.aux       = load;
        file.auxStated = true;
        if (!file.note.empty()) file.note += "; ";
        file.note += "listing tokenized";
    }
    out = std::move(file);
    return true;
}

}  // namespace pom2
