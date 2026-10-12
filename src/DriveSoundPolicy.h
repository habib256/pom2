// POM2 — GPL-3.0-or-later
#ifndef POM2_DRIVE_SOUND_POLICY_H
#define POM2_DRIVE_SOUND_POLICY_H
#include <string>
namespace pom2 {
inline bool useVirtualIIDriveSounds(bool compact525, const std::string& preference,
                                    bool recordingsReady)
{
    return recordingsReady && (preference == "virtual2" ||
                               (preference == "auto" && compact525));
}
} // namespace pom2
#endif
