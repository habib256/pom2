// POM2 — GPL-3.0-or-later
#include "SessionSnapshot.h"
#include "DiskIICard.h"
#include "Ay3_8910.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "NoSlotClock.h"
#include "SnesMaxCard.h"
#include "SnapshotIO.h"

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
bool configuration(Memory& memory, Blob& out, std::string& error)
{
    if (memory.isIIcClass() || memory.cpuSocketAccelerator()) {
        error = "sessions support II/II+/IIe without a socket accelerator"; return false;
    }
    const auto* clock = memory.getNoSlotClock();
    out = {1, uint8_t(clock != nullptr), uint8_t(clock && clock->isEnabled()),
           uint8_t(clock ? clock->slot() : 0)};
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
    else { error = "sessions do not support card: " + std::string(card->name()); return false; }
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
    if (session.version != 1 && session.version != 2) {
        error = "unsupported session version"; return false;
    }
    const size_t cardLimit = session.version == 1 ? 1024u * 1024u : kMaxSessionCardBytes;
    if (session.core.size() > 17u * 1024u * 1024u) {
        error = "session core is too large"; return false;
    }
    if (!configuration(memory, config, error)) return false;
    if (!machineId || session.machineId != machineId || session.configuration != config) {
        error = "session machine/clock configuration differs; select the matching configuration first";
        return false;
    }
    if (memory.slotBus().peripheral(0) || session.slots[0].kind != Card::Empty ||
        !session.slots[0].identity.empty() || !session.slots[0].state.empty()) {
        error = "sessions require an empty slot 0"; return false;
    }
    std::array<std::unique_ptr<DiskIICard::SessionRestore>, 8> disks;
    size_t mediaBytes = 0;
    for (int slot = 1; slot <= 7; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        SessionArchive::Slot live;
        if (!describe(card, live, error)) return false;
        const auto& record = session.slots[slot];
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
                                          session.version >= 2))
                return false;
            for (const auto& media : disks[slot]->media) mediaBytes += media->sessionStorageBytes();
            if (mediaBytes > 128u * 1024u * 1024u) {
                error = "session media expansion exceeds 128 MiB"; return false;
            }
        } else {
            if (record.kind != Card::SnesMax &&
                !safeMockingboardState(record.state, record.kind == Card::SoundII)) {
                error = "invalid session Mockingboard register/timer state"; return false;
            }
            // A detached device validates the exact current wire format.
            // Require a canonical round trip so silent loader refusals and
            // clamped/partial/foreign blobs cannot enter a live session.
            std::unique_ptr<SlotPeripheral> validator;
            if (record.kind == Card::SnesMax) validator = std::make_unique<SnesMaxCard>(slot);
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
    for (int slot = 1; slot <= 7; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        if (!card) continue;
        if (disks[slot]) static_cast<DiskIICard*>(card)->applySessionState(std::move(*disks[slot]));
        else {
            const auto& state = session.slots[slot].state;
            card->loadSnapshotState(state.data(), state.size());
        }
    }
    return true;
}
}
