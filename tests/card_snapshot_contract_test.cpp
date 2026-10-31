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

//
// card_snapshot_contract_test — TODO G5-2. `card_snapshot_state_test.cpp`
// asserts the RIGHT thing for six named cards; this one asserts the CONTRACT
// for every card in the catalog, so a card added tomorrow is covered for free.
// Four properties, per card:
//
//   P1  save -> load into a FRESH card -> save again reproduces the blob
//       (the only way to see the PRIVATE fields the bus cannot read back),
//       and re-loading a card's own blob into itself is a no-op.
//   P2  a foreign blob (another card's, or junk, or null) leaves the card
//       exactly as it was — the property MachineSnapshot.cpp:199-201 relies on.
//   P3  every truncation of a valid blob, and a valid blob with junk appended,
//       parses to either "unchanged" or "the captured state" — never a hybrid.
//   P4  the restored card and the original answer the next 600 bus accesses
//       identically, register byte for register byte and IRQ line included.
//
// Caught on first run: PhasorCard rejecting its own blob whenever the mode
// register held an un-named value, and LironCard restoring an IWM whose clock
// then jumped the whole rewind depth forward.
//
// Two additions (2026-09-16). The bare constructors below leave the ROM-gated
// cards without firmware, so a Liron never enabled a drive and the 68705 on
// the mouse never ran — which is how two lossy restores went unseen. Those
// cards are ALSO built through SlotCardFactory, with the shipped dumps; that
// half found the Liron flushing its restored write window into the drive and
// the 68705 loader halving the mouse's timer. And every key of the slot
// catalog must map to an entry here, so a new card cannot be left out.
#include "CffaCard.h"
#include "Cffa3000Card.h"
#include "ClockCard.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "FourPlayCard.h"
#include "SnesMaxCard.h"
#include "AppleParallelCard.h"
#include "GrapplerCard.h"
#include "GrapplerClassicCard.h"
#include "LeChatMauveCard.h"
#include "LironCard.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPortCard.h"
#include "SoftCardZ80.h"
#include "AlsCpmCard.h"
#include "SoftCardIICard.h"
#include "ApplicardCard.h"
#include "PremiumSoftCard.h"
#include "SaturnCard.h"
#include "Ccs7710Card.h"
#include "Dan2Card.h"
#include "EzColorCard.h"
#include "VidexUltratermCard.h"
#include "SuperSerialCard.h"
#include "TranswarpCard.h"
#include "SlinkyCard.h"
#include "SamCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "VidexVideotermCard.h"
#include "VidHDCard.h"
#include "WorkstationCard.h"
#include "SlotCardCatalog.h"
#include "SlotCardFactory.h"
#include "SlotPeripheral.h"

#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace pom2;

namespace {

using Blob = std::vector<uint8_t>;
using Maker = std::function<std::unique_ptr<SlotPeripheral>()>;

struct Entry { const char* name; Maker make; };

uint32_t rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// Drive the card through every window a guest can touch, deterministically.
void drive(SlotPeripheral& c, uint32_t seed)
{
    uint32_t s = seed;
    for (int i = 0; i < 400; ++i) {
        const uint32_t r = rnd(s);
        switch (r % 6) {
        case 0: c.deviceSelectWrite(uint8_t(r >> 8) & 0x0F, uint8_t(r >> 16)); break;
        case 1: (void)c.deviceSelectRead(uint8_t(r >> 8) & 0x0F); break;
        case 2: c.slotRomWrite(uint8_t(r >> 8), uint8_t(r >> 16)); break;
        case 3: (void)c.slotRomRead(uint8_t(r >> 8)); break;
        case 4: c.advanceCycles(int((r >> 8) % 97) + 1); break;
        case 5: c.expansionRomWrite(uint16_t((r >> 8) & 0x7FE), uint8_t(r >> 20)); break;
        }
    }
}

// A second history, $C0nX-heavy with long waits: the one that walks a
// Liron with firmware into WRITE mode with a drive enabled, where the plain
// `drive()` above never gets. It is what exposed the restore that flushed
// the IWM's write window (P1c).
void driveLong(SlotPeripheral& c, uint32_t seed)
{
    for (int i = 0; i < 400; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t v = seed >> 8;
        const uint8_t low4 = static_cast<uint8_t>(v & 0x0F);
        switch ((v >> 4) & 3) {
        case 0: c.deviceSelectWrite(low4, static_cast<uint8_t>(v >> 8)); break;
        case 1: (void)c.deviceSelectRead(low4); break;
        case 2: c.advanceCycles(static_cast<int>((v >> 8) & 0x3FF)); break;
        default: (void)c.slotRomRead(static_cast<uint8_t>(v >> 8)); break;
        }
    }
}

Blob cap(const SlotPeripheral& c) { Blob b; c.appendSnapshotState(b); return b; }

void hexdiff(const Blob& a, const Blob& b)
{
    std::printf("      lenA=%zu lenB=%zu\n", a.size(), b.size());
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int shown = 0;
    for (size_t i = 0; i < n && shown < 12; ++i)
        if (a[i] != b[i]) { std::printf("      [%zu] %02X != %02X\n", i, a[i], b[i]); ++shown; }
}

int failures = 0;

void check(const Entry& e)
{
    // ---- P1: round-trip fidelity ------------------------------------
    auto a = e.make();
    drive(*a, 0x1234567u);
    Blob ba = cap(*a);
    if (ba.empty()) { std::printf("  %-22s NO SNAPSHOT STATE (empty blob)\n", e.name); }
    else {
        auto b = e.make();
        b->loadSnapshotState(ba.data(), ba.size());
        Blob bb = cap(*b);
        if (bb != ba) {
            std::printf("  %-22s P1 FAIL: restore is lossy\n", e.name);
            hexdiff(ba, bb);
            ++failures;
        }
        // ---- P1b: self-reload must be a no-op -----------------------
        a->loadSnapshotState(ba.data(), ba.size());
        Blob ba2 = cap(*a);
        if (ba2 != ba) {
            std::printf("  %-22s P1b FAIL: self-reload changed state\n", e.name);
            hexdiff(ba, ba2);
            ++failures;
        }
    }

    // ---- P1c: round-trip fidelity after the long history -------------
    // Eight seeds: whether a given history ends mid-write is luck, and one
    // seed (0xC0FFEE ^ 5) is known to.
    for (uint32_t k = 0; k < 8; ++k) {
        auto a2 = e.make();
        driveLong(*a2, 0xC0FFEEu ^ k);
        const Blob b1 = cap(*a2);
        if (!b1.empty()) {
            auto b2 = e.make();
            b2->loadSnapshotState(b1.data(), b1.size());
            const Blob b2b = cap(*b2);
            if (b2b != b1) {
                std::printf("  %-22s P1c FAIL: restore is lossy after long history #%u\n",
                            e.name, static_cast<unsigned>(k));
                hexdiff(b1, b2b);
                ++failures;
                break;
            }
        }
    }

    // ---- P2: foreign blob must be ignored ---------------------------
    static const Blob kForeign(64, 0xAB);
    {
        auto c = e.make();
        drive(*c, 0x99u);
        Blob before = cap(*c);
        c->loadSnapshotState(kForeign.data(), kForeign.size());
        Blob after = cap(*c);
        if (after != before) {
            std::printf("  %-22s P2 FAIL: foreign blob mutated the card\n", e.name);
            hexdiff(before, after); ++failures;
        }
        // Every other card's blob is foreign too.
        c->loadSnapshotState(nullptr, 0);
        Blob after2 = cap(*c);
        if (after2 != before) {
            std::printf("  %-22s P2b FAIL: null blob mutated the card\n", e.name); ++failures;
        }
    }

    // ---- P3: truncation of our OWN blob must not misparse ------------
    if (!ba.empty()) {
        auto c = e.make();
        drive(*c, 0x99u);
        Blob before = cap(*c);
        // A complete legacy prefix may apply captured/default state; other
        // prefixes must leave live state alone. A hybrid is never legal.
        // Liron's versioned captures additionally require strict unchanged
        // state for every truncation (testLironAtomicRejection below).
        auto rc = e.make(); drive(*rc, 0x99u); rc->onReset();
        const Blob fresh = cap(*rc);
        // Every length through the header and the tail, a prime stride
        // through the bulk in between: exhaustive for every small blob, and
        // linear rather than quadratic for the 64-128 KB RAM images (Z80
        // cards, Saturn), which timed this test out under coverage.
        auto nextLen = [&](size_t n) {
            const size_t edge = 512;
            if (n + 1 < edge || n + 1 + edge >= ba.size()) return n + 1;
            return std::min(n + 4093, ba.size() - edge);
        };
        for (size_t n = 0; n < ba.size(); n = nextLen(n)) {
            auto d = e.make();
            drive(*d, 0x99u);
            d->loadSnapshotState(ba.data(), n);      // truncated
            Blob after = cap(*d);
            if (after != before && after != ba && after != fresh) {
                std::printf("  %-22s P3 FAIL: truncation to %zu/%zu is a hybrid state\n",
                            e.name, n, ba.size());
                ++failures;
                break;
            }
        }
        // Oversized: valid blob with junk appended.
        auto d = e.make();
        Blob over = ba; over.insert(over.end(), 512, 0x5A);
        d->loadSnapshotState(over.data(), over.size());
        Blob after = cap(*d);
        if (after != ba) {
            std::printf("  %-22s P3b FAIL: trailing junk changed the parse\n", e.name);
            hexdiff(ba, after); ++failures;
        }
    }

// ---- P4: behavioural equivalence over the next N cycles ---------
    if (!ba.empty()) {
        auto ref = e.make();
        drive(*ref, 0x1234567u);           // same history as `a`
        Blob br = cap(*ref);
        auto res = e.make();
        res->loadSnapshotState(br.data(), br.size());
        uint32_t s = 0xBEEF01u;
        for (int i = 0; i < 600; ++i) {
            const uint32_t r = rnd(s);
            const uint8_t d4 = uint8_t(r >> 8) & 0x0F;
            const uint8_t d8 = uint8_t(r >> 8);
            uint8_t x1 = 0, x2 = 0;
            switch (r % 5) {
            case 0: x1 = ref->deviceSelectRead(d4); x2 = res->deviceSelectRead(d4); break;
            case 1: x1 = ref->slotRomRead(d8);      x2 = res->slotRomRead(d8);      break;
            case 2: ref->deviceSelectWrite(d4, uint8_t(r >> 16));
                    res->deviceSelectWrite(d4, uint8_t(r >> 16)); break;
            case 3: ref->slotRomWrite(d8, uint8_t(r >> 16));
                    res->slotRomWrite(d8, uint8_t(r >> 16)); break;
            case 4: ref->advanceCycles(int((r >> 8) % 131) + 1);
                    res->advanceCycles(int((r >> 8) % 131) + 1); break;
            }
            if (x1 != x2) {
                std::printf("  %-22s P4 FAIL: read diverges at step %d (%02X vs %02X)\n",
                            e.name, i, x1, x2); ++failures; break;
            }
            if (ref->slotIrqAsserted() != res->slotIrqAsserted()) {
                std::printf("  %-22s P4 FAIL: IRQ line diverges at step %d (%d vs %d)\n",
                            e.name, i, (int)ref->slotIrqAsserted(), (int)res->slotIrqAsserted());
                ++failures; break;
            }
        }
    }
}

}  // namespace

void testLironAtomicRejection()
{
    pom2::LironCard saved(5), live(5);
    drive(saved, 0x1234567u);
    drive(live, 0x99u);
    const Blob blob = cap(saved), before = cap(live);
    assert(blob != before);
    for (std::size_t n = 0; n < blob.size(); ++n) {
        live.loadSnapshotState(blob.data(), n);
        if (cap(live) != before) {
            std::fprintf(stderr, "Liron truncated snapshot changed live state at %zu/%zu\n", n, blob.size());
            assert(false);
        }
    }
    assert(blob[3] == '2');
    const auto get32 = [&](std::size_t at) {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(blob[at + k]) << (8 * k);
        return v;
    };
    const std::size_t iwmBytes = get32(4), busAt = 8 + iwmBytes;
    pom2::SmartPortBusDevice probe;
    const auto mandatory = busAt + probe.loadSnapshotState(blob.data() + busAt, blob.size() - busAt) + 2;
    std::size_t next = mandatory;
    std::vector<std::size_t> corruptAt{4, 8, busAt};
    for (int drive = 0; drive < pom2::LironCard::kDrives; ++drive) {
        corruptAt.push_back(next);     // mechanism length
        corruptAt.push_back(next + 4); // mechanism magic
        next += 4 + get32(next);
    }
    for (auto at : corruptAt) {
        Blob malformed = blob;
        malformed[at] = 0xFF;
        live.loadSnapshotState(malformed.data(), malformed.size());
        assert(cap(live) == before && "malformed nested Liron section mutated live state");
    }
    // Known LIR1 layouts: no mechanism tail, mechanisms only, then clock,
    // then the full media mask. New captures always require all sections.
    for (auto bytes : {mandatory, next, next + 8, next + 12}) {
        Blob legacy(blob.begin(), blob.begin() + bytes);
        legacy[3] = '1';
        pom2::LironCard restored(5);
        restored.loadSnapshotState(legacy.data(), legacy.size());
        const auto migrated = cap(restored);
        assert(std::equal(blob.begin() + 4, blob.begin() + mandatory, migrated.begin() + 4));
        if (bytes == blob.size()) assert(migrated == blob);
    }
}

int main()
{
    testLironAtomicRejection();
    std::vector<Entry> cards = {
        { "DiskIICard",        [] { return std::unique_ptr<SlotPeripheral>(new DiskIICard(6)); } },
        { "ProDOSHardDisk",    [] { return std::unique_ptr<SlotPeripheral>(new ProDOSHardDiskCard(7)); } },
        { "CffaCard",          [] { return std::unique_ptr<SlotPeripheral>(new CffaCard(7)); } },
        { "Cffa3000Card",      [] { return std::unique_ptr<SlotPeripheral>(new Cffa3000Card(7)); } },
        { "SmartPortCard",     [] { return std::unique_ptr<SlotPeripheral>(new SmartPortCard(5)); } },
        { "SuperDrive",        [] {
            auto card = std::make_unique<SmartPortCard>(5);
            card->setSuperDriveMode(true);
            return card;
        } },
        { "LironCard",         [] { return std::unique_ptr<SlotPeripheral>(new LironCard(5)); } },
        { "MockingboardAC",    [] { return std::unique_ptr<SlotPeripheral>(new MockingboardCard(4)); } },
        { "MockingboardSndII", [] { return std::unique_ptr<SlotPeripheral>(new MockingboardCard(4, MockingboardCard::Variant::SoundII)); } },
        { "PhasorCard",        [] { return std::unique_ptr<SlotPeripheral>(new PhasorCard(4)); } },
        { "EchoPlusCard",      [] { return std::unique_ptr<SlotPeripheral>(new EchoPlusCard(3)); } },
        { "EchoPlusTMS",       [] { return std::unique_ptr<SlotPeripheral>(new EchoPlusTMS5220Card(3)); } },
        { "SuperSerialCard",   [] { return std::unique_ptr<SlotPeripheral>(new SuperSerialCard(2)); } },
        { "ClockCard",         [] { return std::unique_ptr<SlotPeripheral>(new ClockCard(4)); } },
        { "MouseCardAppleWin", [] { return std::unique_ptr<SlotPeripheral>(new MouseCardAppleWin(4)); } },
        { "MouseCard",         [] { return std::unique_ptr<SlotPeripheral>(new MouseCard(4)); } },
        { "FourPlayCard",      [] { return std::unique_ptr<SlotPeripheral>(new FourPlayCard(4)); } },
        { "SnesMaxCard",       [] { return std::unique_ptr<SlotPeripheral>(new SnesMaxCard(4)); } },
        { "TranswarpCard",     [] { return std::unique_ptr<SlotPeripheral>(new TranswarpCard(3)); } },
        { "SlinkyCard",        [] { return std::unique_ptr<SlotPeripheral>(new SlinkyCard(4)); } },
        { "SamCard",           [] { return std::unique_ptr<SlotPeripheral>(new SamCard(4)); } },
        { "GrapplerCard",      [] { return std::unique_ptr<SlotPeripheral>(new GrapplerCard(1)); } },
        { "GrapplerClassic",   [] { return std::unique_ptr<SlotPeripheral>(new GrapplerClassicCard(1)); } },
        { "AppleParallel",     [] { return std::unique_ptr<SlotPeripheral>(new AppleParallelCard(1)); } },
        { "VidexVideoterm",    [] { return std::unique_ptr<SlotPeripheral>(new VidexVideotermCard(3)); } },
        { "VidHD",             [] { return std::unique_ptr<SlotPeripheral>(new VidHDCard(3)); } },
        { "PrinterCard",       [] { return std::unique_ptr<SlotPeripheral>(new PrinterCard(1)); } },
        { "UthernetCard",      [] { return std::unique_ptr<SlotPeripheral>(new UthernetCard(3)); } },
        { "UthernetIICard",    [] { return std::unique_ptr<SlotPeripheral>(new UthernetIICard(3)); } },
        { "LeChatMauveFeline", [] { return std::unique_ptr<SlotPeripheral>(new LeChatMauveCard(7)); } },
        { "LeChatMauveEve",    [] { return std::unique_ptr<SlotPeripheral>(new LeChatMauveCard(7, LeChatMauveCard::Variant::Eve)); } },
        { "SoftCardZ80",       [] { return std::unique_ptr<SlotPeripheral>(new SoftCardZ80()); } },
        { "AlsCpmCard",        [] { return std::unique_ptr<SlotPeripheral>(new AlsCpmCard()); } },
        { "SoftCardIICard",    [] { return std::unique_ptr<SlotPeripheral>(new SoftCardIICard()); } },
        { "ApplicardCard",     [] { return std::unique_ptr<SlotPeripheral>(new ApplicardCard()); } },
        { "SaturnCard",        [] { return std::unique_ptr<SlotPeripheral>(new SaturnCard()); } },
        { "Ccs7710Card",       [] { return std::unique_ptr<SlotPeripheral>(new Ccs7710Card(1)); } },
        { "Dan2Card",          [] { return std::unique_ptr<SlotPeripheral>(new Dan2Card(7)); } },
        { "EzColorCard",       [] { return std::unique_ptr<SlotPeripheral>(new EzColorCard()); } },
        { "VidexUltraterm",    [] { return std::unique_ptr<SlotPeripheral>(new VidexUltratermCard(3)); } },
        { "PremiumSoftCard",   [] { return std::unique_ptr<SlotPeripheral>(new PremiumSoftCard()); } },
        { "WorkstationCard",   [] { return std::unique_ptr<SlotPeripheral>(new WorkstationCard(7)); } },
    };
    // The ROM-gated cards once more, as the machine builds them: through the
    // factory, with firmware. Skipped (and said so) where a dump is absent.
    const SlotCardFactory factory;
    const auto fromFactory = [&](const char* key, int slot) -> Maker {
        return [&factory, key, slot]() -> std::unique_ptr<SlotPeripheral> {
            SlotCardFactory::Request req;
            req.key = key;
            req.slot = slot;
            req.cpuIsCmos = true;
            req.profile = SystemProfile::AppleIIe;
            auto made = factory.create(req);
            if (!made.card || made.actualKey != key) return nullptr;
            return std::move(made.card);
        };
    };
    const std::vector<std::pair<const char*, Entry>> withFirmware = {
        { "liron",       { "Liron+ROM",       fromFactory("liron", 5) } },
        { "mouse",       { "MouseCard+ROM",   fromFactory("mouse", 4) } },
        { "cffa",        { "CffaCard+ROM",    fromFactory("cffa", 7) } },
        { "grappler",    { "Grappler+ROM",    fromFactory("grappler", 1) } },
        { "grappler1",   { "Grappler1+ROM",   fromFactory("grappler1", 1) } },
        { "pic",         { "PIC+ROM",         fromFactory("pic", 1) } },
        { "videoterm",   { "Videoterm+ROM",   fromFactory("videoterm", 3) } },
        { "workstation", { "Workstation+ROM", fromFactory("workstation", 7) } },
        { "smartport35", { "SmartPort35+ROM", fromFactory("smartport35", 5) } },
    };
    for (const auto& [key, e] : withFirmware) {
        if (!e.make()) { std::printf("[%s] skipped: no %s firmware here\n", e.name, key); continue; }
        cards.push_back(e);
    }

    // Every catalog key is covered by an entry above.
    const std::map<std::string, std::string> keyToEntry = {
        {"diskii", "DiskIICard"}, {"hdv", "ProDOSHardDisk"}, {"cffa", "CffaCard"},
        {"cffa3", "Cffa3000Card"},
        {"smartport35", "SmartPortCard"}, {"superdrive", "SuperDrive"},
        {"liron", "LironCard"},
        {"ssc", "SuperSerialCard"}, {"printer", "PrinterCard"},
        {"grappler", "GrapplerCard"}, {"clock", "ClockCard"},
        {"uthernet", "UthernetCard"}, {"uthernet2", "UthernetIICard"},
        {"fujinet", "(FujiNetCard: its link is a live socket; snapshot pinned in fujinet_card_smoke)"},
        {"softcard", "SoftCardZ80"}, {"alscpm", "AlsCpmCard"}, {"softcard2", "SoftCardIICard"},
        {"applicard", "ApplicardCard"}, {"saturn", "SaturnCard"}, {"ccs7710", "Ccs7710Card"}, {"dan2", "Dan2Card"}, {"ezcolor", "EzColorCard"}, {"ultraterm", "VidexUltraterm"}, {"chatmauve", "LeChatMauveFeline"},
        {"mouse", "MouseCard"}, {"mouseaw", "MouseCardAppleWin"},
        {"mockingboard", "MockingboardAC"}, {"mockingboard_c", "MockingboardSndII"},
        {"phasor", "PhasorCard"}, {"echoplus", "EchoPlusCard"},
        {"echoplus_tms", "EchoPlusTMS"},
        {"workstation", "WorkstationCard"}, {"4play", "FourPlayCard"},
        {"snesmax", "SnesMaxCard"},
        {"transwarp", "TranswarpCard"},
        {"slinky", "SlinkyCard"}, {"sam", "SamCard"},
        {"grappler1", "GrapplerClassic"}, {"pic", "AppleParallel"},
        {"videoterm", "VidexVideoterm"}, {"vidhd", "VidHD"},
    };
    std::set<std::string> names;
    for (const auto& e : cards) names.insert(e.name);
    for (const auto& t : kCardTypes) {
        if (!t.key || !*t.key) continue;
        const auto it = keyToEntry.find(t.key);
        if (it == keyToEntry.end()) {
            std::printf("catalog key '%s' has no entry in this test — add one\n", t.key);
            ++failures;
        } else if (it->second[0] != '(' && !names.count(it->second)) {
            std::printf("catalog key '%s' maps to missing entry '%s'\n", t.key,
                        it->second.c_str());
            ++failures;
        }
    }

    for (auto& e : cards) { std::printf("[%s]\n", e.name); check(e); }
    std::printf("\n%d failures\n", failures);
    // Return normally so stdio flushes the card-specific diagnostics in CI.
    if (failures != 0) return 1;
    std::printf("card_snapshot_contract: OK\n");
    return 0;
}
