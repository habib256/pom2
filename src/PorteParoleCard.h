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

// Credit: Sébastien Pourcel (GitHub mocheez13) — his PorteParole-MEA8000,
// an emulation of this card checked against a real one (GPL-2.0-or-later,
// third_party/porteparole-mea8000/, THIRD-PARTY.md), is the reference this
// card uses directly (CARDS-015).
//
// PorteParoleCard — Ediciel's "Porte Parole" (Matra & Hachette, 1983,
// board CE 638009 L01): a French text-to-speech card built round an RTC /
// Philips MEA8000 (`Mea8000.h`). Developed by Eric Cheval at Matra with the
// CNET and Philips (manual, back page). Catalog key `porteparole`.
//
// ─── The card, from its manual ──────────────────────────────────────────
//
// [B] Ediciel "Porte Parole" manual, Annexe 5 "Caractéristiques techniques
// de la carte Porte Parole" (pp. 66-67):
//   * "Il est directement connecté sur le bus ; le bit d'adresse A0 est
//     utilisé pour spécifier l'envoi d'une donnée ou d'une commande."
//   * "$C0X0 (données en écriture ou statut en lecture), $C0X1 (commande
//     envoyée au synthétiseur)" — X = 8 + slot. Only A0 is decoded, so the
//     pair repeats across $C0n0-$C0nF; a read at either address is the
//     status ([B] MEA8000 data sheet Fig. 5 note: "A0 is a don't care").
//   * "Il a besoin d'une horloge externe de 4MHz élaborée par un oscillateur
//     à quartz" — X1 reads 4.000 on the board photo. That is the data
//     sheet's maximum, not its 3.84 MHz nominal: the voice runs 4 % high
//     (4 MHz / 480 native samples per second).
//   * "Le synthétiseur donne son statut en bit D7 : si le bit est à zéro,
//     pas besoin de données ; s'il est à 1, une donnée doit être envoyée."
//     D0-D6 float ([B] data sheet Fig. 5), so they read the Apple's floating
//     bus here, not MAME's zeros.
//   * Slot 4 by default; the disks' CONFIGURE moves the software to another
//     (Annexe 4). No slot ROM: the driver is on the disk (Annexe 2 lists it,
//     polling $C0C0 bit 7 and writing $C0C0 / $C0C1).
//
// ─── Not modelled ───────────────────────────────────────────────────────
//
//   * The optional memory: socket Z7 takes a 2 KB 2716 REPROM or 6116 RAM,
//     mapped at $C800-$CEFF and selected by any $Cnxx access, deselected by
//     $CFxx (Annexe 5); "la carte Porte Parole est normalement livrée sans
//     mémoire associée". No software that fills it is known, so the card
//     answers $Cnxx and $C800 with the floating bus, as a card without it.
//   * The audio path: an on-board LF amplifier drives J1 (an external
//     speaker) or J2 (the Apple's own speaker, unplugged from the
//     motherboard and plugged into the card). POM2 mixes the card as its
//     own source beside the speaker. [F] Whether the Apple's beep still
//     reaches the speaker through J2 is not known.
//   * RESET: the MEA8000 has no reset pin ([B] data sheet pinout), and the
//     manual shows no reset wiring, so Ctrl-Reset leaves an utterance
//     running; only power (and STOP) resets the chip.
//   * IRQ: the card has none; REQ is polled.
//   * The 74LS74 that synchronises the write strobe to the MEA8000's clock
//     (Annexe 5): the write lands in the chip at the CPU's access cycle.

#ifndef POM2_PORTE_PAROLE_CARD_H
#define POM2_PORTE_PAROLE_CARD_H

#include "AudioSource.h"
#include "Mea8000.h"
#include "SlotPeripheral.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class M6502;

namespace pom2 {

class PorteParoleCard : public SlotPeripheral
{
public:
    /// [B] manual p. 8: "Insérer la carte dans la fente n° 4".
    static constexpr int kDefaultSlot = 4;
    /// [B] manual Annexe 5 + the 4.000 crystal on the board.
    static constexpr double kChipClockHz = 4000000.0;
    explicit PorteParoleCard(int slot = kDefaultSlot);
    ~PorteParoleCard() override;

    int getSlot() const { return slot_; }
    std::string_view name() const override { return "Porte Parole (MEA8000)"; }

    uint8_t deviceSelectRead(uint8_t low4) override;
    void deviceSelectWrite(uint8_t low4, uint8_t value) override;

    AudioSource* audioSource() override;
    void setCpu(M6502* cpu);
    void setCpuClock(double hz) override;
    void advanceCycles(int cycles) override;
    void onPowerCycle() override;
    void onUnplug() override;
    void onTimeJump() override;

    void setSampleRate(uint32_t hz);
    void setVolume(float value);
    float getVolume() const;
    void setMuted(bool value);
    bool isMuted() const;

    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    /// For tests and the debugger: the chip as it stands (copied under the
    /// lock), and the host-rate samples queued for the mixer.
    Mea8000 chipSnapshot() const;
    std::size_t queuedSamples() const;
    /// Drain up to `count` queued samples (mono, before volume) — tests.
    std::vector<float> drainSamples(std::size_t count);

private:
    struct AudioSrc;
    void sync();
    void syncAt(uint64_t now);
    void advance(uint64_t cycles);
    void pushSample(int16_t level);

    std::unique_ptr<AudioSrc> audio_;
    int slot_;
    mutable std::mutex mtx_;
    M6502* cpu_ = nullptr;
    uint64_t lastCycle_ = 0;
    Mea8000 chip_;
    double cpuHz_ = 1022727.0;
    double clockRemainder_ = 0.0; ///< cycles * 4 MHz remainder, modulo cpuHz_
    uint32_t samplePhase_ = 0;   ///< host resampling phase, modulo 48000
    int16_t previousSample_ = 0;
    std::vector<int16_t> scratch_;
    std::deque<float> pcm_;
    uint32_t rate_ = kAudioSampleRate;
    float volume_ = 0.7f;
    bool muted_ = false;
};

} // namespace pom2

#endif // POM2_PORTE_PAROLE_CARD_H
