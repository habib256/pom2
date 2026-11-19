// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// WavRecorder — see the header.

#include "WavRecorder.h"

#include "AtomicFileReplace.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

namespace {

void le16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }

// The canonical 44-byte RIFF/WAVE header for 16-bit PCM.
void buildHeader(uint8_t h[44], uint32_t rate, int channels, uint64_t frames)
{
    const uint32_t blockAlign = static_cast<uint32_t>(channels) * 2;
    const uint64_t data64 = frames * blockAlign;
    const uint32_t data = data64 > 0xFFFFFFFFu - 36 ? 0xFFFFFFFFu - 36 : static_cast<uint32_t>(data64);
    std::copy_n("RIFF", 4, h);       le32(h + 4, 36 + data);
    std::copy_n("WAVE", 4, h + 8);
    std::copy_n("fmt ", 4, h + 12);  le32(h + 16, 16);
    le16(h + 20, 1);                 le16(h + 22, static_cast<uint16_t>(channels));
    le32(h + 24, rate);              le32(h + 28, rate * blockAlign);
    le16(h + 32, static_cast<uint16_t>(blockAlign));
    le16(h + 34, 16);
    std::copy_n("data", 4, h + 36);  le32(h + 40, data);
}

} // namespace

WavFileWriter::~WavFileWriter() { discard(); }

bool WavFileWriter::open(const std::filesystem::path& path, uint32_t sampleRate,
                         int channels, std::string& error)
{
    discard();
    target_ = path;
    temp_   = tempSiblingPath(path);
    std::error_code ec;
    if (!prepareTempPath(temp_, ec)) { error = "cannot prepare " + temp_.u8string() + ": " + ec.message(); return false; }
#if defined(_WIN32)
    file_ = _wfopen(temp_.c_str(), L"wb");
#else
    file_ = std::fopen(temp_.c_str(), "wb");
#endif
    if (!file_) { error = "cannot create " + temp_.u8string(); return false; }
    rate_ = sampleRate; channels_ = channels; frames_ = 0; writeFailed_ = false;
    uint8_t h[44];
    buildHeader(h, rate_, channels_, 0);
    if (std::fwrite(h, 1, sizeof h, file_) != sizeof h) writeFailed_ = true;
    return true;
}

void WavFileWriter::write(const float* interleaved, std::size_t frames)
{
    if (!file_ || frames == 0) return;
    const std::size_t n = frames * static_cast<std::size_t>(channels_);
    scratch_.resize(n * 2);
    uint8_t* out = reinterpret_cast<uint8_t*>(scratch_.data());
    for (std::size_t i = 0; i < n; ++i) {
        const float v = std::clamp(interleaved[i], -1.0f, 1.0f);
        const int s = static_cast<int>(std::lround(v * 32767.0f));
        le16(out + i * 2, static_cast<uint16_t>(static_cast<int16_t>(s)));   // little-endian on any host
    }
    if (std::fwrite(out, 1, n * 2, file_) != n * 2) writeFailed_ = true;
    frames_ += frames;
}

bool WavFileWriter::close(std::string& error, const std::filesystem::path& publishAs)
{
    if (!publishAs.empty()) target_ = publishAs;
    if (!file_) { error = "no recording in progress"; return false; }
    uint8_t h[44];
    buildHeader(h, rate_, channels_, frames_);
    bool ok = !writeFailed_ && std::fseek(file_, 0, SEEK_SET) == 0
              && std::fwrite(h, 1, sizeof h, file_) == sizeof h;
    ok = (std::fclose(file_) == 0) && ok;
    file_ = nullptr;
    std::error_code ec;
    if (!ok) { error = "write failed: " + temp_.u8string(); std::filesystem::remove(temp_, ec); return false; }
    if (!replaceFileAtomic(temp_, target_, ec)) {
        error = "cannot publish " + target_.u8string() + ": " + ec.message();
        std::filesystem::remove(temp_, ec);
        return false;
    }
    return true;
}

void WavFileWriter::discard()
{
    if (!file_) return;
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(temp_, ec);
}

AudioCaptureRing::AudioCaptureRing(std::size_t capacityFrames)
    : buf_(capacityFrames * kChannels, 0.0f), capacity_(capacityFrames)
{
}

std::size_t AudioCaptureRing::drainTo(WavFileWriter& writer)
{
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    std::size_t done = 0;
    while (tail + done < head) {
        const std::size_t at = (tail + done) % capacity_;
        const std::size_t run = std::min(head - (tail + done), capacity_ - at);
        writer.write(&buf_[at * kChannels], run);
        done += run;
    }
    tail_.store(tail + done, std::memory_order_release);
    return done;
}

} // namespace pom2
