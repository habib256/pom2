// POM2 — GPL-3.0-or-later
#include "SessionCards.h"
#include "EmulationController.h"
#include "TranswarpCard.h"
#include "PhasorCard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "SamCard.h"
#include "FourPlayCard.h"
#include "Via6522.h"
#include <algorithm>
namespace pom2 {
namespace {
using Card = SessionArchive::Card;
uint32_t word(const std::vector<uint8_t>& b, size_t offset) {
    uint32_t value = 0; for (int i = 0; i < 4; ++i) value |= uint32_t(b[offset + i]) << (8 * i); return value;
}
}
bool describeAdditionalSessionCard(SlotPeripheral* card, SessionArchive::Slot& r)
{
    if (auto* tw = dynamic_cast<TranswarpCard*>(card)) {
        r.kind = Card::Transwarp; r.identity = tw->sessionRom();
    } else if (dynamic_cast<PhasorCard*>(card)) r.kind = Card::Phasor;
    else if (dynamic_cast<EchoPlusCard*>(card)) r.kind = Card::EchoPlus;
    else if (dynamic_cast<SamCard*>(card)) r.kind = Card::Sam;
    else if (dynamic_cast<FourPlayCard*>(card)) r.kind = Card::FourPlay;
    else if (dynamic_cast<EchoPlusTMS5220Card*>(card)) r.kind = Card::EchoTms;
    else return false;
    return true;
}
std::unique_ptr<SlotPeripheral> makeAdditionalSessionCard(const SessionArchive::Slot& r, int slot, std::string& error)
{
    switch (r.kind) {
    case Card::Transwarp: {
        auto card = std::make_unique<TranswarpCard>(slot);
        if (r.identity.empty() || card->setRom(r.identity)) return card;
        break;
    }
    case Card::Phasor: if (r.identity.empty()) return std::make_unique<PhasorCard>(slot); break;
    case Card::EchoPlus: if (r.identity.empty()) return std::make_unique<EchoPlusCard>(slot); break;
    case Card::Sam: if (r.identity.empty()) return std::make_unique<SamCard>(slot); break;
    case Card::FourPlay: if (r.identity.empty()) return std::make_unique<FourPlayCard>(slot); break;
    case Card::EchoTms: if (r.identity.empty()) return std::make_unique<EchoPlusTMS5220Card>(slot); break;
    default: break;
    }
    error = "invalid session expansion card identity"; return {};
}
std::unique_ptr<SlotPeripheral> prepareAdditionalSessionCard(const SessionArchive::Slot& r, int slot,
                                                            SlotPeripheral*, std::string& error)
{
    auto card = makeAdditionalSessionCard(r, slot, error);
    if (!card) return {};
    // VIA counters must remain in their 16-bit timer range; the rewind
    // loaders otherwise accept arbitrary signed counters from the blob.
    if (r.kind == Card::Phasor) {
        const size_t expected = 6 + 2 * Via6522::kSnapshotBytes + 4 * Ay3_8910::kSnapshotBytes + 2 * Ssi263::kSnapshotBytes;
        if (r.state.size() != expected || r.state[3] != 3 || r.state[5] != 255) {
            error = "invalid session Phasor layout"; return {};
        }
        for (int i = 0; i < 2; ++i) {
            const size_t p = 6 + i * Via6522::kSnapshotBytes;
            if (word(r.state, p + 9) > 65537 || word(r.state, p + 17) > 65537) {
                error = "invalid session Phasor timer"; return {};
            }
        }
    }
    if (auto* tms = dynamic_cast<EchoPlusTMS5220Card*>(card.get())) {
        if (r.state.size() < 4) { error = "invalid session Echo+ layout"; return {}; }
        const auto rate = word(r.state, r.state.size() - 4);
        if (rate < 8000 || rate > 192000) { error = "invalid session Echo+ sample rate"; return {}; }
        tms->setSampleRate(rate);
    }
    card->loadSnapshotState(r.state.data(), r.state.size());
    std::vector<uint8_t> canonical; card->appendSnapshotState(canonical);
    if (canonical != r.state) { error = "invalid session expansion card state"; return {}; }
    return card;
}
void wireAdditionalSessionCard(EmulationController& c, SlotPeripheral* card)
{
    const auto rate = c.audio().getActualSampleRate();
    if (auto* tw = dynamic_cast<TranswarpCard*>(card)) tw->setMemory(&c.memory());
    if (auto* p = dynamic_cast<PhasorCard*>(card)) { p->setCpu(&c.cpu()); p->setSampleRate(rate); }
    if (auto* p = dynamic_cast<EchoPlusCard*>(card)) { p->setCpu(&c.cpu()); p->setSampleRate(rate); }
    if (auto* p = dynamic_cast<SamCard*>(card)) { p->setCpu(&c.cpu()); p->setSampleRate(rate); }
    if (auto* p = dynamic_cast<EchoPlusTMS5220Card*>(card)) { p->setCpu(&c.cpu()); p->setSampleRate(rate); }
}
}
