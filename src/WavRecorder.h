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

// WavRecorder — the mixed audio bus, recorded to a WAV file (AUDIO-013).
//
// Two halves, because the bus is mixed on the REALTIME audio thread, where a
// file write (or a lock a writer could hold) is a dropout:
//
//   AudioCaptureRing  single-producer single-consumer ring, allocated once.
//                     `AudioDevice::mixSources` pushes the post-master,
//                     post-clamp interleaved stereo it just produced — what
//                     the speakers get. A full ring drops frames and counts
//                     them; it never blocks or allocates.
//   WavFileWriter     16-bit PCM, streamed to a temporary sibling and
//                     published under the real name only on `close()` (the
//                     header's sizes patched first), so a crash mid-take
//                     never leaves a truncated file under the user's name.
//
// The UI thread drains the ring into the writer once a frame. In the
// headless tool the device is OFFLINE and the same thread mixes and drains,
// sample counts derived from emulated cycles — so two runs of one program
// give the same file, byte for byte.

#ifndef POM2_WAV_RECORDER_H
#define POM2_WAV_RECORDER_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

class WavFileWriter
{
public:
    ~WavFileWriter();

    /// Start a 16-bit PCM file. False with `error` set on failure.
    bool open(const std::filesystem::path& path, uint32_t sampleRate, int channels,
              std::string& error);
    bool isOpen() const { return file_ != nullptr; }

    /// Interleaved float frames in [-1, 1]; clamped and rounded to 16 bits.
    void write(const float* interleaved, std::size_t frames);

    /// Patch the header, sync, and publish under the name given to open() —
    /// or under `publishAs` when one is given (the GUI asks for the name
    /// when the take ends, as the MIDI recorder does).
    bool close(std::string& error, const std::filesystem::path& publishAs = {});
    /// Drop the take: nothing appears under the target name.
    void discard();

    uint64_t framesWritten() const { return frames_; }
    uint32_t sampleRate() const { return rate_; }

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path target_, temp_;
    uint32_t rate_ = 0;
    int      channels_ = 0;
    uint64_t frames_ = 0;
    bool     writeFailed_ = false;
    std::vector<int16_t> scratch_;
};

class AudioCaptureRing
{
public:
    /// `capacityFrames` of interleaved stereo, allocated here and only here.
    explicit AudioCaptureRing(std::size_t capacityFrames);

    /// Audio thread. Never blocks, never allocates. Inline: AudioDevice calls
    /// it, and every binary that links AudioDevice need not link the writer.
    void push(const float* interleavedStereo, int frames) noexcept;

    /// Consumer thread: everything pushed so far, into the writer.
    std::size_t drainTo(WavFileWriter& writer);

    uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr int kChannels = 2;
    std::vector<float> buf_;
    std::size_t capacity_;                    // in frames
    std::atomic<std::size_t> head_{0};        // frames written (producer)
    std::atomic<std::size_t> tail_{0};        // frames read (consumer)
    std::atomic<uint64_t> dropped_{0};
};

inline void AudioCaptureRing::push(const float* in, int frames) noexcept
{
    if (frames <= 0 || capacity_ == 0) return;
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t room = capacity_ - (head - tail);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(frames), room);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t slot = ((head + i) % capacity_) * kChannels;
        buf_[slot]     = in[i * kChannels];
        buf_[slot + 1] = in[i * kChannels + 1];
    }
    head_.store(head + n, std::memory_order_release);
    if (n < static_cast<std::size_t>(frames))
        dropped_.fetch_add(static_cast<uint64_t>(frames) - n, std::memory_order_relaxed);
}

} // namespace pom2

#endif // POM2_WAV_RECORDER_H
