// POM2 — GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace pom2 {
// Capture timing stays in 125 ns units. Serialized source chunks are retained
// verbatim; writes overlay only the physical track/side under the head.
struct Disk35Bitstream {
    std::vector<uint8_t> raw;
    std::array<std::vector<uint32_t>, 160> transitions;
    std::array<uint32_t, 160> periods{};
    std::array<bool, 160> changed{};
    bool moof = false;
};
}
