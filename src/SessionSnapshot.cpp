// POM2 — GPL-3.0-or-later
#include "SessionSnapshot.h"
#include "SessionIIc.h"
#include "DiskIICard.h"
#include "ClockCard.h"
#include "Ay3_8910.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "NoSlotClock.h"
#include "ZipChip.h"
#include "SnesMaxCard.h"
#include "SlinkyCard.h"
#include "SnapshotIO.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace pom2 {
namespace {
using Card = SessionArchive::Card;
using Blob = std::vector<uint8_t>;
bool safeMockingboardState(const Blob& bytes, bool speech)
{
    // Current MBS3 layout. Canonical replay below checks all other fields.
    const size_t size = 6 + 2 * Via6522::kSnapshotBytes + 2 * Ay3_8910::kSnapshotBytes +
                        (speech ? Ssi263::kSnapshotBytes : 0);
    if (bytes.size() != size || bytes[3] != (speech ? 3 : 2) || bytes[4] != (speech ? 1 : 0) ||
        bytes[5] != (speech ? 0x1F : 0x0F)) return false;
    auto u32 = [&](size_t p) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(bytes[p + i]) << (8 * i);
        return v;
    };
    for (int chip = 0; chip < 2; ++chip) {
        const size_t p = 6 + chip * Via6522::kSnapshotBytes;
        // Armed counters carry at most the 16-bit latch plus two cycles.
        // Negative/wild values would overflow signed subtraction on advance.
        if (u32(p + 9) > 65537 || u32(p + 17) > 65537) return false;
    }
    for (int chip = 0; chip < 2; ++chip) {
        const size_t p = 6 + 2 * Via6522::kSnapshotBytes + chip * Ay3_8910::kSnapshotBytes;
        if ((bytes[p + 16] & 0x70) || bytes[p + 17] > 3) return false;
    }
    return true;
}
bool safeClockState(const Blob& bytes)
{
    // CLK1: six shift bytes, edge/mode latches, offset, three timer words,
    // and three boolean lines. The card's rewind loader accepts larger
    // blobs and clamps invalid timers; a session requires exact state.
    if (bytes.size() != 36 || bytes[0] != 'C' || bytes[1] != 'L' ||
        bytes[2] != 'K' || bytes[3] != '1' || bytes[11] > 7 ||
        bytes[12] > 1 || bytes[33] > 1 || bytes[34] > 1 || bytes[35] > 1)
        return false;
    uint64_t offset = 0;
    for (int i = 0; i < 8; ++i) offset |= uint64_t(bytes[13 + i]) << (8 * i);
    constexpr uint64_t kMaxOffset = 400ull * 366 * 24 * 60 * 60;
    if (offset > kMaxOffset && offset < UINT64_MAX - kMaxOffset) return false;
    uint32_t rate = 0;
    for (int i = 0; i < 4; ++i) rate |= uint32_t(bytes[21 + i]) << (8 * i);
    return rate == 0 || rate == 32 || rate == 64 || rate == 256 || rate == 2048;
}
bool configuration(Memory& memory, Blob& out, std::string& error)
{
    const auto* accelerator = memory.cpuSocketAccelerator();
    if (accelerator && !dynamic_cast<const ZipChip*>(accelerator)) {
        error = "unsupported session socket accelerator"; return false;
    }
    const auto* clock = memory.getNoSlotClock();
    out = {1, uint8_t(clock != nullptr), uint8_t(clock && clock->isEnabled()),
           uint8_t(clock ? clock->slot() : 0)};
    if (accelerator) {
        out[0] = 2; out.push_back(1);
        out.push_back(static_cast<const ZipChip*>(accelerator)->bootFast() ? 1 : 0);
    }
    return true;
}
bool describe(SlotPeripheral* card, SessionArchive::Slot& out, std::string& error)
{
    if (!card) return true;
    if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
        out.kind = Card::DiskII;
        disk->appendSessionIdentity(out.identity);
    } else if (auto* mb = dynamic_cast<MockingboardCard*>(card)) {
        out.kind = mb->getVariant() == MockingboardCard::Variant::AC ? Card::Mockingboard : Card::SoundII;
    } else if (dynamic_cast<SnesMaxCard*>(card)) out.kind = Card::SnesMax;
    else if (auto* slinky = dynamic_cast<SlinkyCard*>(card)) {
        if (!slinky->romLoaded()) { error = "sessions require the Slinky firmware"; return false; }
        out.kind = Card::Slinky;
        slinky->appendSessionIdentity(out.identity);
    }
    else if (auto* clock = dynamic_cast<ClockCard*>(card)) {
        out.kind = Card::Clock;
        out.identity.push_back(clock->takesC800() ? 1 : 0);
        uint8_t value = 0;
        for (int i = 0; i < 256; ++i) {
            clock->peekSlotRom(uint8_t(i), value);
            out.identity.push_back(value);
        }
        if (clock->takesC800())
            for (int i = 0; i < 0x800; ++i) {
                clock->peekExpansionRom(uint16_t(i), value);
                out.identity.push_back(value);
            }
    }
    else return describeIIcSessionDevice(card, out, error);
    return true;
}
}

bool captureSession(SessionArchive& out, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error)
{
    SessionArchive staged;
    staged.machineId = machineId;
    if (!machineId) { error = "session requires a configured machine profile"; return false; }
    if (!configuration(memory, staged.configuration, error)) return false;
    size_t mediaBytes = 0;
    if (memory.slotBus().peripheral(0)) { error = "sessions require an empty slot 0"; return false; }
    if (memory.isIIcClass() && !captureSessionSonyMedia(memory, staged.slots[0], mediaBytes, error)) return false;
    for (int slot = 1; slot <= 7; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        auto& record = staged.slots[slot];
        if (!describe(card, record, error)) return false;
        if (!card) continue;
        if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
            for (int drive = 0; drive < 2; ++drive)
                mediaBytes += disk->driveImage(drive).sessionStorageBytes();
            if (mediaBytes > 128u * 1024u * 1024u) {
                error = "session media expansion exceeds 128 MiB"; return false;
            }
            if (!disk->appendSessionState(record.state, error)) return false;
        } else if (uint8_t(record.kind) > uint8_t(Card::Clock)) {
            if (!captureIIcSessionDevice(card, record.state, mediaBytes, error)) return false;
        } else card->appendSnapshotState(record.state);
    }
    SnapshotWriter writer(staged.core, machineId);
    captureMachineState(writer, cpu, memory);
    if (!writer.finish()) { error = "session core capture failed"; return false; }
    out = std::move(staged);
    return true;
}

bool restoreSession(const SessionArchive& session, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error)
{
    Blob config;
    if (session.version < 1 || session.version > 7) {
        error = "unsupported session version"; return false;
    }
    const size_t cardLimit = session.version == 1 ? 1024u * 1024u : kMaxSessionCardBytes;
    if (session.core.size() > 17u * 1024u * 1024u) {
        error = "session core is too large"; return false;
    }
    if (!configuration(memory, config, error)) return false;
    // A //c Plus archive written before its built-in accelerator was modelled
    // (2026-10-21) carries the 4-byte v1 configuration. The accelerator is
    // soldered, so that is the same machine: compare the clock bytes only.
    // Refusing it locked out every older //c Plus session, with an error
    // asking for a configuration nobody can select (bug hunt 2026-10-23).
    const auto& saved = session.configuration;
    const bool sameConfig = saved == config ||
        (memory.isIIcPlus() && saved.size() == 4 && saved[0] == 1 &&
         config.size() == 6 && std::equal(saved.begin() + 1, saved.end(), config.begin() + 1));
    if (!machineId || session.machineId != machineId || !sameConfig) {
        error = "session machine/clock configuration differs; select the matching configuration first";
        return false;
    }
    if (memory.slotBus().peripheral(0)) { error = "sessions require an empty slot 0"; return false; }
    std::array<std::unique_ptr<DiskIICard::SessionRestore>, 8> disks;
    std::array<std::unique_ptr<SlotPeripheral>, 8> devices;
    SessionSonyMedia sonyMedia;
    size_t mediaBytes = 0;
    if (memory.isIIcClass()) {
        if (session.version < 4) { error = "//c sessions require format version 4"; return false; }
        if (!prepareSessionSonyMedia(memory, session.slots[0], sonyMedia, mediaBytes, error, session.version >= 5)) return false;
    } else if (session.slots[0].kind != Card::Empty || !session.slots[0].identity.empty() || !session.slots[0].state.empty()) {
        error = "sessions require an empty slot 0"; return false;
    }
    for (int slot = 1; slot <= 7; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        SessionArchive::Slot live;
        if (!describe(card, live, error)) return false;
        const auto& record = session.slots[slot];
        if (session.version < 7 && uint8_t(record.kind) >= uint8_t(Card::Cffa)) {
            error = "this card requires session format version 7"; return false;
        }
        if (session.version < 6 && record.kind == Card::HardDisk) {
            error = "HDV sessions require format version 6"; return false;
        }
        if (session.version < 4 && uint8_t(record.kind) > uint8_t(Card::Clock)) {
            error = "this device requires session format version 4"; return false;
        }
        if (session.version < 3 && (record.kind == Card::Slinky || record.kind == Card::Clock)) {
            error = "this card requires session format version 3"; return false;
        }
        if (record.state.size() > cardLimit || record.identity.size() > 16384) {
            error = "session card record is too large"; return false;
        }
        if (live.kind != record.kind || live.identity != record.identity ||
            (record.kind == Card::Empty && !record.state.empty())) {
            error = "session card type/firmware differs in slot " + std::to_string(slot); return false;
        }
        if (!card) continue;
        if (auto* disk = dynamic_cast<DiskIICard*>(card)) {
            disks[slot] = std::make_unique<DiskIICard::SessionRestore>();
            if (!disk->prepareSessionState(record.state.data(), record.state.size(), *disks[slot], error,
                                          session.version >= 2, session.version >= 5))
                return false;
            for (const auto& media : disks[slot]->media) mediaBytes += media->sessionStorageBytes();
            if (mediaBytes > 128u * 1024u * 1024u) {
                error = "session media expansion exceeds 128 MiB"; return false;
            }
        } else if (uint8_t(record.kind) > uint8_t(Card::Clock)) {
            if (!prepareIIcSessionDevice(card, record, slot, devices[slot], mediaBytes, error, session.version >= 5, session.version >= 6)) return false;
        } else {
            if (record.kind == Card::Clock && !safeClockState(record.state)) {
                error = "invalid session clock state"; return false;
            }
            if (record.kind != Card::SnesMax && record.kind != Card::Slinky &&
                record.kind != Card::Clock &&
                !safeMockingboardState(record.state, record.kind == Card::SoundII)) {
                error = "invalid session Mockingboard register/timer state"; return false;
            }
            // A detached device validates the exact current wire format.
            // Require a canonical round trip so silent loader refusals and
            // clamped/partial/foreign blobs cannot enter a live session.
            std::unique_ptr<SlotPeripheral> validator;
            if (record.kind == Card::SnesMax) validator = std::make_unique<SnesMaxCard>(slot);
            else if (record.kind == Card::Slinky) validator = std::make_unique<SlinkyCard>(slot);
            else if (record.kind == Card::Clock) validator = std::make_unique<ClockCard>(slot);
            else validator = std::make_unique<MockingboardCard>(slot,
                record.kind == Card::SoundII ? MockingboardCard::Variant::SoundII : MockingboardCard::Variant::AC);
            const double hz = pom2VideoTiming(memory.videoStandard()).cpuClockHz;
            validator->setCpuClock(hz); validator->setStandardClock(hz);
            validator->loadSnapshotState(record.state.data(), record.state.size());
            Blob canonical;
            validator->appendSnapshotState(canonical);
            if (canonical != record.state) { error = "invalid session card state in slot " + std::to_string(slot); return false; }
        }
    }
    SnapshotReader core(session.core.data(), session.core.size());
    if (!core.good() || core.machineId() != machineId) {
        error = "invalid or mismatched session core"; return false;
    }
    bool haveEnv = false;
    std::string name;
    uint32_t len;
    while (core.nextSection(name, len)) {
        if (name == "ENV") haveEnv = true;
        core.skipCurrentSection();
    }
    if (!haveEnv || !core.good()) { error = "session core lacks valid environment metadata"; return false; }
    // Core preflight is the final fallible step. All card/media payloads
    // above are validated and prepared; no host-backed card is admitted.
    SnapshotReader reader(session.core.data(), session.core.size());
    const auto result = restoreMachineState(reader, cpu, memory);
    if (!result.ok) { error = result.error; return false; }
    applySessionSonyMedia(memory, sonyMedia);
    for (int slot = 1; slot <= 7; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        if (!card) continue;
        if (disks[slot]) static_cast<DiskIICard*>(card)->applySessionState(std::move(*disks[slot]));
        else if (devices[slot]) applyIIcSessionDevice(card, devices[slot].get());
        else {
            const auto& state = session.slots[slot].state;
            card->loadSnapshotState(state.data(), state.size());
        }
    }
    return true;
}
}
