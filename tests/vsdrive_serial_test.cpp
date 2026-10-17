// POM2 Apple II Emulator — GPL-3.0-or-later
#include "SuperSerialCard.h"
#include "VsDriveTransport.h"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "pom2-vsdrive-serial.po";
    {
        std::array<uint8_t, 512> block{};
        for (size_t i = 0; i < block.size(); ++i)
            block[i] = static_cast<uint8_t>(i ^ 0xC5);
        std::ofstream(path, std::ios::binary).write(
            reinterpret_cast<const char*>(block.data()), block.size());
    }

    SuperSerialCard card(2);
    std::string error;
    auto transport = pom2::makeVsDriveTransport(card, {path.string(), ""}, error);
    assert(transport && error.empty());
    card.setTransport(std::move(transport));
    card.setRawMode(true);
    card.setModemLinesTied(true);
    card.deviceSelectWrite(0xA, 0x01); // DTR asserted, RX enabled
    card.deviceSelectWrite(0xB, 0x0F); // 19200 baud
    assert(card.startListening(0));
    assert(card.vsDriveActive());
    assert(card.vsDrivePath(0) == path.string());

    // Send the exact five bytes produced by a2filecmd's VSDrive read path
    // through the emulated ACIA, then read the native host's 522-byte reply.
    constexpr std::array<uint8_t, 5> request{0xC5, 0x03, 0, 0, 0xC6};
    for (uint8_t b : request) card.deviceSelectWrite(0x8, b);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (card.rxQueueDepth() < 522 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(card.rxQueueDepth() == 522);
    std::vector<uint8_t> reply;
    while (card.rxQueueDepth()) reply.push_back(card.deviceSelectRead(0x8));
    assert(reply.size() == 522);
    for (size_t i = 0; i < 4; ++i) assert(reply[i] == request[i]);
    uint8_t headerXor = 0;
    for (size_t i = 0; i < 8; ++i) headerXor ^= reply[i];
    assert(reply[8] == headerXor);
    uint8_t blockXor = 0;
    for (size_t i = 0; i < 512; ++i) {
        assert(reply[9 + i] == static_cast<uint8_t>(i ^ 0xC5));
        blockXor ^= reply[9 + i];
    }
    assert(reply[521] == blockXor);

    card.stopListening();
    assert(!card.vsDriveActive());
    fs::remove(path);
}
