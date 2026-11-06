// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three ways a mounted 5.25" image used to lose data under a success message,
// and the behaviour that replaces each (DiskImage_WriteBack.cpp,
// MediaAutosave.h "External change", DiskImage::integrityWarning):
//
//   1. the image file changed by another program while mounted — the commit
//      replaced it (and, for a sector format, mixed the two states);
//   2. a damaged WOZ — refused outright on a CRC mismatch, mounted WRITABLE
//      with track records silently skipped, and stamped CRC 0 on save;
//   3. a dirty track of which some sectors no longer decode — the good ones
//      were written, the bad ones kept the file's old bytes, "Saved".
//
// Each fix was disabled in turn (2026-11-05) and this test failed every time:
// never divert, trust the file time alone, accept a partly decoded track,
// forget the damaged track records.

#include "DiskImage.h"
#include "MediaAutosave.h"
#include "SyncGapWrite.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

fs::path workDir()
{
    static const fs::path dir = [] {
        fs::path d = fs::temp_directory_path() /
                     ("pom2_media_external_change_" + std::to_string(std::rand()));
        fs::remove_all(d);
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

std::vector<uint8_t> pattern(uint8_t seed)
{
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 13 + seed) & 0xFF);
    return bytes;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

/// An external rewrite, with a modification time that cannot collide with the
/// mount's even on a file system that keeps whole seconds.
void rewriteExternally(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    writeFile(p, bytes);
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(5));
}

/// A guest write that changes no SECTOR: one sync byte in the middle of a gap
/// becomes $FE. The track is dirty and every sector still decodes to the
/// bytes the file holds. (An unchanged nibble would not dirty the track.)
void touchTrack(DiskImage& img, int track)
{
    if (pom2test::writeInSyncGap(img, track) >= 0) return;
    std::printf("FAIL: no sync gap on track %d\n", track);
    ++failures;
}

std::unique_ptr<DiskImage> mount(const fs::path& p)
{
    auto img = std::make_unique<DiskImage>();   // 242 KB: never on the stack
    img->setWriteBackEnabled(true);
    if (!img->loadFile(p.string())) {
        std::printf("FAIL: cannot load %s: %s\n", p.string().c_str(),
                    img->getLastError().c_str());
        ++failures;
    }
    img->setWriteBackEnabled(true);
    return img;
}

std::vector<fs::path> siblings(const fs::path& image, const std::string& marker)
{
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(image.parent_path())) {
        const std::string name = e.path().filename().string();
        if (name.rfind(image.stem().string(), 0) == 0 &&
            name.find(marker) != std::string::npos)
            out.push_back(e.path());
    }
    return out;
}

bool commit(DiskImage& img, std::string& error, std::string& divertedTo)
{
    DiskImage::PendingWriteBack pending;
    if (!img.takeWriteBack(pending)) { error = img.getLastError(); return false; }
    return DiskImage::commitWriteBack(std::move(pending), error, &divertedTo);
}

// ── 1. External change ──────────────────────────────────────────────────────

void externalChangeDivertsSectorImage()
{
    const fs::path p = workDir() / "ext.dsk";
    const auto mounted = pattern(7), external = pattern(99);
    writeFile(p, mounted);
    auto img = mount(p);

    rewriteExternally(p, external);          // another program wrote the file
    touchTrack(*img, 5);                     // …and the guest wrote track 5

    std::string error, diverted;
    CHECK(commit(*img, error, diverted));
    CHECK(!diverted.empty());
    // The file is exactly what the other program left: no track of the
    // guest's was laid over it.
    CHECK(readFile(p) == external);
    // The guest's COMPLETE disk is in the sibling — every track, not only the
    // dirty one over a foreign base.
    CHECK(!diverted.empty() && readFile(diverted) == mounted);
    CHECK(fs::path(diverted).extension() == ".dsk");

    // Sticky: the next commit of this mount goes to the same sibling.
    touchTrack(*img, 9);
    std::string diverted2;
    CHECK(commit(*img, error, diverted2));
    CHECK(diverted2 == diverted);
    CHECK(readFile(p) == external);
    CHECK(siblings(p, ".guest-").size() == 1);
}

void untouchedFileIsWrittenInPlace()
{
    const fs::path p = workDir() / "plain.dsk";
    writeFile(p, pattern(7));
    auto img = mount(p);
    std::string error, diverted;

    touchTrack(*img, 5);
    CHECK(commit(*img, error, diverted));
    CHECK(diverted.empty());
    // A second commit sees the file THIS process wrote, not a stranger's.
    touchTrack(*img, 6);
    CHECK(commit(*img, error, diverted));
    CHECK(diverted.empty());
    CHECK(siblings(p, ".guest-").empty());
    CHECK(readFile(p) == pattern(7));
}

void touchedButIdenticalFileIsNotAChange()
{
    const fs::path p = workDir() / "touched.dsk";
    writeFile(p, pattern(7));
    auto img = mount(p);
    // Same bytes, new time: a `touch`, a backup tool, a restore over itself.
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(30));
    touchTrack(*img, 5);
    std::string error, diverted;
    CHECK(commit(*img, error, diverted));
    CHECK(diverted.empty());
    CHECK(siblings(p, ".guest-").empty());
}

void twoMountsOfOneImageDoNotDivertEachOther()
{
    const fs::path p = workDir() / "shared.dsk";
    writeFile(p, pattern(7));
    auto a = mount(p), b = mount(p);
    std::string error, diverted;
    touchTrack(*a, 3);
    CHECK(commit(*a, error, diverted));
    CHECK(diverted.empty());
    touchTrack(*b, 4);                       // B's file was last written by A
    CHECK(commit(*b, error, diverted));
    CHECK(diverted.empty());
}

void autosaveReportsTheDiversion()
{
    // The state machine must say "diverted", with the sibling in the text —
    // not "saved", which is what a successful commit used to mean.
    pom2::MediaAutosave autosave;
    struct Done : pom2::MediaCommitOperation {
        pom2::MediaCommitResult r;
        bool ready() const override { return true; }
        pom2::MediaCommitResult wait() override { return r; }
    };
    struct Exec : pom2::MediaCommitExecutor {
        std::shared_ptr<pom2::MediaCommitOperation>
        submit(std::function<pom2::MediaCommitResult()> work) override {
            auto op = std::make_shared<Done>();
            op->r = work();
            return op;
        }
    } exec;
    autosave.start(exec, [] {
        pom2::MediaCommitResult r;
        r.ok = true;
        r.divertedTo = "/tmp/x.guest-1.dsk";
        return r;
    });
    CHECK(autosave.collect("test"));
    pom2::MediumPersistence p;
    autosave.describe(p, /*loaded=*/true, /*dirty=*/false, /*enabled=*/true);
    CHECK(p.state == "diverted");
    CHECK(p.error.find("x.guest-1.dsk") != std::string::npos);
}

// ── 2. Damaged WOZ ──────────────────────────────────────────────────────────

std::vector<uint8_t> soundWoz()
{
    const fs::path p = workDir() / "source.dsk";
    writeFile(p, pattern(31));
    auto img = mount(p);
    std::vector<DiskImage::TrackBuffer> surface(DiskImage::kTracks);
    for (int t = 0; t < DiskImage::kTracks; ++t)
        for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
            surface[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
                img->nibbleAt(t, i);
    return DiskImage::buildWozFromSurface(surface, /*thirteenSector=*/false);
}

void restampCrc(std::vector<uint8_t>& woz)
{
    const uint32_t crc = DiskImage::wozCrc32(woz.data() + 12, woz.size() - 12);
    for (int i = 0; i < 4; ++i)
        woz[8 + static_cast<std::size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
}

void soundWozMountsWritableAndMatchesItsSource()
{
    const fs::path src = workDir() / "source.dsk";
    const fs::path p = workDir() / "sound.woz";
    writeFile(p, soundWoz());
    auto woz = mount(p);
    auto dsk = mount(src);
    CHECK(woz->isLoaded() && woz->isWoz());
    CHECK(woz->integrityWarning().empty());
    CHECK(!woz->isFileWriteProtected());
    // The builder is lossless: the WOZ reads back the very bit stream the
    // nibble image serves, on every track.
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        const int n = dsk->trackBitLength(t * 4);
        CHECK(woz->trackBitLength(t * 4) == n);
        int diff = 0;
        for (int i = 0; i < n; ++i)
            if (woz->bitAt(t * 4, i) != dsk->bitAt(t * 4, i)) ++diff;
        CHECK(diff == 0);
    }
}

void crcMismatchMountsReadOnly()
{
    const fs::path p = workDir() / "badcrc.woz";
    auto woz = soundWoz();
    woz[2000] ^= 0x10;                       // one flipped bit in track data
    writeFile(p, woz);
    auto img = mount(p);
    CHECK(img->isLoaded());                  // it used to be refused outright
    CHECK(img->integrityWarning().find("CRC32") != std::string::npos);
    CHECK(img->isFileWriteProtected());
    CHECK(img->isWriteProtected());
    // …and nothing is ever written back over the damage.
    DiskImage::PendingWriteBack pending;
    CHECK(img->takeWriteBack(pending) && !pending.valid);
}

void unreadableTrackRecordMountsReadOnly()
{
    const fs::path p = workDir() / "badtrk.woz";
    auto woz = soundWoz();
    // TRK record of track 3 (index 3, at 256 + 3*8): keep its start block and
    // bit count, zero its block count — a half-filled record.
    woz[256 + 3 * 8 + 2] = 0;
    woz[256 + 3 * 8 + 3] = 0;
    restampCrc(woz);                         // the CRC is fine; the table is not
    writeFile(p, woz);
    auto img = mount(p);
    CHECK(img->isLoaded());
    CHECK(img->integrityWarning().find("unreadable") != std::string::npos);
    CHECK(img->integrityWarning().find("3.00") != std::string::npos);
    CHECK(img->isFileWriteProtected());      // it used to mount writable
}

// ── 3. A track that no longer fits the format ───────────────────────────────

void partlyDecodableTrackIsRefusedAndRecovered()
{
    const fs::path p = workDir() / "partial.dsk";
    const auto original = pattern(7);
    writeFile(p, original);
    auto img = mount(p);

    // Break ONE data field of track 5: find the first data prologue and put
    // a byte that is not a 6-and-2 nibble in the middle of the field.
    int at = -1;
    for (int i = 0; i + 2 < DiskImage::kNibblesPerTrack; ++i)
        if (img->nibbleAt(5, i) == 0xD5 && img->nibbleAt(5, i + 1) == 0xAA &&
            img->nibbleAt(5, i + 2) == 0xAD) { at = i; break; }
    CHECK(at >= 0);
    img->writeNibbleAt(5, at + 100, 0x80);

    std::string error, diverted;
    CHECK(!commit(*img, error, diverted));   // it used to be "Saved 1 track"
    CHECK(error.find("no longer decodes") != std::string::npos);
    // All or nothing: not one byte of the file moved.
    CHECK(readFile(p) == original);
    CHECK(img->hasUnsavedChanges());         // the guest's work is still owed

    // The surface is on disk anyway, as a WOZ that mounts and reads the same.
    const fs::path recovered = p.string() + ".recovered.woz";
    CHECK(fs::exists(recovered));
    CHECK(error.find(recovered.filename().string()) != std::string::npos);
    if (fs::exists(recovered)) {
        auto woz = mount(recovered);
        CHECK(woz->isLoaded() && woz->integrityWarning().empty());
        const int n = img->trackBitLength(5 * 4);
        CHECK(woz->trackBitLength(5 * 4) == n);
        int diff = 0;
        for (int i = 0; i < n; ++i)
            if (woz->bitAt(5 * 4, i) != img->bitAt(5 * 4, i)) ++diff;
        CHECK(diff == 0);
    }

    // The guest repairs the sector: the image itself is written again.
    const uint8_t good = [&] {
        auto fresh = mount(p);
        return fresh->nibbleAt(5, at + 100);
    }();
    img->writeNibbleAt(5, at + 100, good);
    CHECK(commit(*img, error, diverted));
    CHECK(diverted.empty());
    CHECK(readFile(p) == original);
}

}  // namespace

int main()
{
    externalChangeDivertsSectorImage();
    untouchedFileIsWrittenInPlace();
    touchedButIdenticalFileIsNotAChange();
    twoMountsOfOneImageDoNotDivertEachOther();
    autosaveReportsTheDiversion();
    soundWozMountsWritableAndMatchesItsSource();
    crcMismatchMountsReadOnly();
    unreadableTrackRecordMountsReadOnly();
    partlyDecodableTrackIsRefusedAndRecovered();

    std::error_code ec;
    fs::remove_all(workDir(), ec);
    if (failures) {
        std::printf("media_external_change: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("media_external_change: OK\n");
    return 0;
}
