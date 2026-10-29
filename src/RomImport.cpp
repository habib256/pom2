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

#include "RomImport.h"
#include "ZipMember.h"

#include "Logger.h"
#include "ResourcePaths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

bool readBounded(const fs::path& path, std::size_t cap,
                 std::vector<std::uint8_t>& out, std::string& err)
{
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    if (ec) { err = "cannot stat: " + ec.message(); return false; }
    if (n > cap) {
        err = std::to_string(n) + " bytes — too large to be a ROM";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot read"; return false; }
    out.resize(static_cast<std::size_t>(n));
    if (n && !f.read(reinterpret_cast<char*>(out.data()),
                     static_cast<std::streamsize>(n))) {
        err = "short read";
        return false;
    }
    return true;
}

bool hasZipExtension(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".zip";
}

bool looksLikeZip(const std::vector<std::uint8_t>& b)
{
    return b.size() >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4;
}

struct Found {
    const RomFetchEntry*      entry;
    std::string               source;
    std::vector<std::uint8_t> bytes;
};

/// Every catalogued dump inside one archive: each member on its own, then
/// the images the catalogue builds by concatenating members.
void scanArchive(const std::vector<std::uint8_t>& zip, const std::string& name,
                 std::vector<Found>& found, std::string& err)
{
    std::uintmax_t unpacked = 0;
    if (!zipUnpackedSizeWithinCap(zip, unpacked, err)) return;
    std::vector<std::string> members;
    if (!listZipMembers(zip, members, err)) return;

    for (const auto& m : members) {
        if (m.empty() || m.back() == '/') continue;
        std::vector<std::uint8_t> bytes;
        std::string memberErr;
        // A member too big, encrypted or damaged is simply not a ROM we know.
        if (!readZipMember(zip, m, bytes, memberErr, kMaxRomImportFileBytes))
            continue;
        for (const RomFetchEntry* e : identifyRom(bytes))
            found.push_back({ e, name + ":" + m, bytes });
    }

    const std::set<std::string> have(members.begin(), members.end());
    for (const auto& e : romFetchCatalog()) {
        if (!e.zipMember || !e.zipConcat || !have.count(e.zipMember)) continue;
        std::vector<std::uint8_t> bytes;
        std::string memberErr;
        bool ok = readZipMember(zip, e.zipMember, bytes, memberErr,
                                kMaxRomImportFileBytes);
        std::string source = name + ":" + e.zipMember;
        for (const char* const* m = e.zipConcat; ok && *m; ++m) {
            std::vector<std::uint8_t> more;
            ok = have.count(*m) &&
                 readZipMember(zip, *m, more, memberErr, kMaxRomImportFileBytes);
            bytes.insert(bytes.end(), more.begin(), more.end());
            source += std::string("+") + *m;
        }
        if (!ok) continue;
        const auto ids = identifyRom(bytes);
        if (std::find(ids.begin(), ids.end(), &e) != ids.end())
            found.push_back({ &e, source, std::move(bytes) });
    }
}

void install(const Found& f, const fs::path& destRoot, RomImportItem& item)
{
    item.source  = f.source;
    item.destRel = f.entry->destRel;
    item.label   = f.entry->label;

    // Already what the machine loads: nothing to write, nothing to back up.
    const std::string resolved = findResource(f.entry->destRel);
    if (!resolved.empty()) {
        std::vector<std::uint8_t> cur;
        std::string readErr;
        if (readBounded(resolved, kMaxRomImportFileBytes, cur, readErr) &&
            cur == f.bytes) {
            item.alreadyPresent = true;
            item.installedAt    = resolved;
            return;
        }
    }

    const fs::path dest = destRoot / fs::path(f.entry->destRel).filename();
    std::string err;
    if (!installFetchedRom(dest, *f.entry, f.bytes, err, item.backup)) {
        item.error = err;
        return;
    }
    // Saved is not USED: an earlier search root that could not be the
    // destination still wins findResource (the same check the fetch makes).
    const std::string now = findResource(f.entry->destRel);
    std::error_code eqEc;
    if (!now.empty() && !fs::equivalent(now, dest, eqEc)) {
        item.error = "saved " + dest.string() + ", but " + now +
                     " is found first — remove or replace it";
        return;
    }
    item.installedAt = dest.string();
}

}  // namespace

std::vector<const RomFetchEntry*> identifyRom(const std::vector<std::uint8_t>& bytes)
{
    std::vector<const RomFetchEntry*> out;
    if (bytes.empty()) return out;
    std::string digest;   // computed once, and only if a size matches
    for (const auto& e : romFetchCatalog()) {
        if (!e.expectedSha256 || !*e.expectedSha256) continue;
        if (e.expectedSize && bytes.size() != e.expectedSize) continue;
        if (digest.empty()) digest = sha256Hex(bytes.data(), bytes.size());
        if (digest == e.expectedSha256) out.push_back(&e);
    }
    return out;
}

bool romImportCandidate(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    if (hasZipExtension(path)) return true;
    const auto n = fs::file_size(path, ec);
    return !ec && n > 0 && n <= kMaxRomImportFileBytes;
}

RomImportResult importRoms(const std::vector<fs::path>& inputs,
                           const fs::path& destRoot)
{
    RomImportResult r;
    std::vector<Found> found;

    for (const auto& in : inputs) {
        const std::string name = in.filename().string();
        const std::size_t before = found.size();
        std::vector<std::uint8_t> bytes;
        std::string err;
        const std::size_t cap = hasZipExtension(in) ? kMaxUnpackedZipBytes
                                                    : kMaxRomImportFileBytes;
        if (!readBounded(in, cap, bytes, err)) {
            r.unrecognised.push_back(name + " (" + err + ")");
            continue;
        }
        if (looksLikeZip(bytes)) {
            scanArchive(bytes, name, found, err);
        } else {
            for (const RomFetchEntry* e : identifyRom(bytes))
                found.push_back({ e, name, bytes });
        }
        if (found.size() == before)
            r.unrecognised.push_back(err.empty() ? name : name + " (" + err + ")");
    }

    std::set<std::string> done;
    for (const auto& f : found) {
        if (!done.insert(f.entry->destRel).second) continue;
        RomImportItem item;
        install(f, destRoot, item);
        if (!item.error.empty()) {
            ++r.failed;
            log().warn("ROM", "import of " + item.source + " as " +
                       item.destRel + " refused: " + item.error);
        } else if (item.alreadyPresent) {
            ++r.alreadyPresent;
        } else {
            ++r.installed;
            log().info("ROM", "imported " + item.source + " as " + item.installedAt +
                       (item.backup.empty() ? std::string()
                                            : " (previous file kept as " + item.backup + ")"));
        }
        r.items.push_back(std::move(item));
    }

    if (r.items.empty()) {
        r.summary = "No known ROM dump in " +
                    (inputs.size() == 1 ? inputs.front().filename().string()
                                        : std::to_string(inputs.size()) + " files") +
                    " — POM2 only installs dumps it can identify.";
    } else {
        r.summary = "ROM import: " + std::to_string(r.installed) + " installed";
        if (r.alreadyPresent) r.summary += ", " + std::to_string(r.alreadyPresent) + " already present";
        if (r.failed)         r.summary += ", " + std::to_string(r.failed) + " refused";
        if (!r.unrecognised.empty())
            r.summary += ", " + std::to_string(r.unrecognised.size()) + " unrecognised";
        r.summary += r.installed ? " — loaded at the next profile switch or restart."
                                 : ".";
    }
    return r;
}

}  // namespace pom2
