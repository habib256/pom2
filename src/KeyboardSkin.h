// POM2 — GPL-3.0-or-later
#ifndef POM2_KEYBOARD_SKIN_H
#define POM2_KEYBOARD_SKIN_H
#include "AppleIIeKeyboardLayout.h"
namespace pom2 {
enum class SystemProfile;
struct KeyboardSkin {
    const char* image;
    const char* title;
    const std::vector<KeyHotspot>& keys;
    bool compact = false;
    // Presentation width / height; zero follows the source texture.
    float displayAspect = 0.0f;
};
const KeyboardSkin& keyboardSkin(SystemProfile profile);
}
#endif
