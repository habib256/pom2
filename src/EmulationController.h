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

// Drives the M6502 + Memory in a worker thread so the UI can render at
// 60 Hz without stalling the simulation. Single thread, single mutex; the
// UI thread takes the mutex briefly each frame to render the screen.

#ifndef POM2_EMULATION_CONTROLLER_H
#define POM2_EMULATION_CONTROLLER_H

#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "Debugger.h"
#include "Disk35Image.h"
#include "FloppySoundDevice.h"
#include "IIcExternalSmartPort.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "NoSlotClock.h"
#include "RewindBuffer.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "SpeakerDevice.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 { class StateAccess; }

class EmulationController
{
public:
    enum class Mode { Stopped, Running, Step };

    EmulationController();
    ~EmulationController();

    /// Unchecked access to the emulated state. Correct ONLY on the worker
    /// thread, during construction before the worker starts, or for the
    /// handful of Memory entry points that carry their own finer-grained
    /// lock (the keyboard latch / paste queue guard `Memory::kbMutex`).
    /// Every other caller wants `lockState()` — see the note there.
    Memory&         memory()   { return mem; }
    M6502&          cpu()      { return processor; }

    /// Continuous state-recording ring buffer behind the rewind feature.
    /// Disabled by default (zero overhead); enable via
    /// rewind().setEnabled(true). The worker thread captures one frame at
    /// each 60 Hz boundary while enabled — see workerLoop().
    pom2::RewindBuffer& rewind() { return rewind_; }

    /// Run-control debugger — breakpoints and the stop reason. Always
    /// constructed; it costs nothing until something is armed, at which point
    /// `syncDebugHook()` attaches it to the CPU and the CPU switches to its
    /// debugged loop. Every mutating call must be made under `stateMutex`
    /// (see Debugger.h § Threading).
    pom2::Debugger& debugger() { return *debugger_; }
    const pom2::Debugger& debugger() const { return *debugger_; }

    /// Attach or detach the CPU hook to match the debugger's armed state.
    /// Call after ANY breakpoint mutation — this is what keeps an un-armed
    /// debugger off the CPU's hot loop entirely. Caller holds `stateMutex`.
    void syncDebugHook();

    /// Resume from a debugger stop: clears the hit and grants the current PC
    /// one instruction of amnesty, so Run does not re-trigger the breakpoint
    /// it is standing on. Caller holds `stateMutex`.
    void debugResume();

    /// Step one instruction even when stopped at a breakpoint: the same
    /// amnesty as debugResume(), then one queued step. Takes `stateMutex`
    /// itself, so the caller must NOT hold it.
    void debugStepInstruction();

    /// Run until control returns past the current instruction — a step OVER.
    /// For a JSR that is the address after it; for anything else this is an
    /// ordinary single step, because there is nothing to step over. Takes
    /// `stateMutex` itself.
    void debugStepOver();

    /// Run until the PC reaches `addr`, then stop. Takes `stateMutex` itself.
    void debugRunToCursor(uint16_t addr);
    CassetteDevice&    cassette()    { return *tape; }
    SpeakerDevice&     speaker()     { return *spk; }
    /// 5.25" Disk II mechanical sounds (head step / motor / click).
    /// DiskIICard plug routes here.
    FloppySoundDevice& floppySound525() { return *floppy525; }
    /// 3.5" Sony / SmartPort mechanical sounds. Sony35Drive (//c+ on-
    /// board) and SmartPortCard (Liron-class slot card) route here.
    FloppySoundDevice& floppySound35()  { return *floppy35; }
    /// Legacy single-instance accessor — alias for the 5.25" device, kept
    /// only for any out-of-tree caller. Internal call sites should pick
    /// floppySound525()/floppySound35() explicitly.
    FloppySoundDevice& floppySound() { return *floppy525; }
    AudioDevice&       audio()       { return *audioDev; }
    pom2::IWMDevice&   iwm()         { return *iwmDev; }
    pom2::SmartPortHub& smartPortHub() { return *hub; }
    pom2::IIcExternalSmartPort& externalSmartPort() { return *extSmartPort_; }
    /// Dallas DS1216E "SmartWatch" — lives at controller scope so its
    /// (battery-backed on real hardware) state machine survives profile
    /// switches and CPU resets.
    pom2::NoSlotClock&  noSlotClock() { return *noSlotClock_; }
    pom2::Sony35Drive&  sony35Internal() { return *drive35Int; }
    pom2::Sony35Drive&  sony35External() { return *drive35Ext; }
    pom2::Disk35Image&  disk35Internal()  { return *image35Int; }
    pom2::Disk35Image&  disk35External()  { return *image35Ext; }

    /// Mount an 800K Sony 3.5" image into drive `idx` (0 = internal,
    /// 1 = external). Takes the state mutex while swapping the
    /// `Disk35Image` payload and notifying the Sony35Drive's disk-
    /// change flip-flop. Returns true on success; on failure the
    /// drive is left empty and the image's `lastError()` carries the
    /// reason.
    bool mount35(int idx, const std::string& path);

    /// Unmount whatever is in 3.5" drive `idx` (0/1). No-op when empty.
    ///
    /// Two-phase like `mount35`: the 800 KB write-back is lifted out under
    /// `stateMtx` (a memcpy) and committed with the lock RELEASED, then the
    /// medium is dropped under the lock again. A failed commit puts the dirty
    /// flag back and refuses the eject, so the user can fix the cause and
    /// retry instead of losing the session's writes.
    bool eject35(int idx);

    /// Block until every deferred 3.5" write-back has been committed.
    /// Shutdown / test hook — the queue drains on its own thread otherwise.
    ///
    /// Call it with `stateMutex` NOT held: the queue takes that lock to
    /// deliver each completion, so draining from inside it deadlocks. The
    /// quit path calls it before the settings are written, which is the one
    /// place a user can tell the difference (the destructor's own drain runs
    /// after that).
    void drainDeferredWriteBacks() { writeBackQueue_.drain(); }

    // ─── Cassette transport (forwarded to CassetteDevice under stateMtx) ──
    /// Load / save a tape file. Both do their file work with `stateMtx`
    /// RELEASED (a compressed tape is decoded in full — see the TapeOffBus
    /// note in the .cpp), unhooking the deck from the bus for the duration
    /// instead of freezing the machine behind the lock.
    bool loadTape (const std::string& path);
    bool saveTape (const std::string& path);
    void playTape();
    void stopTape();
    void pauseTape(bool paused);
    void rewindTape();
    void ejectTape();
    void clearTapeCapture();
    void armRecording();   // takes stateMutex — safe to call off the CPU thread
    void seekTapeRelative(double deltaSeconds);
    void setCassetteVolume(float v);

    void start();

    /// Single-threaded tick path — execute ONE frame's worth of work
    /// (cyclesPerFrame for Running, drain stepsPending for Step,
    /// no-op for Stopped) and return. Designed for hosts that can't
    /// run a CPU worker thread (browser WASM without SharedArrayBuffer
    /// / pthreads); call once per render frame. Threaded hosts ignore
    /// this entirely — the `workerLoop` spawned by `start()` covers
    /// the same logic.
    void tickFrame();

    /// Park the worker: sets Mode::Stopped, wakes it, then blocks UNTIL the
    /// worker has actually parked at the Stopped idle wait — a hard
    /// guarantee (unbounded wait with a periodic warn log), because after
    /// stop() returns callers (applyProfile / restartEmulationFromSettings /
    /// shutdown) rebuild ROMs/SlotBus outside stateMutex(); a best-effort
    /// timeout returning early would hand them a use-after-free. The worker
    /// re-checks the mode between its 4096-cycle chunks and the per-frame
    /// budget is capped (CLI/AI clamp at 2 M cycles), so parking is prompt
    /// in practice.
    ///
    /// MUST NOT be called while holding stateMutex(): the worker needs that
    /// lock to finish its current chunk before it can park — a violation
    /// now DEADLOCKS (loudly, with the warn log) instead of silently
    /// degrading to a race.
    void stop();

    // Reset API — POM2 exposes 4 verbs. The MAME equivalents are only 2
    // (per Agent F audit, gap F-1-4): `machine_start` runs once at
    // power-on (RAM init pattern, region select) and `machine_reset`
    // (II/II+) / `reset_w` (IIe/IIc/IIc+) handle every subsequent reset.
    // POM2's split is:
    //
    //   softReset()    → MAME `reset_w(true)→reset_w(false)` sequence.
    //                    On IIe-class wipes the MMU/IOU/LC list; on
    //                    II/II+ only clears kbd strobe + cnxx tracker
    //                    (per `resetSoftSwitchesWarm`). A/X/Y/RAM/zp
    //                    all survive. SP decremented by 3 (Theme 7).
    //
    //   hardReset()    → Same MAME path as softReset but the CPU also
    //                    zeros A/X/Y. POM2-only convention to give the
    //                    user a "deterministic CPU state" without a
    //                    full RAM wipe. RAM contents preserved.
    //
    //   coldBoot()     → MAME `machine_start` + `machine_reset` combo:
    //                    wipes user RAM ($0000-$BFFF + LC + aux) with
    //                    the 00/FF pattern, then runs the full soft-
    //                    switch reset. The only path that wipes RAM.
    //
    //   bootFromSlot() → Synthetic shortcut: coldBoot + force PC=$Cn00
    //                    after validating the slot has the autostart
    //                    signature ($Cn01=$20, $Cn03=$00, $Cn05=$03,
    //                    $Cn07=$3C — Apple II Ref Manual Appx C). On
    //                    signature mismatch, falls back to coldBoot
    //                    so the F8 autostart firmware can scan slots
    //                    naturally (Theme 8).
    void hardReset();
    void softReset();
    void coldBoot();
    /// Returns true when the machine really was launched into that slot's
    /// ROM. False means the card carried no Apple-II JSR-dispatch signature,
    /// so this degraded to a plain cold boot: the emulator is running, but
    /// off the F8 ROM's own slot scan, not off this card. Callers that
    /// report "booted <image>" to the user MUST honour it — the drop / CLI /
    /// Library paths used to claim success on that fallback.
    bool bootFromSlot(int slot);
    void requestStep(int n = 1);   // queue n single-instruction steps

    void setMode(Mode m);
    Mode getMode() const { return mode.load(); }

    // ─── Rewind transport (UI-facing) ────────────────────────────────────
    // Coordinate the rewind ring buffer with the worker thread. While
    // "scrubbing", the worker is parked (Stopped) so the UI can freely
    // restore historical frames without the in-flight frame overrunning
    // them. All of these take stateMutex internally — call from the UI
    // thread, not the worker.

    /// Park the worker so historical frames can be restored, then report
    /// whether there is anything to scrub (rewind enabled + ≥ 1 frame).
    bool   rewindBeginScrub();
    /// Restore frame `index` (clamped to the ring). Caller must have begun
    /// scrubbing. Returns the clamped index, or RewindBuffer::kNoFrame if
    /// the ring is empty.
    size_t rewindSeek(size_t index);
    /// Restore the newest frame whose cycle stamp is <= `cycle`.
    size_t rewindSeekToCycle(uint64_t cycle);
    /// Leave scrub: discard the abandoned future after `index` and resume
    /// live execution from there.
    void   rewindEndAndResume(size_t index);
    /// Leave scrub but stay paused at the current frame (keeps the ring).
    void   rewindEndPaused();
    /// Media-write policy for the rewind ring.
    ///
    /// The ring never captures MEDIA of a block device (HDV / CFFA /
    /// SmartPort, up to 32 MiB), a 3.5" image (800 KB) or a writable WOZ
    /// (its bits live in `wozRaw`, a store the Disk II media snapshot does
    /// not cover). Rolling RAM back over a ProDOS SAVE while the volume
    /// stayed written is a real corruption path: the restored directory and
    /// bitmap disagree with the blocks on the disk and the next allocation
    /// cross-links them.
    ///
    /// Policy (the safe minimum, chosen over per-frame media capture): a
    /// rewind may never CROSS such a write. Every one of those write paths
    /// bumps `pom2::mediaWriteEpoch()`; this checks it at the ring's capture
    /// point and clears the history when it moved, so the timeline restarts
    /// after the write instead of spanning it. Cheap — one relaxed atomic
    /// load per captured frame — and it also covers write paths that do not
    /// exist yet, because the epoch is bumped at the storage leaf.
    ///
    /// Non-WOZ Disk II nibble writes deliberately do NOT bump: those ARE
    /// captured (DiskIICard's v2 media snapshot) and a rewind is expected to
    /// undo them.
    void   noteMediaWrite();
    /// Put everything that is NOT in the snapshot back onto the restored
    /// timeline after a time jump (rewind scrub OR a snapshot load): the
    /// free-running audio devices, and the debugger's per-timeline
    /// transients. PUBLIC because
    /// the two file-driven load paths — the AI server's `/snapshot/load` and
    /// the CLI's `--snapshot-load` — used to hand-roll `speaker().reset()`
    /// and so silently missed every device added to this function since.
    /// Callers must hold `stateMutex`.
    void   noteTimeJump();
    /// True while a scrub owns the machine — the worker is parked at a
    /// historical frame and the live state is that frame, not the newest.
    ///
    /// The controller owns this, not the panel: `setMode(Running)` from ANY
    /// path (toolbar Play, Machine > Run, the `machine.run` palette command,
    /// the kiosk menu) ends the scrub, and a UI that kept its own flag would
    /// go on believing it was still scrubbing — its next drag would seek a
    /// machine that is running, so the slider visibly does nothing.
    bool   rewindScrubbing() const {
        return scrubIndex_.load() != pom2::RewindBuffer::kNoFrame;
    }
    /// True once the worker has parked at the Stopped idle wait (test hook).
    bool   rewindIsParked() const { return workerParked_.load(); }

    // 6502 cycles per ImGui frame (CPU-pacing budget). Default = ~17 045
    // cycles/frame = 1.0227 MHz emulated. Setting it higher than the real
    // clock turbo-runs the CPU; UI uses this for the "MAX" button.
    void setCyclesPerFrame(int n) { cyclesPerFrame.store(n); }
    int  getCyclesPerFrame() const { return cyclesPerFrame.load(); }

    /// `cyclesPerFrame` after any plugged accelerator's multiplier — the
    /// budget the frame loop actually burns. Public so the status bar and
    /// the tests can report the machine's real speed rather than the
    /// setting. Must be called with the SlotBus stable (CPU thread, or
    /// under stateMutex).
    int64_t scaledFrameBudget() const;

    // Machine video standard (NTSC 60 Hz / PAL 50 Hz). Sets the worker's
    // frame-pacing interval and propagates the 262/312-line geometry to
    // Memory (for beam-racing). The CPU budget per frame (cyclesPerFrame) is
    // set separately from the active profile's defaultCyclesPerFrame, so the
    // effective clock = cyclesPerFrame × refreshHz works out to ~1.0227 MHz
    // (NTSC) / ~1.0156 MHz (PAL).
    void          setVideoStandard(VideoStandard s);
    VideoStandard getVideoStandard() const { return videoStandard_.load(); }

    // Identity of the machine currently configured, as
    // `pom2::snapshotMachineId(profile)`. Set by applyProfile alongside the
    // video standard; read by the snapshot FILE and AI-server paths so a
    // capture is stamped and a load can refuse a snapshot taken on a
    // different Apple. 0 means "no profile applied yet", which those paths
    // treat as "cannot verify" rather than as a mismatch.
    //
    // Atomic and not under `stateMutex` on purpose: it is configuration, not
    // emulated state, and the AI server reads it while answering a request.
    void          setMachineId(uint32_t id) { machineId_.store(id); }
    uint32_t      machineId() const { return machineId_.load(); }

    // Block for up to `timeoutMs` until the CPU is paused at an
    // instruction boundary. Cheap: the worker holds `stateMutex` only
    // while running a slice, releases it on every iteration.
    //
    // Two spellings, one mutex, and the choice is not stylistic:
    //   • `lockState()`  — "I need to read or write the emulated state."
    //                      Hands back Memory and the CPU *through* the lock,
    //                      so the access cannot be written without it.
    //   • `stateMutex()` — "I need mutual exclusion against the worker, but
    //                      I am not touching Memory or the CPU" (serialising
    //                      a card pointer against a profile switch, say).
    // Reaching `memory()` / `cpu()` below while holding a bare
    // `stateMutex()` is the old spelling of the first case; prefer
    // `lockState()` in new code so the lock and the access stay welded.
    std::mutex& stateMutex() { return stateMtx; }

    /// Take the state lock and the state together — see `pom2::StateAccess`
    /// below. `[[nodiscard]]` because discarding the handle locks and unlocks
    /// in the same expression, which is never what the caller meant.
    [[nodiscard]] pom2::StateAccess lockState();

private:
    friend class pom2::StateAccess;
    Memory                          mem;
    M6502                           processor;
    std::unique_ptr<CassetteDevice>    tape;
    std::unique_ptr<SpeakerDevice>     spk;
    std::unique_ptr<FloppySoundDevice> floppy525;
    std::unique_ptr<FloppySoundDevice> floppy35;
    std::unique_ptr<AudioDevice>       audioDev;
    std::unique_ptr<pom2::IWMDevice>    iwmDev;
    /// Deferred 3.5" write-backs, and the thread that commits them.
    ///
    /// The firmware-issued eject (`Sony35Drive` register 7) fires from the
    /// IWM path — on the CPU worker, `stateMtx` held — and CLAUDE.md's rule
    /// is that the lock is never held across file I/O. The drive captures the
    /// payload (a memcpy) and submits it here; `submit` only takes this
    /// queue's own uncontended mutex, so it is safe under the machine lock.
    /// One thread, started on the first submission, does every write in
    /// order — which also keeps two ejects of the same file from racing.
    ///
    /// Declared BEFORE the drives that point at it: members are destroyed in
    /// reverse declaration order, so the sink outlives every `Sony35Drive`
    /// holding its address.
    class WriteBackQueue final : public pom2::Disk35WriteBackSink
    {
    public:
        explicit WriteBackQueue(EmulationController& owner) : owner_(owner) {}
        ~WriteBackQueue() override;
        void submit(pom2::Disk35Image::PendingWriteBack&& pending,
                    Completion onDone = {}) override;
        /// Block until the queue is empty and the in-flight commit is done.
        /// Never call it holding `stateMtx` — see drainDeferredWriteBacks().
        void drain();
        /// Stop the thread and commit whatever is left, WITHOUT reporting:
        /// by then there is nothing to report to. Idempotent, and called from
        /// `~EmulationController`'s body rather than left to the member
        /// destructor — `stateMtx` is declared after this queue, so it would
        /// already be gone when the worker tried to take it (see report()).
        void shutdown();
    private:
        struct Item {
            pom2::Disk35Image::PendingWriteBack pending;
            Completion                          done;
        };
        void run();
        /// Report one outcome to the submitter WITH `stateMtx` held and this
        /// queue's own mutex released — the callback touches drive/image
        /// state, and taking the two locks in the other order would invert
        /// `submit`'s (stateMtx → mtx_).
        void report(const Item& item, bool ok, const std::string& error);

        EmulationController&    owner_;
        std::mutex              mtx_;
        std::condition_variable cv_;
        std::condition_variable idleCv_;
        std::vector<Item>       queue_;
        std::thread             worker_;
        bool                    stopping_ = false;
        bool                    busy_     = false;
    };
    WriteBackQueue writeBackQueue_;

    std::unique_ptr<pom2::Disk35Image>  image35Int;
    std::unique_ptr<pom2::Disk35Image>  image35Ext;
    std::unique_ptr<pom2::Sony35Drive>  drive35Int;
    std::unique_ptr<pom2::Sony35Drive>  drive35Ext;
    std::unique_ptr<pom2::SmartPortHub> hub;
    std::unique_ptr<pom2::IIcExternalSmartPort> extSmartPort_;   // plain //c rear port
    std::unique_ptr<pom2::NoSlotClock>  noSlotClock_;

    pom2::RewindBuffer rewind_;
    /// Last `pom2::mediaWriteEpoch()` value the ring was reconciled against
    /// — see noteMediaWrite().
    uint64_t rewindMediaEpoch_ = 0;
    std::unique_ptr<pom2::Debugger> debugger_;

    std::atomic<Mode> mode{Mode::Stopped};
    std::atomic<int>  cyclesPerFrame{17045};
    // Worker frame-pacing interval (µs) and the active video standard. PAL
    // paces at 50 Hz (20000 µs), NTSC at 60 Hz (~16667 µs).
    std::atomic<int>  frameIntervalUs{1'000'000 / 60};
    /// Wall-clock of the previous `tickFrame()` (single-threaded / WASM
    /// path only). Used to scale the per-call CPU budget by the time that
    /// actually elapsed, since the browser calls us once per DISPLAY
    /// refresh rather than once per emulated frame. Zero-initialised =
    /// "no previous tick", which runs one nominal budget.
    std::chrono::steady_clock::time_point lastTickWall_{};
    std::atomic<VideoStandard> videoStandard_{VideoStandard::NTSC};
    std::atomic<int>  stepsPending{0};   // queued single-step count (Step mode)
    std::atomic<bool> exitRequested{false};
    // True while the worker is idling in the Stopped CV wait. The rewind
    // transport and stop() wait on this so a UI-thread restore / profile
    // rebuild can't be overrun by an in-flight Running frame. The Running
    // branch re-checks `mode` between 4096-cycle chunks, so the worker
    // parks within ~one chunk of a Stopped request.
    std::atomic<bool> workerParked_{false};
    // Frame index the scrub currently sits on, kNoFrame when not scrubbing.
    // Written from the UI thread (the rewind transport), read anywhere.
    std::atomic<size_t> scrubIndex_{pom2::RewindBuffer::kNoFrame};

    std::mutex              stateMtx;
    std::condition_variable wakeCv;
    std::atomic<uint32_t>   machineId_{0};
    std::thread             worker;

    /// One CPU budget slice under stateMutex: normally M6502::run, but
    /// while a slot card claims DMA bus mastery (SoftCard Z80 — see
    /// SlotPeripheral::dmaActive) the slice is handed to the card's
    /// processor instead. The 6502 yields mid-chunk on the hand-over
    /// (the card calls M6502::stop() from its toggle write), so the
    /// swap takes effect at the next instruction boundary, not the next
    /// 4096-cycle chunk. Budget + return value stay in 6502 cycles in
    /// both branches (the card converts its own clock — emuCycles never
    /// leaves the 6502 domain).
    int  runCpuSlice(int chunk);
    /// Park the machine after a debugger stop. Called from runCpuSlice with
    /// `stateMutex` held.
    void noteDebuggerStop();
    /// One single-instruction step of the current bus master: the DMA
    /// claimant's processor when a card owns the bus (SoftCard Z80),
    /// else the 6502. All Step verbs (debugger, CLI --step, AI /step)
    /// route through this so stepping can't run parked-6502 code that a
    /// DMA-halted CPU would never execute.
    void stepBusMaster();
    void workerLoop();
    void waitUntilParked();      // block (bounded) until workerParked_ is set
    void flushAudioForRewind();  // → flushAudioForTimeJump (rewind callers)
};

namespace pom2 {

/// RAII handle over `EmulationController::stateMtx` that also *carries* the
/// emulated state.
///
/// The invariant that mutex exists for — "never touch Memory or the CPU off
/// the worker thread without holding it" — was otherwise enforced by nothing
/// but care, across ~120 call sites in the UI, the AI control server and the
/// CLI runner, none of which run on the worker thread. Reaching the state
/// *through* the lock welds the two: there is no way to spell `st.memory()`
/// without having taken the lock to obtain `st`, and no way to keep the
/// reference past the unlock without deliberately storing it.
///
///     auto st = controller->lockState();
///     st.memory().memWrite(0x300, 0xEA);
///     st.cpu().setProgramCounter(0x300);
///
/// **Non-recursive.** `stateMtx` is a plain `std::mutex`, so a second
/// `lockState()` on a thread that already holds one deadlocks. A helper that
/// needs the state but runs from both locked and unlocked callers must NOT
/// lock for itself — it takes a `const StateAccess&` and lets the caller
/// prove ownership by passing its handle. `MainWindow::plugSlotsFromSettings`
/// is that shape, and the reason this class is a namespace-scope type rather
/// than a member of EmulationController: `MainWindow.h` deliberately stays
/// outside the EmulationController include cone (see the note on
/// `MainWindow::emul()`), and only a non-nested class can be forward-declared
/// there.
class StateAccess
{
public:
    Memory& memory() const { return ctl_->mem; }
    M6502&  cpu()    const { return ctl_->processor; }

    StateAccess(StateAccess&&) noexcept            = default;
    StateAccess& operator=(StateAccess&&) noexcept = default;
    StateAccess(const StateAccess&)                = delete;
    StateAccess& operator=(const StateAccess&)     = delete;

private:
    friend class ::EmulationController;
    explicit StateAccess(EmulationController* c)
        : ctl_(c), lk_(c->stateMtx) {}

    EmulationController*         ctl_;
    std::unique_lock<std::mutex> lk_;
};

} // namespace pom2

inline pom2::StateAccess EmulationController::lockState()
{
    return pom2::StateAccess(this);
}

#endif // POM2_EMULATION_CONTROLLER_H
