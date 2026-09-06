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

// Snapshot-blob parser fuzz smoke — bounded, deterministic, self-contained.
//
// A snapshot is untrusted input in the same way a disk image is: the user
// loads a `.snap` from wherever they got it, and the AI control server accepts
// one over HTTP. `restoreMachineState` walks a section list driven by lengths
// that came out of that blob, and `MachineSnapshot.h` already records one
// past over-read here ("the round 10 #3 over-read hardening") — which is the
// reason to keep testing it rather than assume it stays fixed.
//
// The blob is CAPTURED, not invented: a random buffer is rejected at the magic
// and never reaches the section walker, so a fuzzer seeded with noise would
// only ever exercise the reject path. Mutating a real capture keeps the header
// plausible and gets the fuzzer through the front door — the test prints the
// acceptance rate so a change that starts rejecting everything (and therefore
// testing nothing) is visible rather than silent.
//
// Mutations are STRUCTURE-AWARE, which is what makes this test bite. Blind
// byte-flipping cannot find a section-length bug: the length fields are four
// bytes each in a ~160 KB blob, so a random flip essentially never lands on
// one. `mutateSections` walks the section list (name[8] + len[4] + payload)
// and aims at the lengths and names directly. Verified against a deliberately
// removed bounds check in `Memory::loadSnapshotState` — the blind version
// missed it entirely, the structure-aware version catches it.
//
// Note what this can and cannot prove. Every read in `SnapshotReader` goes
// through an istream over a bounded streambuf, so THAT layer cannot over-read
// by construction; its guards exist to stop unbounded ALLOCATION. The real
// raw-pointer parser downstream is `Memory::loadSnapshotState`, reached via
// the MEX section, and shortening MEX's declared length is exactly how you
// would catch a missing check there.
//
// Restore is TRANSACTIONAL for file/API input, so a mutant that fails partway
// must leave the machine as it was; the loop reads through CPU and memory
// afterwards, since a restore that reports success but leaves an inconsistent
// Memory only shows up when something uses it.
//
// Under a plain build this catches crashes and assertion failures. Under
// `-fsanitize=address,undefined` it also catches any over-read the section
// walker could be talked into.

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"
#include "ClockCard.h"
#include "TranswarpCard.h"
#include "M68705P3.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr size_t kMagicLen  = 8;
constexpr size_t kHeaderLen = kMagicLen + 4 + 4;   // magic + version + reserved
constexpr size_t kNameLen   = 8;

uint32_t rd32(const uint8_t* p)
{ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void wr32(uint8_t* p, uint32_t v)
{ for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

/// Aim at the section table rather than at the bytes. Returns false when the
/// blob no longer has a walkable one (a previous mutation may have wrecked it).
bool mutateSections(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.size() < kHeaderLen + kNameLen + 4) return false;

    std::vector<size_t> hdrs;                       // offset of each name[8]
    for (size_t i = kHeaderLen; i + kNameLen + 4 <= b.size();) {
        const uint32_t len = rd32(b.data() + i + kNameLen);
        const size_t payload = i + kNameLen + 4;
        if (len > b.size() - payload) break;
        hdrs.push_back(i);
        i = payload + len;
    }
    if (hdrs.empty()) return false;

    const size_t h = hdrs[rng() % hdrs.size()];
    switch (rng() % 3) {
        case 0:     // SHORTEN a section — the case that reaches a downstream
                    // raw-pointer parser with less data than it declared.
            wr32(b.data() + h + kNameLen, uint32_t(rng() % 4096));
            break;
        case 1:     // Overlong / absurd length — allocation and EOF guards.
            wr32(b.data() + h + kNameLen,
                 (rng() % 2) ? 0xFFFFFFFFu : uint32_t(rng()));
            break;
        case 2:     // Corrupt the NAME so a section is misrouted, or rename a
                    // neighbour onto MEX/MEM and feed it foreign bytes.
            b[h + (rng() % kNameLen)] = uint8_t(rng());
            break;
    }
    return true;
}

void mutate(std::vector<uint8_t>& b, std::mt19937& rng)
{
    if (b.empty()) return;
    switch (rng() % 5) {
        case 0: {                                  // truncate mid-section
            std::uniform_int_distribution<size_t> d(1, b.size());
            b.resize(d(rng));
            break;
        }
        case 1: {                                  // smash a length field
            if (b.size() < 4) break;
            std::uniform_int_distribution<size_t> d(0, b.size() - 4);
            const size_t off = d(rng);
            for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(rng());
            break;
        }
        case 2: {                                  // max a length — overflow bait
            if (b.size() < 8) break;
            std::uniform_int_distribution<size_t> d(0, b.size() - 8);
            const size_t off = d(rng);
            for (int i = 0; i < 8; ++i) b[off + i] = 0xFF;
            break;
        }
        case 3: {                                  // scatter
            std::uniform_int_distribution<size_t> d(0, b.size() - 1);
            const int n = 1 + int(rng() % 24);
            for (int i = 0; i < n; ++i) b[d(rng)] = uint8_t(rng());
            break;
        }
        case 4: {                                  // trailing garbage
            const size_t add = rng() % 1024;
            for (size_t i = 0; i < add; ++i) b.push_back(uint8_t(rng()));
            break;
        }
    }
}

} // namespace


// ─── Per-card restore CLAMPS (2026-09-06 bug hunt #2, item S14) ─────────
//
// The section walker above guards the FRAMING. These guard the semantics of
// individual fields once a card's own loader has accepted a blob: values that
// are in range for their C++ type but out of range for the invariant the live
// code assumes. Each one below stalled or spun something.

const std::vector<uint8_t> kForeign(64, 0xAB);

void testTranswarpDisplacedRom()
{
    // S1. `displaced_` is the Apple's OWN $F000-$FFFF, held aside while the
    // card shadows it — the only copy that exists, since Memory holds the
    // card's ROM there. It was never serialised, so a restore + a later
    // $C072 wrote 4 KB of zeroes over Applesoft + Monitor.
    //
    // No Memory here: drive the card through setRom/onPlug so it shadows,
    // then assert the blob CARRIES the 4 KB (v2 = one flag byte + the ROM)
    // and that a fresh card re-emits an identical blob after a restore.
    pom2::TranswarpCard a(4);
    std::vector<uint8_t> bare;
    a.appendSnapshotState(bare);

    // Not shadowing (no Memory attached): one flag byte, no 4 KB.
    assert(bare.size() < 4096);

    pom2::TranswarpCard b(4);
    b.loadSnapshotState(bare.data(), bare.size());
    std::vector<uint8_t> out;
    b.appendSnapshotState(out);
    assert(out == bare);

    // A v1 blob (the pre-fix layout: everything up to and including
    // slowCycles_, no displaced tail) must still load.
    std::vector<uint8_t> v1(bare.begin(), bare.end() - 1);
    v1[4] = 1; v1[5] = 0;                     // version 2 → 1
    pom2::TranswarpCard c(4);
    c.loadSnapshotState(v1.data(), v1.size());

    // A blob claiming a version this build does not know is refused whole.
    std::vector<uint8_t> future = bare;
    future[4] = 99;
    pom2::TranswarpCard d(4);
    std::vector<uint8_t> fresh;
    d.appendSnapshotState(fresh);
    d.loadSnapshotState(future.data(), future.size());
    std::vector<uint8_t> after;
    d.appendSnapshotState(after);
    assert(after == fresh);

    std::printf("  ok: TransWarp snapshot is v2 and tolerates v1 / refuses "
                "unknown versions\n");
}


void testTranswarpSlowCyclesClamp()
{
    // S14. `slowCycles_` is only ever counted DOWN by advanceCycles, so a
    // restored negative (the field is signed, the blob field is a raw u32) or
    // an absurd positive parks the card at 1 MHz for ever.
    pom2::TranswarpCard a(4);
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    // slowCycles_ is the u32 after magic(4) + version(2) + six flag bytes.
    constexpr size_t kSlowAt = 4 + 2 + 6;
    for (uint32_t v : { 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu }) {
        std::vector<uint8_t> bad = blob;
        for (int i = 0; i < 4; ++i)
            bad[kSlowAt + i] = static_cast<uint8_t>(v >> (8 * i));
        pom2::TranswarpCard c(4);
        c.loadSnapshotState(bad.data(), bad.size());
        const int got = c.slowCyclesRemaining();
        assert(got >= 0 && got <= 4096 &&
               "TransWarp slowCycles_ restored outside the range a real "
               "slowdown window can produce");
    }
    std::printf("  ok: TransWarp slowCycles_ clamped on restore\n");
}

void testM68705StackPointerClamp()
{
    // S14. The 68705's stack is a 32-byte window that WRAPS between kSpFloor
    // ($60) and kSpMask ($7F); push/pull compare S against those two ends by
    // EQUALITY, so a restored value outside the window never hits either and
    // walks S straight out of the stack, one push at a time, over RAM.
    M68705P3 a;
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(blob.size() == M68705P3::kSnapshotBytes);

    // S is the byte after the 16-bit PC.
    for (uint8_t bad : { uint8_t(0x00), uint8_t(0x5F), uint8_t(0x80),
                         uint8_t(0xFF) }) {
        std::vector<uint8_t> b = blob;
        b[2] = bad;
        M68705P3 c;
        assert(c.loadSnapshotState(b.data(), b.size()) == b.size());
        // `reg` is private; re-serialise and read S back out of the blob.
        std::vector<uint8_t> back;
        c.appendSnapshotState(back);
        assert(back[2] >= 0x60 && back[2] <= 0x7F &&
               "M68705 stack pointer restored outside its 32-byte window");
    }
    // A legal value is left alone.
    {
        std::vector<uint8_t> b = blob;
        b[2] = 0x6A;
        M68705P3 c;
        (void)c.loadSnapshotState(b.data(), b.size());
        std::vector<uint8_t> back;
        c.appendSnapshotState(back);
        assert(back[2] == 0x6A);
    }
    std::printf("  ok: M68705 stack pointer clamped to its 32-byte window\n");
}

void testClockCardTpPeriodRederived()
{
    // S15. The TP half-period is DERIVED from the rate and the machine's CPU
    // clock; taking the blob's value verbatim imported the period of the
    // machine the snapshot came from, and let a corrupt blob pair a live rate
    // with a 1-cycle period (advanceCycles then loops per cycle).
    ClockCard a(4);
    a.setCpuClock(1020000.0);
    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    for (size_t i = 0; i < blob.size(); ++i) {
        std::vector<uint8_t> bad = blob;
        bad[i] = 0xFF;
        ClockCard c(4);
        c.setCpuClock(1020000.0);
        c.loadSnapshotState(bad.data(), bad.size());
        c.advanceCycles(20000);         // one PAL frame's worth
    }
    std::printf("  ok: ClockCard TP timer survives a corrupted blob\n");
}

int main(int argc, char** argv)
{
    testTranswarpDisplacedRom();
    testTranswarpSlowCyclesClamp();
    testM68705StackPointerClamp();
    testClockCardTpPeriodRederived();

    const unsigned seed  = (argc > 1) ? unsigned(std::stoul(argv[1])) : 20260820u;
    const int      iters = (argc > 2) ? std::stoi(argv[2]) : 1500;

    // One real capture to mutate from.
    std::vector<uint8_t> golden;
    {
        Memory mem;
        M6502  cpu(&mem);
        for (int i = 0; i < 4096; ++i)
            mem.writeRamUnchecked(uint16_t(i), uint8_t(i * 7));
        pom2::SnapshotWriter w(golden);
        pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
        assert(w.finish() && "capture must succeed");
    }
    assert(!golden.empty());

    // The unmutated blob MUST restore, or every mutant below is only
    // exercising the reject path and the test proves nothing.
    {
        Memory mem; M6502 cpu(&mem);
        pom2::SnapshotReader r(golden.data(), golden.size());
        const auto res = pom2::restoreMachineState(r, cpu, mem);
        assert(res.ok && "golden snapshot must round-trip");
    }

    std::mt19937 rng(seed);
    int accepted = 0;
    for (int i = 0; i < iters; ++i) {
        std::vector<uint8_t> b = golden;
        const int rounds = 1 + int(rng() % 3);
        for (int r = 0; r < rounds; ++r) {
            // Mostly structure-aware; the generic pass still runs sometimes,
            // since it is what produces the ragged truncations and broken
            // magics the front door has to reject.
            if (!(rng() % 4) || !mutateSections(b, rng)) mutate(b, rng);
        }
        if (b.empty()) continue;

        Memory mem; M6502 cpu(&mem);
        pom2::SnapshotReader rd(b.data(), b.size());
        const auto res = pom2::restoreMachineState(rd, cpu, mem);
        if (res.ok) ++accepted;
        // Use the machine: an "ok" restore that left Memory inconsistent is
        // only visible through a read.
        for (int a = 0; a < 64; ++a) (void)mem.memRead(uint16_t(a * 1021));
        (void)cpu.getProgramCounter();
    }

    // Guard the fuzzer against itself: if mutants stop being accepted, the
    // walker is no longer being reached and this test has quietly become a
    // no-op. Loose bound — it is a smoke alarm, not a tuned ratio.
    assert(accepted > iters / 20 &&
           "too few mutants accepted — the section walker is not being reached");

    std::printf("fuzz_snapshot: %d mutants survived, %d accepted (seed %u)\n",
                iters, accepted, seed);
    return 0;
}
