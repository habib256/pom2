// POM2 — GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {
inline constexpr size_t kMaxSessionBytes = 32u * 1024u * 1024u;
inline constexpr size_t kMaxSessionCardBytes = kMaxSessionBytes - 24;
struct SessionArchive {
    enum class Card : uint8_t { Empty, DiskII, Mockingboard, SoundII, SnesMax };
    struct Slot {
        Card kind = Card::Empty;
        std::vector<uint8_t> identity, state;
    };
    uint32_t version = 2, machineId = 0;
    std::vector<uint8_t> configuration, core;
    std::array<Slot, 8> slots;
};
// Encoding/checksum verification runs outside the machine state lock.
bool encodeSessionArchive(const SessionArchive& session, std::vector<uint8_t>& out,
                          std::string& error);
bool decodeSessionArchive(const std::vector<uint8_t>& bytes, SessionArchive& out,
                          std::string& error);
}
