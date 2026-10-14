// POM2 Apple II Emulator
// Copyright (C) 2026
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or (at your
// option) any later version.

#ifndef POM2_CORE_HPP
#define POM2_CORE_HPP

#include <cstdint>
#include <memory>
#include <cstddef>
#include <string>
#include <string_view>

namespace pom2 {

/// CPU silicon selected for the public, deterministic core host.
enum class CpuModel : std::uint8_t {
    NMOS6502,
    CMOS65C02,
};

/// Motherboard timing used by video soft switches and cycle accounting.
enum class VideoTiming : std::uint8_t {
    NTSC,
    PAL,
};

enum class MockingboardModel : std::uint8_t {
    AC,
    SoundII,
};

/// Construction-time machine choices. Slot cards remain opt-in: the public
/// facade starts with an empty expansion bus and never opens a host device.
struct CoreConfig {
    CpuModel   cpu       = CpuModel::NMOS6502;
    VideoTiming timing   = VideoTiming::NTSC;
    bool       iieMemory = false;
};

/// Register snapshot taken at an instruction boundary.
struct CpuState {
    std::uint8_t  accumulator  = 0;
    std::uint8_t  x            = 0;
    std::uint8_t  y            = 0;
    std::uint8_t  status       = 0;
    std::uint8_t  stackPointer = 0;
    std::uint16_t programCounter = 0;
    std::uint64_t cycles       = 0;
    bool          halted       = false;
};

/// Non-owning view of the most recently rendered software framebuffer.
/// Each 32-bit pixel is packed as 0xAABBGGRR. The pointer remains valid until
/// the next renderFrame(), move-assignment, or destruction of its Core.
struct FramebufferView {
    const std::uint32_t* pixels = nullptr;
    int width  = 0;
    int height = 0;
};

/// Host-visible cassette transport state. Program tapes remain armed after
/// PLAY until the guest first polls $C060, matching the physical interface.
struct CassetteState {
    bool loaded       = false;
    bool playEngaged  = false;
    bool paused       = false;
    bool rewinding    = false;
    bool hasRecording = false;
    bool audioStream  = false;
};

/// Stable, single-threaded facade over POM2's CPU and motherboard model.
///
/// Core deliberately has no worker thread and performs no wall-clock pacing.
/// The embedding host owns the schedule and calls run() or step(). Instances
/// are independent, movable, and not safe for concurrent access.
class Core final {
public:
    explicit Core(CoreConfig config = {});
    ~Core();

    Core(Core&&) noexcept;
    Core& operator=(Core&&) noexcept;

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    /// Load a 12/16/32 KiB Apple II system ROM. For //c-style 32 KiB dumps,
    /// pass lowerBankFor32K=true; //e combined dumps use the upper bank.
    bool loadRom(std::string_view path, bool lowerBankFor32K = false);
    bool loadCharacterRom(std::string_view path, int bank = 0);
    std::string lastError() const;

    /// Install a Disk II controller in slot 6. The P5 boot ROM is required;
    /// the P6 logic-state-sequencer ROM is optional but recommended.
    bool attachDiskII(std::string_view bootRom,
                      std::string_view sequencerRom = {});
    bool insertDisk(int drive, std::string_view path);
    bool ejectDisk(int drive);
    bool diskInserted(int drive = 0) const;
    /// Cold boot directly at the slot-6 boot PROM. Returns false unless the
    /// controller, its boot ROM and at least one disk are present.
    bool bootDisk();

    /// Install a 3.5" SmartPort controller in an empty slot (5 by convention)
    /// with two 800 KiB units, ProDOS drive 1 and drive 2 of that slot. The
    /// card is POM2's block-level model with its synthetic slot ROM — no
    /// Liron dump is read — identifying as a ProDOS block device ($Cn07=$01).
    /// It has no mechanical timing: a block costs exactly the CPU cycles of
    /// the driver's 512-byte copy loop, so runs are cycle-reproducible.
    bool attachSmartPort(int slot = 5);
    /// Mount an 800 KiB (819 200-byte) ProDOS-order .po or an 800 KiB .2mg on
    /// unit 0 or 1. Mounting over a disk first writes that disk's changed
    /// blocks back to its file, as insertDisk() does for the Disk II. Guest
    /// writes reach the file on flushSmartPortDisk(), on the next insert or
    /// eject, and when the Core is destroyed. A read-only file mounts
    /// write-protected; POM2_MEDIA_WRITE_DEFAULT=protected protects them all.
    bool insertSmartPortDisk(int unit, std::string_view path);
    bool ejectSmartPortDisk(int unit);
    bool smartPortDiskInserted(int unit = 0) const;
    /// Write the unit's changed blocks back to its image file now, durably
    /// (temp file + rename). True when there was nothing to write.
    bool flushSmartPortDisk(int unit);
    /// Cold boot directly at the boot ROM of the card in `slot` ($Cn00).
    /// Returns false unless that slot's ROM carries the ProDOS boot
    /// signature ($Cn01=$20, $Cn03=$00, $Cn05=$03). Slot 6 is bootDisk().
    bool bootSlot(int slot);

    /// Load a program tape (.aci/.wav/.mp3/.ogg/.flac). Playback is controlled
    /// by the host transport below, while the guest sees the real $C060 input.
    bool loadCassette(std::string_view path);
    /// Save output captured from guest accesses to $C020. The extension picks
    /// .wav or the native pulse-based .aci format.
    bool saveCassette(std::string_view path);
    void playCassette();
    void stopCassette();
    void pauseCassette(bool paused);
    void rewindCassette();
    void ejectCassette();
    /// Start/clear capture of the guest's $C020 cassette-output transitions.
    void armCassetteRecording();
    void clearCassetteRecording();
    CassetteState cassetteState() const;
    /// Cassette monitor gain in [0, 2]. Values outside the range are clamped.
    void setCassetteVolume(float volume);

    /// Install a stereo Mockingboard in an empty expansion slot (slot 4 by
    /// convention). SoundII adds the SSI263 speech synthesizer.
    bool attachMockingboard(int slot = 4,
                            MockingboardModel model = MockingboardModel::AC);
    bool mockingboardAttached() const;
    void setMockingboardVolume(float volume);

    /// Reset variants mirror the emulator UI: warm soft reset preserves CPU
    /// registers, hard reset clears A/X/Y, and cold boot also clears RAM.
    void softReset();
    void hardReset();
    void coldBoot();

    /// Execute until at least minCycles elapsed. Instructions may overshoot
    /// the requested budget by a few cycles; the actual count is returned.
    int run(int minCycles);
    void step();

    /// Apple II bus accesses. Reads may have hardware side effects, exactly as
    /// they do for the emulated CPU; these are not debugger-style peeks.
    std::uint8_t read(std::uint16_t address);
    void write(std::uint16_t address, std::uint8_t value);

    void queueKey(std::uint8_t ascii);
    /// Physical key state for IIe AKD ($C010), independent of the strobe.
    /// Tokens in 0..511 distinguish simultaneous host keys. queueKey still
    /// supplies character/repeat events; these calls do not inject a byte.
    void setKeyHeld(std::uint16_t token, bool down);
    void releaseHeldKeys();
    std::size_t pasteText(std::string_view text);

    /// Drive one of the four Apple II paddle inputs directly. Values map to
    /// the RC discharge interval observed at $C064-$C067: 0 is shortest and
    /// 255 longest. Returns false for an index outside 0..3.
    bool setPaddle(int paddle, std::uint8_t value);
    /// Drive one of the three game-port push buttons exposed at $C061-$C063.
    /// Returns false for an index outside 0..2.
    bool setPaddleButton(int button, bool pressed);
    /// Convenience mapping for a modern two-axis joystick. Each finite axis
    /// is clamped from [-1,+1] onto paddles 0/1; non-finite values are centred.
    /// Dead zones, inversion and square-gate policy remain frontend concerns.
    void setJoystickAxes(float x, float y);

    /// Render the current Apple II video state entirely on the CPU. No GL
    /// context is involved; width is 280 or 560 depending on 80-column mode.
    FramebufferView renderFrame();

    /// Configure the host PCM rate before running code that drives cassette
    /// audio. Pull methods also apply their sampleRate argument, but cassette
    /// pulses are converted while the guest runs and therefore need the rate
    /// in advance when it differs from the 44100 Hz default.
    void setAudioSampleRate(std::uint32_t sampleRate);

    /// Pull mono float32 samples from the built-in 1-bit speaker. The caller
    /// owns both the buffer and scheduling; call after emulating the matching
    /// span of CPU time. Samples are in [-1, +1]. Returns zero for a null
    /// buffer, zero frames, or a zero sample rate.
    std::size_t pullSpeakerAudio(float* monoOutput, std::size_t frameCount,
                                 std::uint32_t sampleRate = 44100);
    /// Speaker gain in [0, 2]. Values outside the range are clamped.
    void setSpeakerVolume(float volume);

    /// Pull the complete interleaved stereo mix: centred built-in speaker and
    /// cassette plus Mockingboard AY1 on the left and AY2 on the right when
    /// attached. The output buffer must hold frameCount * 2 floats. This
    /// consumes the same speaker timeline as pullSpeakerAudio(); a host should
    /// use one API or the other, not both for the same emulated time span.
    std::size_t pullAudio(float* interleavedStereo, std::size_t frameCount,
                          std::uint32_t sampleRate = 44100);

    CpuState cpuState() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pom2

#endif // POM2_CORE_HPP
