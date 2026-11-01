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

// Host audio policy shared by the frontend composition paths. The coordinator
// owns the complete registration inventory and resolves live slot cards under
// the machine lock. Slot cards may disappear during a profile rebuild while
// AudioDevice's callback is still running, so neither teardown nor UI state may
// be reconstructed from retained MainWindow aliases.

#ifndef POM2_AUDIO_COORDINATOR_H
#define POM2_AUDIO_COORDINATOR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class AudioDevice;
class AudioSource;
class CassetteDevice;
class EmulationController;
class FloppySoundDevice;
class SpeakerDevice;

namespace pom2 {

class PrinterSoundDevice;
class Settings;

class AudioCoordinator final
{
public:
    enum class CardKind : std::uint8_t {
        Mockingboard,
        Phasor,
        EchoPlus,
        EchoPlusTms5220,
        Sam,            ///< S.A.M. 8-bit DAC (SamCard)
    };
    /// Number of `CardKind` values. Anything that indexes an array by kind
    /// sizes it from here: the legacy type-wide key table in `persist()` was
    /// hard-coded to 3 while the enum had 4, so the last kind fell off the
    /// end of an `index < legacy.size()` guard instead of failing to build.
    static constexpr std::size_t kCardKindCount = 5;

    struct CardMixSettings {
        float volume = 1.0f;
        bool muted = false;
    };

    struct InventorySnapshot {
        std::vector<int> mockingboardSlots;
        std::vector<int> phasorSlots;
        std::vector<int> echoPlusSlots;
        std::vector<int> echoPlusTmsSlots;

        int primaryMockingboardSlot() const noexcept;
    };

    struct MixerCardSnapshot {
        CardKind kind = CardKind::Mockingboard;
        int slot = -1;
        float volume = 1.0f;
        bool muted = false;
        float peak = 0.0f;
        float clicks = 0.0f;   ///< AudioSource::clicksPerSecond, the crackle detector
    };

    struct MixerCardCommand {
        CardKind kind = CardKind::Mockingboard;
        int slot = -1;
        float volume = 1.0f;
        bool muted = false;
    };

    struct ViaAySnapshot {
        std::uint8_t t1cl = 0, t1ch = 0, t1ll = 0, t1lh = 0;
        std::uint8_t sr = 0, acr = 0, pcr = 0, ifr = 0, ier = 0;
        std::array<std::uint8_t, 16> ay{};
        std::uint32_t viaWrites = 0, ayWrites = 0, ayResets = 0;
        std::uint32_t cmdInactive = 0, cmdRead = 0;
        std::uint32_t cmdWrite = 0, cmdLatch = 0;
    };

    struct Ssi263Snapshot {
        std::array<std::uint8_t, 5> regs{};
        std::uint8_t currentPhoneme = 0;
        std::uint8_t mode = 0;
        bool aRequest = false;
        bool powerDown = false;
        bool irqEnabled = false;
        int phonemeRemainingCycles = 0;
        std::uint32_t phonemeWriteCount = 0;
    };

    struct MockingboardSnapshot {
        bool plugged = false;
        int slot = -1;
        float volume = 1.0f;
        bool muted = false;
        bool slotIrq = false;
        std::array<ViaAySnapshot, 2> via{};
        bool hasSsi = false;
        Ssi263Snapshot ssi;
    };

    struct PhasorViaSnapshot {
        std::uint8_t t1cl = 0, t1ch = 0, t1ll = 0, t1lh = 0;
        std::uint8_t sr = 0, acr = 0, pcr = 0, ifr = 0, ier = 0;
        std::uint32_t writes = 0;
    };

    struct PhasorAySnapshot {
        std::array<std::uint8_t, 16> regs{};
        std::uint32_t writes = 0;
        std::uint32_t resets = 0;
    };

    struct PhasorSnapshot {
        bool plugged = false;
        int slot = -1;
        float volume = 1.0f;
        bool muted = false;
        bool slotIrq = false;
        std::uint8_t mode = 0;
        int clockScale = 1;
        std::array<PhasorViaSnapshot, 2> via{};
        std::array<PhasorAySnapshot, 4> ay{};
    };

    struct EchoPlusSnapshot {
        bool plugged = false;
        int slot = -1;
        float volume = 1.0f;
        bool muted = false;
        bool slotIrq = false;
        Ssi263Snapshot chip;
    };

    /// The one-argument form remains useful for the callback-inventory unit
    /// test. Slot discovery is available only on the composition-root form.
    explicit AudioCoordinator(AudioDevice& device) : device_(device) {}
    AudioCoordinator(AudioDevice& device, EmulationController& controller)
        : device_(device), controller_(&controller) {}
    ~AudioCoordinator();

    AudioCoordinator(const AudioCoordinator&) = delete;
    AudioCoordinator& operator=(const AudioCoordinator&) = delete;

    /// Register exactly once and retain the authoritative teardown inventory.
    void registerSource(AudioSource* source);
    /// Detach every registered source before any of their owners are destroyed.
    void unregisterAll();

    bool contains(const AudioSource* source) const;

    InventorySnapshot captureInventory() const;
    std::vector<MixerCardSnapshot> captureMixerCards() const;
    bool applyMixerCard(const MixerCardCommand& command);
    MockingboardSnapshot captureMockingboard() const;
    PhasorSnapshot capturePhasor() const;
    EchoPlusSnapshot captureEchoPlus() const;

    /// Slot-specific keys preserve independent levels when two variants
    /// coexist; legacy type-wide keys remain the fallback for old state.cfg.
    CardMixSettings restoreCardSettings(const Settings& settings,
                                        CardKind kind, int slot,
                                        float defaultVolume) const;

    // ── Device buffer (AudioBufferPolicy.h) ────────────────────────────
    struct BufferSnapshot {
        std::uint32_t periodFrames   = 0;   ///< the rung the device is open with
        std::uint32_t periods        = 0;
        std::uint32_t sampleRate     = 0;
        double        bufferMs       = 0.0;
        std::uint64_t lateCallbacks  = 0;   ///< since launch
        std::uint32_t pinnedFrames   = 0;   ///< 0 = automatic (the ladder)
        std::uint32_t minFrames      = 0;
        std::uint32_t maxFrames      = 0;   ///< the platform's top rung
        bool          available      = false;
    };
    BufferSnapshot captureBuffer() const;
    /// Pin the device to one rung (`frames`), or 0 to let the ladder decide
    /// again from the current rung. Re-opens the device at once.
    void pinBuffer(std::uint32_t frames);
    /// Once per UI frame: climbs one rung when the device reported
    /// `AudioBufferPolicy::kLateToEscalate` late callbacks inside one
    /// window (`kWindowSec`), unless a rung is pinned. The climb is
    /// remembered (`audio_buffer_learned`) so the next launch starts there.
    void tick();

    /// Restore host-only mixer policy. Persist writes both host controls and
    /// the live slot-card levels captured through the locked topology.
    void restore(Settings& settings,
                 SpeakerDevice& speaker,
                 CassetteDevice& cassette,
                 FloppySoundDevice& floppy525,
                 FloppySoundDevice& floppy35,
                 PrinterSoundDevice& printer);
    void persist(Settings& settings,
                 const SpeakerDevice& speaker,
                 const CassetteDevice& cassette,
                 const FloppySoundDevice& floppy525,
                 const FloppySoundDevice& floppy35,
                 const PrinterSoundDevice& printer) const;

private:
    AudioDevice&               device_;
    EmulationController*       controller_ = nullptr;
    std::vector<AudioSource*>  sources_;

    // The ladder's state (AudioBufferPolicy.h). UI thread only.
    std::uint32_t pinnedFrames_  = 0;   ///< 0 = automatic
    std::uint32_t learnedFrames_ = 0;   ///< highest rung the ladder reached
    std::uint64_t lateAtWindowStart_ = 0;
    double        windowStartSec_    = -1.0;
    void applyLadderRung(std::uint32_t frames, const std::string& why);
};

} // namespace pom2

#endif // POM2_AUDIO_COORDINATOR_H
