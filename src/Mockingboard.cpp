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

#include "Mockingboard.h"

#include "AyPsgSynth.h"
#include "ByteIO.h"
#include "CpuClock.h"
#include "M6502.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

namespace {

// ─── AY-3-8910 amplitude table ───────────────────────────────────────────
// Lives in AyPsgSynth.h since 2026-08-01 so Mockingboard and Phasor share
// one copy (and one provenance note — the old "MAME build_single_table"
// citation here was wrong; see the header).
using pom2::ay::kVolumeTable;

// AY-3-8910 input clock on the Mockingboard — pin 22 (CLOCK) is wired to
// the slot's phase 0 clock, i.e. the Apple II 1.0227 MHz CPU clock.
//
// MAME `ay8910.cpp:1077` runs its synthesis stream at `clock/8` and
// increments tone/noise counters by 1 per sample. Datasheet output
// freq is `clock/(16*TP)` — produced by a divide-by-2 T-flop on the
// counter output (one toggle per `TP` counter ticks → output cycle =
// `2*TP/(clock/8) = TP/(clock/16)` sec). POM2 matches MAME by stepping
// counters at clock/8 and toggling on `counter >= TP`; the implicit
// /2 lives in the toggle. Earlier POM2 versions used clock/16 which
// produced **one octave too low** on every Mockingboard note. Fixed
// 2026-05-14.
// These are the NTSC nominal values, kept for reference and for tests
// that quote them. The render loop derives its own tick rate from the
// LIVE CPU clock instead (`ticksPerSample` in fillAudioBuffer) so that a
// PAL machine clocks the AY at its real 1 015 625 Hz.
[[maybe_unused]] constexpr float kAyClockHz     = static_cast<float>(POM2_CPU_CLOCK_HZ);
[[maybe_unused]] constexpr float kAyToneStepHz  = kAyClockHz / 8.0f;   // ~127.8 kHz
[[maybe_unused]] constexpr float kAyNoiseStepHz = kAyClockHz / 8.0f;   // ~127.8 kHz

// Convenience aliases — the VIA register layout, IFR bits, AY register
// count, and PB control-bus map all live as static members of the
// shared `pom2::Via6522` / `pom2::Ay3_8910` types since 2026-05-27.
// These using-declarations preserve the existing call sites without
// touching the (already-tested) implementation below.
using Via    = pom2::Via6522;
using AyChip = pom2::Ay3_8910;
constexpr int     kAyNumRegs    = AyChip::kAyNumRegs;
[[maybe_unused]] constexpr uint8_t kPbBitBc1     = AyChip::kPbBitBc1;
[[maybe_unused]] constexpr uint8_t kPbBitBdir    = AyChip::kPbBitBdir;
[[maybe_unused]] constexpr uint8_t kPbBitReset   = AyChip::kPbBitReset;
[[maybe_unused]] constexpr uint8_t IFR_T1        = Via::IFR_T1;
[[maybe_unused]] constexpr uint8_t IFR_T2        = Via::IFR_T2;
[[maybe_unused]] constexpr uint8_t IFR_ANY       = Via::IFR_ANY;
constexpr uint8_t VIA_ORB       = Via::VIA_ORB;
constexpr uint8_t VIA_ORA       = Via::VIA_ORA;
constexpr uint8_t VIA_DDRB      = Via::VIA_DDRB;
constexpr uint8_t VIA_DDRA      = Via::VIA_DDRA;
constexpr uint8_t VIA_T1CL      = Via::VIA_T1CL;
constexpr uint8_t VIA_T1CH      = Via::VIA_T1CH;
constexpr uint8_t VIA_T1LL      = Via::VIA_T1LL;
constexpr uint8_t VIA_T1LH      = Via::VIA_T1LH;
constexpr uint8_t VIA_T2CL      = Via::VIA_T2CL;
constexpr uint8_t VIA_T2CH      = Via::VIA_T2CH;
constexpr uint8_t VIA_SR        = Via::VIA_SR;
constexpr uint8_t VIA_ACR       = Via::VIA_ACR;
constexpr uint8_t VIA_PCR       = Via::VIA_PCR;
constexpr uint8_t VIA_IFR       = Via::VIA_IFR;
constexpr uint8_t VIA_IER       = Via::VIA_IER;
constexpr uint8_t VIA_ORANH     = Via::VIA_ORANH;

}  // namespace

// ─── Forward types ───────────────────────────────────────────────────────
//
// Via6522 + Ay3_8910 live in shared headers (`Via6522.h` / `Ay3_8910.h`)
// since 2026-05-27 so PhasorCard can reuse them verbatim — same VIA
// timer logic, same AY register-bank + control-bus decoder. Since
// 2026-08-01 the audio-thread synthesis is shared too (`AyPsgSynth.h`:
// generators, mixer, box integration, DC blocker). What stays private to
// this card is the AudioSrc *shell* — the cycle-stamped event queue and
// its jitter-buffer cursor, which are coupled to MockingboardCard's
// `ayResetCount_` / `ayEnvWriteCount_` telemetry and the audio thread's
// mutex protocol. Phasor has no equivalent queue yet.

// File-scope aliases so call sites below (constructors,
// `via_[chip]->advance()`, `Ay3_8910::ApplyResult::Wrote`, …) stay
// unchanged after the extraction.
using Via6522  = pom2::Via6522;
using Ay3_8910 = pom2::Ay3_8910;


// ─── AudioSrc ─────────────────────────────────────────────────────────────
//
// Inner AudioSource, owned by the card. Holds the synthesis state for
// both AY chips and runs entirely on the audio thread (AudioDevice's
// callback). Per call, locks the parent mutex briefly to snapshot both
// AYs' register banks, then synthesises mono samples summed from both
// chips.
struct MockingboardCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(MockingboardCard* p) : parent(p) {}

    MockingboardCard* parent;

    std::atomic<uint32_t> sampleRate { kAudioSampleRate };
    std::atomic<float>    volume     { 0.5f };       // pre-mix gain
    std::atomic<bool>     muted      { false };
    /// Emulated CPU clock for the emuCycles replay cursor — retuned by
    /// MockingboardCard::setCpuClock on a PAL/NTSC switch.
    std::atomic<double>   cpuClockHz { static_cast<double>(POM2_CPU_CLOCK_HZ) };

    // SSI263 mix scratch — member, not a per-callback local: this runs on
    // the realtime audio thread, where a heap allocation per buffer tick
    // is the canonical source of underruns/clicks. Grown once, only
    // touched by the audio thread (fillAudioBuffer is single-consumer).
    std::vector<float> speechScratch;

    /// RateAware override — auto-config when AudioDevice::addSource picks
    /// this AudioSrc up (see MainWindow plugMockingboard).
    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = kAudioSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    // Per-chip synthesis state + the per-tick generators live in
    // AyPsgSynth.h, shared with PhasorCard. Before the 2026-08-01
    // extraction both cards carried verbatim copies and had already
    // drifted apart.
    using ChipState = pom2::ay::ChipSynthState;

    ChipState chip[2];

    // ── emuCycles replay state (audio thread only) ────────────────────
    // `liveRegs` is this thread's register bank, advanced by replaying
    // the CPU thread's cycle-stamped writes; `audioCursor` is the CPU
    // cycle rendered up to so far (SpeakerDevice::audioCpuCursor idiom).
    uint8_t  liveRegs[2][kAyNumRegs] = {};
    bool     regsPrimed = false;
    uint64_t audioCursor = 0;
    /// Sub-cycle remainder of the cursor. cyclesPerSample is ~23.19 at
    /// 44.1 kHz; truncating it per sample made the cursor advance 0.8-1.4 %
    /// SLOWER than the producer — a bigger error, in the same direction,
    /// than the 0.7 % PAL/NTSC delta setCpuClock exists to remove. Same
    /// idiom as SpeakerDevice::subSampleAccum.
    double   cursorFrac = 0.0;
    /// Cycle-stamped register writes not yet rendered. PERSISTENT across
    /// callbacks — see the jitter-buffer note in fillAudioBuffer. Before
    /// 2026-08-01 this was cleared and re-filled every callback, and
    /// anything stamped past the buffer's cycle span was dumped wholesale
    /// at the buffer edge.
    std::deque<MockingboardCard::AyRegEvent> pending;
    /// Last `MockingboardCard::ayQueueGen_` this thread acted on. A
    /// change means the stamps in `pending` describe a timeline the
    /// machine has left (rewind, snapshot load, reset, queue overflow),
    /// so the backlog is dropped rather than replayed.
    uint32_t lastSeenQueueGen = 0;

    /// Fold one replayed event into this thread's register bank. Shared
    /// by the three places that consume the queue — the pre-roll catch-up,
    /// the per-sample render loop, and the overflow drop — so a new event
    /// kind can never be honoured in one and ignored in another.
    void applyEvent(const MockingboardCard::AyRegEvent& e)
    {
        if (e.chip >= 2) return;
        if (e.reg == MockingboardCard::kRegAyReset) {
            // AY /RESET (PB2 low). MAME `ay8910.cpp ay8910_reset_ym`
            // writes 0 to registers 0..AY_PORTA-1 and leaves R14/R15
            // (the I/O ports) alone, then re-seeds the generators.
            std::memset(liveRegs[e.chip], 0, 14);
            chip[e.chip].resetGenerators();
            return;
        }
        if (e.reg < kAyNumRegs) {
            liveRegs[e.chip][e.reg] = e.val;
            // R13 store restarts the envelope even when the shape byte is
            // unchanged (real AY set_shape semantics).
            if (e.reg == 13) chip[e.chip].envRetrigger = true;
        }
    }

    // ── DC blocker ────────────────────────────────────────────────────
    // The AY channel model is UNIPOLAR: a channel contributes
    // `kVolumeTable[level]` or nothing, so a 50 %-duty tone carries a DC
    // term of half its amplitude, and a channel with both tone and noise
    // masked off in R7 (the volume-register PWM / digi technique — 54 %
    // of Digidream 2's register traffic) is pure DC. Every note-on and
    // every volume write therefore steps the DC level, which is an
    // audible click, and half the headroom goes to an inaudible offset.
    // Real hardware AC-couples through the card's output capacitor.
    // Implementation + the MAME citation live in AyPsgSynth.h.
    // One per side since the card went stereo: each carries its own
    // chip, and the filter is linear, so summing the two sides gives
    // exactly what the single blocker on the summed signal gave.
    pom2::ay::DcBlocker dcL;
    pom2::ay::DcBlocker dcR;

    // ── Stereo ────────────────────────────────────────────────────────
    // MAME wires the card to one 2-channel speaker with AY1 on channel 0
    // and AY2 on channel 1 (`a2mockingboard.cpp:159-165`), and routes the
    // speech chip to BOTH channels at unity (`:186-189`) — so speech sits
    // centred while the two PSGs are hard-panned. AppleWin splits it the
    // same way, and says so in terms of the slot window rather than the
    // chips: `MockingboardCardManager.cpp:388-407`, "L = Address.b7=0,
    // R = Address.b7=1" — i.e. VIA1's AY ($Cn00) left, VIA2's ($Cn80)
    // right, which is the pairing POM2 indexes as ay_[0] / ay_[1].
    //
    // `render` is the single implementation. With `right == nullptr` it
    // writes the mono fold-down 0.5*(L+R), which is bit-for-bit the
    // pre-stereo render: that one summed both chips and scaled by 1/6,
    // and 0.5*(c0/3 + c1/3) is the same number.
    void fillAudioBuffer(float* output, int frameCount) override
    {
        render(output, nullptr, frameCount);
    }

    bool fillAudioBufferStereo(float* left, float* right,
                               int frameCount) override
    {
        render(left, right, frameCount);
        return true;
    }

    void render(float* left, float* right, int frameCount)
    {
        if (frameCount <= 0) return;
        // Silence both planes — `right` is null on the mono path.
        const auto silence = [&]() {
            std::fill_n(left, frameCount, 0.0f);
            if (right) std::fill_n(right, frameCount, 0.0f);
        };
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        if (sr == 0) { silence(); return; }
        const bool  isMuted = muted.load(std::memory_order_relaxed);
        const float vol     = volume.load(std::memory_order_relaxed);

        // Snapshot both chips' register banks under the parent mutex.
        // Brief: 32 bytes of memcpy. The CPU thread holds this same
        // mutex on every VIA write, so contention is bounded. Also
        // snapshot `ayResetCount_[]` so the audio thread can detect a
        // PB2=0 reset that fired in the inter-block window and re-seed
        // the noise LFSR per MAME (deterministic noise after reset).
        uint8_t  regSnap[2][kAyNumRegs];
        uint32_t resetCountSnap[2];
        uint32_t envWriteCountSnap[2];
        uint64_t latestEventCycle = 0;
        uint64_t cpuNowSnap       = 0;
        // Set when the CPU side signalled a timeline break (rewind,
        // snapshot load, reset, queue overflow) since the last callback.
        bool     timelineBroke    = false;
        // NOTE: `pending` is NOT cleared unless the timeline broke — it is
        // a jitter buffer that carries un-rendered writes over to the next
        // callback.
        {
            std::lock_guard<std::mutex> lk(parent->mtx);
            std::memcpy(regSnap[0], parent->ay_[0]->regs, kAyNumRegs);
            std::memcpy(regSnap[1], parent->ay_[1]->regs, kAyNumRegs);
            resetCountSnap[0] = parent->ayResetCount_[0];
            resetCountSnap[1] = parent->ayResetCount_[1];
            envWriteCountSnap[0] = parent->ayEnvWriteCount_[0];
            envWriteCountSnap[1] = parent->ayEnvWriteCount_[1];
            // The VIA's synced "now" — the CPU cycle the emulation has
            // actually reached. Pacing needs THIS, not the last write; see
            // the guard below.
            cpuNowSnap = parent->lastSyncCycle_;
            // Timeline break check FIRST: `parent->ayEvents_` was emptied
            // at the moment of the break, so everything still in it now
            // belongs to the new timeline and must survive the purge.
            if (parent->ayQueueGen_ != lastSeenQueueGen) {
                lastSeenQueueGen = parent->ayQueueGen_;
                pending.clear();
                timelineBroke = true;
            }
            // Take the emuCycles-stamped register writes under the same
            // lock, APPENDING to whatever the last callback could not
            // render yet. See the replay loop below.
            pending.insert(pending.end(),
                           parent->ayEvents_.begin(), parent->ayEvents_.end());
            parent->ayEvents_.clear();
            latestEventCycle = parent->latestAyEventCycle_.load(
                std::memory_order_relaxed);
        }

        // ── emuCycles replay of AY register writes ────────────────────
        // `liveRegs` is the audio thread's OWN register bank: it starts
        // from the CPU-side snapshot on the first buffer, then advances
        // ONLY by replaying stamped events at their exact sample offset.
        // Before this, the once-per-buffer snapshot collapsed every write
        // inside the window to its last value, so an arpeggio written at
        // ~1 ms intervals came out quantised to the ~10 ms buffer (notes
        // merged or dropped). A PB2 reset still resyncs the whole bank
        // from the CPU side, since that path zeroes registers wholesale.
        if (!regsPrimed || timelineBroke) {
            // A break is a genuine discontinuity, so the live CPU-side
            // bank — not the dropped backlog — is the truth at the new
            // anchor point. Zeroing the cursor hands the re-anchor below
            // its `starved` branch, which is the same code path a
            // pause/resume or a disk-turbo burst already takes.
            std::memcpy(liveRegs, regSnap, sizeof(liveRegs));
            regsPrimed = true;
            if (timelineBroke) {
                for (int ci = 0; ci < 2; ++ci) chip[ci].resetGenerators();
                audioCursor = 0;
                cursorFrac  = 0.0;
            }
        }
        // LIVE clock, not the NTSC constant: under PAL the CPU produces
        // 1 015 625 cycles/s, and a cursor advancing 0.7 % faster than the
        // producer outruns every queued event — they would all land on the
        // first sample of the buffer, undoing the sub-buffer timing on the
        // PAL demos that need it most.
        const double cyclesPerSample =
            cpuClockHz.load(std::memory_order_relaxed) / static_cast<double>(sr);
        [[maybe_unused]] const uint64_t bufferCycles =
            static_cast<uint64_t>(cyclesPerSample * frameCount);

        // ── Cursor pacing: a JITTER BUFFER, not a chase ───────────────
        // The producer and the consumer move the same number of cycles
        // per second by construction (17045 cycles x 60 Hz = 44100
        // samples x 23.19 cycles), but NOT at the same granularity: the
        // CPU worker publishes one video frame's worth of writes in a
        // single burst every ~16.7 ms, while one audio callback only ever
        // covers `bufferCycles` (256 frames = 5937 cycles at 44.1 kHz).
        // A burst therefore lands ~3 callbacks' worth of future writes in
        // the queue at once.
        //
        // Until 2026-08-01 the loop drained the entire queue every
        // callback, replayed whatever fitted inside this buffer's cycle
        // span, dumped ALL the rest at the buffer edge, and then set
        // `audioCursor = pending.back().cycle` — parking the cursor ON
        // the newest event, i.e. at zero lag. Consequences, measured on a
        // producer emitting 1 write per 1000 cycles: ~90 % of writes
        // collapsed to the buffer edge, and because the tail dump applies
        // them in order to the same register bank, only the LAST value
        // written to each register in the burst survived. Volume-register
        // PWM (Digidream 2's channel-A "SID voice" — 54 % of its register
        // traffic) and digi playback were destroyed outright; the cursor
        // then over-ran the next burst and tripped the backward re-anchor
        // roughly every third buffer, re-quantising the timeline.
        //
        // The fix is to run the cursor deliberately BEHIND the newest
        // event, so there is always a backlog to render in time order,
        // and to keep un-rendered events queued instead of dumping them.
        //
        // SIZING THE LAG (got this wrong on the first attempt, 2026-08-01,
        // and Digidream 1 exposed it as tempo glitches). Between bursts
        // `latestEventCycle` is frozen while the cursor free-runs, so the
        // lag falls by one full burst every producer tick and then jumps
        // back up when the next burst lands. A target of ONE burst
        // therefore makes the lag oscillate between one burst and ZERO —
        // it sits exactly on the re-anchor threshold, and any scheduling
        // jitter trips a resync every frame. The target has to be TWO
        // bursts so the lag oscillates in [1, 2] bursts and the minimum
        // stays at the one burst that spreading a burst actually needs.
        //
        // `kBurst` is one PAL video frame — the slower of the two refresh
        // rates, hence the safe bound. Cost: ~40 ms of added latency on
        // this card, which is inaudible for a music/SFX peripheral and far
        // cheaper than a resync.
        const uint64_t burst = static_cast<uint64_t>(
            cpuClockHz.load(std::memory_order_relaxed) / 50.0);
        const uint64_t targetLag = 2 * burst;
        // TWO preconditions, both learned the hard way (2026-08-01):
        //
        //   `!pending.empty()` — re-anchoring is only meaningful when there
        //   is a backlog whose placement the cursor decides. With an IDLE
        //   producer (card plugged, nothing driving it; a gap between
        //   patterns; a disk load) `latestEventCycle` freezes while the
        //   cursor keeps free-running, so `caughtUp` goes true and STAYS
        //   true — it fired on every single callback, slamming the cursor
        //   backwards each time. Harmless while there is nothing to render,
        //   but the moment writes resume the cursor is mis-anchored and the
        //   resumed events go out in a lump. That is the mechanism behind
        //   the tempo glitches reported on Digidream 1, whose music has
        //   exactly those gaps. A synthetic harness with an unbroken write
        //   stream never reproduces it, which is why the first version of
        //   this guard measured clean and shipped broken.
        //
        //   `latestEventCycle > targetLag` — without it, `desired` fell
        //   back to 0 during the first ~40 ms of a session (and forever, if
        //   the software only ever pokes the AY once), so the "re-anchor"
        //   was a slam to cycle 0 rather than to a sane lag.
        // Pace against CPU-NOW, never against the last WRITE. This is the
        // Digidream 1 bug, and it is worth stating precisely because the
        // difference looks cosmetic and is not.
        //
        // A music driver writes the AY in one dense clump per frame and
        // then leaves it alone: measured on DD1, the card is write-silent
        // for 88 % of every frame, with a worst inter-write gap of 17.6 ms.
        // If the guard measures lag as `latestAyEventCycle_ - audioCursor`,
        // that 17.6 ms of legitimate silence is charged against the budget
        // as if the cursor had caught up, because `latestAyEventCycle_`
        // stops moving while the cursor does not. Measured margin before
        // the guard falsely trips: 1076 cycles = **1.06 ms** on DD1, vs
        // 9.6 ms on DD2 (whose writes are 40x denser). Hence one demo
        // glitching and the other not.
        //
        // Each false trip is a real dropout: the cursor jumps back ~30 ms
        // and the register bank then freezes for 7-8 consecutive callbacks
        // = 40-46 ms, i.e. two frames of music. Under ordinary conditions
        // (host 0.5 % slow, or one dropped frame per 200 ms) it fired
        // 4-71 times per 25 s.
        //
        // `lastSyncCycle_` is the VIA's synced "now", advanced every
        // emulated instruction, so it keeps moving through write silence
        // and the guard measures what it actually means to measure.
        const uint64_t producerNow =
            (cpuNowSnap > latestEventCycle) ? cpuNowSnap : latestEventCycle;
        if (producerNow > targetLag) {
            const uint64_t desired = producerNow - targetLag;
            // Re-anchor only on gross error: starved (pause+resume, disk
            // turbo, first buffer) or genuinely run up onto the producer.
            // Steady state never trips either bound, so the cursor just
            // free-runs at cyclesPerSample and the timeline is continuous.
            const bool starved =
                producerNow > audioCursor &&
                (producerNow - audioCursor) > 5 * burst;
            const bool caughtUp = audioCursor + burst / 2 > producerNow;
            if (starved || caughtUp) {
                audioCursor = desired;
                cursorFrac  = 0.0;
            }
        }
        // Queue hygiene, and the reason there is no separate backward-jump
        // guard any more. Anything at or behind the cursor — events the
        // re-anchor above just skipped over — is folded into `liveRegs`
        // here as silent state catch-up, so the render loop's queue front
        // is always in the future.
        //
        // The first version of this code kept a second guard that yanked
        // the cursor BACKWARD whenever `pending.front()` sat behind it.
        // With a persistent queue that guard fired on the re-anchor above
        // rather than on a real rewind: forward snap, immediate backward
        // snap, repeat. The two fought every callback, which is what
        // Digidream 1's dense digidrum stream turned into audible tempo
        // instability.
        //
        // A rewind is NOT covered by either of them, and the note that
        // used to stand here claiming `caughtUp` handled it had the
        // reasoning inverted: `caughtUp` moves the CURSOR, while the
        // damage is in the QUEUE — a rolled-back cycleCounter leaves
        // `pending` full of events stamped in the pre-rewind future,
        // which the render loop's strict front-ordering then treats as
        // "not yet due" and blocks on for the whole rewind depth. That is
        // what `ayQueueGen_` / `timelineBroke` above exist for.
        while (!pending.empty() && pending.front().cycle <= audioCursor) {
            applyEvent(pending.front());
            pending.pop_front();
        }
        size_t nextEvent = 0;
        for (int ci = 0; ci < 2; ++ci) {
            // NEITHER counter drives this thread, for the same reason:
            // both are CPU-NOW counters while the cursor deliberately runs
            // ~40 ms behind CPU-now, so acting on one applies its effect
            // 40 ms EARLY.
            //
            // For R13 that was measured at 202 spurious retriggers in 25 s
            // of Digidream 1 (its 103 R13 stores x 2 chips), moving 13.8 %
            // of the render's RMS. For the /RESET strobe it was worse than
            // early: the bank was wiped at CPU-now and the ~40 ms of
            // pre-reset writes still queued behind the cursor then
            // replayed straight back on top of the zeros, so a board the
            // driver had just silenced went on holding its last note
            // forever. Both now arrive as ordinary cycle-stamped events
            // (`kRegAyReset`, and the reg-13 case in `applyEvent`), which
            // also covers same-value stores because the producer queues an
            // event for every write regardless of value.
            //
            // Kept in sync so a later reader does not think they were
            // forgotten; `ayResetCount_` remains live as UI telemetry.
            chip[ci].lastSeenResetCount    = resetCountSnap[ci];
            chip[ci].lastSeenEnvWriteCount = envWriteCountSnap[ci];
        }

        // Base clock/8 ticks covered by one output sample. Tone, noise and
        // envelope all run off this single base tick (MAME allocates ONE
        // stream at `master_clock / 8`, `ay8910.cpp:1298`), so one figure
        // serves all three.
        //
        // Derived from the LIVE CPU clock, not the NTSC compile-time
        // constant: the AY's pin-22 CLOCK is wired to the slot's phase-0
        // line, so on a PAL machine the chip really does run at
        // 1 015 625 Hz. Synthesising PAL music at the NTSC rate put every
        // note 0.699 % sharp = 12.05 cents — small, but Digidream 2 and
        // the rest of the French Touch / DIX corpus are PAL-timed, which
        // is exactly the material this path exists for.
        const float ticksPerSample = static_cast<float>(
            cpuClockHz.load(std::memory_order_relaxed) / 8.0
            / static_cast<double>(sr));
        const float invTicksPerSample =
            (ticksPerSample > 0.0f) ? (1.0f / ticksPerSample) : 0.0f;

        dcL.setRate(sr);
        dcR.setRate(sr);

        if (isMuted) {
            silence();
            return;
        }

        // ── SSI263 speech (Sound II variant only) ─────────────────────
        // Render the speech chip's PCM into a temp buffer under the
        // parent mutex (fillAudio advances the chip's playback cursor,
        // which the CPU thread also touches on DURPHON writes) — same
        // pattern as EchoPlusCard::AudioSrc::fillAudioBuffer. Mixed into
        // the output below at unity (pre-volume) gain, matching the
        // EchoPlus loudness; the chip's own amplitude register scaling
        // stays inside Ssi263::fillAudio. Before 2026-06-12 nothing
        // called fillAudio here at all, so Sound II speech was silent
        // despite the register/IRQ emulation being complete.
        const float* speechBuf = nullptr;
        if (parent->ssi_) {
            if (speechScratch.size() < static_cast<size_t>(frameCount))
                speechScratch.resize(static_cast<size_t>(frameCount));
            std::fill_n(speechScratch.begin(), frameCount, 0.0f);
            std::lock_guard<std::mutex> lk(parent->mtx);
            parent->ssi_->fillAudio(speechScratch.data(), frameCount, sr);
            speechBuf = speechScratch.data();
        }

        for (int i = 0; i < frameCount; ++i) {
            // Apply every register write stamped at or before this
            // sample's CPU cycle — this is what gives sub-buffer timing.
            cursorFrac += cyclesPerSample;
            const uint64_t whole = static_cast<uint64_t>(cursorFrac);
            audioCursor += whole;
            cursorFrac  -= static_cast<double>(whole);
            while (nextEvent < pending.size() &&
                   pending[nextEvent].cycle <= audioCursor) {
                applyEvent(pending[nextEvent++]);
            }
            // Per chip, NOT summed: chip 0 is the left channel and chip 1
            // the right one (MAME `a2mockingboard.cpp:161-165`).
            float chipOut[2] = { 0.0f, 0.0f };
            for (int ci = 0; ci < 2; ++ci) {
                ChipState& cs = chip[ci];
                const uint8_t* r = liveRegs[ci];

                // ── Box-integrate the mixer across this output sample ──
                // MAME renders the PSG on the chip's own clock/8 grid
                // (`ay8910.cpp:1298`) and hands the result to a decimating
                // resampler (`src/emu/resampler.cpp`). POM2 renders
                // straight to the device rate, so the decimation has to
                // happen right here: averaging the mixer output over the
                // ~2.9 base ticks each output sample spans is an exact
                // one-sample box filter.
                //
                // It buys two distinct things. It band-limits the square
                // waves — the old point-sampler folded every harmonic
                // above Nyquist straight back into the audible band,
                // measured at -11 dB of inharmonic energy on an ordinary
                // mid-register note, and at envelope periods below 2 it
                // skipped whole envelope steps without ever sampling
                // them. And, because the partial ticks at both ends of
                // the sample are weighted by their true duration, it
                // restores the sub-sample edge position that the old code
                // computed and then threw away. That second property is
                // what fixes CPU-driven volume-register PWM (Digidream
                // 2's channel-A "SID voice"), whose edges land on
                // arbitrary CPU cycles rather than on tick boundaries.
                chipOut[ci] = pom2::ay::renderChipSample(
                    cs, r, ticksPerSample, invTicksPerSample);
            }
            // ── Level + DC blocking ───────────────────────────────────
            // ONE AY x 3 channels x peak 1.0 = 3.0 per side, so `/3` is
            // the headroom-safe normalisation now that each channel
            // carries a single chip, and the mono fold-down below is the
            // `/6` the summed render used. The mix stays strictly LINEAR.
            //
            // An earlier pass on 2026-08-01 tried `/3` plus a `tanh` soft
            // knee, reasoning that single-AY software (the common case —
            // Digidream 2 never touches the second chip) was sitting 6 dB
            // down. That was a regression, and Digidream 1 showed it
            // immediately: DD1 drives BOTH AYs, so its 6-channel sum
            // routinely exceeds unity, and `tanh` then compressed and
            // intermodulated it continuously — timbres audibly wrong,
            // while single-AY DD2 never reached the nonlinearity and
            // sounded fine. Loudness is a knob the user already has; a
            // waveshaper across the whole mix is not something to spend
            // it on. The honest fix for single-AY level was the stereo
            // split this code now does, where each side carries one chip
            // and `/3` falls out naturally.
            const float dcOutL = dcL.process(chipOut[0] * (1.0f / 3.0f));
            const float dcOutR = dcR.process(chipOut[1] * (1.0f / 3.0f));
            // Speech is CENTRED — MAME routes the card's speech chip to
            // both channels at unity (`a2mockingboard.cpp:186-189`), and
            // there is one SSI263 on a Sound II, not one per side.
            const float ssi = speechBuf ? speechBuf[i] : 0.0f;
            if (right) {
                left[i]  = (dcOutL + ssi) * vol;
                right[i] = (dcOutR + ssi) * vol;
            } else {
                left[i] = (0.5f * (dcOutL + dcOutR) + ssi) * vol;
            }
        }

        // Drop only what was actually rendered. Everything still queued
        // is stamped past this buffer's cycle span and belongs to a LATER
        // buffer — it stays put and gets replayed at its true sample
        // offset next time round. (Until 2026-08-01 the leftovers were
        // instead applied in bulk right here, which meant only the last
        // value written to each register in a burst survived, and the
        // cursor was then yanked to `pending.back().cycle`. See the
        // jitter-buffer note above.)
        pending.erase(pending.begin(),
                      pending.begin() + static_cast<ptrdiff_t>(nextEvent));
        // Bound the queue: if the audio thread stops consuming (device
        // closed, buffer starvation) the CPU side would otherwise grow it
        // without limit. kMaxAyEvents is the same bound the producer uses.
        if (pending.size() > kMaxAyEvents) {
            const size_t drop = pending.size() - kMaxAyEvents;
            for (size_t k = 0; k < drop; ++k) applyEvent(pending[k]);
            pending.erase(pending.begin(),
                          pending.begin() + static_cast<ptrdiff_t>(drop));
        }
    }
};

// ─── MockingboardCard ─────────────────────────────────────────────────────

MockingboardCard::MockingboardCard(int slotNum, Variant variant)
    : slot_(slotNum), variant_(variant)
{
    via_[0] = std::make_unique<Via6522>();
    via_[1] = std::make_unique<Via6522>();
    ay_[0]  = std::make_unique<Ay3_8910>();
    ay_[1]  = std::make_unique<Ay3_8910>();
    if (variant_ == Variant::SoundII) {
        ssi_ = std::make_unique<pom2::Ssi263>();
    }
    audio_  = std::make_unique<AudioSrc>(this);
    onReset();
}

MockingboardCard::~MockingboardCard() = default;

void MockingboardCard::onUnplug()
{
    // SlotBus::detachFromBus() auto-releases any pending IRQ line bit
    // before letting us go, so no explicit assertIrq(false) here.
}

// ─── Rewind / snapshot ──────────────────────────────────────────────────────
// VIA + AY register/timer state — the audible music state of the card. The
// SSI263 (Sound II) is captured too, including its phoneme playback cursor
// (Ssi263::appendSnapshot), and its A/!R→VIA1.CA1 IRQ latch is restored via
// the VIA's ifr. Blob is self-describing (magic + version + present mask) so
// a foreign card on this slot ignores it. Both Mockingboard and Phasor reuse
// Via6522/Ay3_8910::append/loadSnapshot.
void MockingboardCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    std::lock_guard<std::mutex> lk(mtx);
    // ver 2 since 2026-07-30: the VIA sections gained a byte (portAIn).
    putU8(out, 'M'); putU8(out, 'B'); putU8(out, 'S'); putU8(out, 2);  // magic + ver
    putU8(out, static_cast<uint8_t>(variant_));
    uint8_t present = 0;
    if (via_[0]) present |= 0x01;
    if (via_[1]) present |= 0x02;
    if (ay_[0])  present |= 0x04;
    if (ay_[1])  present |= 0x08;
    if (ssi_)    present |= 0x10;
    putU8(out, present);
    if (via_[0]) via_[0]->appendSnapshot(out);
    if (via_[1]) via_[1]->appendSnapshot(out);
    if (ay_[0])  ay_[0]->appendSnapshot(out);
    if (ay_[1])  ay_[1]->appendSnapshot(out);
    if (ssi_)    ssi_->appendSnapshot(out);
}

void MockingboardCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    std::lock_guard<std::mutex> lk(mtx);
    pom2::byteio::Reader r(data, len);
    if (!r.has(6)) return;
    if (r.u8() != 'M' || r.u8() != 'B' || r.u8() != 'S') return;
    // v1 blobs carry 24-byte VIA sections, v2 carry 25 (portAIn added
    // 2026-07-30 for the AY read-bus latch). The layout change must bump
    // this byte, or an old snapshot's every later field shifts by one.
    const uint8_t blobVer = r.u8();
    if (blobVer != 1 && blobVer != 2) return;
    const std::size_t viaBytes = (blobVer >= 2)
        ? pom2::Via6522::kSnapshotBytes : pom2::Via6522::kSnapshotBytesV1;
    (void)r.u8();                       // variant — informational
    const uint8_t present = r.u8();
    const std::size_t required = 6 +
        ((present & 0x01) ? viaBytes : 0) +
        ((present & 0x02) ? viaBytes : 0) +
        ((present & 0x04) ? pom2::Ay3_8910::kSnapshotBytes : 0) +
        ((present & 0x08) ? pom2::Ay3_8910::kSnapshotBytes : 0) +
        ((present & 0x10) ? pom2::Ssi263::kSnapshotBytes : 0);
    if (len < required) return;          // validate before mutating any chip
    auto loadVia = [&](std::unique_ptr<pom2::Via6522>& v) -> bool {
        if (v) v->loadSnapshot(r.p + r.pos, viaBytes);
        r.pos += viaBytes;
        return true;
    };
    auto loadAy = [&](std::unique_ptr<pom2::Ay3_8910>& a) -> bool {
        if (a) a->loadSnapshot(r.p + r.pos);
        r.pos += pom2::Ay3_8910::kSnapshotBytes;
        return true;
    };
    auto loadSsi = [&]() -> bool {
        if (ssi_) ssi_->loadSnapshot(r.p + r.pos);
        r.pos += pom2::Ssi263::kSnapshotBytes;
        return true;
    };
    // A restore re-anchors the machine at a cycle that is almost always in
    // the PAST of whatever is queued (rewind, snapshot load —
    // `MachineSnapshot.cpp` sets the cycle counter in the same pass that
    // calls us). Those stamps describe a timeline the machine has left, so
    // both queues go. Without this the audio thread's `pending` front sits
    // in the pre-restore future, the render loop's strict front-ordering
    // blocks on it, and the card is SILENT for the whole rewind depth.
    invalidateAyTimeline();
    ayResetHeld_[0] = ayResetHeld_[1] = false;
    if ((present & 0x01) && !loadVia(via_[0])) return;
    if ((present & 0x02) && !loadVia(via_[1])) return;
    if ((present & 0x04) && !loadAy(ay_[0]))   return;
    if ((present & 0x08) && !loadAy(ay_[1]))   return;
    if ((present & 0x10) && !loadSsi())        return;
}

void MockingboardCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx);
    via_[0]->reset();
    via_[1]->reset();
    ay_[0]->reset();
    ay_[1]->reset();
    if (ssi_) ssi_->reset();
    assertIrq(false);
    // Re-anchor the lazy-sync clock to "now" so a freshly reset card
    // doesn't run a giant catch-up on its first MMIO access.
    lastSyncCycle_ = cpu_ ? cpu_->getCycleCountNow() : 0;
    viaWriteCount_[0] = viaWriteCount_[1] = 0;
    ayWriteCount_[0]  = ayWriteCount_[1]  = 0;
    ayEnvWriteCount_[0] = ayEnvWriteCount_[1] = 0;
    ayResetHeld_[0] = ayResetHeld_[1] = false;
    // Drop any cycle-stamped writes queued before the reset — replaying
    // them afterwards would resurrect the pre-reset tone. Clearing only
    // this side was not enough: the audio thread's own `pending` backlog
    // (the deliberate ~40 ms of replay lag) still held pre-reset stamps
    // and replayed them straight over the zeroed bank, so F12 / a profile
    // switch left the card droning its last note. `invalidateAyTimeline`
    // purges BOTH sides.
    invalidateAyTimeline();
    // BUMP, don't zero. The audio thread resyncs its own register bank
    // only when this counter CHANGES; zeroing it was a no-op whenever it
    // was already 0 (a driver that never strobes PB2 low), so F12 / cold
    // boot / profile switch zeroed the CPU-side AY banks while the audio
    // thread kept the old tone — the card droned on forever. Before the
    // emuCycles queue the per-buffer snapshot masked this.
    ++ayResetCount_[0];
    ++ayResetCount_[1];
}

AudioSource* MockingboardCard::audioSource()
{
    return audio_.get();
}

void MockingboardCard::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = kAudioSampleRate;
    audio_->sampleRate.store(hz, std::memory_order_relaxed);
}

void MockingboardCard::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    audio_->volume.store(v, std::memory_order_relaxed);
}
float MockingboardCard::getVolume() const
{
    return audio_->volume.load(std::memory_order_relaxed);
}
void MockingboardCard::setCpuClock(double hz)
{
    if (hz > 0.0 && audio_) audio_->cpuClockHz.store(hz, std::memory_order_relaxed);
}

void MockingboardCard::setMuted(bool m)
{
    audio_->muted.store(m, std::memory_order_relaxed);
}
bool MockingboardCard::isMuted() const
{
    return audio_->muted.load(std::memory_order_relaxed);
}

bool MockingboardCard::snapshotSsi263(Ssi263Snap* out) const
{
    if (!ssi_ || !out) return false;
    std::lock_guard<std::mutex> lk(mtx);
    for (int r = 0; r <= pom2::Ssi263::REG_FILFREQ; ++r) {
        out->regs[r] = ssi_->peekRegister(static_cast<uint8_t>(r));
    }
    out->currentPhoneme         = ssi_->currentPhoneme();
    out->mode                   = static_cast<uint8_t>(ssi_->currentMode());
    out->aRequest               = ssi_->aRequest();
    out->powerDown              = ssi_->powerDown();
    out->irqEnabled             = ssi_->irqEnabled();
    out->phonemeRemainingCycles = ssi_->phonemeRemainingCycles();
    out->phonemeWriteCount      = ssi_->phonemeWriteCount();
    return true;
}

// Lazy timer catch-up. The Mockingboard's 6522 VIA T1/T2 counters tick
// once per CPU cycle on real hardware. POM2's host loop advances slot
// peripherals in batches at the end of each CPU run-slice (~17 045
// cycles in default mode), which is fine for steady-state music but
// breaks the tight write-T1-then-read-IFR sequences detection routines
// rely on (Nox Archaist, Skyfox, Broadside — see CLAUDE.md). Sync the
// VIAs to "now" before every MMIO access so the IFR the routine reads
// reflects the cycles that have actually elapsed since its T1 write.
// Caller must hold `mtx`.
void MockingboardCard::syncToCpuCycle()
{
    if (!cpu_) return;
    // +1: the bus access happens on the FINAL (data) cycle of the
    // instruction, but `getCycleCountNow()` = cycleCounter + cpu.cycles and
    // cpu.cycles does NOT yet count that in-flight data cycle (e.g. `SBC
    // $C404`, a 4-cycle abs read, calls memRead with cpu.cycles == 3). A real
    // 6522 has already decremented T1/T2 on the φ2 of that cycle, so syncing
    // to the pre-cycle count samples the timers one cycle early — every
    // Mockingboard MMIO read/write saw its VIA counter one too high.
    //
    // Invisible to steady-state music and to write-then-read detection idioms
    // (Nox Archaist / Skyfox): those read RELATIVE to their own arm, so the
    // +1 on the read and the +1 on the arm write cancel. It only moves reads
    // taken relative to a FREE-RUNNING underflow — the stable-raster T1-IRQ
    // dispatch French Touch's OLDSKOOL FORT ET VERT does: its handler reads
    // $C404 to phase-measure the beam and self-modifies a BVC by
    // `mem[$03] - T1CL - $19`. One-too-high T1CL made that phase wrap
    // $00 -> $FF, jumping the dispatch into a slice that RTS'd off the 3-byte
    // IRQ frame -> SP=0 -> crash (~10 s in). TRIBU's raster is unaffected: it
    // re-arms every link (closed loop), so a constant read+write offset
    // cancels and the 5-link/frame chain stays drift-free (via_t1_rearm_chain,
    // dix_menu_raster_probe). Pinned by mockingboard_t1_irq_phase.
    syncToCpuCycleAt(cpu_->getCycleCountNow() + 1);
}

// Advance the VIAs to an explicit absolute CPU cycle. Split out from
// syncToCpuCycle() so the end-of-step batch path can pass the CORRECTED
// "now": at that point Memory::advanceCycles has already folded the slice
// into cycleCounter while cpu->cycles still holds it, so getCycleCountNow()
// overshoots by one instruction (see advanceCycles).
void MockingboardCard::syncToCpuCycleAt(uint64_t now)
{
    if (now <= lastSyncCycle_) {
        // A SMALL step back is routine: `advanceCycles` corrects "now" by
        // one instruction (see its comment), so any MMIO access that
        // already synced later in the same instruction lands ahead of it.
        // A LARGE one is the cycle counter being rolled back under us —
        // rewind, or `MachineSnapshot`'s `mem.setCycleCounter`. The
        // threshold only has to clear one instruction (7 cycles at most);
        // 1024 leaves three orders of magnitude of margin below the
        // shallowest meaningful rewind (one video frame, 17045 cycles).
        //
        // The card's own `loadSnapshotState` covers the restore path when
        // the card is in the blob; this covers the rest (a bare
        // `setCycleCounter`, a card plugged after the snapshot was taken).
        constexpr uint64_t kTimelineBreakCycles = 1024;
        if (lastSyncCycle_ - now > kTimelineBreakCycles) invalidateAyTimeline();
        lastSyncCycle_ = now;
        return;
    }
    const uint64_t delta = now - lastSyncCycle_;
    // The VIAs' `advance()` takes an int; clamp to a sane upper bound
    // (a single CPU run-slice is ~17 045 cycles, so anything beyond a
    // few million here means our sync clock got desynchronised).
    const int step = (delta > 0x7FFFFFFFu) ? 0x7FFFFFFF
                                           : static_cast<int>(delta);
    via_[0]->advance(step);
    via_[1]->advance(step);
    lastSyncCycle_ = now;
}

uint8_t MockingboardCard::slotRomRead(uint8_t low8)
{
    // Address decode:
    //   Variant::AC      — bit 7 selects VIA, bits 0..3 select register,
    //                      bits 4..6 are partial-decode mirrors.
    //   Variant::SoundII — READS are identical to AC: "Reads only select
    //                      the VIA" (MAME `a2mockingboard.cpp:346-349`;
    //                      the base `a2bus_ayboard_device::read_cnxx`
    //                      routes the whole $00-$7F page to VIA1). The
    //                      SSI263 is write-only on the bus — a driver
    //                      polling VIA1 IFR through a $4x partial-decode
    //                      mirror (LDA $Cs4D) must reach the VIA, not the
    //                      speech chip. An earlier revision routed $40-$4F
    //                      reads to the SSI263, breaking exactly that
    //                      idiom. Writes shadow into BOTH chips — see
    //                      slotRomWrite.
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // make T1/T2/IFR cycle-accurate at "now"
    const int chip = (low8 & 0x80) ? 1 : 0;
    const uint8_t out = via_[chip]->read(low8 & 0x0F);
    updateIrq();          // T1CL clears IFR.T1, may drop IRQ
    return out;
}

void MockingboardCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // T1 counters reflect "now" before T1CH reload
    if (ssi_ && (low8 & 0xF0) == 0x40) {
        // SSI263 register write ($40-$4F, regs 0-7 from the low 3 bits).
        // Per MAME `a2mockingboard.cpp:346-349` "Cn40 will write to both
        // the VIA and the first SSI-263" — the write SHADOWS into VIA1's
        // reg 0-15 mirror as well (fall through below), it does not
        // replace it. The chip's own write() acks A/!R internally for
        // $00..$02 — the host CPU clears the VIA's IFR.CA1 separately
        // (typical Mockingboard Sound II driver writes the CA1 bit to
        // IFR after each phoneme).
        ssi_->write(low8 & 0x07, v);
    }
    const int chip = (low8 & 0x80) ? 1 : 0;
    ++viaWriteCount_[chip];
    const uint8_t events = via_[chip]->write(low8 & 0x0F, v);
    if (events) onViaPortBChange(chip);
    updateIrq();
}

uint32_t MockingboardCard::getAyCommandCount(int chip, int cmd) const
{
    if (chip < 0 || chip > 1 || cmd < 0 || cmd > 3) return 0;
    const auto& ay = *ay_[chip];
    switch (cmd) {
        case 0: return ay.inactiveCount;
        case 1: return ay.readStrobeCount;
        case 2: return ay.writeStrobeCount;
        case 3: return ay.latchCount;
    }
    return 0;
}

void MockingboardCard::onViaPortBChange(int chip)
{
    // Marshal the VIA's current Port A / Port B output to the AY.
    // Note this fires for *both* PA and PB changes (events bit 0/1) —
    // PA-only changes also matter because LATCH/WRITE strobes bring PA
    // (the AY data bus) to the chip just before / just after PB sets
    // BDIR/BC1. We reapply on either edge so the order doesn't matter.
    // The AY data bus is the VIA's PA *pins*, not its output latch: a bit
    // whose DDRA says "input" is undriven and the pull-ups take it to 1.
    // MAME `via6522.cpp output_pa()` hands its PA handler
    // `(m_out_a & m_ddr_a) | ~m_ddr_a`, and `a2bus_ayboard_device` latches
    // exactly that byte into `m_porta1` — the value `update_ay1()` then
    // gives the chip. `Via6522::readPortA()` composes the same thing, with
    // `portAIn` (0xFF at rest, the AY's own bus value after a READ strobe)
    // standing in for MAME's re-latched `m_porta`. Masking with ddrA alone
    // fed the AY zeros for every undriven bit — so a driver that latches a
    // register address and then flips DDRA to 0 to read it back (Ultima IV's
    // Echo+ probe idiom) re-strobed register $00 instead of the one it
    // latched.
    const uint8_t pa = via_[chip]->readPortA();
    const uint8_t pb = via_[chip]->portBOut & via_[chip]->ddrB;
    const auto res = ay_[chip]->applyControl(pa, pb);
    // Any result other than ResetOnly means PB2 (AY /RESET) is high, so
    // the strobe's falling edge can arm again.
    if (res != Ay3_8910::ApplyResult::ResetOnly) ayResetHeld_[chip] = false;
    if (res == Ay3_8910::ApplyResult::Wrote) {
        ++ayWriteCount_[chip];
        // emuCycles stamp for the audio thread: replay this store at its
        // exact sample offset instead of letting the once-per-buffer
        // register snapshot collapse every write in the window to the
        // last one. `lastSyncCycle_` is the VIA's synced "now" (the MMIO
        // path calls syncToCpuCycle() before touching state), so it is
        // the same clock SpeakerDevice's queue speaks.
        const uint8_t reg = static_cast<uint8_t>(ay_[chip]->latchedAddr & 0x0F);
        queueAyEvent(chip, reg, ay_[chip]->regs[reg]);
        // R13 (envelope shape) restarts the envelope generator on EVERY
        // write, even when the value is unchanged. Surface same-value R13
        // stores to the audio thread (which only sees the register snapshot)
        // as a monotonic counter, mirroring the ayResetCount_ pattern.
        if ((ay_[chip]->latchedAddr & 0x0F) == 13) ++ayEnvWriteCount_[chip];
    } else if (res == Ay3_8910::ApplyResult::Read) {
        // MAME `mockingboard.cpp via_psg_ctrl`: on a READ command the AY
        // drives the selected register onto the data bus and the card
        // latches it onto VIA port A (MAME's `m_porta` shadow). Latched
        // until the next port-A write, so a driver's presence check
        // (write a register, read it back) actually sees the chip.
        via_[chip]->setPortAInput(ay_[chip]->busOut);
    } else if (res == Ay3_8910::ApplyResult::ResetOnly) {
        ++ayResetCount_[chip];              // UI telemetry, every strobe
        // Stamp only the FALLING edge. `applyControl` reports ResetOnly on
        // every VIA write while PB2 is held low, and holding it low across
        // a run of writes is legal, so an un-gated push would flood the
        // queue with duplicates of an idempotent event.
        if (!ayResetHeld_[chip]) {
            ayResetHeld_[chip] = true;
            queueAyEvent(chip, kRegAyReset, 0);
        }
    }
}

// Queue one cycle-stamped AY event for the audio thread. Caller holds mtx.
void MockingboardCard::queueAyEvent(int chip, uint8_t reg, uint8_t val)
{
    // Overflow (audio device stalled or absent): break the timeline
    // instead of evicting the oldest event. Eviction silently diverged
    // the CPU-side and audio-side banks for the rest of the session —
    // nothing else ever re-seeds `liveRegs`, so one lost write persisted
    // forever — and dropping the CPU queue alone left the audio thread's
    // own backlog to replay stale values on top of the resync.
    if (ayEvents_.size() >= kMaxAyEvents) invalidateAyTimeline();
    ayEvents_.push_back(AyRegEvent{ lastSyncCycle_,
                                    static_cast<uint8_t>(chip), reg, val });
    latestAyEventCycle_.store(lastSyncCycle_, std::memory_order_relaxed);
}

// Declare the cycle-stamped event stream discontinuous. Caller holds mtx.
void MockingboardCard::invalidateAyTimeline()
{
    ayEvents_.clear();
    // `latestAyEventCycle_` is half of the cursor's `producerNow`. Leaving
    // a pre-rewind value in it would hold `producerNow` at the OLD
    // timeline's high-water mark, so the re-anchor would refuse to bring
    // the cursor back with the machine. Zero lets `lastSyncCycle_` — which
    // the rollback already dragged down — speak for itself.
    latestAyEventCycle_.store(0, std::memory_order_relaxed);
    ++ayQueueGen_;
}

void MockingboardCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    std::lock_guard<std::mutex> lk(mtx);
    // Tick the SSI263 (Sound II only) and surface A/!R end-of-phoneme
    // to VIA1.CA1 — real card wires SSI263.A/!R inverted into CA1, so
    // a 0→1 of A/!R = negative edge on CA1, matching PCR.0 == 0 (the
    // default config used by stock Sound II drivers).
    if (ssi_ && ssi_->advance(cycles)) {
        via_[0]->setCa1NegativeEdge();
    }
    if (cpu_) {
        // Lazy-sync path: any cycles already accounted for via MMIO accesses
        // during this slice were advanced by syncToCpuCycle() already; catch
        // up the remainder so end-of-slice IRQ state is published.
        //
        // Memory::advanceCycles ran `cycleCounter += cycles` BEFORE dispatching
        // to us, yet M6502::step() hasn't cleared cpu->cycles (still == this
        // `cycles`). So getCycleCountNow() == cycleCounter + cpu->cycles
        // overshoots the true "now" by exactly `cycles`. Subtract them, or the
        // VIAs jump one instruction ahead and the next mid-instruction MMIO
        // read hits the `now <= lastSyncCycle_` early-out — losing cycle
        // accuracy precisely where the lazy-sync was meant to provide it.
        syncToCpuCycleAt(cpu_->getCycleCountNow() - static_cast<uint64_t>(cycles));
    } else {
        // No CPU back-pointer (unit-test harness — see
        // mockingboard_smoke_test.cpp). Fall back to the legacy
        // batched advance so existing tests keep their semantics.
        via_[0]->advance(cycles);
        via_[1]->advance(cycles);
    }
    updateIrq();
}

void MockingboardCard::updateIrq()
{
    const bool combined = via_[0]->irqOut() || via_[1]->irqOut();
    // `assertIrq()` debounces against the base-class cache and fans out
    // through the SlotBus IRQ router (installed by Memory::setCpu).
    // Wire-OR semantics on the CPU side mean another card on a different
    // slot stays asserted even when this one releases — see
    // M6502::setIrqLine().
    assertIrq(combined);
}

uint8_t MockingboardCard::getAyRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 1 || reg < 0 || reg >= kAyNumRegs) return 0;
    std::lock_guard<std::mutex> lk(mtx);
    return ay_[chip]->regs[reg];
}

uint8_t MockingboardCard::peekViaRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 1 || reg < 0 || reg > 15) return 0xFF;
    std::lock_guard<std::mutex> lk(mtx);
    // Read-only peek: replicate the read() switch but skip side effects.
    auto& v = *via_[chip];
    switch (reg & 0x0F) {
    case VIA_ORB:    return v.readPortB();
    case VIA_ORA:    return v.readPortA();
    case VIA_DDRB:   return v.ddrB;
    case VIA_DDRA:   return v.ddrA;
    case VIA_T1CL:   return static_cast<uint8_t>(v.t1Counter & 0xFF);
    case VIA_T1CH:   return static_cast<uint8_t>((v.t1Counter >> 8) & 0xFF);
    case VIA_T1LL:   return static_cast<uint8_t>(v.t1Latch & 0xFF);
    case VIA_T1LH:   return static_cast<uint8_t>((v.t1Latch >> 8) & 0xFF);
    case VIA_T2CL:   return static_cast<uint8_t>(v.t2Counter & 0xFF);
    case VIA_T2CH:   return static_cast<uint8_t>((v.t2Counter >> 8) & 0xFF);
    case VIA_SR:     return v.sr;
    case VIA_ACR:    return v.acr;
    case VIA_PCR:    return v.pcr;
    case VIA_IFR:    return v.computedIfr();
    case VIA_IER:    return static_cast<uint8_t>(v.ier | 0x80);
    case VIA_ORANH:  return v.readPortA();
    default:         return 0xFF;
    }
}

// ── Note on the parent mutex ─────────────────────────────────────────────
//
// The MockingboardCard's `mtx` is referenced from the AudioSrc inner class
// (declared in the header as `std::mutex` member of the card). It guards:
//   * the VIA register file (read/write/advance)
//   * the AY register banks (writes from the CPU side, snapshot reads from
//     the audio thread)
// The AudioSrc holds the lock only briefly (32-byte memcpy) per audio
// callback — it does NOT hold it during the synthesis loop. The CPU side
// holds it for the duration of one slotRomRead/Write or advanceCycles()
// call, which is bounded.
