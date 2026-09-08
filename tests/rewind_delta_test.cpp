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

// Rewind delta-codec + keyframe test (Phase 2).
//
// RewindBuffer keeps memory bounded by storing periodic full keyframes and
// XOR deltas in between. This test forces a small keyframe interval so
// reconstruction crosses several keyframe boundaries, then pins:
//   1. EVERY stored frame restores bit-for-bit (CPU + RAM + cycle), proving
//      the keyframe/delta reconstruction is exact;
//   2. deltas actually shrink memory vs all-keyframes, and keyframes land at
//      the configured cadence;
//   3. eviction (with keyframe rebasing) keeps every survivor restorable;
//   4. truncateAfter() drops the future and lets capture continue exactly.
//
// The Phase 1 test (rewind_roundtrip) already pins the public API against the
// full-snapshot behaviour; this one pins the compressed path behind it.

#include "M6502.h"
#include "Memory.h"
#include "RewindBuffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void setState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    cpu.setAccumulator(tag);
    cpu.setXRegister(static_cast<uint8_t>(tag ^ 0xFF));
    cpu.setYRegister(static_cast<uint8_t>(tag + 0x10));
    cpu.setStackPointer(static_cast<uint8_t>(0xF0 - tag));
    cpu.setStatusRegister(static_cast<uint8_t>((tag & 0x0F) | 0x20));
    cpu.setProgramCounter(static_cast<uint16_t>(0x1000 + tag));
    mem.setCycleCounter(cyc);
    // A few scattered RAM writes — exercises the delta span coalescing.
    mem.memWrite(0x0300, tag);
    mem.memWrite(0x0400, static_cast<uint8_t>(tag + 7));
    mem.memWrite(0x2000, static_cast<uint8_t>(tag * 5));
    mem.memWrite(0x6000, static_cast<uint8_t>(tag * 3 + 1));
    mem.memWrite(0x9000, static_cast<uint8_t>(0xA0 ^ tag));
}

void checkState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    assert(cpu.getAccumulator()    == tag);
    assert(cpu.getXRegister()      == static_cast<uint8_t>(tag ^ 0xFF));
    assert(cpu.getYRegister()      == static_cast<uint8_t>(tag + 0x10));
    assert(cpu.getStackPointer()   == static_cast<uint8_t>(0xF0 - tag));
    assert(cpu.getStatusRegister() == static_cast<uint8_t>((tag & 0x0F) | 0x20));
    assert(cpu.getProgramCounter() == static_cast<uint16_t>(0x1000 + tag));
    assert(mem.getCycleCounter()   == cyc);
    assert(mem.data()[0x0300] == tag);
    assert(mem.data()[0x0400] == static_cast<uint8_t>(tag + 7));
    assert(mem.data()[0x2000] == static_cast<uint8_t>(tag * 5));
    assert(mem.data()[0x6000] == static_cast<uint8_t>(tag * 3 + 1));
    assert(mem.data()[0x9000] == static_cast<uint8_t>(0xA0 ^ tag));
}

void scramble(M6502& cpu, Memory& mem) { setState(cpu, mem, 0x5A, 0xDEADBEEF); }

// tag/cycle for the k-th captured frame.
uint8_t  tagOf(int k)   { return static_cast<uint8_t>(1 + k); }
uint64_t cycOf(int k)   { return static_cast<uint64_t>(1 + k) * 1000; }

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);

    // ── (1) + (2) Bit-exact across keyframe boundaries; memory shrinks ─────
    {
        pom2::RewindBuffer rb(1000);
        rb.setEnabled(true);
        rb.setKeyframeInterval(5);            // keyframe every 5 frames

        const int N = 23;
        for (int k = 0; k < N; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        assert(rb.size() == static_cast<size_t>(N));

        // Keyframes at indices 0,5,10,15,20 → ceil(23/5) = 5.
        assert(rb.keyframeCount() == 5);
        assert(rb.infoAt(0).keyframe);
        assert(rb.infoAt(5).keyframe);
        assert(!rb.infoAt(1).keyframe);
        assert(!rb.infoAt(6).keyframe);

        // Every single frame must round-trip exactly — including the frames
        // farthest from their keyframe (4, 9, 14, ...).
        for (int k = N - 1; k >= 0; --k) {     // reverse order = real rewind
            scramble(cpu, mem);
            assert(rb.restore(static_cast<size_t>(k), cpu, mem));
            checkState(cpu, mem, tagOf(k), cycOf(k));
        }

        // Deltas must be much smaller than storing N full keyframes. A full
        // blob is the keyframe size; deltas here touch only regs + ~5 bytes.
        const size_t keyframeBytes = rb.infoAt(0).bytes;
        const size_t deltaBytes    = rb.infoAt(1).bytes;
        assert(deltaBytes * 4 < keyframeBytes);             // deltas are tiny
        assert(rb.bytes() < keyframeBytes * static_cast<size_t>(N) / 2);  // big win

        // restoreToCycle still lands on the right frame through the codec.
        scramble(cpu, mem);
        assert(rb.restoreToCycle(cycOf(12) + 500, cpu, mem) == 12);
        checkState(cpu, mem, tagOf(12), cycOf(12));
    }

    // ── (3) Eviction with keyframe rebasing keeps survivors restorable ─────
    {
        pom2::RewindBuffer rb(7);             // small ring
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);

        const int N = 30;
        for (int k = 0; k < N; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        // Both caps are CEILINGS, not exact counts (bug hunt #4, item #4):
        // eviction drops a keyframe together with the deltas hanging off it,
        // in one step, instead of reconstructing frames_[1] into a fresh
        // keyframe per evicted frame. So the ring holds between
        // `maxFrames - keyframeInterval + 1` and `maxFrames` frames.
        assert(rb.size() <= 7);
        assert(rb.size() >= 7 - 4 + 1);
        // Front must be a keyframe (the rebase invariant) so reconstruction
        // of every survivor is well-defined.
        assert(rb.infoAt(0).keyframe);

        const uint64_t oldest = rb.oldestCycle();
        const uint64_t newest = rb.newestCycle();
        assert(newest == cycOf(N - 1));
        for (size_t i = 0; i < rb.size(); ++i) {
            const uint64_t c = rb.infoAt(i).cycle;
            assert(c >= oldest && c <= newest);
            // map cycle back to its tag: cyc = (tag) * 1000, tag = 1 + k.
            const uint8_t tag = static_cast<uint8_t>(c / 1000);
            scramble(cpu, mem);
            assert(rb.restore(i, cpu, mem));
            checkState(cpu, mem, tag, c);
        }
    }

    // ── (4) truncateAfter() drops the future; capture continues exactly ────
    {
        pom2::RewindBuffer rb(1000);
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);

        for (int k = 0; k < 12; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        rb.truncateAfter(5);                  // keep frames 0..5
        assert(rb.size() == 6);
        assert(rb.newestCycle() == cycOf(5));

        // Old frames still restore.
        scramble(cpu, mem);
        assert(rb.restore(5, cpu, mem));
        checkState(cpu, mem, tagOf(5), cycOf(5));

        // Resume capturing on the new timeline; the first appended frame
        // delta-bases off frame 5 (truncate rebuilt prevBlob_).
        setState(cpu, mem, 200, 999'000);
        rb.capture(cpu, mem);
        assert(rb.size() == 7);
        scramble(cpu, mem);
        assert(rb.restore(6, cpu, mem));
        checkState(cpu, mem, 200, 999'000);
        // …and the pre-truncate frames are still intact.
        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, tagOf(0), cycOf(0));
    }

    // ── (5) Byte budget bounds memory (the RamWorks degradation path) ──────
    {
        pom2::RewindBuffer rb(100000);        // frame cap effectively unlimited
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);

        // Prime one frame to learn the keyframe size, then budget for ~3.
        setState(cpu, mem, 1, 1000);
        rb.capture(cpu, mem);
        const size_t keyframeBytes = rb.bytes();
        assert(keyframeBytes > 0);
        const size_t budget = keyframeBytes * 3;
        rb.setMaxBytes(budget);

        for (int k = 1; k < 200; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        // Memory stays bounded (a lone oversized keyframe is the only allowed
        // exception, and our keyframes are < budget here).
        assert(rb.bytes() <= budget);
        assert(rb.size() >= 1);
        assert(rb.infoAt(0).keyframe);        // front is restorable

        // Whatever survived must still restore exactly.
        const uint64_t oldestCyc = rb.oldestCycle();
        const uint8_t  oldestTag = static_cast<uint8_t>(oldestCyc / 1000);
        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, oldestTag, oldestCyc);

        // Raising the byte cap doesn't retro-actively recover evicted frames,
        // but lowering it evicts immediately.
        const size_t before = rb.size();
        rb.setMaxBytes(keyframeBytes);        // room for ~1 keyframe
        assert(rb.size() <= before);
        assert(rb.bytes() <= keyframeBytes || rb.size() == 1);
    }

    // ── (6) The chunked equal-run scan round-trips exactly ────────────────
    // `encodeXorDelta` advances over unchanged bytes with a memcmp-chunked
    // compare instead of a byte loop (bug hunt #4, item #3): the scan is what
    // a capture spends its time in and it runs under `stateMutex`, so at
    // RamWorks blob sizes the byte loop cost 12.9-19.6 ms per frame against
    // 0.9-2.4 ms for a block compare of the same span.
    //
    // The wire format did not change, and this is what says so. The patterns
    // are chosen around the codec's two constants: the 16-byte coalesce
    // window decides whether two changed spans become one record or two, and
    // the 64-byte compare block decides whether a difference is found by the
    // block compare or by its byte-wise fixup. A boundary read wrong still
    // survives the encoder's own inverse, so the assertion is the stronger
    // one — the restored machine equals the captured one over ALL 64 KiB.
    {
        pom2::RewindBuffer rb(1000);
        rb.setEnabled(true);
        rb.setKeyframeInterval(6);            // deltas AND keyframes in the mix

        std::vector<std::vector<uint8_t>> expect;
        std::vector<uint64_t>             expectCyc;

        auto snap = [&](uint64_t cyc) {
            mem.setCycleCounter(cyc);
            expect.emplace_back(mem.data(), mem.data() + 0x10000);
            expectCyc.push_back(cyc);
            rb.capture(cpu, mem);
        };

        // Frame 0: the baseline keyframe.
        for (uint32_t a = 0x2000; a < 0x4000; ++a)
            mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a * 31));
        snap(10'000);

        // One isolated byte.
        mem.memWrite(0x3000, 0x01);
        snap(11'000);

        // Pairs separated by exactly 15 / 16 / 17 bytes — the coalesce cut.
        for (uint16_t gap : { uint16_t(15), uint16_t(16), uint16_t(17) }) {
            const uint16_t base = static_cast<uint16_t>(0x3100 + gap * 0x10);
            mem.memWrite(base, static_cast<uint8_t>(gap));
            mem.memWrite(static_cast<uint16_t>(base + gap + 1),
                         static_cast<uint8_t>(gap ^ 0xFF));
            snap(12'000 + gap);
        }

        // Differences landing 63 / 64 / 65 bytes apart — the block boundary,
        // and one that starts a span in the middle of a block.
        for (uint16_t d : { uint16_t(63), uint16_t(64), uint16_t(65) }) {
            const uint16_t base = static_cast<uint16_t>(0x3400 + d);
            mem.memWrite(base, static_cast<uint8_t>(d + 1));
            mem.memWrite(static_cast<uint16_t>(base + d), static_cast<uint8_t>(d + 2));
            snap(13'000 + d);
        }

        // A long changed run that crosses many blocks, then a single byte
        // right at the far end of the blob (the tail the block loop cannot
        // cover).
        for (uint16_t a = 0x5000; a < 0x5400; ++a)
            mem.memWrite(a, static_cast<uint8_t>(a ^ 0x5A));
        snap(14'000);
        mem.memWrite(0xBFFF, 0x99);
        snap(15'000);

        // Deterministic scatter — many small spans at unaligned offsets.
        uint32_t rng = 0x1234567u;
        for (int f = 0; f < 12; ++f) {
            for (int i = 0; i < 40; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const uint16_t a = static_cast<uint16_t>(0x6000 + (rng >> 8) % 0x2000);
                mem.memWrite(a, static_cast<uint8_t>(rng >> 24));
            }
            snap(16'000 + static_cast<uint64_t>(f) * 10);
        }

        assert(rb.size() == expect.size());
        assert(rb.keyframeCount() > 1 && "the case must cross keyframes");
        for (size_t i = 0; i < rb.size(); ++i) {
            scramble(cpu, mem);
            assert(rb.restore(i, cpu, mem));
            assert(mem.getCycleCounter() == expectCyc[i]);
            assert(std::memcmp(mem.data(), expect[i].data(), 0x10000) == 0 &&
                   "the delta codec did not reproduce the captured RAM");
        }
    }

    // ── (7) Eviction is bounded and never reconstructs per evicted frame ──
    // (bug hunt #4, item #4.) Promoting `frames_[1]` into a keyframe copied a
    // whole blob and freed only the evicted delta, so one blob's worth of
    // budget cost ~`keyframeInterval` blob-sized copies under `stateMutex` —
    // measured 75-110 ms captures dropping ~30 frames every 2 s at the
    // 256 MiB cap. Eviction now walks forward to the next keyframe in one
    // step. The shape that pins it (deliberately not a millisecond ceiling,
    // which would only measure the build machine's load): a single capture
    // never drops more than one keyframe group, and the front stays a
    // keyframe so every survivor is still restorable.
    {
        pom2::RewindBuffer rb(100000);        // frame cap does not bind
        rb.setEnabled(true);
        rb.setKeyframeInterval(8);

        setState(cpu, mem, 1, 1000);
        rb.capture(cpu, mem);
        const size_t keyframeBytes = rb.bytes();
        assert(keyframeBytes > 0);
        rb.setMaxBytes(keyframeBytes * 3);    // binds within a few keyframes

        bool sawEviction = false;
        for (int k = 1; k < 300; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            const size_t before = rb.size();
            rb.capture(cpu, mem);
            const size_t after = rb.size();
            assert(after <= before + 1);
            const size_t dropped = before + 1 - after;
            if (dropped) sawEviction = true;
            assert(dropped <= rb.keyframeInterval() &&
                   "one capture evicted more than a whole keyframe group");
            assert(rb.infoAt(0).keyframe && "front is no longer a keyframe");
            // …and it sits on a real keyframe BOUNDARY. Forward eviction can
            // only ever leave a frame that was captured as a keyframe, so
            // this is structural rather than incidental; it is what would
            // break first if someone re-introduced a partial-group eviction
            // (which would leave a delta at the front with no base). Note it
            // does NOT discriminate against the old promotion loop, which
            // happened to stop on the same boundary — that difference is
            // cost, not shape. Keyframes land at capture ordinals 0, 8,
            // 16, … and ordinal k carries cycle (k + 1) * 1000.
            const uint64_t frontOrdinal = rb.infoAt(0).cycle / 1000 - 1;
            assert(frontOrdinal % rb.keyframeInterval() == 0 &&
                   "the front keyframe was manufactured by reconstructing a "
                   "delta — eviction is copying a full blob per evicted frame");
            assert(rb.bytes() <= keyframeBytes * 3 || rb.size() == 1);
        }
        assert(sawEviction && "the byte cap never bound — the case proves nothing");

        // Everything that survived still restores.
        for (size_t i = 0; i < rb.size(); ++i) {
            const uint64_t c = rb.infoAt(i).cycle;
            scramble(cpu, mem);
            assert(rb.restore(i, cpu, mem));
            checkState(cpu, mem, static_cast<uint8_t>(c / 1000), c);
        }
    }

    // ── The serialize scratch stays hot between captures ─────────────────
    // (bug hunt #8.) `capture` used to move-assign the scratch into the
    // running blob, which freed the old blob and left the scratch at capacity
    // 0 — the next capture re-grew and first-touched the whole blob (10.5 MB
    // with 128 RamWorks banks) under stateMutex, every frame. Swap keeps both
    // buffers' capacity.
    {
        pom2::RewindBuffer rb(100);
        rb.setEnabled(true);
        for (int k = 0; k < 3; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        assert(rb.scratchCapacity() >= rb.infoAt(0).bytes &&
               "the capture scratch was handed away — the next capture "
               "re-grows the whole blob under the lock");
    }

    std::printf("Rewind delta codec: OK (keyframe/delta exact + evict + truncate + "
                "budget + chunked-scan round-trip + bounded eviction)\n");
    return 0;
}
