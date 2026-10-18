#include "Disk35Image.h"
#include "Woz35Fixture.h"

#include <cassert>
#include <cstring>
#include <filesystem>
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
    const auto flux = toFlux(encodeTrack(0, 0, wantedPayload.data()));
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
    uint8_t got[512], want[512];
    fillBlock(want, 0);
    assert(image.readBlock(0, got));
    assert(std::memcmp(got, want, 512) == 0);
    std::error_code ec; fs::remove(path, ec);
}

void testUnsupportedDiskTypes()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    for (uint8_t diskType : {uint8_t(1), uint8_t(3)}) {
        const auto path = writeTemp("pom2_moof35_unsupported.moof",
                                    makeMoof(tracks, diskType));
        pom2::Disk35Image image;
        assert(!image.loadFile(path.string()));
        assert(image.lastError().find("only 800K GCR") != std::string::npos);
        std::error_code ec; fs::remove(path, ec);
    }
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
    assert(!image.loadFile(path.string()));
    assert(image.lastError().find("no Sony GCR sectors") != std::string::npos);
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
    testWrongHeadIsIgnored();
    testCrcMismatch();
}
