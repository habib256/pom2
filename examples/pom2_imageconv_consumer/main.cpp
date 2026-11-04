// The image conversion package from an install tree: a 280x192 gradient to an
// HGR page and a DHGR page pair, with the bundled stb reader and writer linked
// the way a command-line converter links them, then a folder made into a ProDOS
// volume. Prints each page's digest and the volume's size.
#include "hgrpaint/HgrConvert.h"
#include "hgrpaint/HgrPaintModel.h"
#include "Cam16.h"
#include "ProDOSVolume.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

int main()
{
    std::vector<std::uint8_t> rgba(280u * 192u * 4u);
    for (unsigned y = 0; y < 192; ++y)
        for (unsigned x = 0; x < 280; ++x) {
            std::uint8_t* p = &rgba[(y * 280u + x) * 4u];
            p[0] = static_cast<std::uint8_t>(x * 255u / 279u);
            p[1] = static_cast<std::uint8_t>(y * 255u / 191u);
            p[2] = static_cast<std::uint8_t>(128u);
            p[3] = 255u;
        }
    hgrpaint::ImportOptions options;
    options.stretch = true;

    std::vector<std::uint8_t> hgr(8192);
    hgrpaint::imageToHgrPage(rgba.data(), 280, 192, options, hgr.data());
    std::vector<std::uint8_t> dhgr(16384);
    hgrpaint::imageToDhgrPage(rgba.data(), 280, 192, options, dhgr.data());

    const auto sum = [](const std::vector<std::uint8_t>& page) {
        std::uint32_t h = 2166136261u;                  // FNV-1a
        for (const std::uint8_t b : page) h = (h ^ b) * 16777619u;
        return h;
    };
    const hgrpaint::Cam16Ucs ink = hgrpaint::srgb8ToCam16Ucs(255, 255, 255);

    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "pom2_imageconv_consumer";
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    fs::create_directories(folder);
    { std::ofstream(folder / "HELLO.TXT") << "HELLO\r"; }
    std::vector<std::uint8_t> volume;
    const pom2::ProDOSBuildResult built =
        pom2::buildVolumeFromFolder(folder.string(), "CONSUMER", volume);
    fs::remove_all(folder, ignored);

    std::printf("hgr=%08x dhgr=%08x white=%s volume=%zu\n", sum(hgr), sum(dhgr),
                ink.J > 0 ? "ok" : "bad", volume.size());
    return ink.J > 0 && built.ok && !volume.empty() ? 0 : 1;
}
