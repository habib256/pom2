// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenshotExport.h"
#include "AiControlJson.h"
#include "CrtPresets.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;
static uint32_t be(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
// Deliberately independent of stb's table-based CRC used by the writer.
static uint32_t crc(const uint8_t* p, size_t n)
{
    uint32_t result = 0xFFFFFFFFu;
    while (n--) {
        result ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ ((result & 1) ? 0xEDB88320u : 0);
    }
    return ~result;
}
static std::vector<uint8_t> read(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct DecimalComma : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};

int main(int argc, char** argv)
{
    pom2::ScreenshotMetadata metadata;
    metadata.software = "POM2 test";
    metadata.machine = "iie-pal";
    metadata.monitor = u8"Écran \"vert\"\n\\";
    metadata.videoStandard = "PAL";
    metadata.crtEnabled = true;
    metadata.crt.scanlines = 0.375f;
    metadata.crt.bloom = 0.25f;
    metadata.crt.palMode = true;
    metadata.crt.textSharp = false;
    // Distinct channels, rows and ignored alpha: detect flips/swizzles.
    const std::vector<uint32_t> pixels = {0x000000FF, 0xFF00FF00, 0x22FF0000,
                                         0x00332211, 0xFFFF00FF, 0x00000000};
    std::vector<uint8_t> png;
    std::string error;
    const auto oldLocale = std::locale();
    std::locale::global(std::locale(oldLocale, new DecimalComma));
    assert(pom2::encodeScreenshotPng(3, 2, pixels, metadata, png, error));
    std::locale::global(oldLocale);
    assert(error.empty());
    const uint8_t signature[] = {137,80,78,71,13,10,26,10};
    assert(png.size() > 8 && std::equal(png.begin(), png.begin()+8, signature));
    bool found = false, ended = false;
    for (size_t pos = 8; pos < png.size();) {
        assert(pos + 12 <= png.size());
        const size_t len = be(&png[pos]);
        assert(pos + 12 + len <= png.size());
        assert(crc(&png[pos+4], len+4) == be(&png[pos+8+len]));
        const std::string kind(reinterpret_cast<const char*>(&png[pos+4]), 4);
        if (kind == "iTXt") {
            assert(!found && !ended); found = true;
            const std::string payload(reinterpret_cast<const char*>(&png[pos+8]), len);
            assert(payload.substr(0,9) == std::string("POM2\0\0\0\0\0",9));
            const auto json = payload.substr(9);
            assert(pom2::aijson::jsonGetString(json,"monitor") == metadata.monitor);
            assert(pom2::aijson::jsonGetString(json,"machine") == "iie-pal");
            assert(pom2::aijson::jsonGetString(json,"capture_stage") == "framebuffer_before_gl_effects");
            assert(json.find("\"schema\":1") != std::string::npos);
            assert(json.find("\"scanlines\":0.375") != std::string::npos);
            assert(json.find("\"bloom\":0.25") != std::string::npos);
            assert(json.find("\"gl_effects_applied\":false") != std::string::npos);
            assert(json.find("\"crt_enabled\":true") != std::string::npos);
            assert(json.find("\"pal_mode\":true") != std::string::npos);
            assert(json.find("\"text_sharp\":false") != std::string::npos);
            for (size_t f = 0; f < pom2::crt::kFields; ++f)
                assert(json.find(std::string("\"") + pom2::crt::info(static_cast<pom2::crt::Field>(f)).key + "\":") != std::string::npos);
        }
        if (kind == "IEND") { ended = true; assert(pos+12 == png.size()); }
        pos += 12 + len;
    }
    assert(found && ended);
    int w=0, h=0, channels=0;
    auto decoded = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &channels, 3);
    assert(decoded && w == 3 && h == 2 && channels == 3);
    for (size_t i = 0; i < pixels.size(); ++i)
        for (int c = 0; c < 3; ++c) assert(decoded[i*3+c] == uint8_t(pixels[i] >> (c*8)));
    stbi_image_free(decoded);

    const auto dir = fs::temp_directory_path() / ("pom2-screenshot-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path saved;
    assert(pom2::saveScreenshotPng(dir, 3, 2, pixels, metadata, saved, error));
    assert(saved.filename() == "screenshot_000.png" && read(saved) == png);
    std::ofstream(dir / "screenshot_998.ppm") << "old PPM";
    std::ofstream(dir / "screenshot_999.png") << "old PNG";
    std::ofstream(dir / "screenshot_oops.png") << "unrelated";
    assert(pom2::saveScreenshotPng(dir, 3, 2, pixels, metadata, saved, error));
    assert(saved.filename() == "screenshot_1000.png" && read(saved) == png);
    assert(read(dir / "screenshot_999.png") == std::vector<uint8_t>({'o','l','d',' ','P','N','G'}));
    assert(pom2::saveScreenshotPng(dir, 3, 2, pixels, metadata, saved, error));
    assert(saved.filename() == "screenshot_1001.png");
    // Optional artifact for an independent decoder; no fixture is needed by CTest.
    if (argc == 2) fs::copy_file(saved, argv[1], fs::copy_options::overwrite_existing);
    assert(!pom2::saveScreenshotPng(dir / "screenshot_999.png", 3, 2, pixels, metadata, saved, error));
    assert(saved.empty() && !error.empty());
    assert(!pom2::encodeScreenshotPng(0,2,pixels,metadata,png,error) && png.empty());
    assert(!pom2::encodeScreenshotPng(4097,2,pixels,metadata,png,error));
    assert(!pom2::encodeScreenshotPng(3,3,pixels,metadata,png,error));
    metadata.crt.bloom = std::numeric_limits<float>::quiet_NaN();
    assert(!pom2::encodeScreenshotPng(3,2,pixels,metadata,png,error));
    metadata.crt.bloom = 0;
    std::ofstream(dir / "screenshot_18446744073709551615.png") << "limit";
    assert(!pom2::saveScreenshotPng(dir,3,2,pixels,metadata,saved,error));
    fs::remove_all(dir);
}
