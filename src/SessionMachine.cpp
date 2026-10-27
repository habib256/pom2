// POM2 — GPL-3.0-or-later
#include "SessionMachine.h"
#include "EmulationController.h"
#include "SessionSnapshot.h"
#include "SessionCards.h"
#include "SnapshotIO.h"
#include "MachineSnapshot.h"
#include "DiskIICard.h"
#include "ClockCard.h"
#include "CffaCard.h"
#include "IIcMouse.h"
#include "LeChatMauveCard.h"
#include "Mockingboard.h"
#include "ProDOSHardDiskCard.h"
#include "SlinkyCard.h"
#include "SnesMaxCard.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SuperSerialCard.h"
#include <algorithm>
#include <array>
#include <utility>

void EmulationController::setZipChipInstalled(const pom2::StateAccess&, bool on)
{
    zipInstalled_.store(on); zipChip_.onReset(0);
    installSocketAccelerator();
}

bool ClockCard::installSessionIdentity(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty() || bytes[0] > 1 || bytes.size() != 257u + (bytes[0] ? 2048u : 0u)) return false;
    std::copy_n(bytes.data() + 1, rom.size(), rom.begin());
    expansionRomLoaded_ = bytes[0] != 0;
    if (expansionRomLoaded_) std::copy_n(bytes.data() + 257, expansionRom_.size(), expansionRom_.begin());
    romFromDump_ = expansionRomLoaded_; romSource_.clear();
    return true;
}
namespace pom2 {
namespace {
using Card = SessionArchive::Card;
using Blob = std::vector<uint8_t>;
bool coreConfiguration(const SessionArchive& s, Blob& environment, M6502::CpuMode& mode,
                       SystemProfile& profile, std::string& error)
{
    bool known = false, haveCpu = false;
    for (auto p : allProfiles()) if (snapshotMachineId(p) == s.machineId) { profile = p; known = true; break; }
    if (!known) { error = "unknown session machine profile"; return false; }
    SnapshotReader r(s.core.data(), s.core.size());
    if (!r.good() || r.machineId() != s.machineId) { error = "invalid session core identity"; return false; }
    std::string name; uint32_t size;
    while (r.nextSection(name, size)) {
        if (name == "ENV") {
            if (!environment.empty() || size < 8 || size > 65536) { error = "invalid session environment"; return false; }
            environment.resize(size); r.readBytes(environment.data(), size);
        } else if (name == "CPU") {
            if (haveCpu || size < 16 || size > 17) { error = "invalid session CPU configuration"; return false; }
            for (int i = 0; i < 7; ++i) r.readU8();
            const auto cpu = r.readU8();
            if (cpu > 1) { error = "invalid session CPU mode"; return false; }
            mode = cpu ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS; haveCpu = true;
        }
        r.skipCurrentSection();
    }
    if (!r.good() || environment.empty() || !haveCpu) { error = "session lacks machine configuration"; return false; }
    const auto& cfg = profileConfig(profile);
    if (bool(environment[1] & 1) != cfg.iieMode ||
        bool(environment[1] & 2) != cfg.noPhysicalSlots ||
        bool(environment[2]) != (cfg.videoStandard == VideoStandard::PAL)) {
        error = "session environment disagrees with its machine profile"; return false;
    }
    // The profile switch never puts an NMOS core under a soldered 65C02
    // (resolveCpuModeSetting), so no capture carries one: a file that does
    // would boot the //c / enhanced //e ROM into KIL opcodes and freeze.
    if (cfg.defaultCpu == M6502::CpuMode::CMOS && mode == M6502::CpuMode::NMOS) {
        error = "session CPU mode is NMOS on a 65C02-only machine"; return false;
    }
    return true;
}
std::unique_ptr<SlotPeripheral> makeCard(const SessionArchive::Slot& r, int slot, SystemProfile profile, std::string& error)
{
    const auto& id = r.identity;
    switch (r.kind) {
    case Card::Empty: return {};
    case Card::DiskII: {
        auto card = std::make_unique<DiskIICard>(slot);
        if (card->installSessionIdentity(id)) return card;
        break;
    }
    case Card::Mockingboard: case Card::SoundII:
        return std::make_unique<MockingboardCard>(slot, r.kind == Card::SoundII ?
            MockingboardCard::Variant::SoundII : MockingboardCard::Variant::AC);
    case Card::SnesMax: return std::make_unique<SnesMaxCard>(slot);
    case Card::Slinky: {
        auto card = std::make_unique<SlinkyCard>(slot);
        if (card->loadRom(id)) return card;
        break;
    }
    case Card::Clock: {
        auto card = std::make_unique<ClockCard>(slot);
        if (card->installSessionIdentity(id)) return card;
        break;
    }
    case Card::Cffa: {
        auto card = std::make_unique<CffaCard>(slot);
        if (card->installSessionIdentity(id)) return card;
        break;
    }
    case Card::HardDisk: return std::make_unique<ProDOSHardDiskCard>(slot);
    case Card::IIcMouse: return std::make_unique<IIcMouse>(profile == SystemProfile::AppleIIcPlus ? 7 : 4);
    case Card::ChatMauve:
        if (id.size() == 1 && id[0] < LeChatMauveCard::kVariantCount)
            return std::make_unique<LeChatMauveCard>(slot, LeChatMauveCard::Variant(id[0]));
        break;
    case Card::Serial: {
        if (id.size() != 8 || id[0] != 1 || id[1] > uint8_t(SuperSerialCard::Cable::NullModem) || id[4] > 1 || id[5] > 1 || id[6] > 1 || id[7] > 1) break;
        auto card = std::make_unique<SuperSerialCard>(slot);
        card->setBuiltInPort(true); card->setCable(SuperSerialCard::Cable(id[1]));
        card->setDipSwitches(id[2], id[3]); card->setPrinterTap(id[4]);
        card->setModemLinesTied(id[5]); card->setIrqDipEnabled(id[6]); card->setRawMode(id[7]);
        return card;
    }
    case Card::SmartPort: {
        if (id.size() != 3 + SmartPortCard::kMaxUnits || id[0] != 1 || id[2] > 1) break;
        auto card = std::make_unique<SmartPortCard>(slot);
        card->setUnitCount(id[1]); card->setSuperDriveMode(id[2]);
        if (card->unitCount() != id[1]) break;
        for (size_t i = 0; i < SmartPortCard::kMaxUnits; ++i) {
            if (id[3 + i] > 2) { error = "invalid session SmartPort unit type"; return {}; }
            if (id[3 + i] == 2) card->setUnit(i, std::make_unique<SmartPortHdvUnit>());
            else if (id[3 + i] == 1) card->setUnit(i, std::make_unique<SmartPort35Unit>());
            else card->setUnit(i, nullptr);
        }
        return card;
    }
    default:
        if (uint8_t(r.kind) >= uint8_t(Card::Transwarp)) return makeAdditionalSessionCard(r, slot, error);
        break;
    }
    error = "invalid session card identity in slot " + std::to_string(slot); return {};
}
void wire(EmulationController& c, SlotPeripheral* card, int slot)
{
    if (auto* d = dynamic_cast<DiskIICard*>(card)) {
        d->setCpu(&c.cpu()); d->setFloppySound(&c.floppySound525());
        if (slot == 6) d->setIWM(&c.iwm());
    }
    if (auto* mb = dynamic_cast<MockingboardCard*>(card)) {
        mb->setCpu(&c.cpu()); mb->setSampleRate(c.audio().getActualSampleRate());
    }
    if (auto* rgb = dynamic_cast<LeChatMauveCard*>(card)) rgb->setMemory(&c.memory());
    if (auto* port = dynamic_cast<SmartPortCard*>(card)) port->setFloppySound(&c.floppySound35());
    wireAdditionalSessionCard(c, card);
}

bool outgoingSafe(EmulationController& c, std::string& error)
{
    if (!c.cassette().sessionReplaceable()) {
        error = "eject/clear the outgoing cassette before replacing the machine"; return false;
    }
    for (int i = 0; i < 3; ++i)
        if (c.sony35(i).isEjectPending() || !c.disk35(i).sessionReplaceable()) {
            error = "outgoing 3.5-inch medium has unsaved changes or pending writes"; return false;
        }
    for (int i = 1; i <= 7; ++i) if (auto* card = c.memory().slotBus().peripheral(i)) {
        // A live network worker cannot be joined while holding stateMutex.
        if (auto* serial = dynamic_cast<SuperSerialCard*>(card)) {
            if (!serial->sessionIdle()) { error = "stop the outgoing serial transport first"; return false; }
        }
        if (!card->sessionReplacementReady()) {
            error = "outgoing card is busy or requires removal before session replacement: " + std::string(card->name()); return false;
        }
        for (auto* medium : card->autosavedMedia()) if (medium) {
            const auto p = medium->persistence();
            if (p.state == "saving" || (p.pending && p.state != "session")) {
                error = "outgoing medium has unsaved changes or pending writes"; return false;
            }
        }
        for (auto* b : card->blockBackings()) if (b && !b->sessionReplaceable()) {
            error = "outgoing block medium has unsaved changes or pending writes"; return false;
        }
        if (auto* disk = dynamic_cast<DiskIICard*>(card))
            for (int d = 0; d < 2; ++d) if (disk->driveImage(d).hasPendingWriteBack()) {
                error = "outgoing floppy has unsaved changes or pending writes"; return false;
            }
        if (auto* port = dynamic_cast<SmartPortCard*>(card))
            for (size_t d = 0; d < SmartPortCard::kMaxUnits; ++d)
                if (auto* u = dynamic_cast<SmartPort35Unit*>(port->unit(d)))
                    if (!u->image().sessionReplaceable()) {
                        error = "outgoing SmartPort floppy has unsaved changes"; return false;
                    }
    }
    return true;
}
}

bool reconstructSession(EmulationController& c, const SessionArchive& session,
                        std::string& error, const SessionMachineHooks& hooks)
{
    if (session.version < 7 && !session.cassette.empty()) {
        error = "cassette sessions require format version 7"; return false;
    }
    if (!c.cassette().loadSessionState(session.cassette, error, false)) return false;
    {
        auto state = c.lockState();
        std::string reason;
        if (c.cassette().sessionReplaceable() &&
            restoreSession(session, state.cpu(), state.memory(), c.machineId(), reason)) {
            c.noteTimeJump(); c.rewind().clear();
            return c.cassette().loadSessionState(session.cassette, error);
        }
    }
    Blob env;
    M6502::CpuMode mode;
    SystemProfile profile;
    if (!coreConfiguration(session, env, mode, profile, error)) return false;
    const auto& config = session.configuration;
    if (((config.size() != 4 || config[0] != 1) &&
         (session.version < 7 || config.size() != 6 || config[0] != 2 || config[4] != 1 || config[5] > 1)) || config[1] > 1 || config[2] > 1 ||
        (config[1] && config[3] != 255 && (config[3] < 1 || config[3] > 7)) || (!config[1] && (config[2] || config[3]))) {
        error = "invalid session clock configuration"; return false;
    }
    auto candidate = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    auto configure = [&](EmulationController& target, const StateAccess& state) {
        if (!target.memory().installSessionEnvironment(env, error)) return false;
        target.cpu().setCpuMode(mode);
        if (target.memory().isIIcPlus()) {
            // The //c Plus's accelerator is part of the machine, not the
            // user's //e Zip Chip option — restoring one must not switch that
            // option on (it would follow the user back to a //e profile).
            target.installSocketAccelerator();
        } else {
            target.setZipChipInstalled(state, config.size() == 6);
            if (config.size() == 6) target.zipChip().setBootFast(config[5]);
        }
        target.memory().setNoSlotClock(config[1] ? &target.noSlotClock() : nullptr);
        target.noSlotClock().setEnabled(config[2]); target.noSlotClock().setSlot(config[3]);
        target.setCyclesPerFrame(profileConfig(profile).defaultCyclesPerFrame);
        target.setVideoStandard(profileConfig(profile).videoStandard);
        target.setMachineId(session.machineId);
        if (target.memory().isIIcClass()) {
            if (session.slots[0].identity.size() != 3 || session.slots[0].identity[0] != 1) {
                error = "invalid session Sony drive topology"; return false;
            }
            if (session.slots[0].identity[2] && !session.slots[0].identity[1]) {
                error = "invalid session Sony drive chain"; return false;
            }
            for (int i = 1; i < 3; ++i) {
                if (session.slots[0].identity[i] > 1) { error = "invalid session external drive topology"; return false; }
                target.connectExternalSony35(i, session.slots[0].identity[i] != 0);
            }
        }
        return true;
    };
    { auto state = candidate->lockState(); if (!configure(*candidate, state)) return false; }
    for (int i = 1; i <= 7; ++i) {
        auto card = makeCard(session.slots[i], i, profile, error);
        if (!card && session.slots[i].kind != Card::Empty) return false;
        wire(*candidate, card.get(), i);
        candidate->memory().slotBus().plug(i, std::move(card));
    }
    candidate->setVideoStandard(profileConfig(profile).videoStandard);
    if (!restoreSession(session, candidate->cpu(), candidate->memory(), session.machineId, error)) return false;
    // The entire incoming machine is now proven, without touching the running
    // controller. Serialize the final eligibility check and replacement.
    auto state = c.lockState();
    if (!outgoingSafe(c, error)) return false;
    if (hooks.detach) hooks.detach(state);
    c.iwm().setFloppy(nullptr, 0); c.iwm().reset();
    c.externalSmartPort().reset();
    c.floppySound525().reset(); c.floppySound35().reset();
    c.memory().slotBus().clear();
    for (int i = 0; i < 3; ++i) { c.disk35(i).eject(); c.sony35(i).reset(); }
    if (!configure(c, state)) return false; // identical, previously validated bytes
    for (int i = 1; i <= 7; ++i) {
        auto card = candidate->memory().slotBus().unplug(i);
        wire(c, card.get(), i);
        c.memory().slotBus().plug(i, std::move(card));
    }
    c.setVideoStandard(profileConfig(profile).videoStandard);
    if (!restoreSession(session, state.cpu(), state.memory(), session.machineId, error)) return false;
    c.noteTimeJump(); c.rewind().clear();
    if (!c.cassette().loadSessionState(session.cassette, error)) return false;
    if (hooks.publish) hooks.publish(state, profile);
    return true;
}
}
