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

// EmulationController_TimeTravel — the debugger's two backward verbs, Step
// Back and Run Back (2026-11-27), built on the rewind ring.
//
// A 6502 cannot run backwards, and POM2 does not log what every instruction
// overwrote. What it has is the rewind ring: the whole machine once per video
// frame. So "one instruction back" is: restore the newest frame before now,
// then re-execute forward to the instruction before this one. The replay is
// counted, not timed — run to the present counting instructions, restore
// again, run one fewer — so it lands on an instruction boundary whatever the
// instruction lengths are.
//
// THE REPLAY IS CHECKED. Before anything, the present is witnessed (the
// processor, the cycle, the 64 KiB the CPU sees: ReplayWitness); the replay,
// run from the frame to the present, must stand on that exactly. If it does
// not — a rewind frame does not hold everything that steers the machine —
// the verb is given up and the machine put back, from the frame captured on
// the way in. So what follows is why a replay is USUALLY exact; the check is
// why a wrong one is never landed on.
//
// What that rests on, and where it stops being exact:
//   * The replay is the machine's own code, so it reproduces a frame only
//     as far as the frame was DETERMINISTIC from its snapshot. Host input
//     is what breaks that — a key, a paddle, a mouse movement arriving
//     while the machine ran — so it is a replay barrier too (HostIoEpoch.h,
//     and hostIoSeen_ in the header for the rule): no replay is made across
//     one. And a frame is restored with the input it was captured with,
//     the keyboard included (keyboardAtFrame_). What is left unreproduced
//     is input no device reports: the 4play, SNES MAX and joyport contacts,
//     and a mouse movement banked by the MAME-model mouse card and still
//     draining into the next frame. [F] Settled by: those devices counting
//     their changes as the others do.
//   * No frame, no way back: the ring must be recording (Rewind panel) and
//     hold a frame older than now. A write a rewind cannot undo — block
//     device, 3.5", WOZ, printed output — empties the ring (CLAUDE.md), so a
//     step back never crosses one.
//   * The frames after the one replayed from are dropped, as a rewind "resume
//     here" drops them: the future is made again by running.
//
//   * A replay EXECUTES the guest's code again. Inside the machine that is
//     harmless — the landing restores the frame and replays only up to the
//     landing point. A byte the guest exchanged with the OUTSIDE is not: a
//     serial peer, a network or a MIDI port cannot be rewound. The cards
//     with a host side count their exchanges (HostIoEpoch.h) and no replay
//     crosses one: it is a replay barrier like a reset, and an exchange
//     made since the last captured frame closes the way back until the
//     machine has run on. A replay that reaches the outside all the same
//     (it left the original's path) is abandoned.
//
//   * An accelerator's speed (Zip Chip, TransWarp, FASTChip, the //c+) is
//     sampled by the worker every 4096 cycles and not at all by a replay,
//     which keeps the ratio its frame was captured with. A replay ACROSS a
//     change of speed would put the beam elsewhere than the original run
//     did, so none is made (owner's ruling, 2026-11-27): the cycle of the
//     change is a barrier (`replayBarriers_`), the history ends there for
//     both verbs, and their buttons grey out saying so. At a
//     constant speed, accelerated or not, the replay is exact.
//
// The debugger is told it is reading the past (`Debugger::setReplaying`): no
// pass is counted, and none of its hook's methods is called — the CPU is
// stepped directly — so a trace file, the profile and the 6502 OS-call
// traces see nothing of the replay; the cards' own trace lines (Disk II
// head, a card Z80's BDOS calls) are muted for its length
// (`OsCallTrace::setMuted`). The hook stays ATTACHED all the same, because
// `M6502::step` reads its presence: with a hook an interrupt entry is a step
// of its own, ending on the handler's first instruction; without one the
// entry and that instruction are a single step, and a breakpoint on a
// handler's first instruction — the usual place for one — would sit inside
// a step where no replay could see it or land on it.

#include "EmulationController.h"

#include "Debugger.h"
#include "Block512Backing.h"
#include "HostIoEpoch.h"
#include "Logger.h"
#include "SlotPeripheral.h"

#include <chrono>
#include <mutex>

namespace {
/// A replay is bounded twice. It stops when the machine no longer advances
/// (a halted CPU), and at this many instructions — a hundred emulated
/// seconds of them — so that a replay that lost its way cannot spin for ever
/// under the state lock. Either is a failure, reported as one.
constexpr long kReplayInstructionCap = 50'000'000;
}

// Everything a replay must not touch, as one number: the media-write epoch
// (a rewind may not cross a write to a file) and the host-I/O epoch (bytes
// to or from a peer). Either moving during a replay means it left the path
// the original run took.
uint64_t EmulationController::outsideEpoch() const
{
    return pom2::mediaWriteEpoch().load(std::memory_order_acquire) +
           pom2::hostIoEpoch().load(std::memory_order_relaxed);
}

void EmulationController::replayOne()
{
    if (SlotPeripheral* dma = mem.slotBus().dmaClaimant()) {
        debugger_->runAsBusMaster([&] { return dma->dmaRun(1); });
    } else {
        (void)processor.takeInterruptEntered();
        processor.step();
    }
}

bool EmulationController::cardHoldsHostConnection() const
{
    for (int s = 0; s < SlotBus::kSlotCount; ++s)
        if (const SlotPeripheral* card = mem.slotBus().peripheral(s))
            if (card->holdsHostConnection()) return true;
    return false;
}

void EmulationController::rebaseReplayBarriers(uint64_t cycle)
{
    // The machine was set back to a frame at `cycle` and its future dropped
    // (Rewind panel ▸ Play): the barriers of that future are gone with it —
    // left in, addReplayBarrier (which only appends) swallowed every new one
    // until the cycle counter caught up. And this frame resumes with the
    // LIVE keyboard and pointer, not the ones it recorded: no replay starts
    // on it.
    while (!replayBarriers_.empty() && replayBarriers_.back() > cycle) replayBarriers_.pop_back();
    addReplayBarrier(cycle + 1);
}

void EmulationController::addReplayBarrier(uint64_t cycle)
{
    // Ascending, no duplicates. An event behind the newest barrier cannot
    // be recorded later than it happened: the list only grows at its end.
    // With no frame to bind (Rec off — the default — or an empty ring) only
    // the newest barrier can ever matter: every frame captured later stands
    // after it, and re-enabling drops the old frames. Without this the list
    // grew by one entry per reset, speed change or resume after input, for
    // as long as the session ran with Rec off.
    if (rewind_.empty() || !rewind_.enabled()) replayBarriers_.clear();
    if (replayBarriers_.empty() || replayBarriers_.back() < cycle) replayBarriers_.push_back(cycle);
}

uint64_t EmulationController::replayBarrierFor(uint64_t present) const
{
    for (size_t i = replayBarriers_.size(); i-- > 0;)
        if (replayBarriers_[i] <= present + 1) return replayBarriers_[i];
    return 0;
}

void EmulationController::noteCaptureForReplay()
{
    const uint64_t now = mem.getCycleCounter();
    bool onThisFrame = false;
    // Barriers the machine is not past: time went back (a scrub resumed, a
    // counter restarted), or an event on this very cycle — the frame about
    // to be captured comes AFTER it and replaces the one captured before.
    while (!replayBarriers_.empty() && replayBarriers_.back() > now) {
        replayBarriers_.pop_back();
        onThisFrame = true;
    }
    // Bytes exchanged with a peer, or input, during the frame just run.
    if (const uint64_t io = pom2::hostIoEpoch().load(std::memory_order_relaxed); io != hostIoSeen_) {
        hostIoSeen_ = io;
        onThisFrame = true;
    }
    if (onThisFrame) addReplayBarrier(now);
    // Nothing older than the ring can bind a frame of it.
    if (!rewind_.empty()) {
        const uint64_t oldest = rewind_.oldestCycle();
        size_t drop = 0;
        while (drop < replayBarriers_.size() && replayBarriers_[drop] <= oldest) ++drop;
        replayBarriers_.erase(replayBarriers_.begin(), replayBarriers_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

size_t EmulationController::rewindFrameBefore(uint64_t cycle) const
{
    const uint64_t barrier = replayBarrierFor(cycle);
    for (size_t i = rewind_.size(); i-- > 0;) {
        const uint64_t stamp = rewind_.infoAt(i).cycle;
        if (stamp >= cycle) continue;
        // The newest frame before `cycle` — a start only on this side of
        // the barrier that binds it (older frames are further still).
        return stamp >= barrier ? i : pom2::RewindBuffer::kNoFrame;
    }
    return pom2::RewindBuffer::kNoFrame;
}

bool EmulationController::debugCanStepBack(const char** why) const
{
    const char* reason = nullptr;
    if (mode.load() != Mode::Stopped)
        reason = "The machine is running: stop it first.";
    else if (inputJournalMode() != InputJournalMode::Off)
        reason = "Host input is being recorded or played back: going back would\n"
                 "leave the recording. Stop it first.";
    else if (!rewind_.enabled())
        reason = "The rewind is not recording: tick Rec here (or Record in the\n"
                 "Rewind panel), then run - going back needs its history.";
    else if (scrubIndex_.load() != pom2::RewindBuffer::kNoFrame)
        // The Rewind panel's cursor stands on a frame of the past, with the
        // frames after it still in the ring: a replay from here would land
        // in that future. Resuming (Play) makes this frame the present.
        reason = "The Rewind panel is scrubbing: press Play there (or leave the\n"
                 "scrub) first - Step Back goes back from the present.";
    else if (rewind_.empty())
        reason = "No history yet: the rewind has just started recording. Run on.";
    else if (pom2::mediaWriteEpoch().load(std::memory_order_acquire) != rewindMediaEpoch_)
        reason = "Something a rewind cannot undo happened (a disk image was written\n"
                 "or saved, or something was printed): the history ends here.";
    else if (hostIoAtStop_.load(std::memory_order_relaxed) > hostIoSeen_)
        reason = "Something reached the machine from outside while it ran, after the\n"
                 "last recorded frame - a key, a mouse or joystick movement, or bytes\n"
                 "exchanged with a serial peer, the network or a MIDI port.\n"
                 "Re-executing cannot reproduce it. Run on: the way back reopens.";
    else if (cardHoldsHostConnection())
        reason = "A card holds something of the host's that is not in the machine's\n"
                 "state - an open connection (Uthernet II), frames or bytes received\n"
                 "and not yet read (Uthernet I, Super Serial Card): going back would\n"
                 "drop it or read it early.";
    else if (mem.pendingPasteSize() != 0)
        reason = "A paste is being typed into the machine: going back would\n"
                 "swallow part of it. Wait for it to finish, or cancel it.";
    else if (tape && tape->isPlaybackActive())
        reason = "A cassette is playing: going back would leave the tape ahead\n"
                 "of the machine. Stop the deck first.";
    else if (rewindFrameBefore(mem.getCycleCounter()) == pom2::RewindBuffer::kNoFrame &&
             abandonedAt_ == mem.getCycleCounter())
        // The barrier is the one abandonReplay raised, here: say what really
        // happened (it used to read as a reset or a key, which it was not).
        reason = abandonedReachedOutside_
            ? "The last attempt to go back was given up: re-executing the history\n"
              "reached the outside (a disk write, or bytes for the host). The\n"
              "machine is as it was. Run on: the way back reopens."
            : "The last attempt to go back was given up: re-executing the history\n"
              "did not lead back exactly here - something the rewind does not\n"
              "record steered the machine (host time read by a clock card, a\n"
              "timer's phase...). The machine is as it was. Run on: the way back\n"
              "reopens.";
    else if (rewindFrameBefore(mem.getCycleCounter()) == pom2::RewindBuffer::kNoFrame)
        reason = replayBarrierFor(mem.getCycleCounter()) != 0 &&
                 rewind_.oldestCycle() < mem.getCycleCounter()
            ? "Something happened here that re-executing the frame before it would\n"
              "not reproduce: a reset, a change of an accelerator's speed, a key or\n"
              "a mouse / joystick movement, bytes exchanged with the outside (serial,\n"
              "network, MIDI), or a change you made by hand (a byte in the Memory\n"
              "viewer, the PC). No way back across it: run on, and the history\n"
              "starts again."
            : "No history before this point: this is as far back as the rewind\n"
              "goes (it keeps about 30 s, and restarts at a cold boot or a media\n"
              "write).";
    if (why) *why = reason;
    return reason == nullptr;
}

void EmulationController::beginReplay()
{
    processor.setDebugHook(debugger_.get());    // attached, never called: see the header
    debugger_->setReplaying(true);
    suspendDriveSounds(true);                   // no click burst from re-executed history
}

void EmulationController::suspendDriveSounds(bool on)
{
    if (floppy525) floppy525->setSuspended(on);
    if (floppy35)  floppy35->setSuspended(on);
}

bool EmulationController::replayTo(uint64_t cycle, long& count)
{
    while (mem.getCycleCounter() < cycle) {
        const uint64_t before = mem.getCycleCounter();
        replayOne();
        ++count;
        if (mem.getCycleCounter() == before || count >= kReplayInstructionCap) return false;
    }
    return true;
}

bool EmulationController::landAfterReplay(size_t frame, long instructions)
{
    // The frame itself first, handled as a rewind "resume here" handles it:
    // audio and the debugger's per-timeline state follow the jump, the
    // cassette recording is cut to the frame, the abandoned future leaves
    // the ring and a scrub that was open is over. THEN the instructions up
    // to the landing point, so that what they toggle and record is kept —
    // done the other way round, the cut took the tape recorded between the
    // frame and the landing point with it, and whatever the replays before
    // this one had queued on the audio devices stayed queued.
    // FIRST, for the eye: a restore empties the video soft-switch log and
    // the published frame, so the Screen Inspector (and the screen itself,
    // for a program that flips pages mid-frame) would show the landing
    // point with no page flip above or below the beam. Re-executing the
    // two frames that lead to `frame` rebuilds that log; it is kept across
    // the restore below only if the run-up ends ON `frame`'s own cycle.
    // The machine's STATE still comes from `frame` itself.
    bool logRebuilt = false;
    {
        const uint64_t target = rewind_.infoAt(frame).cycle;
        const uint64_t barrier = replayBarrierFor(target);
        const uint64_t twoFrames = 2 * static_cast<uint64_t>(
            pom2VideoTiming(mem.videoStandard()).cyclesPerFrame);
        size_t from = frame;
        for (size_t i = frame; i-- > 0;) {
            if (rewind_.infoAt(i).cycle < barrier) break;       // no run-up across a barrier
            from = i;
            if (target - rewind_.infoAt(i).cycle >= twoFrames) break;
        }
        if (from != frame && restoreFrameForReplay(from)) {
            long count = 0;
            const uint64_t epoch = outsideEpoch();
            // A run-up that reached the outside took another path than the
            // original (a deterministic one cannot: what it would repeat is
            // fenced by a barrier, and it never starts before one): its log
            // is not this timeline's either.
            logRebuilt = replayTo(target, count) && mem.getCycleCounter() == target &&
                         outsideEpoch() == epoch;
        }
    }
    mem.setKeepVideoLogOnRestore(logRebuilt);
    const bool ok = restoreFrameForReplay(frame);
    mem.setKeepVideoLogOnRestore(false);
    noteTimeJump(/*keepDebuggerMeasurements=*/true);   // the caller trims the history
    rewindTapeTo(frame);
    rewind_.truncateAfter(frame);
    scrubIndex_.store(pom2::RewindBuffer::kNoFrame);
    resumeTruncate_.store(pom2::RewindBuffer::kNoFrame);
    processor.setDebugHook(debugger_.get());  // noteTimeJump re-synced it
    for (long i = 0; ok && i < instructions; ++i) replayOne();
    (void)processor.takeInterruptEntered();
    debugger_->setReplaying(false);
    suspendDriveSounds(false);                // the spindle as it stands at the landing
    debugger_->clearHit();                    // what the replay latched
    syncDebugHook();
    // The frame's keyboard said which keys were HELD then; the host holds
    // none of them now, and no release event is coming for them.
    // (That release counts as input, like anything arriving while the
    // machine stands still: it closes nothing until the machine executes
    // with it — reconcileOutsideBeforeExecuting.)
    // Input that arrived while stopped, BEFORE this verb, did not survive
    // it: the landing restored the frame's keyboard and game port. It is
    // not in this timeline, so it fences nothing (left counted, the next
    // Step put a barrier at the landing and a later Step Back was refused
    // for a key that never reached the machine). The release just below is
    // real, and counts.
    hostIoSeen_ = pom2::hostIoEpoch().load(std::memory_order_relaxed);
    mem.releaseHeldKeys();
    // Nor is a paste pending now (the verb is refused while one is): one the
    // frame held, cancelled or finished since, must not start typing again.
    mem.cancelPaste();
    return ok;
}

size_t EmulationController::capturePresent()
{
    const uint64_t now = mem.getCycleCounter();
    for (size_t i = rewind_.size(); i-- > 0;) {
        const uint64_t stamp = rewind_.infoAt(i).cycle;
        if (stamp == now) return i;       // a tick's end, or a scrub's frame
        if (stamp < now) break;
    }
    rewind_.capture(processor, mem, tape ? tape->recordMark() : 0);
    rememberKeyboardForFrame();
    return rewind_.size() - 1;
}

void EmulationController::rememberKeyboardForFrame()
{
    if (rewind_.empty()) { keyboardAtFrame_.clear(); return; }
    std::vector<uint8_t>& state = keyboardAtFrame_[rewind_.newestCycle()];
    const auto gamePort = mem.sdkPaddleState();
    state.assign(gamePort.begin(), gamePort.end());
    mem.appendSdkKeyboard(state);
    // Follow the ring: nothing older than its oldest frame, nothing newer
    // than its newest (a future the machine abandoned).
    keyboardAtFrame_.erase(keyboardAtFrame_.begin(), keyboardAtFrame_.lower_bound(rewind_.oldestCycle()));
    keyboardAtFrame_.erase(keyboardAtFrame_.upper_bound(rewind_.newestCycle()), keyboardAtFrame_.end());
}

bool EmulationController::restoreFrameForReplay(size_t frame)
{
    if (!rewind_.restore(frame, processor, mem)) return false;
    const auto found = keyboardAtFrame_.find(rewind_.infoAt(frame).cycle);
    if (found != keyboardAtFrame_.end() && found->second.size() > 6) {
        const std::vector<uint8_t>& state = found->second;
        (void)mem.loadSdkPaddles(state.data(), 6, /*apply=*/true);
        (void)mem.loadSdkKeyboard(state.data() + 6, state.size() - 6, /*apply=*/true);
    }
    return true;
}

EmulationController::ReplayWitness EmulationController::witnessNow() const
{
    ReplayWitness w;
    w.cycle = mem.getCycleCounter();
    w.pc = processor.getProgramCounter();
    w.a = processor.getAccumulator();
    w.x = processor.getXRegister();
    w.y = processor.getYRegister();
    w.p = processor.getStatusRegister();
    w.sp = processor.getStackPointer();
    w.memory.assign(0x10000, 0);
    mem.snapshotCpuView(w.memory.data());
    return w;
}

bool EmulationController::standsOn(const ReplayWitness& w) const
{
    if (mem.getCycleCounter() != w.cycle || processor.getProgramCounter() != w.pc ||
        processor.getAccumulator() != w.a || processor.getXRegister() != w.x ||
        processor.getYRegister() != w.y || processor.getStatusRegister() != w.p ||
        processor.getStackPointer() != w.sp)
        return false;
    std::vector<uint8_t> seen(0x10000, 0);
    mem.snapshotCpuView(seen.data());
    return seen == w.memory;
}

void EmulationController::reconcileOutsideBeforeExecuting()
{
    // Something arrived from outside while the machine stood still, and it
    // is about to execute with it. A frame of the machine as it stands now
    // — new input included — is captured and the barrier put on it: the
    // way back stays open down to this point, and no replay crosses it.
    const uint64_t io = pom2::hostIoEpoch().load(std::memory_order_relaxed);
    if (io == hostIoSeen_) return;
    captureRewindLocked();                  // -> noteCaptureForReplay: barrier on this frame
    if (hostIoSeen_ != io) {                // the capture was skipped (a scrub owns the ring)
        hostIoSeen_ = io;
        markReplayBarrier();
    }
}

void EmulationController::reconcileOutsideAfterStep(uint64_t epochBeforeStep)
{
    // The step itself exchanged a byte with a peer (`STA $C0A8` on a serial
    // card): replaying it would send the byte again. Same remedy, after it.
    if (pom2::hostIoEpoch().load(std::memory_order_relaxed) == epochBeforeStep) return;
    captureRewindLocked();
    hostIoSeen_ = pom2::hostIoEpoch().load(std::memory_order_relaxed);
    addReplayBarrier(mem.getCycleCounter());
}

bool EmulationController::abandonReplay(size_t presentFrame, uint64_t epochBefore,
                                        const pom2::Debugger::Hit& stopBefore)
{
    // The replay could not be trusted — it stalled, or it WROTE something a
    // rewind cannot undo (it took another path than the original: a key or a
    // mouse move during the frame, and that path saved a file). Landing in
    // the past would roll RAM back behind a write the medium keeps, which is
    // the one thing a rewind may never do (CLAUDE.md). So the machine goes
    // back to the present — the frame captured on the way in, not a second
    // replay, which would only fail the same way — and the ring, left as it
    // was, is then shown the write, which empties it.
    // "As it was" includes the debugger: the stop it was showing (a Run
    // from here must still get its amnesty, or it re-stops on the same
    // breakpoint having executed nothing and counts a second pass), the
    // profile and the history.
    const bool ok = restoreFrameForReplay(presentFrame);
    noteTimeJump(/*keepDebuggerMeasurements=*/true);
    if (ok) rewindTapeTo(presentFrame);
    (void)processor.takeInterruptEntered();
    debugger_->setReplaying(false);
    suspendDriveSounds(false);                // the spindle as it stood at the present
    debugger_->setHit(stopBefore);
    syncDebugHook();
    abandonedAt_ = mem.getCycleCounter();       // debugCanStepBack names the reason
    abandonedReachedOutside_ = outsideEpoch() != epochBefore;
    if (outsideEpoch() != epochBefore) {
        noteMediaWrite();                       // empties the ring if a medium was written
        hostIoSeen_ = pom2::hostIoEpoch().load(std::memory_order_relaxed);
        pom2::log().warn("Debugger", "going back was abandoned: replaying the history reached "
                                     "the outside (a media write, or bytes exchanged with the "
                                     "host). The machine is as it was.");
    } else {
        pom2::log().warn("Debugger", "going back was abandoned: re-executing the history did not "
                                     "lead back to the present (something the rewind does not "
                                     "record steered the machine: host time read by a clock, a "
                                     "timer's phase...). The machine is as it was.");
    }
    // Either way the history behind this point cannot be trusted again:
    // without this, every further click would repeat the same replay.
    hostIoAtStop_.store(hostIoSeen_, std::memory_order_relaxed);
    markReplayBarrier();
    return false;
}

bool EmulationController::debugStepBack()
{
    if (mode.load() != Mode::Stopped) return false;
    waitUntilParked();
    std::lock_guard<std::mutex> lk(stateMtx);
    noteMediaWrite();               // a pending irreversible write empties the ring
    if (!debugCanStepBack()) return false;
    const uint64_t now = mem.getCycleCounter();
    const uint64_t epoch = outsideEpoch();
    const pom2::Debugger::Hit stopBefore = debugger_->lastHit();
    const ReplayWitness witness = witnessNow();
    const size_t present = capturePresent();
    const size_t frame = rewindFrameBefore(now);
    if (frame == pom2::RewindBuffer::kNoFrame) return false;    // evicted by the capture
    beginReplay();
    // Count the instructions from the frame to the present...
    long count = 0;
    const bool reached = restoreFrameForReplay(frame) && replayTo(now, count);
    // ON the present — the same cycle, the same registers, the same 64 KiB.
    // A replay that arrives anywhere else took another path than the
    // original run, and one instruction before its end is a state the
    // machine never had.
    if (!reached || !standsOn(witness) || outsideEpoch() != epoch)
        return abandonReplay(present, epoch, stopBefore);
    // ...and run one fewer. (count is at least 1: the frame is before now.)
    if (!landAfterReplay(frame, count - 1)) return false;
    debugger_->dropHistoryOf(processor.getProgramCounter());   // the step just un-done
    pom2::Debugger::Hit hit;
    hit.reason = pom2::Debugger::Reason::StepBack;
    hit.pc = processor.getProgramCounter();
    hit.pcBank = hit.bank = mem.debugBank(hit.pc);
    hit.videoCycle = mem.videoNow();
    debugger_->setHit(hit);
    return true;
}

bool EmulationController::debugRunBack()
{
    if (mode.load() != Mode::Stopped) return false;
    waitUntilParked();
    std::lock_guard<std::mutex> lk(stateMtx);
    noteMediaWrite();
    if (!debugCanStepBack()) return false;
    const uint64_t now = mem.getCycleCounter();
    const uint64_t epoch = outsideEpoch();
    const pom2::Debugger::Hit stopBefore = debugger_->lastHit();
    const ReplayWitness witness = witnessNow();
    const size_t present = capturePresent();
    if (rewindFrameBefore(now) == pom2::RewindBuffer::kNoFrame) return false;

    beginReplay();
    // Frame by frame, newest first: replay each one up to where the next
    // begins (the present, for the newest) and keep the LAST boundary a
    // breakpoint would have stopped at. The first frame that has one holds
    // the answer. A stop is a boundary: `at` instructions after the frame —
    // before a PC breakpoint's instruction, after a memory breakpoint's.
    size_t frame = rewindFrameBefore(now);
    uint64_t limit = now;
    long at = -1;
    pom2::Debugger::Hit hit;
    // This runs on the caller's thread, under the state lock, and a frame of
    // a RamWorks machine is megabytes to restore: the whole history can take
    // many seconds. So it works for runBackBudget_ at most and then lands
    // where it has got to, saying so — Run Back again goes on from there.
    const auto started = std::chrono::steady_clock::now();
    bool paused = false;
    const bool boundaryOpen = now == runBackPausedAt_;
    const uint64_t barrier = replayBarrierFor(now);
    // The recorded state of the frame that starts at `limit` (when it is not
    // the present): the replay of the frame before it must arrive on it.
    ReplayWitness limitWitness;
    for (;;) {
        if (!restoreFrameForReplay(frame)) return abandonReplay(present, epoch, stopBefore);
        ReplayWitness thisFrame = witnessNow();
        debugger_->clearHit();
        long count = 0;
        for (;;) {
            const uint64_t cycle = mem.getCycleCounter();
            // The boundary AT the limit belongs to this frame's replay (the
            // next frame starts on it, its snapshot knowing nothing of an
            // access just made) — unless it is the present: where the
            // machine stood, which Run Back is leaving.
            // (...unless a Run Back paused HERE: the boundary it landed on
            // was never probed — it belongs to the frame before, which that
            // call did not replay.)
            // Every frame's replay is checked where it ends (see
            // ReplayWitness): the newest one on the present, an older one on
            // the next frame's own recorded state — a replay that strays in
            // an old frame would otherwise put a stop where none happened.
            if (cycle == limit && !standsOn(limit == now ? witness : limitWitness))
                return abandonReplay(present, epoch, stopBefore);
            if (cycle == limit && limit == now && !boundaryOpen) break;
            // Past the next frame's stamp: the replay left the original's
            // path, and nothing it finds from here on is history.
            if (cycle > limit) return abandonReplay(present, epoch, stopBefore);
            // While a card's processor holds the bus the 6502 is parked: its
            // PC names no instruction about to run, and the live loop never
            // offers it. Only a memory breakpoint can stop there.
            const bool parked = mem.slotBus().dmaClaimant() != nullptr;
            if (parked ? debugger_->stopRequested()
                       : debugger_->probeInstruction(processor.getProgramCounter())) {
                at = count;
                hit = debugger_->lastHit();   // with its own beam cycle: the access's
                debugger_->clearHit();
            }
            if (cycle == limit) break;
            replayOne();
            ++count;
            if (mem.getCycleCounter() == cycle || count >= kReplayInstructionCap)
                return abandonReplay(present, epoch, stopBefore);
        }
        if (outsideEpoch() != epoch) return abandonReplay(present, epoch, stopBefore);
        // The oldest frame a replay may start from: the ring's first, or the
        // first after the accelerator's last speed change.
        if (at >= 0 || frame == 0 || rewind_.infoAt(frame - 1).cycle < barrier) break;
        if (std::chrono::steady_clock::now() - started >= runBackBudget_) { paused = true; break; }
        limit = rewind_.infoAt(frame).cycle;
        limitWitness = std::move(thisFrame);
        --frame;
    }
    // Land: the frame again, then `at` instructions (none: its first cycle).
    if (!landAfterReplay(frame, at < 0 ? 0 : at)) return false;
    runBackPausedAt_ = (paused && at < 0) ? mem.getCycleCounter() : ~uint64_t(0);
    debugger_->clearHistory();              // it described a future now gone
    if (at < 0) {
        hit = {};
        hit.reason = paused ? pom2::Debugger::Reason::RunBackPaused
                            : pom2::Debugger::Reason::RunBackStart;
        hit.pc = processor.getProgramCounter();
        hit.pcBank = hit.bank = mem.debugBank(hit.pc);
        hit.videoCycle = mem.videoNow();
    }
    debugger_->setHit(hit);
    return true;
}
