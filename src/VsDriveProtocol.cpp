// POM2 Apple II Emulator — GPL-3.0-or-later
#include "VsDriveProtocol.h"

#include "Block512Backing.h"   // pom2::noteMediaWrite

#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ctime>
#include <utility>

namespace pom2 {
namespace {

uint8_t checksum(const uint8_t* data, size_t count)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum ^= data[i];
    return sum;
}

std::array<uint8_t, 4> prodosDateTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const uint16_t clock = static_cast<uint16_t>(local.tm_min | local.tm_hour << 8);
    const uint16_t date = static_cast<uint16_t>(local.tm_mday |
        (local.tm_mon + 1) << 5 | ((local.tm_year + 1900) % 100) << 9);
    return {static_cast<uint8_t>(clock), static_cast<uint8_t>(clock >> 8),
            static_cast<uint8_t>(date), static_cast<uint8_t>(date >> 8)};
}

} // namespace

VsDriveProtocol::~VsDriveProtocol() { close(); }

bool VsDriveProtocol::open(const std::array<std::string, 2>& paths,
                           std::string& error)
{
    return open(paths, {}, error);
}

bool VsDriveProtocol::open(const std::array<std::string, 2>& paths,
                           const std::array<bool, 2>& readOnly,
                           std::string& error)
{
    close();
    if (paths[0].empty() && paths[1].empty()) {
        error = "Select at least one ProDOS image.";
        return false;
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].empty()) continue;
        std::FILE* file = readOnly[i] ? nullptr : std::fopen(paths[i].c_str(), "r+b");
        const bool writable = file != nullptr;
        if (!file) file = std::fopen(paths[i].c_str(), "rb");
        if (!file) {
            error = "Cannot open VSDrive " + std::to_string(i + 1) + ": " + paths[i];
            close();
            return false;
        }
        if (std::fseek(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            error = "Cannot measure VSDrive image: " + paths[i];
            close();
            return false;
        }
        const long length = std::ftell(file);
        if (length < 512 || length % 512 != 0 || length > 65536L * 512L) {
            std::fclose(file);
            error = "VSDrive image must contain 1–65536 ProDOS blocks: " + paths[i];
            close();
            return false;
        }
        files_[i] = file;
        blocks_[i] = static_cast<uint32_t>(length / 512);
        writable_[i] = writable;
        readOnly_[i] = !writable;
        paths_[i] = paths[i];
    }
    error.clear();
    resetPacket();
    return true;
}

std::FILE* VsDriveProtocol::liveFile(std::size_t drive)
{
    std::FILE* f = files_[drive];
#if !defined(_WIN32)
    if (!f || paths_[drive].empty()) return f;
    struct stat open{}, named{};
    if (::fstat(fileno(f), &open) != 0 || ::stat(paths_[drive].c_str(), &named) != 0)
        return f;
    if (open.st_dev == named.st_dev && open.st_ino == named.st_ino) return f;
    std::FILE* fresh = std::fopen(paths_[drive].c_str(), writable_[drive] ? "r+b" : "rb");
    if (!fresh) return f;
    if (std::fseek(fresh, 0, SEEK_END) == 0) {
        const long length = std::ftell(fresh);
        if (length >= 512 && length % 512 == 0 && length <= 65536L * 512L) {
            std::fclose(f);
            files_[drive] = fresh;
            blocks_[drive] = static_cast<uint32_t>(length / 512);
            return fresh;
        }
    }
    std::fclose(fresh);
#endif
    return f;
}

void VsDriveProtocol::close()
{
    for (auto& file : files_) {
        if (file) std::fclose(file);
        file = nullptr;
    }
    blocks_.fill(0);
    writable_.fill(false);
    readOnly_.fill(false);
    paths_ = {};
    resetPacket();
}

void VsDriveProtocol::resetPacket()
{
    used_ = 0;
    needed_ = 5;
}

void VsDriveProtocol::feed(const uint8_t* bytes, size_t count,
                            std::vector<uint8_t>& replies)
{
    for (size_t i = 0; i < count; ++i) {
        const uint8_t byte = bytes[i];
        if (used_ == 0 && byte != 0xC5) continue;
        packet_[used_++] = byte;
        if (used_ == 5) {
            const uint8_t cmd = packet_[1];
            // $01 is ADTPro's original read (drive 1, no timestamp); $02-$05
            // are write/read for drives 1 and 2 (CommsThread.java). $01 was
            // refused, so a client that used it hung waiting for a reply.
            if (checksum(packet_.data(), 4) != packet_[4] || cmd < 1 || cmd > 5) {
                resetPacket();
                continue;
            }
            needed_ = (cmd & 1) ? 5 : 518;
        }
        if (used_ == needed_) {
            respond(replies);
            resetPacket();
        }
    }
}

void VsDriveProtocol::respond(std::vector<uint8_t>& replies)
{
    const uint8_t cmd = packet_[1];
    const size_t drive = cmd == 1 ? 0 : static_cast<size_t>((cmd - 2) >> 1);
    const uint32_t block = static_cast<uint32_t>(packet_[2] | packet_[3] << 8);
    std::FILE* file = liveFile(drive);
    const bool inRange = file && block < blocks_[drive];
    std::array<uint8_t, 512> data{};

    if (cmd & 1) {
        bool ok = inRange && std::fseek(file, static_cast<long>(block * 512), SEEK_SET) == 0 &&
                  std::fread(data.data(), 1, data.size(), file) == data.size();
        // $01 carries no date/time: the 4-byte echo and its checksum only.
        const size_t headerLen = cmd == 1 ? 4 : 8;
        std::array<uint8_t, 8> header{};
        for (size_t i = 0; i < 4; ++i) header[i] = packet_[i];
        const auto date = prodosDateTime();
        if (cmd != 1) for (size_t i = 0; i < 4; ++i) header[i + 4] = date[i];
        replies.insert(replies.end(), header.begin(), header.begin() + headerLen);
        replies.push_back(static_cast<uint8_t>(checksum(header.data(), headerLen) + !ok));
        if (ok) {
            replies.insert(replies.end(), data.begin(), data.end());
            replies.push_back(checksum(data.data(), data.size()));
        }
    } else {
        const uint8_t dataXor = checksum(packet_.data() + 5, 512);
        const bool valid = packet_[517] == dataXor;
        bool ok = valid && inRange && writable_[drive] &&
                  std::fseek(file, static_cast<long>(block * 512), SEEK_SET) == 0 &&
                  std::fwrite(packet_.data() + 5, 1, 512, file) == 512 &&
                  std::fflush(file) == 0;
        // Durable before the guest is told "written" (the media layer's
        // commit rule): flushed to the OS was not on the disk.
#if defined(_WIN32)
        if (ok) ok = _commit(_fileno(file)) == 0;
#else
        if (ok) ok = ::fsync(fileno(file)) == 0;
#endif
        // In place on the host file: a rewind must not cross it (CLAUDE.md).
        if (ok) noteMediaWrite();
        replies.insert(replies.end(), packet_.begin(), packet_.begin() + 4);
        replies.push_back(static_cast<uint8_t>(dataXor + !ok));
    }
}

} // namespace pom2
