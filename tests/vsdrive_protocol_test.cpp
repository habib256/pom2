// POM2 Apple II Emulator — GPL-3.0-or-later
#include "VsDriveProtocol.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint8_t xorBytes(const uint8_t* data, size_t count)
{
    uint8_t value = 0;
    for (size_t i = 0; i < count; ++i) value ^= data[i];
    return value;
}

std::array<uint8_t, 5> request(uint8_t command, uint16_t block)
{
    std::array<uint8_t, 5> bytes{0xC5, command,
        static_cast<uint8_t>(block), static_cast<uint8_t>(block >> 8), 0};
    bytes[4] = xorBytes(bytes.data(), 4);
    return bytes;
}

void feed(pom2::VsDriveProtocol& host, const std::vector<uint8_t>& bytes,
          std::vector<uint8_t>& reply)
{
    host.feed(bytes.data(), bytes.size(), reply);
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const auto path1 = fs::temp_directory_path() / "pom2-vsdrive-protocol-1.po";
    const auto path2 = fs::temp_directory_path() / "pom2-vsdrive-protocol-2.po";
    {
        std::array<uint8_t, 1024> disk1{};
        std::array<uint8_t, 512> disk2{};
        for (size_t i = 0; i < 512; ++i) {
            disk1[512 + i] = static_cast<uint8_t>(i ^ 0xA5);
            disk2[i] = static_cast<uint8_t>(i ^ 0x5A);
        }
        std::ofstream(path1, std::ios::binary).write(
            reinterpret_cast<const char*>(disk1.data()), disk1.size());
        std::ofstream(path2, std::ios::binary).write(
            reinterpret_cast<const char*>(disk2.data()), disk2.size());
    }

    pom2::VsDriveProtocol host;
    std::string error;
    assert(host.open({path1.string(), path2.string()}, error));
    std::vector<uint8_t> reply;

    // A2FileCmd's driver transmits the envelope byte by byte. No answer
    // should appear before the fifth byte, and noise before $C5 is ignored.
    const auto read1 = request(3, 1);
    const uint8_t noise[] = {0, 0xFF, read1[0], read1[1]};
    host.feed(noise, sizeof(noise), reply);
    assert(reply.empty());
    host.feed(read1.data() + 2, 3, reply);
    assert(reply.size() == 9 + 512 + 1);
    for (size_t i = 0; i < 4; ++i) assert(reply[i] == read1[i]);
    assert(reply[8] == xorBytes(reply.data(), 8));
    for (size_t i = 0; i < 512; ++i)
        assert(reply[9 + i] == static_cast<uint8_t>(i ^ 0xA5));
    assert(reply.back() == xorBytes(reply.data() + 9, 512));

    // Drive 2 has its own image. A missing block is signaled by a bad
    // header checksum and no payload, exactly as the guest driver expects.
    reply.clear();
    const auto read2 = request(5, 0);
    host.feed(read2.data(), read2.size(), reply);
    assert(reply.size() == 522);
    assert(reply[9] == 0x5A);
    reply.clear();
    const auto missing = request(5, 1);
    host.feed(missing.data(), missing.size(), reply);
    assert(reply.size() == 9);
    assert(reply[8] != xorBytes(reply.data(), 8));

    // A write is one 5-byte envelope plus 512 data bytes and their XOR.
    // Check the reply and then read from the actual image to prove persistence.
    const auto write1 = request(2, 0);
    std::vector<uint8_t> write(write1.begin(), write1.end());
    for (size_t i = 0; i < 512; ++i)
        write.push_back(static_cast<uint8_t>(i * 7));
    write.push_back(xorBytes(write.data() + 5, 512));
    reply.clear();
    host.feed(write.data(), 100, reply);
    assert(reply.empty());
    host.feed(write.data() + 100, write.size() - 100, reply);
    assert(reply.size() == 5);
    for (size_t i = 0; i < 4; ++i) assert(reply[i] == write[i]);
    assert(reply[4] == write.back());
    std::array<uint8_t, 512> persisted{};
    std::ifstream(path1, std::ios::binary).read(
        reinterpret_cast<char*>(persisted.data()), persisted.size());
    for (size_t i = 0; i < 512; ++i)
        assert(persisted[i] == static_cast<uint8_t>(i * 7));

    // Corrupt write checksum: the reply must fail and the image stay intact.
    write.back() ^= 0x80;
    reply.clear();
    feed(host, write, reply);
    assert(reply.size() == 5 && reply[4] != xorBytes(write.data() + 5, 512));
    std::ifstream(path1, std::ios::binary).read(
        reinterpret_cast<char*>(persisted.data()), persisted.size());
    for (size_t i = 0; i < 512; ++i)
        assert(persisted[i] == static_cast<uint8_t>(i * 7));

    // Reject a corrupt envelope and recover for the next valid request.
    reply.clear();
    auto bad = read1;
    bad[4] ^= 1;
    host.feed(bad.data(), bad.size(), reply);
    assert(reply.empty());
    host.feed(read1.data(), read1.size(), reply);
    assert(reply.size() == 522);

    host.close();
    assert(host.open({path1.string(), ""}, {true, false}, error));
    assert(host.readOnly()[0]);
    write.back() ^= 0x80; // restore valid payload checksum
    reply.clear();
    feed(host, write, reply);
    assert(reply.size() == 5 && reply[4] != xorBytes(write.data() + 5, 512));
    host.close();
    assert(!host.open({path1.string(), path1.string() + ".missing"}, error));
    assert(!error.empty());
    fs::remove(path1);
    fs::remove(path2);
}
