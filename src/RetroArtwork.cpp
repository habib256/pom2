#include "RetroArtwork.h"

#include "Logger.h"
#include "Pom2GL.h"
#include "ResourcePaths.h"
#include "stb_image.h"

namespace pom2 {

RetroArtwork::~RetroArtwork()
{
    for (auto tex : textures_)
        if (tex) glDeleteTextures(1, &tex);
}

unsigned int RetroArtwork::load(int index, const char* relative)
{
    if (tried_[index]) return textures_[index];
    tried_[index] = true;
    const auto path = findResource(relative);
    int width = 0, height = 0, channels = 0;
    auto* pixels = path.empty() ? nullptr
        : stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        log().warn("Artwork", std::string("Cannot load ") + relative);
        return 0;
    }
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    auto& tex = textures_[index];
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    stbi_image_free(pixels);
    return tex;
}

unsigned int RetroArtwork::monitor()
{
    return load(0, "pic/retro/monitor/monitor2.png");
}

unsigned int RetroArtwork::drive35(bool busy)
{
    return load(busy ? 10 : 9, busy
        ? "pic/retro/drives/apple35running.png"
        : "pic/retro/drives/apple35.png");
}

unsigned int RetroArtwork::driveIIc(bool loaded, bool busy)
{
    static constexpr const char* paths[] = {
        "pic/retro/drives/diskiicopen.png",
        "pic/retro/drives/diskiicopenrunning.png",
        "pic/retro/drives/diskiicclosed.png",
        "pic/retro/drives/diskiicrunning.png"
    };
    const int variant = (loaded ? 2 : 0) + (busy ? 1 : 0);
    return load(11 + variant, paths[variant]);
}

unsigned int RetroArtwork::hardDisk(bool busy)
{
    return load(busy ? 16 : 15, busy
        ? "pic/retro/drives/applehdrunning.png"
        : "pic/retro/drives/applehd.png");
}

unsigned int RetroArtwork::drive(int index, bool loaded, bool motorOn)
{
    if (index < 0 || index > 1) return 0;
    static constexpr const char* paths[] = {
        "pic/retro/drives/driveopen.png",
        "pic/retro/drives/driveopenrunning.png",
        "pic/retro/drives/driveclosed.png",
        "pic/retro/drives/driverunning.png",
        "pic/retro/drives/drive2open.png",
        "pic/retro/drives/drive2openrunning.png",
        "pic/retro/drives/drive2closed.png",
        "pic/retro/drives/drive2running.png"
    };
    // Upstream calls drive 1's open/motor-on image "driveopenrunning".
    const int variant = index * 4 + (loaded ? 2 : 0) + (motorOn ? 1 : 0);
    return load(1 + variant, paths[variant]);
}

} // namespace pom2
