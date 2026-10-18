#include "Disk35Image.h"
#include "Sony35Drive.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "Woz35Fixture.h"

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
    assert(image.isWriteProtected());
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
    testWrongHeadIsIgnored();
    testCrcMismatch();
}
