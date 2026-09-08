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

// Round-trip smoke test for POM2's SnapshotIO. Writes a synthetic file
// with two named sections, re-opens it, verifies header + payload bytes.

#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pom2_snapshot_smoke.snap";

    const std::vector<uint8_t> cpuPayload = { 0x12, 0x34, 0x56, 0x78 };
    const std::vector<uint8_t> memPayload(8192, 0xAA);

    // Write.
    {
        pom2::SnapshotWriter w(tmp.string());
        assert(w.good());
        w.writeSection("CPU", cpuPayload.data(), cpuPayload.size());
        w.writeSection("MEM", memPayload.data(), memPayload.size());
        assert(w.finish());
        assert(!fs::exists(tmp.string() + ".tmp"));
    }

    // Read.
    pom2::SnapshotReader r(tmp.string());
    assert(r.good());
    assert(r.version() == pom2::kSnapshotVersion);

    std::string name;
    uint32_t    len = 0;

    assert(r.nextSection(name, len));
    assert(name == "CPU");
    assert(len == cpuPayload.size());
    std::vector<uint8_t> got(len);
    r.readBytes(got.data(), len);
    assert(std::memcmp(got.data(), cpuPayload.data(), len) == 0);

    assert(r.nextSection(name, len));
    assert(name == "MEM");
    assert(len == memPayload.size());
    std::vector<uint8_t> got2(len);
    r.readBytes(got2.data(), len);
    assert(std::memcmp(got2.data(), memPayload.data(), len) == 0);

    // No more sections.
    assert(!r.nextSection(name, len));

    // ── Malformed-file hardening ───────────────────────────────────────────
    // A crafted snapshot whose section length field runs past EOF must be
    // rejected by nextSection() rather than handed to a consumer that would
    // size an allocation (or an over-read) from the attacker-controlled
    // length. Pins the fileSize_ bound in SnapshotReader::nextSection.
    auto writeRaw = [](const fs::path& p, const std::vector<uint8_t>& bytes) {
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        assert(f);
        if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    };
    // Valid 16-byte header: magic + version 2 (LE) + flags 0.
    const std::vector<uint8_t> header = {
        'P','O','M','2','S','N','A','P',
        0x02,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00
    };
    auto withSection = [&](std::vector<uint8_t> name8, uint32_t length,
                           size_t payloadBytes) {
        std::vector<uint8_t> f = header;
        name8.resize(8, 0);
        f.insert(f.end(), name8.begin(), name8.end());
        f.push_back(length & 0xFF);
        f.push_back((length >> 8) & 0xFF);
        f.push_back((length >> 16) & 0xFF);
        f.push_back((length >> 24) & 0xFF);
        f.insert(f.end(), payloadBytes, 0xCD);  // actual payload (may be short)
        return f;
    };

    const fs::path bad = fs::temp_directory_path() / "pom2_snapshot_bad.snap";

    // (a) Huge length (0xFFFFFFFF), zero payload → would be a 4 GB allocation.
    writeRaw(bad, withSection({'E','V','I','L'}, 0xFFFFFFFFu, 0));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());                       // header is valid…
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));           // …but the section is rejected.
        assert(!br.good());                      // reader latched into error.
        assert(br.error().find("exceeds file size") != std::string::npos);
    }

    // (b) Length declares 100 bytes but only 10 are present (truncated payload).
    writeRaw(bad, withSection({'B','I','G'}, 100, 10));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));
    }

    // (c) Header + name but the length field itself is truncated (2 of 4 bytes).
    {
        std::vector<uint8_t> f = header;
        const std::vector<uint8_t> nm = {'T','R','U','N','C',0,0,0};
        f.insert(f.end(), nm.begin(), nm.end());
        f.push_back(0x10); f.push_back(0x00);    // only 2 bytes of the u32 length
        writeRaw(bad, f);
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));
    }

    // (d) A length that exactly fills the file must still be accepted (the
    //     bound is "exceeds", not "equals" — guard against an off-by-one).
    writeRaw(bad, withSection({'F','I','T'}, 16, 16));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(br.nextSection(n, l));
        assert(n == "FIT" && l == 16);
        assert(!br.nextSection(n, l));           // and then clean EOF.
        assert(br.good());                       // EOF is not an error.
    }

    fs::remove(bad);

    // ── Machine identity in the header ────────────────────────────────────
    // The word after `version` used to be written as a reserved 0 and read
    // back with `(void)readU32()`. Nothing in the file said WHICH Apple the
    // state came from, while CPU/MEM/MEX all restore unconditionally — so a
    // //e snapshot loaded on a //c put PC and 64 KB of RAM against a
    // different ROM and memory map, freezing or silently running the wrong
    // code with no diagnostic. Snapshots are also the one artefact users
    // hand to each other, which is what made this worth a format field.
    {
        const std::uint32_t kId = 0xC0FFEEu;
        std::vector<std::uint8_t> blob;
        {
            pom2::SnapshotWriter w(blob, kId);
            w.writeSection("ID", nullptr, 0);
            assert(w.finish());
        }
        pom2::SnapshotReader r(blob.data(), blob.size());
        assert(r.good());
        assert(r.machineId() == kId);

        // A writer given no identity records 0, and 0 must keep loading:
        // that is every snapshot taken before this field existed, plus the
        // rewind ring's in-memory frames (the ring is cleared on a profile
        // switch, so it needs no stamp).
        std::vector<std::uint8_t> legacy;
        {
            pom2::SnapshotWriter w(legacy);
            w.writeSection("ID", nullptr, 0);
            assert(w.finish());
        }
        pom2::SnapshotReader lr(legacy.data(), legacy.size());
        assert(lr.good());
        assert(lr.machineId() == 0);

        // Mutating the word in a valid file is seen by the reader — this is
        // the value the CLI and AI-server guards compare, so a snapshot
        // whose identity was altered is refused rather than applied.
        std::vector<std::uint8_t> tampered = blob;
        assert(tampered.size() >= 16);
        tampered[12] = 0x21; tampered[13] = 0x43;
        tampered[14] = 0x65; tampered[15] = 0x87;
        pom2::SnapshotReader tr(tampered.data(), tampered.size());
        assert(tr.good());                       // still a valid container…
        assert(tr.machineId() == 0x87654321u);   // …with a different machine.
        assert(tr.machineId() != kId);
    }

    // Cleanup.
    fs::remove(tmp);
    std::printf("SnapshotIO smoke: OK (round-trip + malformed-file hardening"
                " + machine identity)\n");
    // ── Two writers on one path publish their OWN bytes ──────────────────
    // (bug hunt #9.) The temp name was a fixed `<path>.tmp`, the one every
    // POM2 instance derives, so two writers opened the same temp with trunc:
    // whoever renamed first published the other's bytes and still reported
    // success, and the loser's finish() failed. tempSiblingPath is unique per
    // process and per call.
    {
        const fs::path both = fs::temp_directory_path() / "pom2_snapshot_two_writers.snap";
        std::error_code ec;
        fs::remove(both, ec);
        const std::vector<uint8_t> aBytes(92, 0x41), bBytes(40000, 0x42);
        pom2::SnapshotWriter a(both.string(), 1);
        pom2::SnapshotWriter b(both.string(), 2);
        assert(a.good() && b.good());
        a.writeSection("AAAAAAAA", aBytes.data(), aBytes.size());
        b.writeSection("BBBBBBBB", bBytes.data(), bBytes.size());
        auto firstSection = [&]() {
            pom2::SnapshotReader r(both.string());
            std::string nm; uint32_t ln = 0;
            if (!r.good() || !r.nextSection(nm, ln)) return std::string("<bad>");
            return nm;
        };
        assert(a.finish() && "the first writer's commit failed");
        assert(firstSection() == "AAAAAAAA" &&
               "the first writer published the OTHER writer's snapshot");
        assert(b.finish() && "the second writer's commit failed — its temp was taken");
        assert(firstSection() == "BBBBBBBB");
        assert(!fs::exists(both.string() + ".tmp"));
        fs::remove(both, ec);
        std::printf("  two writers on one path each publish their own bytes: OK\n");
    }

    return 0;
}
