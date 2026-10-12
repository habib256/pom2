#ifndef POM2_RETRO_ARTWORK_H
#define POM2_RETRO_ARTWORK_H

#include <array>

namespace pom2 {

// UI-thread-only PNG textures, loaded lazily and released before GL teardown.
// Artwork attribution and editable sources live in pic/retro/.
class RetroArtwork {
public:
    RetroArtwork() = default;
    ~RetroArtwork();
    RetroArtwork(const RetroArtwork&) = delete;
    RetroArtwork& operator=(const RetroArtwork&) = delete;

    unsigned int monitor();
    unsigned int drive(int index, bool loaded, bool motorOn);
    unsigned int drive35(bool busy);
    unsigned int driveIIc(bool loaded, bool busy);
    unsigned int hardDisk(bool busy);

private:
    unsigned int load(int index, const char* path);
    std::array<unsigned int, 17> textures_{};
    std::array<bool, 17> tried_{};
};

} // namespace pom2
#endif
