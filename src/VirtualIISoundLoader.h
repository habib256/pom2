// POM2 Apple II Emulator — GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace pom2 {

// Both miniaudio and AudioToolbox can accept the available prefix of a
// truncated AIFF. Validate its declared container before either decoder.
inline bool virtualIIContainerComplete(const std::string& path)
{
    if (std::filesystem::path(path).extension() != ".aiff") return true;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto bytes = input.tellg();
    if (bytes < 12) return false;
    input.seekg(0);
    unsigned char header[12];
    if (!input.read(reinterpret_cast<char*>(header), sizeof(header)) ||
        std::string(reinterpret_cast<const char*>(header), 4) != "FORM") return false;
    const uint64_t declared = (uint64_t(header[4]) << 24) |
        (uint64_t(header[5]) << 16) | (uint64_t(header[6]) << 8) | header[7];
    if (declared < 4 || declared + 8 > static_cast<uint64_t>(bytes)) return false;
    return true;
}

// Virtual ][ RAW recordings: mono signed 16-bit little-endian, 44.1 kHz.
// Bound allocations like the WAV loaders; never rewrite the source files.
inline bool decodeVirtualIIOriginal(const std::string& path,
                                    std::vector<float>& data, uint32_t& rate,
                                    uint64_t maxFrames)
{
    const auto extension = std::filesystem::path(path).extension();
    if (extension == ".raw") {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const auto bytes = file.tellg();
        if (bytes <= 0 || bytes % 2 != 0 ||
            static_cast<uint64_t>(bytes) / 2 > maxFrames) return false;
        file.seekg(0);
        data.resize(static_cast<size_t>(bytes) / 2);
        for (float& sample : data) {
            unsigned char pair[2];
            if (!file.read(reinterpret_cast<char*>(pair), 2)) return false;
            const int value = pair[0] | (static_cast<int>(pair[1]) << 8);
            sample = static_cast<float>(value >= 32768 ? value - 65536 : value) / 32768.0f;
        }
        rate = 44100;
        return true;
    }
#ifdef __APPLE__
    if (extension != ".aiff") return false;
    if (!virtualIIContainerComplete(path)) return false;
    const auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.data()), path.size(), false);
    if (!url) return false;
    ExtAudioFileRef file = nullptr;
    const OSStatus opened = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (opened != noErr) return false;
    struct Close { ExtAudioFileRef file; ~Close() { ExtAudioFileDispose(file); } } close{file};
    AudioStreamBasicDescription source{};
    UInt32 size = sizeof(source);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat,
                               &size, &source) != noErr ||
        source.mSampleRate < 1 || source.mSampleRate > 192000) return false;
    SInt64 frames = 0;
    size = sizeof(frames);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames,
                               &size, &frames) != noErr ||
        frames <= 0 || static_cast<uint64_t>(frames) > maxFrames) return false;
    AudioStreamBasicDescription client{};
    client.mSampleRate = source.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    client.mBytesPerPacket = client.mBytesPerFrame = sizeof(float);
    client.mFramesPerPacket = client.mChannelsPerFrame = 1;
    client.mBitsPerChannel = 32;
    if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                               sizeof(client), &client) != noErr) return false;
    data.resize(static_cast<size_t>(frames));
    AudioBufferList buffer{};
    buffer.mNumberBuffers = 1;
    buffer.mBuffers[0].mNumberChannels = 1;
    buffer.mBuffers[0].mDataByteSize = static_cast<UInt32>(data.size() * sizeof(float));
    buffer.mBuffers[0].mData = data.data();
    UInt32 read = static_cast<UInt32>(frames);
    // A short read means the file ended before its advertised frame count.
    // Do not mark a truncated required recording as a usable sound bank.
    if (ExtAudioFileRead(file, &read, &buffer) != noErr ||
        read != static_cast<UInt32>(frames)) return false;
    data.resize(read);
    rate = static_cast<uint32_t>(source.mSampleRate);
    return true;
#else
    return false;
#endif
}

} // namespace pom2
