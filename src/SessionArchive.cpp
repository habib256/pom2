// POM2 — GPL-3.0-or-later
#include "SessionArchive.h"
#include "SessionIO.h"

#include <array>
#include <cstring>
#include <utility>

namespace pom2 {
namespace {
uint32_t crc32(const uint8_t* data, size_t n)
{
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int bit = 0; bit < 8; ++bit) v = (v >> 1) ^ ((v & 1) ? 0xEDB88320u : 0);
            values[i] = v;
        }
        return values;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
}
bool encodeSessionArchive(const SessionArchive& session, std::vector<uint8_t>& out,
                          std::string& error)
{
    if ((session.version < 1 || session.version > 3) || !session.machineId || session.core.size() > 17u * 1024u * 1024u ||
        session.configuration.size() > 64) {
        error = "invalid session machine/core/configuration"; return false;
    }
    const size_t cardLimit = session.version == 1 ? 1024u * 1024u : kMaxSessionCardBytes;
    // Bound the total before allocating/copying the payload, even for
    // callers that construct an archive directly rather than decoding one.
    size_t total = 8 + session.configuration.size() + session.core.size();
    for (const auto& slot : session.slots) {
        const size_t room = kMaxSessionBytes - 24;
        if (slot.identity.size() > 16384 || slot.state.size() > cardLimit ||
            total > room || 9 + slot.identity.size() + slot.state.size() > room - total) {
            error = "session exceeds card or archive size limit"; return false;
        }
        total += 9 + slot.identity.size() + slot.state.size();
    }
    std::vector<uint8_t> payload;
    sessionio::blob(payload, session.configuration);
    sessionio::blob(payload, session.core);
    for (const auto& slot : session.slots) {
        if (slot.identity.size() > 16384 || slot.state.size() > cardLimit ||
            uint8_t(slot.kind) > uint8_t(session.version >= 3 ? SessionArchive::Card::Clock
                                                              : SessionArchive::Card::SnesMax) ||
            (slot.kind == SessionArchive::Card::Empty &&
             (!slot.identity.empty() || !slot.state.empty()))) {
            error = "invalid session card record"; return false;
        }
        payload.push_back(uint8_t(slot.kind));
        sessionio::blob(payload, slot.identity);
        sessionio::blob(payload, slot.state);
    }
    if (payload.size() > kMaxSessionBytes - 24) {
        error = "session exceeds 32 MiB"; return false;
    }
    std::vector<uint8_t> bytes{'P','O','M','2','S','E','S','S'};
    byteio::putU32(bytes, session.version);
    byteio::putU32(bytes, session.machineId);
    byteio::putU32(bytes, uint32_t(payload.size()));
    // Include the header in the checksum as well as the payload.
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    byteio::putU32(bytes, crc32(bytes.data(), bytes.size()));
    out = std::move(bytes);
    return true;
}

bool decodeSessionArchive(const std::vector<uint8_t>& bytes, SessionArchive& out,
                          std::string& error)
{
    if (bytes.size() < 24 || bytes.size() > kMaxSessionBytes ||
        std::memcmp(bytes.data(), "POM2SESS", 8)) {
        error = "not a supported POM2 session"; return false;
    }
    sessionio::Reader r(bytes.data(), bytes.size());
    r.pos = 8;
    SessionArchive staged;
    staged.version = r.u32();
    if (staged.version < 1 || staged.version > 3) { error = "unsupported session version"; return false; }
    const size_t cardLimit = staged.version == 1 ? 1024u * 1024u : kMaxSessionCardBytes;
    staged.machineId = r.u32();
    const uint32_t payload = r.u32();
    if (!staged.machineId || payload != bytes.size() - 24) {
        error = "invalid session header/length"; return false;
    }
    sessionio::Reader checksum(bytes.data() + bytes.size() - 4, 4);
    if (crc32(bytes.data(), bytes.size() - 4) != checksum.u32()) {
        error = "session checksum mismatch"; return false;
    }
    r.size -= 4;
    if (!r.blob(staged.configuration, 64) || !r.blob(staged.core, 17u * 1024u * 1024u)) {
        error = "invalid session core/configuration length"; return false;
    }
    for (auto& slot : staged.slots) {
        slot.kind = SessionArchive::Card(r.u8());
        if (uint8_t(slot.kind) > uint8_t(staged.version >= 3 ? SessionArchive::Card::Clock
                                                             : SessionArchive::Card::SnesMax) ||
            !r.blob(slot.identity, 16384) || !r.blob(slot.state, cardLimit) ||
            (slot.kind == SessionArchive::Card::Empty &&
             (!slot.identity.empty() || !slot.state.empty()))) {
            error = "invalid session card record"; return false;
        }
    }
    if (!r.done()) { error = "trailing or truncated session data"; return false; }
    out = std::move(staged);
    return true;
}
}
