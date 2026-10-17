// POM2 Apple II Emulator — GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pom2 {

// ADTPro VSDrive wire protocol: two 512-byte ProDOS block volumes over an
// eight-bit serial stream. The parser accepts arbitrary packet boundaries.
class VsDriveProtocol {
public:
    VsDriveProtocol() = default;
    ~VsDriveProtocol();
    VsDriveProtocol(const VsDriveProtocol&) = delete;
    VsDriveProtocol& operator=(const VsDriveProtocol&) = delete;

    bool open(const std::array<std::string, 2>& paths, std::string& error);
    bool open(const std::array<std::string, 2>& paths,
              const std::array<bool, 2>& readOnly, std::string& error);
    void close();
    void resetPacket();
    void feed(const uint8_t* bytes, size_t count, std::vector<uint8_t>& replies);
    const std::array<std::string, 2>& paths() const { return paths_; }
    const std::array<bool, 2>& readOnly() const { return readOnly_; }

private:
    std::array<std::FILE*, 2> files_{};
    std::array<uint32_t, 2> blocks_{};
    std::array<bool, 2> writable_{};
    std::array<bool, 2> readOnly_{};
    std::array<std::string, 2> paths_{};
    std::array<uint8_t, 518> packet_{};
    size_t used_ = 0;
    size_t needed_ = 5;

    void respond(std::vector<uint8_t>& replies);
};

} // namespace pom2
