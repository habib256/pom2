#include "Disk35Image.h"
#include "Sony35Drive.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "Woz35Fixture.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pom2::test;

namespace {

void set16(std::vector<uint8_t>& bytes, size_t off, uint16_t value)
{
    bytes[off] = uint8_t(value);
    bytes[off + 1] = uint8_t(value >> 8);
}

void set32(std::vector<uint8_t>& bytes, size_t off, uint32_t value)
{
    for (int i = 0; i < 4; ++i) bytes[off + i] = uint8_t(value >> (i * 8));
}

// The fixture's independent GCR encoder produces cells; MOOF uses the same
// TRKS layout as WOZ2, with a different signature and INFO fields.
std::vector<uint8_t> makeMoof(const std::vector<std::vector<uint8_t>>& tracks,
                              uint8_t diskType = 2)
{
    auto bytes = buildWoz(tracks);
    std::memcpy(bytes.data(), "MOOF\xFF\x0A\x0D\x0A", 8);
    bytes[20] = 1;                  // INFO version
    bytes[21] = diskType;
    bytes[24] = 16;                 // 2 microseconds per bit cell
    const size_t trksChunk = 12 + 68 + 168;
    set32(bytes, trksChunk + 4, uint32_t(bytes.size() - trksChunk - 8));
    return bytes;
}

void testBitstreamAndExport()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (int track : {0, 16})
        for (int head = 0; head < 2; ++head) {
            std::vector<uint8_t> payload(size_t(sectorsFor(track)) * 512);
            for (int s = 0; s < sectorsFor(track); ++s)
                fillBlock(payload.data() + size_t(s) * 512,
                          blockIndex(track, head, s));
            tracks[size_t(track * 2 + head)] =
                encodeTrack(track, head, payload.data());
        }
    const auto path = writeTemp("pom2_moof35_cells.moof", makeMoof(tracks));
    pom2::Disk35Image image;
    assert(image.loadFile(path.string()));
    assert(image.kind() == pom2::Disk35Image::ImageKind::Moof35);
    assert(image.originalBitCells(0, 0) &&
           *image.originalBitCells(0, 0) == tracks[0]);
    pom2::Sony35Drive drive;
    drive.setImage(&image);
    drive.notifyMediaChange();
    assert(drive.debugCellStream() == tracks[0]);
    const auto expectedRev =
        (int64_t(tracks[0].size()) * 16 *
         int64_t(drive.standardClockHz() + 0.5) + 4000000) / 8000000;
    assert(drive.cyclesPerRev() == expectedRev);
    drive.ssW(true);
    assert(drive.debugCellStream() == tracks[1]);
    image.setWriteBackEnabled(true);
    assert(!image.isWriteProtected() && image.isBlockWriteProtected());
    for (int track : {0, 16})
        for (int head = 0; head < 2; ++head)
            for (int s = 0; s < sectorsFor(track); ++s) {
                const int block = blockIndex(track, head, s);
                uint8_t want[512], got[512];
                fillBlock(want, block);
                assert(image.readBlock(uint32_t(block), got));
                assert(std::memcmp(want, got, 512) == 0);
            }
    uint8_t scratch[512] = {};
    assert(!image.writeBlock(0, scratch));

    const auto out = fs::temp_directory_path() / "pom2_moof35_cells.po";
    std::error_code ec;
    fs::remove(out, ec);
    std::string error;
    assert(image.exportRawTo(out.string(), error));
    assert(fs::file_size(out) == pom2::Disk35Image::kBytesPerImage);
    pom2::Disk35Image raw;
    assert(raw.loadFile(out.string()));
    assert(raw.readBlock(0, scratch));
    uint8_t want[512]; fillBlock(want, 0);
    assert(std::memcmp(want, scratch, 512) == 0);
    fs::remove(path, ec); fs::remove(out, ec);
}

void emitDelta(std::vector<uint8_t>& flux, uint64_t ticks)
{
    while (ticks >= 255) { flux.push_back(255); ticks -= 255; }
    flux.push_back(uint8_t(ticks));
}

std::vector<uint8_t> toFlux(const std::vector<uint8_t>& cells)
{
    std::vector<uint8_t> flux;
    uint64_t previous = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!cells[i]) continue;
        const uint64_t tick = i * 16;
        emitDelta(flux, tick - previous);
        previous = tick;
    }
    emitDelta(flux, cells.size() * 16 - previous); // closing interval
    return flux;
}

void testFluxOverridesTmap()
{
    std::vector<uint8_t> blankPayload(12 * 512, 0);
    std::vector<uint8_t> wantedPayload(12 * 512);
    for (int s = 0; s < 12; ++s)
        fillBlock(wantedPayload.data() + size_t(s) * 512, s);
    std::vector<std::vector<uint8_t>> tracks(160);
    tracks[0] = encodeTrack(0, 0, blankPayload.data());
    auto bytes = makeMoof(tracks);

    // Add a second TRKS entry containing flux deltas. FLUX[0] points there,
    // while TMAP[0] still points to the ordinary (all-zero) GCR bitstream.
    const auto expectedCells = encodeTrack(0, 0, wantedPayload.data());
    const auto flux = toFlux(expectedCells);
    bytes.resize((bytes.size() + 511) & ~size_t(511));
    const size_t fluxData = bytes.size();
    bytes.insert(bytes.end(), flux.begin(), flux.end());
    bytes.resize((bytes.size() + 511) & ~size_t(511));
    const size_t fluxChunk = bytes.size();
    bytes.insert(bytes.end(), {'F','L','U','X'});
    put32(bytes, 160);
    bytes.insert(bytes.end(), 160, 0xFF);
    bytes[fluxChunk + 8] = 1;
    set16(bytes, 20 + 40, uint16_t(fluxChunk / 512));
    set16(bytes, 20 + 42, 1);
    // INFO(68 bytes) + TMAP(168 bytes), then the TRKS chunk header.
    const size_t trksChunk = 12 + 68 + 168;
    set32(bytes, trksChunk + 4, uint32_t(fluxChunk - trksChunk - 8));
    const size_t secondEntry = trksChunk + 8 + 8;
    set16(bytes, secondEntry, uint16_t(fluxData / 512));
    set16(bytes, secondEntry + 2, uint16_t((flux.size() + 511) / 512));
    set32(bytes, secondEntry + 4, uint32_t(flux.size()));

    const auto path = writeTemp("pom2_moof35_flux.moof", bytes);
    pom2::Disk35Image image;
    assert(image.loadFile(path.string()));
    assert(image.originalBitCells(0, 0) &&
           *image.originalBitCells(0, 0) == expectedCells);
    pom2::Sony35Drive drive;
    drive.setImage(&image);
    drive.notifyMediaChange();
    assert(drive.debugCellStream() == expectedCells);
    uint8_t got[512], want[512];
    fillBlock(want, 0);
    assert(image.readBlock(0, got));
    assert(std::memcmp(got, want, 512) == 0);
    std::error_code ec; fs::remove(path, ec);
}

void testUnsupportedDiskTypes()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (uint8_t diskType : {uint8_t(4)}) {
        const auto path = writeTemp("pom2_moof35_unsupported.moof",
                                    makeMoof(tracks, diskType));
        pom2::Disk35Image image;
        assert(!image.loadFile(path.string()));
        assert(image.lastError().find("only 400K/800K GCR and 1.44M MFM") != std::string::npos);
        std::error_code ec; fs::remove(path, ec);
    }
}

void testSingleSided400k()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (int track : {0, 16}) {
        std::vector<uint8_t> payload(size_t(sectorsFor(track)) * 512);
        int base = 0;
        for (int t = 0; t < track; ++t) base += sectorsFor(t);
        for (int s = 0; s < sectorsFor(track); ++s)
            fillBlock(payload.data() + size_t(s) * 512, base + s);
        tracks[size_t(track * 2)] = encodeTrack(track, 0, payload.data());
    }
    const auto path = writeTemp("pom2_moof35_400k.moof", makeMoof(tracks, 1));
    pom2::Disk35Image image;
    assert(image.loadFile(path.string()));
    assert(image.kind() == pom2::Disk35Image::ImageKind::Moof400k);
    assert(image.blockCount() == 800);
    assert(image.headCount() == 1);
    assert(image.originalBitCells(0, 1) == nullptr);
    pom2::Sony35Drive drive;
    drive.setImage(&image);
    drive.notifyMediaChange();
    assert(drive.debugCellStream() == tracks[0]);
    drive.ssW(true);
    assert(drive.debugCellStream().empty());
    for (int track : {0, 16}) {
        int base = 0;
        for (int t = 0; t < track; ++t) base += sectorsFor(t);
        for (int s = 0; s < sectorsFor(track); ++s) {
            uint8_t got[512], want[512];
            fillBlock(want, base + s);
            assert(image.readBlock(uint32_t(base + s), got));
            assert(std::memcmp(got, want, 512) == 0);
        }
    }
    uint8_t scratch[512];
    assert(!image.readBlock(800, scratch));
    const auto out = fs::temp_directory_path() / "pom2_moof35_400k.po";
    std::error_code ec;
    fs::remove(out, ec);
    std::string error;
    assert(image.exportRawTo(out.string(), error));
    assert(fs::file_size(out) == pom2::Disk35Image::kBytesPerImage400);
    pom2::Disk35Image raw;
    assert(raw.loadFile(out.string()));
    assert(raw.kind() == pom2::Disk35Image::ImageKind::Raw400k);
    assert(raw.blockCount() == 800);
    fs::remove(path, ec); fs::remove(out, ec);
}

uint16_t crcMfm(uint16_t crc, uint8_t byte)
{
    crc ^= uint16_t(byte) << 8;
    for (int i = 0; i < 8; ++i)
        crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    return crc;
}

void putMfmWord(std::vector<uint8_t>& cells, uint16_t word)
{
    for (int bit = 15; bit >= 0; --bit) cells.push_back(uint8_t((word >> bit) & 1));
}

void putMfmByte(std::vector<uint8_t>& cells, uint8_t byte, bool& previous)
{
    for (int bit = 7; bit >= 0; --bit) {
        const bool data = (byte >> bit) & 1;
        cells.push_back(uint8_t(!previous && !data));
        cells.push_back(uint8_t(data));
        previous = data;
    }
}

void putMfmField(std::vector<uint8_t>& cells, uint8_t mark,
                 const uint8_t* payload, size_t count)
{
    bool previous = false;
    for (int i = 0; i < 12; ++i) putMfmByte(cells, 0, previous);
    for (int i = 0; i < 3; ++i) putMfmWord(cells, 0x4489);
    previous = true;
    putMfmByte(cells, mark, previous);
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; ++i) crc = crcMfm(crc, 0xA1);
    crc = crcMfm(crc, mark);
    for (size_t i = 0; i < count; ++i) {
        putMfmByte(cells, payload[i], previous);
        crc = crcMfm(crc, payload[i]);
    }
    putMfmByte(cells, uint8_t(crc >> 8), previous);
    putMfmByte(cells, uint8_t(crc), previous);
}

void testMfm1440k()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (int slot : {0, 1, 2}) {
        const int track = slot / 2, head = slot % 2;
        auto& cells = tracks[size_t(slot)];
        cells.insert(cells.end(), 640, 0);
        for (int s = 1; s <= 18; ++s) {
            const uint8_t id[4] = {uint8_t(track), uint8_t(head), uint8_t(s), 2};
            putMfmField(cells, 0xFE, id, 4);
            uint8_t payload[512];
            fillBlock(payload, slot * 18 + s - 1);
            putMfmField(cells, 0xFB, payload, 512);
            cells.insert(cells.end(), 256, 0);
        }
    }
    auto moof = makeMoof(tracks, 3);
    moof[24] = 8; // 1 microsecond MFM bit cells
    const auto path = writeTemp("pom2_moof35_mfm.moof", moof);
    pom2::Disk35Image image;
    assert(!image.loadFile(path.string()));
    assert(image.lastError().find("SuperDrive controller") != std::string::npos);
    image.setMfmCapable(true);
    assert(image.loadFile(path.string()));
    assert(image.kind() == pom2::Disk35Image::ImageKind::Moof1440k);
    assert(image.blockCount() == 2880);
    for (int s = 0; s < 54; ++s) {
        uint8_t got[512], want[512];
        fillBlock(want, s);
        assert(image.readBlock(uint32_t(s), got));
        assert(std::memcmp(got, want, 512) == 0);
    }
    pom2::SmartPortCard ordinary(5);
    ordinary.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    auto* ordinaryUnit = dynamic_cast<pom2::SmartPort35Unit*>(ordinary.unit(0));
    assert(ordinaryUnit && !ordinaryUnit->loadImage(path.string()));
    pom2::SmartPortCard superDrive(5);
    superDrive.setSuperDriveMode(true);
    superDrive.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    auto* hdUnit = dynamic_cast<pom2::SmartPort35Unit*>(superDrive.unit(0));
    assert(hdUnit && hdUnit->loadImage(path.string()));
    assert(hdUnit->blockCount() == 2880);
    uint8_t block[512], want[512];
    fillBlock(want, 17);
    assert(hdUnit->readBlock(17, block));
    assert(std::memcmp(block, want, 512) == 0);
    superDrive.deviceSelectWrite(0x0, 0);
    superDrive.deviceSelectWrite(0x1, 17);
    superDrive.deviceSelectWrite(0x2, 0);
    for (int i = 0; i < 512; ++i)
        assert(superDrive.deviceSelectRead(0x3) == want[i]);
    const auto out = fs::temp_directory_path() / "pom2_moof35_mfm.po";
    std::error_code ec;
    fs::remove(out, ec);
    std::string error;
    assert(image.exportRawTo(out.string(), error));
    assert(fs::file_size(out) == pom2::Disk35Image::kBytesPerImage1440);
    pom2::Disk35Image raw;
    raw.setMfmCapable(true);
    assert(raw.loadFile(out.string()));
    assert(raw.kind() == pom2::Disk35Image::ImageKind::Raw1440k);
    assert(raw.readBlock(17, block));
    assert(std::memcmp(block, want, 512) == 0);
    raw.setWriteBackEnabled(true);
    fillBlock(want, 99);
    assert(raw.writeBlock(17, want));
    assert(raw.saveDirty());
    pom2::Disk35Image saved;
    saved.setMfmCapable(true);
    assert(saved.loadFile(out.string()));
    assert(saved.readBlock(17, block));
    assert(std::memcmp(block, want, 512) == 0);
    assert(fs::file_size(out) == pom2::Disk35Image::kBytesPerImage1440);
    fs::remove(path, ec); fs::remove(out, ec);
}

void testMfmCaptureWrites()
{
    for (size_t rotate : {size_t(0), size_t(1000), size_t(8500)}) {
        std::vector<std::vector<uint8_t>> tracks(160);
        for (int slot : {0, 1, 158, 159}) {
            auto& cells = tracks[size_t(slot)];
            cells.insert(cells.end(), 640, 0);
            for (int s = 1; s <= 18; ++s) {
                const uint8_t id[] = {uint8_t(slot / 2), uint8_t(slot % 2), uint8_t(s), 2};
                putMfmField(cells, 0xFE, id, 4);
                uint8_t payload[512]; fillBlock(payload, slot * 18 + s - 1);
                putMfmField(cells, s == 1 ? 0xF8 : 0xFB, payload, 512);
                cells.insert(cells.end(), 256, 0);
            }
            std::rotate(cells.begin(), cells.begin() + rotate, cells.end());
        }
        auto bytes = makeMoof(tracks, 3); bytes[24] = 8;
        const std::vector<uint8_t> metadata{'M','E','T','A',4,0,0,0,'k','e','e','p'};
        bytes.insert(bytes.end(), metadata.begin(), metadata.end());
        const auto path = writeTemp("pom2_moof35_mfm_write.moof", bytes);
        pom2::Disk35Image image; image.setMfmCapable(true); image.setWriteBackEnabled(true);
        assert(image.loadFile(path.string()) && !image.isBlockWriteProtected());
        assert(image.blockIndexFor(79, 1, 17) == 2879);
        uint8_t want[512], got[512];
        for (int slot : {0, 1, 158, 159}) {
            fillBlock(want, 9000 + slot);
            assert(image.writeBlock(uint32_t(slot * 18), want));
            assert(image.readBlock(uint32_t(slot * 18), got));
            assert(std::memcmp(got, want, 512) == 0);
            // Compare every clock/data cell with an independent encoding,
            // including the first clock after the new CRC low byte.
            std::vector<uint8_t> field;
            putMfmField(field, 0xF8, want, 512);
            auto expected = tracks[size_t(slot)];
            const size_t fieldStart = 640 + 352;
            auto set = [&](size_t at, uint8_t bit) {
                expected[(at + expected.size() - rotate) % expected.size()] = bit;
            };
            for (size_t i = 256; i < field.size(); ++i) set(fieldStart + i, field[i]);
            set(fieldStart + field.size(), uint8_t(!field.back()));
            assert(*image.originalBitCells(slot / 2, slot % 2) == expected);
        }
        const auto beforeMissing = *image.originalFluxTicks(0, 0);
        assert(!image.writeBlock(36, want)); // no track 1 in this capture
        assert(*image.originalFluxTicks(0, 0) == beforeMissing);
        assert(image.saveDirty()); // packed tracks become exact 125 ns flux
        pom2::Disk35Image reloaded; reloaded.setMfmCapable(true); reloaded.setWriteBackEnabled(true);
        assert(reloaded.loadFile(path.string()));
        for (int slot : {0, 1, 158, 159}) for (int s = 0; s < 18; ++s) {
            fillBlock(want, s ? slot * 18 + s : 9000 + slot);
            assert(reloaded.readBlock(uint32_t(slot * 18 + s), got));
            assert(std::memcmp(got, want, 512) == 0); // also verifies new data CRCs
        }
        reloaded.setHostWriteProtected(true);
        std::vector<uint8_t> archived;
        std::string sessionError;
        assert(reloaded.appendReadOnlySession(archived, sessionError));
        pom2::Disk35Image restored; restored.setMfmCapable(true);
        assert(restored.loadReadOnlySession(archived.data(), archived.size(), sessionError));
        fillBlock(want, 9159);
        assert(restored.readBlock(159 * 18, got) && std::memcmp(want, got, 512) == 0);
        reloaded.setHostWriteProtected(false);
        // A second write starts from a FLUX capture. Keep an off-grid
        // transition in the leading gap byte-for-byte through that splice.
        if (!rotate) {
            const int64_t tick = 101;
            assert(reloaded.writeFlux125ns(0, 0, 100, 102, &tick, 1, 0));
            fillBlock(want, 12345);
            assert(reloaded.writeBlock(0, want));
            assert(std::binary_search(reloaded.originalFluxTicks(0, 0)->begin(),
                                      reloaded.originalFluxTicks(0, 0)->end(), 101));
            assert(reloaded.saveDirty());
            pom2::SmartPortCard port(5); port.setSuperDriveMode(true);
            port.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
            auto* unit = dynamic_cast<pom2::SmartPort35Unit*>(port.unit(0));
            assert(unit && unit->loadImage(path.string()));
            unit->setWriteBackEnabled(true);
            assert(unit->readBlock(0, got) && std::memcmp(got, want, 512) == 0);
            port.deviceSelectWrite(0, 0); port.deviceSelectWrite(1, 17); port.deviceSelectWrite(2, 0);
            fillBlock(want, 777);
            for (auto byte : want) port.deviceSelectWrite(3, byte);
            assert(unit->saveDirty());
            assert(unit->readBlock(17, got) && std::memcmp(got, want, 512) == 0);
            unit->setHostWriteProtected(true);
            assert(unit->isWriteProtected() && !unit->writeBlock(17, got));
        }
        std::ifstream file(path, std::ios::binary);
        const std::vector<uint8_t> saved{std::istreambuf_iterator<char>(file), {}};
        assert(std::search(saved.begin(), saved.end(), metadata.begin(), metadata.end()) != saved.end());
        bytes[22] = 1; // MOOF's own write-protect bit is authoritative
        writeTemp("pom2_moof35_mfm_write.moof", bytes);
        assert(image.loadFile(path.string()) && image.isBlockWriteProtected());
        assert(!image.writeBlock(0, want));
        std::error_code ec; fs::remove(path, ec);
    }
}

void testMfmFullGeometryBudget()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (int slot = 0; slot < 160; ++slot) {
        const uint8_t id[] = {uint8_t(slot / 2), uint8_t(slot % 2), 1, 2};
        uint8_t block[512]; fillBlock(block, slot);
        putMfmField(tracks[size_t(slot)], 0xFE, id, 4);
        putMfmField(tracks[size_t(slot)], 0xFB, block, 512);
        tracks[size_t(slot)].resize(200000); // 300 rpm, 1 us cells
    }
    auto bytes = makeMoof(tracks, 3); bytes[24] = 8;
    const auto path = writeTemp("pom2_moof35_full_mfm.moof", bytes);
    pom2::Disk35Image image; image.setMfmCapable(true);
    assert(image.loadFile(path.string()));
    for (int slot = 0; slot < 160; ++slot) {
        uint8_t want[512], got[512]; fillBlock(want, slot);
        assert(image.readBlock(uint32_t(slot * 18), got));
        assert(std::memcmp(want, got, 512) == 0);
    }
    std::error_code ec; fs::remove(path, ec);
}

void testWrongHeadIsIgnored()
{
    std::vector<uint8_t> payload(12 * 512);
    for (int s = 0; s < 12; ++s)
        fillBlock(payload.data() + size_t(s) * 512, blockIndex(0, 1, s));
    std::vector<std::vector<uint8_t>> tracks(160);
    tracks[0] = encodeTrack(0, 1, payload.data()); // TMAP slot 0 is head 0
    const auto path = writeTemp("pom2_moof35_wrong_head.moof", makeMoof(tracks));
    pom2::Disk35Image image;
    assert(image.loadFile(path.string()));
    assert(image.originalBitCells(0, 0) &&
           *image.originalBitCells(0, 0) == tracks[0]);
    pom2::Sony35Drive drive;
    drive.setImage(&image);
    drive.notifyMediaChange();
    assert(drive.debugCellStream() == tracks[0]);
    drive.ssW(true);
    assert(drive.debugCellStream().empty());
    uint8_t block[512];
    assert(image.readBlock(uint32_t(blockIndex(0, 1, 0)), block));
    for (uint8_t byte : block) assert(byte == 0);
    std::error_code ec; fs::remove(path, ec);
}

void testCrcMismatch()
{
    std::vector<uint8_t> payload(12 * 512);
    for (int s = 0; s < 12; ++s)
        fillBlock(payload.data() + size_t(s) * 512, s);
    std::vector<std::vector<uint8_t>> tracks(160);
    tracks[0] = encodeTrack(0, 0, payload.data());
    auto bytes = makeMoof(tracks);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 12; i < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    set32(bytes, 8, ~crc);
    const auto path = writeTemp("pom2_moof35_bad_crc.moof", bytes);
    pom2::Disk35Image image;
    assert(image.loadFile(path.string()));
    bytes[bytes.size() - 1] ^= 1;
    writeTemp("pom2_moof35_bad_crc.moof", bytes);
    assert(!image.loadFile(path.string()));
    assert(image.lastError().find("CRC32 mismatch") != std::string::npos);
    std::error_code ec; fs::remove(path, ec);
}

} // namespace

int main()
{
    testBitstreamAndExport();
    testFluxOverridesTmap();
    testUnsupportedDiskTypes();
    testSingleSided400k();
    testMfm1440k();
    testMfmCaptureWrites();
    testMfmFullGeometryBudget();
    testWrongHeadIsIgnored();
    testCrcMismatch();
}
