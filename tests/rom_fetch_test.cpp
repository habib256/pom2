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

// RomFetch — the RetroBIOS mapping must stay a subset of what POM2
// actually probes. A destRel that is not in SystemProfile / RomCatalog /
// CharRomCatalog would download a file the ROM Status panel never shows
// and that no card ever opens. The planner is the other half: present
// files are skipped, missing ones are queued, and the source URL is the
// one the panel quotes.

#include "RomFetch.h"
#include "RomCatalog.h"
#include "CharRomCatalog.h"
#include "SystemProfile.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

std::set<std::string> knownDests()
{
    std::set<std::string> s;
    for (pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        for (const auto& c : cfg.romProbeOrder) s.insert(c);
        for (const auto& c : cfg.charRomProbeOrder) s.insert(c);
    }
    for (const auto& e : pom2::charRomCatalog()) {
        if (e.path && *e.path) s.insert(e.path);
    }
    for (const auto& e : pom2::romCatalog()) {
        for (const char* c : e.candidates) s.insert(c);
    }
    return s;
}

}  // namespace

int main()
{
    const auto known = knownDests();
    const auto& cat  = pom2::romFetchCatalog();

    expect(!cat.empty(), "catalog is not empty");
    expect(std::string(pom2::kRetroBiosSourceUrl).find("Abdess/retrobios")
               != std::string::npos,
           "source URL names Abdess/retrobios");

    std::set<std::string> dests;
    for (const auto& e : cat) {
        expect(e.destRel && *e.destRel, "every entry has a destRel");
        expect(e.label && *e.label, std::string("label for ") +
               (e.destRel ? e.destRel : "?"));
        expect(e.url && *e.url, std::string("url for ") +
               (e.destRel ? e.destRel : "?"));
        expect(e.expectedSize > 0, std::string("size for ") +
               (e.destRel ? e.destRel : "?"));
        if (e.destRel) {
            expect(dests.insert(e.destRel).second,
                   std::string("unique destRel ") + e.destRel);
            expect(known.count(e.destRel) == 1,
                   std::string(e.destRel) +
                   " is a path POM2 actually probes");
            expect(std::string(e.destRel).rfind("roms/", 0) == 0,
                   std::string(e.destRel) + " lives under roms/");
        }
        if (e.url) {
            const std::string url(e.url);
            expect(url.rfind(pom2::kRetroBiosRawPrefix, 0) == 0,
                   std::string("url is under the RetroBIOS raw prefix: ") +
                   url);
        }
        if (e.zipConcat) {
            expect(e.zipMember != nullptr,
                   std::string(e.destRel ? e.destRel : "?") +
                   " concat list needs a first zipMember");
        }
    }

    // Planner: nothing present → every entry; everything present → none;
    // a single hit removes just that dest.
    {
        const auto all = pom2::romsToFetch([](const char*) { return false; });
        expect(all.size() == cat.size(), "missing-everything queues the catalog");
    }
    {
        const auto none = pom2::romsToFetch([](const char*) { return true; });
        expect(none.empty(), "present-everything queues nothing");
    }
    {
        const char* keep = cat.front().destRel;
        const auto rest = pom2::romsToFetch([&](const char* destRel) {
            return destRel && keep && std::string(destRel) == keep;
        });
        expect(rest.size() == cat.size() - 1,
               "one present dest is skipped and only that one");
        for (const auto* e : rest)
            expect(std::string(e->destRel) != keep,
                   "skipped dest does not reappear");
    }

    // Size was the ONLY gate: a download that was the right length and the
    // wrong file (a mirror serving another revision, an error page padded
    // out) was installed over the user's roms/ and surfaced days later as
    // "it doesn't boot". Entries POM2 has a documented reference dump for now
    // carry that CRC and it is checked before the file is published.
    // (Bug hunt 2026-09-06 #H9.)
    {
        int withCrc = 0;
        for (const auto& e : cat) {
            if (!e.expectedCrc) continue;
            ++withCrc;
            expect(e.crcLabel && *e.crcLabel,
                   std::string(e.destRel ? e.destRel : "?") +
                   " names the dump its CRC identifies");
        }
        expect(withCrc >= 2,
               "the entries with a documented reference dump carry its CRC32");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("rom_fetch: %zu catalog entries, planner ok\n", cat.size());
    return 0;
}
