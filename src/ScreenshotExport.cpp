// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenshotExport.h"
#include "AiControlJson.h"
#include "AtomicFileReplace.h"
#include "CrtPresets.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

// Private encoder, independent of the paint editor's stb implementation.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace pom2 {
namespace {
void bigEndian(std::vector<uint8_t>& out, uint32_t n)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(n >> shift));
}

std::string metadataJson(const ScreenshotMetadata& m, int width, int height)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"schema\":1,\"software\":\"" << aijson::jsonEscape(m.software)
        << "\",\"machine\":\"" << aijson::jsonEscape(m.machine)
        << "\",\"monitor\":\"" << aijson::jsonEscape(m.monitor)
        << "\",\"video_standard\":\"" << aijson::jsonEscape(m.videoStandard)
        << "\",\"capture_stage\":\"framebuffer_before_gl_effects\""
        << ",\"gl_effects_applied\":false,\"width\":" << width
        << ",\"height\":" << height
        << ",\"crt_enabled\":" << (m.crtEnabled ? "true" : "false")
        << ",\"crt\":{";
    for (size_t i = 0; i < crt::kFields; ++i) {
        const auto field = static_cast<crt::Field>(i);
        const float value = crt::get(m.crt, field);
        if (!std::isfinite(value)) return {};
        if (i) out << ',';
        out << '"' << crt::info(field).key << "\":" << value;
    }
    out << ",\"pal_mode\":" << (m.crt.palMode ? "true" : "false")
        << ",\"text_sharp\":" << (m.crt.textSharp ? "true" : "false") << "}}";
    return out.str();
}
} // namespace

bool encodeScreenshotPng(int width, int height, const std::vector<uint32_t>& pixels,
                         const ScreenshotMetadata& metadata,
                         std::vector<uint8_t>& png, std::string& error)
{
    png.clear(); error.clear();
    // Bound allocations and stb's signed stride arithmetic before multiplying.
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        pixels.size() != size_t(width) * size_t(height)) {
        error = "invalid screenshot dimensions";
        return false;
    }
    const auto json = metadataJson(metadata, width, height);
    if (json.empty() || json.size() > 65536) {
        error = "invalid screenshot metadata";
        return false;
    }
    std::vector<uint8_t> rgb(pixels.size() * 3);
    for (size_t i = 0; i < pixels.size(); ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = uint8_t(pixels[i] >> (c * 8));
    int length = 0;
    unsigned char* encoded = stbi_write_png_to_mem(rgb.data(), width * 3,
                                                  width, height, 3, &length);
    if (!encoded) { error = "PNG encoding failed"; return false; }
    // stb emits a complete PNG ending in the 12-byte IEND chunk.
    png.assign(encoded, encoded + length);
    STBIW_FREE(encoded);
    if (png.size() < 12 || std::memcmp(png.data() + png.size() - 8, "IEND", 4)) {
        png.clear(); error = "PNG encoder returned no IEND"; return false;
    }
    // [B] W3C PNG 3 (2025-06-24), §11.3.3.4 and §5.3:
    // https://www.w3.org/TR/2025/REC-png-3-20250624/#11iTXt
    // Uncompressed UTF-8 JSON; empty language tag and translated keyword.
    const std::string payload = std::string("POM2\0\0\0\0\0", 9) + json;
    std::vector<uint8_t> chunk;
    bigEndian(chunk, static_cast<uint32_t>(payload.size()));
    chunk.insert(chunk.end(), {'i', 'T', 'X', 't'});
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    bigEndian(chunk, stbiw__crc32(chunk.data() + 4, int(chunk.size() - 4)));
    png.insert(png.end() - 12, chunk.begin(), chunk.end());
    return true;
}

bool saveScreenshotPng(const std::filesystem::path& directory,
                       int width, int height, const std::vector<uint32_t>& pixels,
                       const ScreenshotMetadata& metadata,
                       std::filesystem::path& savedPath, std::string& error)
{
    savedPath.clear();
    std::vector<uint8_t> png;
    if (!encodeScreenshotPng(width, height, pixels, metadata, png, error)) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) { error = "cannot create " + directory.string() + ": " + ec.message(); return false; }
    uint64_t next = 0;
    fs::directory_iterator it(directory, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        const auto ext = it->path().extension().string();
        if (name.rfind("screenshot_", 0) != 0 || (ext != ".png" && ext != ".ppm")) continue;
        const auto digits = name.substr(11, name.size() - 11 - ext.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        uint64_t index = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (parsed.ec != std::errc{} || index == std::numeric_limits<uint64_t>::max()) {
            error = "screenshot sequence exhausted"; return false;
        }
        next = std::max(next, index + 1);
    }
    if (ec) { error = "cannot list " + directory.string() + ": " + ec.message(); return false; }
    auto digits = std::to_string(next);
    if (digits.size() < 3) digits.insert(0, 3 - digits.size(), '0');
    const auto path = directory / ("screenshot_" + digits + ".png");
    if (!writeFileAtomic(path, png.data(), png.size(), ec)) {
        error = "cannot write " + path.string() + ": " + ec.message(); return false;
    }
    savedPath = path;
    return true;
}
} // namespace pom2
