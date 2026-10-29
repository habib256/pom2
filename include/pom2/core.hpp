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
#include <functional>
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

/// The Apple Mouse Interface Card. AppleMouse runs Apple's own firmware on
/// both chips — the slot EPROM 341-0270-C and the MC68705 MCU's mask ROM
/// 341-0269, emulated as in MAME. AppleMouseAppleWin runs the same slot
/// EPROM against AppleWin's high-level model of the MCU (one ROM).
enum class MouseModel : std::uint8_t {
    AppleMouse,
    AppleMouseAppleWin,
};

/// An RGB card on the video output. LeChatMauveFeline is the //e's
/// aux-slot card; LeChatMauveIIcAdapter is the "Adaptateur IIc" on the //c's
/// DB-15. Both are told their mode by the 80COL/AN3 sequence the guest
/// writes; neither has a slot ROM or I/O space for a program to detect.
enum class RgbCardModel : std::uint8_t {
    LeChatMauveFeline,
    LeChatMauveIIcAdapter,
};

/// What an RGB card makes of double hi-res: 140 cells of 16 colours, 560
/// dots in black and white, or the two mixed per byte (bit 7 of each byte).
enum class RgbDhgrMode : std::uint8_t {
    Color140,
    Mono560,
    Mixed,
};

/// A named machine, or Custom to take `cpu` and `iieMemory` as given.
/// AppleIIPlus = NMOS, II+ memory; AppleIIe = NMOS, //e memory;
/// AppleIIeEnhanced = 65C02, //e memory; AppleIIc = 65C02, //e memory, the
/// //c's built-in mouse on port 4 (its firmware is in the system ROM, so
/// moveMouse()/setMouse() drive it with no attachMouse()), and a 32 KiB ROM
/// loaded as a //c dump (bank 0 at reset). AppleIIcPlus is the //c+: the same,
/// its mouse firmware at $C700, plus the MIG, the IWM and the two 3.5" drives
/// its ROM drives — reached through the SmartPort calls below — and a Disk II
/// in slot 6 from the start. It runs at 1 MHz: the CGGA accelerator is the
/// application's, not the facade's. The ROM itself is still loadRom().
/// New machines are added at the end.
enum class Machine : std::uint8_t {
    Custom,
    AppleIIPlus,
    AppleIIe,
    AppleIIeEnhanced,
    AppleIIc,
    AppleIIcPlus,
};

/// Construction-time machine choices. Slot cards remain opt-in: the public
/// facade starts with an empty expansion bus and never opens a host device.
/// `machine` is LAST so the positional `{cpu, timing, iieMemory}` form
/// written before it existed still compiles and means the same machine.
struct CoreConfig {
    CpuModel   cpu       = CpuModel::NMOS6502;   ///< Custom only
    VideoTiming timing   = VideoTiming::NTSC;
    bool       iieMemory = false;                ///< Custom only
    Machine    machine   = Machine::Custom;
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

/// The video soft switches as the motherboard holds them, read without side
/// effects. On a ][/][+ only the first four (and an3) exist; the others stay
/// false. `doubleHiRes` is the //e's DHIRESON latch ($C05E, AN3 low): the
/// screen shows double hi-res only with !text, hiRes and eightyColumn too.
struct DisplayState {
    bool text         = true;    ///< $C050 off / $C051 on
    bool mixed        = false;   ///< $C052 / $C053
    bool page2        = false;   ///< $C054 / $C055
    bool hiRes        = false;   ///< $C056 / $C057
    bool eightyColumn = false;   ///< 80COL, $C00C / $C00D
    bool doubleHiRes  = false;   ///< $C05E on / $C05F off
    bool an3          = false;   ///< set by $C05F, cleared by $C05E, every machine
    bool altCharSet   = false;   ///< ALTCHAR, $C00E / $C00F
    bool eightyStore  = false;   ///< 80STORE, $C000 / $C001
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

    /// Load an Apple II system ROM: common sizes are 12/16/20/32 KiB.
    /// A 20 KiB dump skips its 4 KiB prefix; other accepted sizes in
    /// 2..64 KiB use the loader's best-effort top-of-memory placement.
    /// For //c-style 32 KiB dumps pass lowerBankFor32K=true; //e combined
    /// dumps use the upper bank. Character ROMs load separately.
    bool loadRom(std::string_view path, bool lowerBankFor32K = false);
    bool loadCharacterRom(std::string_view path, int bank = 0);
    std::string lastError() const;

    /// Install the core's Disk II controller, with its two drives, in `slot`
    /// (1-7, 6 by convention). The P5 boot ROM is required, except on a //c
    /// or //c+ (pass it empty: their $C600 is the system ROM). The P6
    /// logic-state-sequencer ROM is optional for .dsk/.po/.nib media but
    /// REQUIRED for a WOZ: POM2 embeds neither PROM (both are Apple's), and
    /// insertDisk() refuses a WOZ on a controller without one. The //c+'s
    /// built-in controller starts with no sequencer: call
    /// attachDiskII({}, "roms/diskii_p6.rom") to give it one. A second call
    /// replaces the controller, moving it if `slot` differs; a //c's disk
    /// port is wired to slot 6, so another slot is refused there.
    bool attachDiskII(std::string_view bootRom,
                      std::string_view sequencerRom = {},
                      int slot = 6);
    /// Drive 0 or 1 (drive 1 and drive 2 of the controller).
    bool insertDisk(int drive, std::string_view path);
    bool ejectDisk(int drive);
    bool diskInserted(int drive = 0) const;
    /// Write `drive`'s guest writes back to its file now, without ejecting.
    /// True when there was nothing to write.
    bool flushDisk(int drive);
    /// Protect the mounted disk, as a sticker over its notch would: the guest
    /// gets the drive's write-protect signal and nothing reaches the image or
    /// its file. Lasts until the disk is ejected or replaced, and does not
    /// touch the file's own read-only bit. Lifting it cannot unprotect a
    /// read-only file, a write-protected WOZ/2IMG, or a run under
    /// POM2_MEDIA_WRITE_DEFAULT=protected.
    bool setDiskWriteProtected(int drive, bool protectedOn);
    /// The protection the guest sees, whatever its cause.
    bool diskWriteProtected(int drive = 0) const;
    /// Cold boot directly at the controller's boot PROM ($Cn00). Returns
    /// false unless the controller, its boot ROM (on a //c or //c+, whose
    /// $C600 is the system ROM, not needed) and a disk in drive 0 are
    /// present — the PROM boots drive 1 only — and false, after the cold
    /// boot, when the machine maps its own firmware over that page.
    bool bootDisk();

    /// Install a 3.5" SmartPort controller in an empty slot (5 by convention)
    /// with two 800 KiB units, ProDOS drive 1 and drive 2 of that slot. The
    /// card is POM2's block-level model with its synthetic slot ROM — no
    /// Liron dump is read — identifying as a ProDOS block device ($Cn07=$01).
    /// It has no mechanical timing: a block costs exactly the CPU cycles of
    /// the driver's 512-byte copy loop, so runs are cycle-reproducible.
    /// On Machine::AppleIIcPlus there is nothing to attach (false): its two
    /// built-in 3.5" drives answer the calls below, unit 0 the internal one
    /// and unit 1 the external one, and bootSlot(5) boots them.
    bool attachSmartPort(int slot = 5);
    /// Mount raw ProDOS-order 400/800 KiB, supported 2IMG or GCR WOZ/MOOF on
    /// unit 0 or 1. Captures are block-write-protected; MFM/1.44 MB is disabled.
    /// Mounting over a disk first writes that disk's changed
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
    /// signature ($Cn01=$20, $Cn03=$00, $Cn05=$03), with RAM untouched; and
    /// false, after the cold boot, when the machine maps its own firmware
    /// over that page out of reset (slot 3 on a //e, slots other than 5 on a
    /// //c). The Disk II's slot is bootDisk(). On a //c+, slot 5 is a cold
    /// boot through the ROM's own scan — internal drive first, the external
    /// one when the internal bay is empty — and needs a 3.5" disk in either.
    bool bootSlot(int slot);

    /// Load a program tape (.aci/.wav/.mp3/.ogg/.flac). Playback is controlled
    /// by the host transport below, while the guest sees the real $C060 input.
    bool loadCassette(std::string_view path);
    /// Save output captured from guest accesses to $C020. A .wav suffix picks
    /// WAV; other suffixes select ACI. No suffix is appended automatically.
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
    /// convention). SoundII adds the SSI263 speech synthesizer. On a
    /// Machine::AppleIIc it is the Mockingboard 4c on the internal connector
    /// (`slot` is ignored): dormant until the guest writes into $C400-$C4FF,
    /// then answering that page — over the mouse firmware — until a power
    /// cycle. On AppleIIcPlus it is the 4c+, the same way, over the ROM's
    /// memory-expansion firmware.
    bool attachMockingboard(int slot = 4,
                            MockingboardModel model = MockingboardModel::AC);
    bool mockingboardAttached() const;
    void setMockingboardVolume(float volume);

    /// Install an Apple Mouse card in an empty slot (4 by convention: the
    /// slot Apple sold it for, and the one software that skips the scan
    /// calls). A Machine::AppleIIc has its mouse built in and refuses this.
    /// Reads `mouse_341-0270-c.bin` (and, for AppleMouse,
    /// `mouse_341-0269.bin`) from `romDirectory`. Its interrupts — movement,
    /// button, VBL at the beam's line 192 — reach the 6502 as in the
    /// application.
    bool attachMouse(int slot = 4, MouseModel model = MouseModel::AppleMouse,
                     std::string_view romDirectory = "roms");
    bool mouseAttached() const;
    /// Install an RGB card (slot 7 by convention). The Féline needs a slotted
    /// machine and the //c adapter a Machine::AppleIIc; on a //c `slot` only
    /// names where the adapter is recorded (POM2 keeps it on slot 7).
    /// renderFrame() keeps painting the composite picture: the card changes
    /// what rgbDhgrMode() reports, not the software framebuffer.
    bool attachRgbCard(int slot = 7,
                       RgbCardModel model = RgbCardModel::LeChatMauveFeline);
    bool rgbCardAttached() const;
    /// The card's double hi-res mode now; Color140 (its power-on and reset
    /// mode) when no card is attached.
    RgbDhgrMode rgbDhgrMode() const;
    /// Relative motion and the button, what the card's hardware receives.
    /// Exact on both models; on AppleMouse keep each axis within ±127
    /// between two run() slices (the MCU reads an 8-bit counter).
    void moveMouse(int dx, int dy, bool button);
    /// Absolute position in the firmware's clamp window (the units of the
    /// screen holes $0478+n / $04F8+n). A closed loop, as in the
    /// application: each call pushes one correction of at most ±127 per
    /// axis toward the target, which the card takes as the guest runs; call
    /// it every frame and a large jump converges in a few. Returns true once
    /// the card reports the target.
    bool setMouse(int x, int y, bool button);

    /// CPU/RAM reset variants: soft reset preserves A/X/Y while updating
    /// PC/P/SP; hard reset clears A/X/Y, and cold boot also clears RAM.
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

    /// Inject a 7-bit key. With no pending paste, a newer key replaces an
    /// unread latch value; while paste bytes remain, the key appends to them.
    /// II/II+ memory mode folds ASCII lowercase to uppercase.
    void queueKey(std::uint8_t ascii);
    /// Physical key state for IIe AKD ($C010), independent of the strobe.
    /// Tokens in 0..511 distinguish simultaneous host keys. queueKey still
    /// supplies character/repeat events; these calls do not inject a byte.
    void setKeyHeld(std::uint16_t token, bool down);
    void releaseHeldKeys();
    /// Queue clipboard text, normalizing line endings to CR and filtering
    /// controls except CR/HT. Well-formed UTF-8 uses best-effort ASCII
    /// transliteration; other buffers use Apple high-ASCII masking.
    /// II/II+ mode folds lowercase to uppercase. At most 4096 characters
    /// including pending input are accepted. Returns output characters
    /// queued, not input bytes consumed; it does not wait for guest reads.
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
    /// span of CPU time. This is the filtered source signal with speaker
    /// gain applied, without the stereo mixer's final [-1, +1] clamp.
    /// Returns frameCount after filling, or zero for a null buffer, zero
    /// frames, or a zero sample rate; it does not advance the CPU.
    std::size_t pullSpeakerAudio(float* monoOutput, std::size_t frameCount,
                                 std::uint32_t sampleRate = 44100);
    /// Speaker gain in [0, 2]. Values outside the range are clamped.
    void setSpeakerVolume(float volume);

    /// Pull the complete interleaved stereo mix: centred built-in speaker and
    /// cassette plus Mockingboard AY1 on the left and AY2 on the right when
    /// attached. Uses the shared mixer with unity master gain and stereo
    /// output clamped to [-1, +1]. Fills and returns frameCount, or returns
    /// zero for a null buffer, zero frames, or a zero sample rate, without
    /// advancing the CPU. The buffer must hold frameCount * 2 floats. This
    /// consumes the same speaker timeline as pullSpeakerAudio(); a host should
    /// use one API or the other, not both for the same emulated time span.
    std::size_t pullAudio(float* interleavedStereo, std::size_t frameCount,
                          std::uint32_t sampleRate = 44100);

    CpuState cpuState() const;
    /// Overwrite A, X, Y, P, S and PC at the current instruction boundary;
    /// the next step()/run() executes at `programCounter`. `cycles` and
    /// `halted` are ignored, and nothing else moves: the cycle counter, the
    /// soft switches (80STORE, RAMRD/RAMWRT, ALTZP, language card, video)
    /// and a pending IRQ are left as they are — which is what a RESET-vector
    /// trampoline could not do. Bit 5 of P reads 1 on every 6502, so it is
    /// forced. Same on the NMOS 6502 and the 65C02. Call between run()/step().
    void setCpuState(const CpuState& state);

    DisplayState displayState() const;
    /// Speaker toggles (accesses to $C030-$C03F) since the Core was created,
    /// a host read()/write() there included — as on the bus, it clicks.
    std::uint64_t speakerToggleCount() const;

    /// Called for each CPU bus access to a watched address, after it was
    /// performed, with the value read or written and the cycle it lands on
    /// (cpuState().cycles counts the same clock). Opcode fetches, interrupt
    /// stack pushes and soft-switch reads count; read()/write() from the
    /// host do not. A read watch sends every read through the slow path
    /// while armed, so arm only what you watch. Do not call run() or step()
    /// from inside the callback.
    using BusWatch = std::function<void(std::uint16_t address,
                                        std::uint8_t value, bool write,
                                        std::uint64_t cycle)>;
    void setBusWatch(BusWatch watch);                 ///< empty: off
    /// Arm [first, last] for reads and/or writes. Watches add up; returns
    /// false when first > last.
    bool watchAddresses(std::uint16_t first, std::uint16_t last,
                        bool reads, bool writes);
    void clearWatchedAddresses();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pom2

#endif // POM2_CORE_HPP
