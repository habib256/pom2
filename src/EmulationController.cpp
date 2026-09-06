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

#include "EmulationController.h"

#include "CpuClock.h"
#include "Logger.h"
#include "ThreadGuard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

void EmulationController::setVideoStandard(VideoStandard s)
{
    videoStandard_.store(s);
    frameIntervalUs.store(1'000'000 / pom2VideoTiming(s).refreshHz);
    // Geometry propagates to Memory so pushVideoEventLocked stamps the right
    // scanline. Called from applyProfile with the worker stopped, so the
    // plain Memory member is set without a concurrent reader.
    mem.setVideoStandard(s);
    // Retune the 1-bit speaker's cycle→sample reconstruction to the standard's
    // actual CPU clock (PAL ≈ 1.0156 MHz vs NTSC ≈ 1.0227 MHz). Without this
    // the audio path assumed NTSC under PAL and starved the reconstructor,
    // glitching continuous speaker music. (AY/SSI263 device clocks stay at the
    // NTSC nominal by design — their 0.7 % delta is an inaudible pitch approx.)
    if (spk) spk->setCpuClock(static_cast<double>(pom2VideoTiming(s).cpuClockHz));
    // Same starvation applies to the cassette's realtime pulse monitor (its
    // tape-FILE timebase intentionally stays NTSC-nominal — format spec).
    if (tape) tape->setCpuClock(static_cast<double>(pom2VideoTiming(s).cpuClockHz));
    // The Mockingboard's emuCycles replay cursor (audio thread) needs the
    // same retune: it maps a queued AY register write's CPU-cycle stamp to
    // a sample offset inside the buffer, so a cursor left at the NTSC rate
    // outruns a PAL producer and collapses every write to the buffer edge.
    // Reached through the slot bus — the card is owned there, and a profile
    // switch re-plugs it, so this is re-applied on every standard change.
    // Virtual hook rather than a dynamic_cast per card type: casting made
    // EmulationController link-depend on every card it named, breaking
    // tests that link the controller without them.
    for (int slot = 1; slot <= 7; ++slot) {
        if (SlotPeripheral* card = mem.slotBus().peripheral(slot))
            card->setCpuClock(static_cast<double>(pom2VideoTiming(s).cpuClockHz));
    }
}

EmulationController::EmulationController()
    : processor(&mem)
{
    cyclesPerFrame.store(POM2_CPU_CYCLES_PER_FRAME_60HZ);

    // Audio device first — we want its negotiated sample rate before the
    // cassette starts streaming. miniaudio sometimes negotiates 48 kHz on
    // Apple Silicon even when 44.1 kHz is requested; using a stale rate
    // would drift the cassette pulse playback by the rate ratio.
    audioDev = std::make_unique<AudioDevice>();
    tape     = std::make_unique<CassetteDevice>();
    tape->setAudioOutputSampleRate(audioDev->getActualSampleRate());
    tape->setAudioAvailable(audioDev->isAvailable());
    if (audioDev->isAvailable()) audioDev->addSource(tape.get());

    // 1-bit speaker: cycle-driven AudioSource. Sample rate must match
    // the device's negotiated rate or the audio drifts.
    spk = std::make_unique<SpeakerDevice>();
    spk->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(spk.get());

    // Floppy mechanical sounds — two independent instances so the 5.25"
    // and 3.5" sample sets coexist (FloppySoundDevice stores a single
    // sample bank per instance). Samples are loaded later by MainWindow
    // once the roms/floppy_samples/ directory has been probed; both
    // sources are silent until then. Loading at audio-construction time
    // would require EmulationController to know about a roms/ filesystem
    // convention that is otherwise MainWindow's concern.
    floppy525 = std::make_unique<FloppySoundDevice>();
    floppy525->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(floppy525.get());

    floppy35 = std::make_unique<FloppySoundDevice>();
    floppy35->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(floppy35.get());

    // //c / //c+ on-board IWM. Memory mirrors $C0E0-$C0EF accesses to
    // this device on iicHasAltBank profiles so its state machine
    // (MAME-faithful, see `IWMDevice.{h,cpp}`) evolves in lock-step
    // with the slot-6 DiskIICard's lightweight shadow. On II/II+/IIe/
    // /16K-//c profiles the pointer is set but never consulted (the
    // iicHasAltBank guard in Memory keeps the mirror off).
    iwmDev = std::make_unique<pom2::IWMDevice>();
    // Routes $C0EC/ED/EE/EF *reads* through the IWM on iicHasAltBank
    // profiles. ON by default (`Memory.h iwmAuthoritative = true`), but
    // since 2026-07-29 "authoritative" is scoped to the one device the
    // IWM owns: IIcClassProfile::ioReadIWM returns the IWM's byte only
    // while the SmartPortHub routes to a 3.5" Sony; 5.25" data always
    // comes from DiskIICard's LSS (the IWM walker mis-framed RWTS
    // enough that a //c+ DOS 3.3 SAVE failed its write-verify). Set
    // `POM2_IWM_AUTHORITATIVE=0` to force full shadow mode.
    if (const char* env = std::getenv("POM2_IWM_AUTHORITATIVE")) {
        mem.setIWMAuthoritative(env[0] != '0');
    }

    // //c+ SmartPort 3.5" hub. Holds the two Sony 3.5" drive objects
    // plus the drive-selection state machine. MIG state changes
    // (Memory $C0CC / $C0CE windows on bank-1 alt firmware) route
    // through it; the IWM's phases/devsel callbacks are wired here so
    // 3.5" drives receive command strobes. Off-path on II/II+/IIe/
    // 16K-//c profiles — the hub is constructed but Memory never
    // routes traffic into it unless `iicHasAltBank` is set.
    image35Int = std::make_unique<pom2::Disk35Image>();
    image35Ext = std::make_unique<pom2::Disk35Image>();
    drive35Int = std::make_unique<pom2::Sony35Drive>();
    drive35Ext = std::make_unique<pom2::Sony35Drive>();
    hub        = std::make_unique<pom2::SmartPortHub>();
    drive35Int->setImage(image35Int.get());
    drive35Ext->setImage(image35Ext.get());
    // Mechanical-sound source: dedicated 3.5" `FloppySoundDevice`
    // instance, loaded with the `35_*.wav` Sony sample set. Step cadence
    // + motor on/off are driven from `Sony35Drive::strobeWriteRegister`
    // cases 0x1 / 0x2 / 0x3, stamped with the IWM's last-tick CPU cycle
    // so the audio thread can measure cadence in emulated time (matches
    // the comment block on FloppySoundSink::step). Samples are loaded
    // later by MainWindow from roms/floppy_samples/ — until then the
    // 3.5" channel stays silent.
    drive35Int->setFloppySound(floppy35.get());
    drive35Ext->setFloppySound(floppy35.get());
    // Firmware-issued ejects (register 7) fire from the IWM path on the CPU
    // worker, `stateMtx` held. Route their write-back through the queue so
    // the 800 KB rewrite happens off that lock — see WriteBackQueue.
    drive35Int->setWriteBackSink(&writeBackQueue_);
    drive35Ext->setWriteBackSink(&writeBackQueue_);
    hub->attach(iwmDev.get());
    hub->setSony35(drive35Int.get(), drive35Ext.get());

    // Dallas DS1216E "No-Slot Clock". Battery-backed on real hardware,
    // so we hold it at controller scope; survives profile switches and
    // CPU resets. Memory hooks the $F800-$FFFF intercept through this
    // pointer; nullptr disables (MainWindow toggles via setEnabled()).
    noSlotClock_ = std::make_unique<pom2::NoSlotClock>();

    // Run-control debugger. Constructed always, attached to the CPU never —
    // until something is armed. An un-armed debugger holds no memory (its
    // bitmaps are lazy) and leaves `M6502::run` on the loop it has always
    // used, so a session that does not debug pays nothing for it.
    debugger_ = std::make_unique<pom2::Debugger>();
    // Where Memory reports a watched write. The sink is permanent; what
    // switches watchpoints on and off is Memory's own (lazily allocated)
    // diversion table, rebuilt by syncDebugHook().
    mem.setWatchSink(debugger_.get());

    // Wire $C020 / $C060 (cassette) and $C030 (speaker, with sub-
    // instruction timestamping via the CPU back-pointer).
    mem.setCassetteDevice(tape.get());
    mem.setSpeakerDevice(spk.get());
    mem.setCpu(&processor);
    mem.setIWM(iwmDev.get());
    mem.setSmartPortHub(hub.get());
    // The plain //c's external 3.5": its own firmware over the SmartPort
    // bus, answered by whatever the built-in slot 5 holds. Only the
    // 32 KB //c profile consults it (IIcClassProfile::ioReadIWM).
    extSmartPort_ = std::make_unique<pom2::IIcExternalSmartPort>(&mem.slotBus());
    mem.setExternalSmartPort(extSmartPort_.get());
    mem.setNoSlotClock(noSlotClock_.get());
}

EmulationController::~EmulationController()
{
    exitRequested.store(true);
    wakeCv.notify_all();
#ifndef __EMSCRIPTEN__
    if (worker.joinable()) worker.join();
#endif

    // Tear down audio first so the callback thread is drained before the
    // sources it's pulling from go away.
    if (audioDev && tape)      audioDev->removeSource(tape.get());
    if (audioDev && spk)       audioDev->removeSource(spk.get());
    if (audioDev && floppy525) audioDev->removeSource(floppy525.get());
    if (audioDev && floppy35)  audioDev->removeSource(floppy35.get());
    audioDev.reset();
    mem.setCassetteDevice(nullptr);
    mem.setSpeakerDevice(nullptr);
    mem.setCpu(nullptr);
    mem.setIWM(nullptr);
    mem.setSmartPortHub(nullptr);
    mem.setExternalSmartPort(nullptr);
    mem.setNoSlotClock(nullptr);
    tape.reset();
    spk.reset();
    floppy525.reset();
    floppy35.reset();
    iwmDev.reset();
    // Order matters: hub holds raw pointers to the drives, drives hold
    // raw pointers to the images. Tear down in reverse-attach order so
    // no dangling pointers escape into the audio/UI thread.
    hub.reset();
    drive35Int.reset();
    drive35Ext.reset();
    image35Int.reset();
    image35Ext.reset();
}

// ─── Cassette transport ───────────────────────────────────────────────────

namespace {

/// Two-phase tape file I/O, for the reason in MediaMount.h: `stateMtx` is
/// held by the CPU worker every 4096-cycle chunk and by the UI thread on
/// every frame, and a tape load is not a small read — `.mp3/.ogg/.flac` go
/// through `CassetteDevice::loadMiniaudioTape`, which decodes the WHOLE file
/// (up to 30 emulated minutes) and runs pulse extraction over every frame.
/// Holding the lock across that froze the machine and the window together,
/// exactly like the 800K read `mount35` below moved out of the lock.
///
/// The deck cannot be staged the way a disk image is — it is one live object
/// wired to the bus, not a payload that can be decoded detached and then
/// swapped in — so the phases invert: unhook it from the bus (that is the
/// only thing the CPU worker reaches it through, and every use site in
/// Memory is null-guarded), let the file work run with the lock released,
/// then hook it back. The machine keeps executing throughout, blind to the
/// cassette port for the duration of the load, and the UI thread can still
/// take the lock to paint.
///
/// The audio thread is untouched by this: it reaches the deck through
/// `AudioDevice`, never through `stateMtx`, so it raced the loader exactly
/// the same way before (CassetteDevice guards that side with `audioMutex` /
/// `audioStreamMutex`).
///
/// RAII so an exception escaping the decoder cannot leave the deck off the
/// bus.
class TapeOffBus
{
public:
    TapeOffBus(std::mutex& mtx, Memory& mem, CassetteDevice* dev)
        : mtx_(mtx), mem_(mem), dev_(dev)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        mem_.setCassetteDevice(nullptr);
    }
    ~TapeOffBus()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        mem_.setCassetteDevice(dev_);
    }
    TapeOffBus(const TapeOffBus&)            = delete;
    TapeOffBus& operator=(const TapeOffBus&) = delete;

private:
    std::mutex&     mtx_;
    Memory&         mem_;
    CassetteDevice* dev_;
};

} // namespace

bool EmulationController::loadTape(const std::string& path)
{
    TapeOffBus offBus(stateMtx, mem, tape.get());
    return tape->loadTape(path);
}
bool EmulationController::saveTape(const std::string& path)
{
    // Same shape: the write serialises the recorded transition list, which
    // the CPU worker appends to on every $C020 toggle — off the bus it
    // cannot, so the snapshot the writer walks is stable without the lock.
    TapeOffBus offBus(stateMtx, mem, tape.get());
    return tape->saveTape(path);
}
void EmulationController::playTape()         { std::lock_guard<std::mutex> lk(stateMtx); tape->playTape(); }
void EmulationController::stopTape()         { std::lock_guard<std::mutex> lk(stateMtx); tape->stopTape(); }
void EmulationController::pauseTape(bool p)  { std::lock_guard<std::mutex> lk(stateMtx); tape->setPlaybackPaused(p); }
void EmulationController::rewindTape()       { std::lock_guard<std::mutex> lk(stateMtx); tape->rewindTape(); }
void EmulationController::ejectTape()        { std::lock_guard<std::mutex> lk(stateMtx); tape->ejectTape(); }
void EmulationController::clearTapeCapture() { std::lock_guard<std::mutex> lk(stateMtx); tape->clearRecordedTape(); }
void EmulationController::armRecording()     { std::lock_guard<std::mutex> lk(stateMtx); tape->armRecording(); }
void EmulationController::seekTapeRelative(double dt)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    tape->seekRelativeSeconds(dt);
}
void EmulationController::setCassetteVolume(float v) { tape->setVolume(v); }

// ─── 3.5" Sony SmartPort ──────────────────────────────────────────────────
// MAME `apple2e.cpp:4521-4524` instantiates `m_floppy[2..3]` as `add_35()`
// drives. POM2 collapses the "drive" + "image" into a single mount call
// per slot index.

bool EmulationController::mount35(int idx, const std::string& path)
{
    if (idx < 0 || idx > 1) return false;

    // Two-phase, for the reason in MediaMount.h: `stateMtx` is held by the CPU
    // worker every 4096-cycle chunk and by the UI thread on every frame, so
    // reading and GCR-decoding an 800K image inside it froze the machine and
    // the window together. Phase 1 is that read, with no lock held; phase 2
    // below is a move.
    //
    // Phase 1 needs one thing from the object — the write-back flag — plus a
    // hint about whether the read is worth doing at all: re-inserting the file
    // the guest has unsaved writes to would capture pre-flush bytes, and
    // installing those rolls the writes back.
    //
    // The hint is only a hint. It is read under one lock and acted on under
    // another, so the guest can dirty the medium in between; the decision that
    // MATTERS is re-taken below, under the same lock as the flush, and a
    // wasted phase-1 read is simply discarded. `DiskIICard::installDisk` does
    // not need this dance because its caller holds one lock across both steps.
    bool writeBack = false;
    bool skipRead  = false;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        pom2::Disk35Image* image = idx == 0 ? image35Int.get() : image35Ext.get();
        if (!image) return false;
        writeBack = image->isWriteBackEnabled();
        skipRead  = image->isLoaded() && image->hasUnsavedChanges() &&
                    !image->path().empty() && image->path() == path;
    }

    pom2::Disk35Image staged;
    staged.setWriteBackEnabled(writeBack);
    if (!skipRead && !staged.loadFile(path)) return false;

    // Phase 2.
    std::lock_guard<std::mutex> lk(stateMtx);
    pom2::Disk35Image*  image = idx == 0 ? image35Int.get() : image35Ext.get();
    pom2::Sony35Drive*  drive = idx == 0 ? drive35Int.get() : drive35Ext.get();
    if (!image || !drive) return false;

    // Decided HERE, under the lock that also does the flush, so no window
    // exists between the two.
    const bool staleAfterFlush =
        image->isLoaded() && image->hasUnsavedChanges() &&
        !image->path().empty() && image->path() == path;

    if (image->isLoaded() && image->hasUnsavedChanges() &&
        !image->saveDirty()) {
        return false;  // keep the only in-memory copy mounted for retry
    }
    if (staleAfterFlush || !staged.isLoaded()) {
        // Rare: the file the guest just wrote to. The flush above has landed,
        // so read it back here, under the lock, and pay the inline cost.
        staged = pom2::Disk35Image{};
        staged.setWriteBackEnabled(writeBack);
        if (!staged.loadFile(path)) return false;
    }
    *image = std::move(staged);
    drive->notifyMediaChange();
    // User-initiated mount → one-shot insert click. Same pattern as
    // `DiskIICard::insertDisk` (5.25"). Silent when no FloppySoundDevice
    // sink is wired (headless / tests).
    drive->emitInsertClick();
    return true;
}

// ─── Deferred 3.5" write-back queue ───────────────────────────────────────
// See EmulationController.h. `submit` runs with `stateMtx` held (the IWM
// strobed the drive's eject register on the CPU worker), so it does nothing
// but move a payload under its own uncontended mutex; `run` — a separate,
// guarded thread — does the file work with no machine lock in sight.

EmulationController::WriteBackQueue::~WriteBackQueue()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    // Join, do not detach: the queue may still hold the only copy of a
    // session's guest writes, and the process is about to exit.
    if (worker_.joinable()) worker_.join();
    // Anything still queued when the thread never started (or exited early)
    // is committed here rather than dropped.
    for (auto& pending : queue_) {
        std::string error;
        if (!pom2::Disk35Image::commitWriteBack(std::move(pending), error))
            pom2::log().warn("Sony35", "Deferred write-back failed: " + error);
    }
}

void EmulationController::WriteBackQueue::submit(
    pom2::Disk35Image::PendingWriteBack&& pending)
{
    if (!pending.valid) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        queue_.push_back(std::move(pending));
        if (!worker_.joinable()) {
            // Guarded: an exception escaping a std::thread callable calls
            // std::terminate() with no log line (CLAUDE.md § thread barrier).
            worker_ = pom2::guardedThread("Disk35WriteBack", [this] { run(); });
        }
    }
    cv_.notify_one();
}

void EmulationController::WriteBackQueue::run()
{
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
            if (stopping_) return;
            continue;
        }
        pom2::Disk35Image::PendingWriteBack pending = std::move(queue_.front());
        queue_.erase(queue_.begin());
        busy_ = true;
        lk.unlock();                       // ← the file I/O happens here
        std::string error;
        if (!pom2::Disk35Image::commitWriteBack(std::move(pending), error))
            pom2::log().warn("Sony35", "Deferred write-back failed: " + error);
        lk.lock();
        busy_ = false;
        idleCv_.notify_all();
    }
}

void EmulationController::WriteBackQueue::drain()
{
    std::unique_lock<std::mutex> lk(mtx_);
    idleCv_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

bool EmulationController::eject35(int idx)
{
    if (idx < 0 || idx > 1) return false;

    // Two-phase, for the reason in MediaMount.h and mount35 above: an 800 KB
    // rewrite plus two fsyncs under `stateMtx` freezes the CPU worker and the
    // UI thread together. Phase 1 lifts the payload out (a memcpy), phase 2
    // writes it with the lock released, phase 3 drops the medium.
    pom2::Disk35Image::PendingWriteBack pending;
    std::string pendingPath;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        pom2::Disk35Image* image = idx == 0 ? image35Int.get() : image35Ext.get();
        if (!image) return false;
        if (!image->isLoaded()) return true;      // already empty — no-op
        pending     = image->takeWriteBack();
        pendingPath = pending.path;
    }

    std::string error;
    const bool committed =
        !pending.valid ||
        pom2::Disk35Image::commitWriteBack(std::move(pending), error);

    std::lock_guard<std::mutex> lk(stateMtx);
    pom2::Disk35Image* image = idx == 0 ? image35Int.get() : image35Ext.get();
    pom2::Sony35Drive* drive = idx == 0 ? drive35Int.get() : drive35Ext.get();
    if (!image) return false;
    if (!committed) {
        // Refuse the eject and hand the writes back, so the user can fix the
        // cause and retry — the pre-split behaviour. Only if the SAME medium
        // is still in the bay: a mount that landed while phase 2 ran unlocked
        // owns the drive now, and re-dirtying it would write this disk's
        // blocks into that one's file.
        if (image->isLoaded() && image->path() == pendingPath)
            image->restoreDirty();
        pom2::log().warn("Sony35", "3.5\" eject refused: " + error);
        return false;
    }
    if (!image->isLoaded()) return true;          // ejected under us — done
    image->eject();
    if (drive) {
        drive->notifyMediaChange();
        // Mechanical click on user-initiated eject — pairs with
        // the click emitted by `mount35` above.
        drive->emitInsertClick();
    }
    return true;
}

void EmulationController::start()
{
    // "Start" means "make the machine run": resume execution AND, on the
    // first call, spawn the worker thread. Re-arming the mode here is the
    // load-bearing part — applyProfile() and restartEmulationFromSettings()
    // both do stop()…rebuild cards…start() while the worker is ALREADY
    // live, so start() can't re-spawn the thread (worker.joinable() short-
    // circuits below). Without setting Running here the machine would stay
    // Stopped after a profile/slot switch and sit frozen on a garbage
    // (HOME-never-ran) text page — the "doesn't boot on launch" bug, since
    // a saved non-default profile auto-applies on startup. Keeps stop()/
    // start() symmetric: stop() parks the mode, start() un-parks it.
    // Same invariant as setMode(): clear workerParked_ on the setter thread
    // BEFORE mode leaves Stopped, or a later stop()/rewind-scrub can read
    // the stale `true` left by the previous park and proceed mid-frame.
    workerParked_.store(false);
    mode.store(Mode::Running);
    wakeCv.notify_all();
#ifndef __EMSCRIPTEN__
    if (worker.joinable()) return;
    // Guarded: an exception escaping workerLoop() would call std::terminate()
    // and take the process with it, silently. workerLoop() captures rewind
    // frames — multi-MB vector growth against a 256 MiB budget — so bad_alloc
    // is a live possibility, not a theoretical one. On the way out (clean or
    // not) publish a coherent state: Stopped and parked, so waitUntilParked()
    // returns at once instead of burning its 200-step poll on a dead thread.
    worker = std::thread([this] {
        pom2::runGuarded("Emulation", [this] { workerLoop(); });
        mode.store(Mode::Stopped);
        workerParked_.store(true);
        wakeCv.notify_all();
    });
#endif
    // Under Emscripten the browser owns the frame schedule — the host
    // calls tickFrame() once per RAF. No worker thread is spawned.
}

void EmulationController::tickFrame()
{
    const Mode m = mode.load();
    if (m == Mode::Stopped) {
        return;
    }
    if (m == Mode::Step) {
        const int n = stepsPending.exchange(0);
        if (n > 0) {
            std::lock_guard<std::mutex> lk(stateMtx);
            for (int i = 0; i < n; ++i) {
                stepBusMaster();
            }
        }
        mode.store(Mode::Stopped);
        return;
    }
    // Mode::Running — same chunking as workerLoop (4 KiB cycles per
    // lock-hold) so any host code that grabs stateMtx between frames
    // still gets fair access mid-budget. On WASM there's only one
    // thread, but the lock is cheap and keeps the lock-discipline
    // contract identical to the threaded path.
    constexpr int kLockChunkCycles = 4096;
    // WALL-CLOCK PACING. This path is driven by the browser's rAF
    // (`emscripten_set_main_loop_arg(..., fps = 0, ...)`), i.e. once per
    // DISPLAY refresh — which has nothing to do with the emulated
    // machine's refresh. Burning a full `cyclesPerFrame` budget per call
    // ran a PAL profile at 20313 × 60 = 1.22 MHz on a standard 60 Hz
    // panel: 20 % over-clocked, guest VBL at 60.1 Hz instead of 50.08.
    // (NTSC had the same hazard on 120/144 Hz panels.) Scale the budget
    // by how much wall time actually elapsed, in units of the machine's
    // own frame interval, so the emulated clock tracks real time on any
    // display. The threaded path doesn't need this — workerLoop sleeps to
    // an absolute deadline.
    int64_t budget = scaledFrameBudget();          // int64: see workerLoop note
    // WASM ONLY. The browser is the only caller that drives this off a
    // display refresh; every other caller is a HEADLESS TEST, where "one
    // call = one full frame budget" is the contract. Scaling there would
    // collapse the budget to ~1 cycle (test loops complete in
    // microseconds), so a test expecting CPU progress would silently get
    // none — a trap for any future test, and two existing ones survive
    // only by accident (one calls tickFrame exactly once, the other
    // measures per-call rewind captures rather than cycles).
#ifdef __EMSCRIPTEN__
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastTickWall_.time_since_epoch().count() != 0) {
            const auto elapsedUs = std::chrono::duration_cast<
                std::chrono::microseconds>(now - lastTickWall_).count();
            const int64_t intervalUs = frameIntervalUs.load();
            if (intervalUs > 0 && elapsedUs > 0) {
                // Cap at 4 frames' worth so a backgrounded tab (or a
                // breakpoint) doesn't dump seconds of emulated time into
                // one call and freeze the page.
                const int64_t scaled =
                    budget * std::min<int64_t>(elapsedUs, intervalUs * 4)
                           / intervalUs;
                budget = std::max<int64_t>(scaled, 1);
            }
        }
        lastTickWall_ = now;
    }
#endif
    for (int64_t done = 0; done < budget; ) {
        const int chunk = static_cast<int>(std::min<int64_t>(kLockChunkCycles, budget - done));
        std::lock_guard<std::mutex> lk(stateMtx);
        const int actually = runCpuSlice(chunk);
        done += (actually > 0 ? actually : chunk);
    }
    if (iwmDev) {
        std::lock_guard<std::mutex> lk(stateMtx);
        iwmDev->tick(mem.getCycleCounter());
    }
    // Rewind capture for the single-threaded (WASM) path — same quiescent
    // frame-boundary hook as the worker loop. Gated on enabled() so a
    // disabled ring costs nothing.
    if (rewind_.enabled()) {
        std::lock_guard<std::mutex> lk(stateMtx);
        rewind_.capture(processor, mem);
    }
}

void EmulationController::stop()
{
    setMode(Mode::Stopped);
    // Block until the worker has ACTUALLY parked — this is a hard
    // guarantee, not best-effort. stop()'s callers — applyProfile /
    // restartEmulationFromSettings / ~MainWindow — proceed to tear down
    // slot cards, reload ROMs and re-plug the SlotBus largely OUTSIDE
    // stateMutex; if this returned with the worker still in flight (as
    // the earlier bounded 200 ms poll could on a loaded host or behind a
    // long-held stateMutex), the rebuild would race processor.run() —
    // use-after-free on the SlotBus. waitUntilParked()'s bounded poll is
    // kept for the rewind scrub path, where overrunning is recoverable
    // and UI responsiveness wins.
    //
    // Termination: the worker re-checks `mode` between 4096-cycle chunks
    // (see workerLoop) and the per-frame budget is capped (CLI --speed
    // and AI /speed both clamp to 2 M cycles), so parking is bounded in
    // practice; the warn below fires only if something is genuinely
    // wedged — better a diagnosed hang than silent memory corruption.
    //
    // Deadlock-safety: the caller must NOT hold stateMutex() here (no
    // current caller does) — the worker needs that lock to finish its
    // current chunk.
    if (!worker.joinable()) return;
    int waitedMs = 0;
    while (!workerParked_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++waitedMs % 1000 == 0) {
            pom2::log().warn("Emul",
                "stop(): worker not parked after " +
                std::to_string(waitedMs) + " ms — still waiting");
        }
    }
}

void EmulationController::hardReset()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    // Same soft-switch policy as softReset (MAME reset_w / machine_reset):
    // II/II+ preserve LC + display switches; IIe-class wipes MMU/IOU/LC.
    // Pre-fix hardReset called resetSoftSwitches() unconditionally, which
    // forced TEXT+no NTSC on every F12 even on II/II+.
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
    if (tape)   tape->resetCpuSide();
    // A step-over / run-to-cursor transient is armed at an address in the
    // code that WAS running. After a reset (F12, and applyProfile step 11,
    // which is a profile switch) that address means nothing, but the
    // transient stays armed and fires the first time the PC happens past it
    // — minutes later, with no visible cause. Disarm it and re-sync, because
    // `armed()` counts the transient: dropping it may be what detaches the
    // hook and puts the CPU back on its fast loop.
    if (debugger_) { debugger_->clearTransient(); syncDebugHook(); }
    processor.hardReset();
    pom2::log().info("Emul", "Hard reset");
}

void EmulationController::softReset()
{
    // Real Apple II Ctrl-Reset: the reset line is asserted briefly. The
    // 6502 latches PC from $FFFC, sets the I flag, and decrements SP by 3
    // (the reset sequence simulates a BRK push of PC + P without storing).
    // RAM, A/X/Y, and the zero page survive. Slot cards see their reset
    // line too — Disk II spins down, Le Chat Mauve FIFO returns to its
    // power-on default, etc. Soft-switch policy depends on the profile:
    // II/II+ machine_reset preserves LC + display (MAME `apple2.cpp:325-
    // 331`); IIe/IIc/IIc+ reset_w wipes the MMU/IOU/LC list (MAME
    // `apple2e.cpp:1453-1508`). resetSoftSwitchesWarm() applies the
    // right one based on iieMode.
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    // SmartPort hub state — MAME re-asserts m_35sel=false and
    // m_intdrive=false on every reset (`apple2e.cpp:1266`). Without
    // this (E-3-1), Ctrl-Reset would keep the //c+ alt firmware's
    // last drive selection across the warm-reset boundary.
    if (hub) hub->reset();
    processor.softReset();
    pom2::log().info("Emul", "Soft reset (Ctrl-Reset)");
}

void EmulationController::coldBoot()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();   // F-1-3: parity with hardReset
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
    if (tape)   tape->resetCpuSide();
    // As in hardReset(): the transient names an address in a program that no
    // longer exists — here its RAM has literally been wiped.
    if (debugger_) { debugger_->clearTransient(); syncDebugHook(); }
    processor.hardReset();
    rewind_.clear();   // RAM wiped → the recorded timeline is a different machine
    scrubIndex_.store(pom2::RewindBuffer::kNoFrame);
    pom2::log().info("Emul", "Cold boot (RAM wiped)");
}

bool EmulationController::bootFromSlot(int slot)
{
    if (slot < 1 || slot > 7) return false;
    std::lock_guard<std::mutex> lk(stateMtx);
    // Explicit GUI/CLI boot — arm the //c-class on-board SmartPort so its
    // $C500 firmware stub becomes visible for the signature check + boot
    // below (every reset/cold-boot disarms it again). See Memory::
    // setIicSmartPortArmed + project_iic_smartport_boot. No-op off //c-class.
    // ONLY when booting slot 5 itself: arming during e.g. a slot-6 5.25"
    // Library boot on a //c with SmartPort media mounted re-creates the
    // dual-device confusion the gate exists to prevent (booted ProDOS may
    // call real-firmware $C5xx entries the stub lacks — "garbled banner").
    mem.setIicSmartPortArmed(slot == 5);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
    // Same CPU-side cassette wipe hardReset() and coldBoot() do: the output
    // flip-flop and the cycle timebase are clobbered by the reset line, the
    // tape position and recording buffer are not (a real deck does not
    // rewind because the computer was reset). bootFromSlot is documented as
    // "coldBoot-equivalent, inlined" (CLAUDE.md § Reset architecture) and
    // this was the one line the inlining dropped — a boot from the Library
    // left `lastOutputToggleCycle` in the future of a rebased `currentCycle`,
    // so the first $C020 toggle after it recorded a wrapped, huge pulse.
    if (tape)   tape->resetCpuSide();
    // Any step-over / run-to-cursor transient belongs to the machine that
    // just went away — see hardReset().
    if (debugger_) { debugger_->clearTransient(); syncDebugHook(); }
    rewind_.clear();   // RAM wiped → the recorded timeline is a different machine
    scrubIndex_.store(pom2::RewindBuffer::kNoFrame);
    // Card-has-boot-entry sanity check. Apple II Ref Manual Appx C
    // describes 4 signature bytes ($Cn01=$20, $Cn03=$00, $Cn05=$03,
    // $Cn07=$3C); the F8 Autostart Monitor (Apple part 341-0020-00)
    // checks ALL four and only auto-scans Disk II / SmartPort
    // ($Cn07=$3C). HDV uses $Cn07=$01 (ProDOS non-removable) and
    // SmartPort uses $Cn07=$3C; both carry the JSR-dispatch trio at
    // $Cn01/03/05. The user-initiated "Boot" GUI button bypasses the
    // F8 scan deliberately — it's the WHOLE POINT of bootFromSlot
    // (the F8 firmware refuses to scan HDV cards, so the user clicking
    // "Boot HDV" needs this shortcut). We therefore validate only the
    // JSR trio, NOT the $Cn07=$3C marker. Theme 8 audit (gap B-2-2)
    // originally required $Cn07=$3C too, but that broke HDV boot —
    // see DEV.md § Storage § DiskIICard for the bootFromSlot rationale.
    const uint16_t cnxx = static_cast<uint16_t>(0xC000 + slot * 0x100);
    const uint8_t b1 = mem.memRead(static_cast<uint16_t>(cnxx + 1));
    const uint8_t b3 = mem.memRead(static_cast<uint16_t>(cnxx + 3));
    const uint8_t b5 = mem.memRead(static_cast<uint16_t>(cnxx + 5));
    const bool hasBootDispatch =
        (b1 == 0x20) && (b3 == 0x00) && (b5 == 0x03);
    // //c+ with a device answering on its rear connector: the real $C500
    // carries a valid signature but, entered here rather than from reset,
    // never scans the port — it reports UNABLE TO FIND A BOOTABLE DISK.
    // Its reset-time scan ($F223) does find the device and boots it, so an
    // explicit boot of slot 5 is a reset there (bug hunt 3). The plain //c's
    // $C500 boots when entered directly and keeps this path.
    if (slot == 5 && mem.iicPlusBootsSlot5ByReset()) {
        pom2::log().info("Emul",
            "Slot 5 on a //c+ is served by its own firmware over the "
            "SmartPort bus — booting through the ROM's reset scan");
        mem.setIicSmartPortArmed(false);
        processor.hardReset();
        workerParked_.store(false);
        mode.store(Mode::Running);
        wakeCv.notify_all();
        return true;
    }
    if (!hasBootDispatch) {
        pom2::log().warn("Emul",
            "Slot " + std::to_string(slot) +
            " has no Apple-II JSR-dispatch signature at $Cn01/03/05 — "
            "the card isn't bootable. Falling back to cold boot so the "
            "F8 ROM can scan for a different bootable slot.");
        // Honour the "every reset disarms" invariant on this path too —
        // the //c F8 autostart must see its real $C500 firmware.
        mem.setIicSmartPortArmed(false);
        processor.hardReset();
        workerParked_.store(false);  // same setter-thread invariant as setMode()
        mode.store(Mode::Running);
        wakeCv.notify_all();
        return false;   // the machine is running, but NOT off this card
    }
    // Prime text page 1 with $A0 (space + high bit set) — what the Monitor
    // ROM's HOME routine would write. We force PC into the slot ROM here
    // instead of going through the Monitor cold-boot (which would scan
    // slots and pick slot 6 if a floppy is mounted there), so the user
    // would otherwise see freshly-zeroed RAM render as a screen full of
    // `@`-tile garbage for several seconds while the booting card loads.
    for (uint16_t a = 0x0400; a <= 0x07FF; ++a) {
        mem.memWrite(a, 0xA0);
    }
    // Replicate the F8 Autostart Monitor's text + I/O zero-page setup. The
    // normal boot reaches the slot ROM via the Monitor cold-start (SETNORM/
    // INIT/SETTXT/VTAB), which initialises the text-window and I/O-hook
    // zero page; bootFromSlot jumps straight to $Cn00 and skips it. Without
    // this, a boot's RWTS that calls Monitor screen routines hits garbage:
    // e.g. CLREOP ($FCA0) loops on `CPY WNDWDTH($21)` — with $21 left at 0
    // it overruns the line and clobbers the boot loader in $0800, hanging
    // the Mr. Robot 4am crack (and any timing/screen-sensitive loader).
    // Values verified against a clean autostart-to-BASIC dump.
    mem.memWrite(0x0020, 0x00);   // WNDLFT  = 0
    mem.memWrite(0x0021, 0x28);   // WNDWDTH = 40
    mem.memWrite(0x0022, 0x00);   // WNDTOP  = 0
    mem.memWrite(0x0023, 0x18);   // WNDBTM  = 24
    mem.memWrite(0x0024, 0x00);   // CH (cursor column)
    mem.memWrite(0x0025, 0x00);   // CV (cursor row)
    mem.memWrite(0x0028, 0x00);   // BASL ┐ text base = $0400 (page 1, row 0)
    mem.memWrite(0x0029, 0x04);   // BASH ┘
    mem.memWrite(0x0032, 0xFF);   // INVFLG = normal video
    mem.memWrite(0x0036, 0xF0);   // CSWL ┐ output hook → COUT1 ($FDF0)
    mem.memWrite(0x0037, 0xFD);   // CSWH ┘
    mem.memWrite(0x0038, 0x1B);   // KSWL ┐ input hook  → KEYIN ($FD1B)
    mem.memWrite(0x0039, 0xFD);   // KSWH ┘
    processor.hardReset();
    processor.setProgramCounter(cnxx);
    workerParked_.store(false);  // same setter-thread invariant as setMode()
    mode.store(Mode::Running);
    wakeCv.notify_all();
    pom2::log().info("Emul",
        "Boot via slot " + std::to_string(slot) + " ROM ($C" +
        std::to_string(slot) + "00)");
    return true;
}

void EmulationController::requestStep(int n)
{
    if (n <= 0) return;
    // Counter, not a boolean: callers (e.g. CLI `--step N`) may queue many
    // steps in a burst; a boolean would coalesce them into a single step.
    stepsPending.fetch_add(n);
    setMode(Mode::Step);
    wakeCv.notify_all();
}

void EmulationController::setMode(Mode m)
{
    // Clear the parked flag the instant we leave Stopped, on the *setter*
    // thread — not later on the worker. Otherwise a resume→rescrub burst
    // (rewindEndAndResume → rewindBeginScrub) could read a stale `true` left
    // over from the previous park, before the worker has observed the new
    // Running mode and cleared it itself, and waitUntilParked() would return
    // while a Running frame is still about to run. Only the worker ever sets
    // it back to true, and only once it genuinely re-enters the Stopped wait.
    if (m != Mode::Stopped) workerParked_.store(false);
    // Leaving Stopped ends any scrub, whoever asked. `rewindEndAndResume` is
    // only ONE of the ways the machine resumes — the toolbar Play button,
    // Machine > Run, the `machine.run` palette command and the kiosk menu all
    // land here instead, and they used to leave the scrub flagged as live.
    // Clearing it here (rather than at each of those sites) is what keeps the
    // UI honest for resume paths that do not exist yet. The abandoned future
    // itself is dropped by the next `RewindBuffer::capture`, which is on the
    // worker and therefore cannot deadlock against callers that reach setMode
    // while already holding stateMtx (the Disk II Library's boot buttons do).
    if (m != Mode::Stopped) scrubIndex_.store(pom2::RewindBuffer::kNoFrame);
    // Stop disarms any step-over / run-to-cursor transient. The user asked the
    // machine to halt, so a breakpoint they never placed must not survive to
    // halt it again minutes later, at an address that belonged to a step they
    // abandoned. `Debugger::clearTransient()` is the ONE method of that class
    // callable without `stateMutex`, for exactly this site: setMode is reached
    // by callers that already hold the lock (the Disk II Library boot buttons)
    // and by the CPU worker itself through `noteDebuggerStop`, and the lock is
    // not recursive — so `transientArmed_` is an atomic instead.
    //
    // The hook is deliberately NOT re-synced here (that needs the lock): if
    // this was the only thing armed, the CPU keeps its debugged loop until the
    // next syncDebugHook, which costs a predictable branch per instruction on
    // a machine that is stopped anyway.
    if (m == Mode::Stopped && debugger_) debugger_->clearTransient();
    mode.store(m);
    wakeCv.notify_all();
}

// ─── Rewind transport ──────────────────────────────────────────────────────
void EmulationController::waitUntilParked()
{
    // No worker thread (e.g. headless construction without start()) → nothing
    // can run the CPU, so a restore is already safe.
    if (!worker.joinable()) return;
    // The worker parks within one frame (≤ ~16 ms at 60 Hz; sooner under
    // turbo). Bounded spin so a stuck worker can't hang the UI thread.
    for (int i = 0; i < 200 && !workerParked_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool EmulationController::rewindBeginScrub()
{
    if (!rewind_.enabled()) return false;
    setMode(Mode::Stopped);
    waitUntilParked();
    std::lock_guard<std::mutex> lk(stateMtx);
    if (rewind_.empty()) return false;
    scrubIndex_.store(rewind_.size() - 1);
    return true;
}

size_t EmulationController::rewindSeek(size_t index)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    if (rewind_.empty()) return pom2::RewindBuffer::kNoFrame;
    const size_t clamped = std::min(index, rewind_.size() - 1);
    if (!rewind_.restore(clamped, processor, mem))
        return pom2::RewindBuffer::kNoFrame;
    scrubIndex_.store(clamped);
    flushAudioForRewind();
    return clamped;
}

size_t EmulationController::rewindSeekToCycle(uint64_t cycle)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    const size_t got = rewind_.restoreToCycle(cycle, processor, mem);
    if (got != pom2::RewindBuffer::kNoFrame) {
        scrubIndex_.store(got);
        flushAudioForRewind();
    }
    return got;
}

void EmulationController::rewindEndAndResume(size_t index)
{
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        if (index < rewind_.size()) {
            // Make the live machine exactly the cursor frame, then drop the
            // abandoned future so new captures append from here.
            if (!rewind_.restore(index, processor, mem)) {
                setMode(Mode::Running);
                return;
            }
            rewind_.truncateAfter(index);
            flushAudioForRewind();
        }
    }
    setMode(Mode::Running);
}

void EmulationController::flushAudioForRewind()
{
    // Caller holds stateMtx. Jumping the CPU/RAM back in time leaves the
    // speaker's 1-bit reconstruction holding samples from a future that no
    // longer happens; reset it so a scrub/rewind is silent instead of
    // popping, and the resumed timeline starts clean. (Deeper sound-chip
    // continuity — Mockingboard AY/VIA mid-note — is a separate follow-up;
    // those cards don't yet serialize their state.)
    if (spk) spk->reset();
}

void EmulationController::rewindEndPaused()
{
    // Stay Stopped at the current frame; nothing to do but keep the worker
    // parked. Provided as a named counterpart to rewindEndAndResume so the
    // UI's intent is explicit.
    setMode(Mode::Stopped);
}

// NOT inside the __EMSCRIPTEN__ guard: tickFrame — the browser-RAF CPU
// driver on WASM — routes its chunks through here too. (2026-07-12 bug
// hunt: defining this below, worker-only, broke the wasm link.)
int64_t EmulationController::scaledFrameBudget() const
{
    const int64_t base = cyclesPerFrame.load();
    const double  mul  = mem.slotBus().cpuSpeedMultiplier();
    if (mul == 1.0) return base;
    // Guard the arithmetic, not the caller: the multiplier comes from a
    // card and the base from --speed / the AI /speed endpoint, and their
    // product is the only place in the loop where either could overflow.
    const double scaled = static_cast<double>(base) * mul;
    constexpr double kCeil = 1.0e9;
    if (scaled <= 1.0)   return 1;
    if (scaled >= kCeil) return static_cast<int64_t>(kCeil);
    return static_cast<int64_t>(scaled);
}

int EmulationController::runCpuSlice(int chunk)
{
    // DMA daisy chain (SoftCard Z80): while a card claims the bus the
    // 6502 stays parked and the card's processor burns the budget. The
    // claimant scan is 8 virtual calls per 4096-cycle chunk — noise.
    if (SlotPeripheral* dma = mem.slotBus().dmaClaimant()) {
        const int spent = dma->dmaRun(chunk);
        // A card that flips off mid-slice returns what it consumed; the
        // remainder of the chunk goes back to the 6502 so the hand-back
        // doesn't cost a whole chunk of dead time.
        if (spent < chunk && !dma->dmaActive())
            return spent + processor.run(chunk - spent);
        return spent;
    }
    const int spent = processor.run(chunk);
    // A debugger stop ends the slice early. Handled HERE, in the one funnel
    // both drivers (worker thread and the WASM RAF tick) go through, rather
    // than duplicated into each: the worker's inner loop re-reads `mode` at
    // the top of every iteration, so parking here stops it within one chunk.
    if (debugger_ && debugger_->stopRequested()) {
        noteDebuggerStop();
        return spent;
    }
    // Symmetric hand-over: the 6502 yields mid-chunk when its $CnXX
    // write grants the bus (the card calls M6502::stop()); give the
    // remainder to the new claimant instead of burning a dead chunk.
    if (spent < chunk) {
        if (SlotPeripheral* dma = mem.slotBus().dmaClaimant())
            return spent + dma->dmaRun(chunk - spent);
    }
    return spent;
}

void EmulationController::noteDebuggerStop()
{
    // The CPU halted with the PC still on the breakpoint instruction, which
    // is the whole point: the register dump the user reads is the state going
    // IN to it, and resuming re-tries it rather than skipping it.
    setMode(Mode::Stopped);
}

void EmulationController::syncDebugHook()
{
    processor.setDebugHook(debugger_->armed() ? debugger_.get() : nullptr);
    // Watchpoints are not a hook Memory calls on every access — a write
    // watch is an address DIVERTED off memWrite's fast path, a read watch
    // flips the one flag that diverts every read (Memory.h § Write / § Read
    // watchpoints) — so Memory keeps tables that have to follow the
    // debugger's. Rebuilt wholesale rather than incrementally: this runs on a
    // UI edit and never on the CPU's path, and one authority for the tables
    // beats several that can drift.
    mem.clearWriteWatches();
    mem.clearReadWatches();
    for (const auto& w : debugger_->watchpoints()) {
        if (w.access & pom2::Debugger::Write) mem.setWriteWatch(w.addr, true);
        if (w.access & pom2::Debugger::Read)  mem.setReadWatch(w.addr, true);
    }
}

void EmulationController::debugResume()
{
    // Amnesty for exactly one instruction at the current PC. Without it Run
    // would re-trigger the breakpoint the machine is standing on and nothing
    // would ever move.
    debugger_->armResumeFrom(processor.getProgramCounter());
    debugger_->clearHit();
}

void EmulationController::debugStepInstruction()
{
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        debugResume();
    }
    requestStep(1);
}

void EmulationController::debugStepOver()
{
    uint16_t resumeAt = 0;
    bool     over     = false;
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        const uint16_t pc = processor.getProgramCounter();
        // $20 = JSR, the only 6502 instruction with a subroutine to step over.
        // Everything else — including JMP, which does not come back — is an
        // ordinary single step, because there is nothing to step over.
        //
        // peekMainRam, not memRead: a debugger must never perturb the machine
        // it is inspecting, and memRead on a $C0xx address FLIPS SOFT
        // SWITCHES. It reads main RAM only, which is the same view the
        // Disasm panel and MemoryViewer already show — so on a //e running
        // code out of aux, or under a Language Card bank, this can misread
        // the opcode. The failure is benign in both directions: a missed JSR
        // becomes a single step, and a phantom JSR arms a transient that
        // simply never fires, leaving the user to press Stop.
        if (mem.peekMainRam(pc) == 0x20) {
            resumeAt = static_cast<uint16_t>(pc + 3);
            over     = true;
            debugger_->setTransient(resumeAt, pom2::Debugger::Reason::StepOver);
            debugResume();
            syncDebugHook();
        } else {
            debugResume();
        }
    }
    if (over) setMode(Mode::Running);
    else      requestStep(1);
}

void EmulationController::debugRunToCursor(uint16_t addr)
{
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        debugger_->setTransient(addr, pom2::Debugger::Reason::RunToCursor);
        debugResume();
        syncDebugHook();
    }
    setMode(Mode::Running);
}

// One single-instruction step of whichever CPU owns the bus. The
// debugger/CLI/AI Step verbs go through here: on a DMA-halted bus a real
// 6502 executes nothing, so stepping while the SoftCard Z80 owns the bus
// must advance the Z80 — stepping the parked 6502 instead ran its
// post-hand-back continuation early and desynced the 6502↔Z80 handshake
// (2026-07-12 bug hunt). dmaRun(1) retires exactly one Z80 instruction:
// the per-instruction loop exits as soon as spent >= 1.
void EmulationController::stepBusMaster()
{
    if (SlotPeripheral* dma = mem.slotBus().dmaClaimant())
        dma->dmaRun(1);
    else
        processor.step();   // step() runs mem.advanceCycles internally
}

#ifndef __EMSCRIPTEN__
void EmulationController::workerLoop()
{
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now();

    // ── Hang detector (POM2_TRACE_HANG=1) ──────────────────────────────────
    // Diagnostic for deterministic freezes (e.g. Nox Archaist hanging when
    // entering a city): if the CPU stays confined to a small PC window for a
    // few seconds it is spinning in a wait loop. We disassemble that loop so
    // the operand reveals exactly which hardware register the game is polling
    // forever ($C019 VBL / $C0EC disk data / $C0D3 HDV status / $C000 kbd …),
    // which pinpoints the missing/incorrect emulation. Near-zero cost when off.
    const bool hangTrace = std::getenv("POM2_TRACE_HANG") != nullptr;
    if (hangTrace) mem.setIoReadTrace(true);
    constexpr int kHangSamples = 180;        // ~3 s at 60 Hz
    uint16_t pcRing[kHangSamples] = {0};
    int  pcRingCount = 0;
    int  pcRingHead  = 0;
    bool hangDumped     = false;
    int  framesConfined = 0;
    auto dumpHang = [&](uint16_t lo, uint16_t hi, bool repeat) {
        std::lock_guard<std::mutex> lk(stateMtx);
        std::fprintf(stderr,
            "\n*** POM2 %s — CPU confined to $%04X..$%04X ***\n",
            repeat ? "STILL FROZEN (permanent loop — this is the real freeze)"
                   : "HANG DETECTED (could be a slow chunk; watch for STILL FROZEN)",
            lo, hi);
        std::fprintf(stderr,
            "  A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X  cycles=%llu\n",
            processor.getAccumulator(), processor.getXRegister(),
            processor.getYRegister(), processor.getStackPointer(),
            processor.getStatusRegister(), processor.getProgramCounter(),
            static_cast<unsigned long long>(mem.getCycleCounter()));
        const uint16_t mm = mem.iieModeFlags();
        std::fprintf(stderr,
            "  //e paging: 80STORE=%d RAMRD=%d RAMWRT=%d INTCXROM=%d ALTZP=%d "
            "80COL=%d (flags=$%04X)\n",
            (mm & Memory::MF_80STORE) ? 1 : 0, (mm & Memory::MF_RAMRD) ? 1 : 0,
            (mm & Memory::MF_RAMWRT) ? 1 : 0, (mm & Memory::MF_INTCXROM) ? 1 : 0,
            (mm & Memory::MF_ALTZP) ? 1 : 0, (mm & Memory::MF_80COL) ? 1 : 0,
            static_cast<unsigned>(mm));
        const uint16_t start = (lo >= 3) ? static_cast<uint16_t>(lo - 3) : 0;
        const uint16_t end   = static_cast<uint16_t>(hi + 8);
        std::fprintf(stderr, "  loop bytes (as currently paged; may be aux RAM):\n");
        for (uint32_t a = start; a <= end; a += 16) {
            std::fprintf(stderr, "    $%04X:", static_cast<unsigned>(a));
            for (uint32_t k = 0; k < 16 && (a + k) <= end; ++k)
                std::fprintf(stderr, " %02X",
                             mem.memRead(static_cast<uint16_t>(a + k)));
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
            "  recent $C0xx reads (addr(label)xcount, most-polled first):\n    %s\n",
            mem.recentIoReadSummary().c_str());
        // Zero-page pointers used by the decompressor + the buffer it reads,
        // so we can compare the in-RAM compressed data against the .hdv to
        // tell data-corruption (disk/paging bug) from a pure logic/CPU bug.
        const uint8_t z04 = mem.memRead(0x04), z05 = mem.memRead(0x05);
        const uint8_t z06 = mem.memRead(0x06), z07 = mem.memRead(0x07);
        const uint8_t zEA = mem.memRead(0xEA), zEB = mem.memRead(0xEB);
        const uint8_t zEC = mem.memRead(0xEC), zED = mem.memRead(0xED);
        std::fprintf(stderr,
            "  ZP: $04=%02X $05=%02X $06=%02X $07=%02X  out($EA/EB)=%02X%02X"
            "  in($EC/ED)=%02X%02X\n",
            z04, z05, z06, z07, zEB, zEA, zED, zEC);
        const uint16_t inPtr = static_cast<uint16_t>(zEC | (zED << 8));
        std::fprintf(stderr, "  bytes around input ptr $%04X (current paging):\n",
                     inPtr);
        for (int row = -16; row < 32; row += 16) {
            const uint16_t base = static_cast<uint16_t>(inPtr + row);
            std::fprintf(stderr, "    $%04X:", base);
            for (int k = 0; k < 16; ++k)
                std::fprintf(stderr, " %02X",
                             mem.memRead(static_cast<uint16_t>(base + k)));
            std::fprintf(stderr, "\n");
        }
        // Decisive: the SAME input addresses in BOTH physical banks. Tells us
        // whether the real (compressed, non-$00FF) data is in main or aux —
        // i.e. whether the decompressor is reading the WRONG bank, or the data
        // was never written to the bank it expects.
        const uint8_t* mn = mem.data();
        const uint8_t* ax = mem.auxData();
        std::fprintf(stderr, "  input region MAIN vs AUX (which bank holds real data?):\n");
        for (int row = -16; row < 32; row += 16) {
            const uint16_t base = static_cast<uint16_t>(inPtr + row);
            std::fprintf(stderr, "    $%04X main:", base);
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %02X", mn[(base + k) & 0xFFFF]);
            std::fprintf(stderr, "\n            aux :");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %02X", ax[(base + k) & 0xFFFF]);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    };

    while (!exitRequested.load()) {
        const Mode m = mode.load();
        if (m != Mode::Stopped) workerParked_.store(false);

        if (m == Mode::Stopped) {
            // Idle wait. Wake on any state change so toggling Run is snappy.
            // Mark parked so the rewind transport knows no Running frame is
            // in flight and a restore won't be overrun.
            workerParked_.store(true);
            std::unique_lock<std::mutex> lk(stateMtx);
            wakeCv.wait_for(lk, std::chrono::milliseconds(50),
                [this]{ return exitRequested.load() ||
                               mode.load() != Mode::Stopped; });
            nextTick = clock::now();
            continue;
        }

        if (m == Mode::Step) {
            // Drain ONE queued step per worker iteration, releasing stateMtx
            // between steps (so the UI thread isn't starved during a long
            // `--step N`), and stay in Step mode until the queue empties.
            if (stepsPending.load() > 0) {
                {
                    std::lock_guard<std::mutex> lk(stateMtx);
                    stepBusMaster();
                    // M6502::step() already calls memory->advanceCycles(cycles)
                    // with the *current instruction's* cycle count — per-step
                    // accounting is canonical so cassette + speaker + slot
                    // peripherals stay cycle-aligned. Calling it again here
                    // would double-count (which is exactly the bug that made
                    // the speaker tone freeze: cycleCounter drifted ahead of
                    // wallclock, the audio cursor lagged 200 ms+, the catch-up
                    // logic dropped events in a loop, and the synth got stuck
                    // on whatever level survived the drop).
                }
                stepsPending.fetch_sub(1);
            }
            if (stepsPending.load() <= 0) {
                mode.store(Mode::Stopped);
                // A requestStep() on the UI/CLI thread can race the store
                // above: it may have re-armed Mode::Step and queued a step
                // between our load and this store, which we'd then clobber to
                // Stopped — permanently losing that step. Recover by checking
                // the queue once more and re-arming Step if work appeared.
                if (stepsPending.load() > 0) mode.store(Mode::Step);
            }
            continue;
        }

        // Running: execute one frame's worth of cycles, then sleep until
        // the next 50/60 Hz boundary (frameIntervalUs follows the video
        // standard). Using steady_clock keeps wallclock pace without
        // drifting on busy machines.
        //
        // The beam-racing video-event log is NOT bracketed here: recording
        // is continuous and Memory::advanceCycles publishes the completed
        // frame at each video-frame boundary (65 × 262/312 cycles). The old
        // per-tick bracket let the 60 Hz UI steal a half-recorded tick and
        // drop the rest — fatal for PAL (50 Hz worker) mid-scanline demos.
        //
        // We chunk the budget into small pieces and release `stateMtx`
        // between each — the UI thread takes that mutex many times per
        // render pass (display snapshot, every panel's state read,
        // every menu click handler), and even one ~25 ms hold under
        // turbo (cyclesPerFrame = 1M with the heavier IIe firmware
        // dispatch) was enough to drop the GUI to a few fps. A 4096-
        // cycle chunk is < 0.1 ms wall on Apple Silicon, so the UI
        // gets the lock between chunks within a frame's UI budget —
        // ~250 chunks per emulated turbo frame, ~50 µs of pure
        // lock/unlock overhead per frame, which is invisible.
        //
        // Smaller chunks would mostly amortise into wasted contention;
        // larger chunks (we tried 16K first) leave the UI noticeably
        // laggy during long disk reads.
        constexpr int kLockChunkCycles = 4096;
        // int64 accumulator: cyclesPerFrame is capped only at INT_MAX by the
        // CLI, and processor.run() may overshoot `chunk`, so an `int done`
        // could overflow (signed UB) near the ceiling. int64 makes the loop
        // bound safe without restricting the accepted --speed range.
        // An accelerator card (TransWarp) multiplies the frame's cycle
        // budget: on this machine "running the 6502 faster" IS giving it
        // more cycles per video frame, which is why POM2 keeps the Apple's
        // own CPU where MAME has to substitute a second one. Read once per
        // frame — see TranswarpCard.h on why sampling a sub-frame duty
        // cycle at this rate is exact in aggregate. Returns 1.0 (and
        // touches nothing) on any machine without such a card.
        const int64_t budget = scaledFrameBudget();
        bool interrupted = false;
        for (int64_t done = 0; done < budget; ) {
            // Re-check the mode between chunks so a stop()/park request
            // (profile switch, rewind scrub, shutdown) interrupts the frame
            // within ~one chunk instead of after the full budget — under a
            // turbo budget (up to 2M cycles via --speed / the AI /speed
            // endpoint) finishing the frame first kept the requester
            // waiting for whole seconds, and the old stop() contract let
            // teardown proceed while this loop was still mutating
            // CPU/Memory.
            if (exitRequested.load() || mode.load() != Mode::Running) {
                interrupted = true;
                break;
            }
            const int chunk = static_cast<int>(std::min<int64_t>(kLockChunkCycles, budget - done));
            std::lock_guard<std::mutex> lk(stateMtx);
            const int actually = runCpuSlice(chunk);
            done += (actually > 0 ? actually : chunk);
            // No mem.advanceCycles here — see Step branch above.
        }
        if (interrupted) {
            // Skip the frame-boundary housekeeping (IWM pulse, rewind
            // capture, hang sampling) and especially the pacing sleep:
            // re-dispatch on the new mode immediately — stop() is blocked
            // in waitUntilParked() until we reach the Stopped branch.
            continue;
        }
        // Pulse the IWM once per frame so its 1-emulated-second
        // drive-disable timer (MAME `iwm.cpp:70-84 update_timer_tick`)
        // still drains when the //c+ alt firmware stops poking
        // $C0Ex between disk operations. Cheap when idle — the
        // `!active_` early-out in `sync` short-circuits.
        if (iwmDev) {
            std::lock_guard<std::mutex> lk(stateMtx);
            iwmDev->tick(mem.getCycleCounter());
        }

        // Rewind: capture a full machine snapshot at this quiescent frame
        // boundary (CPU budget spent, IWM ticked — nothing else mutates
        // CPU/Memory until the next frame). enabled() is checked before the
        // lock so a disabled ring costs nothing; held under stateMtx for a
        // consistent view vs. any UI-thread memory write.
        if (rewind_.enabled()) {
            std::lock_guard<std::mutex> lk(stateMtx);
            rewind_.capture(processor, mem);
        }

        // Hang detector: sample end-of-frame PC; if every sample over the
        // last ~3 s sits inside a small window, the CPU is stuck in a wait
        // loop — dump it once. Skipped entirely while a DMA claimant
        // (SoftCard Z80) owns the bus: the 6502 PC is then legitimately
        // parked for minutes and every sample would be identical — a
        // guaranteed false "HANG DETECTED" on a healthy CP/M session
        // (2026-07-12 bug hunt). The ring is reset so stale pre-DMA
        // samples don't blend with post-hand-back ones.
        if (hangTrace && mem.slotBus().dmaClaimant() != nullptr) {
            pcRingCount = 0;
            hangDumped = false;
            framesConfined = 0;
        } else if (hangTrace) {
            const uint16_t pc = processor.getProgramCounter();
            pcRing[pcRingHead] = pc;
            pcRingHead = (pcRingHead + 1) % kHangSamples;
            if (pcRingCount < kHangSamples) ++pcRingCount;
            if (pcRingCount == kHangSamples) {
                uint16_t lo = 0xFFFF, hi = 0;
                for (int i = 0; i < kHangSamples; ++i) {
                    lo = std::min(lo, pcRing[i]);
                    hi = std::max(hi, pcRing[i]);
                }
                if (hi - lo <= 0x2000) {   // wide window: catch big freeze loops too
                    // Confined. Dump once immediately, then keep re-dumping
                    // every ~3 s while STILL confined — a transient slow chunk
                    // dumps once then escapes; a permanent freeze keeps
                    // printing "STILL FROZEN".
                    if (!hangDumped) {
                        hangDumped = true; framesConfined = 0;
                        dumpHang(lo, hi, /*repeat=*/false);
                    } else if (++framesConfined >= kHangSamples) {
                        framesConfined = 0;
                        dumpHang(lo, hi, /*repeat=*/true);
                    }
                } else {
                    hangDumped = false;   // PC escaped — re-arm for next freeze
                    framesConfined = 0;
                }
            }
        }

        nextTick += std::chrono::microseconds(frameIntervalUs.load());
        const auto now = clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_for(nextTick - now);
        } else if (now - nextTick > std::chrono::milliseconds(100)) {
            // We fell behind — resync rather than try to catch up forever.
            nextTick = now;
        }
    }
}
#endif // !__EMSCRIPTEN__
