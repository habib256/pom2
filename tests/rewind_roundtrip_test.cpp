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

// Rewind ring-buffer round-trip test (Phase 1).
//
// Drives Memory + M6502 directly (no ImGui / controller) and pins the core
// rewind contract:
//   1. capture → restore reproduces CPU registers + main RAM + cycle
//      counter bit-for-bit;
//   2. the ring evicts oldest-first once over its frame cap;
//   3. restoreToCycle() lands on the newest frame at-or-before a target
//      cycle (and clamps to the oldest when the target predates the ring);
//   4. a disabled buffer captures nothing (zero overhead);
//   5. a capture stamped at-or-before the tail (the machine jumped back in
//      time) drops the abandoned future, so the ring stays strictly
//      increasing and the seek helpers keep meaning what they say.
//
// This is the gate for any change to RewindBuffer or the MachineSnapshot
// capture/restore sequence it rides on.

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "RewindBuffer.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Stamp a recognisable, fully-recoverable machine state keyed by `tag`.
// Uses main-RAM addresses ($0300/$6000) that are plain writable RAM on the
// default (II+) profile, so restoreMainRam round-trips them.
void setState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    cpu.setAccumulator(tag);
    cpu.setXRegister(static_cast<uint8_t>(tag ^ 0xFF));
    cpu.setYRegister(static_cast<uint8_t>(tag + 0x10));
    cpu.setStackPointer(static_cast<uint8_t>(0xF0 - tag));
    cpu.setStatusRegister(static_cast<uint8_t>((tag & 0x0F) | 0x20));
    cpu.setProgramCounter(static_cast<uint16_t>(0x1000 + tag));
    mem.setCycleCounter(cyc);
    mem.memWrite(0x0300, tag);
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
    assert(mem.data()[0x6000] == static_cast<uint8_t>(tag * 3 + 1));
    assert(mem.data()[0x9000] == static_cast<uint8_t>(0xA0 ^ tag));
}

void scramble(M6502& cpu, Memory& mem)
{
    setState(cpu, mem, 0x5A, 0xDEADBEEF);   // anything distinct from a tag
}

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);

    // ── (4) Disabled buffer captures nothing ──────────────────────────────
    {
        pom2::RewindBuffer rb;
        assert(!rb.enabled());
        setState(cpu, mem, 1, 1000);
        rb.capture(cpu, mem);
        assert(rb.empty());
    }

    // External snapshots are strict: a valid CPU paired with a short MEM
    // section must be rejected before either half is applied.
    {
        setState(cpu, mem, 66, 66'000);
        std::vector<uint8_t> malformed;
        pom2::SnapshotWriter w(malformed);
        auto h = w.beginSection("CPU");
        w.writeU16(0x1234);
        for (int i = 0; i < 6; ++i) w.writeU8(static_cast<uint8_t>(i));
        w.writeU64(1234);
        w.writeU8(0);
        w.endSection(h);
        std::vector<uint8_t> shortMem(0xffff, 0xA5);
        w.writeSection("MEM", shortMem.data(), shortMem.size());
        assert(w.finish());

        pom2::SnapshotReader reader(malformed.data(), malformed.size());
        const auto result = pom2::restoreMachineState(reader, cpu, mem);
        assert(!result.ok);
        checkState(cpu, mem, 66, 66'000);
    }

    // SLOT payloads belong only to trusted in-memory rewind frames. An
    // external file cannot drive host-backed cards or smuggle a pathological
    // controller state into the next emulation tick.
    {
        setState(cpu, mem, 65, 65'000);
        std::vector<uint8_t> hostile;
        pom2::SnapshotWriter w(hostile);
        auto h = w.beginSection("CPU");
        w.writeU16(0x4321);
        for (int i = 0; i < 6; ++i) w.writeU8(static_cast<uint8_t>(i + 1));
        w.writeU64(4321);
        w.writeU8(0);
        w.endSection(h);
        w.writeSection("MEM", mem.data(), 0x10000);
        const uint8_t slotPayload[] = { 'D', '2', 2, 0xff };
        w.writeSection("SLOT6", slotPayload, sizeof(slotPayload));
        assert(w.finish());

        pom2::SnapshotReader reader(hostile.data(), hostile.size());
        const auto result = pom2::restoreMachineState(reader, cpu, mem);
        assert(!result.ok);
        checkState(cpu, mem, 65, 65'000);
    }

    // A late malformed section must roll back CPU, RAM and extended memory,
    // not leave a hybrid machine after reporting failure.
    {
        setState(cpu, mem, 99, 99'000);
        std::vector<uint8_t> hostile;
        {
            pom2::SnapshotWriter w(hostile);
            auto h = w.beginSection("CPU");
            w.writeU16(cpu.getProgramCounter());
            w.writeU8(cpu.getAccumulator());
            w.writeU8(cpu.getXRegister());
            w.writeU8(cpu.getYRegister());
            w.writeU8(cpu.getStatusRegister());
            w.writeU8(cpu.getStackPointer());
            w.writeU8(0);
            w.writeU64(mem.getCycleCounter());
            w.writeU8(0);
            w.endSection(h);
            w.writeSection("MEM", mem.data(), 0x10000);
            const uint8_t malformedMex = 0xFF;  // bad version + truncated
            w.writeSection("MEX", &malformedMex, 1);
            assert(w.finish());
        }

        setState(cpu, mem, 77, 77'000);
        std::vector<uint8_t> before;
        {
            pom2::SnapshotWriter w(before);
            pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
            assert(w.finish());
        }
        pom2::SnapshotReader reader(hostile.data(), hostile.size());
        const auto result = pom2::restoreMachineState(reader, cpu, mem);
        assert(!result.ok);
        checkState(cpu, mem, 77, 77'000);
        std::vector<uint8_t> after;
        {
            pom2::SnapshotWriter w(after);
            pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
            assert(w.finish());
        }
        assert(after == before && "failed restore must be fully transactional");
    }

    // ── (1) Bit-for-bit capture → restore ─────────────────────────────────
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);

        setState(cpu, mem, 11, 11'000);
        rb.capture(cpu, mem);                 // frame 0
        setState(cpu, mem, 22, 22'000);
        rb.capture(cpu, mem);                 // frame 1
        setState(cpu, mem, 33, 33'000);
        rb.capture(cpu, mem);                 // frame 2
        assert(rb.size() == 3);
        assert(rb.oldestCycle() == 11'000 && rb.newestCycle() == 33'000);

        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 11, 11'000);

        scramble(cpu, mem);
        assert(rb.restore(2, cpu, mem));
        checkState(cpu, mem, 33, 33'000);

        scramble(cpu, mem);
        assert(rb.restore(1, cpu, mem));
        checkState(cpu, mem, 22, 22'000);

        assert(!rb.restore(3, cpu, mem));     // out of range
    }

    // ── (3) restoreToCycle lands on newest frame <= target ────────────────
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);
        setState(cpu, mem, 1, 1000); rb.capture(cpu, mem);
        setState(cpu, mem, 2, 2000); rb.capture(cpu, mem);
        setState(cpu, mem, 3, 3000); rb.capture(cpu, mem);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(2500, cpu, mem) == 1);   // newest <= 2500 → 2000
        checkState(cpu, mem, 2, 2000);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(3000, cpu, mem) == 2);   // exact match
        checkState(cpu, mem, 3, 3000);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(50, cpu, mem) == 0);     // predates ring → oldest
        checkState(cpu, mem, 1, 1000);

        pom2::RewindBuffer empty;
        assert(empty.restoreToCycle(123, cpu, mem) == pom2::RewindBuffer::kNoFrame);
    }

    // ── (2) Ring eviction: oldest-first, cap honoured ─────────────────────
    {
        pom2::RewindBuffer rb(3);             // cap = 3 frames
        rb.setEnabled(true);
        for (uint8_t i = 0; i < 5; ++i) {
            setState(cpu, mem, static_cast<uint8_t>(40 + i),
                     static_cast<uint64_t>(40 + i) * 1000);
            rb.capture(cpu, mem);
        }
        assert(rb.size() == 3);
        // Frames 0,1 evicted; the survivors are tags 42,43,44 (cycles 42k..44k).
        assert(rb.oldestCycle() == 42'000);
        assert(rb.newestCycle() == 44'000);
        assert(rb.infoAt(0).cycle == 42'000);
        assert(rb.infoAt(0).bytes > 0);

        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 42, 42'000);

        // Shrinking the cap evicts immediately.
        rb.setMaxFrames(1);
        assert(rb.size() == 1);
        assert(rb.newestCycle() == 44'000);   // the most recent survives

        rb.clear();
        assert(rb.empty() && rb.bytes() == 0);
    }

    // ── (5) A jump back in time drops the abandoned future ────────────────
    // The ring's stamps must be STRICTLY increasing: `indexForCycle` breaks
    // out at the first frame past its target, so a single out-of-order frame
    // makes every seek beyond it land somewhere else, and `newest - oldest`
    // (the panel's "span" readout) stops being a duration at all.
    //
    // The machine really does jump back: a scrub resumed through anything but
    // `rewindEndAndResume` — the toolbar Play button, Machine > Run, the
    // `machine.run` palette command, the kiosk menu — leaves the old future in
    // the deque and records the new timeline straight on top of it.
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);          // deltas in the survivors too
        for (uint8_t i = 0; i < 12; ++i) {
            setState(cpu, mem, static_cast<uint8_t>(60 + i),
                     static_cast<uint64_t>(60 + i) * 1000);
            rb.capture(cpu, mem);
        }
        assert(rb.size() == 12);

        // Scrub back to frame 3, then resume WITHOUT truncateAfter.
        assert(rb.restore(3, cpu, mem));
        checkState(cpu, mem, 63, 63'000);
        setState(cpu, mem, 99, 63'500);     // one resumed frame's worth
        rb.capture(cpu, mem);

        assert(rb.size() == 5);             // frames 0..3 + the resumed one
        assert(rb.newestCycle() == 63'500);
        for (size_t i = 1; i < rb.size(); ++i)
            assert(rb.infoAt(i).cycle > rb.infoAt(i - 1).cycle);
        // The seek helper lands where it claims.
        assert(rb.indexForCycle(63'400) == 3);
        assert(rb.indexForCycle(70'000) == 4);
        // Survivors still reconstruct — truncateAfter rebased the delta base.
        scramble(cpu, mem);
        assert(rb.restore(1, cpu, mem));
        checkState(cpu, mem, 61, 61'000);
        scramble(cpu, mem);
        assert(rb.restore(4, cpu, mem));
        checkState(cpu, mem, 99, 63'500);

        // A jump back to before the OLDEST retained frame abandons the whole
        // ring — and the restart must be a keyframe, not a delta against a
        // base blob belonging to a timeline that no longer exists.
        setState(cpu, mem, 7, 500);
        rb.capture(cpu, mem);
        assert(rb.size() == 1);
        assert(rb.infoAt(0).keyframe);
        assert(rb.newestCycle() == 500);
        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 7, 500);
    }

    // ── Re-enabling Record starts an EMPTY timeline ───────────────────────
    // (bug hunt #8.) The splice fix above restarted only the delta base; the
    // frames from before the pause stayed in the deque. Ten frames, a
    // five-minute pause, ten more: the panel read "20 frames · 300 s" and one
    // slider notch across the join jumped the machine five minutes back.
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);
        const uint64_t frame = 20313;
        for (uint8_t i = 0; i < 10; ++i) {
            setState(cpu, mem, static_cast<uint8_t>(20 + i), (i + 1) * frame);
            rb.capture(cpu, mem);
        }
        rb.setEnabled(false);
        const uint64_t resumeAt = 10 * frame + 300ull * 50 * frame;   // 300 s later
        rb.setEnabled(true);
        for (uint8_t i = 0; i < 10; ++i) {
            setState(cpu, mem, static_cast<uint8_t>(40 + i), resumeAt + i * frame);
            rb.capture(cpu, mem);
        }
        assert(rb.size() == 10 &&
               "frames from before the Record pause survived into the new "
               "timeline");
        for (size_t i = 1; i < rb.size(); ++i)
            assert(rb.infoAt(i).cycle - rb.infoAt(i - 1).cycle <= frame &&
                   "the ring presents two timelines as one history");
        assert(rb.infoAt(0).keyframe);
        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 40, resumeAt);
    }

    std::printf("Rewind ring buffer: OK (round-trip + eviction + seek + "
                "abandoned-future drop + record re-enable)\n");
    return 0;
}
