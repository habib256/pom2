// POM2 — GPL-3.0-or-later
#include "SessionIIc.h"
#include "IIcMouse.h"
#include "CffaCard.h"
#include "SessionCards.h"
#include "LeChatMauveCard.h"
#include "Memory.h"
#include "SessionIO.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortCard.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "SuperSerialCard.h"
#include <utility>

namespace pom2 {
namespace {
using Blob = std::vector<uint8_t>;
using Card = SessionArchive::Card;
bool budget(size_t& total, const Disk35Image& image, std::string& error) {
    total += image.sessionStorageBytes();
    if (total <= (128u << 20)) return true;
    error = "session media expansion exceeds 128 MiB"; return false;
}
bool budget(size_t& total, const Block512Backing& image, std::string& error) {
    total += image.blockCount() * Block512Backing::kBlockBytes;
    if (total <= (128u << 20)) return true;
    error = "session media expansion exceeds 128 MiB"; return false;
}
Sony35Drive* sony(Memory& memory, int index) {
    auto* hub = memory.getSmartPortHub();
    return !hub ? nullptr : index == 0 ? hub->internal35() :
        index == 1 ? hub->external35() : hub->external35Second();
}
bool sonyReady(Sony35Drive* drive, std::string& error) {
    if (!drive || (!drive->isEjectPending() && (!drive->image() || drive->image()->sessionCaptureReady()))) return true;
    error = "3.5-inch disk has pending writes/eject"; return false;
}
// Session media use a private name. Rebase only the snapshot's path hashes,
// preserving partially received WRITE blocks, offsets and protocol results.
void sessionIdentities(SmartPortCard& card, Blob& runtime) {
    const size_t offset = runtime.size() - SmartPortCard::kMaxUnits * 8;
    for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) {
        uint64_t hash = 14695981039346656037ULL;
        if (card.unit(i) && card.unit(i)->isLoaded())
            for (unsigned char c : std::string("(session disk)")) { hash ^= c; hash *= 1099511628211ULL; }
        for (int j = 0; j < 8; ++j) runtime[offset + i * 8 + j] = uint8_t(hash >> (j * 8));
    }
}
}
bool describeIIcSessionDevice(SlotPeripheral* card, SessionArchive::Slot& out, std::string& error) {
    if (auto* cffa = dynamic_cast<CffaCard*>(card)) {
        out.kind = Card::Cffa; cffa->appendSessionIdentity(out.identity);
    } else if (auto* serial = dynamic_cast<SuperSerialCard*>(card)) {
        if (!serial->builtInPort() || !serial->sessionIdle()) {
            error = "sessions require idle built-in serial ports"; return false;
        }
        out.kind = Card::Serial;
        out.identity = {1, uint8_t(serial->cable()), serial->dipSwitch1(), serial->dipSwitch2(),
                        uint8_t(serial->printerTap()), uint8_t(serial->modemLinesTied()),
                        uint8_t(serial->irqDipEnabled()), uint8_t(serial->rawMode())};
    } else if (auto* hdv = dynamic_cast<ProDOSHardDiskCard*>(card)) {
        out.kind = Card::HardDisk;
        for (int i = 0; i < 256; ++i) out.identity.push_back(hdv->slotRomRead(uint8_t(i)));
    } else if (dynamic_cast<IIcMouse*>(card)) out.kind = Card::IIcMouse;
    else if (auto* rgb = dynamic_cast<LeChatMauveCard*>(card)) {
        out.kind = Card::ChatMauve; out.identity = {uint8_t(rgb->variant())};
    } else if (auto* port = dynamic_cast<SmartPortCard*>(card)) {
        if (port->isLironRomLoaded()) { error = "sessions require the built-in SmartPort firmware"; return false; }
        out.kind = Card::SmartPort;
        out.identity = {1, uint8_t(port->unitCount()), uint8_t(port->isSuperDriveMode())};
        for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) {
            auto* unit = port->unit(i);
            const bool isHdv = dynamic_cast<SmartPortHdvUnit*>(unit) != nullptr;
            if (unit && !isHdv && !dynamic_cast<SmartPort35Unit*>(unit)) {
                error = "unsupported session SmartPort unit type"; return false;
            }
            out.identity.push_back(isHdv ? 2 : unit ? 1 : 0);
        }
    } else if (describeAdditionalSessionCard(card, out)) {
    } else { error = "sessions do not support card: " + std::string(card->name()); return false; }
    return true;
}
bool captureIIcSessionDevice(SlotPeripheral* card, Blob& out, size_t& mediaBytes, std::string& error) {
    if (auto* cffa = dynamic_cast<CffaCard*>(card)) {
        return budget(mediaBytes, *cffa->blockBacking(), error) && cffa->appendSessionState(out, error);
    }
    if (auto* hdv = dynamic_cast<ProDOSHardDiskCard*>(card)) {
        Blob runtime; hdv->appendSnapshotState(runtime); sessionio::blob(out, runtime);
        for (int i = 0; i < 2; ++i) {
            Blob media;
            if (!budget(mediaBytes, hdv->backing(i), error) || !hdv->backing(i).appendSessionMedia(media, error)) return false;
            sessionio::blob(out, media);
        }
        return true;
    }
    auto* port = dynamic_cast<SmartPortCard*>(card);
    if (!port) { card->appendSnapshotState(out); return true; }
    Blob runtime;
    port->appendSnapshotState(runtime);
    sessionIdentities(*port, runtime);
    sessionio::blob(out, runtime);
    for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) {
        auto* unit = dynamic_cast<SmartPort35Unit*>(port->unit(i));
        Blob media;
        if (auto* hdv = dynamic_cast<SmartPortHdvUnit*>(port->unit(i))) {
            if (!budget(mediaBytes, *hdv->blockBacking(), error) ||
                !hdv->blockBacking()->appendSessionMedia(media, error)) return false;
        } else if (unit && (!budget(mediaBytes, unit->image(), error) ||
                     !unit->image().appendSessionMedia(media, error))) return false;
        sessionio::blob(out, media);
    }
    return true;
}
bool prepareIIcSessionDevice(SlotPeripheral* card, const SessionArchive::Slot& record, int slot,
                             std::unique_ptr<SlotPeripheral>& out, size_t& mediaBytes, std::string& error, bool allowWritable, bool allowBlocks) {
    Blob runtime = record.state;
    if (uint8_t(record.kind) >= uint8_t(Card::Transwarp)) {
        out = prepareAdditionalSessionCard(record, slot, card, error);
        return bool(out);
    } else if (record.kind == Card::Cffa) {
        auto prepared = std::make_unique<CffaCard>(slot);
        auto* live = static_cast<CffaCard*>(card);
        if (!prepared->installSessionIdentity(record.identity) ||
            !live->prepareSessionState(record.state, *prepared, error) ||
            !budget(mediaBytes, *prepared->blockBacking(), error)) return false;
        out = std::move(prepared); return true;
    } else if (record.kind == Card::Serial) {
        auto serial = std::make_unique<SuperSerialCard>(slot);
        serial->setBuiltInPort(true);
        // The SSC loader clamps masks but trusts this derived word width.
        if (runtime.size() != 17 || runtime[10] < 5 || runtime[10] > 8) {
            error = "invalid session serial register state"; return false;
        }
        out = std::move(serial);
    } else if (record.kind == Card::HardDisk) {
        if (!allowBlocks) { error = "HDV sessions require format version 6"; return false; }
        auto* live = static_cast<ProDOSHardDiskCard*>(card);
        auto hdv = std::make_unique<ProDOSHardDiskCard>(slot);
        sessionio::Reader r(record.state.data(), record.state.size());
        if (!r.blob(runtime, 32)) { error = "invalid session HDV runtime"; return false; }
        for (int i = 0; i < 2; ++i) {
            Blob media;
            if (!live->backing(i).sessionReplaceable()) { error = "HDV medium has pending host writes"; return false; }
            if (!r.blob(media, kMaxSessionCardBytes)) { error = "invalid session HDV media length"; return false; }
            if (!hdv->backing(i).loadSessionMedia(media.data(), media.size(), error) ||
                !budget(mediaBytes, hdv->backing(i), error)) return false;
        }
        if (!r.done()) { error = "trailing session HDV data"; return false; }
        out = std::move(hdv);
    } else if (record.kind == Card::IIcMouse) {
        // Keep the live card's firmware slot: 7 on a //c Plus (its mouse
        // firmware and screen holes moved there, TN Misc #8), 4 otherwise.
        // The default ctor said 4, so after a session load a //c Plus's
        // Mouse inspector and the AI /mouse reported the wrong port.
        out = std::make_unique<IIcMouse>(static_cast<IIcMouse*>(card)->getSlot());
    }
    else if (record.kind == Card::ChatMauve) {
        auto* rgb = static_cast<LeChatMauveCard*>(card);
        out = std::make_unique<LeChatMauveCard>(slot, rgb->variant());
    } else if (record.kind == Card::SmartPort) {
        auto* live = static_cast<SmartPortCard*>(card);
        auto port = std::make_unique<SmartPortCard>(slot);
        port->setSuperDriveMode(live->isSuperDriveMode());
        port->setUnitCount(live->unitCount());
        sessionio::Reader r(record.state.data(), record.state.size());
        if (!r.blob(runtime, 1u << 20)) { error = "invalid session SmartPort runtime"; return false; }
        for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) {
            Blob media;
            if (!r.blob(media, kMaxSessionCardBytes)) { error = "invalid session SmartPort media length"; return false; }
            if (auto* hdv = dynamic_cast<SmartPortHdvUnit*>(live->unit(i))) {
                if (!allowBlocks || !hdv->blockBacking()->sessionReplaceable()) {
                    error = "SmartPort HDV requires version 6 and no pending host writes"; return false;
                }
                auto unit = std::make_unique<SmartPortHdvUnit>();
                if (!unit->blockBacking()->loadSessionMedia(media.data(), media.size(), error) ||
                    !budget(mediaBytes, *unit->blockBacking(), error)) return false;
                port->setUnit(i, std::move(unit)); continue;
            }
            auto* existing = dynamic_cast<SmartPort35Unit*>(live->unit(i));
            if (!existing) {
                if (!media.empty()) { error = "unexpected session SmartPort medium"; return false; }
                continue;
            }
            if (!existing->image().sessionReplaceable()) { error = "SmartPort medium has pending writes"; return false; }
            auto unit = std::make_unique<SmartPort35Unit>();
            unit->setMfmCapable(live->isSuperDriveMode());
            if (!unit->image().loadSessionMedia(media.data(), media.size(), error, allowWritable) ||
                !budget(mediaBytes, unit->image(), error)) return false;
            port->setUnit(i, std::move(unit));
        }
        if (!r.done()) { error = "trailing session SmartPort data"; return false; }
        out = std::move(port);
    } else { error = "invalid session peripheral type"; return false; }
    out->loadSnapshotState(runtime.data(), runtime.size());
    Blob canonical;
    out->appendSnapshotState(canonical);
    if (canonical != runtime) { error = "invalid session peripheral state in slot " + std::to_string(slot); return false; }
    return true;
}
void applyIIcSessionDevice(SlotPeripheral* card, SlotPeripheral* prepared) {
    if (auto* cffa = dynamic_cast<CffaCard*>(card)) {
        cffa->applySessionState(*static_cast<CffaCard*>(prepared)); return;
    }
    Blob runtime;
    prepared->appendSnapshotState(runtime);
    if (auto* hdv = dynamic_cast<ProDOSHardDiskCard*>(card)) {
        auto* source = static_cast<ProDOSHardDiskCard*>(prepared);
        for (int i = 0; i < 2; ++i) hdv->backing(i).swapSessionMedia(source->backing(i));
    }
    if (auto* port = dynamic_cast<SmartPortCard*>(card)) {
        auto* source = static_cast<SmartPortCard*>(prepared);
        for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) port->setUnit(i, source->setUnit(i, nullptr));
    }
    card->loadSnapshotState(runtime.data(), runtime.size());
}
bool captureSessionSonyMedia(Memory& memory, SessionArchive::Slot& record, size_t& mediaBytes, std::string& error) {
    record.kind = Card::SonyMedia;
    for (int i = 0; i < 3; ++i) {
        auto* drive = sony(memory, i);
        auto* image = drive ? drive->image() : nullptr;
        if (!sonyReady(drive, error)) return false;
        record.identity.push_back(image ? 1 : 0);
        Blob media;
        if (image && (!budget(mediaBytes, *image, error) || !image->appendSessionMedia(media, error))) return false;
        sessionio::blob(record.state, media);
        Blob writeState;
        if (drive) drive->appendSessionWriteState(writeState);
        sessionio::blob(record.state, writeState);
    }
    return true;
}
bool prepareSessionSonyMedia(Memory& memory, const SessionArchive::Slot& record, SessionSonyMedia& out,
                             size_t& mediaBytes, std::string& error, bool allowWritable) {
    if (record.kind != Card::SonyMedia || record.identity.size() != 3 || record.state.size() > kMaxSessionCardBytes) {
        error = "invalid session motherboard media"; return false;
    }
    sessionio::Reader r(record.state.data(), record.state.size());
    for (int i = 0; i < 3; ++i) {
        auto* drive = sony(memory, i);
        auto* image = drive ? drive->image() : nullptr;
        Blob media;
        if (record.identity[size_t(i)] != uint8_t(image != nullptr) || !sonyReady(drive, error) ||
            (image && !image->sessionReplaceable()) ||
            !r.blob(media, (16u << 20) + 32) || (!image && !media.empty())) {
            error = "session motherboard drive configuration differs or has pending writes"; return false;
        }
        if (image) {
            out[size_t(i)] = std::make_unique<Disk35Image>();
            if (!out[size_t(i)]->loadSessionMedia(media.data(), media.size(), error, allowWritable) ||
                !budget(mediaBytes, *out[size_t(i)], error)) return false;
        }
        if (allowWritable) {
            auto& writeState = out.writes[size_t(i)];
            if (!r.blob(writeState, (1u << 20) + 64) ||
                (!drive && !writeState.empty()) ||
                (drive && !drive->loadSessionWriteState(writeState.data(), writeState.size(), false))) {
                error = "invalid session Sony write state"; return false;
            }
            mediaBytes += writeState.size();
            if (mediaBytes > (128u << 20)) { error = "session media expansion exceeds 128 MiB"; return false; }
        }
    }
    if (!r.done()) { error = "trailing session motherboard media"; return false; }
    return true;
}
void applySessionSonyMedia(Memory& memory, SessionSonyMedia& prepared) {
    for (int i = 0; i < 3; ++i) {
        if (prepared[size_t(i)]) {
            *sony(memory, i)->image() = std::move(*prepared[size_t(i)]);
            // Core MEX restored the mechanism; replace only its media/cache.
            sony(memory, i)->invalidateSessionCache();
        }
        const auto& writeState = prepared.writes[size_t(i)];
        if (!writeState.empty()) sony(memory, i)->loadSessionWriteState(writeState.data(), writeState.size());
    }
}
}
