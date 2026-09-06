# POM2 — Changelog

Notable changes, ordered most recent to oldest. The `git log` remains the
canonical source for the exact mechanics; this file captures the **"why"**
and the pitfalls we don't want to rediscover. Active backlog → `TODO.md`.
Current implementation → `DEV.md`.

## 2026-09-06 — The bug hunt: 51 findings, 49 fixed, 2 declined on evidence

Eight read-only reviewers, one per subsystem, each asked for defects it could
trace end to end; every finding was re-verified in the code before it was
handed to a fixer. Seven commits (`266a289` → `d79444a`), one per file-owning
lot. The full list with `file:line` lives in the session report; this entry
keeps what the next reader must not rediscover.

**Data loss and memory safety (G2 class).**

* **`LironCard` was flushed by nothing.** `StorageCoordinator::flushAll`
  walked three families (Disk II, block, SmartPort); the Liron is a fourth,
  a `MountableMediaCard` with no keyspace of its own, and had no destructor
  flush either. Quit or profile switch destroyed the medium with the session's
  writes in RAM, then the remount read the old file back — no warning.
  `MountableMediaCard::flushBay()` now exists and its **default refuses** a
  bay with unsaved changes and no flush path, so the next such card fails a
  test instead of a user. `LironCard::ejectBay` also discarded `saveDirty()`'s
  return and ejected anyway; it now refuses like every sibling.
* **`<image>.pom2tmp` got none of the target's scrutiny.** `ofstream(trunc)`
  follows a symlink, so a link planted at the temp path took the rewrite and
  the rename carried the link over the user's image. `prepareTempPath` was
  written for exactly this (2026-08) but only `Disk35Image::saveDirty` and the
  settings/snapshot writers called it. Now every write-back does: 5.25", HDV/
  2MG, 3.5" export, the ImageWriter PNG.
* **Eject wrote the image under `stateMutex`.** Mount has been two-phase for a
  long time; eject never was, and the *firmware* 3.5" eject (`EjectOn` on the
  IWM path) did an 800 KB write plus two fsyncs on the CPU worker with the lock
  the UI thread paints under. `DiskIICard` and `Disk35Image` gained the
  take/commit/restore triple `Block512Backing` already had; `Sony35Drive`
  hands its payload to a `Disk35WriteBackSink` and `EmulationController`
  commits it on its own guarded thread (`WriteBackQueue`, joined and drained
  at shutdown). Still inline under the lock: `StorageCoordinator::ejectAllMedia`.
* **WOZ `saveDirty` could `continue` past a quarter-track and still return
  true** with every dirty flag cleared. Latent today (the legacy nibble gate is
  forced off on a WOZ) but one gate away from silent loss on a copy-protected
  disk. It refuses now, like `reportUndecodable`.
* **`SlotRomAsm::finish()` applied fixups after a failed assembly.** `put()`
  refuses to write past a region's limit but does not advance `pc_`, so a
  branch emitted at an overflow recorded its operand at `limit_` — the
  neighbour's first byte, or `rom_[256]` on a 256-byte `std::array`.
* **A SmartPort WRITE whose data packet never came latched the bus.** Only the
  data packet or a bus reset cleared `pendingWrite_`; the firmware abandons a
  data packet on a bad checksum and retries the *command*, so `active()` stayed
  true, the //c external port claimed every `$C0E0-$C0EF` access and the
  Disk II was dead until reset. A command packet now supersedes the pending
  one. FORMAT also answered `$00` on an empty or write-protected unit.

**Lock discipline — the freeze CLAUDE.md forbids.** Six places held
`stateMutex` across work measured in seconds: the character-set switch
(`loadCharRom` + `settings->save()`), the FujiNet panel's `link.stop()` (a
worker join; `enumerateDevices` could spend 32 × timeout with no stop check),
`~FujiNetCard` (2 s SIGTERM grace, destroyed synchronously by `SlotBus::plug`
on every slot rebuild), the built-in `N:` HTTP fetch (12 s budget on the CPU
thread, *default on*), the AI server's snapshot-load and mouse-503 replies
(4 s send timeout), and `SpSerialTransport::readSome` ignoring the stop latch.
All moved off the lock. The `N:` fetch now runs on a worker that owns its
state through a `shared_ptr` + cancel flag, so the device can be destroyed
under the lock without joining. `ChildProcess` (FOUNDATION layer) may not
include `ThreadGuard.h`; its detached-stop thread carries a hand-written
barrier, with the reason recorded in place.

**Emulation correctness.**

* `M6502::step()` vectored an interrupt *and* ran the handler's first
  instruction in one call, so a breakpoint on an IRQ/NMI entry never fired
  and a watchpoint hit by `handler[0]` was blamed on the interrupted
  instruction. Fixed **gated on `debugHook_`** so the un-hooked path's
  `advanceCycles` granularity — and the VIA/LSS lazy-sync phase pinned by
  `mockingboard_t1_irq_phase` / `via_t1_rearm_chain` — is untouched.
* `AN0-AN2` were in neither the snapshot nor `resetSoftSwitches`; AN2 drives
  A12 of an 8 KB international character generator, so rewind and reset
  rendered the wrong font. IOU trailer section grew 4 → 7 bytes; two tests
  that hard-coded the old length were the only casualties.
* The CMOS disassembler gave the 30 one-byte NOPs of the `$x3`/`$xB` columns
  their NMOS 2/3-byte lengths.
* `PhasorCard::syncToCpuCycle` never got the `+1` in-flight data-cycle fix
  `ccb7c55` gave the Mockingboard, so a Phasor reproduced the OLDSKOOL T1
  phase wrap. Both cards also built the AY data bus as `portAOut & ddrA`
  (undriven = 0) against MAME's `(m_out_a & m_ddr_a) | ~m_ddr_a`; they use
  `Via6522::readPortA()` now. The Phasor also gained `setCpuClock` (its four
  AYs were 0.7 % sharp under PAL) and the `cycles <= 0` guard.
* The display painted from the **live** `DisplayState` in four places
  (`render()` with no published events, `fillCompositeSignal`,
  `renderInternal`, `patchMixedTextBand`) while the rest of the path
  reconstructs the *published* frame — a mode flip one frame early and a
  mixed frame's text band dropped. All four take the frame-start state once
  the machine has completed a video frame; before that the live state is the
  only description of the screen (and how the display tests drive the class).
* Multi-page PostScript was truncated (`extraPages` was never assigned;
  Ghostscript writes every page into one `pgmraw` stream) and a Ctrl-D during
  a render was dropped, welding two jobs into one.
* W5100 UDP/TCP never `bind()`ed `Sn_PORT`, so no unsolicited datagram could
  reach the guest. The SLIP framer dropped the packet after a shared
  delimiter. The SCC's SDLC `txFrame` grew without bound.
* Ctrl+letter used GLFW's US key *position*: on AZERTY the key marked A sent
  Ctrl-Q and M sent nothing. `glfwGetKeyName(key, scancode)` resolves the
  layout's cap, positional map as fallback.
* `MainWindow` was a local of `main()`, destroyed after `glfwTerminate()`, so
  every `glDelete*` in its destructor chain hit a torn-down context. It is
  heap-owned and reset before the ImGui/GLFW teardown, user pointer cleared
  first. `MainWindow.cpp`'s comment claiming the opposite was wrong; it is
  right now.

**Declined, with the evidence.** *AppleWin HLE mouse raising VBL IRQs with
mode `$08`*: AppleWin's `OnMouseEvent` does not gate VBL on `MOUSE_ON` and POM2
matches it verbatim. *SmartPort INIT chain / unit-0 STATUS counting mounted
media instead of bays*: a SmartPort device is the drive, not the disk; the
host caches the count at boot and validates every unit number against it
(`$CCD1`), so compacting the chain would renumber a user's volumes on an eject.

**Pitfalls met on the way.**

* The disk filled to 0 bytes mid-run. `build/`'s per-target
  `compiler_depend.make` / `.internal` files are ~4 MB *each* (every test
  compiles a slice of `src/`), ~850 MB across the tree, and regenerate on
  every `make`. They are safe to delete to free space — but `make` then fails
  with *No rule to make target `compiler_depend.make`* until `cmake .` has
  recreated the stubs. A 3 GB `build-san/` sanitizer tree was the real
  consumer.
* `ctest` runs whatever binary is on disk: after a failed link it passes on
  the stale one. Check `make`'s exit status first (memory note from
  2026-08-22, confirmed again).

## 2026-09-06 — Four ejects, one coordinator: the settings keys that outlived the disk

`TODO.md` G2 named three defects that reach a user's data. All three are fixed,
and the first turned out to be three times larger than the entry recorded.

**The eject that cleared the wrong drive.** `StorageCoordinator` owns Disk II
persistence: `diskIIPathSettingKey` writes `disk_path_slot<N>` for drive 1 and
`disk_path_slot<N>_drive2` for drive 2, and `restoreMediaFromSettings` loops
BOTH drives on the way back in. The status-bar eject — the most-clicked eject
in the UI — built `"disk_path_slot" + N` for *both* drives. Eject drive 2 and
it cleared **drive 1's** path while leaving `_drive2` set, then committed that
with an eager `settings->save()`. Next launch: drive 1 empty, and the disk you
had just ejected back in drive 2.

A clean quit self-heals it (`persistSessionSettings` rewrites both keys from
live card state), which is exactly why it survived — the corruption is only
visible after a crash, a `kill`, or a kiosk session. That also made it worth
fixing rather than shrugging at: the window is precisely the case where the
user has no chance to intervene.

**The audit found four such paths, not two.** G2 described "two eject paths ...
in the same file". Enumerating every `ejectDisk` / `ejectBay` call site outside
the coordinator turned up two more, both undocumented:

* `MainWindow_Slots.cpp` (Slot Config) skipped drive 2 on both Insert and
  Eject, justified by a comment — *"Only drive 1 has a persisted path key;
  drive 2 mounts are session-only (matches legacy scheme)"* — that had been
  false since `diskIIPathSettingKey` grew the `_drive2` suffix. A drive-2
  insert from this panel was lost on restart while the same insert from the
  File menu survived; a drive-2 eject here left the image to be remounted.
* `MainWindow_StoragePanels.cpp` (Disk Library) ejected 5.25" media in two
  places and cleared **no** settings key at all. The telling detail: the 3.5"
  branch twelve lines below the first one had already been routed through the
  coordinator, with a comment explaining why the direct call was wrong. The
  5.25" sibling next to it was left hand-rolled — the failure mode is not "one
  function drifts", it is "whichever path is convenient gets written by hand".

A **fifth** site was the mirror image, on the read side: the profile-switch /
Slot-Config-Apply remount (`MainWindow_Slots.cpp`) read one key and called
`insertDisk(diskPath)` with its default argument, so an Apply silently emptied
drive 2 while its `_drive2` key stayed set. That is the same "default argument
= drive 1 only" mistake `MainWindow_Session.cpp`'s comment claimed to have
fixed in "the last of the five places" — it was the last but one. It cannot
delegate: it runs inside the `stateMutex` scope that makes the SlotBus rebuild
atomic against the AI server, and a coordinator command takes that lock itself.
So `diskIIPathSettingKey` is now **exported** from `StorageCoordinator.h` and
used inline instead: one definition of the key, no sixth copy of the `_drive2`
rule.

All four ejects now delegate. The fix *moves the call into already-covered
code*, so
the interesting part is the test: `storage_coordinator` asserted that ejecting
drive 2 clears `_drive2`, but never that it leaves drive 1's key alone — the
half that was actually broken. It does now.

One sub-claim in G2 was already **stale** when re-verified: the status-bar
save was no longer under `stateMutex`; that lock scope had been closed. Worth
recording, because CLAUDE.md's standing warning about re-verifying a `file:line`
before acting on it earned itself again.

**A snapshot now says which Apple it came from.** The 32-bit word after
`version` was written as a reserved 0 and read back with `(void)readU32()`.
Nothing in the file identified the machine, while CPU, MEM and MEX all restore
unconditionally and `Memory::loadSnapshotState` never checks `iieMode`. Save on
//e Enhanced PAL, switch to //c, load: PC and 64 KB of RAM land against a
different ROM and memory map — freeze, or silent wrong execution, with no
diagnostic. Live rewind was already defended (`applyProfile` clears the ring);
`--snapshot-load` and the AI server's `/snapshot` were not, and a snapshot file
is the one artefact users hand to each other.

The word now carries `pom2::snapshotMachineId(profile)` — FNV-1a over the
profile's canonical **persistence key**, not the enum's numeric value: the key
is stable by contract (state.cfg and `--preset` both carry it) whereas the enum
is appended to and its order is deliberately not the display order. Both load
paths compare before touching any state and refuse with a message naming both
machines. **0 stays legal** and means "not recorded", so every snapshot written
before this build still loads; rewind frames record none on purpose, since the
ring is cleared on a profile switch and the check would be dead weight on the
hot capture path. This is the general form of a mitigation already in the tree:
`MachineSnapshot.cpp` reads the CPU-mode byte and deliberately discards it,
because an NMOS blob forcing a //c's 65C02 ROM onto an NMOS core hits a KIL and
freezes. That comment was the argument for doing the whole thing.

**`PhasorCard::onReset` zeroed the counter its sibling bumps.** The audio thread
re-seeds a chip's tone/noise/envelope generators only when `ayResetCount_[ci]`
*changes* against its own `lastSeenResetCount`. `Mockingboard::onReset` bumps
it, under six lines of comment naming the bug — *"BUMP, don't zero… zeroing it
was a no-op whenever it was already 0… the card droned on forever"*. The Phasor
assigned 0, which is a no-op on every reset not preceded by an AY reset strobe
(a second F12; a cold boot on a driver that never strobes PB2 low). The
CPU-side `ay_[i]->reset()` then cleared the register bank while the audio thread
kept the old tone, so the card held its last note through the reset: the exact
symptom the Mockingboard fix removed, reintroduced in the sibling. Zeroing also
made the counter non-monotonic, which is what let the two implementations
diverge without anything noticing.

The new `phasor_card_smoke` case was checked the only way a regression test is
worth anything — the old assignment was restored and the test failed, then the
fix was put back and it passed.

## 2026-09-05 — The v0.9.0 release run stopped on a moving tag, so the tag stopped moving

The release build for v0.9.0 lost all three aarch64 jobs 35 seconds in, while
x86_64, macOS, Windows and WASM had already gone green. Not the code:

```
appimagetool.AppImage: OK
sha256sum: WARNING: 1 computed checksum did NOT match
linuxdeploy.AppImage: FAILED
```

`packaging/linux/fetch_appimage_tools.sh` took appimagetool and the ET_EXEC
runtime from AppImageKit release **12** — its own header says why: "both are
immutable release assets rather than a moving `continuous` tag" — but took
linuxdeploy from `linuxdeploy/releases/download/continuous/`. Upstream
re-uploaded that asset on 2026-09-01 under the same tag, so the pin stopped
matching. The pin did exactly its job; the URL was the defect.

**The pin was not simply bumped to whatever the URL now serves.** A checksum
mismatch on a downloaded binary is the one signal that separates upstream churn
from a substituted one, and re-pinning without a second source throws that
away. The candidate was downloaded independently and hashed, and the result
compared against the digest GitHub computes server-side for the asset; the
asset's `updated_at` was checked against the release's publication time.

That verification also found the better fix. Upstream does publish immutable
dated releases, so linuxdeploy now comes from `1-alpha-20251107-1`
(`620095110d69…`, `updated_at` equal to its publication time, never rewritten)
and is held to the same rule as the other two. The failure mode is structural,
not bad luck: a moving tag can only ever break at fetch time, and this script
is only ever fetched by a release — the worst moment to learn a dependency
moved. For the record, `continuous` as of 2026-09-01 was `556ab80baa98…`,
verified the same way.

The v0.9.0 tag was moved onto the fix rather than a v0.9.1 cut: no Release
object had been created and no asset published, so the tag had no consumers.

## 2026-09-04 — StorageCoordinator gets its first tests, and a resync gap closes

A bug hunt, reported with its uncertainty intact.

Looking for where defects can hide, the useful question turned out to be which
non-UI source files no test ever links. Almost every answer is a `MainWindow_*`
or `*_ImGui` translation unit — expected, they need a GL context. One was not:
**`StorageCoordinator.cpp`, 1640 lines of pure logic with no test of any kind**,
deciding which disk goes where and what reaches `state.cfg`. It is also the
code the day's earlier HDV report ran through.

`persistRebuildSettings` is the function Slot Config's Apply (and every profile
switch) calls to write the LIVE media state into Settings before the SlotBus is
torn down and rebuilt from those same keys. Settings are the transport, and the
function exists because the keys are not trusted — that distrust is why it was
written at all, after Apply once dropped whatever was mounted in Disk II drive
2. Its Disk II and CFFA branches resync path *and* write-back, unconditionally,
including "nothing". Its HDV branch resynced the path only when an image
happened to be loaded, and never resynced the write-back opt-in at all.

**What that is, honestly: a latent gap, not a reproducible bug.** Every HDV
mutation path was traced — the media bay row, the HDV panel's eject and
write-back toggle, `ejectMediaBay`, `setMediaBayWriteBack`, `ejectAllMedia` —
and every one persists both keys independently, so the hole is masked. But it
is a hole in the function's own contract, in the same shape as the defect that
motivated the function, on the two facts that decide whether a mounted image
comes back and whether a session's writes reach the file. It now writes what
the card actually holds. The exclusions are unchanged (the session-local
auto-provisioned slot, a synthesised "[host folder] " volume), and it still
deliberately does not clear the key when there is no HDV card at all — that is
`persistSessionSettings`' job on quit, and doing it here would wipe a path a
card is about to be given.

New: `tests/storage_rebuild_persist_test.cpp` (`storage_rebuild_persist`), the
file's first test — an eject and a write-back opt-in through a real capture →
persist round-trip. Both assertions failed on the code as found. Suite is now
240. Worth recording what the wiring showed: linking a settings round-trip
required `EmulationController` and a dozen more sources, because the class
mixes its pure capture/persist half with mount/eject calls that go through the
controller. That coupling is itself much of why the file had no tests.

## 2026-09-04 — The nightly sanitizer legs go green again (three harness defects, no emulator bug)

Both nightly jobs had been red since 2026-09-03 while every push run stayed
green — the sanitizers only run on `schedule` / `workflow_dispatch`. All three
findings are in `tests/`; none is a defect in the emulator.

**UBSan, `chatmauve_dot_rules`** — "index 6 out of bounds for type 'unsigned
int [6]'". The AppleWin oracle indexes `g_pPaletteRGB`, AppleWin's full
16-entry palette, so its `6 - color` at color 0 lands on a real entry there;
the port kept the arithmetic but narrowed the array to the 6-entry slice it
needs, and ran one past the end. The value was always dead — the caller reads
`colors[i]` only when the 3-dot window is 010 or 101, i.e. exactly color 1 or
2 — so the fix keeps AppleWin's expression verbatim (it is the point of the
oracle) and gives the unread entries a defined value. Reproduced locally under
UBSan, same message, before and after.

**TSan, `fujinet_net_device`** — a data race on `HttpStub::listenFd`. `stop()`
writes `listenFd = -1` *before* joining, and it has to: closing the listening
socket is what unblocks the worker's `accept()`. The worker read the member.
The thread now captures the descriptor by value at creation, so nothing is
shared; `close()` still wakes it. Worth recording how thin the first
verification was: one run passed with the fix reverted, which would have
"confirmed" a fix that changed nothing. Looped instead — **9 races in 15 runs
without the fix, 0 in 30 with it**.

**ASan/UBSan, `pom2_core_sdk_consumer`** — not a race or an overflow but a
link failure, and the interesting one. That test installs the SDK and builds
`examples/pom2_core_consumer` as a *separate* CMake project; the installed
`libpom2_core.a` carries the parent's instrumentation, so the consumer must be
built with the matching `-fsanitize` or it fails on the runtime's own symbols
(`__asan_option_detect_stack_use_after_return`, `__ubsan_vptr_type_cache`).
Forwarding `CMAKE_CXX_FLAGS` would NOT have fixed it: the parent applies the
sanitizer through `add_compile_options`/`add_link_options`, which never reach
that variable. `POM2_SANITIZE` now travels to the sub-configure by name.
Verified by reading the generated consumer's `CMakeCache.txt` — the flags are
there — not merely by the test turning green.

## 2026-09-04 — A block device with write-back off now says so, standing

Reported as "SCOSWAMP.HDV doesn't seem to get written". Two independent causes,
one of them ours.

**The silent half.** A floppy folds the host opt-in into the medium: both
`DiskImage` and `Disk35Image` define `isWriteProtected() = fileWriteProtected
|| !writeBackEnabled`, so with write-back off the guest is *told no* and DOS
raises an error. A block device deliberately does not — `ProDOSHardDiskCard::
writeDataByte` presents a fully writable volume on the grounds that "a real
hard disk is read/write to ProDOS", and only the 2IMG header WP blocks a write.
That decision stands (it is what makes a session feel like real hardware), but
its consequence had no voice anywhere: the guest's save **succeeds**, the
blocks live in RAM, and they are dropped at eject or quit with nothing on
screen ever having said so. `MediaBayInfo::hasUnsavedChanges` exists for
exactly this and was consulted only by the status bar's eject menu — which the
session that loses work never opens, because its shape is mount, play, quit.

The bay row in Slot Config / Internal Disks & Media now carries a standing
amber line whenever a loaded bay has write-back off, worded for what actually
happens on a block device rather than borrowed from the floppy panel's
"read-only" text, and stronger once `hasUnsavedChanges` is set. No
guest-visible semantics changed; flipping the block path to report
write-protect like a floppy would be a policy reversal, and is deliberately
not done here.

**The other half, in the neighbouring repo** (`pom2adventure`, recorded here
because it is what actually made the image stale): `make hdv` aborted before
touching the image because `SCOSWAMP.MORE/TOOLS/build/build_prodos_volume` was
missing, and the guard rule printed the two cmake lines then `exit 1`. That
build directory is gitignored, so it vanishes on any clean; the .hdv is
gitignored too but survives in the working tree, so the emulator kept booting a
stale image — 13 hours older than the `SCOSWAMP.BIN` it was supposed to
contain. The rule now runs those two commands instead of reciting them.

## 2026-09-04 — Le Chat Mauve gets its second connector: an analog RGB bandwidth stage

The OpenEmulator work POM2 had ported was the *display* half — barrel,
scanlines, mask, persistence, vignette, gamma — and `CrtEffectStack` already
applied all of it to the Chat Mauve. What was missing was the half OE calls the
**connection**: the video chain between machine and tube, which is not the same
cable in every setup. The composite pipelines model theirs inside the
demodulator; the RGB ones had none at all and emitted perfectly square dots.

The Chat Mauve is the case that makes this concrete. The cards have two
connectors (`docs/chatmauve_plan.md` § 3.7): a TTL RGB header, and an analog
Péritel socket where R, G and B each leave through a resistor ladder and a trim
pot before a metre of cable. POM2 rendered both identically. `NtscParams::
rgbBandwidthMHz` (default **0 = off**, so no existing look moves) now drives a
17-tap Hann-windowed sinc low-pass — horizontal, per channel — as a pre-pass at
source resolution ahead of the glass.

The non-obvious part is *where* it runs. Band-limiting is an operation on the
sample grid, not on the screen, and the Apple II's two framebuffer widths are
two different clocks: `frame80` is sampled at 14.318 MHz, `frame` at 7.16 MHz.
Filtering there means one MHz figure lands correctly on every mode with no
per-mode special case — which is what a real cable does. Measured swing out of
215 on the 560 grid: at 5 MHz, 7.16 MHz content (a true 560-dot DHGR/COL280
picture) drops 215 → 1 while 3.58 MHz content (280-dot HGR doubled into
frame80) stays at 215; drop to 3-4 MHz and HGR softens too. On the 280-wide
buffer 5 MHz is above Nyquist, so `applyBandwidth` returns 0 and the pass is
skipped outright rather than run as an identity.

Kernel weights are normalised by their own sum, so DC gain is exactly 1 and
dragging the slider cannot walk the picture's brightness (flat field: mean
180 → 180, span 0). A failed shader compile or an incomplete FBO zeroes
`bwProgram` and logs — the knob goes inert, the glass pass keeps working.
Persisted as `ntsc_rgb_bandwidth_mhz`; slider under "Analog link" in CRT
Settings. Checked by `crt_barrel_view` (exit 4), which also prints the sweep.

## 2026-09-04 — Shadow-mask pitch is a property of the glass, not of the signal

`CrtEffectStack`'s shadow mask derived its horizontal coordinate from the
source framebuffer: `oxBase = uv.x * (uSrcSize.x * 2.0)`. Since the triad
period is a fixed 3 units, that tied the number of triads on screen to the
video mode. The 280-wide modes (Apple2Display's legacy `frame`: 40-col text,
40-col HGR, lo-res) got a mask exactly **twice as coarse** as the 560-wide ones
(`frame80`: 80-col, DHGR, Le Chat Mauve, and the OE demod output, which is 560
too) — so merely switching video mode, or a program flipping to 80 columns,
visibly changed the tube the picture was being displayed on.

A real CRT's triads have a fixed pitch in millimetres whatever resolution is
fed to it. The coordinate is now pinned to the 560-dot line at the same
2-units-per-dot scale the scanline coordinate uses for rows: `kMaskUnitsX =
1120.0`. Note the asymmetry is principled, not an oversight — the row axis
never had the bug and keeps `uSrcSize.y * 2.0`, because the scanline count *is*
a signal property (192 real beam sweeps) while the mask is not. Choosing 560
rather than 280 as the reference leaves every 560-wide mode (including both OE
paths, where the mask is most used) pixel-identical and only brings the
280-wide ones onto the same glass.

No ctest pin is possible — the shader needs a GL context and CI has none — so
the check lives in the existing offscreen harness `crt_barrel_view`, which now
renders a flat field at 280 and at 560 through an aperture grille and counts
the triads landing on an identically-sized output; mismatch is exit code 3.
Measured before the fix: 298 vs 596 sign changes. After: 596 vs 596. All
three offscreen GL harnesses also stopped being Linux-only in passing —
`crt_barrel_view`, `oe_signal_view` and `ntsc_oe_ab_tool` each included a bare
`<GL/gl.h>`, which does not exist on macOS (and is frozen at GL 1.1 on
Windows), so none of them had ever once built there. They all go through
`Pom2GL.h` now, which is what made the measurements above possible at all.

## 2026-09-02 — OLDSKOOL crash fixed: Mockingboard MMIO sync lands on the access data cycle

French Touch's standalone `oldskool.dsk` (FORT ET VERT, SHADOW 2021) blew the
6502 stack (SP -> $00, into DOS) ~10 s in; the DIX-packaged copy ran fine, and
removing the Mockingboard removed the crash. Root cause: OLDSKOOL arms a
free-running Mockingboard T1 IRQ and its handler reads the T1 counter (`$C404`)
to phase-measure the beam, self-modifying a `BVC` by `mem[$03] - T1CL - $19` to
dispatch. POM2's lazy VIA sync (`syncToCpuCycle`) advanced the timers to
`getCycleCountNow() = cycleCounter + cpu.cycles`, but `cpu.cycles` does not yet
count the in-flight DATA cycle of the reading instruction (`SBC $C404` reads
with `cpu.cycles == 3`), so the T1 counter read back one too high. That single
count wrapped the phase `$00 -> $FF`, jumping the dispatch into a code slice
that `RTS`'d off the 3-byte IRQ frame -> return to DOS RWTS garbage -> `TXS` ->
SP=0 -> runaway.

Fix: `syncToCpuCycle()` advances to `getCycleCountNow() + 1` — the cycle the
bus access actually occurs on. It cannot be the VIA read-back bias (`-1`, pinned
by `via_t1_rearm_chain`): that models a DIRECT read and OLDSKOOL wants the
opposite; and it cannot be an IRQ-entry change: MMIO and batch syncs still
converge on a load's instruction-end. The +1 cancels for write-then-read idioms
(detection routines; TRIBU's closed-loop re-arm), so it moves ONLY reads taken
against a free-running underflow. DIX menu raster columns are byte-identical
(`dix_menu_raster_probe`), all VIA/Mockingboard/CPU/Disk II suites green, and
the fix is Mockingboard-local (no CPU-core or Disk II sub-cycle change). Pinned
by `mockingboard_t1_irq_phase`; `mockingboard_sync_smoke::testNoEndOfStepOvershoot`
reworked to drive its MMIO through real executed instructions.

## 2026-09-02 — Raster per-kind offset narrowed to AN3/DHGR (MAD EFFECT regression fix)

The OLDSKOOL per-kind offset (earlier today) shifted HiRes / Dhgr / An3
mid-line switches one column left. That over-reached: MAD EFFECT flips
lo/hi-res ($C056/$C057) mid-line to place its beam-raced picture, and the
HiRes shift dragged those regions one character cell too far left (user
report; visible on MAD EFFECT's lo-res "FRENCH" + raster-lines screen).

The -1 belongs ONLY to the DHIRES/AN3 colour clock ($C05E/$C05F): OLDSKOOL's
raster bands are AN3-driven (the DHGR artifact colour), which is what the
shift fixes -- user-confirmed in RGB and composite. `beamColForEvent` now
shifts only Dhgr / An3; HiRes joins PAGE2 / TextMode / 80Col on the
fetch-side -24. MAD EFFECT confirmed fixed live; OLDSKOOL's AN3 colour
alignment is unchanged (AN3 still -25). `raster_switch_kind_offset` updated
to pin HiRes = PAGE2 (both -24); display/beam suite green.

## 2026-09-02 — Block ASCII Anthology: dual-bank char ROM switched by annunciator 2

"Block ASCII Anthology" (French Touch) rendered as garbled letters/brackets:
it is an Unenhanced-//e demo (the corpus machine — no MouseText) that ships
its OWN 8 KB character generator (`eprom2164.bin`) to draw block art. But it
uses TWO fonts and switches between them at runtime — a normal-text intro
("HAVE YOU EVER SEEN BLOCK ASCII ART ON AN APPLE II ?") and the block-glyph
art — so a single loaded font gets one screen right and the other wrong.

The mechanism (found by research, apple2history.org ch.12): a localized //e
fits a 2364-class 8 KB char ROM holding two 4 KB sets and wires the char
ROM's A12 to **annunciator 2** — `$C05C` (AN2 off) / `$C05D` (AN2 on) — so
software flips the whole font by poking AN2 (the Japanese j-Plus katakana
toggle is the classic example). POM2 was swallowing AN2 with no effect and
collapsing 8 KB dumps to one 4 KB bank at load time.

Now `Memory::loadCharRom` with a sentinel bank of -1 keeps BOTH 4 KB sets
(each normalised), and `charRomActiveData()` selects the live one from the
AN2 state; the text renderer and its frame-cache key follow it, so an AN2
font flip re-renders. Shipped as `roms/apple2e_char_ft_blockascii.rom`,
catalog entry `//e — French Touch (Block ASCII custom)` (key `iie_ft_block`,
bank -1 = dual-bank). Confirmed live on the Unenhanced-PAL machine: the
normal-text intro AND the block art now BOTH render correctly as the demo
toggles AN2. A plain 4 KB ROM leaves AN2 a no-op, as on a US machine; the
existing 342-0274-A FR/US entries keep their single-bank behaviour. Pinned
by `char_rom_catalog`; display/char suite green.
## 2026-09-02 — OLDSKOOL raster: per-kind mid-line switch column (the second half)

Follow-up to the Unenhanced-PAL profile: the OLDSKOOL FORT ET VERT raster
bands were off ~7 px (one character cell) from the TV-set art even on the
genuine-NMOS profile, so it was a rendering bug, not the 65C02 cycle drift.

`frameCycleToPos`'s `byteCol = hpos - 24` is calibrated (madef_phase_probe /
vbl_edge_phase) on MAD EFFECT's mid-line `$C055` — a PAGE2 flip, which is
*fetch-side* (it picks the address the scanner reads NEXT, so its effect
shows one byte later). OLDSKOOL's mid-line switches are `$C056`/`$C057`
(hi-res) and `$C05F` (AN3/DHIRES) — *display-side*: they re-interpret the
byte being fetched NOW, one column LEFT of a page flip on the same cycle.
`Apple2Display_Beam.cpp::beamColForEvent` now pulls HiRes / Dhgr / An3 back
one column (`hpos-25`) while PAGE2 and everything else keep `hpos-24`. Scope
was kept to exactly what the demo exercises: TextMode / MixedMode stay at -24
(no measured title flips them mid-line, and `$C050/$C051` also moves the
fetch region, so their side is ambiguous). User-confirmed on the live demo —
aligned in BOTH the Chat Mauve RGB and OpenEmulator composite paths. MAD
EFFECT, DROL, DIX and the three `horizontal_split` pins stay green untouched;
new pin `raster_switch_kind_offset` locks HiRes = PAGE2 - 1 column. The
earlier "POM2 is faithful for the machine selected" claim (round-2 CHANGELOG
+ TODO) is corrected: the machine was necessary, this mapping was the rest.

## 2026-09-02 — Apple //e Unenhanced PAL: the French Touch machine exists now

A user screenshot showed OLDSKOOL FORT ET VERT's raster bands "7 pixels
off" on the default `Apple //e Enhanced PAL` profile.
`tests/oldskool_raster_probe` (new diagnostic, EXCLUDE_FROM_ALL like the
DIX probe) measured the mechanism: the demo's per-scanline dispatcher is
`LSR TBUFFER,X` — 7 cycles on the NMOS 6502, **6 on the 65C02** (real
silicon; `M6502::RmwAbsX` models both) — so on the Enhanced machine every
65-cycle line loop runs in 64 and each mid-line switch drifts one character
cell (7 px) per line. NMOS probe: switch locked at hpos 26 on all 66 band
lines. CMOS: sweeping. The demo's own `.nfo` says "Apple IIe mode (not
Enhanced), 6502 required" — POM2 was being *faithful* to the wrong machine,
and offered no right one: the only PAL //e was Enhanced, whose 65C02 is
soldered (`resolveCpuMode` refuses NMOS there for good reason).

So the machine exists now: **`Apple //e Unenhanced PAL (50 Hz)`** — NMOS
6502 + PAL timing + the `apple2e_unenh` ROM probes, derived from the NTSC
Unenhanced config the way the other PAL profiles derive from theirs. Key
`iie-u-pal`, CLI aliases `frenchtouch` / `//e-u-pal`.

**Correction (same day, after testing on the new profile):** the NMOS
switch removes the *staircase* (the per-line cycle drift), but a **residual
fixed ~1-column offset survives on genuine NMOS** — verified live on the
`iie-u-pal` profile (`/status` → `cpu_mode: nmos`), the band-vs-TV
misalignment persists. So OLDSKOOL stacks two bugs: the CPU cycle count
(fixed by this profile) AND the `frameCycleToPos` per-kind mid-line column
mapping (`hpos - 24`, calibrated only on MAD EFFECT's `$C055`; OLDSKOOL uses
`$C056/$C057/$C05F`). The second is STILL OPEN and needs a measured,
regression-tested calibration — not a blind shift. See TODO § "Mid-scanline
raster offset" for the full write-up and the measurement blocker. Display order
reorganised while at it: `allProfiles()` now reads NTSC chronologically
then the PAL block (Unenhanced //e · Enhanced //e · //c Le Chat Mauve),
and the Unenhanced NTSC row was renamed "Apple //e Unenhanced (1983)" to
mirror its Enhanced sibling. Nine machines total; menu/palette/toolbar/ROM
Status all derive from the one array. Pinned by `system_profile_smoke` +
`cli_kiosk`; probe note: headless, the intro returns to DOS ~10 s after
BRUN before its PRESENT/bands phase — an environment quirk to chase when
the subject reopens (the drift measurement is from the phase it reaches).

## 2026-09-02 — Bug-hunt round 2: twelve fixes across six fresh lenses

Second adversarial hunt (parsers / MAME parity / snapshot-rewind / network /
audio-printer / error paths): 12 confirmed, all fixed.

- **Host-folder write-back preserved user edits** (the high one): the synth
  volume is a MOUNT-TIME snapshot, but the decode rewrote every differing
  file — a host file edited while the volume was mounted was silently
  reverted to the stale copy and counted as saved. `Block512Backing` now
  stamps the snapshot time, `PendingWriteBack` carries it, and
  `decodeVolumeToFolder` preserves (warn + `filesSkipped`) any host file
  whose mtime is later. DEV.md's "read-only, `$2B` on writes" note was
  stale and is corrected. Pinned by `prodos_volume_smoke`.
- **TNFS TCP transport**: `connect()` was "bounded" by SO_SNDTIMEO, which
  the project's own 2026-08-21 measurement disproved (75 s vs 8 asked), and
  `getaddrinfo` was unbounded — `POM2 tnfs://unreachable/…` froze ~75 s.
  The FujiNet `resolveBounded`/`connectBounded` pair moved to
  `SocketUtil.h` (shared; `SocketCompat::setBlocking` added) and TnfsClient
  uses it. Framing: only READ replies were reassembled across TCP
  segments; a split MOUNT/OPEN/STAT/READDIR failed AND stranded its tail,
  desynchronising the session for good. Per-command pulls + a pre-send
  drain fix both. Cache keys hashed (FNV-1a of full host+path) when
  truncated — the pure tail-truncation dropped the host, colliding distinct
  servers onto one cache file. Pinned by `tnfs_client` (split STAT).
- **VIA T2 read-back** joins T1 on the hardware line `written+1−elapsed`
  (was MAME's −2 — one low, the exact drift class of the T1 raster crawl),
  continuous through the underflow. Pinned by `via_t2_timing`.
- **AY envelope retrigger**: `applyEnvShape` zeroed `envCounter` on every
  R13 store; MAME's cited-verbatim `set_shape` leaves the period counter
  running. Buzz-bass retriggers were up to a period late.
- **Rewind Record checkbox** now takes `stateMutex` around `setEnabled` —
  since the splice fix it clears `prevBlob_`, racing the worker's capture
  (vector UB, or a delta against the stale pre-pause blob).
- **Chat Mauve beam-replay ring** cleared on reset + snapshot/rewind
  restore: future-stamped edges from the abandoned timeline replayed a
  mode the machine never entered (Memory purges its video log for the
  same clock-jump reason; the card's parallel log did not).
- **Grappler+**: BUSY is released on the PostScript-model and no-source
  early returns (switching to LaserWriter with BUSY latched hung the guest
  in the $CD89 ACK poll forever), and `setPrinterBusy` re-derives the ACK
  IRQ on the busy→idle edge (MAME raises it on /ACK; an IRQ-mode driver
  never woke without a status read).
- **`prepareTempPath`** unlinks a regular file with link count > 1: a hard
  link planted at `<target>.tmp` shared the target's inode and the trunc
  destroyed the target before commit — contract says "target untouched on
  every failure path". `Disk35Image::saveDirty` now routes its
  `.pom2tmp` through the same scrutiny. Pinned by `atomic_file_replace`.
- **AI `/disk` endpoints** already fixed in round 1; **FloppyEmu
  `listing()`** now honors `file_size`'s error code (a vanished file showed
  a ~16 EiB row — the same fix `parseFavorites` already carried).

## 2026-09-02 — Bug-hunt batch: eject race, 3.5" opt-out wedge, pause-deaf speaker

A five-lens adversarial bug hunt (locking / memory / storage / timing /
lifecycle) confirmed six defects; all fixed here, each pinned.

- **Two-phase eject dropped guest writes racing the commit** (the high one).
  `StorageCoordinator::ejectMediaBay` ran phase 2 (`commitWriteBack`, tens of
  ms) with `stateMutex` released — by design — while the medium stayed
  mounted and writable; phase 3 then blanket-cleared **all** dirty flags, so
  blocks ProDOS wrote during the window were wiped unwritten and the eject
  reported ok: silent loss from the user's only host copy.
  `Block512Backing::takeWriteBack` now **moves** the dirty set out (flags
  retired atomically at capture, under the lock), so racing writes keep
  their flags and `ejectBay`'s own save-on-eject flushes the (normally
  empty) remainder inline; a failed commit restores the captured set via
  `restoreDirty` so retry still works. `clearDirty`/`clearBayDirty` are
  gone — the API that made the bug expressible no longer exists. Pinned by
  `two_phase_block_mount` cases 7–8.
- **`Disk35Image::saveDirty` errored when write-back was off**, where
  `DiskImage::saveDirty` no-ops: once dirty, unchecking write-back wedged
  3.5" eject / swap / the //c+ flush gate forever ("image is
  write-protected"). Now a successful no-op; `dirty_` survives it so opting
  back in still saves. Failure paths also log now (they only set
  `lastError_`, so a failed shutdown flush died silently —
  `MainWindow_Session` logs it too, like the Disk II path always did).
  Pinned by `disk35_atomic_save`.
- **Speaker deaf after pause/resume**: the audio callback keeps running
  under `Mode::Stopped`, so `audioCpuCursor` consumed ~1 M cycles/s while
  `Memory::cycleCounter` froze; on resume every toggle landed behind the
  cursor and the stale purge ate the stream (one parity flip per buffer) for
  the rest of the session. `fillAudioBuffer` now has the consumer-ahead
  mirror of its forward catch-up: a gap > `catchUpCycles` re-anchors the
  cursor `kCatchUpSecs` before the first pending toggle. Pinned by
  `speaker_smoke` case 5.
- **AI `/disk` endpoints hard-coded slot 6** but operate on the *primary*
  (lowest-slot) Disk II: with cards in slots 5+6, ejecting "slot 6" flushed
  and dropped the slot-5 disk and returned 200. The endpoints now validate
  the requested slot against the bound card's real one and echo it back
  (`slot` omitted = the bound card).
- **Internal Disks & Media panel showed stale paths after a slot rebuild**:
  its primed InputText statics survived `applyProfile` /
  `restartEmulationFromSettings`, presenting the old card's image path
  against a rebuilt card — one Mount click from inserting it. A seed
  generation (`mediaPanelSeedGen_`) bumped by both rebuild paths re-primes
  the buffers.

## 2026-09-02 — The 6522 T1 read-back was one cycle low, and DIX rasters crawled

TODO "Next up" §2, run to ground with the real demo. TRIBU (DIX anthology)
chains five VIA T1 IRQs per PAL frame, each handler re-arming from the
free-running counter (`ADC $C404 … STA $C405`) with constants the author
tuned on AppleWin and a real Apple //e. That release-tuned `DELAY = 24`
fixes the T1 counter read-back line at `written + 1 - elapsed`; POM2 used
MAME's `-IFR_DELAY` bias (one lower), so every link lost a cycle — measured
-5 cycles/frame: the whole raster layout crawled left one character cell
every ~3 frames, which is exactly the "edges off by a cell" symptom filed
as a mid-scanline placement bug. `Via6522.h` read-back bias is now -1
(armed and free-running alike, the line continuous through the underflow);
the continuous auto-reload period stays latch+2 (MAD EFFECT's pin). With
the fix TRIBU's arms lock to scanlines 0/64/88/176/184 — the demo's design
— and the frame is 20280 cycles on the nose. Pinned by `via_t1_rearm_chain`
(read-back line + the exact TRIBU arithmetic drift-free over 40 frames);
`dix_menu_raster_probe` (built, not a ctest) boots any DIX-family disk and
dumps per-frame events, T1 arms and PPM frames. 237/237 green — the 4am
Mockingboard detectors never pinned the ±1, and no golden moved.

## 2026-09-02 — Beam-racing gets its own translation unit (and CI gets green)

The 2026-09-01 push tripped CI twice: `-Werror` unused `iie80` (orphaned by
the Chat Mauve early-return in `usesLegacyPath`) and the file-size ratchet
(`Apple2Display.cpp` +42 over its 2486 ceiling). Fixed the variable, and
honoured the ratchet the intended way: the beam-raced replay
(`usesLegacyPath`, `renderInternalSegment`, `forEachBeamSegment`,
`renderBeamRacing`) moved to `src/Apple2Display_Beam.cpp` — same shape as
the Chat Mauve painters' split. `Apple2Display.cpp` 2528 → 2294, ceiling
lowered 2486 → 2294.

## 2026-09-02 — DIX rasters under the Chat Mauve: one frame, one buffer

User report: beam-raced DIX effects run clean on the composite pipelines but
fall apart with the Féline. Root cause: the Chat Mauve is the ONE mode where
graphics render 560-wide (`frame80`) while the machine sits in 40 columns —
so a raster frame mixing TEXT/GR bands with Féline HGR painted half its
segments into the 280-wide `frame` and half into `frame80`, and the
presented buffer was whichever the last segment happened to set (the
documented "mixed 280/560 split is undefined" v1 scope-out, now closed).
Fix: under the card the whole frame is 560-domain — `usesLegacyPath` says
so, and the legacy 280 tail pixel-doubles its band into `frame80` during a
replay (`bandLatch_ >= 0`), so every segment composes in the same buffer.
Static frames keep the native 280 render: no golden moved (236/236 green).
Pinned by `chatmauve_latch_split` section 4 (TEXT40 ⇄ Féline HGR split).

## 2026-09-02 — The slot picker says silicon or service

Every card in Slot Configuration's combo now carries its emulation level —
`[L1 · LLE]`, `[H1 · HLE]`, … — straight from the abstraction catalog (the
LLE/HLE panel's source of truth, `docs/lle_vs_hle.md` made live), with the
catalog's one-line "what is modelled" as the item tooltip. Three key
renames are aliased (smartport35→smartportcard, printer→printercard, the
Sound II shares the Mockingboard entry) and three family rows reused
(Phasor→Mockingboard, Cricket→SSI263, Echo+ TMS→TMS5220). Four cards have
no row in the doc yet — liron, workstation, 4play, transwarp — and show no
tag rather than an invented one; TODO records that the doc and the catalog
move together.

## 2026-09-02 — "Extasie is wrong on the Eve": yes — the real Eve too, and now POM2 says so

Reported as a colour/B&W mixing problem with the Eve model where the
Féline is fine. That is the hardware: *"la carte Eve n'est pas compatible
avec ce mode"* (Manuel Arlequin, about the mixed DHGR mode Extasie is
built on) — POM2 folds the mixed latch to COL140 on the Eve and the RVB
Graph, so fine 560-mono linework renders as colour fringes, exactly as on
the real board. What was missing is the explanation: the card now logs one
line when a program clocks the mixed mode on a model that lacks it
("Extasie/Arlequin need the Féline — Slot Config → model"), the Chat Mauve
panel shows the same warning next to the DHGR decode, and the Slot Config
"model" combo grew a tooltip mapping models to their modes.

## 2026-09-02 — Extasie's SCHEMA: the disk was lying, and POM2 could prove it

**Reported: "the circuit file doesn't display correctly in Extasie's
images."** The circuit is `SCHEMA` on the slideshow side, and it renders as
non-deterministic noise — a different garbage every pass while the nine
other images re-unpack byte-identically. The trail: RAM dumps per slide
showed the renderer faithfully painting garbage RAM; re-rendering that RAM
under every interpretation (BW560, COL140, mixed) stayed noise, so the
slideshow's own decompressor was producing it. The Reloaded source disk
carries the decompressor's assembly listing plus a compressed/uncompressed
reference pair (CAMELOT): a Python reimplementation of the exact RLE
(count byte, bit 7 = repeat, column-major writes, AUX then MAIN, 6502
`DEC`-wrap semantics) matches CAMELOT 100 % — and matches the EMULATED
garbage for SCHEMA to 99.7 %, deraililng past EOF exactly as the machine
does. So POM2 executes the decompressor faithfully; the input is bad:
`disks_5.4/gist/Extasie disk2.dsk` carries a SCHEMA of 11 264 bytes that
is 66-86 % zeros. The second preservation (underground2e.free.fr,
`Extasie_Slide.dsk`) is byte-identical for every other file and has
SCHEMA at 3 886 bytes, intact: through POM2 it displays a clean 560-mono
electronics schematic, pixel-identical to the offline decompression. On the
user's explicit call, the gist image was replaced in place by that intact
preservation (the damaged version stays one `git checkout` away) — the
standing "never touch disk images as cleanup" rule bends only to an
explicit owner decision, which this was.

Found and fixed on the way: the day-old motor-off coast let a stale
write-mode flag spray nibbles through the one-second window — the //c+
IWM forwarding leaves the shadow Disk II in write mode while its 3.5"
boots, and `iic_external_smartport` case E (its 5.25" takes ZERO flux)
caught it. The legacy gate now serves reads only while coasting.

## 2026-09-02 — Arlequin boots, the PLA turns out to be a router, the latch beam-races, the RVB Graph gets its four strobes

The rest of the Chat Mauve "next up" list, each item taken to where its
sources allow.

**Arlequin, headless.** The demo side fetched yesterday boots through the
generic probe (`extasie_mouse_probe` + `POM2_PROBE_DISK`): ProDOS →
« MENU PRINCIPAL ARLEQUIN » (80-col text) → « DÉMONSTRATION » → its
« MENU DE DEMO » rendered full-screen in **Féline mixed mode** — the
maker's own condensed-text driver, yellow on magenta, 560 wide, through
POM2's per-dot mixed rule. The graphics editor asks for side 1 (Pascal),
as the real thing did. Corpus § 5 updated; a pinned golden of that menu
screen is the natural next step.

**P3 closed as bounded.** The PLS100 simulation (over the decoded fuse
map, § 3.5.1 of the plan) settles what the chip IS: a dot-stream router
and cell assembler — at I9=0 the window taps map I2→F1, I7→F4, I3→F5,
I6→F2 and at I9=1 the pairs swap; families keyed by I12/I13/I14/I4 feed
the same four latch lines with F6 as a family strobe. Nothing in it is a
palette. So SPEC1/2, DASH and the COL280 colours are decided downstream
(the LS parts and the resistor DAC on the board photo), and the fuse map
cannot arbitrate them: they stay modelled from the manual's prose and
Purplesoft's bytes. Reopen only on a schematic or board trace.

**P6, first rung: the mode latch beam-races.** The card appends a
timestamped (cycle, fifo before/after) edge to a small ring at every
clock; `forEachBeamSegment` now walks the latch in parallel with
DisplayState from the same event log — a Dhgr ON→OFF event IS the AN3
rising edge, clocking the logged 80COL level — seeded by
`latchBefore(first event)`. Each band paints with the latch of its own
moment (`bandLatch_` → `dhgrModeFor`), so a French-Touch frame can run
COL140 on top and BW560 below. Pinned by `chatmauve_latch_split`, both
directions plus a no-clock merge guard. Still per frame: the Eve's
`$C0Bx` (no video events), the in-cell dot position, the TTL palette.

**P4, partial: the RVB Graph exists.** Fifth variant (`rvb`), II/II+
slot card, with the only four registers on record (forum.system-cfg
t=9395, from a Sonotec clone): `$C0F0-$C0F3` at the card's device select
= colour + white text / colour + green text / mono white / mono green —
any access decodes, like Apple's own switches. COL140/BW560 by the
latch, no mixed, no 160; mono strobes force the 560-dot monochrome; green
text rides the TXTGREEN pass. The text-colour register, the six
programmable HGR colours and the dotted-lines option stay unmodelled:
the manual has never surfaced. Snapshot blob v4 carries the strobe.

**P5 stays deliberately unbuilt.** The //c adapter's inferred-80COL
quirk has no grounded mechanism anywhere public: fenarinarsa's two
articles give the symptom only (PoP's title dropping to mono), and the
silicium.org clone thread — the one place the LCM chip was reverse
engineered — sits behind an Anubis proof-of-work wall that no plain
fetch passes (a real browser session is the way in, when one is
connected). Modelling a failure mode without its mechanism would be
fiction; TODO records the exact source to unlock.

## 2026-09-02 — Extasie's mouse: slot 4 or nothing

**Reported: "the mouse does not work in Extasie."** Reproduced headless
(`tests/extasie_mouse_probe.cpp` boots the ProDOS disk, walks the menus by
keyboard, injects host-mouse motion, and logs every device-select write) and
diagnosed from Extasie's own code: DET's mouse layer reads the firmware
entry-point table at `$C412+` and calls it by **self-modified `JSR $C4xx`**
(`$75FA`: `LDX $C414 / STX $763E / LDX #$C4 / LDY #$40 / JSR $C400`) — no
slot scan, slot 4 or nothing. With the mouse in slot 4 the probe shows the
whole chain working, for BOTH card variants: firmware clamps the mouse to
`[0..559]×[0..191]`, MOUSE_READ streams (hundreds of thousands of PIA
accesses), and the arrow cursor tracks the injected motion. In any other
slot Extasie never touches the card. The reporter's config had the mouse in
slot 1; POM2's old fresh-install default put it in slot 2 — dead either way.

Two changes. The **fresh-install default map** now gives slot 4 to the
mouse — Apple's slot for it — and slot 2 to the Mockingboard, which is safe
for the priority corpus because DIX *scans* `$C7→$C1` for its 6522
(`boot_unidisk.a` `bdet`, timer read twice, expects −8); titles that
hard-code a slot-4 Mockingboard must swap the two in Slot Config, which now
**warns inline** whenever a mouse card sits anywhere but slot 4 (same
pattern as the slot-3 printer warning). Existing installs keep their saved
`slot_N_card` keys — the warning is what reaches them.

Found on the way: the probe first ran the MAME-LLE card with its two 2 KB
ROMs swapped (`loadRoms(slotRom, mcuRom)` takes the slot EPROM FIRST; both
are 2048 bytes, so the mistake loads silently and the firmware page reads
zeros — no signature, no entry table). The probe now passes them the right
way round; worth remembering when wiring the LLE card by hand.

## 2026-09-01 — Le Chat Mauve, P0-P2: four cards in one, dot-exact, and Purplesoft says what the manual could not

`docs/chatmauve_plan.md` phases P0, P1 and P2 landed the same day the plan
was written, and the card maker's own software corrected the plan three
times before evening.

**One card, four variants.** `LeChatMauveCard` now carries a *variant* —
Féline, Adaptateur //c, Eve, Video-7 — as a card setting
(`chatmauve_variant`; //c-class profiles default to the adapter, the others
to the Féline) rather than four catalog keys: a dozen call sites reason about
"the RGB card" by the one key `chatmauve`, and the profile is what decides the
card's home. The card owns the *state* (the patent's 2-bit latch, the Eve's
switch byte, CPREG) and answers three questions — `dhgrMode()`,
`hgrMode(an3On)`, `textMode(eightyCol, an3On)`; `Apple2Display` routes on the
answers and owns the pixel rules. The Video-7 variant is where the old
behaviour went (F/B text with the foreground in the high nibble, F/B HGR when
AN3 is off, the 160-wide chunky mode): none of that exists on a Chat Mauve,
and the Féline — the //c PAL profile's default — now shows plain text and
monochrome HGR (`POKE -16290,0`) in those states, as its manual says.

**The Féline, dot for dot.** The mixed DHGR rule is now the hardware's: a
per-byte 560/140 mux over a free-running 4-dot cell latch, so a colour cell
that runs into a BW byte is *cut* and a BW byte that runs into a colour byte
has its *last dot repeated* to the cell boundary — fenarinarsa's measurements
on a //c adapter, AppleWin PR #837. `chatmauve_dot_rules` carries literal
ports of AppleWin's `UpdateDHiResCellRGB` and `UpdateHiResRGBCell` as oracles
and agrees with them over every dot of 3×32×192 and 64×192 random rows, then
spells the three boundary cases out by hand. MAME's byte-level rule, which
painted a partial cell from the mixed nibble, is retired for this card. Two
inventions of POM2's own went with it: HGR turning monochrome when the latch
read BW560 (AppleWin never consults the latch in single HGR), and
`invertBit7` flipping the HGR bank bit (it is a mixed-mode selector, and
nothing else).

**The Eve, from its manual and from Purplesoft.** Sixteen switches at
`$C0B0-$C0BF` (any access decodes, a write loads CPREG, all off at power-on,
LOCKRES vs Ctrl-Reset, the slot-3 collision guard widened to the whole
window), TXT16 with the aux nibbles the other way round from Video-7's (high
= background — `POKE -16199,16*F+C`, F = fond), TXTGREEN as a white → green
pass over the text rows, LOCKCPREG, ENHRCPREG. **CPREG's auto-write** — the
card depositing its colour byte in aux at the address of every CPU write to
the text or HGR page — is a `Memory` hook, `setAuxShadow`, built the way the
write watchpoints are: arming clears `writable[]` over the page so the fast
path's own test diverts the write, nothing is tested for on the hot path, and
the two diversions are pinned to coexist. `$C0BA/B` was never "HGR
Duochrome": it is TXTGREEN, and the fg/bg HGR that toggle used to select is
the Eve's CP280, chosen by table IX-1.

**Then `PURPLESOFT*` was read.** Its `& GR 1..10` handler selects modes
through five switch tables; they settle what the manual's scan left
ambiguous. BW560 is **HR2+HR3** (the pair that is HRBW with AN3 on), not HR3
alone; SPEC2 is HR1+HR2. Booting the demo disk with an Eve
(`tests/purplesoft_eve_probe.cpp`, ~1 s of wall time for 400 s of machine)
then corrected the model twice more. `& GR 6` (COL140) leaves the patent
latch at 00 — a Féline would show BW560 — and expects COL140: **the latch
plays no part on the Eve**. `& GR 9` (CP280) runs with **80COL off**: the Eve
*is* the auxiliary memory and has the attribute byte whatever 80COL says; only
the 560-dot modes need the doubled shift rate, and Purplesoft's 80COL table
says exactly that (on for 6, 7, 8, 10; off for 9). And a program typed into
the probe (`& COLOR= 9: & PLOT 0,0 TO 100,0` …) read the COL280 bit order
straight out of the bytes `& PLOT` writes: orange = (main `$2A`, aux `$55`),
green = (`$55`, `$2A`), white = (`$7F`, `$7F`) — **COL280 is the 560-dot
stream in 2-dot cells**, not one bit from each bank per dot, which closes
the plan's first open question without the PLA. `DEMO GR16K` and `DEMO
TEXTE` now come out as the maker drew them: green COL140 with 16-colour
lines, COL280A/B in their four colours, CP280's orange field with attribute
blocks at the crossings, BW560, colour text on black, green 80-column text.

**Also found — and fixed: the legacy gate stopped the spindle instantly.**
Headless DOS 3.3 paid RWTS's one-second motor-on wait on nearly every sector
(the demo disk took 115 s of machine time to reach the prompt;
`dos33_save_smoke` had written "~30M cycles" down as a fact of life). The
mechanism, read out of RWTS's own bytes in the running machine: the "is the
disk spinning?" check reads `$C08C` repeatedly BEFORE re-asserting `$C0E9`,
and skips the wait only if the latch moves — on real hardware the analog
card's 556 one-shot keeps the spindle driven ~1 s after `$C0E8`, so between
back-to-back RWTS calls the disk is still turning. The bit-LSS path has
modelled that window since the MAME port (`MODE_DELAY`); the legacy nibble
gate — plain `.dsk`, no P6 PROM, i.e. every headless test — cleared `motorOn`
on the spot, freezing the latch at `$FF`. `$C0E8` on the legacy path now arms
the same one-second `motorOffDelay` (a `$C0E9` inside the window cancels it;
the countdown in `advanceCycles` stops the drive and fires the spin-down
sound). Boot to the Purplesoft prompt: 115 s → under 10 s of machine time,
3 % of boot samples in the wait loop instead of 40 %. Pinned by
`diskii_motor_coast` — spin, coast, cancel, stop.

**What the harness grew.** `display_golden_hash` has a per-variant
`cm/<variant>/…` block (26 entries, including the four mixed-mode boundary
rows) and `POM2_GOLDEN_DUMP=<entry>:<line>` prints a scanline as hex RGB, so
a rule can be argued from pixels. `video7_parity_smoke` runs on the Video-7
variant and no longer pins the mixed mode against MAME (MAME's rule is the
approximation). `display_persistence_smoke` moved its BW560 check into DHGR.
`docs/test_corpus.md` § 5 registers the corpus. Snapshot blob v3.

## 2026-09-01 — Best1a.nib was not a bad dump, and the Chat Mauve gets a plan

**Twenty bytes.** `disks_5.4/gist/Best1a.nib` hung every boot in the Disk II
PROM's prologue hunt at `$C65E`, motor on: track 0, physical sector 7 had no
readable data field. The first reading — a bad dump, a real machine would
hang the same way — was wrong, and git said so: the folder is tracked, and
commit `06f8d62` (2026-07-30, "Update gist disk images from emulator
write-back") had changed exactly 20 bytes of the file, the sync run, the
`D5 AA AD` and the first nibbles of that one data field, for a burst of
almost-all-ones that is what a handful of LSS-written self-sync `$FF` look
like once re-serialised into a byte-aligned `.nib`. POM2 did it. Restored
from `f1e6bb6`; the disk boots to its menu. The lesson goes in TODO with the
disk: a `.nib` that stops booting is diffed against git *first* — `cmp -l`
by 6656-byte track tells the disk from the emulator in one line. What wrote
~740 cycles of sync onto track 0 that day is open (that was the day the
slot dispatch was being rewritten for speed); `iic_external_smartport`
case C now pins the rule it broke — an empty external port leaves the 5.25"
with zero flushes and nothing dirty — which passed silently before because
nobody had asserted it.

**`docs/chatmauve_plan.md`.** The research pass on the Chat Mauve family
(the Eve reference manual and its erratum, the Eve's PLS100 fuse map, the
Video-7 patent, the *Manuel Arlequin*, fenarinarsa's real-hardware
measurements) written up as the phased plan to a dot-level model, mixed
DHGR first. Two of today's labels are wrong and are named there:
`$C0BA/B` is TXTGREEN, not "HGR Duochrome", and the fg/bg HGR POM2 renders is
the Eve's CP280 with the nibbles swapped. Table IX-1 read off the scan
(BW560 is HR3 alone; the "blanked" row keeps CPREG working), and the
addendum's colour-table program turned out to *be* the rev A palette: rev B
with the 4-bit code rotated right by one. `TODO.md` opens with a "Next up"
section — the plan, then the one-character-cell mid-scanline offset DIX's
rays expose, then the Best1a finding.

## 2026-09-01 — The bus survives a rewind, and the DIB says what it is

Two notes bug hunt 3 had left in TODO as "small and honest", closed.

**The port's state is in snapshots now.** The rewind ring snapshots every
frame and a 3.5" block transfer spans several, so "start clean on restore"
was not the harmless choice it read as: a rewind landing inside a READ
handed the firmware an empty reply and, after its retries, an I/O error the
guest never earned. `SmartPortBusDevice` serialises the frame being
received, the reply being read and where in it, the REQ/ACK state, the
pending WRITE and the host-assigned chain numbers; `IIcExternalSmartPort`
adds its private IWM (behind a length — that blob is opaque and variable)
and its line bytes, riding as a self-identifying `XSP1` tail on the
//c-class profile's blob; `LironCard` carries the same for its own IWM and
bus (`LIR1`). Older blobs, and machines without a port, are the previous
layout and start the port clean. A truncated tail is refused as a whole,
not half-applied. Pinned on the bench: a device snapshotted between the
ack and REQ's release serves the armed reply after restore, with its chain
numbers.

**The DIB names the device it is.** STATUS code 3 answered "UniDisk 3.5,
type $01" for everything; an HDV on the bus is a hard disk (type $02,
subtype $80 — extended calls, not removable) and says so.

## 2026-09-01 — Bug hunt 3: seven findings on the day's work, all confirmed, all fixed

A five-lens hunt over `a6b2a03..HEAD` (bus protocol, //c port wiring,
IWM/Liron/Sony, browser persistence, threads and lifetime), each finding put
to an adversarial refuter. Eight raw, seven after dedup, seven confirmed,
none refuted. In order of what they would have cost:

**1. The bus's bytes were also fed to the Disk II** (`Memory.cpp`). The read
side already kept a claimed access from the slot-6 card; the write side
forwarded every `$C0Ex` write to it regardless. The firmware's packet
sequence — drive 2, motor on, Q6+Q7, `STA $C0ED/$C0EF` — is exactly a Disk
II write sequence, so the card entered write mode and spliced the packet as
flux into the track under its head. With Disk II write-back on (opt-in):
8 flushes on a plain //c 5.25" boot, 31 booting slot 5, 287 on a //c+ where
the disk in question is the boot disk; `.woz`/`.nib` took the garbage to
the file, sector images kept the file but lost track-0 sectors for the
session. `ioWriteIWM` now returns whether the port claimed the write and
`Memory` drops it from the slot bus when it did — the mirror of the read
gate. Pinned: `iic_external_smartport` now runs every scenario with a
writable scratch 5.25" in the shared drive and write-back on, asserting zero
flushes; mutation-checked (the old code fails it with the measured 8 and 31).

**2 + 4. `bootFromSlot(5)` on the //c+ jumped into the real `$C500` and
failed** — two lenses found it independently. With a device on the port the
stub is withheld, the real page carries a valid signature, and entered
directly (not from reset) the //c+ firmware never scans the port: "UNABLE TO
FIND A BOOTABLE DISK ONLINE". Its reset-time scan at `$F223` does find the
device, so on a //c+ whose firmware serves the port an explicit boot of slot
5 is now a reset (`Memory::iicPlusBootsSlot5ByReset`). The plain //c's page
boots when entered directly and keeps that path.

**3. The `liron` catalog entry was unreachable from Slot Config**: the
rebuild dispatch had no branch for it, the live column no mapping. Both
added (`plugLiron`, `liveCardKey`, multi-instance), and its media now
persists: `StorageCoordinator` restored 3.5" units through `SmartPortCard`
only, so a Liron's bays started empty on every launch. Cards with bays but
no keyspace of their own go through generic `media_slotN_bayK_path /
_writeback` keys — mount, write-back, eject-all and restore, pinned in
`storage_coordinator`.

**5. `LironCard` never armed bus capture on its IWM**: packet bytes reached
the shifter, "write underrun" on every packet, and with write-back on a
track decode per block over garbage. Mirrors the port's write path now.

**6. `imgui.ini` moved without its owner.** Up to v0.8.5 it lived at
`~/.config/POM2/` on every Unix and in the launch directory on Windows;
`userConfigDir()` put it next to `state.cfg`, whose `ui_dock_seeded=true`
then stopped any re-seed — every panel floating on the first launch after
upgrade. The legacy file is carried over once when the new place is empty.

**7. The checksum was computed and not enforced.** A frame spliced from two
transactions (an eject mid-WRITE, a mount, the retried command appended
behind the stale `$C3`) could decode as a data packet and write a block of
garbage. A bad checksum now gets no reply (the firmware retries), and any
change in which units hold media resets the protocol. Pinned on the bench:
`smartport_bus_device` (frames built by hand — host-assigned numbers,
refused checksum, two-packet WRITE, STATUS bytes).

**One outside the diff, from CI rather than the hunt**: `sp_over_slip_link`
aborted once on a macOS runner — `isConnected()` had gone false while
`deviceCount()` still said 2. `peerLostLocked` dropped the socket before
clearing the devices; the two are read under different locks, so the order
was the only thing between an observer and "not connected, two devices".
Devices first now.

## 2026-09-01 — The rear connector on the //c+ too, and what the 16 KB //c cannot do

"The external port exists on the //c+ and the 16 KB //c as well." It does,
and the two answers are opposite.

**The //c+ probes it.** A trace of the //c+ boot shows its firmware asserting
PH1 + LSTRB with SEL on drive 2 and polling SENSE fifty times at `$F223`
(bank 1) — the SmartPort presence scan, same protocol as the Liron's (its
bank 1 carries the identical sync table at `$C88F`), a different
implementation (trampolined through page 3, the send at `$C895`). Nothing
answered, so an 800K on the slot-5 card was invisible to a //c+ exactly as
it had been to a //c. The wiring differs from the //c's in one respect: the
//c+'s shared IWM already owns the port for the MIG-routed Sony drives, so
the port rides along on that chip — `SmartPortBusPort` grew a `shared*`
half the profile calls around its own IWM access — instead of tracking
registers of its own.

**And it taught the responder something the //c had let it get wrong
harmlessly: chain numbers belong to the host.** The first //c+ INIT arrived
for device **2** — its internal MIG drive is device 1 — and a responder that
mapped "destination 2" onto its second (empty) bay refused the boot's READ
with `$2F`. A real drive does not know its number until the scan tells it.
`SmartPortBusDevice` now takes the number each INIT carries, in chain order,
and forgets them on bus reset. With that, an empty internal bay boots the
external 3.5" (ProDOS 8 after 39 block reads) and a full one lists both
slot-5 units; `iic_external_smartport` pins both as cases D and E.

**The 16 KB //c cannot.** ROM 255 has no SmartPort firmware at all — its
`$C500` reads `FF 20 4D CE …`, not a disk controller page — which is
historically why Apple sold the ROM 0 upgrade with the UniDisk 3.5. On that
machine the rear connector takes a second 5.25", which POM2 already has as
`DiskIICard` drive 2, and a 3.5" stays reachable only through the host-served
`$C500` substitute. Written down so nobody goes looking for a probe that is
not there.

## 2026-09-01 — The //c gets its external 3.5" back: the SmartPort bus, finished and wired

The request was plain: a //c with its internal 5.25" **and** a 3.5" on the
rear port, both working. Measured before touching anything, it did not: a
5.25" boot listed `S6,D1 S6,D2 S3,D2` with an 800K image mounted on slot 5,
and arming the host-served `$C500` stub from cold dropped the machine into
the monitor at `$0806`. The cause was never a bug in POM2's stub: the //c's
own boot does `JSR $C5F8` into that page, and the page under it IS the disk
controller firmware — the Liron's, byte for byte — which talks to the drive
as an intelligent device over the SmartPort **bus**. No stub survives being
the thing the machine's own firmware jumps through. The answer had to be the
bus, and the bus responder started the same morning stopped after one
transaction.

**Why it stopped: SENSE is a handshake, not a presence flag.** The first
responder derived the line from its buffers ("high while no command is
pending"), so after the INIT reply it answered LOW to the next probe — the
boot's READ — and the firmware's 3000-retry loop looked exactly like a hang.
Read from the ROM, the line is the device's ACK: HIGH whenever REQ (PH0) is
low, LOW when a packet has been taken or a reply fully read; the host waits
for that edge before it releases REQ, and the release is what makes the
device ready again and what puts a prepared reply on the wire. Modelled as
that state machine (`SmartPortBusDevice`, its own translation unit), the
firmware ran the whole enumeration — INIT to device 1 ("more follow"), INIT
to device 2 ("last") — and then sent the READ.

**Then two facts about the packet the wire had not needed yet.** The unit is
the packet's **destination** (`$CD02` swaps the chain number into `$5A`,
`$C837` puts it on the wire); contents byte 1 is the parameter count from the
firmware's own table at `$CDE3`, and reading it as the unit refused every
READ with `$11`. And the header's status byte is the ProDOS result — the
`$01` that had satisfied the INIT scan ("non-zero ends the scan") made every
successful READ an error `$27`. With those, `liron_boot35` boots ProDOS 8
through the real EPROM, and the Liron is in the catalog as `liron`.

**Wiring it to the //c without breaking the 5.25".** On a real //c one IWM
drives both drives; POM2 deliberately keeps the 5.25" on `DiskIICard`, and
`iic_diskii_no_iwm_conflict` pins that the machine's shared IWM stays out of
`$C0E0-$C0EF` on the plain //c because a second controller there once sent
DOS 3.3 into seek storms. So `IIcExternalSmartPort` carries its **own**
IWM, used purely as a register tracker, and claims an access only while the
firmware is addressing the bus (PH1 + LSTRB with the port enabled, or a
transaction in flight) and only while a slot-5 unit holds media. The units
come from the built-in `smartport35` card the //c profiles already plug —
the media panel did not change — through a new `SlotPeripheral::
smartPortBusUnit`. And while that port is live, `Memory` leaves `$C500`
alone: the real firmware runs, `bootFromSlot(5)` jumps into it, and the
arming gate becomes what it should always have been on this ROM, unused.

**The evidence** (`iic_external_smartport`, three full boots): with the 3.5"
mounted, a normal 5.25" boot brings up ProDOS with `S5,D1 S5,D2 S6,D1 S6,D2`
plus the RAM disk, after three bus transactions (two INITs and a STATUS);
Boot on slot 5 brings up ProDOS 8 off the 3.5" after 252 block reads over
the bus; with nothing mounted the port takes no command and the boot is
byte-for-byte what it was. `iic_diskii_no_iwm_conflict`, `iicplus_boot35`
and `iic_onboard_smartport_smoke` are unchanged and green.

**One thing the Disk II must not see.** A claimed access used to be forwarded
to the slot-6 card "for side effects" (motor sound, turbo); the bus traffic's
`$C0EB` + `$C0E9` then enabled the card's empty second bay, nothing ever
disabled it, and the drive-2 spin loop played for ever after a 3.5" session.
On the real machine the internal drive is not enabled during that traffic;
now it is not told about it either.

## 2026-09-01 — The SmartPort wire, decoded, encoded and checksummed

Continuing the entry below: POM2 now speaks the SmartPort bus protocol
properly rather than answering its handshake with canned bytes. The command
packet is decoded, the reply is built with the real encoding and a real
checksum, and the firmware's enumeration accepts the device and stops
scanning. It still does not boot — but for the first time the remaining
question is about the device's ANSWERS, not the wire.

**All of it was read out of the ROM.** The frame is
`FF… $C3 | 7 header | odd section | groups | chk1 chk2 | $C8`, every byte
carrying bit 7 because that is what "byte ready" means to the IWM shifter.
The header lands at `$0051` down to `$004B`, so `$004C` is the odd-byte count
and `$004B` the count of seven-byte groups. Each group is a marker byte
carrying the seven data bytes' bit 7s, most significant first — POM2 derives
that from the ROM's own tables at `$CA27/$CA37/$CA47/$CA57`, which contain
nothing but `$80`/`$00` masks keyed on the marker's bits. The checksum is a
running XOR of the header WIRE bytes, the decoded group bytes and the decoded
odd bytes, sent 4-and-4 and recovered as `((chk2 << 1) | 1) & chk1`.

**The wire is not a guess any more.** The checksum POM2 computes for a
header-only reply is `$81`, and `$81` is the byte the firmware's own `$40`
holds when it reaches its terminator check. That number matching is the
difference between "it seems to work" and "we know what it is".

**The bug that cost the most, and reads like a checksum failure.** A first
version answered every command with a 512-byte block. The enumeration's reply
buffer is a few bytes of zero page — so the payload walked over `$004B` and
`$004C`, the very fields that say how long the packet is, and the firmware
rejected it. The fix is to look at the command: `$05` (enumerate) gets a
header-only status answer, `$01` (read block) gets the 512 bytes. Two other
mistakes worth the same warning: the phase pattern ADDRESSES the device but
does not gate every byte — the firmware drops PH1 as soon as it starts reading
the reply, so a per-access gate hands the rest of the packet to an empty IWM —
and a write once a reply exists opens a NEW transaction, without which the
command buffer accumulates the whole session and every answer is the first
one.

**Where it stops**: after the enumeration the firmware does not proceed to the
boot's block read. The likely gap is the content of the status reply — a
device descriptor the scan stores (`$07F8,Y` holds the count, `$CCD1` checks
the unit against it) and the boot consults. The next step is the enumeration's
continuation at `$CE34`. `smartport_bus_handshake` pins the water mark
reached, including the decoded command (`$05`, nine body bytes) — a decoder
that silently mis-framed would still hand back a reply and still look fine
until block numbers mattered.

## 2026-09-01 — Speaking the SmartPort bus, three steps of five

The wall the entry below describes is a protocol, and a protocol can be
answered. `LironCard` now carries an optional byte-level responder for the
SmartPort **bus** — the exchange an intelligent UniDisk 3.5 has with the
firmware — and the firmware now gets three steps further than "no device
connected": it sees a device, sends its command packet, and reads back a
header reply. Then it asks for the payload, and POM2 has nothing to say.

**The protocol was read out of the ROM, not looked up.** POM2's own
`disassemble6502` over `roms/liron.rom`: the probe at `$C800` (PH1 + LSTRB
high, SEL, motor, then fifty polls of the status register for SENSE), the
sender at `$C87D` (bytes to `$C0nD` with bit 7 set, write handshake at
`$C0nC` between them, seven data bytes per group behind a byte of their
gathered high bits), the drain at `$C92C`, the acknowledgement at `$C943`
(SENSE must go LOW), and the receiver at `$C960` — which re-asserts the
attention lines, reads `$C0nD`, waits for SENSE HIGH, hunts for `$C3` and
takes seven header bytes into `$0051` down to `$004B`. `$004C` is the payload
length and `$004D` must be non-zero or the enumeration throws the device away.
The full map is in `TODO.md` § Storage, and the //c's bank-1 firmware is the
same code byte for byte, so it serves both machines.

**Three things the firmware taught, each of which cost a run to find:**

- The write handshake's bit 6 is an underrun flag the firmware waits to see
  **clear** after its last byte. Answering `$C0` (bit 6 set) parks it in the
  drain loop at `$C92C` for ever. A device with nothing in flight says `$80`.
- SENSE is not a write-protect line here, it is the device's attention line,
  and it has three states in one transaction: HIGH for the presence poll, LOW
  to acknowledge the command, HIGH again when the reply is ready. Holding it
  high throughout stalls the acknowledgement; holding it low stalls the reply.
- There is no usable edge on the handshake lines to trigger the reply on — the
  firmware leaves PH1 and LSTRB up across the whole exchange. Its read of
  `$C0nD` at `$C97A` is the only distinctive event, and nothing else in the
  transaction touches that offset.

**Off by default**, and the default is the honest one: with the responder off
the card behaves like a Liron with an empty port, which is a shippable state;
with it on the firmware advances and then waits, which is a workbench. Two
tests pin the two states — `liron_boot35` and `smartport_bus_handshake` — and
between them they say exactly where the boundary is.

**For whoever wires this to the //c**: that machine's internal drive is a
**5.25"** owned by `DiskIICard`, with the 3.5" on the rear expansion port.
The responder must answer for the external device only;
`IIcClassProfile::ioReadIWM`'s `isPlus_` gate exists because an IWM and a
DiskIICard fighting over one 5.25" drive corrupt the head position badly
enough to send DOS 3.3 RWTS into seek storms. The Liron has no internal drive
to fight over, which is why the protocol work belongs there first.

## 2026-09-01 — The Liron card, and the wall both it and the //c hit

`LironCard` is the Apple II 3.5" Disk Controller as silicon: the real 4 KB
EPROM (`roms/liron.rom`) executing on the 6502, a real `IWMDevice` behind
$C0nX, real Sony mechanisms. Not to be confused with `SmartPortCard`, which
wears the same hardware's name and answers ProDOS's block calls from the host
— that one works and stays; this one was to be the same machine as the //c+,
on the expansion bus.

**It does not boot, and the reason is worth the card.** TODO § Storage
estimated 8-12 hours on the grounds that "the risk was never the card". The
card was indeed small. The risk was the DRIVE. Disassembled from the dump with
POM2's own `disassemble6502`, the firmware's device scan at $C800 drives PH1
and LSTRB high, asserts SEL and the motor, then polls the status register
fifty times for the SENSE line — and on timeout reports ProDOS $28, "no device
connected", with no fallback. Holding LSTRB high through a status read is the
tell: on a dumb Sony mechanism that line is the write strobe. This is the
SmartPort **bus** handshake, which only an *intelligent* device answers — a
UniDisk 3.5, with its own 65C02 inside the drive. POM2's TODO has kept that
processor out of scope from the start; it turns out to be the thing standing
between this card and a boot, and it took building the card to learn that.

**The plain //c is the same code, byte for byte.** `apple2c-32Kv0.rom` bank 1
$C88C matches the Liron's $C806, and $CA80 matches $CA05 — one Apple code base
in two packages. So the campaign's last two items were never two ports: they
are one missing subsystem, a SmartPort bus-level responder. (The //c+ boots
because its ROM *also* carries a GCR path for its on-board dumb drive,
selected through the MIG. The 16 KB //c dumps carry no 3.5" firmware at all.)
Two independent dumps and a running trace say so, which is why the backlog now
says it in one item instead of two.

**Two bugs found on the way, both in the card and both invisible without the
real firmware:**

- Phase lines were forwarded only to the *selected* drive. The firmware sets
  the Sony register address up BEFORE it enables a drive, so every sense read
  answered for register 0 and the probe never even strobed. CA0-CA2 and LSTRB
  are wired straight to the connector on real hardware; they go to the whole
  chain now.
- `devsel` was mapped to the drive, as the //c+'s MIG-driven hub does it. A
  Liron has no MIG: SEL is head select, and drive selection is the daisy
  chain. Mapping SEL to a second (empty) bay meant every probe answered for a
  drive with no disk in it.

The card is deliberately **not** in the slot catalog yet: shipping a card that
enumerates nothing would be a worse answer than `SmartPortCard`, which serves
the same volumes and works. `liron_boot35` pins what is real today — the ROM
identity, the per-slot page, and the probe reaching the drive — and
`POM2_LIRON_BOOT_STRICT=1` is the acceptance test waiting for the responder.

## 2026-09-01 — The //c+ boots a 3.5" disk with its own firmware

Not through POM2's host-served SmartPort substitute at slot 5 — through the
real path: the //c+ ROM drives the MIG gate array, the MIG selects the drive,
the IWM walks the bit cells, the Sony drive turns its motor and steps its
head, and **ProDOS 8 v2.4.3 boots off the internal 3.5" bay** with nothing
else mounted in the machine. TODO § Storage called this "the largest
remaining fidelity gap in the storage stack" and put three features behind it.

**The evidence, because "it boots now" is a claim worth distrusting.** The
same harness on the pre-fix build prints `UNABLE TO FIND A BOOTABLE DISK
ONLINE.` and leaves the head parked on track 0 — it cannot read the boot block
to learn where to go next. Post-fix, on four in-repo 800K images: A2DeskTop
reaches ProDOS at 8.4 M cycles, The New Print Shop walks the head to **track
22** across both sides and paints 7401 of 8192 hi-res bytes, DIX paints 4128.
Same harness, same images, same cycle budget; the difference is the
controller and nothing else. Pinned as `iicplus_boot35`, and
mutation-checked: restoring either half of the controller fix turns the test
red with the firmware's own message.

**Nothing in the firmware, the MIG or the drive model had to change.** All of
it was already there and individually pinned; what was missing was a test that
crossed the seam between the encoder and the IWM's walker. Both faults are in
the entry above.

**A harness bug worth recording, because it cost an hour and pointed at
innocent code.** The first version of this test plugged no card into slot 6.
`IIcClassProfile::ioReadIWM` hands the CPU the IWM's byte only while a 3.5"
Sony is selected and falls through to the slot-6 DiskIICard otherwise — with
no card the fall-through returns the **floating bus**, and $FF has bit 5 set,
which reads as "drive enabled". The firmware then sat in a loop at $E51D
polling a status it thought it had already got, and every pass through its MIG
routine reset the IWM. It looked exactly like a MIG decode bug. It was an
empty slot: the //c+ has an on-board 5.25" drive, and modelling the machine
without it is not modelling the machine.

## 2026-09-01 — The 800K read path works: two faults, one clock

The harness below said the fault was between the encoder and the decoder, in
the flux → bit-cell-window path, and that granularity was the suspect. Acting
on that took two fixes, and the second one only became visible after the
first. The path now reads **every sector on all five speed zones, both heads,
payloads exact**, through the real `IWMDevice`: 12/12, 11/11, 10/10, 9/9, 8/8.

**1. The IWM does not run on the CPU clock, and now POM2 does not pretend it
does.** On a //c the controller is wired to A2BUS_7M — 14.31818/2 = 7.159 MHz
— which is exactly seven times the 6502's 14.31818/14. POM2 had collapsed the
device onto the CPU clock and divided MAME's window constants by 7, so 28/14
became 4/2 and 36/18 became 5/2: two of the four window settings ended up
identical, and none could place an edge inside a 2.02-cycle Sony cell. The
state machine now counts in IWM ticks (`POM2_IWM_TICKS_PER_CPU_CYCLE`,
CpuClock.h) and MAME's 28/14/36/18, its 4/8-tick register delay and its 7- and
14-tick write constants are used verbatim — several of which had been
individually "corrected" over time in ways that were each wrong (the async
stale-data timer went 14 → 2 to stop it firing 7× late; in ticks it is 14
again, which is what the hardware always said). `Sony35Drive`'s flux timeline
moved to the same unit, read and write side together.

That alone took address-field recovery from 4 of 24 to 17 of 24 — and still
zero complete sectors.

**2. `nextTransition(lastSync_ + 1)` where MAME asks from `lastSync_`.** POM2
queried the flux stream one tick late, so any transition landing exactly one
tick past the last sync point was skipped — and `nextFluxChange` is a local
reset on every `sync()` entry, so the transition was skipped for good rather
than picked up on the next pass. That alignment is not rare: `lastSync_` parks
on the caller's poll boundary every time a `sync()` runs out of time
mid-window. The cost was one 1-bit read as 0 roughly every 35 bytes — enough
for an 8-byte address field to survive and a 700-byte data field never to,
which is exactly the shape the numbers had. Invisible under the old clock,
where coarser errors swamped it.

**Whether this is what a real //c+ needs is still unproven** — the firmware
has to drive the MIG, the IWM and the drives itself, and that is the next
item. What is proven is that the controller underneath it reads Sony GCR.

Two consequences worth knowing:

- **Snapshot blobs are `IWM2`.** The state-machine stamps in them are ticks,
  a different number for the same instant; restoring a v1 blob verbatim would
  park the walker seven times too early and freeze the device until emulated
  time caught up — the exact hazard `iwm_mig_snapshot` exists to prevent. The
  loader accepts both and scales v1.
- **`Sony35Drive::writeFlux` and `nextTransition` take ticks now.** Read and
  write have to share one timeline or a write lands on a different cell than
  the read that verifies it.

The harness's investigation half is a plain test as of this commit: it
asserts what it used to merely report. `POM2_GCR_CONTROL_ONLY=1` still runs
only the encoder→decoder control, which is what to reach for if it ever fails
again — it says in one run whether the format or the controller broke.

## 2026-09-01 — The 3.5" harness, and the answer it gave in five minutes

TODO § Storage has asked, for months, that the 800K campaign start with a
harness rather than a card: "find out where the GCR read path diverges …
building a card first would only produce a card that does not boot." Three
features sit behind that one question — the //c+ on-board boot, a Liron card,
the plain //c. `tests/sony35_iwm_read_path_test.cpp` is that harness, and the
answer is unambiguous:

| Path | Address fields | Sectors |
| ---- | -------------- | ------- |
| blocks → Sony GCR encoder → decoder, no IWM | 12 of 12 | **12/12, payloads exact** |
| the same track through `IWMDevice`'s window walker | ~4 per revolution | **0** |

Same on all five speed zones and both heads. The encoder, the zone layout,
`sony35::blockIndexFor` and the decoder are therefore exonerated, and that
half of the harness is now a pinned regression test (`sony35_gcr_zones`)
covering every zone on both heads — which nothing did before; the existing
3.5" tests either stop at `debugCellStream()` or never leave track 0. It is
mutation-checked: shifting `blockIndexFor`'s head term by one zone fails it on
four tracks.

**The finding.** The fault is in the flux → bit-cell-window path, and its
shape says granularity. The drive clocks a cell every 155745 / 76950 = **2.02
CPU cycles**; POM2 runs the IWM state machine on whole CPU cycles, so
`windowSize()` is 2 and a resync can only ever land on a cycle boundary — half
a cell of error is a coin flip. MAME runs the identical walker on the card's
~7.16 MHz clock, where the same window is 14 ticks. The parity dashboard
already carries this as an open item under `IWMDevice` ("Q3 fast clock
(Mac/IIgs only)") without anyone having connected it to the 3.5" boot. So the
next step in the campaign is a finer time base for the 3.5" read path, and
**not** a Liron card: a card debugged against a controller that cannot read a
sector produces two mysteries instead of one.

**Three bugs in the harness itself, all worth writing down**, because each is
a way to conclude something false about the hardware:

- Stepping the head with side 1 selected does nothing at all. The drive's
  register address is `{ HDSEL, CA2, CA1, CA0 }`, so "step" (1) becomes "MFM
  mode on" (9) and a `while (track() < n)` loop never ends. The harness now
  selects side 0 before seeking, and bounds the loop so a regression here
  fails the test instead of hanging CI.
- The mode register is written by an odd-offset write while **Q6 and Q7 are
  both set**; `write(0xE, …)` clears Q7 rather than setting Q6. The first draft
  swept four mode bytes and silently measured mode $00 four times. The harness
  now reads the mode back and fails if it did not take — the sweep exists
  precisely to compare modes, so a sweep that cannot change the mode is worse
  than no sweep.
- Restarting the harness clock at a fixed value for each track read froze the
  device: the IWM keeps absolute cycle stamps and its `sync()` walker only
  moves forward, so a timestamp older than the one it already reached reads
  nothing at all. The first track worked and every later one came back empty.
  `IWMDevice.h` documents exactly this hazard for the rewind path; it bites a
  test the same way.

## 2026-09-01 — The browser build can remember things now (IDBFS), and the two surprises

`state.cfg` and `imgui.ini` survive a reload of the WASM build. The backlog
called this a 2-4 hour quick win with a one-line diagnosis — "`/persistent` is
mounted via IDBFS but `Settings.cpp` writes to `$HOME`" — and that diagnosis
was right and nowhere near sufficient. Three things were wrong, and only the
first was the one written down.

**1. Nobody wrote to the mount.** `pom2::userConfigDir()` is now the single
answer to "where does POM2's configuration live": `%APPDATA%`, `~/Library/
Application Support`, `$XDG_CONFIG_HOME`, and `/persistent` under Emscripten.
`Settings::resolveStorePath()` and main.cpp's `imgui.ini` path both call it.
They used to be two hand-copied platform dances — main.cpp's even carried a
comment saying it "mirrors `Settings::resolveStorePath`" — and a mirror is a
thing that can stop matching. Under Emscripten it had: neither copy knew about
the mount, so both wrote to MEMFS, which is a fresh empty filesystem on every
visit.

**2. A write to IDBFS is not durable until `FS.syncfs`.** That has no native
analogue — the desktop write path is a rename plus an fsync and is durable when
it returns. `PersistentFs.h` holds the browser half: writers mark the store
dirty, the frame loop pumps, and at most one flush runs per two seconds and
never while another is in flight. Without it every write succeeds, every read
back within the session succeeds, and the lot evaporates on reload.

**3. The browser build has no exit, so nothing ever called the writer.** This
is the one that turned the quick win into an afternoon.
`emscripten_set_main_loop_arg(..., simulate_infinite_loop=1)` unwinds `main()`
*without* destroying its locals — deliberately, so the loop's captured state
outlives the call — which means `~MainWindow()` never runs in a browser, and
`~MainWindow()` was where POM2 wrote everything it remembers. Plumbing to a
writer that is never called would have been indistinguishable from success
until someone reloaded the page. The block is now
`MainWindow::persistSession()` in its own translation unit
(`MainWindow_Session.cpp`, -280 lines off `MainWindow.cpp`), called from the
destructor on the desktop and from a 10-second heartbeat plus the page's
`visibilitychange`/`pagehide` in the browser. A heartbeat is only affordable
because `Settings::save()` now skips a save whose content matches the last one
it wrote — pinned by deleting the file underneath a second save and asserting
it stays deleted.

**The bug the test found, which no amount of reading would have.** The shell's
`preRun` hook fired `FS.syncfs(true, …)` and returned without waiting, so the
runtime could start before IndexedDB had been read back. The fix is
`addRunDependency`, and that fix introduces a failure mode strictly worse than
the one it cures: if the callback never fires, the dependency is never
released and **the emulator never starts** — the page sits on "downloading
assets…" with the splash up, no frame ever drawn. Not hypothetical. It
happened during testing, when a `deleteDatabase()` from a previous page left
the open blocked, and it cost a good while to diagnose because every symptom
pointed at the new heartbeat code. There is now a 5-second watchdog: a browser
that will not answer costs the visitor their stored settings, never the
emulator.

**Two smaller things.** The browser's chrome-light startup (hide every panel,
force the composite display mode) now runs only on a **first** visit —
`settings->empty()`. It used to run on every launch, which was harmless while
nothing persisted and actively wrong the moment something did: the store had
just restored the visitor's panels three lines above, and this closed them all
again. And the list-packing helpers (0x1F-separated paths) moved to
`SettingsList.h`, because the reader and the writer are now in two translation
units and a separator convention with two copies can disagree with itself.

**Verified for real**, not by reading: headless Chrome driven over CDP, three
consecutive visits on a virgin browser profile. Visit 1 boots with an empty
store, writes 134 keys plus `imgui.ini`, and flushes; visit 2 logs `Loaded 134
keys from /persistent/state.cfg`; a value changed between visits comes back
changed and is not clobbered by the heartbeat's rewrite. Two notes for whoever
tests this next: headless Chrome reports the page as **hidden**, which stops
`requestAnimationFrame` dead and with it POM2's entire frame loop — CDP
`Emulation.setFocusEmulationEnabled` is what makes the page consider itself
visible. And `wasm/shell.html` is **not** a link dependency, so editing it
alone rebuilds nothing: the page you serve is the one from the last actual
relink. Touch a source file.

## 2026-09-01 — The 0 % file that was dead, and the one that held a contract

P2-5 said "`RomLoader.cpp` and `CharRomCatalog.cpp` are at 0 %, write tests".
Half of that was the wrong instruction, in the same shape as the three other
premises the 2026-08-28 pass got wrong: **`RomLoader` had no callers at all.**
Not one, in `src/` or `tests/` — the single test that `#include`d the header
never called a function from it. Its API flashes a ROM into `Memory` and
restores the write-protect bitmap; cards stopped doing that when they started
keeping their ROM in their own byte array and answering the slot bus from
there (`GrapplerCard`, `SmartPortCard`, `ClockCard` each open their own
`ifstream`). So the coverage number was not naming an untested boot path, it
was naming 83 lines of code that had quietly fallen out of the machine while
staying in six source lists in `CMakeLists.txt`, nine test targets, and every
binary POM2 ships. Deleted rather than tested: a test would have pinned an
API nothing calls, and made the file *look* alive to the next reader of the
coverage table.

Worth stating as a rule, because the coverage floor will keep producing this
question: **a 0 % file is a fork, not a task.** Either something needs it and
the test is missing, or nothing needs it and the test would be the only thing
keeping it. Check the call sites before writing the test.

`CharRomCatalog` was the other half and it was the real one — 0 % because it
is reachable only from three UI translation units, while what it holds is a
**persistence contract**: `state.cfg` stores the locale as a string key, and
that mapping is written out three times over (the catalog vector, the enum→key
`switch`, the key→enum `if` chain). The drift that costs the user something is
silent by construction: a locale added to the first two and missed in the
third reads back as `ProfileDefault`, so a French //e quietly reverts to the
stock US font on the next launch with no error anywhere. `char_rom_catalog`
pins the round-trip for all fourteen locales, the key strings **verbatim**
(they are on disk in every user's settings; renaming one is a migration, not
a refactor), the unknown-key and out-of-range fallbacks, the profile partition
in both directions (a 2 KB part is never offered to a //e, a 4 KB one never
to a II+, and every profile keeps exactly one reachable "Default"), and the
two 342-0274-A entries selecting **different banks** of the one 8 KB dump —
both on bank 0 would draw identical glyphs in the picker. Both of those last
two were mutation-checked by breaking the source and watching the test fail.

One incident from doing it, since it wastes an hour when it happens: restoring
a mutated source with `cp` gave the file the **same mtime second** as its
object, `make` declared the target up to date, and the full `ctest` run failed
on a binary built from the mutation that no longer existed in the tree. `touch`
the file after any such restore.

## 2026-09-01 — TransWarp: the accelerator POM2 could model as a clock, not a CPU

Applied Engineering's **TransWarp** (catalog `transwarp`), ported from MAME
`bus/a2bus/transwarp.cpp` — with one structural divergence that is worth
writing down, because it is the kind of place where copying MAME faithfully
would have produced a *worse* model.

MAME puts a second `W65C02` on the card and has it DMA the Apple's bus for
every access. It does that out of necessity: a MAME a2bus card cannot retime
the host CPU, so substituting a faster processor is the only lever available.
POM2 has the lever — the worker's per-frame cycle budget IS the CPU clock —
so the card publishes a multiplier and the machine keeps its own `M6502`.
That is closer to what the board does (same program, same memory, faster
clock), and it costs nothing on the hot path where a second CPU would have
roughly doubled the emulation work.

The multipliers are exact rationals rather than fitted decimals: the card
runs off the 7M line, so 7.159/2 and 7.159/4 against the Apple's 14.31818/14
are **3.5×** and **1.75×** exactly.

**What the frame-rate sampling does and does not buy.** The multiplier is
read once per frame, while the card's slowdown windows (20 µs around a slot
access, a whole PREAD around `$C070`) are ~20 cycles inside ~17000. That is
not an approximation *in aggregate* — sampling a duty cycle at a rate
uncorrelated with it is an unbiased estimator of it, so the average speed
converges within a handful of frames. What it does not reproduce is where
inside a frame the slow cycles fall, which would matter to a beam-raced
effect and to nothing else.

**And the slowdowns were never load-bearing here anyway.** POM2's whole time
base is CPU cycles, so a Disk II at 3.5× spins 3.5× faster in wall-clock and
its nibble pacing per CPU cycle is unchanged — the same reason the //c Plus
profile runs at 4× with a working drive. They are modelled because they are
what the board does, not because anything broke without them. Saying so is
the point: a faithful port should be honest about which of its details are
doing work.

Two details kept from the MAME source because they are observable: `$C074` is
taken OFF the bus (its `dma_w` returns there, so the write never reaches the
paddle latch) while `$C072` is only watched and passed through; and DSW2 bit
5 ships at 0, leaving **slot 6 at stock speed** — the Disk II, the one slot
AE did not trust at 3.5×. Pinned by `transwarp_card`.

Wiring this needed two new `SlotPeripheral` hooks — `cpuSpeedMultiplier()`
and `snoopsBus()`/`busSnoop()` — both cached by `SlotBus` as a single pointer
so the snoop sites cost one null test on a machine with no accelerator.

ROM-gated shadow of `$F000-$FFFF` on `roms/ae_transwarp_1.4.bin` (4096 B,
CRC32 `afe37f55`), which POM2 does not ship. AE's speed-corrected Monitor
lives there because the stock F8 delay loops are calibrated for 1 MHz.
Implemented as a 4 KB swap in the ROM mirror, so it is free at run time.

## 2026-08-31 — Videx LOWER CASE CHIP, and two things a ROM's size never told us

The 1980 Videx **LOWER CASE CHIP** — a drop-in replacement for the II/II+
motherboard character generator — is now a `CharRomCatalog` entry
(`videx_lc`, `roms/Videx Lower Case Chip ROM.bin`). Adding it broke two
assumptions POM2 had been making from the dump's SIZE alone.

**"2 KB means no lowercase."** `Apple2Display` folded a-z onto A-Z whenever
`charRomSize < 4096`. Right for the stock generator; exactly wrong for this
chip, which is also 2 KB and exists *for* those glyphs. The real test is in
the Videx manual's own description of the part it replaces: *"Characters
80 - BF are identical to characters C0 - FF"*. `loadCharRom` now compares
those two 512-byte blocks and publishes `Memory::charRomHasLowercase()`; the
renderer folds on that.

**"Bit 7 marks the inverse range."** True of the //e dumps and AppleWin's
`Apple2_Video.rom`. The Videx dump never sets bit 7 anywhere, so
normalisation has to split the range by offset instead. `loadCharRom` probes
for the marker once and picks the rule. This one was worth pinning because
it is *invisible*: the glyph shapes are identical either way, so a wrong
choice only shows up as the whole normal range rendering inverse — and in a
screenshot of ordinary white-on-black text, nothing looks obviously off.

Pinned by `videx_lowercase_char_rom`, which loads BOTH dumps: proving the
detection is not a constant requires the ROM that answers the other way.

Both rules moved into **`src/CharRomDump.cpp`** rather than growing
`Memory.cpp`: they are facts about FILES, not about the memory map, and the
file-size ratchet asking the question is exactly what it is for. Memory.cpp
came out 13 lines SMALLER than before the feature.

## 2026-08-31 — DIX-fix: RETURN at the menu now plays the whole anthology

`tools/make_dix_fix.py` builds **`disks_3.5/DIX-fix.po`** from the pristine
`DIX.po`: same 800 KB image, **12 bytes different**, and pressing RETURN (or
SPACE) on the fresh menu launches *ALL DEMOS IN AUTOMATIC MODE* instead of
crashing.

**The bug is DIX's, and it was already fully diagnosed** — 2026-08-08, in
`docs/test_corpus.md` § "DIX menu: RETURN before any arrow key wedges". The
menu keeps its highlighted entry in `CurrentChoice = $DFFF`, initialised to
0; entry 0 is the "USE ARROWS TO SELECT DEMO" prompt, so only an arrow key
ever gives it a valid 1..16. The launcher then indexes a **16-entry,
one-based** table: `LDX CurrentChoice / DEX` turns 0 into `$FF`, reads
`$D175`/`$D185`, and `JSR $17E1` walks off into unwritten RAM — picture
frozen, Mockingboard music still playing. POM2 reproduces it faithfully.

**The fix reuses what DIX already has.** Menu entry 16 *is* "ALL DEMOS IN
AUTOMATIC MODE" (`AUTOMODE`, `loader.a:132-148`), and jump-table slot 15 is
`$D042`. So index 0 is redirected there — the missing guard turns the dead
keystroke into the most useful thing it could do, and the "use arrows"
prompt and the first-boot intro trigger (both keyed on `CurrentChoice = 0`)
survive untouched.

**Where the nine spare bytes came from.** The loader image is packed solid:
blocks 1-7 fill `$D000-$DDFF` to the last byte — the demo-name text is cut
mid-word at `$DDFF`. But the boot block reads **8** blocks
(`boot_unidisk.a` `SP_BLOCKS2READ = 8`, not the 7 the makefile comment
claims), so block 8 lands at `$DE00-$DFFF` and its tail `$DF56-$DFFE` is 169
bytes of zeros — loaded, resident, and as safe as the loader itself, since
`CurrentChoice` at `$DFFF` already has to survive every part. The stub goes
at `$DF60`:

```
$D02C  20 60 DF  JSR PICKDEMO     ; was  AE FF DF  LDX CurrentChoice
$D02F  EA        NOP              ; was  CA        DEX

$DF60  AE FF DF  LDX CurrentChoice
$DF63  CA        DEX
$DF64  10 02     BPL +
$DF66  A2 0F     LDX #15          ; AUTOMODE
$DF68  60      + RTS
```

`JSR`/`RTS` leave X alone and `LDA DemosL,X` does not read the incoming
flags, so the selected-demo path is bit-identical to the original.

**Verified by booting both images** through `dix_return_crash_probe`, which
grew a `--disk` flag and an `ExpoMode` column for the purpose. Same machine
(//e PAL, slot-4 Mockingboard, slot-5 SmartPort), same RETURN at 20 s:
`DIX.po` → `RUNAWAY: BRK at $17E4` 0.22 s later, then 25 s of frozen video;
`DIX-fix.po` → `expo=1` within the second, part code at `$7xxx`, video
moving for 70 s and a loader re-entry at 87 s as AUTOMODE chains to the next
part. RIGHT-then-RETURN still runs demo 1 with `expo=0`, unchanged.

The patcher refuses to touch an image whose two sites do not hold the exact
original bytes, and re-checks that table slot 15 still points at an
`AUTOMODE` that still starts with `LDA #1 / STA ExpoMode`. `--verify`
re-reads a built image. DIX is GPLv3 and its sources carry the code this
patch reasons about; the modification is described byte-for-byte above.

## 2026-08-31 — 4play: four joysticks, and one bit that reads back set

`FourPlayCard`, catalog key `4play`, a port of MAME's
`src/devices/bus/a2bus/4play.cpp`. Four **digital** joysticks on an Apple II,
one byte each at `$C0nX`, pinned by `fourplay_card`.

The reason it is worth anything: the Apple II's game port is *analogue* and
carries two paddles, so two players is the ceiling and reading them means
timing an RC discharge. This card is four reads with no timing at all.

**The one trap, and it is a good one.** MAME's `4play.cpp:41-48` has every bit
active HIGH except **bit 5**, which is `IP_ACTIVE_LOW IPT_UNKNOWN` — nothing
drives it, so it reads back **set**. An untouched stick is `$20`, not `$00`.
A test that assumed zero would pass against a card that never updated at all,
which is why the layout is asserted bit by bit rather than as a round trip.

**Said plainly, because it cuts against the usual argument for porting a
card**: 4play is modern homebrew (Lukazi, 2016), not period hardware. "MAME
has it" and "there is software for it" are different claims and only the first
comes for free. Its software is the current Apple II scene — which is the
honest reason to want it, not a 1980s catalogue.

Threading follows `PaddleInputs`: the four bytes are atomics, the UI thread
writes them, the CPU worker reads them, neither takes `stateMutex`. Host pads
1-4 become players 1-4 with a 0.5 gate turning analogue sticks into
directions. Deliberately absent from the snapshot, like the game-port paddles:
a rewind must not put somebody's thumb back where it was.

## 2026-08-31 — The file-size gate goes green, and what that cost

`tools/check_file_sizes.sh` had been failing on `main` since **2026-08-29** —
in 15 s, before the Linux job compiled anything, so it was also hiding that
job's build and its GLES tier behind an X nobody could see past. Two ceilings
were raised to their exact current sizes:

* `src/ImageWriter.cpp` 2152 → 2501 (+349)
* `src/Memory.cpp` 2402 → 2442 (+40)

**This is the mechanism working, not being switched off.** The script's own
header says the point of the budget file is that growing a god-object "requires
editing the budget file in the same commit, which is exactly the moment someone
should be asked whether the new code belongs in a new translation unit". The
question was asked and answered: unblock the gate now, split later. Both
numbers are the **exact** current sizes, so the ratchet fails on the next line
either file gains, and the debt is tracked in TODO § [Arch].

Worth being precise about the second one, because it is the smaller and the
more recent: exactly **one** of `Memory.cpp`'s 40 lines is the foreign-bus
dispatch that lets a coprocessor card run the 6502 over its own map. The other
39 predate it. `ImageWriter.cpp` is the real debt — the printer head, the paper
tray, PDF export and the PostScript / screen-dump seams are four concerns in
one translation unit, with edges clean enough to cut along.

## 2026-08-31 — The handshake works, and it was one wrong number

POM2 now services AppleShare's driver call. `ATINIT` calls the card at
`$Cn14` in ProDOS-MLI style — `JSR $Cn14 / .BYTE cmd / .WORD block`, command
`$42` — and the whole two-CPU transaction completes: the command byte reaches
`$CnDB`, both rendezvous semaphores come back to rest, and the call returns
past its inline parameters. Pinned by `workstation_card_smoke`.

**The bug was the expansion-ROM base.** `$C800-$CFFF` was served from file
`0xC800`; it is `0xC400`. So the page's `JMP $CC00` landed on a block-copy
loop instead of the host-side prologue (`CLD / PHP / SEI / LDA #$50 /
STA $C080,X`), and the two CPUs deadlocked with no symptom that pointed at
the cause. Nine bases were swept and `0xC400` is the only one at which the
transaction completes — which is why the constant now carries that sweep in
its comment rather than a derivation.

**The red herring is the part worth keeping.** The card waits on
`BIT $02EE / BVC` for a bit the host's page code never writes, which looked
like proof that the `$C08x` strobe had to set it. Wiring that up **moved the
card one step further**, from the `$D021` wait into the `$02A9` rendezvous —
which made the theory look righter still, and it was wrong. With the base
corrected the transaction completes with **no strobe modelling at all**. A
change that unsticks a stuck system is not evidence that it is correct, and
three sessions were spent learning that here.

**How the two CPUs actually talk**, now fully mapped in
`WorkstationCard.h` § THE HANDSHAKE: the card **patches the host's code**. It
writes `$CnBB`/`$CnBC` — the operands of the host's own `JMP` — to steer
where the host goes, and `$CnC3`/`$C4`/`$C6`/`$C7`, the address operands of
the host's block-move loop, to say what it copies. It releases the host's
spin loops by writing `$38` (`SEC`) over the `$18` (`CLC`) the host is
executing at that address. Data goes through `$CnEA` a byte at a time with
`$02A9` as the handshake.

**Verified end to end**: `disks_3.5/AppleShare IIe Workstation.po` boots, its
ATINIT passes the card's power-up diagnostics — where it used to print *"The
workstation card failed power up diagnostics. AppleTalk has not been
initalized."* — and the workstation software reaches its menu.

## 2026-08-31 — AppleShare finds the card, and the handshake is decoded

`disks_3.5/AppleShare IIe Workstation.po` boots in POM2 with the Workstation
Card plugged, finds it, runs its power-up diagnostics against it and reports:

> The workstation card failed power up diagnostics.
> AppleTalk has not been initalized.

That message is progress, not a defeat: real AppleTalk software is now
talking to the card. Extracting `ATINIT` from the disk (type $E2, key block
50, loads at $2000) and reading it against the card's own firmware decoded
almost all of the handshake — recorded in `WorkstationCard.h` § THE HANDSHAKE
so it is never re-derived:

* **Detection** is `ATLK` at `$CnF9-$CnFC` plus a version byte at `$CnFD`.
  POM2's card passes; that is also why CardCat names it.
* **The driver entry is `$Cn14`**, called ProDOS-MLI style —
  `JSR $Cn14 / .BYTE cmd / .WORD paramblock` — and ATINIT's first call is
  command `$42`.
* **The two CPUs rendezvous by rewriting code the other is executing.** The
  host spins inside the shared page on `$Cn89: 18 90 EB` (`CLC` then a branch
  back) and the card releases it by writing `$38` — `SEC` — into `$0289`, so
  the branch stops being taken. The mirror runs at `$02A9`, where the card
  writes `$38` and waits until it reads `$18`, the host's own `CLC` opcode.
  It is a genuinely lovely piece of 1988 engineering and it is invisible
  unless you watch both CPUs at once.
* **`$02EE` is the request byte**, polled from the card's idle loop: bit 7
  means the host wants service, then `BIT $02EE / BVC` waits for **bit 6**,
  then `STZ $02EE` consumes it, `AND #$0F` is the command, and `$02DB`
  carries the `$42`.

**One bit is still missing, and it is not shipped.** The host page writes only
`$80` to `$CnEE` and strobes `$C080,X`/`$C081,X` in the same loop, so the
strobe must be what sets bit 6 — nothing else in the system can. Wiring that
as an experiment moves the card exactly one documented step further, from the
`$D021` wait into the `$02A9` rendezvous. It stays out of the card because
*which* register does it, and whether it self-clears, is not established, and
a card that invents hardware behaviour would undo the discipline the rest of
it was built with. → `TODO.md`

One process note worth keeping: the AppleShare disk did **not** boot in the
hand-rolled headless harness used for the investigation, and did boot in the
app — twice over, that harness cost more than it saved. The lesson is the one
CardCat already taught: drive the emulator the way the app drives it, and let
guest software be the oracle.

## 2026-08-31 — SDLC, and the card talks LocalTalk

### What the card publishes, and how far the handshake got

The `$Cn00` page's layout is now read off a running card rather than guessed:
entry points from `$Cn14` (each `LDY #cmd` then a branch to the common body at
`$Cn36`), the acquired LocalTalk node address at `$CnF0`, and **`ATLK` at
`$CnF9-$CnFC`**. That signature is the card's identity on the bus — it is
neither a Pascal 1.1 device nor a ProDOS block device, so software finds it by
this and not by `$Cn05`/`$Cn07`, which is exactly why the "no Pascal
signature" note two entries down meant nothing about the mapping.
`signaturePublished()` and `localTalkNode()` expose both, and
`workstation_card_smoke` pins them.

The `$C0nX` handshake is still not settled, and static analysis has now been
taken about as far as it goes. Recorded so it is not re-derived:

* The strobe sites: `$71` to `$C080,X`/`$C081,X` from the page, `$50` from the
  expansion ROM, and `$CnB8: STA $C080,X` with X from `$CnEB` and the byte
  from `$CnEA` — both host-written shared-page slots.
* The card has **no IRQ path** for it (`$EE07` counts anything that is neither
  SCC nor timer as spurious) but **does have an NMI path and arms it**:
  `$ED57` tests bit 7 of `$3A` and dispatches through `($01FE)`, and the card
  sets that bit at `$DBD6` about five seconds in. The vector still points at
  an RTS there, so **/NMI is the leading hypothesis, not a fact** — and POM2
  does not wire it, because a guess here would be worse than answering `$FF`.
* A negative result worth as much: writing `$02E7`/`$02EB` alone does **not**
  make the card rebuild its page. Tried; nothing moved.

The unblock is a **guest AppleTalk disk** — driven the way
`workstation_card_cardcat` drives CardCat, so the firmware answers instead of
being guessed at. There is not one in the repo.


The Workstation Card now puts **real LocalTalk frames on the wire**. Apple's
LAP driver, running on the card's own 65C02, acquires a node address and
broadcasts:

```
0B 0B 81            lapENQ — "is node $0B taken?"
FF 0B 84            LLAP broadcast
FF 0B 01 00 06 …    a short DDP datagram
```

Nothing in POM2 knows what LLAP is. That output is the emulated chip and the
emulated card agreeing well enough for 1988 firmware to run its protocol.

### SDLC is a documented deviation from "MAME is the oracle"

MAME does not model SDLC: `do_sccreg_wr4` logs *"SDLC - not implemented"* and
every CRC reset code in WR0 is a no-op there. LocalTalk is SDLC, so
`Scc8530Device` models it from the Zilog SCC/ESCC user manual (UM010902)
instead. Every site that does is marked **`SDLC (datasheet, not MAME)`** so
the boundary between ported and derived stays visible while reading.

What is modelled is what a byte-granular seam can carry: frame delimitation,
the Tx Underrun/EOM latch that closes a frame, Sync/Hunt and Enter Hunt,
address search against WR6 with the `$FF` broadcast, Send Abort, and End Of
Frame with its residue code as a special receive condition — so the FIFO locks
until Error Reset, exactly as the manual describes. What is not modelled is
what only exists *between* the bytes: bit stuffing, the flag patterns, the
FM0/DPLL line coding. No register can see any of it.

**One number that would have been silently wrong.** MAME's `update_serial`
always hands diserial a start bit. That is harmless in MAME, which does no
SDLC — but an SDLC byte is exactly its data bits, so carrying the start bit
over would have made every byte 9/8 too long and put a 230.4 kbit/s driver
12 % off. `frameHalfBits` special-cases it, and `scc8530_smoke` measures the
byte time to the tick.

### Two things learned by watching the firmware, not by reading

* **The card disables its receiver while it transmits** — LocalTalk is
  half-duplex — and re-enables it after. An endpoint answering a frame has to
  wait for WR3 D0 to come back, not merely for an inter-frame gap: a reply
  100 cycles after the frame closed is dropped, correctly, because the
  receiver is off. That cost an hour of "why is my ACK vanishing", so it is
  written down.
* **Answering lapENQ with lapACK does not move the card off `$0B`.** The frame
  is accepted — the FIFO fills, the interrupt fires — and the driver keeps
  enquiring anyway. Timing window, missing status bit, or just what this
  firmware does on a dead network: open. → `TODO.md`

### CardCat says so too

`workstation_card_cardcat` boots Henry Lowe's **CardCat** off
`disks_3.5/CardCat 1.94.po` on an enhanced //e and reads the slot table off
the text screen:

```
 4   Apple II Workstation Card
```

That is real guest software identifying the card by its firmware signature
bytes, which is a much harder thing to satisfy than any assertion POM2 writes
about itself. The test carries a **negative control** — the same boot with the
slot empty must not print the name — because otherwise it would only be
proving that CardCat contains that string.

### The snapshot gap is closed

`Scc8530Device` carries its own blob now (every register, both FIFOs, the
interrupt block, the three clock accumulators and the SDLC frame in flight),
and `WorkstationCard`'s snapshot appends it. A rewind no longer lands the chip
on its reset values waiting for the firmware to notice. Both refuse foreign,
short and truncated blobs rather than restoring half of one.

## 2026-08-31 — The Apple II Workstation Card boots

The card is emulated: `src/WorkstationCard.{h,cpp}`, catalog key
`workstation`, pinned by `workstation_card_smoke`. Apple's real 341-0358-A
firmware runs on it, completes the card's power-on self-test — including the
255-byte SCC loopback — and configures the chip for LocalTalk at 230400 bit/s,
all with the card plugged into a real `SlotBus` and paced by
`Memory::advanceCycles`.

### It is a computer, not a card

That is the whole shape of this change. On board: a **65C02 of its own**,
28 KB of RAM, the 8530 SCC, an interval timer, and 64 KB of ROM banked into a
32 KB window. The Apple II never executes that firmware — it reaches the card
through a shared RAM page. So POM2 runs a second `M6502` over the card's own
address space.

**There is no MAME oracle**: MAME has no Workstation Card at all. Every line
of the map was read out of the dump with a disassembler and then confirmed by
*running* the firmware. Two that show the shape of the evidence:

* **`$7A00` is five bits wide** — the card's own POST writes `$FF` into it
  with a `DEC` and compares against `#$1F`. The firmware documents its
  hardware for us.
* **`$7C00` is the ROM bank select** — proved by a trampoline the firmware
  relocates *into RAM* at `$42D1`: `LDA $40BB / STA $7C00 / JSR $CC32 /
  LDA $029A / STA $7C00`. It lives in RAM precisely because it switches the
  ROM out from under itself, and the card crashed into the stack page in
  every experiment until the banking was modelled.

And a correction to the entry below: the `$Cn00` page is **not** a ROM slice.
It is a read/write window on card RAM `$0200`. The driver settles it — its own
`$Cn00` code sets `$FB/$FC` to `$Cn00` and stores *through* it — and the
correspondence is checkable, since the driver reads `$Cn9A` and `$029A` is
where the card keeps its bank value. What the guest reads there is what the
card published at boot from ROM `$C3DA-$C4BC`.

### The second CPU cost the first one nothing

`M6502` reaches memory through `Memory`, so a coprocessor needed that core
over a foreign map — and PERFORMANCE §§ 8.2/8.5 had already priced the obvious
answers: a branch on the bus path is +13-16 %, *testing a flag* there is
+7.2 %. So nothing was added. `Memory::ForeignBus` folds into tests that were
already being made: `flatBus_` **replaces** the `testMode` test both slow paths
and `memWrite`'s fast path made — one byte load for another — and
`foreignBus_` folds into the three derived read gates the way `readDivert_`
does, leaving `memRead` untouched. Measured interleaved, best-of-5, two passes,
both workloads: −1.2 % to −2.3 %, RAM and framebuffer hashes byte-identical.
The honest reading is **no measurable cost**; the sub-2 % is layout luck, and
a flattering number is one to distrust. → PERFORMANCE § 9

One trap caught by reading rather than measuring: `Memory::advanceCycles` runs
Apple II scanline bookkeeping past `vblNextEventCycle_`, and a card's Memory
has no beam — `setForeignBus` parks that threshold at the end of time.

### `advanceCycles` interleaves, and that is correctness

The slot bus hands out ~4096 cycles at a time. Running the card CPU for all of
them before the SCC moves makes the chip stand still for four byte-times while
the firmware burns the **fixed poll budget** of its self-test, and the POST
then fails on a timeout no real card would ever see. The card interleaves at
24 cycles — below one poll iteration. This is written down because it looks
exactly like a tuning constant and is not one; widening it makes
`workstation_card_smoke` fail.

### What is still missing, and it is not a detail

* **The `$C0nX` handshake between the two CPUs.** The driver writes `$71` to
  `$C080,X` and `$C081,X` and `$50` from the expansion ROM, and the card
  firmware has **no interrupt path** for any of it — its handler counts
  anything that is neither SCC nor timer as spurious, at `$EE07` — so the
  handshake is presumably a poll of the shared page. Reads answer `$FF` and
  `hostStrobeLog()` records what the guest does. Guest AppleTalk software will
  find the card and then wait.
* **SDLC framing**, on both sides of the SCC port. MAME does not model it
  either, so there is no oracle and no LocalTalk traffic yet.
* **The SCC's register file is not in the card's snapshot.** A rewind lands
  the chip on its reset values and the firmware reprograms it; survivable,
  because nothing outside the card observes it mid-frame, but it is a gap.

## 2026-08-31 — The Z8530 SCC, and what the Workstation Card dump really is

### `Scc8530Device` — a MAME port of `z80scc`

The Zilog 8530 is now emulated: `src/Scc8530Device.{h,cpp}`, ported from MAME
`src/devices/machine/z80scc.{h,cpp}` at commit `588eeb33` (2026-08-29), pinned
by `scc8530_smoke` (11 cases). Same chip as the Macintosh and IIgs serial
ports, so it is useful well beyond the card that motivated it.

Three decisions worth keeping:

* **The variant mask is folded away.** MAME's one class covers eight parts
  behind `m_variant`; POM2 instantiates the NMOS `TYPE_SCC8530` and constant-
  folds every test of it — no Z-Bus, no ESCC extended read, 3-byte Rx FIFO and
  a one-slot Tx buffer. Each folded branch is marked at its site so widening
  the port later is un-folding, not re-deriving. The visible consequence:
  after the fold, `scc_register_read`'s NMOS remap is unconditional, so
  RR4-RR7, RR9 and RR11 are permanently images of other registers. They have
  no handlers because they are **unreachable**, not because they were skipped.
* **The wire is byte-granular, everything above it is not.** MAME shifts bits
  through `device_serial_interface`; POM2 holds a byte in the shift register
  for exactly the frame time the same programming implies — `frameHalfBits`
  reproduces `set_data_frame(1, data, parity, stop)` in half-bit units so 1.5
  stop bits lands on a boundary — and delivers it whole. Nothing on an Apple II
  can see the difference, and it is where a LocalTalk endpoint wants to attach.
* **`tick()` counts PCLK, not CPU cycles**, because every rate in the chip
  descends from PCLK and that is the only way the BRG arithmetic stays equal to
  the datasheet's `TC = clock / (2 × rate × mode) − 2`. Both internal clocks
  use an exact integer accumulator, so ticking once per 4096-cycle chunk loses
  no edges.

One MAME divergence is kept **on purpose** and pinned so a future change to it
is deliberate: on receive overrun MAME parks the offending byte in the slot the
write pointer sits on and never advances past it, so the Overrun bit never
reaches RR1 through a data read. Real silicon discards the byte and reports the
error. Reproducing MAME keeps the oracle usable.

### The 341-0358-A dump is not a slot ROM

The [entry below](#the-workstation-card-identified-not-built) recorded
that there is "no Pascal 1.1 signature and no ProDOS block dispatch trio at any
256-byte alignment, so the `$Cn00` page is not a plain slice of the image".
The first half is true; the conclusion was wrong, and the reason it was wrong
is the interesting part.

**The card has its own 65C02.** The dump is that CPU's firmware, not a slot
ROM: 65C02-only opcodes throughout, its own vector table at `0xFFFA`
(RESET `$C000` lands on a RAM-sizing probe), RAM at `$0000-$6FFF`, I/O selects
at `$7x00` — and the 8530 at `$7500-$7503`, identified by the `LDA #$03 /
STA $7502 / LDA $7502` at `$EE13`, which is an RR3 poll and therefore also
proves the `A1 = A//B, A0 = D//C` pin ordering. The 64 KiB is two 32 KiB images
for one `$8000-$FFFF` window, both live: the LocalTalk strings are in the upper
half, the LaserWriter `IWEm` procset in the lower.

The Apple II side of the firmware **is** a plain slice, at `0xC400-0xC4FF` for
the `$Cn00` page and `0xC800-0xCFFF` for the `$C800` expansion ROM — a textbook
slot page, complete with the `JSR $00FB` / `TSX` slot-discovery trick,
`STA $07F8`, `LDX $CFFF` and `STA $C080,X` with X = slot × 16. There is no
Pascal or ProDOS signature because an AppleTalk card is neither of those kinds
of device; software finds it by other means (note the `ATLK` string at
`0xC518`). Absence of a signature was evidence about the *device class*, not
about the mapping.

So the blocker moved rather than lifting: the card needs its 65C02 run over a
foreign bus, and `M6502` is bound to `Memory` through non-virtual inlined
`memRead`/`memWrite` on the hot path. That is a decision to take on its own
terms, with `docs/PERFORMANCE.md` open — not a side effect of building a card.

### The firmware runs, and it signs off on the SCC

The blocker above is about shipping a *card*. It is not a reason to leave the
evidence unread, so the firmware was driven anyway: POM2's `M6502` over a flat
test bus, `Scc8530Device` at `$7500`, the other `$7x00` selects modelled as
latches. **It completes its power-on self-test**, and then programs the chip
for LocalTalk — WR4 = `$20` (SDLC, x1 clock), WR3 = `$DD` (receiver on, 8
bits, address search, hunt), WR11 = `$F2` (receive clock from the DPLL),
230400 bit/s. Shipped as `scc8530_workstation_firmware`, ROM-gated.

That POST is a far harder test than any unit test written from a datasheet: it
is a **255-byte loopback ping-pong on both channels inside a fixed 8000-poll
budget**, so it fails unless the transmit timing, the TBE/RxCA status bits and
the one-slot transmit buffer all agree. Two hardware facts came out of running
it that reading the dump could not give:

* **The crystal.** The firmware ends with WR12/WR13 = 6 and WR4's x1 clock,
  and `3686400 / (6 + 2) / 2` is exactly 230400 — the LocalTalk rate. /RTxC
  and PCLK are a 3.6864 MHz crystal, derived rather than assumed.
* **A ceiling on the CPU speed.** The ping-pong's fixed budget closes at
  every rate tried up to 2.0 MHz against that 3.6864 MHz SCC clock and fails
  from 2.05 MHz up, because a faster CPU burns its 8000 polls before the 255th
  byte lands. That is an upper bound on the card CPU, not a measurement of it
  — worth stating precisely, because the test is silent about how much slower
  it might be.

A third fact is negative and worth as much: over 2.5 M instructions the
firmware never writes above `$8000`, which is what a correct RAM/ROM split
predicts and the test asserts.

The harness is deliberately **not** a card emulation — it borrows `Memory` in
flat test mode and shims the I/O page by decoding effective addresses around
each step. It exists so the SCC could be validated now instead of after the
`M6502` bus decision, and its header says so, so nobody mistakes it for the
beginning of a card.
→ `docs/printer_plan_2.md` § 5.2, `TODO.md`

## 2026-08-31 — Nine bugs, five more printers, and the LaserWriter

### The bug hunt, and the three it could not finish

A parallel agent sweep found 27 candidates; adversarial verification killed
18 and confirmed 9, all fixed. The single memory-safety bug is the one worth
remembering: the //c+ MIG page cursor restored from a snapshot was masked with
`& 0x7FF`, which *looks* right for a `0x800` array and is not. The index only
stays inside because the live cursor is **32-byte aligned** — it starts at 0
and only ever advances by `0x20` — so the highest legal page is `0x7E0`, and a
crafted blob restoring `0x7FF` indexed up to `0x81E`, over `migPage_`,
`migIntDrive_`, `migHdSel_` and the raw `iwm_` / `hub_` pointers. A range mask
is not an alignment invariant.

The rest were the same shape as each other: **file I/O under `stateMutex`** —
the CLI's snapshot save/load, `loadTape`/`saveTape` decoding whole audio files,
the media panel's Mount button, `mountHdv` discarding its unlocked phase-1 read
and re-reading 32 MiB under the lock, the FujiNet helper's 2 s stop. Plus two
that were simply never right: the Super Serial listener could not be started
from the UI at all (the transport was injected only when the saved state was
already listening, so the button reported "bind failed" for a socket that was
never created), and `swapSlotCardVariant` wrote `slot_N_card` without the
`slotKeyIsUserChoice` guard, so clicking it on a //c clobbered the user's //e
slot map.

Three follow-ups the agents deliberately left out of scope, because the fix
crossed a file they were not allowed to touch, and each turned out to matter:

* **`SpOverSlipLink::stop()` destroyed the transport with no `callMtx_` held**
  while `transact()` cached a raw `SpTransport*` sampled *before* taking that
  mutex. Only `stateMutex` kept the two threads apart — and moving the FujiNet
  panel's stop off `stateMutex`, which is exactly what the fix above did,
  removed that accident. Fixing one bug armed another. The teardown takes
  `callMtx_` now, **after** the join and never before: the worker reaches
  `transact()` through `enumerateDevices()`, so holding it across the join
  parks the two threads on each other.
* **The Eject button could not be fixed by routing it at the coordinator**,
  which is what the agent proposed: `ejectMediaBay` ran save-on-eject — a whole
  file read-modify-write plus rename — inside its own `lockState()`. So
  `Block512Backing` grew `takeWriteBack`/`commitWriteBack` (`saveDirty` is
  composed from them, so there is one copy of the write logic) and the eject
  became three critical sections. **Phase 1 leaves the medium mounted on
  purpose**: a commit can fail on a full disk, and the one-phase code kept it
  so the user could retry.
* **`iwm_mig_snapshot` pinned nothing.** `testMigPageMasked` built a hostile
  blob and fed it to a plain `Memory` with no //c profile to consume it, so the
  sanitiser was never reached. It now drives `IIcClassProfile` directly and
  sweeps all 65536 page values. Confirmed failing against the pre-fix mask
  before being kept — the P0 lesson, again: write the test that makes it FAIL.

### Five more printers, from one table column

The Grappler+ models a seven-position printer-type DIP; POM2 had heads for two
of those positions. Added: the **C. Itoh Prowriter 8510A** and **NEC PC-8023A**
(the mechanism Apple rebadged as the DMP, and its NEC badge — which is why the
Grappler groups them on one position), and the **Epson MX-80**, **MX-80
Graftrax+** and **RX-80**.

ESC/P grew feature by feature across MX → RX → FX, so that is a capability
mask, not new code paths: `IwModelProfile` gained `escPFeatures`. A command the
fitted head lacks is treated exactly like an unknown ESC — dropped with its
ESC, following bytes printing as text — because that is what the firmware did,
and POM2 already reproduces wrong-DIP garbage on purpose.

**A capability mask is not free once a gated command has a BODY.** Dropping
`ESC *` while its data bytes still streamed would print a screenful of them, so
`BitGraph` gained a `swallow` flag — a density of 0 could not express it,
because `dotW = 1/horizDens` goes infinite and `fillDots` clamps that to a
filled row. And the screen dump had to learn which graphics command the fitted
head can answer, since it emitted `ESC *` for any Epson and an MX-80 prints
that as text.

### The LaserWriter, both halves

**Diablo 630 first**, because that is how an Apple II actually got text out of
one without PostScript: the back-panel switch offered daisywheel emulation, and
most word processors had a 630 driver. Planned as its own `LaserWriter.h/.cpp`
with a 300 dpi canvas and **revised on contact with the code** — a 630 is
fixed-pitch text with no graphics, `ImageWriter`'s mechanism was already right,
and `hmi_` was *already* documented as "the guest saying move exactly this far,
which outranks the font", which is precisely a Diablo HMI. So it is a third
parser over the same mechanism, and `escP` became an `IwLineage` enum: two
lineages fit in a bool, three do not.

Deliberately conservative on the grammar. Guessing a parameter COUNT wrong does
not lose one command, it desynchronises the whole job — and the LaserWriter's
emulation was a subset of the real 630 anyway, so a conservative subset is
*closer* to the machine than an eager one.

**PostScript second, by delegation.** It is not a command set: it is a
Turing-complete stack language with Bezier flattening, winding-rule fills,
clipping, halftones and encrypted hinted Type 1 fonts. An interpreter would be
bigger than the rest of the printer subsystem and the failure mode of an
almost-right one is a page that is *subtly* wrong — worse than no page. So
`ChildProcess` supervises Ghostscript, exactly as it supervises the FujiNet
helper.

Four things worth keeping:

* **Ghostscript is AGPL and POM2 is GPLv3, and a separate PROCESS is not
  linking.** Optional runtime dependency, nothing shipped, detected at runtime.
  `PostScriptRender.h` says not to make it a library binding without revisiting
  that.
* **`-dSAFER` is not optional** — the job comes from emulated software, and
  PostScript can open and delete host files.
* **PGM, not PNG.** A five-token header and raw bytes is thirty lines; PNG
  would mean carrying a *decoder* to read back what POM2 only ever writes.
* **The page model was always an intensity ramp** and merely had no source of
  greys, so anti-aliased PostScript text keeps its edges through
  `adoptRenderedPage` rather than being thresholded to one bit.

<a id="the-workstation-card-identified-not-built"></a>

### The Workstation Card: identified, not built

A 64 KiB dump was identified as the Apple II Workstation Card firmware — from
its CONTENTS rather than a part number, which is the stronger evidence: the
`STA $C080,X` device-select idiom, a `THOMAS EAGER WROTE THE LAP DRIVERS`
credit (LocalTalk Link Access Protocol), an `Apple //e Boot` netboot string,
and a `%%IncludeProcSet IWEm 1 1` fragment — a LaserWriter print path asking
for the ImageWriter Emulator procset, which ties this card straight to the
printer work either side of it.

It is **not implemented**, and the reason is not scope. The dump lifts one of
two blockers; the Zilog 8530 SCC is untouched, and POM2's convention wants a
MAME port citing file and line rather than a datasheet reconstruction. Also
recorded, because it is the first thing the implementation must solve: there is
**no Pascal 1.1 signature and no ProDOS block dispatch trio at any 256-byte
alignment**, so the `$Cn00` page is not a plain slice of the image and the
banking scheme is still unknown. Identifying a dump is not knowing how the card
maps it. → `TODO.md`

## 2026-08-28 — TnfsClient gets a caller, and ProDOS gets its marker back

Two things, and the second is a bug the first went looking for.

### `POM2 tnfs://host/path/image.po`

`TnfsClient` was 716 lines of tested code that nothing called — TODO P0-5's
"wire it or delete it". It is wired.

`TnfsMedia` fetches an image from a TNFS server (`tnfs.fujinet.online` carries
a large Apple II library) into a local cache, and the positional argument then
behaves like any other disk: `classifyDiskForSlot` picks the drive, the
two-phase mount moves it in off the state mutex, and writes go to the local
copy — the only honest place for them, since TNFS is served read-only here.
Nothing downstream learns a new concept.

Three decisions worth recording:

* **A file, not a buffer.** Handing bytes straight to a card would mean a
  second mount path beside the one every other image uses, and a write-back
  story with nowhere to write.
* **The cache hit touches no socket.** Existence is the whole test, because
  every fetch lands through `writeFileAtomic` — a file that is there is a
  *complete* previous fetch, and a half-written one cannot survive to be
  mounted as a truncated disk. That is what makes it work offline: a boot disk
  fetched yesterday still boots on a train today. The trade is staleness, and
  TNFS offers no ETag to do better without re-reading the whole file.
* **The `tnfs://` scheme is required for the positional.** The parser accepts a
  bare `host/path` too — convenient in a text field — but `disks/foo.po` is a
  relative filename, and the positional argument may not guess between them.

It needed a socket-less path before it could enter the build at all: the file
referenced `disableSigpipe`, `socklen_c` and `closeHostSocketValue` unguarded,
which is how a translation unit in no `SOURCES` list drifts out of compiling
for one of POM2's two targets with nobody noticing.

### The ProDOS subdirectory marker was in the wrong place, twice

ProDOS refuses to traverse a subdirectory whose header lacks a marker byte —
an I/O ERROR on a volume a permissive host-side decoder parses happily, which
is why POM2's own round-trip test never saw it.

The marker goes at **entry offset `$10`**. The ProDOS 8 TRM calls it byte
`$14`, and that is the whole of a confusion which produced two wrong versions
of one line: a directory block opens with a 4-byte prev/next pair, so the
header entry starts at *block* offset 4 and the TRM's `$14` is this entry's
`$10`. POM2 wrote `dst[0x14] = 0x75` for as long as the synthesiser has
existed, and a first attempt at a fix replaced it with an eight-byte "magic
pattern" at `$14` — no better.

Settled by measurement rather than recall, against the ProDOS images in the
tree:

| | |
|---|---|
| subdirectory headers sampled | 84 |
| carrying `$75` at entry `$10` | 61 |
| carrying `$76` | 23 |
| carrying anything at entry `$14` | **0** |
| volume headers with a marker at `$10` | **0 of 10** |

So: one byte, `$75`, at entry `$10`, with the reserved bytes after it left
zero — and the volume header keeps none, because real ones never have one. The
seven bytes after the marker are zero in all but a handful of real
subdirectories, where they hold a stale copy of the neighbouring
access/entry_length/entries_per_block trio; that is where the folklore
"pattern" came from.

`prodos_volume_smoke` asserted the old wrong offset, so it now pins both:
`$75` at `$10`, and `$00` at `$14`.

The TNFS stub server learned that a path can be missing, too. It answered OPEN
and STAT for anything, which made "a missing file must fail the fetch rather
than produce an empty disk" untestable.

217/217 ctest, zero warnings.

## 2026-08-28 — A coverage number, because reading got it wrong three times

TODO P2-1, and the reason it was worth doing is the three items above it.

The architecture plan asserted that three subsystems had no tests:
`TnfsClient`, `FujiNetNetDevice`, `FloppyEmuDevice`. **All three had test
suites.** Holes were being found by reading the tree, and reading got it wrong
three times out of three — twice in a direction that would have had somebody
write tests that already existed, once (`TnfsClient`) in a direction that
nearly justified deleting 716 lines of tested code.

`tools/coverage.sh` measures instead. Clang **source-based** coverage rather
than gcov: it counts regions, so a half-taken `a && b` and an untaken `else`
show as uncovered instead of reading as covered.

**First measurement: 78.90 %**, and on its first run it named five
first-party files at **0 %** — `CharRomCatalog.cpp`, `RomLoader.cpp`,
`SlirpNetworkBackend.cpp`, `SpSerialTransport.cpp`,
`SuperSerialTcpTransport.cpp`. The first two are the path every boot takes.
That list is now the backlog, in place of guessing.

Three decisions worth recording, because each has an alternative that looks
reasonable and is worse:

* **The denominator is the code the tests LINK, not the whole program.**
  `POM2` links the ImGui frontend, which no headless test can exercise;
  including it would put ~15 000 unreachable lines in the denominator, report
  ~27 %, and make the floor a measure of how much UI exists. The number
  answers "of the code POM2's tests are built against, how much do they
  actually run?" — the question a ratchet can act on.
* **The floor is recorded half a point below the measurement.** Two runs of
  the same tree differ by ~0.1 % (tests that fork, a timing-shaped case taking
  a different branch). A floor pinned to the exact reading fails on noise, and
  a ratchet that fails on noise is one somebody switches off. Half a point
  absorbs the jitter; adding one untested file moves the number by far more.
* **`pom2_core_sdk_consumer` is excluded, and not because it is slow.** It
  configures a separate CMake project against the installed `POM2::core` and
  links it with plain flags — against an instrumented archive, which needs
  `-fprofile-instr-generate` at link time. Teaching the exported package about
  it would bake a build-mode flag into what consumers install. It measures the
  install/export contract, not POM2's code.

The floor may go UP freely and may not go down — the same ratchet shape as
`tools/check_file_sizes.sh`, for the same reason that script's header records.
Mutation-checked: it passes at 78.40 and fails at 85.00.

`FloppyEmuDevice` (P2-4) needed nothing: its smoke test already covers the
mode round-trip, per-mode format filtering, SD navigation bounded to the root
including the symlink escape, and `favdisks.txt` parsing.

## 2026-08-28 — Zero warnings, and a leg that keeps it that way

TODO P2-3, plus the gaps in P2-2. Two of the fourteen warnings cleared were
not style.

**A value-returning lambda that fell off its end.** The Super Serial panel's
`renderOne` returns a `SerialCommand` — the panel's start/stop, port, raw-mode
and printer-tap actions — and never returned it. Falling off the end of a
value-returning function is undefined behaviour; this worked because NRVO
happened to construct `cmd` in the caller's return slot, so the object the
caller read was the one the lambda had been filling. It has been that way
since the panel got its snapshot/command boundary. Nothing detected it,
because there was nothing to detect it *with*.

**A constructor initialiser list in a different order from its declarations.**
Members are constructed in declaration order whatever the list says, and a
list that disagrees is how an initialiser comes to read a member that has not
been built yet. The panel `unique_ptr`s were written after the coordinators
and constructed before them.

The rest were noise, and the noise is why the two above went unread: nine
`-Wmissing-field-initializers` from `RT{ title }`-style aggregate init, an
unused lambda, an unused `this` capture, and a shadowed local. The nine are
fixed at the source — `PanelRegistry::Runtime`'s three `std::function` members
now carry `= {}` like its two bools, so the idiom the call sites use is not a
warning any more.

`-DPOM2_WERROR=ON` promotes warnings to errors and is **on for the macOS CI
job**. That leg rather than Linux because the local development compiler is
the same AppleClang family, so a contributor sees the failure before pushing
rather than after. Extending it to GCC is real work — GCC's warning set is not
clang's — and is recorded as its own item rather than turned on blind.

### The `N:` device's error bytes are a contract

`FujiNetNetDevice` was in the plan as "the only untested network input
parsing". It was not untested — `fujinet_net_device_test.cpp` already covered
the happy path, the header split, the STATUS cap, a stalled server and a
blackholed host. That is the second plan item whose premise was wrong the same
way (`TnfsClient` was the first), and it is the argument for P2-1: a coverage
number would have said which lines were uncovered instead of guessing at whole
files.

What was actually missing, now added and mutation-checked:

* a reply **over the 512 KB cap** — refused outright, not truncated, for the
  same reason as a stalled server: half a document the guest cannot tell from
  a whole one is the failure nobody can diagnose from the Apple II side;
* a reply with **no CRLFCRLF** — passed through whole, pinned so it is not
  rediscovered as a bug. The split looks for what HTTP specifies and what the
  FujiNet firmware's own `N:` looks for; guessing at a boundary would silently
  eat part of a document;
* the **port-number and empty-host refusals**, which are the parser cases a
  string compare does not cover;
* a **refused connection** told apart from a **name that will not resolve**.
  The two error bytes are a contract with the guest: FILE NOT FOUND is how the
  firmware's table spells "no such host", and a guest that cannot tell a typo'd
  hostname from a dead server has nothing to show the user;
* `close()` putting a fetched body out of reach — `available()` and `read()`
  are public and do not consult `open_`, so that is the only thing between a
  closed device and a stale page.

217/217 ctest, zero warnings, `-Werror` build verified clean and
mutation-checked (an unused variable fails it).

## 2026-08-28 — MainWindow.cpp: 8316 lines to 1680

TODO P1-3 / P1-4, and the other half of the architecture assessment's finding.

The composition root had grown from 5590 to 11511 lines **with a rule against
it written down**, which is the whole reason this entry ends with two
mechanisms rather than a promise. It is now ~1680 lines and holds only what a
composition root should: construction, destruction, the DockSpace and its
layout presets, and the frame loop.

Everything else moved to a sibling named for what it owns. Nothing was
rewritten — the panel registry (2026-08-2x) had already made every panel body
a thing nothing outside it refers to, so this was a move:

| new TU | owns |
|---|---|
| `MainWindow_SlotConfig.cpp` | `plugSlotsFromSettings` — the peripheral composition root, including the runtime seams a DEVICES card may not build for itself |
| `MainWindow_Chrome.cpp` | menu bar, status bar, command palette, `runCommand` |
| `MainWindow_Screen.cpp` | framebuffer upload, `drawScreenImage`, screenshots |
| `MainWindow_Input.cpp` | keyboard, paste, pointer grab, mouse, joystick, file drop |
| `MainWindow_Kiosk.cpp` | the whole of kiosk mode, including the two window methods that were in `MainWindow_Slots.cpp` |
| `MainWindow_Media.cpp` | mount / eject / boot policy and the SlotBus queries |
| `MainWindow_StoragePanels.cpp` | every storage window and file dialog |

**The split worth arguing about is Media vs StoragePanels.** One decides what
happens to a disk image — which slot it belongs in, whether a card has to be
auto-plugged to boot it, what an eject commits. The other draws. The panels
call in; nothing in `MainWindow_Media.cpp` calls back out to ImGui. Keeping
those apart is what makes the two-phase mount rule (`MediaMount.h`: read and
decode unlocked, take `stateMutex` only to swap the object in) reviewable in
one file instead of interleaved with 1600 lines of widget code.

Three file-scope helpers moved with the code that used them and nothing else:
the on-screen kiosk key grid, the `@PRODOS_HOST_FOLDER@:` library sentinel,
and `freePoNameFor`. The `stb_image` implementation macro moved too — it is
there for the About photo and the //e keyboard photo, both of which now live
in `MainWindow_MiscPanels.cpp`, and `STB_IMAGE_STATIC` means the symbols have
to be in the same TU as the callers.

`MainWindow.cpp` was left carrying 49 includes for code that had moved away.
They are gone, and the handful the file still uses directly are now named
**even though they also arrive transitively through `MainWindow.h`** —
libstdc++ is stricter about transitive includes than libc++, so relying on
them is a GCC-only break waiting for a header cleanup elsewhere.

**Two mechanisms, deliberately different.**

* `tools/check_file_sizes.sh` stays the general ratchet. `MainWindow.cpp`
  **left the budget file entirely**: at ~1680 it is under the 2000-line watch
  threshold, as is every sibling. The two exceptions the budget's header used
  to record — the GPL-notice bump and the host-seam injections — went with it.
* `pom2_enforce_mainwindow_line_limit()` is a **hard cap at configure time**:
  any `src/MainWindow*.cpp` over 2000 lines fails `cmake`. The function had
  been written and never called. It is family-wide on purpose — the failure
  mode was never "MainWindow.cpp grows", it was "the file that grows is
  whichever one is convenient". Mutation-checked at 1600: cmake fails and
  names the file and its length.

217/217 ctest, layer guard clean (every new TU is in the FRONTEND manifest,
which the completeness check makes non-optional), lock scanner clean at 85
coordinator call sites.

## 2026-08-28 — Slot ROMs stop counting bytes

The cure the fence was standing in for. TODO P1-1 / P1-2.

Six cards have no ROM dump and synthesise their `$Cn00` page. Until now all six
wrote it as a byte list with every **address in it computed by hand**:

```cpp
0xF0, 0x37,              // BEQ write   (+55 -> $Cn91)
0x4C, 0xC0, kSlotRomHi,  // JMP $CnC0
rom[0xFF] = 0x50;        // ProDOS driver entry offset
```

Three ways to be wrong, and all three had fired. A region outgrew its budget
and overwrote its neighbour — SmartPortCard's write routine ate its own ProDOS
STATUS, which then answered `$27` on a healthy bay for weeks. A region *shrank*
and left a displacement pointing past the routine it named, changing no byte a
hexdump comparison would flag. Or a displacement was simply mistyped: `BEQ +55`
carried a comment recording that somebody had already re-counted it once.

`SlotRomAsm.h` removes the cause instead of guarding the symptom. **An address
is never typed. It is a label, and the assembler computes the byte.**

| you write | it emits | it replaces |
|---|---|---|
| `branch(0xF0, "read")` | opcode + displacement | `0xF0, 0x37` |
| `jmp("status")` | opcode + lo + `$Cn` | `0x4C, kStatusOff, kSlotRomHi` |
| `byteOf("driver")` | the label's page offset | `rom[0xFF] = kDriverOff;` |
| `region("write", 0xA2, 0xE0)` | nothing — a bounded span, and a label at its start | a comment claiming where the routine lives |

`finish()` fails on a region over budget, a branch outside the ±128 window, an
undefined or duplicated label, a poke outside the open region — and on **two
regions claiming the same bytes**, which is the SmartPort bug in its purest
form and which the bounded builder could not see at all: it checked each region
against its own limit and never against the others. The message names the
region and its span (`region 'read' ($06F..$0A0) ran out of room`), because a
layout error that says only "false" sends the reader back to counting bytes.

**Every one of the six was verified byte-identical** to what it produced
before — all three slots, dumped and diffed at each step — so the rewrite
changes what the code *says*, not what the machine runs. Two things fell out
along the way that the diff proved were already right and are now derived
rather than repeated: the HDV's `$Cn01 = $20` ProDOS signature byte, which is
the low byte of its own `JMP boot`, and the SSC's four Pascal entry bytes at
`$Cn0D-$Cn10`, which are the addresses of four routines defined further down
the same function.

**`POM2_DUMP_SLOT_ROM=1`** makes every card print its page as it is built —
one file per build, diffable against the last:

```
write  $08D..$0C0  43 of 51 bytes
```

The occupancy column is the point: that is how you see a routine approaching
its budget *before* it crosses. It used to read `47 of 47`.

Two corrections to the plan, recorded because they are the kind of thing that
gets re-proposed. It is **not** `constexpr`: a slot page is parameterised by
the slot it is plugged into — every absolute reference carries `$Cn` — and the
slot comes from settings at runtime, so there is no constant to fold. And there
were **six** hand-written ROMs, not seven: `ClockCard` writes nine fixed bytes
with no cursor, and `DiskIICard`'s boot PROM is a verbatim dump.

`SlotRom.h` and its test are deleted with the last conversion. `slot_rom_asm`
replaces them and tests the assembler itself, because a card test can only
assert its flag is clear — which a `finish()` of `return true` would also
satisfy. Mutation-checked on real cards: shrinking SmartPort's read region
names it in the log and fails `smartport_rom_layout`; misspelling a label in
FujiNetCard fails `fujinet_card_smoke`.

217/217 ctest.

## 2026-08-28 — Three guards that reported success while doing nothing

The P0 pass of the 2026-08-28 architecture plan. Its four items were "bound the
remaining hand-assembled ROMs, fix the ratchet for bash 3.2, close the layer
hole". Doing them turned up three separate mechanisms that had been **passing
without checking anything**, which is worth more than the items themselves.

### The version header was never generated

`configure_file` takes an input and an output. `CMakeLists.txt` gave it three
paths — the generated *directory*, then `include/Version.h` — so CMake wrote
the header to a file literally named `generated` and dropped the third argument
with an author warning nobody reads. Two consequences, and the second is why it
survived a week:

* **No clean checkout could build.** `generated` is a file, so `-I` it resolves
  nothing and `MainWindow.cpp:27` fails with `'Version.h' file not found`. The
  macOS, Windows and WASM CI jobs had all been dying there.
* **An existing build tree kept working**, on a `build/generated/Version.h`
  left behind by the previous arrangement — which said **0.8.3**. So the
  banner, the window title and the About box had been reporting a
  two-release-old version, read out of a header the build no longer generates,
  from the very mechanism introduced to give the version one source of truth.

### The file-size ratchet had never run where the code is written

`declare -A` is bash 4. macOS ships bash 3.2 and `#!/usr/bin/env bash` finds it
there, so the script aborted on `declare: -A: invalid option` — and, because
the abort happened inside a `while read` with no `set -e`, it aborted with
**exit status 0**. It printed two error lines and passed. The only machine it
ever really ran on was CI's Ubuntu.

Rewritten without associative arrays (the budget file is a few dozen lines and
the tree a few hundred files; a `awk` scan per file is not worth a data
structure), plus a guard that a budget parsing to *zero* ceilings is a broken
ratchet — exit 2 — rather than an empty one. Mutation-checked in all four
directions: growth → 1, stale entry → 1, empty budget → 2, clean → 0.

Once it ran, it immediately failed: `src/MainWindow.cpp` was 8319 lines against
a ceiling of 8300, and CI's Linux job had been red on it for a day. Three stray
blank lines left in a comment by the layer-guard commit come out; the remaining
+16 is real code — the host-seam injections the layer guard required — and is
recorded in `tools/file_size_budget.txt` with the reason and a pointer at
TODO P1-3, which is the answer to the question the rule exists to ask.

### The layer guard ignored 14 translation units, two of them listed

The guard only ever examined files it had been *told* about and silently
skipped the rest, so twelve TUs — `MediaMount.cpp` and the four `MainWindow`
panel TUs among them — were outside the model entirely. An unclassified `.cpp`
is now a configure error, which is the part that keeps coverage at 100 %: a new
file cannot be added without someone deciding which layer it is in.

That check then found the second hole, which nobody would have found by
reading: **`POM2_FOUNDATION_SOURCES` was never read.** The FOUNDATION branch
set the source variable to the empty string, so `ChildProcess.cpp` and
`SerialPort.cpp` sat in a manifest, under a comment explaining why they belong
to foundation, and were never examined.

Reading them surfaced a third thing. The host-API ban — no `<thread>`, no
socket headers below RUNTIME — applied to FOUNDATION, which is where the
platform *wrappers* live. `<poll.h>` in `SerialPort.cpp` is where the polling
is supposed to be. The two platform primitives are exempted **by name**, not
the layer, so a `<thread>` added to a foundation *contract* still fails.

`MediaMount` moved MEDIA → RUNTIME. The header is pure contract — one
`<string>` and forward declarations — but the implementation reaches
`DiskIICard` and `EmulationController`. Nothing below the frontend includes it,
which is what made the move free, and it is the file the plan named as its
proof: `#include "MainWindow.h"` there used to pass configure silently and now
does not.

### Every hand-assembled slot ROM is bounded

`SlotRom.h`'s `SlotRomBuilder` replaces the five remaining copies of
`rom[pc++]`. It catches **two** failure modes rather than the one the SmartPort
fix guarded:

* **overflow** — a region grew into its neighbour (the SmartPort bug);
* **misaligned** — a region no longer ends where the layout says. These ROMs
  are full of hand-computed branch displacements (`BEQ +55` to reach the next
  routine), so a routine that *shrinks* breaks them exactly as thoroughly as
  one that grows — silently, and without changing a byte a hexdump comparison
  would notice.

Converted: `FujiNetCard`, `ProDOSHardDiskCard`, `PrinterCard`,
`SuperSerialCard`, and `GrapplerCard`'s fallback stub. `ClockCard` writes nine
fixed bytes with no cursor and `DiskIICard`'s boot PROM is a verbatim dump —
neither can trip it.

The audit's finding: **`ProDOSHardDiskCard`'s write-block routine was one byte
from the same bug.** It ends at `$CnBF` and STATUS starts at `$CnC0` — zero
slack — and *nothing executed it*: every HDV test drove the backing store
directly, so an overrun there would have gone unnoticed the same way. That test
now writes a block and reads it back through the ROM, and checks the
neighbouring block is undisturbed.

`slot_rom_builder` unit-tests the guard itself, because each card's test can
only assert the flag is **clear** — which a guard hard-wired to `return false`
would also satisfy. Something has to assert it can be set.

### And then the HDV write routine, which had no room to be fixed in

The other half of the same finding: an empty HDV bay answered `$2B` "write
protected". WRITE tested the write-protect bit and never asked whether media
was there at all — a code a ProDOS caller may reasonably act on by telling the
user to unlock a disk that is not in the machine. READ already answered `$28`
"no device connected"; only WRITE did not.

It could not simply be fixed, which is the point. The routine ran
`$Cn91-$CnBF` against a STATUS routine starting at `$CnC0`: **zero bytes
free**, so adding a probe would have repeated the SmartPort bug exactly. The
fix had to make the routine *shorter*:

* one `BIT $C0n3` answers both questions — the status byte puts "no media" at
  bit 7 and "write protected" at bit 6 precisely so `N` and `V` come back
  loaded, the same trick `SmartPortCard` uses;
* both transfer routines **branch to a shared error tail** in the gap the boot
  routine leaves at `$Cn45-$Cn4C`, instead of each carrying its own
  `LDA #err / SEC / RTS`. Unlike SmartPort — whose identical first attempt had
  to be undone — this ROM has no authentic dump overlaid on it, so the page
  gap really is free.

Read drops 43 → 39 bytes, write 47 → 43, and the write routine now has eight
bytes of margin instead of none. The dispatch's three branch displacements were
hand-computed literals (one carried a comment recording a previous re-count);
they come from the layout constants now, so moving a routine cannot leave the
dispatch pointing where it used to be.

Both halves are pinned, and mutation-checked separately: dropping the media
probe fails the empty-bay case, dropping the write-protect probe fails the
locked-volume case. Testing media first must not quietly stop the card
reporting write-protect at all — the same pair `smartport_rom_layout` checks.

217/217 ctest, layer guard clean, ratchet clean, clean-checkout configure
verified in an empty build tree.

## 2026-08-28 — The SmartPort slot ROM was overwriting itself

`SmartPortCard` hand-assembles a 256-byte slot ROM, and `emit()` did
`rom[pc++]` on a `uint8_t` with no bound. A routine that outgrew its budget did
not fail — it silently ate its neighbour, and one had.

The write-block routine ran from `$Cn9C` to `$CnD5`, straight **through** the
ProDOS STATUS pre-flight that was supposed to live at `$CnC0`. The dispatch's
`JMP $CnC0` therefore landed mid-instruction inside the write loop:

    C0: D3 C0 C8 D0 F8 C6 45 AD D4 C0 29 01 D0 04 ...

which executed an illegal opcode, fell into the write routine's I/O-error
branch and returned `$27`. **Every ProDOS STATUS call answered "I/O error" on a
healthy bay**, and no block count ever came back, so a volume scanner (BITSY,
ONLINE) could not size the device. The only surviving fragment of STATUS was
its tail at `$CnD6`. The routine this file records as landed on 2026-07-12 had
been dead code ever since the write path grew. Nothing failed, because nothing
executed it.

Two empty-bay error codes were wrong for the same underlying reason — neither
transfer path asked "is there a disk here?" first:

| call  | answered | why it was wrong |
|---|---|---|
| READ  | `$27` I/O error       | fell through to the transfer |
| WRITE | `$2B` write protected | tested WP before media |

An empty bay is neither. Both now call a shared pre-flight and answer `$28`,
"no device connected" — the ProDOS driver error set the dispatch already
speaks. Write-protect still reports `$2B`, and the test pins that half too:
testing media first must not quietly stop the card reporting WP at all.

**The placement was wrong the first time, and the second attempt is the
interesting one.** The obvious homes for the new routines were the slot page's
free gaps at `$Cn13-$Cn1F` and `$CnE3-$CnFF`. Both are in the set the
real-Liron overlay *deliberately* leaves as the dump's own identity bytes, so
with `roms/liron.rom` loaded — the production path — those calls would have
executed real Liron firmware. The tests passed because nothing drove the ProDOS
driver with the dump loaded.

The real problem was arithmetic: boot (36) + dispatch (31) + read (51) + write
(62) + halt (3) + STATUS (25) is 208 bytes for the 195 that `$Cn20-$CnE2`
holds. The slot page had been over budget for a while, and squeezing routines
into leftover gaps was treating the symptom. Both routines now live in the
**`$C800` bank**, which has 1.5 KB free and is already where the SmartPort
handler sits (`$CE00`), reached exactly this way: executing the dispatch in the
slot page is itself what points `$C800` at this slot
(`SlotBus::slotRomRead` sets the active expansion slot).

`emit()` now takes a limit and trips `romLayoutOverflowed()` rather than
writing past it. `smartport_rom_layout` pins both halves — the overflow flag
*and* the behaviour of all three ProDOS entry points, since a relocation that
fits but points somewhere wrong still has to fail. It runs its whole body twice,
synthetic base and real dump, with the two passes asserted to differ
(`$Cn07` = `$01` vs `$00`) so the second one is not vacuous. Mutation-checked
three ways: STATUS back at `$CnC0` fails it, a shrunk region budget trips the
overflow flag, and STATUS parked in the `$CnE3` gap fails *only* the real-dump
pass — the bug the single-pass version could not see.

Closed as correct rather than changed: **extended `$4x` SmartPort calls return
`$01`**. The card never advertises extended SmartPort (`$Cn07` = `$01`, a plain
ProDOS block device, deliberately — see the //c boot note in `buildRom`), and
`$01` BADCMD is what a non-extended controller answers. Already pinned by
`liron_smartport_dispatch`.

## 2026-08-27 (latest) — FujiNetCard becomes a card again, and POM2 becomes a dependency

**`FujiNetCard` was the one card classified RUNTIME**, and the reason was
ownership rather than behaviour: it held `SpOverSlipLink` (worker thread,
sockets, helper process) and `FujiNetNetDevice` (host sockets) **by value**.
Including those headers pinned it to the top layer, and the practical cost was
that the rule *a card may not own a thread* could not be enforced against the
one card most likely to break it.

Two seams, the shape already used for `SuperSerialCard` and `W5100Device` —
interface at the device layer, implementation at runtime:

- **`FujiNetTransport`** — how the *host* drives the link: arm a transport,
  start and stop the worker, read counters. `Mode`, `Stats` and
  `kDefaultTimeoutMs` moved onto it because the panel renders them and the
  settings host persists them; they are contract, not implementation detail.
- **`FujiNetNetwork`** — the `N:` device as the *card* sees it. Six calls, no
  knowledge that the far end resolves a hostname and speaks HTTP. The `kNet*`
  command bytes and the STATUS error codes moved with it, because the card
  decodes them out of the guest's control list.

`makeFujiNetCard()` is now the only place in the tree that names
`SpOverSlipLink` or `FujiNetNetDevice`.

Two side effects were worth more than the reclassification. The
`setLinkForTesting` hook is **gone** from the production header — injection
makes it redundant, and seam-test cards now own a fake link plus the null
transport/network, so they *cannot* open a socket rather than merely declining
to. And `NetworkCoordinator`'s snapshot binds the two surfaces separately, so
the call site shows which reads are protocol (`isConnected`, `devices`) and
which are host lifecycle (`mode`, `running`, counters); it was one binding
before and read as if it were all one thing.

Enforcement verified by mutation: adding `<thread>` or `SocketCompat.h` to
`FujiNetCard.h` now fails configure with a layer violation. Neither was
catchable while the card lived at RUNTIME.

**POM2 is also consumable as a dependency now, not only as a build.** The core
facade (`include/pom2/core.hpp`) had been in the tree for a while with nothing
outside the build able to reach it, so "POM2 as a library" was an untested
claim. `pom2_core` is a real installable STATIC library exported as
`POM2::core` under a `pom2_core_sdk` component; its source list is shared with
`pom2_core_test`, so the shipped archive cannot drift from what the suite
covers. `include/` is PUBLIC and `src/` is PRIVATE — a consumer sees
`<pom2/core.hpp>` and nothing else.

`set_target_properties(... EXPORT_NAME core)` is load-bearing rather than
cosmetic: the in-build `POM2::core` ALIAS does not survive into the installed
package, which exports the target under its real name. Without it a consumer
must write `POM2::pom2_core` and the two spellings diverge silently. This was a
real failure, not a hypothetical — the first consumer build died on
"target POM2::core not found" against an otherwise well-formed package.

`pom2_core_sdk_consumer` is the only test that exercises POM2 from the
**outside**: it installs into a throwaway prefix, configures
`examples/pom2_core_consumer` as a standalone project through `find_package`,
builds it against the imported target and runs it — booting a machine,
rendering a frame and pulling audio. It catches a class of breakage an in-tree
build is structurally blind to: a broken export set, an uninstalled header, a
renamed target, or a transitive dependency the generated package file forgets
to re-find.

## 2026-08-27 (verification) — Two backlog entries were stale, and are now pinned

Neither of these was a code change. Both were long-standing TODO entries
claiming a defect that no longer existed; verifying them cost less than
carrying them, and they are now pinned so they cannot silently regress.

**`$C05E/F` under IOUDIS was already correct.** The 2026-07-30 IOUDIS work
gates the whole `$C058-$C05F` range on `iicProfile_ && !ioudis`, which is
MAME's `(m_isiic || m_isace500) && !m_ioudis` split — with IOUDIS clear those
addresses are the IIc IOU's X0/Y0 edge selects and never reach DHIRES, and with
it set they are SETDHIRES / CLRDHIRES again. Pinned by `iic_ioudis_dhgr` across
all three //c dumps, reads *and* writes, skipping when no dump is present so CI
stays ROM-free. Mutation-checked: narrowing the gate to `$C05D` fails it.

**II+ `$C00C/D` on reads is deliberate, not a bug.** A IIe's 80COL switch is
genuinely write-only (a read of `$C000-$C00F` returns the keyboard latch and
never reaches the IOU), while a II+ has no IOU and no 80COL signal at all — so
POM2 synthesises the RGB card's FIFO data line from the bus access itself,
which is the only way a Le Chat Mauve / Video-7 class card is usable there. The
`display.eightyCol` it sets is inert for rendering: every consumer in
`Apple2Display` gates on `mem.isIIE()`. Pinned by `iiplus_rgb_data_line`, which
checks both halves — the card clocks its FIFO to COL140, *and* `width()` stays
280 on a II+ while a IIe with the same flag goes to 560. Mutation-checked both
ways.

## 2026-08-27 (later) — Every alias gone, every coordinator wired

The campaign that started with an unmergeable branch finishes here. All
eighteen non-owning card aliases are gone from `MainWindow`, and all eleven
coordinators are in use. `MainWindow.cpp` went 8 913 → 8 289 lines, but the
line count is the least of it: what changed is that no UI path holds a pointer
to something `SlotBus` can destroy underneath it.

**The three device seams were re-derived, not merged.** Each one on the branch
predated a fix main had shipped, so merging would have quietly reverted it:
- `W5100Socket` — the branch's host implementation has no `disableSigpipe` and
  no `MSG_NOSIGNAL` send, because main's SIGPIPE work landed 2026-08-22, after
  the fork. Merging it would have restored a defect that kills the whole
  process when a TCP peer vanishes. Every hardening step was moved with the
  comment recording the failure it prevents. Its `send()` also had to gain a
  mode: main's TX path calls `sendto` and plain `send` against one socket, and
  `flushPendingTx` continues an already-accepted tail that must not be
  re-addressed.
- `FujiNetLink` — needed no code moved at all. `SpOverSlipLink` already
  implemented every method with matching signatures; it was simply never named
  as an interface. The card's accessor split into `link()` (commands) and
  `transportLink()` (lifecycle the UI configures), which is what made the
  interface possible.
- `SuperSerialTransport` — the only one that moves a thread. `guardedThread`
  and the join-before-reassign fix (a worker that exited on its own leaves a
  joinable thread, and assigning over it calls `std::terminate`) were ported
  INTO the transport.

Each seam has a fake and a test that opens no socket, and the assertions that
matter are mutation-checked rather than assumed green.

**The FujiNet panel was the worst alias of the eighteen.** It bound
`auto& link = card->transportLink()` outside any lock and wrote through that
reference in six separate critical sections, applying the timeout change with
no lock at all — against a card that owns a listening socket, an open serial
device and a worker thread.

**And one deadlock, introduced and caught during the work.** Wiring
`PrinterCoordinator` into `renderFujiNetPanelWindow` put a locking call inside
an existing `lock_guard(stateMutex())`. `stateMutex` is non-recursive, so
opening that panel would have hung the UI thread and the emulator together —
while the full suite stayed green, because nothing drives the ImGui panels.
`tools/check_coordinator_locks.sh` is the answer, and it earned its keep three
more times after that: a genuine blind spot (a function that RECEIVES a
`StateAccess` is locked for its whole body and has no `lockState()` call to
find), a false positive worth fixing (`} else {` nets zero braces, so a
per-line counter thought a closed scope was open), and a third finding class
(`…Locked` methods taking a `StateAccess` MUST be inside the lock).

## 2026-08-27 — Wiring the coordinators, and what the aliases were hiding

Five of the landed coordinators are now wired into `MainWindow`:
`MouseCoordinator`, `PrinterCoordinator`, `AudioCoordinator`,
`DevicePanelCoordinator` and `StorageCoordinator`. Thirteen of the eighteen
card aliases are gone.

The aliases were never only a tidiness problem, and the wiring turned up a
dozen defects behind them. Two families account for almost all of it.

**A default argument that meant "drive 1 only."** `DiskIICard`'s drive
parameter defaults to 0, so `isDiskLoaded()`, `getDiskPath()` and
`ejectDisk()` each silently ignored drive 2 — in five different places. Drive
2 was never restored at startup, was lost on every profile switch, was dropped
by Slot Config Apply, stayed mounted through eject-all, and had its path
persisted on exit only to be ignored on the next launch. Each site read
correctly; the bug was that the *whole set* had to be wrong together for the
feature to work, and nothing held them together. `StorageCoordinator` iterates
`kDiskIIDriveCount` in one place.

**A mutation that never wrote the key recording it.** Machine ▸ Eject disk and
Eject HDV cleared the drive but left `disk_path_slotN` / `hdv_path` set, so the
next launch re-mounted what had just been ejected. The Disk II and HDV
write-back toggles wrote the card and not the key, and because
`isWriteProtected()` is `fileWriteProtected || !writeBack`, the setting
reverting meant DOS 3.3 answered WRITE PROTECTED again. The coordinator's
commands mutate under the lock and persist after releasing it, together.

The rest were routing: eject-all reported success over a failed write-back
(the medium stays mounted so the write can be retried — that is exactly when
the user needs telling); the Library's 3.5" eject called `eject35()`
unconditionally, so with a SmartPort card owning the media the button did
nothing while the panel went on showing the disk; the mixer drew one row per
alias, so a second coexisting Mockingboard had no control and inherited the
other's persisted level; the ImageWriter's "not feeding" list never named the
FujiNet unit, so a FujiNet-only setup showed an unconnected printer; and
`printer_sound_pan` was read at startup but never written.

**Slot construction stops interpreting storage settings.** The plug lambdas
build empty hardware; one `restoreMediaFromSettings()` pass runs against the
finished topology. The ordering is the point: "is this the primary card"
decides whether the legacy unsuffixed keys apply, and it is a property of the
whole bus — asking it while the bus was half-built is why a moved primary HDV
restored against the wrong keys. It also deleted a duplicate: the constructor
had its own restore loop that ran *after* the plug pass, so every image was
opened twice at startup.

**One deadlock, introduced and caught here.** Wiring `PrinterCoordinator` into
`renderFujiNetPanelWindow` put a `captureHost()` inside an existing
`lock_guard(stateMutex())`. Every coordinator capture takes the machine lock
itself and `stateMutex` is non-recursive, so opening that panel would have hung
the UI thread and the emulator together — while the full suite stayed green,
because nothing drives the ImGui panels. It survived one review because the
check being run looked only for `lockState()` and that site uses the bare
`stateMutex()`.

`tools/check_coordinator_locks.sh` is the answer to that, and it is
falsifiable rather than assumed: against `44b715f` it reports the real bug and
exits 1. Its allowlist names the two shapes that may legally sit inside a lock
— plain accessors, and methods taking `SlotBus&`/`Settings&` directly rather
than an `EmulationController`, where the signature *is* the contract saying the
caller already holds the lock.

## 2026-08-26 — The coordinator branch lands the half that ports

`refactor/core-boundaries-and-coordinators` was written against `be055e2` and
sat unmerged while main went 83 commits ahead to v0.8.5. Both lines had
independently decomposed the same two god-objects, differently: the branch split
`Memory` into `Keyboard` + `PaddleInputs` (with a `.cpp`) and `MainWindow` into
`_Command`/`_Screen`/`_Media`/`_Input`/`_AuxPanels`; main split them into
`Keyboard` + a header-only `PaddleInputs` and `_Panels`/`_AudioPanels`/
`_SettingsPanels`/`_MiscPanels`. A textual merge offered 139 conflict hunks, 61
of them with real logic on both sides.

**The rule used to resolve them: the released code wins on every collision.**
Where the two lines had rewritten the same thing, v0.8.5's version was kept and
the branch's dropped — not because it is better, but because resolving toward
the branch silently reverts fixes that shipped. The concrete case that settled
it: the branch's `W5100HostSockets.cpp` has no `disableSigpipe` and no
`MSG_NOSIGNAL` send, because main's SIGPIPE hardening landed 2026-08-22, after
the branch forked. Merging that seam would have restored a defect that kills
the whole process — no log line, no dialog — when a TCP peer vanishes.

**What landed.** The ten host-policy coordinators plus `SlotCardFactory`, and
the nine tests that pin them. These port unchanged because they were written as
renderer-free value/command boundaries over `SlotBus` + `EmulationController`:
none of them includes `MainWindow`, so which panel decomposition is in the tree
is not their business. They compile and pass against v0.8.5's card APIs.

Also landed, from the branch's test infrastructure: `pom2_core_test`, an
assertion-enabled static archive of the core minus the renderer, so a test
declares its own sources instead of re-listing a private bundle of core ones. It
is expressed against v0.8.5's flat source list (the core 91 of 137 `SOURCES`,
less `JoystickInput.cpp` for GLFW and `DebugCoordinator.cpp` for Dear ImGui)
rather than the branch's CMake object layers. New tests link it per target, so
the 197 pre-existing tests keep the explicit bundles they were written with.

The public facade `include/pom2/core.hpp` + `src/Pom2Core.cpp` landed with its
contract test. That test failed on arrival, and the failure was a real bug worth
porting on its own: `CassetteDevice::beginRecordingIfNeeded` inferred "no
recording underway yet" from `lastOutputToggleCycle == 0 && recordedDurations
.empty()`. That is not the same predicate — after a reset or a cleared tape both
hold again while recording is still armed, so the first `$C020` toggle re-ran
the start path and the capture never began. Now an explicit `recordingStarted`
flag, cleared in `reset()` and `clearRecordedTape()`.

**What did not land, and why.** The branch's `MainWindow` and `Memory`
decompositions, superseded by v0.8.5's. The device injection seams
(`W5100Socket`, `SuperSerialTransport`, `FujiNetLink`) and the deterministic
fakes built on them — they need main's later socket hardening, thread guards and
torn-read fix re-derived on top of them first, which is a port to do
deliberately rather than inside a merge. `NetworkCoordinator` with them, since
it is the one coordinator that does depend on those seams. The CMake layer
guard and the `find_package(pom2_core)` install contract, which rest on the
branch's layered object libraries; the flat v0.8.5 tree has no installable
library target for them to attach to.

The coordinators are therefore in the tree and under test, but not yet wired
into `MainWindow` — that wiring is the next step, and it is now an ordinary
refactor against one decomposition instead of a merge between two.

207/207 ctest green.

## 2026-08-23 — Keyboard and PaddleInputs leave the Memory god-object

The order the TODO insisted on: the I/O-path test net first, the split second.
`input_io_smoke` pins the observable bus behaviour — the $C000/$C010 latch as
newest-wins-key plus FIFO-paste, the $C061-$C063 buttons and Open/Solid-Apple,
the $C064-$C067 paddle RC timing and the $C070 re-arm — all through
`memRead`/`memWrite`, so the extraction has something to be checked against
rather than a promise that it "looks the same".

Then both concerns came out. `PaddleInputs.h` owns the game port: four
paddles, three buttons, the Open/Solid-Apple and Shift modifiers, and the
$C070 RC-discharge latch, with the read logic (`button0/1/2`, `discharging`)
that the $C061-$C067 path now calls. `Keyboard.h/.cpp` owns the latch and the
host paste FIFO — the control-byte filter, the CR/LF collapse, the ][/][+
case-fold (now a `foldToUpper` parameter Memory passes as `!iieMode`), the
runaway-paste cap, and the lock-free `$C000` mirror that keeps the hot read
off the mutex. `Memory` keeps one of each and forwards; the softswitch path
calls `keyboard_.latchMirror()` / `lastKey7()` and `paddles_.discharging()`
directly. The hot path is unchanged — the same atomic load on $C000, the same
lock discipline the joystick block already used for paddles.

Behaviour-preserving, and checked as such: `input_io_smoke`, `paste_smoke`
(case-fold, cap, ordering, reset), `iie_memory_smoke`, both snapshot
round-trips and `ui_worker_contention` all green. `Memory.cpp` 2539 → 2391 and
`Memory.h` shed its keyboard/paddle members; the perf job (the 256-entry
`memRead` dispatch) stays in Memory and stays a separate item, deliberately
not merged with this compileability split.

## 2026-08-23 — A TSan pass for the contention the GUI actually creates

The nightly TSan matrix covered the worker's park/resume, rewind, the audio
teardown and the helper-process threads — every test that happens to have two
threads — but not the one contention the GUI creates on every single frame:
the UI thread reading and writing emulated input state while the CPU worker
runs. MainWindow can't be instantiated headless (GLFW/GL), so the race the
whole "TSan the GUI half" item worried about had no test at all.

`ui_worker_contention` is that test without the window. It drives
EmulationController's real worker and, from a second thread, hammers it with
MainWindow's *exact* access disciplines — paddles and buttons under
`lockState()` (the joystick block's shape), `queueKey` with no lock (Memory's
`kbMutex` owns it), Open/Solid-Apple through the atomic flags — while the
worker executes a loop that reads `$C000`/`$C061`/`$C064`, so it touches the
keyboard mirror, the Apple-key atomics and the paddle arrays at the same time.
Under `-fsanitize=thread` this flags any access that escapes those
disciplines; it came back clean, which is the result — the disciplines hold
under real contention, and now a change that reads a paddle off the lock or
writes the latch without `kbMutex` turns the nightly TSan leg red instead of
shipping. Added to the matrix's target list (nine binaries, not eight).

What this does NOT cover, and the item stays open for: `demodMutex`, slot
re-plug under load, and MainWindow itself (still unreachable under TSan
headless).

## 2026-08-23 — The W5100 "lost datagram" was a torn 16-bit read

The one finding the 2026-08-22 sweep left open as a possible real bug — a
loopback datagram vanishing about once in 40 runs — turned out to be a test
artefact, and the shape of it is worth keeping. `SN_RX_RSR` is a 16-bit
register the test read one byte at a time, and on this chip reading its high
byte is *also* what pulls a datagram off the host socket: the Uthernet II has
no RX interrupt, so POM2 receives when the guest polls. A datagram landing
between the two byte reads therefore produced a torn value — the old high byte
with the new low byte — and a 1408-byte datagram read back as 128 (`0x0080`
against the expected `0x0580`, low byte intact, high byte stale).

Instrumenting the assertion to print the staged size is what turned "sometimes
zero" into "always 128 when it fails", which named the tear immediately. The
fix is in the test, not the model: a real W5100 driver reads RSR until two
consecutive reads agree (datasheet §5.2.2), because the register is not latched
against a receive in flight — and `pollForData` now does the same. The model
is faithful; the test was reading a moving 16-bit register as though the two
halves were a snapshot. 0 failures in 250 runs after the fix, across all three
cases that used to trip. The `RUN_SERIAL` mark added on 2026-08-22 to rule out
cross-test contention stays — it was never the cause, but a test that binds a
loopback port has no reason to race the others.

## 2026-08-23 — Read watchpoints, and the flag that is never tested

The debugger's write half shipped free in the morning because `memWrite`'s
fast path already consults a per-address byte a watch can hide in. `memRead`
has no such byte — `mem[addr]` on a ][+, a paging computation on a //e — and
that was the honest reason reads stayed out. The shape that fits is one level
coarser: a single flag, `Memory::readDivert_`, true while ANY read watch is
armed, under which every read takes `memReadSlow`, which performs the read and
reports the watched ones after it.

The obvious implementation — test the flag at the top of `memRead` — measured
**+7.2 % / +4.2 %** on the ][+ and //e banners: one byte load and a branch
that always predicts, and still half the cost of the naive tap rejected on
2026-08-22. What shipped tests nothing new. The fast path already decided
three things — `!iieMode || testMode`, `!bankTrace_`, `!iicProfile_` — and
each became a derived byte with `readDivert_` folded in
(`plainRead_`, `iieFastRead_`, `romFastRead_`, recomputed by
`refreshReadFastFlags()` wherever a source flag is written). Arming a read
watch closes all three gates and the reads fall through on branches that were
already there; the ][+ path went from two tests to one on the way. Interleaved
best-of-9 on the three `pom2_bench` workloads, RAM hashes identical:
**+0.0 % / −3.0 % / −0.5 %**.

A second trap was caught by the same discipline: wrapping the original slow
body in the report left the compiler keeping that large body out of line, a
reproducible **+1.0 %** across three best-of-15 series on the ][+ banner, whose
keyboard poll lives on the slow path. `always_inline` took it to +0.0 %.

Armed, the cost is the pre-2026-08 profile coming back — every bus read out of
line — measured with the new `pom2_bench --read-watch` at **+38 % / +55 % /
+11 %**, paid only by the session that armed one and only while it is armed;
the machine still runs at well over 100× real time under the bench. The panel
offers R / W / RW and defaults to W, which is free in both states. Morning's
"the API keeps only the Write bit" is gone with the reason for it; a read watch
fires on the bus access after the read, opcode fetches included — a watch on
`$FBB3` answers "who checks the ROM ID byte" — and soft-switch reads included,
with their side effects, as on the real bus. Pinned by `debugger` cases 10-11.
→ [DEV § Debugger](DEV.md#debugger-debuggerhcpp-debugger_imgui),
[PERFORMANCE § 8.5](docs/PERFORMANCE.md)

## 2026-08-23 — The macOS SIGBUS was a stack overflow, and a real one

The first macOS CI run ever to execute the suite came back 191/192, with
`disk_path_snapshot` killed by SIGBUS on arm64 and green on Linux at every
parallelism and under every sanitizer. Filed as a platform finding with the
first things to try on a Mac. On a Mac it took one run: deterministic, 10/10,
and `lldb` put the fault in `___chkstk_darwin` — the stack-probe helper — with
`DiskImage::loadFile` as the caller. Not the dangling reference the test's own
comments invited everyone to suspect; the reference in case 1 is sound (it
aliases an in-place `images[2]` member whose identity survives insert/eject).
It was the **writer thread's stack**.

`sizeof(DiskImage)` is **247 480 bytes**: the 35 track buffers live in the
object (`std::array<std::array<uint8_t, 6656>, 35>`), which is what keeps the
LSS hot path free of an indirection. The insert chain stacked one temporary
per frame — `insertDisk`'s `replacement`, `prepareDisk`'s `staged`,
`loadFile`'s own `replacement` — about 725 KB of frames. A Linux `std::thread`
gets 8 MB of stack and never noticed. A macOS one gets **512 KB**. The test's
writer is a `std::thread`; so is the AI control server's HTTP thread, which
reaches `insertDisk` by exactly that path on every `/disk` insert. The test
did not have a platform bug. The app did, and a `/disk` request on a Mac
could kill it.

Fixed by heap-allocating the six temporaries (`std::make_unique<DiskImage>`
in `DiskImage::loadFile` ×2, `DiskIICard::prepareDisk` / `insertDisk` /
`installDisk`, and `mountDiskII`). The object's layout is untouched — the
alternative, moving `tracks` to the heap, would have made every move O(1)
instead of a 233 KB memcpy, but it touches the hottest disk path and was not
the bug. A NOTE on `class DiskImage` now states the constraint.

Pinned by `diskii_insert_thread_stack`, which runs the insert chain on a
pthread whose stack is *explicitly* 512 KB, so the regression fails on Linux
CI too rather than only where the platform default happens to be small.
Verified falsifiable: exit 138 with the source fix stashed, green with it.
`disk_path_snapshot` itself is unchanged and 20/20 green on the Mac. The
macOS CI job now runs `ctest`, in the same commit, as its comment promised.

## 2026-08-23 — A panel is described once now, not six times

The UI's god-object problem was never the line count. It was that every panel
existed in six places at once: 32 lines loading its visibility, 32 saving it,
38 offering it in the command palette, 38 dispatching that command, 37 menu
rows carrying its label / tooltip / greyed-out condition, and a 28-assignment
block hiding "every" panel on the browser build. Splitting the file into
`MainWindow_<Area>.cpp` moves those rows between files; it does not remove one
of them, and the coupling is what hurts.

They had already drifted, in exactly the ways six unlinked lists drift:

* **Seven panels had no settings key at all** — the Debugger and the memory
  viewer among them. The palette opened them and they were gone next launch,
  and nothing in the code said whether that was a decision or an omission.
* **The WASM chrome-light block named 28 panels by hand**, so every panel added
  after it was written stayed open on the browser build. A list that could only
  rot in one direction.
* **The Help menu attached ROM Status's tooltip to the Abstraction Levels row**
  (two `IsItemHovered` blocks after the same `MenuItem`): one row showed the
  wrong tip, the other showed none.
* The palette and the menus carried **different labels for the same window** —
  "Disk II drive" against "Disk II (slot 6)".

`PanelCatalog.h` is now the one list: 38 rows of `{id, title, menu group,
settings key, shortcut, tooltip}`. `PanelRegistry` binds each row to the `bool`
holding its visibility plus two optional runtime bits — an availability
predicate (an unplugged card greys the row) and a dynamic title (the label that
carries a slot number). `MainWindow_Panels.cpp` holds that binding table, and
the menus, the palette, the palette's dispatch, the settings round-trip and the
browser build's chrome-light startup are all *derived* from it.

The Devices menu went from 157 hand-written lines to eight — four
`SeparatorText` headers and four `panelMenuGroup` calls. `MainWindow.cpp` lost
**341 lines** (11 495 → 11 154, ceiling lowered in the same commit),
`MainWindow.h` lost its 38 visibility members, and both stopped being where
panel facts live. Adding a panel is a catalog row
plus a `bind` line; forgetting the bind is caught at startup by
`PanelRegistry::unbound()`, which names the panel in the log instead of leaving
a menu row that toggles nothing.

The five formerly keyless View panels now persist, and the Welcome panel is the
one entry that deliberately does not — a first launch with no ROM opens it from
the constructor, before settings are read, so a stored `false` would cancel the
greeting a newcomer needs. `panel_registry` counts that exception rather than
allowing it: a second keyless panel fails the test and has to argue its case.

Verified end to end, not just built: `panel_registry` (catalog invariants,
catalog-order iteration, the persistence loop, unbound reporting), the whole
suite at 195/195, and a real GUI run under Xvfb — startup logs no unbound or
unknown panel, a clean exit writes all 37 keys, and editing two of them in the
config and restarting brings them back, which is the load half nothing else
proves.

**The storage moved in too, and the render block became a loop** (same day,
second pass). `MainWindow` no longer carries 38 `bool showXxx` members:
visibility is one `std::array<bool, PanelId::Count>` in the registry, reached
as `show(PanelId::Debugger)` — a `bool&`, so the 92 sites that read or took the
address of a member changed name and nothing else. Correspondence between the
enum and the table is by an explicit field in each row rather than by position,
and `panelCatalogIsComplete()` is a `static_assert`: a forgotten row, a
duplicate, or a copy-pasted enumerator fails the build instead of quietly
toggling the wrong window. `defaultOpen` in the catalog replaced the three
`= true` member initialisers that decided what a fresh install shows.

`MainWindow::render`'s ~43 panel calls — some gated by the caller, most gating
themselves, ordered by whoever added them last — are now `renderPanels(delta)`,
walking the catalog and drawing each panel while it is visible. What stayed
behind is the code that is not a panel: the modal file dialogs,
`pumpImageWriter()` (a side effect that must run whether or not its window is
open), the About box, the status bar, and the palette overlay, which must still
be last so it draws above everything.

**One panel is drawn while closed**, and finding out why is the reason this
kind of loop is worth pinning: the //e keyboard latches Open-Apple and
Solid-Apple, and a latch that outlives the window showing it as down is a key
the guest holds forever with nothing left to release it. `renderKeyboardPanel`
had an edge-triggered teardown in its "not visible" branch, which a naive loop
would simply never call. `Runtime::drawAlways` keeps it called every frame;
`panel_registry` pins that it is.

Verified in the running GUI, not only in tests: a config with 30 panels open,
run under Xvfb, and every one of them appears in `imgui.ini` with a real
`Pos`/`Size`/`LastUsed` — the marker ImGui only writes for a window that was
actually created that session, as opposed to one merely pre-docked by the
layout builder. Card-gated panels correctly did not draw with no card plugged.

Deliberately a table and not self-registration: static registrars in 40
translation units scatter the UI surface back across the codebase, and the
value here is that one file answers "what panels exist, where do they live,
what persists". Same argument the command palette already makes for its
non-panel commands.

## 2026-08-23 — Write watchpoints, and they cost the hot path nothing

The debugger shipped on 2026-08-22 with breakpoints, step, step-over and
run-to-cursor — and without watchpoints, because the obvious implementation had
been measured: wrap `memRead`/`memWrite`, test one pointer, call a sink, and
pay **+13.4 % / +16.5 % / +9.2 %** on the three `pom2_bench` workloads. Forcing
the wrapped body inline made it worse, which located the cost in the extra
branch and the code growth around the hottest function in the emulator rather
than in an inlining accident. The number was kept and the feature was dropped.

Keeping the number is what made the feature possible a day later, because it
stated the budget precisely enough to design against: **do not add a test to
the fast path**. So this one does not test for a watchpoint there. It removes
the address *from* the fast path.

`memWrite`'s hot case already consults a per-address `writable[]` byte. Arming
a write watch CLEARS that byte, so the existing test fails for that address and
the write falls into `memWriteSlow` on its own — no new branch, no new load,
not one instruction added. The slow path reports the access and performs the
write from a shadowed copy of the real permission, kept beside the armed bit in
a table that is empty and unallocated until somebody debugs. Interleaved
best-of-9 against the previous binary, pinned to one core, RAM hashes identical
on every workload: −2.1 % / −0.7 % / −0.1 %, which is this host's noise floor —
no measurable cost. Numbers and method: `docs/PERFORMANCE.md` § 8.3.

Three ways this design can go wrong, all pinned by `debugger` cases 7-9:

* **The write must still land.** A diverted address is write-protected as far
  as `writable[]` knows. Forget the shadow and a watchpoint silently corrupts
  the machine under the debugger's nose — a worse failure than not stopping.
* **The diversion must not invent permission.** A watch on ROM reports the
  access and still drops the write, and `markRomRegion` — which a profile
  switch runs while a watch may be armed — updates the shadow too.
* **A state restore must ignore it.** `restoreMainRam` skips non-writable cells
  so a snapshot cannot clobber the ROM mirror; it now asks `ramWritable()`, or
  a watched byte would be the one cell every rewind quietly refused to restore.

One of the two caveats recorded when the design was sketched **turned out not
to exist**: Language-Card paging does not rewrite `writable[]` at all —
`markRomRegion` is its only mutator and the LC has its own path — so the shadow
had exactly one function to survive rather than a soft switch on the hot path.

Addresses from $C000 up need no diversion at all, since those writes already
reach `memWriteSlow`: soft switches, slot I/O and the language card are
watchable for free. The stop lands at the first instruction boundary after the
access (it cannot be un-done), so the banner reports two different and equally
useful addresses — what was written, and the instruction that wrote it, latched
by `onInstruction` because the CPU's own PC has walked past the operands by the
time Memory reports.

**Read watchpoints are still not implemented**, and the API still accepts a
Read watch that never fires rather than faking one. `memRead`'s fast path has
no per-address table to hide a watch in, which is precisely why the write half
was free and the read half is not. The panel offers writes only, and says so.

`Memory` reports through `MemoryWatchSink` — a two-line interface — rather than
calling `Debugger` directly, because `Memory.cpp` is linked into two dozen test
binaries and a benchmark that have no business pulling in the debugger.

The file-size ratchet failed on `src/Memory.cpp` (+59 lines) and the budget was
raised in the same commit rather than split into a new translation unit: that
is the question the ratchet exists to force, and the answer here is that the
`Memory` split has a prerequisite — TODO's P2 says it waits for an I/O-path
test net — so doing it now to save 59 lines would be doing the wrong half of it.

Suite 194/194.


## 2026-08-23 — Rewind stops recording a timeline that runs backwards

`RewindBuffer`'s whole seek layer assumes one thing about the ring: cycle
stamps increase. `indexForCycle` scans forward and **breaks** at the first
frame past its target, which is only a correct search while that holds. It was
assumed rather than enforced, and the machine breaks it routinely.

`rewindEndAndResume` is only one of the ways POM2 resumes. It is the only one
that truncates the abandoned future — and the toolbar Play button, Machine ▸
Run, the `machine.run` palette command and the kiosk menu all call
`setMode(Mode::Running)` directly instead. So: scrub ten seconds back, press
Play on the toolbar rather than "resume here", and the frames captured from the
rewound point were appended *after* frames stamped ten seconds later. Nothing
crashed. The timeline simply started lying — the span readout went wrong (and
could go negative through an unsigned subtract), and every seek past the join
landed somewhere other than the cycle asked for.

The second half was worse to use. `Rewind_ImGui::scrubbing_` also stayed true,
because only the panel's own buttons cleared it. `beginScrubIfNeeded` early-outs
when that flag is set, so the next drag skipped the `setMode(Stopped)` +
`waitUntilParked()` and seeked a **running** machine: the slider visibly did
nothing. A released `F6` hold, meanwhile, called `rewindEndAndResume` with a
stale cursor and teleported the machine into a timeline the user had left.

Fixed at the two layers that own the two halves, not at the four call sites:

* **`RewindBuffer::capture`** drops every frame stamped at-or-after the incoming
  one before appending. One compare on the hot path; the walk is O(dropped). A
  jump back past the oldest retained frame clears the ring instead, so the
  restart is a keyframe rather than a delta against a base blob from a timeline
  that no longer exists. Being in the one funnel every capture goes through, it
  also covers a snapshot load that forgot its `rewind().clear()` — and resume
  paths nobody has written yet.
* **`EmulationController` owns the scrub** (`scrubIndex_`, read via
  `rewindScrubbing()`); `setMode(m != Stopped)` ends it, whoever asked. The
  panel's flag became a view of it, resynced on entry to `render`,
  `beginScrubIfNeeded` and `releaseHold`.

The truncation is deliberately *not* done inside `setMode`: it is reached from
callers already holding `stateMutex` (the Disk II Library's boot buttons take
`lockState()` and then resume), and `stateMutex` is non-recursive — truncating
there would deadlock the machine to fix a cosmetic bug. Deferring the drop to
the worker's next capture keeps every resume path lock-free.

Pinned by `rewind_roundtrip` case 5 and `rewind_transport` case 6; each half
verified falsifiable on its own (disable the drop → the transport test's ring
never shrinks; disable the scrub clear → the bare resume is still flagged live).

## 2026-08-23 — A dead FujiNet helper stops costing 250 ms per call

The `stateMutex` family's last substantive member, and the one whose
description was slightly wrong about itself. The item read "transact waits up
to timeoutMs inside a SmartPort call" — but the wait was never the bug. A
bounded stall repeated without bound is not bounded.

`transact()` already declared the peer lost when a WRITE failed. A silence did
not: a helper that accepted writes and never answered kept its socket open, so
every subsequent call paid the full budget again. 250 ms per call, forever. A
ProDOS boot reads a lot of blocks, which is why the symptom people reported was
"POM2 hangs" rather than "one call was slow" — and the FujiNet panel's own Stop
button was unreachable throughout, because drawing it needs the same mutex the
CPU thread is sitting on inside the call.

Three consecutive timeouts now drop the link. That closes the socket, so every
later call returns at the `isOpen()` gate having waited for nothing. Three
rather than one, deliberately: a single timeout is an ordinary hiccup on a busy
helper, and dropping a live peer over one slow reply would be its own bug.
A dead peer now costs ~0.75 s in total instead of ~250 ms per block.

Pinned by a new case in `sp_over_slip_link`, and it is worth saying what it
pins, because the file already had a timeout test. `testTimeoutAndRecovery`
covers "a call can time out and the link survives" — which must keep working,
and does. The new case covers the other thing: a peer that answers NOTHING is
dropped, and the call after that costs under 60 ms rather than another full
budget. Verified falsifiable by disabling the counter.

**The two thread `join()`s in the same family were examined and deliberately
left**, with the reasoning recorded in `TODO.md` so it is not re-derived.
`slotBus().clear()` on a profile switch is not a machine freeze at all —
`applyProfile` has already stopped the CPU worker, so only the UI blocks,
during a modal cold reset. And the FujiNet Stop / Drop-peer buttons genuinely
need their lock: `FujiNetCard` reaches `transact()` from the CPU thread under
`stateMutex`, and `stop()` ends in `transport_.reset()`, so dropping it trades
200 ms for a use-after-free on a live transport. The clean fix is to move that
exclusion onto the link's own `callMtx_` — the lock order `transact` already
uses, so no inversion — but that is a lock-order change in a three-thread
subsystem to save 200 ms on a button the user pressed on purpose. A one-off
expected pause is not the repeated freeze this entry is about.

## 2026-08-22 (last) — The 32 MiB stall: HDV mounts stop freezing the machine

The last big member of the `stateMutex` family, and the one with the worst
number on it. Mounting an HDV held the lock the CPU worker takes every
4096-cycle chunk and the UI thread takes to paint every frame, across a read of
up to 32 MiB — so the machine and the window stopped together, cancel button
included, for most of a PAL frame at best.

`Block512Backing::loadImage` was 131 lines doing four things, and it is now
split at the seam that matters: `readImageFile` is **static** — it touches no
object state, which is exactly why it can run with no lock — and does the open,
the size gates, the read, and the host-writability probe (a syscall, so it
belongs on that side). `adoptImage` does the flush, the 2IMG parse and the
adopt, under the lock, making **no syscalls at all**. `ProDOSBlockCard` and
`SmartPortUnit` each carry a matching `adoptImage`; `CffaCard`,
`ProDOSHardDiskCard` and `SmartPortHdvUnit` forward it in one line each. All
seven UI mount sites now go through `pom2::mountBlockCard` /
`mountSmartPortUnit`.

Measured on a 32 MiB image: **25.8 ms under the lock, down to 0.0 ms.** The
zero is not rounding. Splitting alone got it to 10.4 ms — the remainder being a
straight 32 MiB copy out of phase 1's buffer into the backing store — so a raw
`.hdv`, where the payload IS the whole file, now moves that buffer instead of
copying it and the memcpy becomes a pointer swap. A 2IMG still copies: its
payload starts 64 bytes in, and moving then shifting it down would be the same
memcpy wearing a different hat. Containers are the smaller case and the honest
one to pay for.

The inline `loadImage` halved as a side effect, 25.8 → 13.4 ms, which the CLI,
the tests and the profile-switch remount all get for nothing since they share
the adopt half.

**The trap was real, and it is now guarded.** `loadImageFromBytes` was already
on the interface and looks exactly like a ready-made phase 2. It is for
SYNTHESISED volumes: it skips the 2IMG header parse, forces `synth_`, and ties
write-back to it — so a real `.hdv`/`.2mg` routed through it would mount 64
bytes of container header as block data with write-protect and write-back
quietly wrong. Case 2 of `two_phase_block_mount` exists to fail loudly if
anyone simplifies back onto it, and it was verified by doing exactly that: the
mount is refused outright.

The other five cases cover what splitting a write-back path can break rather
than what it was split for: a raw image mounting identically both ways, the
2IMG offset and locked flag surviving, a chmod-read-only file still coming up
write-protected (the probe moved translation units), write-back still
preserving the container, and — the same hazard the Disk II split had —
re-mounting the SAME file while the guest holds unsaved writes, which must
flush and re-read rather than adopt bytes that predate the flush. That one was
verified falsifiable too.

## 2026-08-22 (newest) — A debugger, and the watchpoints that measurement refused

The audit two entries down named the debugger the highest-leverage thing left
in the backlog, on the grounds that its return is every other item. The
argument was countable rather than rhetorical: `tests/` held **24 one-off
trace / dump / probe binaries, 5 535 lines, of which 4 129 were registered as
no test at all** — compiled on every build, run by nothing. Every parity hunt
was paying for a throwaway binary because nothing could stop a running machine
and look at it.

**What shipped**: breakpoints, single-step, step-over, run-to-cursor, and a
banner that says *why* the machine stopped — "it stopped" and "it stopped
because you asked it to break at $C600" being different facts, only one of them
useful. `Debugger.h/.cpp` for the core, `Debugger_ImGui.*` for the panel:
registers, a disassembly that follows the PC, and a breakpoint gutter you click
the way every debugger since Turbo Pascal has worked.

**The CPU does not know about it.** `M6502.h` declares a two-method interface
and `Debugger` implements it, so the dependency runs one way and `M6502.cpp`
gained no include. `MainWindow.cpp` gained **six lines** — three to own and
show the panel, three to register it — and the F7/F8 shortcuts live in the
panel rather than in MainWindow's global key routing, because a feature that
can keep its keys out of the god-object should. Those six lines were not free:
the file-size ratchet added last week failed the build and forced the budget
edit into the same commit. That is the mechanism working, not being worked
around.

**A breakpoint stops BEFORE its instruction**, so the registers you read are
the state going in. Writing the test for that took two attempts, and the first
one is the interesting one: "check before the instruction at PC" and "check
after the previous instruction" leave the machine in the *same* observable
state everywhere except one place — a breakpoint on the entry PC of a run. The
first stops immediately having executed nothing; the second runs the
instruction and then never matches, missing the breakpoint entirely. The
original assertions passed under both. The test now breaks on the entry point,
and a hook moved after `step()` fails it.

**Cost when nobody is debugging: none, measured.** `M6502::run` picks between
two loops once per *call* — one branch per 4096-cycle chunk, not per
instruction — and the hook stays detached until something is armed. Three
`pom2_bench` workloads, three series, RAM hashes identical, no change outside
the noise floor.

**Watchpoints did not ship, and that is the part worth reading.** The obvious
implementation — wrap `memRead`/`memWrite`, test one pointer, call a sink —
measured **+13.4 %, +16.5 % and +9.2 %** on the three workloads. Forcing the
wrapped body inline made it *worse*, which locates the cost in the extra branch
and the code growth around the emulator's hottest function rather than in an
inlining accident that could be argued away. Thirteen percent is not payable
for a feature that is off by default, so the tap was reverted and `Debugger`
carries the watchpoint API **unhooked** — an API honest about being inert
beats one that is faked, and beats one that silently taxes every session that
never opens the debugger. The design that would be free for writes (arm the
watch by clearing the `writable[]` byte the fast path already consults, so the
address falls to `memWriteSlow` on its own) is written down in `TODO.md`, and
the numbers are in `docs/PERFORMANCE.md` § 8 so nobody re-derives them by
shipping the regression.

## 2026-08-22 (latest) — Architecture audit: the crashes you cannot report, and the freezes you cannot cancel

An audit pass over stability and solidity rather than over any one subsystem.
The micro-quality bar came out clean and stayed untouched: 114 first-party
translation units, **zero warnings** at `-Wall -Wextra -Wshadow`, and only four
benign ones under a much stricter set (`-Wduplicated-branches`, `-Wlogical-op`,
`-Wnull-dereference`, `-Wsuggest-override`, `-Wnon-virtual-dtor`,
`-Wcast-align`, `-Wformat=2`) — the other 215 all sit in vendored stb/miniaudio
or in `SocketCompat.h`'s macro branches. One raw `new` in the whole tree,
against 106 `unique_ptr`. Performance was left alone on purpose: `PERFORMANCE.md`
documents two profiling campaigns and, more usefully, what was deliberately
left on the table and why. The findings were all structural.

**Six of seven long-lived threads could kill the process in silence.** An
exception escaping a `std::thread` callable calls `std::terminate()` — no log
line, no message, nothing the user can report or you can diagnose. The rule was
already written down, verbatim, in `main.cpp`'s CLI deferred-action thread, and
applied at two sites out of eight. The unguarded one that mattered was the CPU
worker: `workerLoop()` drives `rewind_.capture()`, which grows multi-MB vectors
against a 256 MiB budget, so `bad_alloc` there was a live possibility. Factored
the barrier into `ThreadGuard.h` and put it on all of them. Two of the detached
threads needed more than a wrapper: `FujiNetNetDevice`'s lookup left its
`std::promise` unset on the failure path, which turns the waiter's `fut.get()`
into a `future_error` thrown on the *calling* thread, and `W5100Device`'s
leaked an `inFlight` count that pinned the guest on "DNS still in flight" for
the rest of the session. Both now settle on every path. Pinned by
`thread_guard`, which forks a child so a regression fails a test instead of
killing the test runner — verified falsifiable by removing the barrier from a
copy of the header (exit 1, `child died on signal 6`).

**Mounting a disk froze the machine and the window together — at ~20 sites,
not the four already known.** `stateMutex` is held by the CPU worker every
4096-cycle chunk and by the UI thread to paint every frame, so slow work inside
it stops both, cancel button included. The cause was the API's shape, not any
one call site: `insertDisk(drive, path)` flushes, reads, decodes and installs
in one call, so a caller needing the install serialised had to serialise the
read too. Measured floors, warm cache: 12.8 ms to read a 32 MB image (0.6 of a
PAL frame), 30.1 ms for a 4 MB write plus one `fsync`, and a commit does two.
Split into `prepareDisk()` (unlocked, all the I/O) and `installDisk()` (locked,
a move), wrapped as `pom2::mountDiskII` in its own TU so call sites shrank
rather than grew — `MainWindow.cpp` lost 25 lines to the migration.
`EmulationController::mount35` got the same treatment for 800K media, and the
AI server's `/snapshot/save` now serialises into RAM under the lock and commits
through a new `pom2::writeFileAtomic` outside it, while `/snapshot/load` reads
the file before taking the lock (the file-backed `SnapshotReader` pulls its
bytes lazily from *inside* `restoreMachineState`, so constructing it early was
not enough). Two costs were kept deliberately, both documented at the call
site: the outgoing medium's write-back still commits under the lock, because
swapping before knowing whether the old medium could be written loses the
user's changes; and re-inserting the *same* file while it has unsaved changes
re-reads under the lock, because phase 1 read before phase 2 flushed and
installing the stale image would roll the guest's writes back.

That last one is pinned by `two_phase_mount`, and writing it was instructive:
the obvious assertions — the file changed, the card reports no unsaved changes
— pass whether or not the collision is detected, because a clean medium is
never written back. The test only discriminates once it asks what the *guest*
would see: step the head to another track, write there, flush, and check that
track 0 still carries the first burst. A stale mounted image writes its whole
self back on that second flush and silently reverts track 0 — save a file,
re-insert the disk, save a second file, and the first one is gone. Verified
falsifiable by forcing the collision check to `false`.

The HDV / block-device mount is **not** converted and is now the biggest
remaining stall (up to 32 MiB). It is recorded in `TODO.md` with the trap
spelled out: `ProDOSBlockCard::loadImageFromBytes` looks like a ready-made
phase 2 and is not — it is for synthesised volumes, skips the 2IMG parse and
ties write-back to `synth_`, so routing a real image through it would quietly
disable write-back and write-protect.

**The test suite was green because CI ran it two-wide.** 132 of the 189
declared `TIMEOUT`s were 5 or 10 seconds. At `--parallel 8` on an idle 16-core
box, `diskii_lss_smoke` and `printer_history` blew budgets they clear in 0.30 s
and 1.24 s alone. That is not a gate, it is a coin that happens to be landing
heads, and it would have turned red the day CI moved to a wider runner. Fixed
at the source rather than per-test: one policy block at the end of
`tests/CMakeLists.txt` applies a 30 s floor to every test and an integer
`POM2_TEST_TIMEOUT_SCALE` for environments that are slower by a known factor.
The thirteen tests whose assertions are *about* elapsed wall-clock time — plus
`ai_control_server_smoke`, which binds a fixed loopback port — are marked
`RUN_SERIAL`, because no budget makes a timing assertion robust against
contention. CI now runs `--parallel $(nproc)`: using the runner's real width is
what keeps the fix honest. 191/191 green at `-j8` and at `-j16`.

**Windows and macOS only ever compiled on a tag push.** The `windows` and
`macos` jobs live in `release.yml`, which triggers on tags — so the 62 `_WIN32`
directives in the tree (29 in `SocketCompat.h` alone) were compiled once per
release, and a break on either platform surfaced *during* a release with the
version already cut. Added build-only jobs for both to `ci.yml` (packaging and
signing stay a release concern). macOS was meant to run `ctest` too, being a
genuine second platform — and its very first run earned the job its keep by
failing: 191/192, with `disk_path_snapshot` killed by SIGBUS on arm64. That
test is green on Linux at every parallelism, green under ASan+UBSan with leak
detection, and green under TSan, so it is a real platform finding rather than a
flake. It is filed in `TODO.md` with its signature and the first things to try
on a Mac; the job builds the tests (their portability matters) and runs none of
them for now. Neither an exclusion nor a permanently red pipeline was on the
table — the first stops the test meaning anything, the second trains people not
to look. Added a
nightly ASan+UBSan and TSan matrix: `POM2_SANITIZE` had been a CMake option
that CI never once used, which left the "controller TSan clean" result in
`TODO.md` with nothing keeping it true. The sanitizer job sets
`POM2_TEST_TIMEOUT_SCALE=6`, measured rather than guessed: the suite takes 71 s
uninstrumented and 417 s under ASan+UBSan on the same host.

Both sanitizer variants were run locally before the job was added — shipping a
CI job nobody has executed is how a pipeline becomes decorative — and two
things would have made it red on its first night. TSan dies instantly on
kernels >= 6.6 with "unexpected memory mapping", because the default
`vm.mmap_rnd_bits=32` puts mappings outside the range its shadow memory
assumes; `setarch -R` around ctest fixes it (6 of 8 tests died without it, 8 of
8 pass with it) and costs the other sanitizers nothing. And TSan reported a
write/write race inside libpulsecommon, reached through miniaudio's PulseAudio
backend — both stacks entirely in Pulse's own code, the classic
uninstrumented-library false positive — so `tests/tsan.supp` suppresses that
one library. Its patterns are deliberately narrow: TSan treats a
`called_from_lib` pattern matching more than one loaded object as a FATAL
error, and a bare `libpulse` matches both `libpulse.so` and
`libpulsecommon-*.so`, killing every test outright. Nothing is listed there
speculatively, for the same reason. Final locally: ASan+UBSan 191/191, TSan subset
8/8 across three consecutive runs.

Running it on the real CI then found three more things local runs could not,
which is its own small lesson about validating a pipeline on the pipeline. The
TSan leg died after a 51-minute build on a bash syntax error: `-R (a|b)`
interpolated bare into the run line is unquoted shell, so the filter now
travels through the environment and is quoted at use. Fifty of those
fifty-one minutes were spent compiling two hundred test binaries to run eight,
so that leg now names its eight targets. And the ASan leg ran past 100 minutes
without reaching ctest at all: `--parallel 2` is the convention the other jobs
use and this one cannot afford it, so the sanitizer build takes the runner's
full four cores and the job's ceiling moved to 180 minutes.

Four cores turned out not to be enough either — the instrumented build of the
whole test tree still ran about two hours before ctest, and a nightly that
spends its night compiling reports nothing. Almost all of that is translation
units identical to the night before, so the job now carries a ccache keyed on
the sanitizer flavour (never sharing objects between differently instrumented
legs) and prints its hit rate, so a night that goes slow again is diagnosable
from the log rather than by guesswork.

**`MainWindow.cpp` grew 74 % *after* the rule against growing it.** The
standing instruction in `TODO.md` is "do not grow the god-objects", target
< 3000 lines. The measurement: 5590 lines on 2026-05-27, 6622 at the audit that
set the target, 11511 today. A rule with no mechanism is a wish, so it has one
now — `tools/check_file_sizes.sh` is a ratchet over
`tools/file_size_budget.txt`, recording a ceiling for every first-party file
already above 2000 lines. Ceilings may go down freely and never up, a new file
crossing 3000 lines without one fails, and a stale entry fails too. Growing
`MainWindow.cpp` now requires editing the budget in the same commit — which is
precisely the moment to ask whether the code belongs in a new translation unit.
It does not shrink anything; it stops the bleeding while the split is planned.

## 2026-08-22 (later) — Bug hunt 2: five subsystems, and a process that died silently

A second sweep, this time over code that had nothing to do with the previous
day's work: disk-image parsing and write-back, the AI control server, the
snapshot and rewind machinery, the Uthernet II TCP/IP offload, and the
state-lock discipline across every thread. Five adversarial reviewers, then
each finding reproduced before it was fixed.

**POM2 was being killed by SIGPIPE during ordinary web browsing.** `SIGPIPE`
is fatal by default, and `W5100Device` was the only socket owner in the tree
that never armed `SO_NOSIGPIPE`/`MSG_NOSIGNAL`. The chip deliberately keeps
sending in `CLOSE_WAIT` — our direction is still open, per the datasheet — so
the ordinary shape of the retro web (server answers, server closes, guest
writes again) had the peer answer `RST` and the next write take the whole
process down: no log line, no dialog, the emulated machine and any
un-written-back disk simply gone. Reproduced at exit code 141 and pinned.

**A guest upload could lose a megabyte in silence.** `Sn_TX_FSR` is the only
backpressure signal the chip gives, and it was computed from the ring pointers
alone — but `sendData` advances `Sn_TX_RD` before knowing whether the host
socket took the bytes, parking the rest in a host-side backlog. So a guest
polling FSR exactly as the datasheet says was told "ring empty" every time,
kept sending, and had the connection killed after 1 MiB of its data was
dropped. It now counts the backlog, which turns "the peer is slower than the
emulated CPU" from data loss into ordinary flow control.

**One reported defect turned out not to be one, and the investigation is
worth more than the fix would have been.** A reviewer found that an MTU-sized
UDP stream loses every other datagram into the 2 KB power-on ring: the "come
back later" gate's comment claimed the datagram stayed queued in the host
socket, while the read that followed consumed it and the ring-room check then
dropped it. Peeking first made the comment true — and was reverted. A real
W5100 has nowhere to put a frame that does not fit its ring either; it loses
it on the wire, so the loss is FAITHFUL, and buffering it in the host socket
emulates memory the chip does not have. The peek also left datagrams queued
across socket teardown, which took the receive-path test file from 20/20 to
17/20 runs — measured both ways, twice. What shipped is the corrected comment
and a test that pins the loss as deliberate, so the next person to "fix" it
finds the reasoning first.

**Three ways a disk write could vanish without a word.** `saveDirty()` skipped
a dirty track whose nibble stream no longer decoded, then cleared the dirty
flags and returned success, logging "Saved 0 modified track(s)" — the guest's
sector silently reverted. `ejectDisk()` did not commit the burst the
controller was mid-way through, so clicking Eject during a `SAVE` dropped it
(both `insertDisk` and `flushPendingWrites` had always committed first; eject
was the one that did not). And on a CNib2 `.nib`, whose 6384-byte tracks are
padded to the 6656-nibble runtime width, everything written past 6384 was
discarded on save. All three now refuse rather than lie, keeping the changes
in memory so nothing is thrown away.

**A snapshot missing its `MEX` section restored half a machine and said it
worked.** `MEX` is the only carrier of the IIe paging mode, the language-card
latches, all 64 KB of aux and the display state: without it, main RAM, the CPU
and the clock are replaced while every soft switch keeps the *live* session's
values. On the default //e profile with ALTZP set, zero page and the stack then
resolve to the wrong 64 KB and the machine dies instantly. Required now for v2
files, loudly warned for v1. Relatedly, section order comes from the FILE and
was never constrained, so a file putting `CPU` last applied the clock jump
*after* the beam-race log was cleaned up — the log then published empty every
frame for the whole rewound span while its stale tail grew without bound. The
invalidation moved to the end of the restore, where order cannot matter.

**A planted symlink at `<target>.tmp` redirected any write-back.** Callers vet
the target — the AI control server refuses paths outside the working
directory, refuses symlinks, demands a `.pom2snap` extension — but the temp
path is derived afterwards and inherited none of it, while `ofstream(trunc)`
follows symlinks. The write landed on the victim and the rename then moved the
symlink away, leaving no trace. `AtomicFileReplace.h` now owns a
`prepareTempPath()` that clears a symlink and refuses anything else
non-regular, used by all five write-back paths.

Smaller, same sweep: a single aborted connection (any local process can make
one) permanently retired the accept loop of both the AI control server and the
SSC telnet bridge, while `isRunning()` kept saying yes — the shared
`pollAcceptOnce` now distinguishes a dead listener from a transient failure and
backs off on resource exhaustion. `jsonGetInt` accepted a partial parse, so
`{"cycles_per_frame":2.5e6}` — legal JSON — became 2, passed the range check
and set the machine to ~120 cycles per second while answering 200 OK. `/eject`
ignored its documented `slot` field and always ejected slot 6, flushing and
dropping the wrong medium. A same-origin GET carries no `Origin` header, so the
token-less default was readable by any DNS-rebound page; the `Host` header is
checked now. A 3.5" WOZ had none of the size caps its 5.25" sibling carries — a
15 MB file froze the emulator for 31 s at 294 MB, with the state mutex held.
The MacBinary sniff is four bytes wide and false-positived on ordinary disk
images, making them unloadable with a misleading diagnostic. And re-enabling
the rewind ring spliced two disjoint timelines together.

The whole suite also runs clean under ASan+UBSan, which is how the previous
sweep's TNFS overflow was caught; this time it found nothing new, which is
worth knowing.

## 2026-08-22 — Bug hunt: what the network code does when the far end lies

A sweep over the ~1 400 lines of FujiNet/TNFS code written the day before,
none of which had been read cold. Thirteen defects, all verified against a
stub built to reproduce them rather than by reading alone. The theme is one
mistake repeated: **trusting the far end**, whether that end is a public TNFS
server, a FujiNet peer process, or an HTTP host — and doing it on the CPU
thread, which holds the emulator's state mutex.

**A TNFS server could write past the end of the caller's buffer.** `readAt`
took the byte count from the reply and `memcpy`'d that many bytes into the
caller's block, checking only that the reply really contained them — never
that they fit what was asked for. A server answering an 8-byte request with
525 bytes wrote 517 attacker-chosen bytes past the end (confirmed under ASan).
`TnfsClient` exists to mount `tnfs.fujinet.online`, so the far end is nobody
we audit. Now the count is bounded by the request, and `tnfs_client_hostile`
holds a canary past the buffer to prove nothing lands there.

**A wrong sequence byte was an infinite loop and a packet storm.** The UDP
straggler drop did `--attempt; continue;`, which the loop's `++attempt` undid
immediately — so a server answering every request with the wrong sequence kept
the loop alive for ever *and* re-sent on every pass: 446 000 requests in 6 s
against a 0.6 s budget. It now drains a bounded number of stragglers per
attempt. Measured after: 2 requests.

Also in TNFS: a `STAT` guard that asked for 9 bytes to protect a field ending
at byte 11 (so a short reply read off the end and returned a size built from
adjacent heap); a `READDIR` loop with no cap, where a server that always
returns one more name grew the heap until it ran out; a listing cut short by a
dead connection reported as a *complete* listing; and no check that a reply
answered the command that was sent — which over TCP desynchronises the stream
permanently, since the framing keys on that byte.

**A blackholed host froze the whole emulator for 75 seconds.** The built-in
`N:` set `SO_SNDTIMEO` and a comment claiming the fetch was bounded. It is
not: `SO_SNDTIMEO` does not bound `connect()`. Measured on macOS against
192.0.2.1 (TEST-NET-1, swallows SYNs): 75 s for an 8 s request. And a
per-recv timeout never bounds a *transfer* — a server drip-feeding one byte
just inside the timeout held it open indefinitely. All of it runs on the CPU
thread inside `runCpuSlice`, under `stateMtx`, which the UI thread needs to
paint anything: the window was unpaintable and the panel's own buttons out of
reach. The whole exchange now shares one deadline, DNS included — `getaddrinfo`
is unbounded too, and there is no portable async resolver, so the lookup runs
on its own thread and is abandoned if it overruns.

**A short read is not a short page.** That fetch used to hand the guest
whatever bytes had arrived and report success. A truncated document the guest
cannot distinguish from a whole one is the one failure nobody can diagnose from
the Apple II side, so it is an error now.

**A peer that was merely slow was declared dead.** The device sweep treated
"no answer within 250 ms" as "the peer is gone" for every unit — and tearing
the connection down skipped the publish at the end, discarding every device
already enumerated. The worker then reconnected, hit the same slow unit and
dropped again: a livelock in which the panel and the guest both saw *zero*
devices for ever, and in which the serial transport reopened the CDC device
several times a second, driving the ESP32's auto-reset line. Raising
`kMaxUnits` 8 → 32 the day before had made it far easier to hit. This is the
"the FujiNet dies easily" complaint, and it is now: retry any unit, and if
units already answered, keep them and stop the sweep.

**A peer that stopped reading hung POM2 unrecoverably.** The accepted TCP
socket was left blocking, so `writeAll`'s wait-for-room branch was dead code
that could never run. A peer alive but not draining parked `send()` for ever
with `callMtx_` *and* the state mutex held — UI blocked, Stop button
unreachable, kill-only. The socket is non-blocking now and one deadline covers
the whole packet.

Smaller, same sweep: the built-in `N:` sat at a fixed unit 11 while a
SmartPort chain is contiguous 1..N, so a standard scan stopped at unit 1 and
never found it (it now takes the slot past the peer's last device, and never
shadows a unit the peer really has); its READ consumed the body *before*
validating the guest address, so a refused write silently ate a chunk and the
retry got the *next* one; its DIB claimed to be a block device and its
general-status call answered with the network status, whose first byte reads
as "offline"; and the `$FFFF`-wrap guard the relay path has was missing.

**One wire, two answers.** Write-protect on a Disk II is a single signal, and
POM2 offered two ways to read it: the canonical
`LDA $C08D,X / LDA $C08E,X / BMI` sequence, which reported an empty drive as
protected, and POM2's own shortcut at `$C0nD`, which reported it as writable.
What a guest was told depended on which idiom it happened to use. Protected is
the right answer, and not only because the other probe says so — the sense is a
phototransistor watching the write-enable notch, and with no disk in the way
the light reaches it, which *is* the protected state. Both sites agree now,
pinned by `diskii_empty_drive` on both gates, empty and loaded. (The loaded
half of that check needs write-back ON: without it `isWriteProtected()` is true
for every image and the test would be exercising the toggle, not the probes.)

`test_fujinet_card` had not linked since the built-in `N:` landed — ctest was
running a stale binary and reporting a pass. Two new pinned suites:
`tnfs_client_hostile` (a server that lies) and, in `fujinet_net_device`, a
stalled server and a blackholed host.

## 2026-08-21 (later) — The app icon, redrawn, and generated from one source

The macOS icon was ugly, and three of the reasons were structural rather than
a matter of taste.

**It was drawn with a font.** The old SVG set the "][" as `<text
font-family="monospace">`, so every renderer picked a different face: the
committed `.icns` and macOS QuickLook disagreed about the same file. The
brackets are now `<path>` outlines. Nothing in the icon uses text any more.

**Four hand-made binaries, no generator.** `POM2.svg`, six hicolor PNGs, the
`.icns` and the `.ico` were each maintained by hand and free to drift.
`tools/gen_icons.sh` now rasterises all of them from the SVG, which is the
only source. `packaging/windows/POM2.rc` pointed at "packaging/regen_icons
notes in the release documentation" — a file that never existed; it now points
at the script.

**The `.icns` stopped at 256 px** and carried non-standard 48/64 slots, so
Retina Finder and the Dock upscaled. It now carries the full modern iconset,
`@2x` included, up to 1024.

The drawing itself: a rounder apple with real shoulders and a stem dip, a soft
wide specular instead of the hard bubble, a leaf with a vein, and a badge
gradient with a hairline top rim rather than the glass band. The "][" bar
thickness is 14/256 — measured, not guessed: 11 breaks into dashes at 32 px,
17 reads as one block at 512.

**At 16 physical pixels no bar thickness survives** — the mark becomes bright
mush that swallows the apple. That one slot is rendered from the master with
the bracket group stripped, derived inside the generator rather than kept as a
second file, so the simplified art cannot drift from the master.

`tools/png2ico.py` assembles the `.ico`, because `magick a.png b.png … out.ico`
stores every entry as an uncompressed DIB: 25 KB became 370 KB, all of it
embedded into `POM2.exe` by `POM2.rc`. Entries are PNG throughout, which is
what the `.ico` this replaces already did (checked entry by entry first).

Regenerating needs `rsvg-convert` (`brew install librsvg`): ImageMagick's
built-in MSVG renderer silently drops `fill="url(#gradient)"` and would
flatten the badge and the apple to black.

## 2026-08-21 — An empty drive froze the machine: the Ultima V "save" hang was never a WOZ-write bug

**Symptom**: Ultima V's *Save Music Configuration* wedged POM2 solid — no
error, no timeout, a dead machine. It looked like WOZ write-back failing,
because it only happened on `.woz`.

**It was not.** WOZ write-back works; the guest-side round-trip is now pinned
twice over (below). What actually happened is three separate things stacked:

1. **Ultima V saves to the BRITANNIA disk (disk 2 of 8), in DRIVE 2.** Not to
   the Program disk it booted from. With drive 2 empty the game polls the
   empty drive at `$D407` — the universal nibble wait, `LDA $C08C,X / BPL -3`,
   followed by its own 16-bit timeout at `$79/$7A`.
2. **`DiskIICard::lssSync` froze `lssData` for an unloaded drive** (`if
   (!img.isLoaded()) { lssCycle = …; return; }`). A data register that never
   changes keeps bit 7 clear forever, so the `BPL` never falls through and the
   guest never even *reaches* its timeout counter. Hang, not error.
   The legacy 32-cycle nibble gate never had the bug — `deviceSelectRead`
   returns `$FF` for an empty drive — which is exactly why the same game
   errors out cleanly from a `.dsk` and froze from a `.woz`. The asymmetry is
   what made this read as "WOZ writing is broken".
3. **The Program disk dump is physically write-protected** (WOZ `INFO+2` = 1,
   Applesauce v1.0.6), so POM2 mounts it read-only — correct, and a red
   herring. The BRITANNIA dump has `INFO+2` = 0: it is the disk meant to be
   written.

**The fix**: an empty drive now delivers *noise*, as the real read amplifier
does — one pseudo-random byte per 8 bit cells (4 µs each = 64 LSS cycles),
high bit set as on every byte the LSS ever hands the CPU. Bit 7 comes up, the
loop exits, RWTS times out into a clean I/O error. The byte is derived from
the LSS cycle cursor by hash rather than from a PRNG member, so it stays
deterministic across snapshot restore and rewind — both replay that cursor.
Ultima V now shows *"Please insert BRITANNIA disk:"* and stays responsive.

Note the branch is reachable at all because `$C0n9` sets `active =
MODE_ACTIVE` unconditionally while gating only `lssStart()` on media — the
motor spins whether or not there is a disk under it, which is also what the
drive does.

**Pinned** by `tests/diskii_empty_drive_test.cpp`: five cases (bit-LSS ×
empty drive 1 / empty drive 2 / both empty, plus the two legacy-gate cases as
a regression guard), each running the real `LDA $C0EC / BPL` loop as 6502
code and asserting it exits. Verified to fail on the three bit-LSS cases with
the fix reverted.

**WOZ write-back, proven end to end** while chasing this:

- Ultima V writes its music configuration to the BRITANNIA `.woz` (346 write
  flushes, one quarter-track spliced back, header CRC32 zeroed per the
  Applesauce 2.1 "not computed by the imager" sentinel), and a *fresh boot*
  reads the setting back — slot 4 = Mockingboard C.
- `dos33_save_smoke` now takes the image as `argv[1]`, so the same guest-side
  SAVE → LOAD → LIST round-trip runs against any 5.25" container; a WOZ1
  built from `dos33_master.dsk` passes it.

**Also fixed**: `dos33_save_smoke` looked for `disks_5.4/dos33_master.dsk`,
but the image lives in `disks_5.4/dsk/`. The test had been silently SKIPping.

**Diagnostic harness**: `tests/u5_woz_save_probe.cpp` (built, not in ctest —
it needs the game disk) boots a disk on a //e with a Mockingboard in slot 4,
drives the guest through a key script (`esc cr down up left right space
sleepN`, plus `shot` to dump both HGR pages and `dumpXXXX` to dump memory +
CPU + disk state), and reports write flushes per keystroke. That is how the
`drive=1` in the disk state was spotted — the single fact that unlocked this.

## 2026-08-20 (evening) — Second performance campaign, and the bench's //e was a BRK loop

Full write-up with the profiles, the reasoning and the numbers:
`docs/PERFORMANCE.md` § 7. The short version, most recent host Apple M1,
release build, output hashes identical throughout, `ctest` green (186):
][+ banner **−19 %**, 5.25" boot **−26 %**, //e banner **−35 %**, //e PAL
no-render **−37 %**, a game running in HGR **−29 %** (profiling build; the
release build runs the same 20 000 frames in 1.96 s).

**The finding that matters beyond the numbers**: `pom2_bench --iie` called
`loadAppleIIRom()` before `setIIEMode(true)`, the loader only maps the
internal `$C100-$CFFF` ROM when the mode is already on, and the //e booted
into an empty `$C300` and executed `BRK` forever. Every "//e" measurement in
the previous campaign, and three of the PGO training runs, profiled a BRK
loop. Fixed, and `pom2_bench --dump-text` now shows text page 1 + the PC so
a workload can be eyeballed before it is trusted.

The changes: `memRead()` covers the //e internal ROM inline (the //e
executes its keyboard loop from there — the § 3.2 "ROM window" trap one
machine later); `memWrite()` gets the same inline/slow split with a shared
`iieWriteToAux()`; `advanceCycles()` runs its VBL/frame body only at the next
event cycle instead of per instruction (and loses a second per-instruction
64-bit division); the keyboard latch is mirrored into an atomic so `$C000`
reads no longer take `kbMutex` — and non-keyboard soft switches never did
need it; `renderHiRes` precomputes its phase/palette/average tables and
caches each row's decode (input → output, so always safe to reuse). A second
pass hoisted the opt-in trace switches out of function-local statics (each
cost a guard check per instruction) and gave `DiskIICard::advanceCycles` an
idle early-out; caching `memRead`'s ROM-window condition in one bool was
tried, measured 4 % slower, and dropped.

**The pitfall**: the //e read fast path shipped without an `addr < 0xD000`
bound, so language-card RAM reads took the internal-ROM shortcut. Both bench
hashes stayed identical — no bench workload maps LC RAM on a //e — and
`softcard_cpm_boot_iie` caught it. New `bus_fastpath` test: differential
`memRead`/`memWrite` vs the slow paths over every address × 1024 paging
states, with all four RAM banks seeded distinctly (its first version, with
zeroed banks, let the bug through too) and checked to fail on the bug before
being kept.

## 2026-08-20 (later still) — Bug hunt 3: no defects, two fuzzers landed

A third pass found **nothing**, and the tooling that failed to find it is the
deliverable. Recorded because a negative result is only worth anything if you
can say what it ruled out.

What was run: **72 000 mutated files** through the image and snapshot parsers
under ASan+UBSan; **ThreadSanitizer** over every test that touches threading,
including the one that stands up a real `EmulationController` worker plus an
HTTP server (0 races); and four invariants checked by hand — the kiosk
settings promise, the //c PAL built-in Chat Mauve vs `noPhysicalSlots`, the
command palette's id dispatch, and the Voxel3D render targets. All correct.
Two were already-fixed bugs whose comments document the exact failure being
looked for.

The pattern across three rounds is worth stating: every real defect found (six,
in rounds 1 and 2) was **semantic**, in the UI/host layer, and invisible to
every tool used here — a panel clobbering a shared latch, a warning that could
never fire, a control that silently no-opped. Three rounds of tooling aimed at
the emulation core found nothing, because the core is in good shape.

### The fuzzers are now ctest targets — and the first version was useless

`fuzz_disk_image` (~2.0 s) and `fuzz_snapshot` (~0.1 s), both bounded and
deterministic. Two things nearly made them worthless, and both are the kind of
mistake that leaves a green test protecting nothing:

**They must synthesise their seeds.** The throwaway versions read
`disks_5.4/woz` — 719 real images, an excellent corpus, and not one byte of it
tracked by git. A corpus-reading fuzzer finds zero seeds on a fresh clone or in
CI and passes for the worst possible reason. Both harnesses now build their own
WOZ2 (5.25" and 3.5"), 2IMG, bare DSK/PO, NIB and HDV containers.

**Blind mutation cannot find a parser bug.** The first version scattered
byte-flips over the header region and, tested against a deliberately removed
bounds check in `Disk35Image::loadWoz`, did not catch it in 600 rounds — the
fields that matter are four bytes each in a 250 KB file, so random flipping
essentially never lands on one. Both mutators now parse the container and aim
at the numbers the loader trusts: WOZ chunk lengths, TMAP track indices and the
TRKS start/count/bit-count triple; snapshot section lengths and names. The same
sabotage is then caught in under a second.

Both are verified by sabotage, which is the only evidence that means anything
for a fuzzer: removing `loadWoz`'s payload bounds check surfaces as an ASan
fault in `cellsFromPackedBits`; disabling `Memory::loadSnapshotState`'s
`need()` check surfaces as a heap-buffer-overflow. One useful thing learned on
the way: `SnapshotReader` cannot over-read *at all*, because every read goes
through an istream over a bounded streambuf — its guards are there to stop
unbounded allocation, and the genuine raw-pointer parser is
`Memory::loadSnapshotState` behind the MEX section. Two sabotage attempts
failed to produce anything observable before that became clear, which is why
it is written down.

### One nit, not fixed

`MainWindow.cpp:3313` labels the checkable menu item
`MenuItem("Rewind (time-travel)", "F6", &showRewindBar)`. The checkbox toggles
the *bar*; F6 holds *rewind*. On a checkable item the shortcut hint implies the
key toggles the check. Left alone — the wording is a matter of taste, not a
defect.

## 2026-08-20 (later) — Bug hunt 2: the media layer

A second pass, widened past the UI commit. Three things were run at the whole
tree rather than at a diff, and the negative results are worth recording:

- **ASan + UBSan over the entire suite** — all 179 test binaries plus the six
  heavy CPU vector suites (Klaus 6502/65C02, TomHarte, zexdoc, zexall) pass
  clean. No out-of-bounds, no UB, no leaks reported.
- **A stricter warning pass** than the build's `-Wall -Wextra -Wshadow`, adding
  `-Wunreachable-code-aggressive`, `-Wconditional-uninitialized`,
  `-Wloop-analysis`, `-Wcomma`, `-Wshadow-all` and friends across all 320 of
  our own translation units — `src/` **and** the 203 files in `tests/`. It
  found the dead code below plus one shadowed lambda parameter in
  `ai_control_server_smoke_test.cpp` (a lambda taking `body` inside a scope
  that already had a `body`, which made it read as if it reused the outer
  one). Both trees are now clean under the whole set; the only warnings left
  anywhere are in vendored `third_party/`.
- **A lock-discipline scan** for `stateMutex` taken recursively — every
  self-locking `EmulationController` and `MainWindow` method checked against
  every scope that already holds the lock. Nothing. The non-recursive mutex is
  being respected.

The conclusion mirrors the first pass: the emulation core is in good shape, and
what defects remain live in the host layer that no test reaches.

### The eject warning was dead for every bay except a Disk II

The status bar's eject menu warns "Unsaved changes — ejecting writes them back
if write-back is on for this drive, and drops them if it is not". It could
never fire for an HDV, a CFFA or a SmartPort 3.5" unit: those rows are built
from `MountableMediaCard::bayInfo`, and the status bar hardcoded `dirty=false`
for all of them because `MediaBayInfo` had no field to carry it.

That is exactly backwards from where the warning is needed. A Disk II at least
has a panel of its own; a SmartPort 3.5" unit with write-back off silently
drops every guest write at eject, and the one place that promised to say so
said nothing. The data was never missing — `Disk35Image::hasUnsavedChanges()`
and `ProDOSBlockCard::hasUnsavedChanges()` both exist and are already shown in
the 3.5" and HDV panels — it simply had no route to the bay abstraction.
`MediaBayInfo` now carries `hasUnsavedChanges`, `SmartPortUnit` gained the
accessor its two subclasses could already answer, and both `bayInfo`
implementations fill it in.

### Undefined behaviour in the WOZ track walker

`Disk35Image::loadWoz` closed the track circle with

    cells.insert(cells.end(), cells.begin(), cells.begin() + n);

— an insert whose *input range points into the very vector being resized*,
which [sequence.reqmts] leaves undefined. It works on today's libstdc++ and
libc++ because the reallocation path copies out of the still-live old buffer,
but that is an implementation detail rather than a guarantee, and the
allocation always reallocates here (the vector was sized exactly `once`). Now a
`reserve` plus an index-based append, which is defined and also avoids the
double copy.

### Dead code in the Sony GCR decoder

Three findings, all confirmed by the compiler once the right flags were on:

- `sony35::decodeSectors` ended with `return written;` **twice**.
- The same function carried a `decodeOk` flag that nothing ever cleared, so
  both its loop guard and its `if (!decodeOk) continue;` were dead. It read as
  if invalid GCR were rejected there. It is not — `gcr6Decode` maps all 256
  nibbles, and a corrupt group is caught by the running checksum and the DE AA
  epilogue instead. Said so in a comment rather than leaving the flag.
- `Sony35Drive.cpp` held a private duplicate of the read-side `kGcr6bw` table
  **and** of `gcr6Decode`, both entirely unused — that file needs only the
  write side. A dead duplicate of a MAME-cited routine is worse than none: it
  is the copy nobody remembers to correct when the original is.

### Abstraction Levels under-reported the network transport

`netbackend` was the only entry in its group that pushed no live state, so it
fell through to `NotApplicable` — "always present, no plug state to report".
libslirp is an optional build dependency: without it `SlirpNetworkBackend` is a
stub that always fails, Uthernet I has no transport at all, and Uthernet II is
confined to its own W5100 stack. A panel whose stated purpose is catching
silently-degraded subsystems should not have had that particular blind spot.
It now reports the `POM2_HAVE_SLIRP` state and says what is lost without it.

## 2026-08-20 — Bug hunt over the new UI panels

Four defects found by review of `1afe203`; the build was clean and 182/182
tests passed throughout, because every one of them lives in a UI path no test
reaches. That is the lesson as much as the fixes are.

### The keyboard panel disabled Open-Apple / Solid-Apple for the whole session

`$C061`/`$C062` bit 7 is one wire, and POM2 grew a **second** thing pressing it
when the clickable //e keyboard landed: the host's Left/Right Alt (`onKey`) and
the panel's own latches. Both *assigned* the shared latch. The panel
republishes every frame — the Apple keys are levels, not events — so it stamped
whatever Alt had just set back to its own value 60x/s. Worse, `keyboardPanel`
is never destroyed once built, so the panel's **closed** branch went on calling
`setOpenAppleKey(false)` for the rest of the session.

Net effect: **open Devices → Apple //e Keyboard once, close it, and Left/Right
Alt are dead until you restart.** Open-Apple+Ctrl+Reset stopped cold-booting;
every //e title reading bit 7 as button 0/1 stopped seeing the keys. And it
looked like an *emulator* fault rather than a UI one, because
`Memory::memRead` ORs `paddleButton[]` into the same case (`Memory.cpp:1612`) —
so a real joystick kept working the whole time. Persisted `show_keyboard` meant
a user who quit with the window open was broken from the first frame of the
next launch.

Fixed structurally rather than by ordering: `AppleKeyLatch.h` holds the two
sources apart and ORs them at the point of use, so no writer *can* express
"…and release the other one". The close-time release is edge-triggered on
`kbPanelWasOpen_` and now drops only the panel's half. Pinned by
`apple_key_latch` — which fails on the old assignment, verified by
re-introducing it.

### Filesystem stats under `stateMutex`, 60x a second

`renderDisk35PanelWindow` called `freePoNameFor()` **inside** the state lock,
once per WOZ drive per frame, and that helper stats the filesystem up to 99
times. Blocking I/O on the lock the CPU worker needs is the exact shape of the
`disk_turbo` UI freeze we already paid for once. The answer only changes when
the medium changes, so it now resolves outside the lock, memoised on the source
path. Display only: `convertWoz35ToPo` re-resolves the name at conversion time,
so a stale memo can never misdirect a write.

### Abstraction Levels: two dead controls

With neither side of a switchable boundary plugged (`selected == -1`), the
radio pair stayed enabled; clicking it called `swapSlotCardVariant`, which
found no slot holding either key and returned `false` **without a word**. The
pair is now disabled, with the "add one in Slot Configuration first" note as
the answer. The `restartEmulationFromSettings` failure path was equally silent
— the radio just snapped back — and now says why.

Separately, the `blockedBy` tooltips ("Unavailable — roms/… is needed") could
never be read: `IsItemHovered()` reports false for a disabled item, and
explaining the greying is the entire point of those two strings. They now pass
`ImGuiHoveredFlags_AllowWhenDisabled`.

### Status-bar media chips lit up through their own popup

The hover tint came from `IsMouseHoveringRect`, which is clip-rect aware but
not **z-order** aware, while the click went through `IsItemClicked`, which is —
so a chip highlighted as interactive underneath anything drawn over it, its own
eject menu included. The chip is now a real item (an `InvisibleButton` the size
of the text, painted through the draw list), so one item answers hover, tooltip
and click alike.

## 2026-08-19 (later, 8) — A 3.5" WOZ can be converted to a writable .po

**Reported as "why can't I save to a WOZ?", from a real case: The New Print
Shop on an 800K WOZ, which keeps its printer configuration — the colour ribbon
setup — on its own program disk.** The diagnosis is worth writing down because
the obvious suspects were both innocent: that image's INFO chunk says
`write_protected: 0` and it carries no FLUX tracks, so neither of the two
5.25"-side reasons applied. It is read-only because it is a **3.5"** WOZ, and
POM2 has the Sony GCR *decoder* but no encoder — a `.woz` at 800K is decoded to
blocks once at load and has nothing for guest writes to be folded back into.

So the panel now offers the way out it always could have: **Convert to writable
`.po`**. `Disk35Image::exportRawTo` writes the 1600 decoded blocks beside the
WOZ (temp file + atomic rename + fsync, refusing to overwrite and
auto-numbering ` (2).po`), and `MainWindow::convertWoz35ToPo` mounts the copy in
the same drive **with write-back already on**. Both halves matter: leaving the
WOZ mounted would mean the user converts, sees nothing change and still cannot
save, and mounting the copy with write-back off would refuse the writes for a
second reason — which is exactly the confusion the feature exists to end. The
`.woz` is never touched: it stays the archival master, which is the right split
anyway, since a program saving its configuration has no business re-mastering
flux.

The panel also now **names the reason** on any mounted 3.5" WOZ instead of
showing a bare "(write-protected)". A user who ticks write-back and gets
refused has no way to tell which of several write-protect rules caught them —
and that is how "why can't I save?" starts.

Verified against the actual image: 1600/1600 blocks byte-identical between the
WOZ decode and the reloaded `.po`, valid ProDOS volume `NPS`, and a write to
the copy survives save + reload.

## 2026-08-19 (later, 7) — Eject from the status bar

The mounted-media chips at the bottom of the window are controls now, not
labels: click one and it offers to eject that bay. They brighten under the
pointer, because a status bar reads as read-only furniture until something
reacts to the cursor.

**A menu, not eject-on-click.** The bar is a dense strip of small targets
directly under the emulated screen, and a stray click would pull a disk out
from under a running program. The extra click also buys room to name the bay
and to warn when the medium has unsaved changes.

`ejectMediaBay(slot, index, diskII)` addresses the bay by **slot number**, not
by a card pointer: the status bar builds its rows as a value snapshot taken
under `stateMutex` and released before drawing, so a pointer captured then
could belong to a card a Slot-Config Apply has since destroyed. It re-resolves
through the SlotBus, clears whichever settings key would otherwise remount the
image on the next launch (`disk_path_slotN`, `smartport_slotN_unitK_path`,
`hdv_path`, `cffa_slotN_path` — the SmartPort one is written eagerly at mount,
so an eject that skipped it would silently resurrect the image), and leaves the
medium **mounted** when its write-back save fails, rather than losing the
writes.

## 2026-08-19 (later, 6) — Slot Config Apply cold-boots instead of hard-resetting

**Reported as "Apply doesn't really reset the machine", and it didn't.**
`restartEmulationFromSettings` ended on `controller->hardReset()`, and
hardReset **preserves RAM** by design (CLAUDE.md § Reset architecture: RAM
survives, registers are wiped, `resetSoftSwitchesWarm` applies the
profile-appropriate soft-switch policy). So the card set changed underneath a
memory image built for the *old* one: DOS 3.3 still hooked to a slot whose
Disk II had just been unplugged, ProDOS still holding a device table for cards
that no longer existed, a player still poking a Mockingboard that was gone —
and on II/II+ even the display mode and Language Card banks survived, because
the warm path deliberately leaves those alone.

Now `controller->coldBoot()`: `clearRam()` with the MAME 00/FF pattern, the
FULL `resetSoftSwitches()`, a hard CPU reset. Which is simply what the real
event is — you open the lid, change cards, and power back on. It also matches
what `applyProfile` has always done for a profile switch (step 4 wipes RAM,
step 11 hard-resets the CPU); Apply is the same event, one rebuild smaller.

**The three-verb split itself checks out.** `EmulationController` implements
exactly what CLAUDE.md documents — `softReset` (RAM + registers survive,
SP -= 3, warm soft switches), `hardReset` (RAM survives, registers wiped, warm
soft switches), `coldBoot` (RAM wiped, full soft switches, rewind ring
dropped) — and F11/F12/toolbar-power route to the right one. The bug was one
call site picking the wrong verb, not a broken distinction. The Apply button
now says "cold-boots the machine" and its tooltip says RAM is wiped, because a
button that power-cycles your machine should say so before you click it.

## 2026-08-19 (later, 5) — Two new windows: Abstraction Levels, and a clickable //e keyboard

**Abstraction Levels (LLE / HLE)** — `Help → Abstraction Levels`.
`docs/lle_vs_hle.md`'s master table, live. Two things the document cannot do:

- **It says which level is running right now.** Every ROM-driven low level in
  POM2 degrades *silently* to a working higher one when its dump is absent —
  Disk II to the legacy 32-cycle nibble gate, the Mouse Card from an executing
  M68705 mask ROM to a C++ state machine, ClockCard and Grappler+ to synthetic
  ROMs. The machine still works, which is correct product behaviour and
  exactly why nobody notices. The doc named this a structural hole and
  proposed reporting *degraded* rather than merely *missing*; the "Now" column
  is that report, sourced from the card ROM-state accessors (one of which,
  `DiskIICard::usingBitLss`, was added for it — `hasLssRom()` alone lies,
  because a mounted WOZ forces the bit-level path with no dump on disk).
- **It lets you move the boundary.** The four subsystems that ship both levels
  are presented as a choice of *level*, not of catalog key — you no longer have
  to know that `mouseaw` is the HLE one. A side whose dump is missing is greyed
  out: offering a switch that would silently land on the fallback would repeat
  the exact mistake the panel exists to expose.

**Apple //e Keyboard** — `Devices → Apple //e Keyboard`. A photo of the real
keyboard with one hotspot per cap, so Open-Apple, Solid-Apple and the //e's own
Reset are reachable with the real legends on them.

The hotspots are **measured off the photo**, not drawn:
`tools/gen_keyboard_layout.py` takes a 75th-percentile column profile through
each key row and cuts at the dark valleys between caps. The percentile is the
whole trick — this is a European //e whose caps carry two legends (French over
US), and under a *median* the glyphs put enough dark pixels mid-cap to split
one key into two. Rects are stored as fractions of the 2578x908 image, so they
track the picture at any window size; `Show hitboxes` is the visual check, and
it is how the Reset key was caught sitting in its own recess **lower** than the
row-1 caps and given an absolute rect instead of the row band.

Two fidelity decisions worth recording. **Reset refuses to fire without
Control** — RESET is wired through the keyboard encoder's Ctrl line on every
Apple II precisely so a knock cannot reboot the machine, so the panel is no
more dangerous than the hardware; Ctrl+Reset warm-resets, Open-Apple+Ctrl+Reset
cold-boots. And the photo draws **both** horizontal arrow caps pointing left,
which is an error in the picture: the table follows the hardware.

The Apple keys are levels, not events — pushed to `$C061`/`$C062` every frame
while latched, and released when the window closes, so a latched Open-Apple
cannot outlive the window that shows it as down.

## 2026-08-19 (later, 4) — Status bar says how to capture the mouse

When the pointer is over the emulated screen, a Mouse Card is plugged and
nothing is captured, the status bar now spells out `Ctrl+Alt+G or middle click
to capture`. That is the exact moment the user is about to wonder why the guest
cursor will not follow theirs — the card is a relative device, so uncaptured
the host pointer stops at the edge of the screen widget while the guest cursor
still has clamp window left, and the two drift apart.

In the status bar rather than on the screen: the on-screen captions were
removed for being noise over a running game, and this is the same information
in a place that is already a status surface. Gated on `screenHovered_` (ImGui's
z-order-aware verdict, so a menu drawn over the screen suppresses it) and on a
card actually being plugged — `shouldToggleGrab` refuses to capture without
one, and advertising a shortcut that does nothing is worse than silence.

## 2026-08-19 (later, 3) — Leaving kiosk releases the mouse

`setKioskModeRuntime` now calls `setMouseGrab(false)` on the way out, before
it touches the window. Kiosk is the mode where a captured pointer costs
nothing — there is no UI to click — and the windowed GUI is the mode where it
costs the user their menus, panels and docked tabs; coming back to a full UI
you cannot click is the worst of the two states. Doing it before the monitor
change also keeps a `GLFW_CURSOR_DISABLED` pointer out of the full-screen →
windowed transition, which the OS re-warps it across.

**Entering** kiosk deliberately does not touch the grab: a game in full screen
is exactly what a captured mouse is for.

## 2026-08-19 (later, 2) — New default machine: //e Enhanced PAL, loaded

**A fresh install now boots an Apple //e Enhanced PAL** with Composite
(OpenEmulator) video and seven slots that mean something, instead of a bare
Apple ][+ with an SSC, a clock card and an HDV:

| sl1 | sl2 | sl3 | sl4 | sl5 | sl6 | sl7 |
|---|---|---|---|---|---|---|
| `grappler` | `mouseaw` | *empty* | `mockingboard` | `smartport35` | `diskii` | `chatmauve` |

**Slot 3 stays empty on purpose**, and it is the one entry worth spelling out:
the //e's 80-column card is *not* a slot card in POM2 or on real hardware. The
80-col firmware is internal ROM at `$C300` and the Extended 80-Column Text
Card lives on the AUX connector — both arrive with `iieMode`, which the //e
profiles set. Putting a card in slot 3 would also fight the SLOTC3ROM switch.

**The default profile is expressed as a `getString` default, not as an
`activeProfile` initialiser** — the non-obvious part. `activeProfile` comes
from the ROM auto-probe, and the catch-up branch under it only calls
`applyProfile` when the resolved profile *differs* from that probe. Feeding
`"iie-pal"` in as the stand-in for a saved key makes it differ, so
`applyProfile` runs — and `applyProfile` is what installs the PAL video
standard, the 20313-cycle frame budget and the `setCpuClock` sweep across
every slot card. Assigning `activeProfile` directly would have skipped all of
it and left a //e running 60 Hz timing while the UI said PAL. Falls back to
the auto-probe when no //e ROM resolves; `--ii-plus` still wins.

Verified end-to-end rather than by inspection: a run under a scratch `$HOME`
with `POM2_AUTO_QUIT` logs `Profile: Active = Apple //e Enhanced PAL`, plugs
all six cards with no warning (the Grappler+ and Liron ROMs both resolve), and
writes exactly the intended keys to a virgin `state.cfg`.

**Display defaults to `ColorCompositeOE`.** It degrades safely — with no GL
shader `NtscPostProcessor` falls back to the NTSC LUT — so this can't strand
anyone on a black screen. Restoring the mode now also seeds
`lastColorHiResMode_`, or the first mono → colour round-trip on the toolbar
would have snapped back to that member's `ColorNTSC` initialiser instead of
the mode the user was looking at. Invisible while the default *was*
ColorNTSC; a bug the moment it wasn't.

One knock-on: slot 4 no longer defaults to `clock`, which makes the legacy
`clock_card_enable=false` opt-out in `plugSlotsFromSettings` inert unless the
settings file also names `clock` there. Left in place, commented.

## 2026-08-19 (later) — Ctrl+Alt+F toggles kiosk

**Second binding for GUI ⇄ full-screen (kiosk), alongside F10.** F10 is not
POM2's to claim on every desktop: GNOME and KDE both bind it to "open the
focused window's menu", so it is swallowed before GLFW ever sees it and the
documented way in and out of kiosk simply does nothing there. `Ctrl+Alt+F`
joins the same chord family as `Ctrl+Alt+G` (mouse capture) and is reachable
everywhere. F10 still works — removing it would break fingers and every
screenshot in the README.

Handled next to the mouse-grab chord, **above** every other branch in
`MainWindow::onKey`, for the two reasons that placement always encodes here:
leaving full screen must never be blocked by state (the in-kiosk menu gate
sits below it), and the chord must be tested before the `Ctrl-A..Z` path or
it would *also* inject Ctrl-F (`$06`) into the keyboard latch. PRESS only —
on `GLFW_REPEAT` a held chord would flip full-screen ⇄ windowed ~30×/s, each
flip doing a monitor change plus a synchronous `settings->save()`. Added to
`isGlobalKey` in `main.cpp` so it fires even while ImGui holds keyboard focus.

**Two stale F10 references fixed while in here.** The kiosk Start menu's
keyboard fallback has been **F1**, not F10, since the day it was written (F10
entering kiosk would have opened the menu in the same frame) — but the joystick
log line and DEV.md both still said F10.

## 2026-08-19 — CRT panel: presets removed, gentler default curve; new startup layout

**The look presets are gone.** The CRT Settings panel led with a
*Clean / Composite TV / Trinitron / Arcade* row, and each button overwrote the
whole glass block in one click — every slider below it jumped at once, with no
record of what had changed or what it had been. Reported as the panel being
confusing to use, and it is: a control whose effect is "eleven other controls
move" is hard to reason about. The sliders are the panel now, under an
`Advanced` header opened by default (with the presets removed, a collapsed
header would open on an empty window). `Reset to defaults` remains the one
way back to a known state.

**Default barrel is `0.02`, was `0.05`.** `NtscParams::barrel`
(`NtscPostProcessor.h`) — the shipped tube curvature was a visible warp on a
flat panel. `0.02` reads as a hint of glass instead. Existing `ntsc_barrel`
values in `settings.json` are untouched; this only moves what a fresh install
and `Reset to defaults` land on.

**New default dock layout.** `DockLayout::Reset` — which is also what a fresh
install seeds — now tabs **Disk Library / Slot Configuration / ImageWriter II**
to the right of the centred screen: what you mount, what the machine is made
of, what it prints. `showSlotConfigPanel` and `showImageWriterPanel` flipped to
`true` by default to match (docking a hidden panel places its tab but shows
nothing). Cassette Deck and Floppy Emu moved into the bottom-right inspector
group — still assigned to a node, so they never float over the screen, just
not part of the opening set.

## 2026-08-18 (later still, 5) — Read/write audit: every format, and one convention removed

Asked for a state of play: can every format POM2 mounts be written back?
The answer is in DEV.md § Storage as a table. Everything can, except two
formats that refuse **by design** — and one that was refusing for no reason.

**An 800 K `.dsk` or `.image` mounted read-only, and `.po` did not.** Same
819 200-byte bare ProDOS payload, same loader branch, same save path — the
only difference was the name, on the grounds that such dumps are "sometimes
read-only by convention".

That is the wrong layer for a convention. `fileWriteProtected_` is the
*physical* write tab: it outranks `setWriteBackEnabled`, so a user could
open Slot Config, tick write-back on an 800 K `.dsk`, get silently refused,
and be told nothing — while the byte-identical payload under a `.po` name
wrote fine. Writability now comes from the file: the host permission bit and
the 2IMG flag, both of which describe what the file actually *is*.

Nothing is loosened. Writes still need the `writeBackEnabled` opt-in, which
is off by default, and a host file that is genuinely writable — the test
asserts both, including that a `chmod -w` `.dsk` still mounts WP. It fails on
the old code at `"extension must not force WP"`, verified by reverting.

**Two other findings from the audit:**

- **`isWoz()`'s documentation said the opposite of the code.** It claimed
  WOZ images are "always reported write-protected for now (write-back to
  .woz is not yet implemented — incoming flux events get dropped)". Write-back
  landed a while ago: `saveDirty` splices the dirty quarter-tracks back into
  `wozRaw` and zeroes the header CRC32 per the Applesauce 2.1 "not computed
  by the imager" sentinel, and WOZ sits under the same gate as `.dsk`/`.nib`.
  The comment survived the change that falsified it. Corrected, with the one
  real exception spelled out: a WOZ carrying **FLUX** tracks is still forced
  physically WP, because POM2 cannot serialise delta streams and accepting
  the writes would report a successful save while discarding them.
- **CNib2 write-back was unpinned.** `disk_cnib2_smoke` covered the load path
  only. It works — verified by round trip — and now has a case, which matters
  more than it sounds: CNib2 is detected **by file size**, so if `saveDirty`
  failed to truncate the 6656-byte runtime pad back to the 6384-byte source
  width, the file would grow by 9 520 bytes and the *next* load would no
  longer recognise the format. The test asserts the size, not just the byte.

**The two remaining ❌ are refusals, not gaps**: 3.5" WOZ, and 5.25" WOZ with
FLUX tracks. Both would require re-encoding the user's flux to give blocks
back. Refusing at mount is the honest behaviour; accepting writes and dropping
them at flush is the failure that forecloses on the only copy.

## 2026-08-18 (later still, 4) — Slot 3 on a //e is a trap, and now it says so

Reported: *"why doesn't the mouse work with A2 Desktop?"* Because the mouse
card was in **slot 3** on a `//e PAL` profile, and on a //e slot 3 does not
exist as far as a card's `$Cs00` page is concerned.

With `SLOTC3ROM` off — the reset default, so where every machine starts — the
motherboard owns `$C300-$C3FF` outright and slot 3's I/O SELECT never asserts.
POM2 already models this exactly (`Memory.cpp`, pinned by
`iie_memory_smoke_test`), including refusing to forward *writes* to the slot
bus so a deselected card cannot latch the `$C800` window. The emulation was
never wrong. The **configuration** was, and nothing said so.

What that does to a GUI: software finds the mouse by scanning slots for the
Apple signature — `$Cn05=$38`, `$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$20`. Measured
with the same card and the same ROM in two slots:

```
slot 3 ($C300): 00 00 00 00  -> mouse signature NOT FOUND
slot 4 ($C400): 38 18 01 20  -> mouse signature FOUND
```

So A2DeskTop scans, finds the 80-column firmware where the signature should
be, concludes there is no mouse, and runs keyboard-only. MousePaint and
MultiScribe do the same. Real hardware behaves identically — which is why
Apple sold the mouse for slot 4 and why the //e manual reserves slot 3 for the
80-column card.

Slot Configuration now flags it inline, in red, on the offending row, seeded
from the live slot config so an existing setup is marked the moment the panel
opens rather than only after the user touches the combo.

**The warning is not mouse-specific, because the problem is not.** The panel
already warned about slot 3 for printer cards, but for a different and much
milder reason (they share the 80-column firmware's screen holes and wrap every
line — degraded, not dead). Checking what else lives in that window turned up
a worse case: **a Mockingboard addresses its VIAs through `$Cs00`**
(`MockingboardCard::slotRomRead`), so it is as silent in slot 3 as the mouse
is invisible. The SoftCard finds itself by toggling slot-ROM windows. So the
warning covers every card, with the mouse getting the specific "use slot 4"
wording and everything else getting the general one — and it deliberately says
that a card using only its `$C0nX` soft switches is still fine, because
claiming otherwise would be false.

Warned, not forbidden: a user who knows to flip `SLOTC3ROM` can still have it.

## 2026-08-18 (later still, 3) — A left click no longer takes your mouse

Three changes to pointer capture, all from the same complaint: it happened
when you did not ask for it, and it explained itself in the wrong place.

**A left click never captures.** The old contract was the classic one — click
the emulated screen and the guest gets your mouse — and it has a real cost
that is easy to miss when writing it and impossible to miss when using it. The
capturing press has to be **swallowed**: the guest cursor is wherever its
firmware left it, not under the host pointer, so forwarding the click would
fire the guest's button at an arbitrary spot (a stray dot in MousePaint, the
wrong pick in A2Desktop). So an ordinary click both disappeared *and* silently
changed what every later click meant. Left presses now always route to the
card by `shouldRouteButton`, and mean what they look like.

**Middle click toggles instead of only releasing.** It was release-only, on
the reasoning that "an escape hatch that can also arm the trap is a worse
escape hatch" — sound while a left click was the way in, and pointless once it
is not. Capture is now reachable by exactly two deliberate gestures, and each
is also the way out.

Which buys the property the rest of this entry rests on: **you can only get
captured by the same gesture that releases you.** Nobody arrives in the
captured state by accident, so nobody needs to be told the way out before they
get there.

`shouldToggleGrab` still refuses to *capture* with no card on the bus, and
under the 3D voxel view where middle-drag pans the camera (Ctrl+Alt+G still
works there — a chord cannot be confused with a drag). It never refuses to
*release*, whatever the state: an escape hatch that any condition can block is
not an escape hatch, and each refusal would strand a captured pointer with the
OS cursor hidden. The test asserts all four of those release cases one at a
time.

**Nothing is drawn on the emulated screen any more.** Both captions are gone —
"Click to capture the mouse" and the how-to-get-out reminder. They existed to
paper over click-to-grab: a click that silently changed the mouse had to
announce itself first, and a user captured by surprise had to be told the way
out. Neither problem survives the change above. The standing indicator is the
status bar's `GRAB` chip, and the way out is now **spelled out in full beside
it for 30 s** rather than 4 — the old budget was tuned for a caption painted
over the Apple II screen, where it had to get out of the way fast. In the bar
it costs only width, and the person who needs it is the one still working out
where their pointer went, who is not in a mood to go hovering things for a
tooltip.

**The `mouse_click_to_grab` setting is gone**, along with its View-menu item.
It gated the behaviour that no longer exists, so it could not change anything
— and a preference that does nothing is worse than no preference. Stale keys
in an existing `state.cfg` are simply never read.

Worth being precise about the test: "a left click never captures" is now a
**structural** property, not a behavioural one. There is no longer any
function in the policy that maps a left press to a capture, so the test pins
it by asserting the left button carries no toggle and that the same context
which used to capture now routes the press to the card. That is a weaker kind
of pin than a case that fails on the old code, and it is the strongest one
available once the path is deleted rather than disabled.

## 2026-08-18 (later still, 2) — Clicking a Floppy Emu image does something now

Reported: *"I can't boot disks by clicking in floppyemu."* Exactly right, and
the panel said so itself — selecting a 5.25" image set the status line to
*"Inserted X — reboot the Apple II to boot it."* The click mounted and stopped.

That is faithful to the hardware (a real Floppy Emu does not reboot your Apple
when you pick an image) and it is still the wrong behaviour here, because the
rest of POM2 has already made the opposite promise: the Disk Library's own
header says *"left-click = insert + boot"*. Two disk browsers a tab apart, one
booting and one not, is not a defensible distinction — it just reads as the
second one being broken.

Selecting an image now boots it. Routing stays **mode-driven**, which is the
part worth keeping: the Floppy Emu emulates whatever its mode says, so a `.2mg`
picked in Smartport mode boots from the SmartPort slot even though the same
file classified by extension would land elsewhere. Per mode: Disk II boots its
own slot (and parks the head at track 0 first, the step the library click
already did), 3.5" boots the SmartPort slot or cold-boots when the mount landed
on the //c+ on-board hub, and Smartport HD boots `routeMountHdv`'s slot — which
was already being computed into a local and then **dropped on the floor**.

**The SD card is also a Disk Library tab now**, after HDV. It lists the same
`floppyemu/` folder the OLED browses, so an image can be booted with one click
instead of walked to with PREV/NEXT/SELECT. The two disagree on purpose about
where an image goes: the OLED honours the device mode, a library click
classifies the file like every other tab and goes through `insertAndBootImage`
— the helper whose comment already invited "any future single-call boot entry
point", and which auto-plugs an HDV card when the config has none.

The tab accepts every extension the other three do **without their size
sniffing**: the sniff exists to route a file to the right bay, and an SD card
legitimately holds 5.25", 3.5" and Smartport images side by side because the
device emulates all of them. Its `*` marker is the union of the other tabs'
mounted paths — the SD card is not a bay of its own, so "mounted" can only mean
"currently in some drive". Note `floppyemu/` ships **only** in the WASM bundle
(`packaging/bundle.manifest`), so the tab is populated in the browser demo and
empty on a desktop package until the user makes the folder.

**And the macOS DMG step got a retry.** Two consecutive release runs failed on
macOS for two *different* transient reasons — first `curl: (28)` fetching GLFW,
then `hdiutil: create failed - Resource busy`, with the runner reporting an
orphaned `diskimages-help` process on its way out. Nothing about the inputs is
wrong when that happens. `hdiutil create` had one shot, so a race inside
someone else's daemon could sink an eight-package release; it now retries four
times with a backoff and fails loudly after that, because a genuine problem
(no space, a corrupt stage) must not be retried into a timeout.

## 2026-08-18 (later still) — F10 killed the browser build

Reported from the demo: pressing F10 to leave full screen made POM2 die with
*"POM2 failed while loading or initializing: glfwSetWindowMonitor not
implemented."*

`setKioskModeRuntime` drives full screen through `glfwSetWindowMonitor` — the
correct call on a desktop, and the only one that gets *exclusive* full screen.
In Emscripten's GLFW port that entry point is neither a no-op nor a stub that
returns failure: it is `abort('glfwSetWindowMonitor not implemented.')`
(upstream `src/lib/libglfw.js`), and `abort()` tears the whole module down. So
the toggle did not degrade, it **killed the running machine** — and because the
page reports any abort through its load handler, the message named a phase the
emulator was long past, which is why it reads like a startup failure.

Not a regression: the same four call sites are in v0.8.2, so every published
demo has had this. `MainWindow_Slots.cpp` was simply the one UI file in the
tree with no `__EMSCRIPTEN__` guard anywhere in it, and kiosk is the only
feature it owns that touches the window/monitor pair.

The fix reuses the fallback the function already had designed for a host with
no usable monitor — *"stay windowed but still enter the chrome-free path, the
user asked for it"*. On wasm that is not a degraded mode, it is the correct
one: a canvas already fills its page, and real full screen inside a page
belongs to the browser (F11, or `requestFullscreen`), not to the application.
So kiosk in the browser is chrome-free with the canvas untouched, both
entering and leaving.

**How it is verified is worth recording**: not by a test, but by absence.
Emscripten only emits a JS stub for a GLFW function something actually
references, so the built bundle is the oracle — `glfwSetWindowMonitor` appears
**once** in the shipped `wasm/POM2.js` (as the abort) before the fix and
**zero** times after. There is no reachable path left to test.

Swept for the same shape while there: of the 28 GLFW entry points Emscripten
`abort()`s on, POM2 references exactly two. The other, `glfwJoystickIsGamepad`,
sits inside `JoystickInput::poll`'s native-only `#else` branch (the browser
half uses `emscripten_get_gamepad_status`), so it never reaches a wasm build.
One landmine, now defused, and no others.

**Full screen also has a toolbar button now.** It was reachable by F10, the
View menu and the `view.kiosk` palette command — three routes, none of them
visible on screen. `ICON_FA_EXPAND` next to the screenshot and memory-grid
buttons, same wording as the View item, and routed through the same
`toggleKioskMode()` so the four ways in cannot drift apart. Deliberately not
drawn as a toggle: the toolbar does not exist in kiosk (`renderFrame`
early-outs above the menu bar), so an "on" state is unreachable and painting
one would be a lie.


## 2026-08-18 (later) — 3.5" WOZ images mount

A `.woz` holds bit CELLS; POM2 stores 3.5" media as a flat block array and has
no GCR encoder, so a flux dump had nothing to be mounted *as* — `Disk35Image`
took `.po`/`.2mg` only and the 5.25" WOZ loader rejects `INFO.disk_type = 2`.
That is what stopped *The New Print Shop 800K.woz* (Applesauce, the format 3.5"
preservation actually ships in) from being usable at all, and it had to be
converted offline before the colour test above could run.

**It mounts now**, because the hard part was already in the tree: the Sony 800K
GCR read path `Sony35Drive` uses to fold guest-written tracks back into the
image. It moved to `src/Sony35Gcr.{h,cpp}` unchanged — same tables, same
checksum walk, same MAME `flopimg.cpp:2107` lineage — and both callers now
share it: the drive decoding what the guest wrote, and `Disk35Image::loadWoz`
decoding a file at load. One copy, because a second transcription of that table
is a second thing to get quietly wrong.

Checked against the real disk: POM2's own load of `The New Print Shop 800K.woz`
is **byte-identical, all 1 600 blocks**, to an independent offline conversion —
and booting straight from the `.woz` prints the same colour test page,
`5 194` bytes and the same three ribbon bands.

Details worth keeping:

- **A WOZ mounts write-protected, always.** Giving blocks back would mean
  re-encoding the user's flux, which POM2 cannot do; accepting writes and
  dropping them at flush is the failure that forecloses. `setWriteBackEnabled`
  does not override it.
- **The track is a CIRCLE.** Walking it once loses the sector straddling the
  seam — about one per track-side, ~150 blocks on an 800K disk. The loader
  walks one revolution plus an overlap and de-duplicates.
- **Refusals say which drive they wanted.** A 5.25" WOZ gets "mount it as a
  Disk II image" rather than "not an 800K image"; a WOZ1 says so by name.
- **`classifyDiskForSlot` asks the file, not its size.** Flux file size is a
  property of the DUMP, so the old "any `.woz` → Disk II" rule sent an 800K
  3.5" image to the wrong drive. It now reads `INFO.disk_type`. The 3.5" bay,
  the Disk Library and the mount dialog all offer `.woz` too.
- Pinned by `woz35_load`, which builds a synthetic WOZ2 with its **own**
  encoder written from MAME's `build_mac_track_gcr` rather than reusing POM2's
  tables — a test that shared them could not catch a bad table — and asserts a
  byte-exact round trip across a zone boundary on both heads, the
  write-protection, and both refusals.
- `iic_printer_port`'s CTest budget went 60 s → 240 s: 6.9 s native but 56 s
  under ASan+UBSan, so a parallel sanitizer run tripped it while it passed
  alone. Same reasoning as `disk_writeflux_framing` earlier in this hunt.

## 2026-08-18 — The Print Shop printed nothing, and said it had

Asked to check that the ImageWriter II prints **in colour** with The Print
Shop. Driving the real disk (`disks_5.4/gist/PrintShop.dsk`) headlessly —
boot, Setup, printer = *Apple DMP/Imagewriter/Scribe*, interface = *Apple
Parallel Interface*, slot 1, then its own **PRESS RETURN TO TEST PRINTER** —
found something worse than a colour problem first.

**The synthetic `PrinterCard` captured nothing, and the guest was told it
worked.** Print Shop's parallel driver never touches our slot ROM: it writes
the character to the card's data latch at `$C0n0`, pulses the strobe by
writing the same byte to `$C0n2`, and polls `$C0n4` for ready. The card
decoded **only `$C0n1`** — the offset its own `PR#n` COUT trampoline uses —
so all 702 bytes of a real `ESC G` page went into the void while `$C0n4` kept
answering `0xFF`, and Print Shop advanced to "IF WELCOME MESSAGE WAS PRINTED,
THEN PRINTER TEST WAS SUCCESSFUL". A printer that reports success and prints
nothing is the worst of the available behaviours.

That gap is invisible to every existing test because `PR#n` + COUT — how
BASIC, DOS and the Monitor print — goes through offset 1 and works fine. It
only bites software that drives the interface DIRECTLY, which is what
graphics programs do.

The card now takes data on `$C0n0` as well: that is the data latch on the
real Apple Parallel Printer Interface it is modelled on, so accepting it costs
nothing. `$C0n2` stays ignored **on purpose** — the strobe carries a copy of
the byte, and taking it too would double every character. Same job after the
fix: **702 bytes spooled, page printed**, byte-identical to what the Grappler+
(which has a real ROM dump and was already fine) produces.

**And the colour answer: this Print Shop cannot print in colour, and the
emulator is not why.** It is the 1984 original, which predates the
ImageWriter II's colour ribbon by a year. Its Setup offers a single Apple
entry — "APPLE DMP, IMAGEWRITER, SCRIBE" — with no `(C)` variant, and the
captured byte stream proves it: the escape sequences it emits are `ESC T`,
`ESC >`, `ESC P` and `ESC G`, and there is **no `ESC K`** anywhere in the job.
Colour is a *New Print Shop* feature, whose Setup names "Apple Imagewriter II
**(C)**" — the same note DEV.md already carries on `Ribbon`.

**Confirmed on the real colour driver.** With *The New Print Shop* now in the
tree, its own **Test Printer** was driven end to end: Setup already reads
`Printer: Apple Imagewriter II (C)` / `Interface Card: Apple ][ Parallel` /
`Slot: 2`, and the job it emits is **5 194 bytes carrying 13 `ESC K` band
selects** — K1, K2 and K3, the yellow/magenta/cyan passes — each followed by a
bare CR and its own `ESC G` block. POM2 prints it as
"Welcome to The New Print Shop" in magenta, green (yellow over cyan) and cyan:
three ribbon bands and one subtractive overprint on the page.

That run is also what the fix above buys. The same job on the pre-fix card:
**5 194 bytes written, 0 spooled**, because "Apple ][ Parallel" is exactly the
direct-drive path — every byte went to `$C0n0`, never to `$C0n1`. Colour
printing with The New Print Shop did not work in POM2 before today, and the
reason had nothing to do with colour.

One thing it needed that POM2 cannot do yet: the disk is an **800K 3.5" WOZ**
(Applesauce flux, `INFO.disk_type = 2`), and nothing in the tree mounts one —
`Disk35Image` takes `.po`/`.2mg` only, and the 5.25" WOZ loader rejects a 3.5"
image. It was converted offline for this test (WOZ → GCR-decoded 819 200-byte
`.po`, 1 600/1 600 blocks, volume `NPS`) using POM2's own decode tables from
`Sony35Drive`. See TODO — the decoder already exists in the tree, it is just
not wired to a file loader.

POM2's colour path itself is correct, and is now pinned against the shape the
2026-07-26 trace recorded from a real colour driver: three `ESC G` passes,
each with its own `ESC K` band, separated by **bare CRs** so they overprint.
`imagewriter_smoke` asserts they land on ONE line (a staircase was the
original bug), that the bands OR into the right subtractive mixes — cyan
alone, cyan|yellow = green, all three = black — and that the palette turns
those into real RGB. It runs in the default `AutoFeed::Auto`, since deciding
"does CR feed paper?" from the stream is exactly what makes the overprint
work.

## 2026-08-17 (later still) — Bug hunt 8: two things that ran off the end of the page

Hunt 7 swept what had recently changed. This one went the other way and asked
which module had never been swept **at all**: `src/hgrsprite/` — 1 850 lines,
landed in one commit, the only source directory with no test of its own, and its
header claiming a `hgr_sprite_blit_smoke` which exists in POM1
(`tests/hgr_sprite_blit_smoke_test.cpp`) and never existed here. The defect was
in it.

**`HgrSpriteEditor`'s ca65 DHGR export read past the end of the 16 KB pair it
was slicing.** With the DHGR target on, Export ASM rasterises the shape into an
aux+main pair and emits two `.byte` tables, `nPer` bytes per row per plane. It
derived `nPer` from the **un-clipped** shape width:

```
dotCols = (wpx() * 4 + 6) / 7;   nPer = (dotCols + 1) / 2;   // = 2 x wBytes
```

But one lit shape pixel rasterises to one DHGR **colour pixel**, and a DHGR line
holds 140 of them — `buildDhgrPair` drops the rest, because `plotDhgrPixel`
range-checks `x`. So past 20 bytes of width the export kept counting bytes the
picture no longer had:

- **W ≥ 21 bytes** — `nPer` exceeds the 40 bytes a plane row actually holds, so
  each row runs on past its own end into whatever the interleave puts there
  (screen holes, other rows). Silently wrong tables, no crash.
- **W ≥ 25 bytes with H = 192** — the last row starts at page offset `$1FD0`,
  48 bytes short of the plane end, so `pair[kHiresSize + rowBase + i]` leaves
  the vector. At the UI maxima (W = 40, H = 192, both plain `InputInt`s clamped
  only by `clampGeom` to 40/192) it reads **32 bytes past a 16 KB heap block**,
  confirmed under ASan.

W and H are ordinary numeric fields in the top bar and the DHGR checkbox is
offered on every IIe-class profile, so this is a few clicks away, not a
contrived state.

The fix does not add a bounds check at the read — it removes the disagreement.
The clipping now lives in two pure functions next to the rest of the byte layer,
`hgrsprite::dhgrExportRowBytes()` (clip the width at `kDhgrWidth` FIRST, which
lands the row length at exactly one 40-byte plane row) and
`extractDhgrPlanes()` (whose READS clamp to one plane row and 192 rows, so no
argument can take it outside the pair — the caller's stride is still its own).
The editor calls both, and says so in the status line and the exported `.byte`
header when a sprite was wider than the DHGR line: a clip you cannot see is the
reason the arithmetic could disagree for this long.

Both additions are **additive** to `HgrSpriteBlit.{h,cpp}`, which was still
byte-identical with POM1's copy (as is `HgrSpriteAsmExport.cpp`), so the module
stays resyncable. `HgrSpriteEditor.cpp` had already diverged — the DHGR target
is a POM2-only addition, 1 323 lines here against POM1's 1 110 — which is also
why POM1 does not carry this bug.

`tests/hgr_sprite_blit_test.cpp` is the test the header always promised: extract /
stamp round-tripping through the row interleave, their edge clipping,
`magnifyColor2x`'s colour clock + palette bit, and the two new helpers — with
the regression pinned as a property (`dhgrExportRowBytes` never exceeds a plane
row for **any** width the editor can be set to; the old formula returned 80 at
the maximum, twice the row).

**Three smaller things, all real:**

- **`EchoPlusTMS5220Card` serialized nothing, and unlike the other card in that
  position it had something to lose.** The 2026-07-29 workflow hunt fixed five
  cards that carried no snapshot state; this one landed afterwards and inherited
  the gap. Scaffold or not, every byte it owns is guest-READABLE — `$Cs00`
  returns the TMS5220 status, `$Cs04-$Cs07` return the selected register of
  either AY-3-8913 — so a rewind restored the machine around a card still
  holding the abandoned timeline's registers, and a driver polling status across
  the jump read a value from a future that no longer happens. It now emits the
  same magic-tagged, version-prefixed, validate-before-mutate blob as its
  siblings (TMS status + last write + both address latches + both full AY
  banks). `card_snapshot_state` grows its sixth case — which, being an
  `assert(!blob.empty())` on the capture, fails outright against the old card.
- **`PrinterHistory::nowStamp` formatted a timestamp into a buffer that could
  not hold it.** `char buf[32]` for `"%04d-%02d-%02d %02d:%02d:%02d"` — `%04d`
  is a minimum width, and nothing stops `tm_year + 1900` (an `int`) needing 11
  characters, so the worst case is 72 bytes. GCC 13 says so under `-O3` +
  `_FORTIFY_SOURCE` and it is right: the only way a 32-byte buffer answers is by
  truncating, which would put a malformed timestamp in the durable print-history
  index rather than fail. Now 80 bytes. It was the ONE warning in the whole
  first-party build under `-Wall -Wextra -Wshadow`, which is otherwise clean on
  GCC 13.3 too — this one only fires with optimisation and fortification on, so
  hunt 7's warning-clean measurement was not wrong, just taken elsewhere.
- **A test assertion that could only ever fail for the wrong reason.**
  `fujinet_card_smoke_test.cpp::testDeviceCountWithoutPeer` timed the run with a
  stopwatch and asserted `ms < 100`, commented "a bus scan must not pay a
  network timeout per probe". It cannot measure that: with no peer attached —
  which is the whole point of that test — `SpOverSlipLink::transact` returns on
  `!transport_->isOpen()` **before** the 250 ms wait, so the failure mode named
  is unreachable and the only thing the bound reacts to is host speed. It duly
  fired under the valgrind sweep below, on a path with nothing network about it,
  and it would fire the same way on a loaded CI runner. It now asserts the
  link's own counters (`stats().calls == 0`, `timeouts == 0`), which state the
  intended property directly and no slowdown can perturb. Its cousin
  `disk_writeflux_framing` had the same shape one level up — 0.6 s native,
  10.3 s under ASan+UBSan against a 10 s CTest `TIMEOUT`, so the sweep reported
  a failure on a test that was passing. Now 60 s, which still catches the hang
  the budget exists for.

**What was swept, and what came back clean:**

- **The whole suite under ASan + UBSan** (GCC 13.3, RelWithDebInfo): **180/180
  green, zero sanitizer diagnostics** — no out-of-bounds, no UB, including
  every path this hunt changed. (The 181st, `disk_skew_sniff`, wants the repo
  root as its working directory and passes there; the sweep ran from an
  out-of-tree build dir.) The build is warning-free under the sanitizers too.
- **The whole suite under valgrind memcheck** (`--track-origins=yes`), 164 test
  binaries, **zero diagnostics** on the 162 that finish. This is not a repeat of
  hunts 5 and 7: those ran ASan+UBSan, and ASan cannot see a read of
  *uninitialised* memory at all. Memcheck can, and found none — no uninitialised
  branch, no invalid access, no bad free, across every device, parser and
  snapshot path the suite reaches. The two exceptions are the tool, not the
  code: `iic_printer_port` needs more than a 15 min budget at ~30× slowdown, and
  `fujinet_card` tripped the stopwatch fixed above (and passes cleanly since).
- **A ThreadSanitizer pass over `EmulationController`** — the item TODO.md calls
  "the highest-yield gap we know of" — driving the real concurrency shape
  without a GUI: the CPU worker, a UI thread running the transport verbs a user
  clicks (rewind scrub/seek/resume, cassette, 3.5" mount/eject, speed, mode
  toggles, a `lockState()` read per frame), an AI-server thread doing
  `lockState()` reads plus snapshot capture/restore and key injection through
  `Memory::kbMutex`, a live miniaudio callback thread, and a Mockingboard in
  slot 4 with a guest loop writing its VIA and toggling the speaker so the
  emuCycles AY queue — the one genuine producer/consumer pair between the CPU
  and the audio thread — is fed for the whole run. **Zero races** across five
  runs totalling ~9 minutes of wall clock. Honest limit: TSan instruments every
  load and store in an interpreter whose hot loop is nothing else, so the CPU
  manages only ~400-1 400 emulated cycles per wall-clock second (the 7-minute
  run retired 175 404 of them). The run therefore covers the *lock protocol* —
  park/unpark, mode flapping, scrub under load, snapshot under the lock, audio
  callbacks against worker chunks — thoroughly, and *emulated execution*
  thinly. The GUI half of that TODO item is still open.
- **Bounds re-derived by hand, not by tool, on the parsers a crafted file
  reaches**: `ProDOSVolume`'s build + decode walk (the block/bitmap arithmetic
  and the path-traversal guards), `SnapshotIO` + `MachineSnapshot`'s
  transactional restore, `Memory`'s MEX trailer, `IWMDevice` and
  `IIcClassProfile`'s nested device blobs, and `Sony35Drive::decodeAndCommit`'s
  GCR sector walk. All hold.
- **Snapshot field coverage per card.** Every `SlotPeripheral` was checked for
  members that are guest-visible but unserialised — which is how the Echo+ gap
  above surfaced. The other card with no snapshot at all, `PrinterCard`, owns
  only its ROM and the host-side spool (printer output is deliberately outside
  the machine snapshot — a rewind must not un-print);
  `LeChatMauveCard::invertBit7_` and `GrapplerCard`'s DIP/BUSY
  fields are host settings, correctly excluded on the same grounds as
  `MachineSnapshot`'s CPU-mode byte; `GrapplerCard::irqAsserted_` is re-derived
  by `updateIrq()` on load. No gaps.

### Round 2 — fuzzing the surfaces that had never been fuzzed

Round 1 read code and ran sanitizers over the existing suite. Round 2 built
harnesses for the three input surfaces with no dynamic coverage at all. One of
them came back with a defect.

**The print head walked off the paper.** `ImageWriter::printBitGraph` advanced
`curX_` one dot column at a time with **no right-margin test**, while every
other head-motion path in that file has one — the text advance wraps on it, the
`ESC F` / `ESC '` absolute moves refuse to cross it. So an over-long bit-image
run just kept going: `ESC V 9060 <col>` at 80 dpi parks the head **113 inches
out on an 8.5-inch page**, and the pacing model charged the full dot-column
rate for all 9 060 columns — **22 emulated seconds of BUSY, printing nothing**,
because `fillDots` had already clipped every one of those dots away. A guest
polling the printer waits out all of it, and the status line reports a head
position no ImageWriter can reach.

Columns whose start is at or past `rightMargin_` are now **discarded** — not
wrapped, which would corrupt a bit image, and which is what the hardware does
with the excess of an over-long graphics line — the head parks against the stop
rather than sailing past it, and `byteCost` stops charging carriage travel for
a carriage that is not moving. The same 7-byte job now drains in **1.7 s**.

The fix is **output-neutral by construction**, which is what makes it safe:
`rightMargin_` is only ever the paper width (no command narrows it), so every
column this drops was already being thrown away by the raster clip. The new
`imagewriter_smoke` case asserts exactly that — an over-long run must produce
the byte-identical page an exactly-fitting run produces — plus the bounded
drain and the head position. It fails on the old code.

Found by fuzzing, and worth recording honestly: the fuzzer's first report was
**wrong**. Its liveness predicate compared the outstanding count against the
running minimum, and a repeat run legitimately *grows* that count when it is
armed (7 queued bytes become 9 060 owed columns), so the harness read a
perfectly healthy drain as a stall in 15 of 60 streams. Delta-debugging the
"stall" down to 7 bytes is what exposed the real defect underneath — a 113-inch
head position — and the corrected predicate (compare against the previous tick)
reports zero stalls across 3 000 streams against the fixed code, along with no
sanitizer report and no page-geometry fault.

**The two that came back clean:**

- **`SnapshotIO` + `MachineSnapshot`, 40 000 mutated blobs.** This closes the
  TODO item that had stood since 2026-08-02 ("built during the ASan sweep but
  never executed, so that parser is the one untrusted-input surface in the tree
  with no dynamic coverage"). Bit flips, truncation, extension, corrupted
  section lengths and names, duplicated sections, absurd lengths and random
  tails behind a valid magic: 6 915 accepted (and then RUN, so a crafted paging
  state has to survive execution), 33 085 rejected, no sanitizer report. The
  fuzzer also asserts the property `MachineSnapshot.cpp` claims in prose —
  "makes a rejected file observationally transactional to the machine" — by
  re-capturing after every rejection and requiring the blob to be identical:
  **0 violations**.
- **The AI control server, 6 000 hostile requests.** The only surface in POM2
  that parses bytes off a socket, and only well-formed requests had ever
  reached it. Random binary, headerless requests, absurd and negative
  `Content-Length`, 60-header soup, 200-parameter query strings, lying content
  lengths, truncated JSON, `\`-terminated strings, path traversal, and
  byte-at-a-time dribbling: no sanitizer report, and the liveness probe
  (`GET /status` must still answer 200) never failed — a wedged worker thread
  is as much a defect as an over-read, and only a probe catches it.
- **The cassette, 12 000 mutated tapes.** `loadTape` is an untrusted-FILE
  surface — the `.wav` path hands the bytes to vendored miniaudio — that disk
  images, WOZ and snapshots had all had a mutation pass ahead of. Malformed
  RIFF/WAVE (bad channel counts, 0 Hz, 4- and 0-bit samples, float PCM, lying
  chunk sizes), `.aci` blobs with absurd transition counts, and pure noise;
  2 493 loaded and then played, seeked, rewound, re-saved and re-loaded. Clean.

### Round 3 — the guest-driven surfaces, and a write-back that ate a file

Rounds 1-2 covered files and sockets. Round 3 went after the surfaces the
GUEST drives — soft switches, the display decoders, the Disk II head — plus the
one path where guest-writable bytes become host filesystem operations. That
last one is where the defect was.

**A ProDOS write-back could merge two files into one and report success.**
`decodeVolumeToFolder` strips trailing dots before composing a host filename —
a name ending in `.` is legal in ProDOS and awkward-to-illegal on the host — so
`README` and `README.` both came out as `README`, and the second write silently
REPLACED the first. Both halves reported success throughout: the build said
2 files included / 0 skipped, the decode said 2 written, and the user was left
with one file holding the other's bytes.

Nobody has to go looking for that pair. `sanitiseProDOSName` maps every
character outside `A-Z 0-9 .` to `.`, so a host folder holding `README` and
`README!` becomes `README` + `README.` inside the volume, `uniqueName` sees two
perfectly distinct ProDOS names, and the write-back merges them back. `NOTES` /
`NOTES?`, `DATA` / `DATA-`, any such pair does it — and the guest can create
both names inside the volume directly, so the decode has to be safe on its own
rather than relying on what the builder emits.

Host names are now reserved per decoded directory, and a clash takes a numeric
suffix instead of overwriting (`README`, `README.1`). The reservation covers
**this decode pass only**, never what is already on disk, so re-decoding an
unchanged volume still lands on the same names and rewrites nothing — a
write-back happens on every eject, and a set that consulted the directory
would grow a fresh `.1` each time. Subdirectory names go through the same
gate: two ProDOS directories merging into one host directory would take their
contents with them. Pinned by `prodos_volume_smoke`, which asserts both files'
CONTENTS survive (the failure mode was one file holding the other's bytes) and
that a second write-back writes nothing; it fails on the old code.

**Round 2's printer fix, checked against POM2's own graphics producer.** The
screen dump is the in-tree source of long `ESC G` runs — an 80-column screen is
560 columns at 72 dpi = 7.78 in, wider than ISO B5's 6.93 in page — so it is
exactly where "the carriage now stops at the right margin" could have changed
real output. Printing the same dump on all seven paper sizes, before and after:
**ink identical on every one**, including B5, where the dump legitimately loses
its right-hand 11 % either way (184 700 lit dots vs 215 032 on the wide papers).
Only the time moved, and only where it should have: B5 went 548 -> 477 ticks,
every paper wide enough to hold the dump unchanged to the tick. The property is
now pinned in `printer_screen_dump` as "the narrow page is the wide page
CROPPED" — 1.4 M dots compared, which fails the moment the excess is wrapped,
shifted or dropped a column early. A separate check confirmed the clip does not
strand the MSB that `ESC V` forces through for its run: text printed after a
clipped run has the same 1 238 lit dots as text printed alone.

**What was swept, and what came back clean:**

- **Soft-switch + display chaos, 6 000 machines / 12 000 frames.** Between them
  `Memory` (2 480 lines of paging dispatch) and `Apple2Display` (2 651 lines of
  scanline decoders, plus AppleWinNtsc and the OE demod) hold most of the index
  arithmetic in the emulator, and every display test sets up ONE deliberate
  state. This one builds a machine per iteration — II+ or IIe, NTSC or PAL,
  RamWorks banks on or off — fills the display pages with the patterns the
  artifact-colour paths key on, throws 20-400 random `$C0xx` accesses and RAM
  writes at it, reads all 64 KB back, then renders twice in a random one of the
  ten display modes and touches every pixel. No sanitizer report, and the
  framebuffer contract (non-null, 280 or 560 x 192) held on every frame.
- **Disk II guest chaos, 300 guests, 28.4 M nibble write flushes.** Every disk
  test drives a DELIBERATE sequence (a DOS 3.3 write loop, a real RWTS). This
  one pulses the phase magnets in any order, at pacings from back-to-back to a
  full revolution apart, flips Q6/Q7 mid-nibble, swaps drives and ejects
  mid-write, and interleaves well-formed write bursts so the write-back path is
  genuinely exercised rather than merely reachable. Clean on all three
  properties: the head never left `[0, kTracks)` whatever order the magnets
  were pulsed in, every written-back image stayed exactly 143 360 bytes, and
  every one still LOADED afterwards.
- **ProDOS volume build/decode, 1 200 random host trees.** Weird names (spaces,
  dots, `#`, accents, 40-character randoms), nested subdirectories, and images
  corrupted between build and decode — scrambled key pointers, crafted `../..`
  and `/etc/pw` entry names, self-referential subdirectory chains. **Zero
  containment breaches**: with sentinels planted outside the destination and
  the whole sandbox re-verified after every decode, nothing the image claimed
  ever produced a file outside the folder it was decoded into.
- **The file-write audit.** Every `save`/`export`/`write` function in `src/`
  was checked for a path that writes user data and reports success without
  testing the stream. None found: the three that looked like candidates
  (`Block512Backing::saveDirty`, both cassette savers) check through helpers.

**Three harness bugs, recorded because they are the interesting part.** Round
2's stall report was a fuzzer artifact; this round's Disk II head-position
property first targeted `getTrackPosition` (the nibble cursor) instead of
`getCurrentTrack`, reporting 223 303 phantom faults, and the ProDOS mutator
indexed 1024+4095 into a 4 608-byte volume and was rightly shot by ASan. A
fuzzer that reports a defect has said something about the fuzzer until the
defect is reduced to a reproducer — which is how both real findings of this
hunt were confirmed, and how these three were dismissed.

## 2026-08-17 (later) — Bug hunt 7: no functional defects; the build is warning-clean again

A sweep over everything that landed since the 2026-08-13 hunt — the mouse-grab
feature, the bundle manifest + AppImage packaging scripts, and the same
morning's `pom2::StateAccess` refactor — plus a fresh pass over the untrusted-
input surfaces. **No functional defect survived verification.** Two compiler
warnings did, and they were the whole yield:

- **`PhasorCard::onViaPortBChange` shadowed its own `viaIdx` parameter.** The AY
  read-bus latch re-derived the VIA index as `chipIdx >> 1` inside a lambda
  nested in a function whose parameter is already `viaIdx`. The two always
  agree — `chipIdx` is `ayBase` or `ayBase + 1`, and `ayBase` comes from
  `viaIdx` — so the shadow was benign, but it was the ONE `-Wshadow` hit in
  110 first-party translation units, and a shadow that happens to be equal
  today is a bug waiting for the day the mapping changes. The local is gone;
  the parameter is used directly, guard included.
- **`NoSlotClock::loadClockSnapshot`'s BCD clamp read as a bug it wasn't.**
  `if (lsb < 0) lsb = 0;   if (lsb > 9) lsb = 9;` on one line is correct C++
  and correct behaviour, and `-Wmisleading-indentation` flags it precisely
  because a reader parses it as one guarded statement. Now `std::clamp`, which
  says the same thing with no room to misread. (The clamp itself earns its
  keep: `tm_sec == 60` is a legal leap-second value the C library really
  hands out.)

Both were the only warnings in the whole first-party build, which is again
clean under `-Wall -Wextra -Wshadow`.

**What was actually swept, and what came back clean:**

- **Lock discipline after the `StateAccess` refactor.** `stateMtx` is a plain
  `std::mutex`, so one nested `lockState()` anywhere is a hard deadlock. Every
  lock site in `src/` was walked with a brace-accurate scanner that follows the
  held scope and flags any call back into an `EmulationController` method that
  re-locks (`hardReset`, `coldBoot`, `bootFromSlot`, `mount35`, the whole
  cassette + rewind transport, `lockState`/`stateMutex` themselves): **zero
  nestings**. The four apparent hits are all `if`/`else` arms — the lock is in
  the branch not taken. The `stateMutex → demodMutex` order also holds in both
  directions at all four sites that take both.
- **Disk-image loaders, fuzzed.** 2 050 mutated/truncated WOZ1, WOZ2, WOZ2+FLUX,
  `.dsk`, `.nib`, CNib2, `.d13` and 2IMG-wrapped images through `loadFile`
  under ASan+UBSan, each survivor then driven through `trackBitLength` /
  `bitAt` / `trackPeriod` / `fluxEvents` / `getNextTransition` past the end of
  the track / `nibbleAt` / media-snapshot round-trip. No crash, no UB.
- **Guest-driven chip register files, fuzzed.** 600 000 random W5100 ops
  (register writes, socket commands, RMSR/TMSR ring geometry changes mid-
  stream, indirect-window traffic, corrupted snapshot restores) and 450 000
  CS8900A I/O-port ops with a frame-generating backend, plus 400 000 SmartPort
  `$C0nX` ops with the medium ejected and remounted underneath the stream.
  Clean — the ring math holds under a geometry rewrite with data staged, which
  is the case the arithmetic is most exposed to.
- **`Memory` snapshot restore, fuzzed.** 4 500 corrupted / truncated / extended
  blobs restored into a machine that is then run: paging state cannot be
  driven out of bounds by a crafted blob.
- **Static analysis.** clang-analyzer core/cplusplus/unix + the bugprone
  subset over all 113 first-party TUs: **28 hits, none actionable**, in four
  clusters, each dismissed on its own evidence rather than in bulk.
  - 12 × `bugprone-incorrect-roundings` (`(int)(x + 0.5)`). The check fires
    on the negative half, where that idiom rounds the wrong way — and every
    site here clamps first: colour channels to `[0,1]` before `× 255`, the
    joystick axis to `[0, 255]` before the cast, `fracX/fracY` to `[0,1]`
    before scaling the mouse clamp window. Non-negative input makes
    truncate-after-`+0.5` exactly round-half-up.
  - 7 × `deadcode.DeadStores` — initialisers overwritten by the switch that
    follows. Style, not behaviour.
  - 3 × `core.CallAndMessage` "called null pointer": `M6502::memReadAbsolute`
    off the default constructor (which sets `memory = nullptr`; its one user,
    `diskii_lss_smoke_test`, uses that CPU purely as a cycle source and never
    executes an instruction on it), and two on `HgrPaintEditor::host`. The
    latter is a real inconsistency worth knowing about: `beginStroke` guards
    with `batch && host`, so it treats a null host as possible, while the
    render path calls `host->textureToImTexture()` unguarded. Both hosts that
    exist (POM2, POM1) always pass one, and an editor with no host can
    neither render nor poke, so nothing is broken today — the guard is what
    is wrong, not the call.
  - 2 × `misplaced-widening-cast` (`kTracks * kNibblesPerTrack` = 232 960 and
    a 0..5 register offset, both computed in `int` and cast after — no
    overflow reachable) and 2 × `signed-char-misuse` on `clip.idx`, an
    `int8_t` buffer written only as `static_cast<int8_t>(std::max(v, 0))`
    from a 0..15 palette index, so the sign bit is never set.
- **One parity claim re-checked against its source.** The CS8900A multicast
  hash index — `(~crc32(dest, 6) >> 26) & 0x3F` — looked like a double
  complement (POM2's CRC-32 already applies the final XOR). MAME's
  `cs8900a.cpp` spells it exactly the same way, so the port is right and the
  oddity belongs upstream.

The fuzz harnesses are not committed, matching the precedent set in hunt 5:
they link against test-target objects rather than a build-system target, which
is a CI decision rather than a bug fix.

## 2026-08-17 — A menu drawn over the screen owns its own clicks

**Clicking an item in a dropdown that overlapped the Apple II screen fired the
menu item *and* handed the press to the Mouse Card — and, with click-to-grab
on, captured the pointer behind the still-open menu.** The cursor vanished with
no visible cause and the way back was a chord the user had no reason to know
they now needed.

The cause was one predicate. `MainWindow::mouseGrabContext()` filled
`mousegrab::Context::insideScreen` by rect containment — *is the cursor between
`screenRectMin` and `screenRectMax`?* — and a menu drawn on top of the screen
is geometrically **inside** that rect. ImGui's z-order was never consulted, so
the policy in `MouseGrab.h` was answering a question about pixels when the
question was about ownership. Nothing in the policy itself was wrong; it was
being fed a fact that did not mean what its name said.

It now reads `ImGui::IsItemHovered()`, captured next to the screen `Image()`
into `MainWindow::screenHovered_` and cleared at the top of every `render()` so
a collapsed or never-drawn screen window cannot latch a stale `true`. The
correct primitive was always two lines away — the 3D voxel camera right below
the same `Image()` had been using `IsItemHovered()` all along.

The `Context` field is renamed `insideScreen` → `screenHovered`, deliberately:
the change is one of *meaning*, so every call site had to fail to compile
rather than keep quietly passing a rect test. Its doc comment now states that a
containment test is not an acceptable source. Pinned by
`mouse_grab_policy_test.cpp::testUiOverlayOwnsItsClicks`, which also fixes the
release half — a button pressed on the screen and released after a menu opened
over it must still clear on the card instead of sticking down in the guest.

**`EmulationController::lockState()` — the state lock now carries the state.**
An audit of all 123 `->memory()` / `->cpu()` call sites found no live race: the
38 that sit in bodies which never lock `stateMutex` are safe, the keyboard
family by way of `Memory::kbMutex` (the finer-grained lock that lets the UI and
the HTTP thread inject keys without contending with the worker) and
`plugSlotsFromSettings()` because two of its three callers hold the lock for
it. The invariant holds today — it just holds by care alone, across ~120 sites
on threads that are not the worker.

`lockState()` returns an RAII `pom2::StateAccess` that hands back `Memory` and
the CPU *through* the lock, so `st.memory()` cannot be spelled without having
taken it. `stateMutex()` stays for the other case — mutual exclusion with no
state access, e.g. serialising a card pointer against a profile switch — and
the two now mean different things on purpose; 85 of the locks are that second
kind and were left alone. It is a namespace-scope class rather than a member of
EmulationController because `MainWindow.h` deliberately stays outside that
header's include cone, and only a non-nested class can be forward-declared
there.

**The sweep is complete**: 123 raw `->memory()` / `->cpu()` sites became 63
`lockState()` handles plus 38 that are *correct* unlocked and now say so at the
call site. Those fall in exactly three categories, and nothing else qualifies:

  1. **Before the worker exists** — the MainWindow constructor and
     `pom2_headless`'s setup, both of which run before `controller.start()`.
  2. **A finer-grained lock owns it** — the keyboard latch and paste queue
     (`Memory::kbMutex`), which is what lets the UI, the CLI and the HTTP
     thread inject keys without contending with the worker on every keystroke;
     plus `setOpenAppleKey`/`setSolidAppleKey`, which are plain atomics.
  3. **Bus topology, UI-thread-confined** — *which slot holds which card* is
     written only by plugSlotsFromSettings / applyProfile / the slot-config
     rebuild, all on the UI thread; the worker only reads it. Locking to grab
     the `SlotBus&` would protect nothing, since the reference outlives the
     scope, while reading as though it did. Per-card *state* is a different
     matter and does take the lock.

Three genuinely unsynchronised reads turned up on the way and are now locked:
`renderStatusBar` copied the live `DisplayState` struct while the worker was
rewriting the soft switches, and both `renderMenuBar` and `applyProfile`'s
"Profile: Active" log read `getCpuMode()` outside the scope that had just set
it. `pollJoystickAndPushToMemory` bound one `Memory&` before the lock and used
it on both sides — the paddle half wants the state lock, the `queueKey` half
must not hold it; they have separate references now, because sharing one is
what made the split invisible.

One landmine defused on the way. `plugSlotsFromSettings()` and everything it
reaches must **not** take `stateMutex` — `applyProfile()` steps 5-7 and the
slot-config rebuild both call in already holding it, and it is a plain
`std::mutex`. The header claimed the opposite ("called with the CPU worker
already stopped, never under stateMutex"), so anyone tightening that function
by adding the "missing" lock would have deadlocked the profile switch
instantly. The comment now states the real contract: the caller owns the lock,
the callee never locks.

## 2026-08-15 — Seven packages from one payload manifest (v0.8.2)

**The list of what ships inside a package was written down four times, and the
four copies had already drifted.** `CMakeLists.txt`'s `install()` rules, the
macOS packager, the Windows packager and the WASM `--preload-file` block each
carried their own version of it. The browser bundle shipped `floppyemu/` and
the *whole* `fonts/` and `pic/` folders — 4 MB of reference photography no code
path reads — while the desktop packages shipped neither the folder nor, of
course, the extra. Nothing was wrong in any single place; the arrangement was
wrong, because anything that has to be repeated four times will eventually be
repeated incorrectly, and no test could see it.

`packaging/bundle.manifest` is now the one list. CMake parses it at configure
time and derives *both* the install rules (which the AppImage inherits, being
staged with `cmake --install`) and the WASM preload arguments;
`packaging/stage_data.sh` parses the same file for the macOS `.app` and the
Windows `.zip`. Two CMake parsing details turned out to be load-bearing and are
commented where they matter: `file(STRINGS)` needs `ENCODING UTF-8` — without
it, a line containing a multi-byte character is treated as binary and *split*,
so the tail of a prose comment comes back as a manifest entry — and the comment
strip must use `FIND`/`SUBSTRING`, because CMake's regex `.` does not match the
bytes of a multi-byte character and `#.*$` stops dead at the first em-dash.

**Packages now have to prove themselves, twice.** `stage_data.sh --verify`
asserts the manifest's payload is present and that nothing from its `deny` list
came along; both failures are silent otherwise — a missing font drops the UI to
ImGui's bitmap face with blank icon boxes, and a leaked `disks_5.4/` turns a
6 MB download into a 200 MB one carrying media that is not ours to
redistribute. Then `pom2_headless --frames 300 --screenshot` boots the bundled
ROM *through the packaged binary* and fails if the frame it draws is a single
flat colour. That last one is the check the release pipeline was missing:
`POM2 --help` passes just as happily with an empty `roms/`, so every package
job could go green around a package that could not boot. It resolves ROMs
through `pom2::findResource`, so running it from the package's `usr/` exercises
the package's own asset resolution rather than a CWD accident. Two details the
capture mode learned the hard way: with a Disk II plugged and no disk, the II+
autostart parks in the boot PROM forever and photographs the uninitialised text
page (so no disk ⇒ no controller), and "distinct byte values" is the wrong
uniformity metric for an Apple II — a white-on-black text screen legitimately
has two, leaving no room between "booted" and "blank"; it counts pixels that
differ from the background instead.

**Three new packages, and the naming that keeps them apart.** A generic
`aarch64` AppImage (native runner, desktop GL, glibc 2.39 — current ARM
desktops), the `pi400` PGO/LTO build promoted out of its manual workflow into
the release, and a `web-wasm` zip. That takes the aarch64 count to three, and
`build_appimage.sh` named every one of them `POM2-v<ver>-aarch64.AppImage`.
Since the publish job flattens every artifact into one directory, two of the
three would have been overwritten in silence — a Pi user handed a package that
will not start, or an ARM desktop user handed the GLES build.
`POM2_APPIMAGE_VARIANT` inserts the tag, each job asserts its own name, and
publish now requires exactly seven packages rather than "at least one". The Pi
job previously produced its distinct name by folding the tag into
`POM2_VERSION` (`0.8-pi400`), which got the file name right while stamping a
bogus version into the binary — and the release asserts that a package
announces the tag's version, so the honest seam was the one that only touches
the name.

**A `dir` entry copies the working tree, not what git tracks** — and building
the bundle with the new manifest is what surfaced it: two untracked ROM `.zip`
archives sitting in `roms/` went straight into `POM2.data`. Nothing would have
caught them. CI builds from a clean checkout, so *its* bundle would not have
had them, meaning the committed bundle (the one GitHub Pages actually serves)
silently differed from what the pipeline produced, and every visitor to the
demo would have downloaded two archives the emulator cannot even read. The
manifest gained a `denyglob` kind, excluded three ways — `install(DIRECTORY …
PATTERN … EXCLUDE)`, emcc's `--exclude-file`, and a prune inside
`stage_data.sh` — with `--verify` failing on any survivor.

**The online demo can no longer go stale in silence.** GitHub Pages serves the
committed `wasm/` folder, so that folder is published content, not a build
artifact that can lag harmlessly — a forgotten rebuild leaves an old demo
online with nothing failing. `tools/wasm_stamp.sh` (ported from NeoST, same
problem) fingerprints the *sources* that determine the bundle rather than the
bundle itself, because emcc is not reproducible across emsdk versions and a
byte diff would fail forever, for nothing.

Android is deliberately absent. POM2's GUI is GLFW + desktop OpenGL; an APK is
a port, not a packaging job, and pretending otherwise would have held the
release for weeks.

## 2026-08-15 — Host pointer capture for the Mouse Card

**The Mouse Card is a relative device, and POM2 was feeding it a bounded
pointer.** Both variants (MAME `mouse`, AppleWin HLE `mouseaw`) are quadrature
devices: `onMouseMove` converts host-pixel deltas into Apple mouse units and
the guest firmware clamps them at the edges of its own window. The host
pointer, meanwhile, stops at the edge of the Apple II Screen widget — the
`shouldRouteMotion` gate is what let the user still reach the ImGui panels —
and every delta past that edge was simply dropped. The two cursors therefore
drift apart in absolute terms, and once the guest cursor lags behind it can
become unable to reach its own clamp edges at all: an MGTK menu bar or a
scroll gutter that the host pointer has no screen left to travel toward. The
closed-loop absolute sync added earlier only papers over this for `mouseaw`,
and only while the firmware's clamp window is readable.

Capturing the pointer removes the boundary rather than compensating for it.
`GLFW_CURSOR_DISABLED` hides the OS cursor and unbounds the reported
position, so deltas keep flowing in every direction for as long as the user
keeps moving; `GLFW_RAW_MOUSE_MOTION` rides along on native so the desktop's
pointer-acceleration curve (tuned for a screen-sized target area) stops
deciding how fast the guest cursor travels. In: a click on the screen, or
`Ctrl+Alt+G`, or View ▸ Capture mouse. Out: **`Ctrl+Alt+G` or a middle
click** — the pair every VM viewer and PC emulator already trained into the
user's fingers — plus focus loss and card removal.

Three decisions worth keeping the reasons for. **The capturing click is
swallowed**: the guest cursor sits wherever its firmware left it, not under
the host pointer, so forwarding that first click would fire a button-down at
an arbitrary spot — a stray dot in MousePaint, the wrong menu in A2Desktop.
**`ImGuiConfigFlags_NoMouse` while captured**: `io.MousePos` follows the
virtual cursor, which would otherwise hover and click panels the user cannot
see. (The GLFW backend already skips its own cursor-shape updates under
`GLFW_CURSOR_DISABLED`, so the two never fight over the input mode.) **The
absolute sync is disabled while captured**: it projects the host cursor's
position-in-widget onto the firmware clamp window, and a virtual position
corresponds to no point on screen.

An escape hatch nobody can find is not an escape hatch, so the way out is
stated three times over: `Ctrl+Alt+G` joins F9/F10/F11/F12 in the
unconditional key set (it must work with an ImGui text field focused), a
caption on the screen says how to leave for four seconds after capture — and
for the whole capture in kiosk, which has no status bar — and a `GRAB` badge
sits in the status bar for as long as it lasts. Releasing also clears a held
button, so the guest can never be left holding one down.

The policy (which chord, which button, what a click means, when motion is
routed) lives in `MouseGrab.h`, GLFW-free so `mouse_grab_policy` can pin it
with no windowing stack; `MainWindow.cpp` static_asserts its mirrored GLFW
tokens against the real header. Like kiosk, a grab is purely host-side: the
machine never sees it and nothing about it is snapshotted.

## 2026-08-14 — Drag-and-drop autoboot: seven defects on the drop → boot path

The promise in README § "Boot a disk in one drag" is that dropping a disk
image on the window mounts it in the right controller and boots it. The
callback had been wired since 2026-05-31 and the plumbing was sound — what
had rotted was everything downstream of it. Seven defects, three of them
silent data loss.

**A dropped 3.5" image was refused on the machines most people run.**
`insertAndBootImage`'s HDV branch auto-plugs a block card when the config
has none; its 3.5" branch did not, and failed with "no 3.5" device in this
config". The stock `Apple ][+` / `//e` configurations ship no SmartPort, so
dropping an 800K `.po`/`.2mg` — the single most common 3.5" distribution
format — never booted on the default profile. The fix is not new code: the
Floppy Emu panel already had an `ensureSmartPort` lambda doing exactly this,
scoped to that one window. It is now the member `ensureSmartPortCardForBoot()`
alongside its `ensureHdvCardForBoot()` sibling, and the panel calls it too.
Session-local like the HDV one: the saved slot config is never touched.

**"Dropped + booted" was printed without anyone checking that it booted.**
`bootFromSlot` validates the Apple II JSR-dispatch trio at `$Cn01/03/05` and,
when it fails, logs a warning and degrades to a plain cold boot — but it
returned `void`, so `insertAndBootImage` returned `true` regardless and the
status bar claimed success while the user watched a BASIC prompt or a hung
loader. It now returns `bool` and every "booted" message honours it. Two
reachable cases the lie covered: **no ROM loaded** (the drop is now refused
up front, pointing at Help → Welcome, instead of jumping `PC` into firmware
that does not exist), and a `.d13` with no `roms/disk2_13.rom`, which spins
in the boot loop forever. The //c+ on-board Sony hub gets the same honesty:
its IWM boot path is deliberately unmodelled, so a 3.5" image routed there
now reports "mounted, cannot boot from it" rather than "booted".

**The 2IMG envelope was classified by guessing at the file size.** 2IMG
carries an arbitrary data offset plus an optional comment/creator trailer,
and `Block512Backing` has always parsed the header and required only the
*payload* to be 512-aligned. `classifyDiskForSlot` instead did arithmetic on
the total file size, so ordinary CiderPress output — a hard-disk `.2mg` with
a comment block — matched no bucket at all and the drop was rejected as "not
a disk image", though mounting the same file from the Library worked. It
also dead-ended every ProDOS volume between 143 KB and 800 KB into the 5.25"
loader, which rejects it with "larger volumes belong on the HDV card" — a
route the classifier then made unreachable. It now reads the header
(`read2mgPayloadLength`) and falls back to the old heuristics only when the
file is not a readable 2IMG, so a malformed envelope degrades rather than
breaks. Pinned by `cli_kiosk`.

**An 800K `.dsk`/`.image` went to the 5.25" loader.** `Disk35Image` accepts a
bare 819200-byte payload under `.po`, `.dsk` *and* `.image` (it even
special-cases `.dsk` for write protection), but the classifier sent every
`.dsk` to Disk II by extension alone, so the drop failed with a message
listing only 5.25" sizes. Also pinned.

**The Disk Library hid `.hdv` files that drag-and-drop booted fine.**
`acceptHdv` still carried the `sz > 819200` bound that `classifyDiskForSlot`
had already documented as a bug — and an `.hdv` fails `accept525`/`accept35`
too, so an exactly-800K image (`AppleWorks_AW.hdv`) appeared in **no** tab
while the drop path and the kiosk scan mounted it. The comment claiming the
two lists mirror each other is now true for every extension except `.2mg`,
where the Library's size rules stay a deliberate cheap approximation of the
header parse — it filters a whole directory scan and cannot open every file.

**Three silent-data-loss defects, all on paths a drop takes.** (1) A card
auto-plugged by `ensureHdvCardForBoot` never received the stored
`hdv_writeback` preference, so a dropped `.hdv` accepted every ProDOS write
into RAM and discarded the lot at exit — and worse, `~MainWindow` persisted
that card's write-back flag with no auto-provision guard (unlike the sibling
`hdv_path` block twelve lines above), so **one drag-and-drop permanently
disarmed write-back for the user's real HDV.** (2) `routeMount35` /
`routeMountHdv` replace a SmartPort bay's unit when the kind differs; the
fresh unit comes up write-back off while the persisted `_writeback` stayed
`true`, so the session's writes vanished and the next launch re-armed the bay
— making the loss look like a fluke rather than a config change. They now
write the flag alongside the unit, as the SmartPort panel's own type swap
does. (3) That same replacement discarded `setUnit`'s returned old unit,
leaving `~SmartPortUnit`'s best-effort `(void)saveDirty()` as the only flush;
if it failed (read-only file, unplugged volume, disk full) the dirty blocks
were freed with no error anywhere. Every other eject/insert path in POM2
treats a failed flush as fatal-and-preserve, and these two now do as well.

**Dropping a disk mid-`SAVE` lost the sector being written.**
`DiskIICard::insertDisk` zeroed `writePosition`/`writeLineActive` with no
splice, while `selectDrive` — the other path that abandons a write outside
the normal Q7 falling edge — commits the burst through `writeFlux` first.
`flushPendingWrites` had the same gap, so a profile switch or shutdown mid-
write dropped it too. Both now call a shared `commitInFlightWrite()`, and in
`insertDisk` it runs *before* `saveDirty()` so the spliced sector actually
reaches the file.

## 2026-08-14 — Three defects cleared before the 0.8 tag

A pre-release audit of what still stood between `main` and a `v0.8` tag. Two
of the three came from **`claude/printer-two-email-printing-cj6dj8`**, a
branch from 18 July whose fixes were never merged: it forked before ~500
files of subsequent work, so merging it would have reverted more than it
repaired, and the fixes were re-applied by hand instead. Worth recording as
a process note — a third fix on that branch (the Memory-viewer self-deadlock)
HAD been re-discovered and fixed independently on `main`, so nobody noticed
the other two were still outstanding. A stale branch is not a backlog.

**The Paint editor indexed the DLGR shadow through the HGR interleave — out
of bounds, read and write.** Three sites (`HgrPaintEditor.cpp`: the
palette-shift tool, the palette-seam overlay, the `POM1HGR` save tag) gated
themselves on `grMode || dhgrMode`, spelled out by hand. That test was
correct when it was written and became wrong the day the DLGR page landed
(2026-07-12): DLGR is neither `grMode` nor `dhgrMode`, so it fell through to
the 280-HGR path and got indexed at `hgrByteOffset()` offsets running to
`$1FF7` — while its shadow is the **2 KB** aux+main lo-res pair, four times
short of that. (GR is excluded from those sites for meaning, not safety: it
keeps the legacy 8 KB scratch, so the interleave lands inside it. DLGR is
the only page the arithmetic actually walks off.) The palette tool then fed
the resulting garbage to `emitShadowEdit`, which POKES THE LIVE MACHINE, so
a DLGR palette-shift scribbled across text page 2, user RAM and HGR page 1;
the save tag overran by 8 bytes and stamped `POM1HGR` into `$3F8/$7F8` on
every DLGR save; the seam overlay read out of bounds once per frame while it
was on. All three now gate on **`sixteenMode()`**, the member that already
enumerated the 16-colour pages — the point being that the next mode added
updates one place, not three. Not pinned: those are private members reached
only through an ImGui frame, and `hgrpaint/` has no headless harness at all
— which is exactly why a green 177-test suite never saw this. Logged in
TODO § Arch.

**Media write-back was atomic but not durable — a power cut could leave a
0-byte file where the disk image was.** Every write-back path already wrote
a sibling temp file and committed it with `rename`, which is atomic for a
*reader*: it never exposes a half-written image. It promises nothing about a
crash. Nothing anywhere in the tree called `fsync`, so the directory entry
could reach the journal while the data blocks were still in page cache, and
the file the user got back was the new name with no contents — the classic
truncated-image outcome the temp-file dance was adopted to prevent in the
first place. The flush went into the step they all already share,
`pom2::replaceFileAtomic` (`AtomicFileReplace.h`): data flushed before the
rename publishes it, parent directory flushed after, so all ten call sites
(`DiskImage`, `Disk35Image`, `Block512Backing`, `ProDOSVolume`, `Settings`,
`PrinterHistory`, `CassetteDevice`, the ImageWriter exports) are covered
rather than the three copies of `writeFileAtomic` the TODO item named.
Failure policy matters as much as the flush: a real I/O error (EIO/ENOSPC)
fails the save so the caller keeps its dirty state and the user can retry,
while a filesystem that merely *cannot* honour the request (EINVAL /
EOPNOTSUPP — network mounts, Emscripten's MEMFS) reports success, because
failing every save over a missing guarantee would lose far more data than
the crash being guarded against. The directory flush is best-effort for the
same reason. Pinned `atomic_file_replace`. The three duplicated writers
still want extracting — that half of the TODO item stands.

**`JSR $Cn0D` on a //e ran motherboard ROM as if it were the SmartPort
handler.** The SmartPort dispatch stub jumped straight into the card's $C800
bank (`JMP $CE00`). But on a //e with SLOTC3ROM off — the default — any read
in `$C300-$C3FF` latches the MMU's INTC8ROM flip-flop, after which
`$C800-$CFFF` answers from the *internal* ROM (`Memory::memRead`, MAME
`apple2e.cpp:c300_int_r`). The 80-column firmware reads `$C3xx` constantly,
so this is the ordinary state of a running machine, not a corner case: the
JMP landed in motherboard firmware and the guest executed whatever it
decoded to. The entry is now `BIT $CFFF` **then** `JMP $CE00`, which is what
the real Liron firmware does — `$CFFF` clears INTC8ROM and releases the
expansion owner, and fetching the JMP at `$Cn10` re-claims the window for
this slot (`SlotBus::slotRomRead` latches the owner on any access to the
slot page), so the target is live by the time the JMP takes it. Two bytes of
side effect: the real-ROM overlay now stops at `$Cn12` instead of `$Cn1F`,
which also settles a contradiction — the code claimed to keep the dump's
`$Cn10-$Cn1F` and was painting NOP padding over them.
`liron_smartport_dispatch` gained an INTC8ROM-latched pass, and **was shown
to fail against the reverted fix** before being kept (the standing rule
after the 2026-08-01 Mockingboard harnesses that passed against the bugs
they were written for).

Also: the committed WASM bundle (`wasm/POM2.{js,wasm,data}`) was rebuilt from
current sources — it dated from 2026-08-02 and GitHub Pages serves it as the
"play in browser" demo, so a tag would have shipped a two-week-old emulator
to the web. Note for the next rebuild: **two emsdk installs** live on this
machine and `build_wasm/`'s CMake cache pinned the broken one
(`~/src/emsdk`, 5.0.4, dangling `$CFGDIR`) even though `emcmake` was handed
the good one — `./build_wasm.sh --clean` is the fix, and the failure looks
like a version mismatch rather than a stale cache.

Full suite green: 177/177.

## 2026-08-13 — Bug sweep across storage, audio, I/O and printer paths

A five-agent subsystem sweep (CPU/memory, storage, audio, network/serial,
display/printer), each finding verified against the source — and for the
worst one, against a runnable repro — before being fixed.

**Disk II LSS writes corrupted sectors whenever the revolution anchor and
the write-clock origin disagreed mod 8.** `DiskImage::writeFlux`'s
"last cell only partly covered" hold-back at a flush seam was evaluated on
the revolution-anchor grid (`endMod % cyc`) while the cell windows are
framed on the write-clock grid (`fr.origin`). The two anchors are latched
at different moments (motor-on vs Q7-on), so they only agree by luck; on a
mismatch a COMPLETE cell was held back and then discarded at the next seam
— one bit lost per ~30-transition flush, shredding ~345 of a data field's
353 nibbles. The existing framing/anchor tests never caught it because
their windows are all multiples of 8 from anchor 0. A repro replaying
`disk_writeflux_framing_test` with anchors 1/3/5 corrupted 344–348 nibbles
before the fix and zero after. The predicate now tests
`(endLssCycle - fr.origin) % cyc` — the same grid everything else uses.

**Rewinding past a disk swap overwrote the newly mounted image with the
old disk's tracks.** The Disk II media snapshot (rewind ring / machine
snapshots) was applied to whatever non-WOZ image is *currently* in the
drive; swap A→B, rewind past the swap, and the next `saveDirty()` wrote
A's decoded tracks into B's file. The snapshot blob is now v3: each
captured media block carries an FNV-1a hash of the image path it came
from, and a restore onto a different disk skips the media apply (with a
warning) instead of cross-writing. v2 blobs still load with the old
semantics.

**Phasor /RESET missed the 2026-08-02 generator-reset fix Mockingboard
got.** The Phasor audio thread re-seeded only the noise LFSR on an AY
reset strobe; tone counters and the envelope machine survived, so a
finished envelope (holding at step 0) silenced the next envelope note
where MAME and MockingboardCard play the 15→0 ramp. It now calls the
shared `ChipSynthState::resetGenerators()`.

**AY register read-back now masks unimplemented bits** (MAME's
hardware-confirmed table): the write-$FF-read-back probe — the standard
AY-vs-YM2149 discriminator, and how some Phasor detectors identify the
board — must see $0F from a 4-bit register, not the raw stored byte.

**Cassette hard-reset semantics existed but were never wired.**
`CassetteDevice::resetCpuSide()` had zero call sites, so the output
flip-flop and cycle base survived F12/power-cycle: the first `$C020`
toggle of a new recording appended the entire across-reset idle gap as
one duration, and `saveWavTape` then refused the whole tape ("exceeds
the 30-minute WAV limit"). `hardReset()`/`coldBoot()` now call it, and it
also re-bases `lastTapeInputCycle` (compared with unsigned subtraction).

**$C068 clamped bit 7 to zero.** The $C061–$C06F mirror block folded
$C068 to $C060 but the switch had no case for it — a tape-read loop
polling the $C068 mirror never saw the comparator flip, and entropy loops
keyed on N were deterministic. It now mirrors the cassette comparator per
MAME's `.mirror(0x8)`. The dead empty guard above the block is gone.

**CPU reset now drops pending interrupt latches.** Neither reset path
cleared the pending-NMI edge or the IRQ source mask, while
`Memory::resetSoftSwitches` documented the opposite assumption. Every
reset path deasserts devices first (slot-bus reset hooks), so clearing
the latches keeps CPU and devices in lock-step — and makes the documented
contract true.

**Telnet SSC: a CR LF split across recv() chunks typed a spurious
ENTER.** `normalizeLineEndings` kept its saw-a-CR state in a per-call
local, so the LF opening the next 256-byte chunk became a second CR —
about one phantom ENTER per 128 pasted lines. The state is now
caller-owned (`telnetPrevCR_`, reset per connection), the same fix the
IAC filter got when it became `processTelnetRx`. Pinned with seam tests
(CR|LF and CR|NUL LF).

**ChildProcess (POSIX): "stop" leaked SIGTERM-resistant grandchildren.**
The grace loop returned as soon as the *direct* child exited, so the
group SIGKILL was never reached — a wrapper script (run-fujinet) died
instantly while its fujinet child trapped SIGTERM, kept the loopback
port, and contended with the next start. The loop now waits for the whole
group (probed with `kill(-group, 0)`) and always sweeps the group with
SIGKILL after the grace.

**Printer fixes.** Screen dump luminance decoded `0xAARRGGBB` but the
framebuffer is `0xAABBGGRR`, landing the red weight on blue: HGR orange
vanished from printouts and medium blue printed as ink (mono screens
masked it — r==g==b). And Epson `ESC J` fed past the bottom margin with
no eject, silently clipping every band after the crossing on `ESC J 24`
paced graphics jobs; it now ejects like `lineFeed()`/VT.

**Char-ROM picker: the 342-0274-A French banks didn't survive a
restart.** `charRomLocaleFromKey` had no cases for the `iie_fr8k_fr` /
`iie_fr8k_us` keys its own inverse emits, so the selection silently fell
back to the profile default on every launch.

## 2026-08-12 — Release hardening for 0.8

The release path now caps build concurrency across CI and every native
packager, with `POM2_JOBS` available as an explicit override.  This prevents
the all-core builds that could exhaust RAM on development and packaging hosts.
The release workflow also rejects tags that disagree with CMake's project
version and runs the quality gate before publishing artifacts.

Runtime hardening in this pass covers writable per-user state directories,
printer-history path confinement, FujiNet guest-address overflow checks,
serial hot-unplug detection, clean GUI termination, and Windows helper-process
quoting/lifetime.  Offline test configuration is now graceful when pinned
external CPU vectors are unavailable, while using them whenever cached.

A second adversarial pass closed the failure paths around those fixes:
printer history now commits its index transaction before queueing a PNG,
removes failed encodes and retries locked-file cleanup; disk and ProDOS writes
propagate deferred I/O failures instead of reporting success. Release tooling
verifies ARM AppImage downloads by SHA-256, treats dependency staging and macOS
signing failures as fatal, and prevents the current directory from entering
the dynamic-loader or bundled-resource precedence paths.

A final storage-focused pass prevents oversized sparse disk images from
driving unbounded allocations, makes HDV and snapshot publication atomic, and
refuses swaps/ejects when pending guest writes cannot be saved. Sector-image
write-back now also rejects a missing or truncated source instead of replacing
unchanged tracks with zeroes. WOZ chunk bounds are overflow-safe on 32-bit
targets, and the Unicode Windows helper launcher builds with the matching
wide-character startup structure.

Snapshot loading is now transactional across CPU, main/extended RAM and slot
state: malformed late sections restore a complete pre-load checkpoint and no
longer discard the valid rewind timeline. POSIX helper startup reports the
actual `chdir` or `exec` failure synchronously. On Windows, FujiNet-PC runs in
a dedicated hidden console so POM2 can send its documented Ctrl+C shutdown,
wait for clean SD-image flushing, and retain Job Object termination only as a
bounded fallback.

## 2026-08-10 — Bug sweep over the FujiNet relay and the printer stack

A multi-agent review of the previous commit, then an adversarial pass that
threw out anything it could refute. Eighteen defects survived; all are fixed
below, and the ones worth a regression test got one.

**The forked helper inherited every open descriptor, and squatted POM2's
ports.** `ChildProcess.h` promised "nothing is inherited by the child beyond
stdio". Win32 kept that promise with `bInheritHandles=FALSE`; POSIX did not
keep it at all. `fork()` dups the whole descriptor table, POM2 opens no socket
with `SOCK_CLOEXEC`, and the child closed nothing before `execv()` — so the
FujiNet helper held a live copy of the SP-over-SLIP listener on 1985, the
AI-control server on 6503 and any SSC telnet listener. Two consequences, both
reproduced: "Drop peer" (stop-then-start) failed to re-bind with `EADDRINUSE`
because the helper's copy was still in `LISTEN`, and a **Ctrl-C on POM2 runs no
destructor**, so `helper_.stop()` never fired and the orphan kept the port for
the next session. The child now closes everything above stderr
(`close_range`, with a loop for pre-5.9 kernels) between `chdir` and `execv`.
Pinned: `child_process` binds a listener, starts a helper, closes its own copy
and asserts the port can be re-bound.

**Status getters were sharing a mutex with blocking I/O.** The FujiNet panel
reads `isOpen()` / `describe()` / `lastError()` every frame **while holding
`stateMutex`**, and those took the transport's I/O mutex — which `readSome()`
holds for its entire timeout. A peer that connected and then went silent
therefore stalled the UI *and*, through `stateMutex`, the CPU worker: measured
as ~750 ms held out of every 1.15 s cycle, i.e. the emulated machine at roughly
a third speed with audible underruns, for as long as the dead peer sat there.
Status is now published separately — atomics where one value suffices, plus a
`statusMtx_` that is never held across a syscall. `lastError()` also stopped
handing out a `const std::string&` the worker rewrites every 200 ms: the
reader's copy-construct could follow a pointer into a block the writer had just
freed, whenever the message changed length.

**A peer that left while the guest was idle was never noticed.** Nothing probed
the socket once a peer was attached — the worker just slept and left discovery
to the CPU thread's failed reads. Restart the helper at the BASIC prompt and
`isOpen()` stayed true forever: the panel named a corpse, and because
`pollForPeer()` is gated behind `!isOpen()`, the replacement sat unaccepted in
the listen backlog until the guest happened to issue a SmartPort call. The
worker now runs a zero-timeout `MSG_PEEK` probe, which distinguishes "idle" from
"closed" without consuming a byte an in-flight `transact()` is waiting for.
`stop()` also resets the SLIP framer, the invariant `peerLostLocked()` already
documented — otherwise half a frame survived a stop/start and ate the next
peer's first packet.

**`--fujinet` did not work.** Two independent reasons. Its default slot 7 is
also where POM2's own first-run map puts a Le Chat Mauve, so the bare flag —
the documented invocation — was refused on a stock install. And combined with
`--preset` it was *silently destroyed after logging success*: `applyProfile`
clears the SlotBus and rebuilds strictly from the `slot_N_card` keys, which a
one-shot CLI card deliberately never writes. Slot 7 is now a preference that
falls back to the first free slot (an explicit `--fujinet-slot N` that is
occupied stays a hard error), and the request is remembered so
`plugSlotsFromSettings()` reproduces it on every rebuild — which also puts it
back *before* `applyProfile`'s cold boot, so the autostart scan still finds it
on the first pass.

**The ESC/P head double-fed every CR+LF.** `processCommandChar` keeps a
"the previous byte was a CR we line-fed for" latch, but sets it *after* routing
to the Epson parser — so that head never maintained it. Every CR+LF line fed
the platen twice and Auto mode could never latch off. Most visible in the screen
dump, whose bands end with `ESC 3 24 \r \n`: bands computed to abut at 1/9"
landed 2/9" apart, a white seam between every one and a dump twice as long.
`testBandsAbut` only ever covered the C. Itoh head, which is how it survived.
While there: `ESC N n` past the form length collapsed the bottom margin to zero
so every line feed ejected a blank sheet (the same failure `ESC H 0000` was
already guarded against), and `ESC C` clamped to the 69" paper maximum instead
of the mounted sheet, silently clipping a 14"-fanfold job on a Letter tray.

**Printer sound: the envelope never decayed.** The attack/decay phase was
inferred from `env < peak` with no flag, and every grain shape here has an
attack step larger than one decay step — so the comparison flipped back to
attack the instant a decay step lowered `env`. The envelope locked into a
two-frame oscillation just under the peak and every voice became a flat-top
burst cut off dead at `end`. Only a grain shorter than ~7.5 ms would ever have
decayed; the shortest constant is 11 ms. Phase is now explicit.

**…and the sound was inaudible or dangling anyway.** It was registered once in
the constructor, but both slot-rebuild paths call `unregisterAllAudioSources()`
and only re-register *card-owned* sources — so any profile switch or Slot Config
"Apply" silenced the printer permanently, including the switch the constructor
itself performs when the saved profile differs from the ROM auto-probe. It was
also never unregistered: `printerSound` is destroyed ~16 members before the
`AudioDevice` that drains the callback thread, so quitting dereferenced a
dangling vptr from the audio callback. Registration moved into
`plugSlotsFromSettings()`, and `~MainWindow` now unregisters everything right
after `controller->stop()`. The mixer finally has a **Printer** row, which its
own registration comment had been promising all along.

**The print-history PNG encode was on the render thread.** `addPage` expanded a
Letter page at 144 dpi to a 7.76 MB RGBA buffer and called `stbi_write_png`
inline, from `pumpImageWriter` — 99-143 ms measured, six to eight dropped
frames per ejected sheet, and `ImageWriter::tick` allows four ejects in one
tick, so a form-feed catch-up froze the UI for half a second. That is a
regression against the budget `ImageWriter.h` already defends (the spool drain
was moved out of `queueBytes` for exactly this reason). A single writer thread
now does the conversion and the deflate; the index and the page list stay
synchronous so the panel sees the row on the next frame, a page still in the
queue is served straight from it, and the destructor drains rather than
discards. Deleting flushes first, so a file cannot be removed and then
recreated as an orphan.

**Smaller ones.** The history panel's texture cache was keyed on a bare list
index, so archiving a new sheet — which pushes onto the front of a newest-first
list — left the previous page's pixels under the new page's label, permanently.
`shutdown()` read `clientFd_` without a lock while `dropPeer()` closed it
outside one, so the `::shutdown()` could land on a recycled descriptor. And on
Win32, `GenerateConsoleCtrlEvent` cannot reach a `CREATE_NO_WINDOW` child at
all (it has its own hidden console), so its ignored return value meant every
stop burned the full grace period on the UI thread before the hard kill.

## 2026-08-10 — The printer grew real ImageWriter faces, and learned to print the screen

Three phases of `docs/printer_plan.md`, the gap analysis against
[mikedaley/web-a2e](https://github.com/mikedaley/web-a2e) (MIT).

**POM2 was drawing every printed character with a substitute font.** Its own
header said so: glyphs came from the bundled 8×8 CP437 bitmap, which meant
`ESC a` did nothing to the page, "NLQ" was only a *speed*, proportional mode
kept a fixed cell so an `i` took as much paper as an `M`, and the seven
international character sets were nearest-CP437 approximations. Seven real
banks now live in a generated `src/ImageWriterRom.h`, and all four of those
statements are false.

**Provenance, stated rather than implied.** The tables are the dot patterns
Apple *published* in the ImageWriter Technical Reference Appendix C,
transcribed by web-a2e. They are not chip dumps: the transcription is MIT, the
typeface design is Apple's. Same class as the AppleWin SSI263 phoneme blob, and
recorded the same way — in the generated header, in the importer, and in
`docs/lle_vs_hle.md`. **This is a judgement call and it is still open**; the
CP437 fallback was deliberately kept so reverting is one file.

**Two generator bugs that compiled clean and printed wrong.** A backslash at
the end of a `//` comment continues that comment onto the next line — and `$5C`
is `\`, so annotating each row with the character it draws swallowed the row
after it. Every bank ended up with 94 initialisers for a 95-element array,
which is not a diagnostic, just a zero-filled tail: one blank glyph (`~`) per
bank, three layers from the cause. Before that, a brace-walking parser counted
the `{` and `}` inside the row comments.

**Screen dump.** "Print what is on screen" now exists (`printer.dumpscreen` in
the palette). It synthesises the `ESC G` byte stream a Grappler ROM or Print
Shop would have sent and pushes it through the printer's REAL parser — nothing
paints a page pixel — so it obeys the ribbon, the pacing and the paper, and
lands in the tray and the PDF export like any other job. Its test is a round
trip, which pins the scanner and the parser against each other; a dump that
agreed only with itself would be a screenshot with extra steps.

**A power switch that does not eat your page.** POM2 had `powerCycle()`, which
resets everything and clears the sheet. A front-panel power switch is a
different thing: off ignores incoming bytes and KEEPS the paper. Offline
(deselect) does the same while staying powered — the usual reason a real
printer appears to hang. Paper is now settable to any size in ¼" steps, clamped
to the tractor's range, reporting what it actually committed.

**One existing test was asserting the old bug** and had to be recast:
`imagewriter_smoke` pinned a proportional advance of `0.1 + 3/120`, i.e. the
fixed cell proportional mode was wrongly using. It now measures the baseline
advance and asserts `ESC 3` *added* 3/120" — which is what that test's comment
always claimed it was about, and it no longer pins font data.

**Three heads, by table not by hierarchy.** The ImageWriter I and the Apple DMP
(a rebadged C. Itoh 8510) now exist alongside the II. The plan called for
splitting `ImageWriter` into a base class with per-model subclasses; reading the
reference implementation's two model classes settled it otherwise — they are
almost entirely overrides that RETURN DATA (which ROM banks exist, colour
ribbon or not, which ESC codes have no hardware, power-on pitch, carriage
rate). So POM2 got `IwModelProfile`: one struct, three rows. A 1459-line
heavily-tested class stayed untouched and the whole suite stayed green.

Two things worth remembering from it. An ESC code a head does not have is
consumed *with its parameters* and dropped — but the early return must still
clear `escCmd_`, or the parser stays armed and swallows the rest of the job
(mine did, and the symptom was a printer that went silent after one command).
And the ImageWriter I / DMP character banks turn out to be **byte-identical to
the II's** upstream, which is stated in the test rather than asserted around:
POM2 keeps them separate so a future divergence lands by itself.

**And the Epson FX-80**, which is a different lineage and got a second parser
rather than a capability mask. The two grammars collide outright — `ESC G` is
*graphics* on the C. Itoh family and *double-strike* on ESC/P — and the test
asserts exactly that: the same four bytes leave 0 dots on one head and 86 on
the other. Its graphics take two binary count bytes with bit 7 as the top dot,
both the opposite of the C. Itoh spelling, which is the kind of mistake that
still looks like a picture, so it is pinned by a round trip.

Commands POM2 does not implement there (user-defined characters, the vertical
forms unit, nine-pin graphics, tab lists, margins) are consumed **with their
parameters**. That is the whole difference between "a missing feature" and
"gibberish on the page", and it is why a partial ESC/P parser is worth doing
carefully rather than quickly.

Font provenance settled the same day: POM2 keeps the web-a2e tables under their
MIT grant, with the source manual named in the generated header.

**And the printer makes a noise now.** Synthesised, not sampled — which is a
first for POM2's audio, and not a shortcut: the floppy sounds are MAME's WAV
set, but no free ImageWriter set exists, and the reference project ships no
audio assets at all (checked rather than assumed — its printer sound is Web
Audio synthesis).

The model it ports is worth stating because it is what makes it sound right: a
dot-matrix impact is a short broadband NOISE click, not a tone, so every voice
is bandpassed noise with a wide Q — an oscillator is what makes a printer
emulation *sing* instead of clack. One grain per character (11 ms) or line feed
(40 ms), and the grains are **spaced along the audio timeline** rather than all
firing when the event arrived, so at print rate they overlap into the
continuous buzz while a lone character stays a tick. Print density drives the
texture for free.

The detail that matters most is a cap on how far ahead the scheduling cursor
may run. A full-black screen dump is tens of thousands of strikes in one UI
frame; at 5 ms apart that queues *100 seconds* of rattle that keeps playing
long after the page is done. Capped, the burst just thins — the test fires
20 000 strikes and asserts silence within ~0.2 s.

Also a correction to the plan: § 9 said this needed `emuCycles` stamping like
the floppy. It does not. The floppy needs it because the guest drives the
stepper and disk turbo collapses the gaps; the ImageWriter paces itself in wall
clock, so its events are already in real time.

**And printouts now survive quitting.** Every ejected sheet is written to
`printouts/history/` as a PNG with the printer, ribbon and paper it was made
on; the ImageWriter panel lists them and puts one back on the canvas when you
click it. The index is tab-separated text rather than the JSON the plan asked
for — POM2 has no JSON parser, and adding one to read a few dozen lines would
be the tail wagging the dog.

The trap worth remembering: the archiver has to compare against the printer's
MONOTONIC eject counter, not the size of its page stack. That stack is capped
at 32 and reused, so a form-feed burst between two frames pushes sheets off it
and a count-based archiver loses them without a word. It now logs how many it
missed instead. Three smaller ones — archiving must run on the path where no
card is feeding (a job already in the buffer keeps ejecting), the filename
counter must resume across sessions or a reload clobbers an existing PNG, and
the index is written to a temp file and renamed so a crash mid-write cannot
truncate it.

**That completes `docs/printer_plan.md` — all six phases.** POM2's printer went
from one head drawing substitute glyphs to four heads with their real faces, a
screen dump, a front panel, mechanical sound and a durable history.

## 2026-08-10 — FujiNet, over loopback TCP *or* a real board on USB

POM2 can now use a **FujiNet** ([fujinet.online](https://fujinet.online/)) —
the ESP32 peripheral whose `N:` device gives an 8-bit machine HTTP(S), TNFS,
FTP, SSH, Telnet and a JSON parser without the guest running any TCP/IP itself.

**Why one card and not six.** On the Apple II *every* FujiNet function is a
SmartPort unit: block storage, the network device, the clock, the printer, the
modem, CP/M. So POM2 relays the SmartPort protocol and gets all of them at
once, instead of porting each device. `FujiNetCard` is a **relay, not an
emulation** — it presents a SmartPort controller to the guest and forwards
every call verbatim over the FujiNet project's published **SP-over-SLIP**
protocol.

**Two transports, both shipping.** Loopback TCP 1985 for a FujiNet *desktop
build* running alongside POM2 (the everyday case, and the port an existing
FujiNet configuration already uses), and **USB CDC-ACM** for a *physical
board*. The spec blesses both — "any medium providing a transparent, duplex,
lossless byte stream" — and the serial path is what lets POM2 drive real
hardware rather than only a software peer.

**MAME is not the source of truth here, and the code says so.** MAME has no
FujiNet device. The references are the project's wiki spec (revision of
2025-01-25) and the FujiNet fork of AppleWin, which is GPL-2.0-**or-later** and
therefore GPLv3-compatible; it was consulted, not copied.

**The two bugs worth remembering.**

- *The stack index must wrap inside page 1.* A SmartPort call is `JSR $Cn0D`
  followed by three inline bytes, so the host has to rewrite the pushed return
  address to step over them. AppleWin's `regs.sp` is already a full `$01xx`
  address; POM2's `getStackPointer()` is the 8-bit register, so
  `0x0100 + ((sp + 1) & 0xFF)` is mandatory. Without the mask, a call made with
  SP near the bottom of the page writes the fixed-up address to `$0200` —
  corrupting unrelated memory *and* leaving the stale address on the stack.
  Pinned with `SP = $01`.
- *`std::mutex` is not recursive.* `transact()` discovers a dead peer while it
  already holds the call lock, so peer teardown exists in two forms
  (`peerLostLocked()` / `handlePeerLost()`). The first version deadlocked the
  CPU thread with the emulated 6502 parked mid-SmartPort-call — caught by
  `sp_over_slip_link`'s clean-shutdown case, and only intermittently.

**The ESP32 auto-reset trap, for anything that ever opens a serial port.**
Every ESP32 USB bridge wires DTR → EN (reset) and RTS → IO0 (boot select)
through the standard auto-reset circuit — that is how `esptool` enters the
bootloader with no button press. An `open()` that lets the OS assert them
**reboots the user's FujiNet every time POM2 opens the port**, and clearing
`HUPCL` is what stops *quitting* POM2 from doing the same. That defence lives
in the new `SerialPort` host primitive (POSIX termios / Win32 DCB), not in the
FujiNet code, so anything else that opens a device inherits it.

**Boot fails safe.** With no FujiNet answering, the card's ROM continues the
autostart slot scan (`JMP $FABA`) rather than erroring — otherwise a FujiNet
card in slot 7 would break booting from the Disk II in slot 6 whenever the
FujiNet was not running. Slot 7 is the default precisely because the //e scans
it first, so a machine *with* a FujiNet boots straight into its CONFIG.

**What it deliberately does not do.** Rewind does not rewind the peer — blocks
it wrote stay written, HTTP requests stay made; the card only resynchronises
its sequence number. And it is II+ / //e only: a //c's forced INTCXROM masks
all slot ROM, and on real hardware the FujiNet *is* the disk-port SmartPort,
which is a different integration.

Timeout defaults to **250 ms** — the emulated 6502 is parked inside its `JSR`
for the whole round trip, so that is how long the machine stalls when the peer
goes quiet: ~50× headroom over a real USB round trip, one dropped frame instead
of a full second when something dies.

Four new pinned tests (`slip_framer`, `serial_port`, `sp_over_slip_link`,
`fujinet_card`), layered so each fails for exactly one reason. Detail →
`DEV.md` § FujiNet; design and remaining phases → `docs/fujinet_plan.md`.

**Phase 2 — the printer reaches POM2's paper.** Bytes the guest writes to the
FujiNet's printer unit are also rendered by POM2's own ImageWriter, through the
same spool contract `PrinterCard` uses. Identifying that unit turned out to be
the interesting part: the firmware's `iwmPrinter::create_dib_reply_packet`
labels the printer `SP_TYPE_BYTE_FUJINET_MODEM`, so **the printer advertises
itself as a modem**. POM2 keys on the DIB *name* instead, and the smoke test
reproduces the upstream bug so nobody "cleans up" the workaround.

Two Phase-2 items were dropped on purpose, not forgotten. Surfacing the peer's
disks as mountable bays would add Mount/Eject buttons that cannot work — the
images live on the FujiNet's own storage — and the panel's device table already
answers the question. Bridging its modem to the SSC telnet path would fight a
stack that already reaches the network.

**Phase 3 — POM2 starts the FujiNet for you.** Not by vendoring the firmware:
the reference recipe for that is 856 lines of bash that builds mbedTLS from
source and applies a dozen patches anchored to exact upstream text, and it buys
nothing except not installing a binary. POM2 launches an existing FujiNet
desktop build instead, and reaps it on exit. It does **not** rewrite the
program's `fnconfig.ini` (WiFi credentials live there, and its Apple default is
already 127.0.0.1:1985).

The trap worth remembering: killing only the direct child leaves *its* children
alive holding POM2's stdout pipe. That showed up as a test passing by hand and
hanging under ctest; in the field it would be a stray FujiNet still holding port
1985 after POM2 "stopped" it. `stop()` signals the process group.

## 2026-08-08 — Core 2× faster on the same output, and a Raspberry Pi build recipe

Ported the optimisation campaign method from **NeoST** (POM2's sibling Atari ST
emulator, `../neost`): a deterministic headless subject, callgrind, then PGO.
Full write-up with the before/after numbers in **`docs/PERFORMANCE.md`**.
Everything below is **output-identical** — 166/166 tests green and byte-equal
RAM/framebuffer hashes on every workload measured.

**`pom2_bench` (new target).** `pom2_headless` cannot be profiled: worker
thread, wall-clock pacing, audio device, waits for a human. `pom2_bench` runs
exactly N frames of `cyclesPerFrame` with no threads, no audio, no sockets and
no pacing, so two runs retire the *same instruction count* — which is what makes
before/after comparisons trustworthy. It prints FNV-1a hashes of RAM and of the
framebuffer: an optimisation that moves either is a bug, not an optimisation.
It doubles as the PGO training driver. ⚠ Its own per-frame framebuffer hash was
17 % of the very first profile taken — hence `--hash-all` being opt-in.

**Flux lookup: resume the search instead of redoing it (−33 % on disk).**
`DiskImage::getNextTransition` ran a full `std::lower_bound` over the track's
flux array on every call — tens of thousands of events, ~16 probes — while
`DiskIICard::lssSync` calls it once per flux event for as long as the motor
turns. Together they were **42 %** of a disk-active profile. The class now
remembers the previous index and resumes from it. The hint is *verified*, never
trusted: the fast path re-checks that the remembered index really is the lower
bound (two comparisons) before using it, so a write splice, an eject or a
snapshot restore just fails the check and falls back to the binary search.
Nothing has to invalidate it — an invalidation you can forget at one call site
is a latent correctness bug; a self-verifying hint cannot be.

**Bus reads decided in the header (−18 % on CPU-bound loads).** `Memory::memRead`
plus the `languageCardRead` it tail-calls were **35 %** of a ROM-banner profile,
most of it the out-of-line call around what is usually one array index. The two
hot cases now inline in `Memory.h` — main RAM below `$C000` on a non-//e, and
`$D000-$FFFF` when the language card maps ROM — and everything else goes to
`memReadSlow` (the original body, untouched). The second case is the trap NeoST
also hit: fast-pathing RAM alone is worth a few percent, because *the ROM is
where the code executes from*. The //e aux/main decision moved into one shared
inline helper (`iieReadFromAux`) rather than being copied.

**PGO is the single biggest win, and it touches no emulation code**: −39 % / −29 %
on top of the above. `packaging/raspberry/build_native_pi.sh --pgo` does the two
passes, `pgo_train.sh` sweeps ][+ and //e, PAL and NTSC, every video pipeline and
a 5.25" boot — *a too-narrow profile is worse than none*, it marks live code cold.
Two traps that cost the entire gain **in silence**, both closed by the script:
GCC names each `.gcda` after the object's absolute path (so both passes share one
build dir), and — POM2-specific — the training driver is `pom2_bench` while the
shipped binary is `pom2_imgui`, two different object directories for the same
sources, so the profiles are copied across and the build **fails** if any of
M6502/Memory/DiskIICard/DiskImage/Apple2Display came out untrained. Also
`pi_tuning.sh` (governor, IRQ pinning, swap) and `packaging/raspberry/README.md`.
⚠ The Pi-specific parts are not yet exercised on real hardware; the build recipe
and both traps were validated end-to-end on x86-64.

**The Pi never has to compile any of this.** `.github/workflows/pi400.yml`
(`workflow_dispatch`, `-f mcpu=cortex-a72|a76|a53`) runs both passes *and* the
training on GitHub's native ARM64 runner inside a `debian:bookworm` container —
bookworm because Raspberry Pi OS *is* bookworm and building on the runner's own
userland would stamp GLIBC_2.39, which starts on no Pi. One build, two packages,
no recompilation: a `-pi400-aarch64.AppImage` (Pi OS with a desktop) and a
`-pi400-aarch64.tar.gz` laid out like `cmake --install` (Pi OS Lite cabinet — no
FUSE). The name carries the core tag on purpose: a release's publish job
flattens every artifact into one directory, and a package named like the generic
aarch64 one would overwrite it in silence. The job verifies rather than hopes —
ET_EXEC runtime, aarch64, glibc ≤ 2.36, GLES-only (desktop libGL on a Pi is the
software rasteriser: a silent ~2 fps regression, not a link error), ROMs in both
packages. The "PGO changes layout, never semantics" claim is *enforced* rather
than asserted: the container runs a fixed workload with the pass-1 (instrumented,
unoptimised) binary, repeats it with the final PGO+LTO one on the same machine,
and **fails the build** if a cycle count or an output hash moved. Getting that
right needed one non-obvious guard — `LC_ALL=C` on the "first `.dsk`" glob,
because collation differs between a French desktop and a C-locale runner, so
without it the two sides can compare *different disks* and agree by accident.

Footnote: **LTO now measures ~0 %** on these workloads — the header inlining did
by hand exactly the cross-TU call LTO was recovering. Kept anyway, as a guard.

## 2026-08-08 — CRT/NTSC shader: negotiate the GLSL dialect instead of demanding 1.50

`OpenGLShader.cpp` hardcoded `#version 150`, so on any desktop-GL driver capped
below it the whole effect stack died with *"GLSL 1.50 is not supported.
Supported versions are: 1.10, 1.20, 1.30, 1.40…"*. Mesa's V3D (Raspberry Pi)
caps *desktop* GL at 3.1 = GLSL 1.40; old llvmpipe and several VM drivers land
in the same place. Nothing needed rewriting — POM2's shader bodies only use
1.30 constructs (`in`/`out`, `texture()`, `fwidth()`); they were merely *asking*
for 1.50.

The dialect is now read from `GL_SHADING_LANGUAGE_VERSION` and tried in cascade
**150 → 140 → 130** (`300 es` unchanged where the context is GLES — the Pi's own
AppImage tier, WASM; `150` unchanged on macOS, whose core profile has no lower
rung). The cascade is a net rather than an affectation: a driver can advertise a
version and still refuse it in *this* context, and only a real compile settles
it. Intermediate failures are silent and `errorOut` is cleared on success, or
the panel would report "shader unavailable" with the stack already running. One
startup line says what was picked and what the driver claims: `[NTSC] GLSL 140
(driver: 1.40)`. Verified under Mesa llvmpipe with forced versions — 4.50 → 150,
**1.40 → 140 (the Pi case)**, 1.30 → 130, each linking cleanly. ⚠ `MESA_*_OVERRIDE`
is ignored by the NVIDIA driver; `LIBGL_ALWAYS_SOFTWARE=1
__GLX_VENDOR_LIBRARY_NAME=mesa` is required to test this at all.
## 2026-08-08 — Disk & floppy management sweep: eight defects around the LSS

An audit of the media-*management* layer — everything that surrounds the
bit-level data path rather than living inside it. The LSS itself came
through clean; every finding below is state that is set on one edge and
never unset, or a persistence path with no owner.

**The 13-sector boot PROM latched forever.** `insertDisk` recomputed
`serving13_` (341-0009 boot PROM at `$Cn00`) and `useBitLss` from the
mounted set; `ejectDisk` recomputed neither. They describe *what is
mounted*, so an eject has to be able to clear them: pull a DOS 3.2 disk
with a stock 16-sector disk in the other drive and the card kept answering
`$Cn00` out of the 13-sector PROM, so the remaining disk could not boot.
Both are now derived by one `refreshMediaDerivedState()` called from insert
**and** eject. Demoting `useBitLss` mid-spin also parks the sequencer —
the legacy 32-cycle gate has no notion of `active`, so `$C0n8` would have
cleared `motorOn` while `active` stayed MODE_ACTIVE and a later WOZ insert
would have resumed the LSS on a drive whose motor was off.

**Quitting threw away the session's disk writes.** `~MainWindow` flushed
3.5" media with a comment claiming to mirror "the Disk II save-on-shutdown
hook". There was no such hook. Write-back was flushed on eject and on swap
only, so with write-back enabled a whole session of DOS `SAVE`s died with
the process unless the user happened to eject first. `DiskIICard` gained
`flushPendingWrites()`, called from `~MainWindow` (ordered before the
settings write) and from `~DiskIICard`, which also covers profile switching
— that rebuilds the slot cards without ejecting anything.

**A read-only image file mounted as a writable disk.** `Block512Backing`
and `Disk35Image` both probe host writability at load and mount
write-protected when the probe fails. `DiskImage` — the 5.25" path — did
not, and the consequence was worse than the RAM-only write loss those two
were fixed for: `writeFileAtomic` renames a sibling temp over the target,
and POSIX `rename` needs write permission on the *directory*, not on the
file. So a `chmod 444` .dsk was replaced anyway, and its mode reset to the
umask default. The user's deliberate read-only marking was silently
defeated. Same probe now, folded in after the per-format loaders (they all
reset `fileWriteProtected` on entry) so it can't be cleared.

**Write-back reset the image's permissions.** `writeFileAtomic` didn't
carry the original file's mode across the rename — the one thing
`Disk35Image::saveDirty`'s copy of the helper already did right (and which
`TODO.md` flags for the eventual consolidation). Any non-default mode on a
disk image was rewritten to the process umask on the first save.

**Ctrl-Reset left the motor humming.** `onReset()` cleared `motorOn`
behind the sound sink's back. `FloppySoundDevice` only silences its spin
loop on an explicit `motor(false)` command, and after a reset nothing else
would ever emit one — so a reset mid-read left the drive hum looping for
the rest of the session. Reset now emits the matching motor-off.

**A failed insert reported the previous disk's path.** The per-format
loaders set `path` only on success and every failure path leaves
`loaded == false`, so a rejected image left the drive advertising
"empty, but here is the last disk's path". Harmless in the UI (every call
site guards on `isDiskLoaded`) but the AI control server publishes `path`
and `loaded` as independent JSON fields, where the stale pair reads as a
mounted disk. `loadFile` clears `path` up front now.

**"Boot disk" didn't tell the IWM the head had moved.** `seekTrack0()`
yanks both heads back to track 0 but was the one head-moving path that
skipped `pushIwmFloppy()`, so on //c / //c+ the on-board IWM kept reading
the pre-boot quarter-track.

**The Disk II panel's LED could never go amber.** It passed a hard-coded
`wp=false` to the shared `statusLed`, so it was the one media panel that
never showed the write-protected colour — while its own body text right
underneath already explained *why* the disk was read-only. It now uses the
same sense the guest sees (medium WP flag, or write-back off).

Pinned by `tests/disk_media_state_test.cpp` (`disk_media_state`), which
reproduces five of the seven against the unfixed code — the read-only case
self-skips when the mode bits are bypassed (running as root).

## 2026-08-02 — Bundle the full `roms/` tree in release artifacts

Packaging previously shipped only `roms/floppy_samples/` plus a drop-here
README, while the copyrighted dumps were already tracked in the public repo.
That split is closed: `cmake --install`, the Linux tarball/AppImage/.deb,
the macOS `.app`/`.dmg`, and the Windows zip all copy the entire `roms/`
directory so a release boots without a separate ROM drop. Docs and
`packaging/roms_README.txt` match.

## 2026-08-02 — SmartPort access LEDs, and the disk turbo they were hiding

The status-bar row shipped earlier today left SmartPort LEDs dark because
the units exposed no activity signal. Wiring one up turned out to uncover a
bigger miss than the lamp.

`SmartPortUnit` now carries the same hysteretic counter `Block512Backing`
uses — a block access sets it, the host bleeds it off one step per frame.
It lives on the base class and `SmartPortCard::noteAccess()` bumps it, so
one site covers 3.5" and HDV units alike; that function already existed as
the audible-motor hook and already fired at all three block-dispatch sites.
Two details worth keeping: the bump goes **before** `noteAccess`'s
`sound_` early-out, because a machine with no FloppySoundDevice still has
an LED; and the unit is passed explicitly, because the SmartPort `$Cn0D`
dispatch addresses a unit by number, which need not be the one the legacy
streaming registers have selected.

`SmartPortHdvUnit` does wrap a `Block512Backing` with a counter of its own,
so it looked like the signal was already there for free. It was not usable:
nothing decays it. The host's decay loop walks `ProDOSBlockCard`
implementers, and `SmartPortCard` implements `MountableMediaCard` instead —
so reporting that counter would have latched the lamp on permanently after
the first access.

**Which is the actual find.** That same gap meant SmartPort media never
counted toward `anyBusy`, so it sat outside **disk turbo** entirely. On
//c / //c+ / //c PAL the built-in slot-5 SmartPort *is* the boot path for
3.5" and HDV, so precisely the machines that depend on it were loading at
1 MHz while an HDV card in a //e got the ~60× speed-up. The decay loop now
covers SmartPort units and they count toward turbo eligibility.

`MediaBayInfo` gained a `busy` field, so the status bar reads activity
uniformly per bay and the old `dynamic_cast<ProDOSBlockCard*>` special case
is gone — and a SmartPort's two units light independently, which a
card-wide flag could not have expressed. Pinned by `smartport_card_smoke`:
a read lights only the unit that was read, with no sound device attached,
and the lamp decays on its own within a bounded number of frames.

## 2026-08-02 — Status bar: drop the MHz readout, show every mounted volume

The achieved-clock readout is gone, along with its sampling state — the
toolbar still shows the requested budget, and the measured figure was
costing a `stateMutex` acquisition every frame to tell most users something
they never acted on.

The media row used to show exactly **one** entry: whichever Disk II was
spinning, else the primary drive, else a mounted HDV as a fallback. A
machine with two floppies, a CFFA and a SmartPort showed one of them. It now
walks the SlotBus slot by slot — each Disk II contributes both drives when
loaded, and every card implementing `MountableMediaCard` contributes each of
its bays, so multi-instance cards appear too (the named aliases in
`MainWindow` only ever remember one card per kind, which is what limited the
old code). Entries are added while there is room and dropped silently past
that, in bus order so a given machine's row does not reshuffle as drives
spin up.

The LEDs mean different things per bay, deliberately: a Disk II lights on
real spindle motion, a block device has no mechanics and bleeds off
`Block512Backing`'s activity counter instead, and SmartPort units expose no
activity signal at all, so theirs stays dark. An honest dark LED beats one
that never means anything — giving SmartPort a real one needs an activity
signal on the card first.

The row is built as a value snapshot under `stateMutex` and drawn with the
lock released — the same discipline the sibling panel snapshots were just
given. Both halves are load-bearing: `getDiskPath()` returns a reference
into live `DiskImage` state that the AI server's HTTP thread rewrites on
`/disk` and `/eject`, and holding `stateMutex` across ImGui calls is exactly
what deadlocked the memory viewer.

`indicatorDot` gained an explicit centring height. Inside a menu bar ImGui
applies no frame padding to a bare `Text()`, so every item sits at the top
of the bar; text carries that off, a circle does not, and the drive light
rode visibly high. The status bar now passes `GetFrameHeight()`.

## 2026-08-02 — `pom2_headless` did not link Winsock

Found by the first Windows package build since 7e7d8de enabled host sockets
there: 17 unresolved Winsock externals out of `SuperSerialCard.obj`. The GUI
target had been given `ws2_32`, the headless one had not — before that
commit `POM2_HAS_SOCKETS` was 0 on Windows and the socket code compiled out
entirely, so the omission was invisible. Worth noting how it surfaced: the
compile succeeded, so the `#ifdef _WIN32` branches themselves were sound;
only the link failed. CI does not build Windows — only the release workflow
does, and it had not run since.

## 2026-08-02 — Bug-hunt sweep: 21 defects across ten subsystems

A ten-way parallel audit of the whole tree, then a fix pass. Framing that
matters for reading the rest: **the suite was 158/158 green before this,
and stayed green throughout** — every defect below sat in a gap the tests
did not cover. An ASan+UBSan build of all 156 test binaries, ~24 000
hostile-input cases against the image parsers and ~6 M random CPU
instructions produced **zero** diagnostics. Everything here came out of
reading code and then proving it with a probe. The lesson is not that
dynamic analysis is weak but that it is blind where this codebase actually
breaks: the GUI, and the seams between threads.

### The four that could bite a user hard

**Print now, free later.** The ImageWriter panel bound `const Page&` to a
completed sheet at the top of the frame, then let "Print now" call
`flushPending()` before `uploadPage()` read it — an eject `push_back()`s
into `pages_` and reallocates. Heap use-after-free, confirmed under ASan,
two clicks from ordinary multi-page printing. At the 32-sheet cap the same
call `erase()`s the front instead, silently renaming every index. What
makes this worth remembering: the sibling buttons "Reset printer" and
"Clear all" were deliberately placed *before* the reference is taken, with
a comment saying why — the discipline existed and one button escaped it.
So the fix is not a reorder: sheet selection is now derived state resolved
on demand and re-resolved after every call into the printer, including the
paper-size / DPI / speed controls, which eject too and were a second
latent instance nobody had noticed.

**Editing a byte froze the emulator.** `renderMemoryViewerWindow()` held
`stateMutex` across `memViewer->render()`, and the write callback re-locked
that same non-recursive `std::mutex`. The UI thread deadlocked *while
holding* the lock the CPU worker needs, so the whole machine stopped, not
just the panel — the exact hazard `MainWindow.cpp` already documents for
`eject35()`. Edits, undo and redo are now staged and drained by
`flushPendingWrites()` after the lock is released, with the contract
written on both methods. Found while pinning it: the inline editor also
*never committed*. `SetKeyboardFocusHere()` posts a nav request ImGui
resolves in `EndFrame`, so `IsItemActive()` is false on the frame that
requests focus and the old cancel closed the box one frame after the
double-click. Byte editing had been quietly dead, which is why the
deadlock went unreported.

**Two Mockingboards, one alias.** `"mockingboard"` (A/C) and
`"mockingboard_c"` (Sound II) are distinct catalog keys, so the duplicate
guard — which compares key strings — lets both plug. But `plugMockingboard`
overwrites a single `mockingboardCard` raw alias, and teardown unregistered
only that one before `slotBus().clear()` destroyed both cards. The
miniaudio thread then dispatched through freed memory every ~5 ms.
`MainWindow` now keeps an inventory of every `AudioSource` it registered,
so a future card cannot reintroduce this by adding a second catalog key.

**A ProDOS volume that never stops unpacking.** `decodeVolumeToFolder`
walked a *guest-writable* image as if it were a tree. A subdir entry's
`key_pointer` was only range-checked, so one pointing back at an ancestor
made the graph cyclic, explored to depth 16 with a fan-out of 13 slots ×
256 chained blocks — each visit doing a real `create_directories`. Measured
before the fix: fan-out 2 produced **262 143 host directories in 10.8 s**;
fan-out 12 was still running when killed at 25 s. It sits on the eject/save
path, so POM2 could never quit while it filled the user's disk. A
malicious image is not required — a crashing guest that scribbles on block
2 will do. The depth cap was never the right tool because it bounds no
fan-out; the walk now carries a global set of expanded blocks (so each is
walked at most once) plus a hard directory budget, and reports what it
skipped instead of emitting a partial tree in silence.

### Mockingboard: two user-reported symptoms, one root shape

Both came from the same place — the audio thread's replay cursor runs
~40 ms behind CPU-now on purpose, and anything that reaches it *out of
band* arrives at the wrong time.

**Rewind silenced the card.** The cycle-stamped queue had no way to learn
the CPU timeline had jumped. After a rewind or snapshot load, `pending`
was full of pre-rewind stamps that the front-ordered render loop read as
"not due yet", blocking everything behind them: measured 0.49 s / 2.00 s /
>3 s of total silence at rewind depths of 0.5 / 2 / 5 s. The in-code
comment asserting that `caughtUp` already handled rewind had the reasoning
inverted — it moves the *cursor*; the damage is in the *queue*. A
generation counter now purges both queues and re-primes from the live bank.
Regression from 2c385ce, which removed the old per-callback `pending.clear()`
in favour of a persistent jitter buffer.

**A note hung between DIX demos.** The wholesale bank wipe reached the
audio thread through `ayResetCount_`, i.e. at CPU-now — so the ~40 ms of
already-queued pre-reset writes replayed *on top of* the zeroed bank and
the board held its last note forever. The trigger is in DIX's own GPLv3
`loader.a`: `RESET_AY`, called at every demo hand-off, silences the board
with nothing but the /RESET strobe and no volume writes. That also explains
why the user saw it only *sometimes* — `CCII_2016` silences by writing 0 to
R8/R9/R10, ordinary stamped writes that were never broken. The strobe now
travels as a `kRegAyReset` event and lands at its true cycle stamp, and
`resetGenerators()` was completed to MAME's full `ay8910_reset_ym`: it had
only reseeded the noise LFSR, so a chip mid-envelope or mid-tone-phase
carried that state across a reset. PhasorCard inherits the same fix.

**And the bass.** `AyPsgSynth.h` justified a 1-pole DC blocker by citing
MAME's `audio_effects/filter.cpp`. That citation was wrong: MAME's is a
**2-pole Butterworth biquad** (`DEFAULT_Q = 0.7071067f`, high-pass on by
default), which is maximally flat where a 1-pole is already drooping. Same
corner, different passband. Ported verbatim; measured recovery of 0.77 dB
at 27.5 Hz, 0.46 dB at 55 Hz, 0.23 dB at 82.5 Hz, now tracking the analytic
MAME response to within 0.01 dB. Stated plainly because it matters for the
next person chasing this: **≤0.8 dB below 80 Hz will not on its own be what
anyone hears as "missing bass."** The volume table (within 0.0007 of
Westcott's measurements), the linear channel sum, the box integrator (sinc
gain 1.0000 below 100 Hz) and the stereo split were all measured against
MAME and found already correct. If the perception persists, the next place
to look is the mixer's `/3` normalisation against MAME's `0.5` route gain —
downstream of the card, not inside the AY synthesis.

### Timing and hardware parity

**The VBL frame phase froze after an NTSC↔PAL switch.** `advanceCycles`
tracks the start-of-frame cycle incrementally (6e9e0f2), and the invariant
is `vblFrameBase_ % frameCycles == 0`. `setVideoStandard()` changes
`frameCycles` on a running machine and nothing re-derived the base — and
because 17030 and 20280 sit within a factor of two, the rollover branch can
never notice, so the stale residue persists forever. Boot NTSC, load the
//c PAL profile, and the VBL edge lands on scanline 252 instead of 192,
disagreeing with `$C019`, `pushVideoEventLocked` and `frameCycleToPos`,
which all take a true modulo. That is precisely the 50 Hz frame sync the
French Touch / DIX demos rely on. The base now re-aligns whenever the
*derived* period moves, so any future input feeding it re-aligns too.

**A plain annunciator poke armed a real IRQ on //c.** The `$C05A`/`$C05B`
→ VBL-mask overlay is a POM2 //e compatibility shim, but it was gated on
`iieMode`, which is also true on //c-class; since IOUDIS resets to *set*,
the MAME-faithful IOU decode was bypassed and the legacy `LDA $C05B` idiom
armed the mask. The arming guard said `iieMode` while the asserting guard
said `iicProfile_` — and on //c, unlike //e, the line really is driven, so
the guest took an unhandled 50/60 Hz IRQ storm through `$FFFE`. `LDA $C05A`
symmetrically ACKed an interrupt a //c guest had legitimately armed. MAME
`apple2e.cpp:1808-1876` keeps DisVBL/EnVBL strictly inside the
`(m_isiic || m_isace500) && !m_ioudis` branch and otherwise falls through
to plain AN0/AN1/AN2.

**The mouse MCU ran 26-50 % fast, and lost its interrupt on rewind.**
`advanceCycles` debited the accumulator by the requested budget while
`mcu.run()` finishes the instruction straddling the edge; with
per-6502-instruction budgets of 4-14 MCU cycles the discarded overshoot was
comparable to the budget itself — measured 2.4906 MCU cycles per bus cycle
against the intended 2.0, now 2.000003. Separately,
`MouseCard::loadSnapshotState` re-derived the slot IRQ from
`pia.irqA() || pia.irqB()`, but this card's IRQ is MCU port B bit 6 —
MAME's `pia_irqa_w`/`pia_irqb_w` (`mouse.cpp:235-240`) are empty stubs, so
that expression was an unconditional `assertIrq(false)` and a rewind taken
mid-MousePaint-handshake killed the mouse until reset. `MouseCardAppleWin`
serialized nothing at all and gained a snapshot blob.

### Networking, and a class of Windows-only divergence

The Windows socket paths from 7e7d8de are new, and three of them were wrong
in ways POSIX hides.

**UDP reads were sized against the ring, not the datagram.**
`recvfrom`'s buffer was `freeRoom - 1`, so with `RMSR $00` a standard
1472-byte reply into a 1 KB ring came back truncated on POSIX — the kernel
discards the remainder and reports *no error*, and the in-band length
stamped into the ring described a datagram the guest never received
(measured on Linux: 1015 bytes stamped "1015" out of 1472). The same call
on Winsock fails with `WSAEMSGSIZE`, which the error arm read as "socket is
dead". UDP now reads into an 8 KB scratch buffer — sized so truncation
cannot pass `ringHasRoomFor` by construction — and drops a datagram the
ring cannot take. TCP stays clamped to the ring, because dropping stream
bytes would tear a hole.

**Errors that describe a packet were killing the socket.** On Winsock an
ICMP port-unreachable from an earlier `sendto` surfaces as `WSAECONNRESET`
on the next `recvfrom` of an *unconnected* UDP socket, so one datagram to a
closed port destroyed the guest's socket. `SIO_UDP_CONNRESET` is now off at
creation and a per-packet error set is tolerated — **for UDP only**, since
on TCP `ECONNRESET` genuinely is the connection dying. Likewise `recvfrom`
returning 0 is a zero-length datagram, not a close; it used to fall into an
arm that destroyed the socket, an arm which turned out to be reachable
*only* in that case.

**`SO_REUSEADDR` means the opposite of what it means on POSIX.** On
Winsock it lets a second socket bind an address another is already
listening on, and the later binder wins new connections — so any local
process could take over the AI control listener and collect its token. The
intent now goes through `setListenerBindPolicy()`: `SO_REUSEADDR` on POSIX
for the wanted `TIME_WAIT` relaxation, `SO_EXCLUSIVEADDRUSE` on Windows.
These branches were verified by reading only — there is no Windows host or
mingw cross-compiler here — but the new test is written entirely through
`SocketCompat.h`, so it will exercise them for real the day a Windows CI
exists.

### Smaller, but real

- **Screen capture froze a soft 560-wide text screen.**
  `demodCompositeForCapture()` rewrites the framebuffer *after* `render()`
  has returned, and the static-text skip key survived it. The fix is
  structural rather than a call added at the guilty site: the key is now
  published at the end of `render()` behind an RAII, `useFrame80` was
  renamed `useFrame80_` so a bare assignment no longer compiles, and every
  mutation invalidates. Verified the optimisation still bites — 400 static
  text frames cost 0.09 ms skipping vs 52.6 ms repainting, a 582× ratio now
  pinned by the test.
- **`ESC R` / `ESC V` / `ESC U` froze the UI.** All three expanded a whole
  run inside the single byte that completed the sequence — past both
  catch-up budgets, which are only checked *between* bytes, and free of
  credit, since `byteCost` returns 0 while `numParam_ < neededParam_`.
  `PRINT CHR$(27);"R999";CHR$(12)` cost 773 ms in one tick at defaults and
  13.8 s at 288 dpi/Ledger while wiping the 32-sheet tray; `ESC U 9999` cost
  1.4 s per catch-up tick. A repeat is now resumable state, worst tick
  0.9 ms, and the count is never clamped — a real printer does print 999
  characters.
- **Disk-path snapshots were taken unlocked, by reference.**
  `getDiskPath()` returns a view into live `DiskImage` state, and
  `controller->stop()` parks only the CPU worker — the AI server's HTTP
  thread keeps serving `/disk` and rewrites those strings. Three snapshot
  builders now lock and copy, matching the sibling panels that already did.
- **`saveScreenshot` bypassed `demodMutex`**, running the same ~1-2 ms
  demod as the AI server's `/screen.ppm` handler with no shared lock.
- **`Disk35Image::saveDirty` truncated the user's 800K image in place** —
  the failure `DiskImage::saveDirty` was hardened against. Now temp file +
  `rename`, permissions carried across.
- **`ClockCard` raced the UI over `std::localtime`'s shared static `tm`**;
  switched to `localtime_r`, as `NoSlotClock` already did. The two UI-side
  callers were converted too.
- **The W5100's `$8000+` mirror was asymmetric** — writes masked before the
  range test, reads did not, so a guest reading `$8403` got plain memory
  instead of socket 0's status.
- **The Audio Mixer's pan slider sat ~100 px off-screen** at the panel's
  default size: the row's hard-coded pixel offsets did not follow
  `uiScale_`/`dpiScale_` while the text and padding did. Widths are now
  font-relative.

### The WASM build had been red since 7e7d8de

Not from this sweep — found while checking CI before pushing it. The Windows
socket commit left `W5100Device.cpp` naming `htons` / `ntohs` /
`SOCK_STREAM` / `IPPROTO_TCP` outside the `POM2_HAS_SOCKETS` guard, and
Emscripten compiles that file (MACRAW/IPRAW framing and the SnMR mode switch
go through `NetworkBackend`, not through a socket). Two CI runs in a row had
failed on it while the Linux job stayed green, so the tree looked healthier
than it was.

The file's own header comment already claimed `SocketCompat.h` supplied
those symbols "even where no socket is opened" — it did not. It does now:
`pom2::hostToNet16` / `netToHost16` are spelled out as the arithmetic they
are rather than borrowed from `<arpa/inet.h>`, and the protocol selectors
get a `pom2::kSockStream` family that exists in both builds. Code that opens
no socket no longer depends on the socket stack. Verified locally with
emsdk, not just left to CI: `build_wasm.sh` produces `POM2.{js,wasm}` again,
and the committed `wasm/` bundle is refreshed with it.

### Notes for next time

`disk_path_snapshot` was flaky on arrival — a reader loop that re-locks
immediately can starve the writer to zero mutations under `ctest -j`,
because `std::mutex` is not fair. Bounding the *writer* and letting the
reader run until it finishes makes the interleaving the thing under test
instead of the scheduler's generosity. Worth copying into any future
thread-stress test here.

Still open, and deliberately not done in this pass: a **ThreadSanitizer**
run over `EmulationController` / `stateMutex` / the audio thread, which is
where ASan is structurally blind and where this sweep's own findings
cluster; the `SnapshotIO` fuzzer that was built but never executed; and the
three divergent copies of the atomic-write helper (`DiskImage.cpp`,
`Disk35Image.cpp`, `ProDOSVolume.cpp`), none of which `fsync` before the
rename, so a power cut can still land an empty file.

## 2026-08-01 — ImageWriter: the freeze, the reprint, the overprint, and four reference bugs

Follow-up to the //c fix below, from the same multi-agent audit of the
print chain. Everything downstream of the interface card had held up under
compiled probes and ASan/UBSan — no memory-safety defect anywhere — so what
was left were behavioural faults, each of which needed its own reasoning.

**The freeze.** `queueBytes()` printed the whole backlog synchronously
once it passed 1 MiB. On the UI thread, from `pumpImageWriter()`. Measured
with a probe: 852 ms for plain text and **301 s for a form-feed storm, in
a single frame**, while audio and the CPU worker carried on — which reads
as a hard freeze, not a slow printer. The bug is a category error: the
credit cap in `tick()` bounds credited *seconds*, and nothing ever bounded
the *work*. Catch-up now happens across ticks under two budgets, bytes and
sheet ejects, budgeted separately because an eject copies a whole page
raster and so is orders of magnitude dearer per byte than a glyph. Worst
frame is ~14 ms. Past a 4 MiB hard ceiling the oldest input is dropped and
counted, the rule the page stack and the SSC spool already follow:
truncating a printout is bad, freezing the emulator is worse.

**The reprint.** One drain cursor serves three possible sources, and it
was re-seated at 0 whenever the source changed. A spool can outlive its
source status — the SSC tap's does, nothing clears it — so one frame with
"Feed ImageWriter printer" unticked reprinted the entire session on the
next frame. Worse on a //c, where slot 1 is the printer port and slot 2
the modem port and both are SSCs: unticking slot 1 handed the source to
slot 2 at 0 and printed the whole modem transcript onto paper. It now
adopts the new source's current total, which also gives the physically
right answer for the toggle — while the box is unticked the cable is out,
and what the guest sent meanwhile went to a port with nothing on the end.
The arithmetic moved to `PrinterFeedCursor.h`, header-only, because it
lived in `MainWindow.cpp` where no test could reach it.

**The overprint.** `resetPrinter()` did not re-arm the CR/LF detector, and
nothing else a guest can send did either — so the latch was scoped to the
host session rather than the job. Once one CR+LF driver had latched it,
every later `PR#n : LIST` printed its whole listing onto one black line,
unrecoverable from inside the guest. `ESC c` ("initialize printer") now
re-arms it. That is safe as the re-arm point precisely because Print Shop
separates its colour passes with a bare CR and never sends `ESC c` — both
directions are pinned, because getting this wrong in the other direction
brings back the coloured staircase the detector exists to prevent.

**Four reference bugs.** Auditing the port against greg-kennedy's
`imagewriter.cpp` proves faithfulness, not correctness. Checked against
the *ImageWriter II Technical Reference* instead, four faithfully-ported
behaviours put visibly wrong ink on paper, so POM2 now deviates
deliberately (documented at each site, per the CLAUDE.md convention):
HT/VT went to the *farthest* tab stop rather than the nearest, so with
stops at 10/20/30 the first TAB jumped to 30 and every later one was a
no-op; `ESC 1`..`ESC 6` assigned an absolute head position instead of
adding intercharacter space, throwing the head backwards out of the left
margin mid-line and destroying justified output; `ESC c` binned the sheet
on the platen, which the reference can afford because it wrote pages to
disk but here is silent data loss; and `ESC H`/`ESC L` took any parameter,
so `ESC H 0000` ejected on every line feed and `ESC L 999` put the margin
83″ off the sheet — both silently.

Also: the stall watchdog's patience now scales with the head byte's cost
instead of a flat 10 s, so a legitimately long form feed is no longer cut
short and logged as a STALL; and the ImageWriter panel names any printer
card that is plugged but *not* feeding, since Slot Config allows a
`PrinterCard` and a `Grappler+` at once and the loser was silently dead.

One reported defect was verified and deliberately left alone: `ESC D`/`ESC
Z` cannot set bit 7 of either soft switch, because the bit-7 mask runs
before the escape parser. Neither bit 7 is wired to anything here — A-8 is
the "LF after CR" switch, which POM2 models with `AutoFeed` rather than
the switch byte, and B-8 is unused — so the change would be a no-op with
nonzero regression risk. Recorded in DEV.md rather than fixed.

## 2026-08-01 — The //c prints again: an armed printer tap is a device on the pins

`PR#1` on //c, //c+ and //c PAL hung the guest and printed nothing. Not
"printed badly" — the machine wedged inside its own printer firmware and
never came back, on three of the eight shipped profiles, with no
workaround available: a //c has no physical slots, so the built-in SSC
printer port is the *only* route to the ImageWriter.

The cause is a two-days-apart interaction between two correct changes.
`$C100` on a //c is **internal** ROM, so `PR#1` runs the machine's own
printer-port firmware rather than the card's synthetic `PR#n` ROM — and
that firmware gates every single character on the 6551 status register,
spinning until `status & (DCD|TDRE)` reads "carrier present, transmitter
empty". On 2026-07-30 the DCD/DSR polarity was corrected to match MAME
(`mos6551.cpp:37-39` inits `m_dsr(1), m_dcd(1)`) and AppleWin
(`SerialComms.cpp:864` returns `ST_DSR|ST_DCD` "when nothing is
attached"). That fix was right — POM2 had the sense inverted, so a
carrier-aware guest saw "online" with an idle listener. But it answered
the pins from the **telnet connection alone**, and two days earlier
(2026-07-28) the printer tap had shipped as a second kind of device on
the same port. With no telnet client, the //c was told its printer was
absent, and it waited for a carrier that a printer never has.

The fix is not a revert — the modem polarity stays as MAME has it. It is
that "nothing is attached" was simply false: an ImageWriter cabled to the
port *is* a DCE sitting there with its lines up. `deviceAttached()`
(= telnet peer **or** armed printer tap) is now what DCD/DSR answer to.
One condition, and the //c prints.

Worth recording is **why the existing test passed throughout**.
`ssc_acia_smoke` does exercise the printer tap — but it drives it through
the *card's* synthetic `PR#n` ROM, which only ever checks TDRE. It is
structurally blind to DCD, so it could not have failed here no matter how
the polarity moved. Nothing booted a //c profile and ran `PR#1` through to
the spool, which is exactly the gap a "//c printing" feature needed
covered. `iic_printer_port` now closes it at three levels: the DCD/DSR
device-present contract, the firmware's `status & $30 == $10` wait-loop
shape, and a real DOS 3.3 boot on all three //c ROMs where `PR#1` +
`PRINT "HI"` must land bytes in the spool. Against the pre-fix source all
three ROMs fail with the PC frozen in firmware ($C2BA / $C1C2 / $C2B7);
post-fix each spools the echoed command line and its output. The
end-to-end half is ROM/disk gated and skips rather than fails when the
user-provided media is absent.

## 2026-08-01 — Host sockets on Windows: the Uthernet II now has a network there

`POM2_HAS_SOCKETS` was 0 on Windows, which took out more than it looked
like: the Uthernet II's TCP and UDP paths, the Super Serial Card's telnet
bridge and the AI control server were all compiled out of every Windows
build. The card still plugged, reset and answered its registers — it just
never saw a packet.

The blocker was never capability. Windows has the same stack behind
Winsock2, and the difference is an API, not a feature. What made it worth
a header rather than a scatter of `#ifdef`s is that **every one of those
differences is silent** — code that compiles clean against Winsock can
still be wrong:

- `SOCKET` is **unsigned** and its failure value is `INVALID_SOCKET`, not
  -1. So `if (fd >= 0)` is always true, and `fd = -1` marks a socket as
  *valid* with a huge handle. Every "is this open?" test in the POSIX
  idiom inverts its meaning without a single warning.
- Errors bypass `errno`: `WSAGetLastError`, different codes, and
  `strerror` cannot render them.
- `close()` closes a CRT file descriptor, a different namespace from
  sockets; `closesocket()` is the one that works. `fcntl(O_NONBLOCK)`
  does not exist.
- The stack needs `WSAStartup` before the first call.

`src/SocketCompat.h` is now the one place that answers "POSIX or
Winsock?", and the three TUs are written against it.

**A fifth trap was not Winsock's fault and bit anyway.** `W5100Device`
already had a member `closeSocket(size_t)` — the chip-level CLOSE for one
of its four sockets. Inside that class, unqualified lookup finds the
member first and stops; `socket_t` then converts to `size_t` without a
murmur. `closeSocket(s.fd)` compiled clean and recursed until the stack
died — `uthernet2_w5100_smoke` caught it as a segfault with 74 000
identical frames. The helper is now named `closeHostSocket`, which no
card would plausibly use for a chip-level operation.

**Readiness waits use `select()` on Windows, not `WSAPoll()`.** The
caller that decides this is `W5100Device::poll()`: it waits for WRITE on
a socket with a non-blocking connect in flight, and it has to learn about
a *refused* connection, not just a successful one. On Winsock the
documented channel for that is `select()`'s `exceptfds` — which is why
Winsock's select takes one. A wait that could only report success would
leave a guest polling `SN_SR` forever on a connection that was refused.

Two smaller Windows facts, both of the silently-wrong kind: `SO_RCVTIMEO`
takes a `DWORD` of milliseconds there, **not** a `timeval` (pass a
timeval and it is accepted, then read as garbage — a 2-second timeout
quietly becomes minutes), and `inet_ntoa`'s static buffer lets two
threads logging a connection splice each other's addresses, so both
workers now use `inet_ntop` via `peerAddressText`.

Verified by cross-compilation, not by reasoning: `x86_64-w64-mingw32-g++
-fsyntax-only` over **every** `src/*.cpp`. 83 compile for Windows
outright; the 9 GL/UI ones needed only GLFW's header staged, including
`MainWindow.cpp`, which is what proves the winsock1-vs-winsock2 include
ordering is safe in the file most likely to break it. The one remaining
ordering hazard — a TU that pulled `windows.h` in first — is now a single
`#error` instead of fifty redefinition errors. Linux: 156/156 ctest.

**What this does NOT cover: the Uthernet I on Windows.** It is a plain
NIC, so it needs raw frames, which means libslirp. vcpkg does carry a
libslirp port (4.9.1), so the library is obtainable — what is missing is
POM2's side: `SlirpNetworkBackend`'s poll loop is written against POSIX
`poll()` over the fds libslirp hands back, and that port cannot be
verified without a Windows libslirp build to test against. CMake
therefore does not look for libslirp on WIN32 at all, and says so; a
documented absence beats trading it for a wall of missing-header errors.
The vcpkg port also pulls glib, which is a real addition to the Windows
CI budget. Tracked in TODO.

## 2026-08-01 — The audio bus goes stereo

The Mockingboard is a stereo card and POM2 was summing it to one channel.
MAME wires it to a single 2-channel speaker with AY1 on channel 0 and AY2
on channel 1 (`a2mockingboard.cpp:159-165`); the Phasor gets a second one,
so left = ay1+ay2 (the VIA1 pair) and right = ay3+ay4 (the VIA2 pair)
(`:192-208`). Digidream 1 writes a deliberate A/B/C pan, and the mono sum
destroyed it — which is also why single-AY software (Digidream 2 never
touches chip 2) sat 6 dB down: it was being normalised for two chips'
worth of headroom while only ever filling one.

**Nothing moved level.** That was the constraint the design had to
satisfy, because a silent 3 dB shift across every source is the kind of
regression nobody reports and everybody hears. Three things follow from
it:

- The **mono contract is unchanged**. `fillAudioBuffer` still hands a
  source one channel; the mixer places it with `AudioSource::pan`, whose
  law is a **balance**, not constant power — centre is unity on *both*
  channels. Constant power is the textbook choice and it would have put
  the speaker, the cassette and both floppy-sound devices 3 dB down on
  day one, in exchange for faithfulness to a stereo position the Apple's
  own speaker does not have.
- Cards that really are stereo override `fillAudioBufferStereo` and own
  their placement (`pan` is then ignored — the card's wiring is the
  authority, not a mixer knob). Both keep a **mono fold-down** of
  `0.5 * (L + R)`, which is bit-for-bit the pre-stereo render: `/3` per
  side folds back to the Mockingboard's old `/6`, `/6` per side to the
  Phasor's `/12`. The test asserts that identity sample-for-sample and
  measures 0.0 worst-case error.
- The **mono-downmix switch** averages rather than sums, for the same
  reason: summing would have made every centred source 6 dB louder the
  moment a user ticked the box.

Speech stays centred: MAME routes the Mockingboard's speech chip to both
channels at unity (`:186-189`) and gives the Echo+ TMS5220 a
`front_center` speaker (`:210-219`). Where a *pair* of speech chips would
sit has no oracle, so it is a documented gap rather than a guess.

The switch is in the mixer panel next to per-channel master meters, and
the mono sources gained a pan knob (right-click to centre). Settings:
`audio_mono_downmix`, `speaker_pan`, `cassette_pan`,
`floppy_sound_pan[_35]`.

Pinned by `tests/audio_stereo_test.cpp`: the pan law including the centre
= unity guarantee, stereo passthrough, the downmix, per-chip placement on
both cards (the silent side must be *exactly* silent — any leak means
something is still summing), the fold-down identity, and a hard-panned
card mixed next to a centred source.

Also in this pass: `setup_imgui.sh` no longer aborts the whole setup when
`apt update` fails. It fails as a whole if *any* configured repository is
unreachable — one stale third-party PPA is enough — and under `set -e`
that killed the run before Dear ImGui was even cloned, despite every
package we actually need having refreshed fine.

## 2026-08-01 — Mockingboard audio: the write queue collapsed, and the synth never band-limited

Digidream 2's Mockingboard music sounded coarse. Four things were wrong; the
first is the one that mattered, and it was not in the synthesiser at all.

**1. ~90 % of AY register writes were dumped at the buffer edge.** The CPU
worker publishes one video frame of writes in a single burst (~17045 cycles),
while one audio callback only covers `periodSizeInFrames` = 256 samples =
~5937 cycles. So every burst carried ~3 callbacks' worth of *future* writes.
`fillAudioBuffer` drained the whole queue each callback, replayed what fitted
in this buffer's cycle span, applied **all the rest in bulk at the buffer
edge** — where, being applied in order to the same register bank, only the
last value written to each register survived — and then set
`audioCursor = pending.back().cycle`, parking the cursor on the newest event
at zero lag. Two callbacks later it had over-run the next burst and tripped
the backward re-anchor, roughly every third buffer.

DD2 is exactly the workload that destroys. A headless trace of the real disk
(150 s, 169 930 writes) shows **54 % of its entire register traffic is R8** —
an Atari-ST style "SID voice" that toggles channel A's volume register in a
50 % duty square between 129 and 1006 Hz, driven by VIA1 T1+T2 interrupts.
That modulation cannot survive being collapsed to one value per burst.

The queue is now a jitter buffer: un-rendered events stay queued instead of
being dumped, and the cursor deliberately runs about one producer burst
*behind* the newest event, with a +/- one-burst deadband so steady-state
playback never snaps at all. Costs one PAL frame of added latency on this
card; buys correct sub-buffer placement.

**2. The synthesiser point-sampled a signal it had already resolved.** It
advanced tone/noise/envelope in integer clock/8 ticks inside a per-output-
sample loop — then read the mixer *once*, throwing away the sub-sample edge
position it had just computed. Every square-wave edge snapped to the 44.1 kHz
grid (+/-22.7 us of jitter) and everything above Nyquist folded back in.
Measured: **7 % of total output power was inharmonic** on an ordinary 4 kHz
note; at envelope periods below 2, whole envelope steps were never sampled.

MAME does not have this problem because it never renders at the output rate:
its stream runs on the chip's own clock/8 grid (`ay8910.cpp:1298`) and a real
decimating resampler takes it to the device rate (`src/emu/resampler.cpp`).
POM2 renders straight to the device rate, so the decimation now happens
inline — the mixer is **box-integrated** across the ~2.9 base ticks each
output sample spans, weighting the partial ticks at both ends by their true
duration. Inharmonic energy drops to **0.51 %** (-22.9 dB), and edge position
becomes continuous again, which is what the volume-register PWM needs.
Cost: 0.67 % of one core per chip at realtime.

**3. No DC blocking, on a unipolar model.** A channel contributes
`kVolumeTable[level]` or nothing, so a 50 %-duty tone carries a DC term of
half its amplitude and a channel with tone *and* noise masked off in R7 (the
digi/PWM configuration) is pure DC. Every note and volume write stepped that
offset — audible clicks, and half the headroom spent on silence. Now a 1-pole
20 Hz high-pass, matching MAME's default per-speaker filter
(`src/emu/audio_effects/filter.cpp:39-44,63-68`).

**4. Level and clock.** `sample / 6.0f` normalised for *both* AYs at once,
so the very common single-AY tune (DD2 never touches the second chip) sat
6 dB down and users made it up on the volume slider — amplifying the aliasing
along with the music. Now `/3` with a `tanh` soft knee for the genuine
two-chip peak. Separately, the AY tick rate was pinned to the NTSC constant;
pin 22 is wired to the slot's phase-0 line, so a PAL machine really does
clock the chip at 1 015 625 Hz. It now derives from the live CPU clock —
PAL music was **12 cents sharp**, and the French Touch / DIX corpus this path
exists for is PAL.

**Extraction.** `MockingboardCard` and `PhasorCard` carried verbatim copies
of the synthesis (~130 lines, 4 differing lines) and had already drifted:
Phasor never gained the cycle-stamped event queue. Both now share
`src/AyPsgSynth.h` (generators, mixer, box integration, DC blocker), so an
audio fix cannot land on one card and silently skip the other. Phasor is
-149/+51 lines. It still lacks the event queue and a `setCpuClock` override —
both real, both now the only remaining divergence rather than a hidden one.

**Not changed, and worth recording as verified rather than assumed.** The
envelope state machine was suspected of an off-by-one on the alternate
shapes (`$0A`/`$0E`), where `envStep--` reaches -1 and the code tests
`envStep & 0x10`. It is correct: `-1 & 0x10 == 0x10` in C++, the same integer
promotion MAME's `s8 step` gets. All 16 shapes are now decoded back out of
the rendered audio and compared against MAME's step sequence in
`mockingboard_audio_quality`. The noise LFSR taps, prescale and period-0
handling were likewise checked against `ay8910.h:263-273` and are right.
The `kAyVolumeTable` **provenance comment** was wrong, though — it claimed
MAME's `build_single_table(normalize=1)`, which actually maps to
[-0.125, +0.375] and only applies under `AY8910_LEGACY_OUTPUT`. The data is
Westcott's measured curve renormalised; citation corrected in place.

New `mockingboard_audio_quality` test: spectral purity (FFT, inharmonic
energy), residual DC, volume-register PWM placement under a reproduced
bursty producer, and the 16 envelope shape sequences. Nothing in the suite
asserted on rendered audio before this — `mockingboard_smoke` only checked
"not silent" and pitch to +/-6 %, so every defect above passed it.

### Same day, follow-up: one regression reverted, one hypothesis retracted

Listening on the real disks corrected two things in the above.

**The `/3` + `tanh` soft knee was a regression and is reverted.** Reported
symptom: Digidream 2 much improved, Digidream 1 worse — glitchy tempo and
timbres that "do not correspond". The split is diagnostic. DD2 drives ONE
AY, so its sum never left the near-linear region of `tanh`; DD1 drives
BOTH, and its 6-channel sum routinely exceeds unity, so the waveshaper was
compressing and intermodulating it continuously. Mixing is linear `/6`
again, on Mockingboard and Phasor. Loudness is a knob the user already
has; a waveshaper across the whole mix is not something to spend it on.
The honest fix for single-AY level is true stereo, where each side carries
one chip — see `TODO.md` [Audio].

**The Digidream 1 cause, measured on the real disk.** An A/B harness
(`tests/dd1_audio_ab`) boots `DD.dsk` on //e PAL, drives one 50 Hz frame
of CPU then pulls 882 samples in 256-frame buffers, and renders through
the old renderer, the new one, and a cycle-exact zero-queue oracle. All
three produce byte-identical AY write logs, so the comparison isolates
the renderer.

The fault is the `caughtUp` guard measuring lag against
`latestAyEventCycle_` — the last WRITE — instead of against CPU-now. A
music driver writes the AY in one dense clump per frame and then leaves
it alone: DD1 is write-silent for **88 % of every frame**, worst
inter-write gap **17.6 ms**. That silence is charged against the guard's
budget as if the consumer had caught up. Measured margin before a false
trip: **1076 cycles = 1.06 ms** on DD1, against **9.6 ms** on DD2, whose
writes are 40x denser — which is exactly why one demo glitched and the
other did not.

Each false trip is a real dropout: the cursor jumps back ~30 ms and the
register bank then freezes for **7-8 consecutive callbacks = 40-46 ms**,
two frames of music. Frequency per 25 s under ordinary conditions: host
0.5 % slow → 4, host 1 % slow → 8, one dropped frame per 200 ms → 71.
Fixed by pacing against `lastSyncCycle_` (the VIA's synced "now", which
keeps advancing through write silence).

Note the earlier `!pending.empty()` guard, added from live instrumentation
that showed an endless `ANCHOR caughtUp lag=-3270`, does **not** fix this
— measured identical trip counts with and without. That instrumentation
had caught a genuine but different defect (an idle producer plus a
re-anchor target that collapsed to cycle 0); `pending` is empty in only
179 of 4306 callbacks and all 179 precede the first AY write. Both fixes
are kept; only the second one addresses DD1.

**A second defect, introduced by the lag itself.** `ayEnvWriteCount_` is
a CPU-now counter, but the cursor deliberately runs ~40 ms behind it, so
honouring it restarted the envelope a second time, ~40 ms early —
**202 spurious retriggers in 25 s** of DD1 (its 103 R13 stores x 2 chips),
moving 13.8 % of the render's RMS. The replayed event already covers
same-value R13 stores, since the producer queues an event for every write
regardless of value, so the counter path is simply dropped.

**Where the timbre change actually came from.** Attribution over a 25 s
render: **89.5 % of the OLD output's total power sat below 50 Hz.** DD1's
digidrum is unipolar PCM, so the old render carried an enormous sub-audio
pedestal that every hit stepped. The DC blocker removes it — peak 0.64 →
0.49, audible level actually **+0.79 dB**, so the "quieter" impression is
the missing bottom end, not lost loudness. Box integration is the small
term (waveform correlation 0.95-0.99 against point sampling; centroid
1327 → 1143 Hz, >6 kHz share halved). The replay timing dominates
everything: OLD vs NEW correlates only 0.10-0.22, and swapping the point
sampler back in changes that by <0.002.

Ruled out with measurements, so nobody re-opens them: envelope hold (DD1
only ever writes shape `$08`, so `envHolding` is never set); the PAL tick
rate (12.05 cents, uniform — a pitch shift, not a tempo change); queue
overflow (`pending` max 731 vs `kMaxAyEvents` 16384); disk turbo (7 turbo
frames, all before music, 1 `starved` at t=1.04 s and none after); buffer
size (256/480/512/1024/2048 all clean in the ideal cadence).

**Methodology, because this cost real time.** Two successive synthetic
harnesses PASSED against the very bugs they were written to catch. The
first produced NTSC-sized bursts while the target lag is sized in PAL
frames, so the lag never swept its critical range. The second used an
unbroken write stream, so it never modelled a production gap — which is
exactly the condition the live instrumentation caught. A Mockingboard
audio regression test is only credible once it has been demonstrated to
FAIL against the reverted fix; test 3c in
`tests/mockingboard_audio_quality` carries an explicit note that it does
not discriminate the defect it accompanies.

## 2026-07-31 — 6522 T1 continuous period is latch+2, not latch+3 (a frame clock that drifted)

`Via6522::advance` reloaded T1 in continuous mode with `latch + 3`, copied
from MAME's `t1_tick` (`6522via.cpp:536-543`) `TIMER1_VALUE + IFR_DELAY`.
**IFR_DELAY is the one-off underflow→IFR latency, not part of the recurring
period.** Folding it into the reload stretched every interval by one cycle.

One cycle per frame is inaudible in the Mockingboard's usual job (a music
tick), which is why this survived so long. It is fatal when T1 is armed as a
*frame clock*, because the error accumulates: French Touch's **MAD EFFECT**
arms T1 with one PAL frame and beam-races a 192-line picture off each
interrupt, so its drawing loop slid a cycle per frame until whole scanlines
fell past line 191, got stamped scanline 192 by `pushVideoEventLocked`, and
were dropped by the renderer. Measured on the real disk: the loop's per-frame
phase went from drifting to stable, and recovered page-flip events per frame
rose from 169 to 188 of 192.

The demo states the contract while computing its own latch (`Sources/main.a`,
GPLv3, archived in `disks_5.4/demo/madef/`):

```
; PAL delay = 65*(192+70+50) = 20280
; -2 (6522 takes 2 cycles to generate INT)
; = 20278 = $4F36
```

period == latch + 2. The first shot keeps N+3 (the IFR latency genuinely
applies once); only the reload changed.

`via_t2_timing` had pinned the *wrong* value — it asserted N+3 spacing for
the continuous reload, citing MAME. That assertion was MAME parity, not
hardware parity, and is now corrected in place with the reasoning; the new
`via_t1_continuous_period` covers eight consecutive reloads, since it is the
reload and not the first shot that accumulates. Consistent with the rest of
today's findings: MAME keeps N+3 and renders the demo wrong, AppleWin only
started running it at 1.29.6.0 after fixing this class of timing bug.

Diagnostic harness: `tests/madef_phase_probe.cpp` (built, not in ctest — it
needs the demo disk) boots the real image and reports per-frame drift,
per-line period, and where each page-flip lands horizontally.

**Still open**: the picture is much closer but not right — some scanlines
still open at column 0 because their switch lands where
`frameCycleToPos` clamps (`byteCol = clamp(hpos - 25, 0, 40)`). A sweep of
all 65 candidate phases finds none that keeps every switch inside the
40-column window (best is 28, still 55 of 380 outside), so this is **not** a
constant offset to tune — the line attribution itself is still wrong for a
subset of events.

Deriving the line origin from `main.a` was attempted and **did not settle
it**, which is worth recording so it is not retried blind. The source pins a
*relation* — 13 cycles between the `$C019` edge and the demo's "cycle 0" —
but not where that cycle 0 sits in POM2's beam coordinates; one equation,
two unknowns. The alternative reading (cycle 0 = first visible byte ⇒ edge
at hpos 12 of line 0, 25 cycles from the hpos-52 placement) was implemented
and measured: MAD EFFECT's page-flips moved from column 0 to column **1**,
not the predicted column 13, and `pal_timing` + `vbl_smoke` both failed
because they require line 192 to read VBL from its first cycle. Reverted.
The hpos-52 placement is what the demo states literally *and* what those two
pinned tests corroborate. Settling the residual needs an independent anchor
— a column-accurate reference capture, or a hardware statement of VBLBAR's
position relative to the start of active video — not more reasoning.

## 2026-07-31 — switch→column mapping is `hpos - 24`: a switch is one cycle too late for its own byte

`Apple2Display::frameCycleToPos` mapped a soft switch to a screen column with
`byteCol = clamp(hpos - 25, 0, 40)` — the raw offset of the visible window,
which opens at hpos 25 after the 25-cycle HBL. But a switch performed *at*
hpos 25+c cannot affect column c: the video scanner latches that byte during
phi1 of the very cycle whose phi2 the CPU is using for its access, so the
change first shows one column later. The effective mapping is `hpos - 24`.

Established by measurement, not by the argument above. Replaying French
Touch's **MAD EFFECT** (GPLv3 sources archived in `disks_5.4/demo/madef/`)
and sweeping all 65 candidate phases, the demo's 192 per-scanline lit-run
starts — the `$C055` whose column *is* the silhouette it draws — land wholly
inside the 40-column window only for offsets **21..24**. 25 sat one cycle
outside, which is exactly why the scanlines whose start falls at the far left
of the silhouette spilled into HBL and clamped to column 0 while every other
line drew correctly.

Method note that cost two wrong turns: sweeping *all* switches has no
solution at any phase. The `$C054` that CLOSES the lit run is legitimately
thrown in HBL — "a switch in blanking governs the whole upcoming line" is the
standard idiom. Only the opening switch must be inside the window.

**This moves the beam-racing convention**, so five pinned tests were
re-baselined: `horizontal_split`, `horizontal_split_composite`,
`horizontal_split_560`, `dix_modpage_split` and one line of `pal_timing`.
None of them measured anything — every one drives a synthetic switch at
hpos 45 and asserted the resulting column, i.e. they restated POM2's own
choice. The *expected column* was moved (20 → 21) rather than the stimulus
(45 → 44), so the change stays visible in the tests instead of hiding in a
one-character edit. `horizontal_split_smoke` now spells the rule out:
hpos 24 → col 0, hpos 25 → col 1, hpos 45 → col 21, and hpos 64 → the
end-of-window clamp (the last cycle of a line can no longer affect the last
byte, which is already latched).

Residual: the measured band was 21..24 and 24 is its edge — the value with a
mechanism behind it, but 21-23 are not excluded by the data. If real software
ever contradicts `hpos - 24`, this is the commit to reopen.

## 2026-07-31 — `$C019` intra-line phase: proposed, implemented, measured, rejected

Recorded because the reasoning is seductive and someone will try it again.

French Touch's **MAD EFFECT** syncs its whole frame off the `$C019`
VBL'→DISPLAY edge, and its cycle-annotated `Sources/main.a` says:

```
; WARNING: DISPLAY detected (VERTBLANK <0) from cycle #52 of last line (#311) of VBL
...                                        ; line 311 / cycle 54
NOP : NOP : NOP : NOP  : LDA $EA           ; +11
                                           ; = 65
; line 0 (display) / cycle 0
```

Read naively this says the edge is 13 cycles before the line boundary, and
POM2 derived the flag from the scanline number alone (`scanline = now / 65`),
i.e. with **no** horizontal phase — apparently a bug. It is not: the sentence
pins a *relation* (13 cycles from the edge to the demo's "cycle 0"), not a
*position*, because nothing says where that cycle 0 sits in POM2's beam
coordinates. One equation, two unknowns.

Both anchorings were implemented and falsified against the real disk, using
the count of MAD EFFECT's 192 per-scanline lit-run starts (`$C055`) that fall
outside the 40-column visible window:

| `$C019` lead | clean-phase band | distance from `frameCycleToPos`'s 25 |
| --- | --- | --- |
| +13 (edge at hpos 52 of the previous line) | 9–12 | 13–16 cycles |
| +28 | ~58 | worse |
| **0 (no shift — what POM2 already did)** | **21–24** | **1–4 cycles** |

So the un-phased implementation was already within 1–4 cycles and every
proposed shift moved *away* from the answer. A third variant (edge at hpos 12
of line 0, from reading the demo's "cycle 0" as the first visible byte) also
broke `pal_timing` and `vbl_smoke`, which independently require line 192 to
read VBL from its very first cycle. All reverted; `vbl_edge_phase` now pins
the un-phased edge together with this history, and the //c VBLINT latch path
carries a matching note.

Method note, since it cost two wrong turns: the first phase sweep demanded
that *every* switch land inside the visible window and therefore had no
solution at all. The `$C054` that CLOSES the lit run is legitimately thrown
in HBL — the standard "a switch in blanking governs the whole upcoming line"
idiom. Only the `$C055` that OPENS it has to be inside the window, because
its column *is* the silhouette. Sweeping those alone is what produced the
table above.

**Resolved the same day** — see the `hpos - 24` entry above: the residual was
the cycle→column mapping, not this edge.

## 2026-07-31 — 8 KB international //e video ROM (342-0274-A)

`Memory::loadCharRom` used to reject anything that was not 2 KB or 4 KB, on the
stated grounds that "no shipped char ROM is 8K". The genuine **342-0274-A** is
8 KB, and it is the part fitted to the French //e — MAME's //e character
generator region (`gfx1`) is 8 KB = **two 4 KB banks**, and the machine's
charset switch picks one. The US `apple2ee` fills both banks with the same 4 KB
part (`342-0265-a.chr` at offset 0 *and* 0x1000); `apple2eefr` instead ships one
8 KB part carrying two different sets.

POM2 now collapses an 8 KB dump to a selected bank and runs its ordinary 4 KB
normalization on it — one normalization routine, not two. The bank layout was
established by CRC rather than assumed: **bank 0 == `apple2e_char_frca.rom`
(2c8fc403), bank 1 == `apple2e_char.rom` (2651014d)**, both of which POM2
already ships standalone, so the two halves are independently checkable.

The picker gains two entries (`iie_fr8k_fr` / `iie_fr8k_us`) rather than
modelling the hardware charset switch, which is not emulated. `charRomBank()`
carries the bank to every `loadCharRom` call site — without that plumbing both
entries would silently load bank 0 and draw identical glyphs.

**Catalogue correction found on the way**: the existing "//e/c — Français
(342-0274-A)" entry points at a 4 KB file that is byte-identical to
`apple2e_char_frca_unenh.rom` (both ab0be706), so it was never 342-0274-A. The
label is now honest and the real part is offered alongside it.

Pinned by `char_rom_8k_bank` (5 checks: both banks match their standalone 4 KB
dumps, the banks differ so the argument is load-bearing, out-of-range clamps,
and a 4 KB dump ignores the bank). It soft-skips without the user-provided ROMs.
`char_rom_test` pinned the *old* "8 K is rejected" contract and was updated
deliberately, not deleted.

Also wired: **`3420033a.256`** (MAME `apple2c0`, the "//c UniDisk 3.5" ROM
revision) appended **last** in the //c probe order — a fallback for users who
own only that dump, not an upgrade. It does not unlock hardware-accurate 3.5
boot on //c: POM2 still serves 3.5"/HDV there through the host-side SmartPort at
built-in slot 5, because the IWM bit-shift path is deliberately unmodelled.
**`342-0326-a.f12`** (French keyboard decode ROM) is catalogued as oracle-only —
POM2 maps host keys directly and has no keyboard-decode ROM. `a2c.128` is
byte-identical to the existing `apple2c-16K.rom` and needed nothing.

## 2026-07-31 — Bug hunt 6: stale screen on the Le Chat Mauve Eve registers

**A regression in the same day's frame skip, found by hunting it rather than
trusting it.** `TextFrameKey` keyed on `Memory::DisplayState`, and that is not
the whole picture: a **Le Chat Mauve "Eve"** has its own $C0B8-$C0BB registers
which select the colour-TEXT renderer (and with it the 560-wide `frame80`).
They are guest writes — `STA $C0B9` — but they reach the card through
`SlotBus::broadcastVideoSwitch` and, unlike $C05E/$C05F, push **no video
event**. So the frame after such a write has an empty event log *and* an
unchanged `DisplayState`: every term of the key agreed, the skip fired, and the
screen kept a stale picture at the wrong geometry (280-wide mono served where
560-wide colour text was due). The card is the **//c PAL profile's built-in
slot 7** — the French Touch / DIX target hardware — so this was not a corner
case. The key now carries the card's identity plus its mode + both Eve toggles.

Two process notes worth keeping:

- The original mutation sweep could not have caught this. It toggled the
  **host-side** `hiResMode` but never the card's **own guest-facing** switches,
  so it proved the key handled everything it already knew about — the classic
  shape of a test that confirms its author's model instead of the behaviour.
- The new section 9 **passed on the first attempt, vacuously**: the card was
  handed to the display via `setChatMauveCard()` but never PLUGGED into the
  `SlotBus`, so `broadcastVideoSwitch` reached nobody and the guest writes went
  nowhere. Plugging it made the failure appear immediately. That is now recorded
  in the test header, because it is the second time in this file that a
  side-by-side harness passed while testing nothing (the first was the shared
  `Memory` draining `takeVideoEvents()`).

All six key terms are now mutation-proven load-bearing: flash phase, video RAM,
DisplayState, colour mode, Chat Mauve state, beam-raced-frame exclusion.

## 2026-07-31 — Static-text frame skip: −84 % on the display

The remaining big win from the 2026-07-30 profile: the display re-decoded all
960 character cells every frame even when the screen had not changed a byte —
`glyphRows7` alone was 56 % of display cost, ~887 host instructions per cell.
`Apple2Display::render` now compares the frame against a **`TextFrameKey`** and
returns without painting when nothing that could affect the pixels has moved.

Measured on booted DOS 3.3 (3000 frames, render phase only, II+):
**93.4 → 15.0 µs/frame, −84 %**, with a byte-identical pixel checksum. The
worst case — text churning every frame, so the key can never match and its
copy+memcmp is pure overhead — is 94.8 → 94.7 µs, i.e. free: 16 KB of memcmp is
nothing against ~850 K instructions of glyph decoding.

**The skip is deliberately narrow, and every exclusion has a reason:**

- **Beam racing.** A frame carrying video events is painted as several bands
  with *different* DisplayStates (and, on the 560-wide path, a column-bounded
  save/restore), so it corresponds to no single whole-frame state. `render()`
  only consults the key on the `events.empty()` branch; the beam-raced branch
  invalidates it. This is what the DIX / French Touch demos depend on.
- **Persistence.** The graphics painters implement a phosphor rule
  (`max(target, prev × decay)`), so their output legitimately changes every
  frame from identical inputs. Only FULL-SCREEN TEXT is skipped —
  `renderText`/`renderText80` write no persistence at all, which makes their
  output a pure function of the key.
- **CPU demod.** AppleWin / OE-CPU overwrite `frame80` from the composite
  signal, so the key would describe pixels that are no longer on screen; that
  branch invalidates too.

**PAL was checked, not assumed.** FLASH is
`frameCounter / kFlashHalfPeriodFrames & 1`, and `frameCounter` is the
*emulated* frame index — `cycleCounter / (65 × scanlinesPerFrame)` — so PAL's
312-line/50 Hz frame and NTSC's 262-line/60 Hz frame each advance it at their
own rate and the key follows automatically. A skip keyed on a host frame
counter would have drifted on PAL only.

The key stores video RAM **by value** — `$0400-$0BFF` from main *and* aux, the
union of text/lo-res pages 1 and 2 — rather than resolving which page is live.
Over-covering costs a bigger memcmp; under-covering would freeze a stale screen.
The character ROM goes in by value too, not by pointer: reloading a different
character set can reuse the same heap block, and a pointer+size compare would
then report "unchanged" across an actual glyph change.

New `display_dirty_skip` test (151/151 green). It runs **two machines in
lockstep** — one display allowed to skip, one forced to repaint via the new
`invalidateTextFrameCache()` — and requires bit-identical framebuffers over a
113-frame script, under **both** video standards. Two separate `Memory`
instances, not one shared: `takeVideoEvents()` drains, so a second display on
the same Memory would see an empty log and never take the beam-racing path —
the test would have passed while testing nothing. (It did, until that was
found; the published events confirm the split now lands at NTSC line 87/174 and
PAL line 104/192 — different positions, same script.)

Mutation-tested: deleting the flash-phase, video-RAM, DisplayState,
colour-mode or beam-race-exclusion terms each makes the test fail. Two terms
survive deletion and are therefore **defensive, not load-bearing**: the
`mixedMode` exclusion (`renderInternalBand`'s `if (state.textMode)`
short-circuits before any mixed handling, in both the 40- and 80-column paths,
so MIXED cannot alter a full-text frame) and the `iie` flag (only changes
across a profile switch, which rebuilds the display). Catching the colour-mode
term required installing a **Le Chat Mauve** card: its colour-TEXT path is the
only text renderer whose pixels depend on the host-side colour mode — every
other mode draws text hard-coded white-on-black.

## 2026-07-30 — Profiling: −17 % on the emulation core

A Callgrind pass over a deterministic `tickFrame()` driver (600 frames of
steady-state DOS, **3 969 356 emulated instructions**) put ~70 % of POM2's work
in the emulation core and ~30 % in the display, and showed the core spending
about as much on the per-instruction device fan-out as on the 6502 itself. Two
fixes came out of it, both measured:

**The VBL check did a runtime-divisor modulo on every emulated instruction.**
`Memory::advanceCycles` derived the scanline with
`(cycleCounter / 65) % scanlinesPerFrame`. The `/ 65` is free — a compile-time
constant the compiler strength-reduces to a multiply-shift — but the `%` has a
*runtime* divisor and stays a real hardware division, executed ~4 M times per
10 emulated seconds. The frame origin is now tracked incrementally, and the
division only runs to resynchronise.

Worth recording because it is a trap: Callgrind rated that line at 1.7 % of the
core, yet removing it was worth **15 %** of wall-clock. Callgrind counts
*instructions*, and a `div` is one instruction of 20-40 cycles — so on
division-heavy code its ranking badly understates the real cost. The full
before/after confirms it: instructions fell 4.6 % while time fell 17.5 %.

The incremental base has to survive `setCycleCounter()`, which snapshot restore
and rewind use to move the counter **arbitrarily, backwards included**. The
subtraction is deliberately unsigned so a backwards jump wraps to a huge value,
misses the one-frame-rollover test and lands in the resync branch — self-healing
in a single division. (A first draft reset the base to 0 and walked forward with
a `while` loop instead; that would have spun for millions of iterations on any
rewind with a large cycle counter. It passed the whole suite regardless, which
is a fair warning about what the suite does not cover.)

**The cassette burned 4.1 % of the core with no tape loaded.** `advancePlayback`
returns immediately when idle, so that was pure call overhead — ~17 instructions
per emulated instruction to decide there was nothing to do, ~4 M times.
`advanceCycles` is now inline in the header and gates the out-of-line work on
the deck actually moving.

`currentCycle` still advances unconditionally, and that is not incidental: it is
the RECORDING timebase (`toggleOutput` measures pulse widths against it), so
gating the whole call — which the first draft did — would silently corrupt the
durations of a tape recorded from a deck that had nothing loaded, i.e. the normal
way to record one. The suite passed that draft too.

Net: **−17.5 % on the core**, −13.8 % including display, 150/150 green.

**Third fix: `SlotBus` now keeps a compact list of the plugged cards.** The
per-instruction fan-out walked all eight `unique_ptr` slots to find the one or
two that exist — 65 host instructions per emulated instruction with a single
Disk II, 15.7 % of the core, plus another 1.9 % attributed separately to
`unique_ptr.h`. `SlotBus::advanceCycles` fell **258 M → 127 M instructions
(−51 %)**.

The cache holds RAW, NON-OWNING pointers, so it dangles the moment a card is
destroyed. That is safe here for three checkable reasons, and they are the whole
argument: `slots` is private with no accessor that can reseat a slot from
outside; there are exactly three mutation points (`plug`, `unplug`, `clear`) and
each rebuilds; and mutations run under `stateMutex` — the same lock the CPU
worker holds around `runCpuSlice` — with `applyProfile` additionally stopping
the worker first. `unplug()` rebuilds *before* returning, since the `return
std::move(slots[slot])` is what empties the slot. A debug-only assertion in
`advanceCycles` cross-checks the cache against `slots` every call, so a future
fourth mutation point fails loudly instead of silently skipping a card or
following a freed pointer; the whole suite was run in a Debug build with it live.

Cumulative across the three fixes: **−25.6 % on the emulation core**, −19.8 %
including display (0.531 s → 0.395 s for 2000 frames), 1 645 M → 1 407 M
instructions. POM2 goes from ~47x to ~59x realtime.

Still on the table: the display re-decodes the whole text screen every frame
even when nothing changed — `glyphRows7` alone is 56 % of display cost, ~887
instructions per character cell. Dirty-region tracking is the remaining big win,
but it touches the beam-racing path the DIX demos depend on, so it is not free.
*(Done 2026-07-31 — see the entry above.)*

## 2026-07-30 — v0.8: GLES tier, four-platform release CI, portability fixes

**`POM2_GLES` — the OpenGL ES 3.0 tier is now a build option, not a browser
accident.** POM2 already contained a complete GLES path (`GLFW_OPENGL_ES_API`,
`#version 300 es`, direct entry points) — it was simply gated on
`__EMSCRIPTEN__` across seven translation units. That conflated two different
questions, *"do we speak GLES?"* and *"are we in a browser?"*, and the
conflation is precisely what made the Raspberry Pi unreachable: a Pi needs the
GLES tier while being an ordinary native Linux build, so every guard took the
desktop branch and the result asked for a GL 3.2 core context, which Mesa's V3D
cannot give (it caps *desktop* GL at 3.1). `src/Pom2Build.h` now owns the
distinction via `POM2_GL_ES`, set by Emscripten **or** `-DPOM2_GLES=ON`.

One detail worth keeping: native GLES must go through **EGL**
(`GLFW_CONTEXT_CREATION_API = GLFW_EGL_CONTEXT_API`). GLX only hands out a GLES
context when the X server advertises `GLX_EXT_create_context_es2_profile`, which
V3D does not — without the hint the context request fails on exactly the
hardware the tier exists for. libEGL itself is *dlopened by GLFW*, not linked by
us (`readelf -d` shows libGLESv2 only); CMake locates it purely as a
presence check so a missing package fails at configure time with a package name
rather than at runtime with "context creation failed".

**Release CI on four native runners**, modelled on POM1: Linux x86_64 (pinned
bionic container, glibc floor 2.27), Raspberry Pi arm64 (bookworm, GLES, floor
2.36), macOS Universal 2 `.dmg`, Windows self-contained `.zip`, plus a publish
job attaching everything with `SHA256SUMS.txt`. POM2 **reuses POM1's**
`pom1-bionic-builder` image rather than duplicating a near-identical one — the
requirements are the same, and one image beats two to keep in sync.

**Five real portability bugs, all found by the first CI runs** — none of them
reachable on the dev machine:
- `MemoryProfile.h` used `size_t` with no `<cstddef>`. libstdc++ 14 leaks it via
  other headers; Debian bookworm's 12 does not, so the arm64 build failed.
- `AudioDevice.cpp` used `std::fabs` with no `<cmath>` — same class, MSVC.
- The macOS deployment target was set to 10.13, but POM2 uses `std::filesystem`
  throughout and libc++ marks those symbols unavailable before **10.15**.
- Seven TUs each hand-rolled the GL include block and had drifted apart. On
  Windows that block had never been exercised, and it was wrong twice over: the
  SDK's `<GL/gl.h>` is not self-contained (needs `<windows.h>` first) and is
  frozen at GL **1.1**, so `GL_CLAMP_TO_EDGE` (GL 1.2) did not exist.
  `src/Pom2GL.h` now does it once, and `opengl-registry` supplies `GL/glext.h`.
- The aarch64 AppImage came out **ET_DYN**, which AppImageLauncher rejects as
  "type -1". AppImageKit never rebuilt the old-style runtime for ARM:
  `continuous/runtime-aarch64` is ET_DYN while `12/runtime-aarch64` is ET_EXEC,
  so the Pi job pins release 12 and passes it via `--runtime-file`.

**Windows ships without host networking for now** (`POM2_HAS_SOCKETS`, new in
`Pom2Build.h`). POM2's three networking TUs are written against POSIX sockets;
Windows has Winsock2, which is a different API, not a `#define`. Rather than
guess, Windows takes the road Emscripten already takes: the SSC opens no telnet
listener and the Uthernet I/II cards plug, reset and answer their registers but
see no traffic. Everything else — CPU, video, audio, disks, printer — is
complete. Guard host-socket code with `POM2_HAS_SOCKETS`, never
`#ifndef __EMSCRIPTEN__`: that assumption ("not a browser, therefore POSIX") is
what broke the Windows build in the first place.

## 2026-07-30 — Bug hunt 5: UI panels fuzzed headlessly; no defects found

The 16 `src/*_ImGui.cpp` files were the largest completely untested surface left
— nothing in `tests/` instantiates an ImGui context. They turn out to be
reachable headlessly: Dear ImGui itself is platform-agnostic (only the backends
need a window), so a context with a built font atlas, a display size and
NewFrame/Render executes every layout, string-formatting and clipping path with
no GPU, no GLFW and no live machine. The panels' own design does the rest —
each takes a plain `Snapshot` struct and returns a plain `Result`, so they can
be driven entirely from synthetic state.

Six panels (Uthernet, Joystick, FloppyEmu, SmartPort, Le Chat Mauve, Toolbar)
driven for **~21 500 frames** under ASan+UBSan with adversarial snapshots —
empty/oversized strings, ImGui markup in labels (`##`, `###`), format-specifier
strings (`%s`, `%d`), NaN/Inf floats, negative and out-of-range indices, empty
and large vectors — at hostile display sizes including 1×1, 4096×2160 and
degenerate 800×1, with the mouse driven over the whole area and buttons/wheel
firing. **No crashes, no undefined behaviour, no ImGui assertion failures** (the
latter would catch unmatched Begin/End or PushID/PopID).

Inspection agreed: cursors are clamped before use (`FloppyEmu` re-homes and
clamps `browseCursor_`/`settingsCursor_` every frame, and loops are bounded by
the vector sizes rather than by the incoming index), lookups are by value with
a null check rather than by raw index (`JoystickPanel::findHost`), fixed-size
`std::array` members are indexed only by constants that match their declared
extents (`kAxes = 2`, `kButtons = 3`), and the `if (!Begin(...)) { End(); }`
pattern is used correctly.

The harness is not committed — it links ImGui into a test target, which is a
build/CI decision rather than a bug fix. It is written and working if that
coverage is wanted.

## 2026-07-30 — Bug hunt 4: persisted floats did not round-trip

**Every float setting shifted on the first save/load cycle.** `Settings::
setFloat` serialised with a bare `os << v`, and `ostringstream` defaults to
**6 significant digits** — not enough to round-trip a float. `1.0f/3.0f` wrote
as `"0.333333"` and read back as a different float. That covers all five
volumes (master / speaker / cassette / both floppy), `ui_scale`, and the ~15
NTSC/CRT and voxel shader parameters: what the user dialled in was not what
they got back. The drift is one-shot rather than cumulative (the reloaded value
re-serialises to the same text), and each shift is ~1e-7, so nothing was
visibly broken — but a persistence layer that doesn't round-trip is wrong, and
it is the same class as the SSC baud-rate bug from hunt 1.

`setFloat` now emits the **shortest** width that round-trips, capped at
`max_digits10` (9). Shortest rather than always-9 on purpose: state.cfg's own
header invites hand-editing, and `0.5` stays `0.5` instead of becoming
`0.500000000` — only genuinely awkward values widen (`0.33333334`).

`settings_roundtrip_test` already existed but only exercised STRING values
(plus two typed values spot-checked as raw strings), which is exactly why this
survived. Extended to cover the typed accessors properly — int/float/bool — and
also boundary whitespace, which `escapeValue` handles specially (load() trims
each line to drop CRLF artifacts, so a value with a leading/trailing space
survives only because of that encoding) but which nothing tested.

**Sweeps that came back clean**, recorded so they aren't redone blind:
- **Write-back identity across the whole writable library** (179 images): load,
  mark every track dirty *without altering a nibble*, `saveDirty()` — the file
  must be byte-identical. It is, for every image. This is the data-loss class:
  if the decode-back-to-source-format were not an exact inverse of the load-time
  encode, merely touching a disk would silently rewrite it with drifted content
  (the shape of the MacBinary bug already recorded in `detectFormat`). Note
  `writeNibbleAt` only dirties on a real change, so the harness has to flip a
  nibble and flip it back; and real WOZ dumps are physically write-protected, so
  WOZ write-back is not covered by this.
- **Mouse card cross-implementation differential** — nothing previously compared
  POM2's two Apple Mouse Card implementations against each other, although both
  claim the same ProDOS firmware contract and each has its own smoke tests. Ran
  identical host-motion scripts through both on full machines, reading the same
  screen holes. Status/button bytes agree exactly; position agrees within a **±2
  quadrature phase residue** that does not converge with more idle time and
  flips sign with the sampling phase. That is inherent to modelling quadrature
  at all (the MCU path decodes real encoder edges; the HLE copies the host delta
  and so has no residue), **not** a defect in either — recorded here so the next
  person to run that comparison doesn't mistake 59 "divergences" for a bug.

## 2026-07-30 — Bug hunt 3: no defects found; six sweeps recorded

A third pass over areas the first two didn't touch. **No bugs.** Recording what
was covered and how, so it isn't redone blind — and one characterised follow-up.

- **//e MMU bank routing, full cross-product**: all six memory-affecting soft
  switches (80STORE, RAMRD, RAMWRT, ALTZP, PAGE2, HIRES) crossed against every
  region boundary, reads and writes — 2 048 checks, clean. The oracle was written
  from the IIe Tech Ref / Sather independently of the implementation, so it
  genuinely tests the properties `iie_aux_paging_conformance_test`'s spot checks
  cannot see: ALTZP governing $0000-$01FF *alone* (RAMRD/RAMWRT must not reach it,
  and ALTZP must not reach $0200+), the 80STORE window not leaking outside
  $0400-$07FF / $2000-$3FFF, HIRES gating only the $2000 window, and PAGE2 alone
  never affecting routing.
- **$C000-$C00F access-type matrix**: those eight switch pairs are write-only on
  the //e. All sixteen addresses verified from both polarities of every switch
  (the existing regression check covers two), plus the $C013-$C018 status reads
  reporting correctly and not self-toggling. 88 checks, clean.
- **Snapshot REPLAY determinism** — the strongest property tried so far, and
  strictly stronger than a capture→restore→recapture blob compare: run to T,
  snapshot, run M more → state A; restore, run M again → state B; A must equal B.
  State absent from the snapshot but still steering execution shows up as a
  RAM/CPU difference, and RAM/CPU *are* in the blob. 60 configurations (5 disk
  images × 2 ROMs × 6 snapshot points, including 5 frames in — mid-boot, LSS
  mid-track) plus //e enhanced/unenhanced and the 1977 ROM: bit-identical every
  time, on the same machine, on a fresh machine, and against two cold boots.
  Caveat worth knowing: this cannot catch host-side-only state (it would NOT have
  found the SSC baud bug, whose effect never re-enters machine state).
- **ProDOS volume decode fuzz**: `decodeVolumeToFolder` writes host files from a
  guest-writable image, so its jail is the thing that matters. 4 000 mutations
  with path-traversal names planted directly at the directory-entry name field
  (`..`, `A/B`, `/etc`, control bytes, dot-only, over-length), self-referencing
  subdir entries, smashed key pointers/EOF fields — checked structurally (scan for
  any entry created outside the target folder + an untouched canary), not by
  trusting the name filter. No escapes, no sanitizer reports.
- **Whole real disk library**: all **4 771** images in the repo through the loader
  and every read path under ASan+UBSan. Zero crashes. 50 refusals, all correct and
  all the same 7 files (duplicated across leftover `.claude/worktrees/` copies):
  800K/hard-disk-sized images handed to the 5.25" loader, which correctly says they
  belong on the HDV card.
- **3.5" / HDV / ATA loaders** (`Disk35Image`, `Block512Backing`) — the untrusted
  surface round 2's DiskImage fuzz skipped: 127 real images + 3 000 mutated cases,
  block and byte accessors probed past the end, truncated `loadFromBytes`. Clean.

**Characterised but deliberately NOT implemented: the Z80 Q register.** The
SCF/CCF gap that keeps the Harte Z80 sweep off a clean unmasked 100 % is now
fully pinned down — exact rule, including why the DD/FD-prefixed forms differ,
validated 1000/1000 on each of the six affected opcode files. Written up in
`DEV.md § Z80 core`. Not done because closing it means maintaining `q` in
**every** instruction epilogue for undocumented flag bits no CP/M or Apple II
software reads; that is a scope decision, not a bug fix. Also recorded there:
MAME's own SCF/CCF expression does not transcribe literally (its `Q` is derived,
not a raw mask — 548/1000 at face value), the same lazy-field trap as `pv_val`.

## 2026-07-30 — Bug hunt 2: Z80 block-I/O repeat flags; sanitizer + fuzz sweeps

**INIR/OTIR/INDR/OTDR set the wrong flags on every repeating iteration.** The
repeating block-I/O opcodes do not just leave the per-iteration INI/OUTI flag
formula in place: while B ≠ 0 the Z80 re-derives X/Y from the rewound PC's high
byte (the same rule LDIR/CPIR already used here), H from B's low nibble when
carry is set, and P/V from the parity of B±1 (or B) xored against the incoming
P/V — plus `WZ = PC+1`. MAME has this as `block_io_interrupted_flags()`
(`z80.cpp:580-604`); POM2 applied the non-repeating formula to all eight
opcodes. The four non-repeating forms (`ed a2/a3/aa/ab`) were already exact.

**Why it survived two exhaustive exercisers:** zexdoc and zexall run under CP/M
and never execute an I/O block instruction, so they are structurally blind to
this — both stayed 100 % green while all four repeating opcodes were wrong on
~99.5 % of vectors. It took a different oracle to see it:
[SingleStepTests/z80](https://github.com/SingleStepTests/z80) publishes the
same per-opcode JSON as the 6502 corpus that found the decimal-SBC bug earlier
the same day, and `Z80::State` already exposes everything it pins (WZ, I, R,
IM, IFF1/2). A full local sweep — **1 092 opcode files, 1 092 000 vectors**,
base + CB + ED + DD + FD + DD CB + FD CB — is now **100 %** apart from SCF/CCF's
F bits 3+5, which need the Q register (already an owned out-of-scope item).

One trap worth recording: MAME's `pv_val` is a **lazy** field whose getter
re-parities the stored byte, so the flag that actually reaches `get_f()` is the
*inverse* of the `(pv_old ^ pv())` MAME stores — P/V lands SET when the two
agree. Transcribing MAME's line literally gets it exactly backwards; the rule
was confirmed against all 3 990 repeat-branch vectors before touching the core.
Pinned by `z80_block_io_flags` (16 vectors inline, spanning carry set/clear ×
data bit 7 set/clear × both H nibble edges — no 1.4 GB download).

**Unsynchronised cycle-counter read in `pom2_headless`.** Its main loop sampled
`Memory::getCycleCounter()` bare while the CPU worker wrote it under
`stateMutex` — a real data race (ThreadSanitizer on the running binary), benign
on x86-64 but UB, and the wrong example to set next to `MainWindow::
renderStatusBar` and `AiControlServer`, which both take the lock for the same
read. `pasteText` on the next line was already safe (Memory's own `kbMutex`).

**Sweeps that came back clean** — recorded so they don't get redone blind:
- **ASan + UBSan over the whole 149-test suite**: zero memory-safety and zero
  undefined-behaviour reports. (Use an in-tree build: 3 tests exceed their tight
  `TIMEOUT` under instrumentation and `disk_skew_sniff` needs cwd = repo root.)
- **TSan on the shipping cross-thread surface**: 113 k concurrent HTTP requests
  across every state-touching AI-control endpoint against a *running* CPU worker,
  with rewind scrub/park interleaved — no races. Worth knowing that
  `ai_control_server_smoke_test` deliberately parks the controller in `Stopped`,
  so that configuration otherwise has no coverage at all. TSan needs
  `setarch -R` on this kernel (high-entropy ASLR → "unexpected memory mapping").
- **7 329 mutated disk images** (truncations, header-field smashing, magic
  swaps, forged MacBinary wrappers, seeded from real WOZ1/WOZ2/2MG/NIB/D13/PO/
  DO/DSK) through `loadFile` + every nibble/bit-cell/flux read path + the media
  snapshot round trip, under ASan+UBSan: no crashes.
- **~30 k mutated machine snapshots** (section-length smashing, tag rewriting,
  chunk splicing, truncation) through `restoreMachineState` with real cards
  plugged, under ASan+UBSan: no crashes.

## 2026-07-30 — Bug hunt: CMOS decimal SBC, CPU-oracle harness, SSC baud restore

**The 65C02 decimal SBC was using the NMOS correction rule.** The WDC 65C02
lets the low nibble's `-6` decimal adjustment **borrow into the high nibble**;
MAME names it outright in `w65c02.cpp:28-46` (`do_sbc_cd`): *"SBC allows
interdigit carry from decimal adjustment on 65C02"*. It packs both nibble
differences and only then applies `-6`/`-$60` to the whole byte, so the borrow
propagates. The NMOS part corrects each nibble in isolation and it never does —
meaning **the two CPU parts genuinely return different accumulators for the same
operands**, and a single shared code path cannot be right for both. `M6502::SBC`
now branches on `cpuMode`.

Measured against a full 256-opcode Tom Harte sweep: every decimal SBC
addressing mode was wrong ~3.4% of the time — `e1,e5,e9,ed,f1,f2,f5,f9,fd`,
*nine* opcodes, where the previous note in `M6502.cpp` had claimed only `e9`
and had written the divergence off as an unmodellable silicon quirk. It is
modellable; MAME models it, and MAME is this project's source of truth. Result
values moved by exactly $10 (the dropped borrow) and the N flag with them.
Divergence is confined to **invalid** BCD digits, so no correct-software
behaviour changes — but "officially undefined" is not the same as "free to get
wrong", and it was masking a real parity gap. WDC CMOS is now 100% on 255 of
256 opcodes (2 530 000 vectors). Pinned by `decimal_sbc_cmos`, which embeds the
corpus vectors inline so it runs without the 1.4 GB download.

**The exhaustive CPU oracle could not actually be run.** `tomharte_cpu_test`'s
`runVector` restored PC/SP/A/X/Y/P and RAM per vector but never re-armed the
CPU's KIL/JAM + STP `halted` latch. `step()` short-circuits before the opcode
fetch while that latch is set and only a reset clears it, so the **first**
vector landing on an NMOS JAM ($02/$12/$22/…) or a CMOS STP ($DB) froze the
shared CPU for every remaining vector in the run: a full NMOS sweep scored
20 000/2 560 000, because file `02` poisoned the other 254 files. The curated
CTest subset never caught it — no JAM opcode is in the manifest. This is why
the "100%" claims in `DEV.md` had only ever been checked on 41 opcodes; with the
latch cleared, the real numbers are in `DEV.md § Tom Harte`, including the fact
that the failing NMOS files are **exactly** the 78 undocumented opcodes with
observable side effects (a much stronger statement than the subset could make).

Two related traps recorded rather than "fixed", since both are deliberate:
- **`$5C` fails the CMOS sweep on purpose.** POM2 charges 3 bytes / 8 cycles,
  matching MAME (`ow65c02.lst` `nop_c_aba`) and the standard 65C02 unused-opcode
  tables. All three of Harte's 65C02 variants say 4. That corpus is generated
  from an implementation conforming to documentation, not from silicon, so MAME
  wins — but `5c : 0/10000` is now documented as expected, not a regression.
- **`tomharte_6502`/`tomharte_65c02` report `Passed` in 0.00 s with no corpus
  on disk** (soft-skip, so networkless CI stays green). A green tick from those
  two names does not mean the CPU was validated. Called out in `DEV.md`.

**Super Serial Card restored its baud rate but not its baud pacing.**
`bytesPerSecond_` is derived from the control register's divider and is not
serialized — the same reason `wordLength_`/`extraStop_` are restored
explicitly. Only `applyControlReg` ever computed it, and a snapshot load is not
a register write, so a restore left the rate the **live** session was last
programmed to: a 300-baud snapshot loaded into a 19 200-baud session drained
the TX ring 64× too fast, and the reverse stalled it. Rewind hit this on every
frame. `loadSnapshotState` now recomputes it and resets the pacing budget +
drain clock, exactly as `applyControlReg` does — a stale `lastDrainTime_` would
otherwise credit the restored rate for all the wall-clock time before the load
and dump a burst. Pinned in `card_snapshot_state` (both directions, so the fix
can't be a hard-coded slow default).

## 2026-07-30 — Post-review sweep: 13 defects in the same day's own code

Two adversarial reviewers went over everything written that day (none of it
had been read by anyone). Findings, all fixed:

**The window persistence did not actually work** — the feature validated by
hand was broken twice over:
- The shutdown capture was **dead code**: `~MainWindow` runs *after*
  `glfwTerminate()` (it is a local of `main`), so `glfwGetWindowPos/Size`
  bailed on the un-init check, zeroed their out-params, and the
  `savedWinW_ <= 0` guard swallowed the write. Quitting normally persisted
  nothing. Moved into `captureWindowGeometryNow()`, called by `main()` while
  GLFW is still alive.
- **`--kiosk` triggered a video-MODE SWITCH**: `setGlfwWindow` (which
  restores geometry) ran *before* `setKioskMode`, so `kiosk_` was still
  false and `glfwSetWindowSize` hit an exclusive full-screen window — which
  GLFW reads as "change the desired video mode". Ordering swapped.
- On X11, measuring right after `glfwRestoreWindow` still read the
  **maximized** rect (the call only posts a `_NET_WM_STATE` message), and
  un-maximized the user's window for nothing. The flag is now recorded
  without touching the window.
- The geometry clamp validated against the primary monitor only, so a
  window kept on a **second display** was dragged back to screen 1 on every
  launch (a monitor left of primary has negative virtual-screen X). Now
  checked against every monitor's work area.

**The kiosk read-only promise had holes**: only 4 of ~20 save sites checked
the flag, so `--kiosk` → F10 → change a profile rewrote `state.cfg`. Fixed
centrally in `Settings::save()` — no call site can forget it now. Also: F10
fired on auto-repeat (holding it flipped full-screen ~30×/s, each entry
doing a disk write); the kiosk menu footer reserved 4 action rows for 5, so
QUIT was clipped below the panel edge with no scrollbar; and leaving kiosk
resumed a machine the user had **deliberately** paused before pressing F10
(the menu's pause is not ours to undo when it was a no-op).

**Emulation core:**
- **The VBL IRQ survived a reset** with no way to clear it: a //c that had
  enabled it re-asserted on the next frame edge into a freshly reset
  machine, and `$C05A` (DisVBL) only decodes while IOUDIS is clear — which
  reset forces back true. `resetSoftSwitches` now disarms and drops the
  line. Self-inflicted by the same day's decision to start asserting it.
- **Snapshot restore did not re-drive the VBL line**, and the `$C070` ack
  was gated on the latch — a rewind landing "not pending" while the line
  was asserted wedged the //c in its IRQ vector forever. The ack is now
  unconditional (as MAME's `lower_irq` is) and restore re-drives the line.
- **The Mockingboard could drone forever after a reset**: the audio thread
  resyncs its register bank only when `ayResetCount_` *changes*, and
  `onReset` set it to 0 — a no-op whenever it already was. It now bumps,
  and clears the pending event queue.
- The AY replay cursor **truncated to whole cycles** (23.19 → 23), drifting
  0.8-1.4 % slower than the producer — larger than the PAL/NTSC delta
  `setCpuClock` exists to fix. Now carries a fractional remainder, like
  `SpeakerDevice::subSampleAccum`.
- A **rewind stranded that cursor**: rolled-back stamps fall far below it,
  so every event collapsed onto sample 0 for the whole rewound span. A
  backward-jump guard re-anchors it.
- Queue overflow dropped the **oldest** event permanently (nothing else
  re-seeds the audio bank); it now drops the queue and forces a resync.
- `M68705P3::kSnapshotBytes` was **137 for 138 bytes written** (the register
  group is four bytes, not three), letting a short blob past the length
  guard and reading one byte past the caller's buffer.
- Two `>` that should be `>=` in "untrusted" clamps (ATA `wordIdx_`, HDV
  `streamOffset`) allowed an index one past the end — an OOB read *and
  write* on the ATA PioOut path.
- The Chat Mauve slot-3 guard tested **occupancy, not type**, so plugging
  the card into slot 3 made it inert; it now only yields to a foreign card.
- `ClockCard::setCpuClock` silently no-opped for non-TP modes (it replayed
  `programTpTimer`, whose `default:` leaves the rate alone); it now
  re-derives from the cached rate. Its snapshot also clamps `tpRateHz_` /
  `tpAccumCycles_` — a crafted blob could give `advanceCycles` a ~2^31
  iteration loop. Stale "48-bit shift register" docs corrected to 40.
- `MouseCard` restore did not re-drive the slot IRQ (the wire-OR lives in
  `SlotPeripheral`, not the PIA), losing a pending mouse interrupt.
- **`tickFrame`'s wall-clock scaling is now `#ifdef __EMSCRIPTEN__`.** The
  browser is its only production caller; every other caller is a headless
  test where "one call = one frame budget" is the contract, and scaling
  collapsed the budget to ~1 cycle. Two existing tests survived only by
  accident. (The suite's slow-test time went 116 s → 295 s once they began
  doing real work again — the fix is measurable.)

## 2026-07-30 — Full screen ⇄ windowed at runtime (kiosk is now a mode, not a launch flag)

Kiosk used to be decided once, on the command line. It is now a runtime
toggle in both directions: **F10**, View → Full screen (kiosk), the
`view.kiosk` command-palette entry, or a new **EXIT KIOSK (WINDOWED)** action
in the in-kiosk menu (so someone who never reads shortcuts can still get
out).

**No snapshot round-trip is involved** — that was the obvious idea, and it
turned out to be unnecessary. Kiosk touches exactly three things: the GLFW
window (exclusive full-screen vs windowed), the render path (`render()`
early-returns to `renderKiosk()`), and settings writing. The CPU, memory and
slot cards are never involved, so the switch is instant and lossless: a game
keeps playing across it, mid-frame. Entering saves the windowed geometry and
restores it on the way back; a session launched with `--kiosk` (which has no
windowed geometry to restore) gets a centred default.

The window geometry is now **persisted** (`window_x/y/w/h`,
`window_maximized` in `state.cfg`) — it previously lived nowhere at all, so
there was literally nothing to restore from. It is written on the way INTO
kiosk (the last chance: kiosk never writes state.cfg) and at normal
shutdown, and applied in `setGlfwWindow` so a plain relaunch also reopens
where you left off. Leaving kiosk prefers the geometry measured this
session, falls back to the persisted one (the `--kiosk`-launch case), and
only then to a centred default. A saved position is clamped back onto a
monitor so a window saved on a since-disconnected screen can't reopen
off-screen.

Details worth knowing:
- **F10 was already taken.** It was the keyboard fallback for the in-kiosk
  Start menu, so entering kiosk ALSO opened that menu in the same frame
  (`onKey` runs during `glfwPollEvents`, before render) — the user asked
  for the game to go full-screen, not for a menu. The kiosk menu's keyboard
  fallback moved to **F1**; the gamepad Start button is unchanged.
- **Leaving full-screen needs the geometry re-applied explicitly.** Many
  window managers ignore the position/size passed to
  `glfwSetWindowMonitor` when un-fullscreening, so the call is now followed
  by `glfwSetWindowSize` + `glfwSetWindowPos` (the standard GLFW
  workaround), and a maximized window is remembered as a flag and
  re-maximized rather than restored as a giant un-maximized rectangle.
- **Leaving kiosk un-pauses.** The in-kiosk menu pauses the machine while
  it is up; exiting from an open menu would otherwise strand the user in
  the GUI with a silently stopped CPU.
- **F10 is routed unconditionally**, alongside F9/F11/F12 — entering kiosk
  from a focused text field must work, and *leaving* it must ALWAYS work. It
  also fires with the in-kiosk menu open, which otherwise swallows keys.
- **The `--kiosk` read-only promise is preserved.** The README says a kiosk
  session "can't disturb your desktop setup". Naively, toggling to the GUI
  would have resumed writing `state.cfg`. A session LAUNCHED in kiosk now
  stays read-only for its whole life (`settingsReadOnly()`), while a GUI
  session that enters kiosk saves once on the way in (so GUI-side changes
  aren't lost if the user quits from kiosk) and is read-only only while
  there.

## 2026-07-30 — PAL + Le Chat Mauve audit: the PAL clock is right, NTSC is the off one

Targeted hunt on the PAL profiles and the Le Chat Mauve RGB card.

Closed the two follow-ups from that audit:
- **WASM ran PAL 20 % fast.** `tickFrame()` burned a full `cyclesPerFrame`
  budget per call, but the browser drives it once per DISPLAY refresh
  (`emscripten_set_main_loop_arg(..., fps = 0)`) — on a 60 Hz panel a PAL
  profile executed 20313 × 60 = 1.22 MHz with a guest VBL at 60.1 Hz
  instead of 50.08. (NTSC had the same hazard on 120/144 Hz panels.) The
  budget is now scaled by the wall time actually elapsed, in units of the
  machine's own `frameIntervalUs`, capped at 4 frames so a backgrounded
  tab can't dump seconds of emulated time into one call. The threaded path
  is untouched — `workerLoop` already sleeps to an absolute deadline.
- **The "unreachable MAME RGB HGR mode" was a false positive**, verified
  against the fetched `apple2video.cpp`. MAME's `hgr_update` gate
  (`rgb_monitor() && m_dhires && !m_80col`, where `m_dhires = !AN3`) is the
  **Video-7** card's foreground-background mode — an American product POM2
  does not model. POM2's Duochrome is the **Le Chat Mauve "Eve"**'s own
  $C0BA/$C0BB soft switch (brevet), which is why the `!state.dhgr` term
  looks inverted, and it IS guest-reachable (`STA $C0BB`; snapshotted since
  blob v2) rather than UI-only. Changing the gate would have broken the Eve
  model and altered the //c PAL default picture, so the divergence is now
  spelled out in a citation comment instead — a real Video-7 would need its
  own card class.

**Headline: POM2's PAL frequency is correct — more accurate than MAME's.**
An Apple II scanline is 65 CPU cycles but **912 master-clock periods**
(64 × 14 plus one stretched "long cycle" of 16). The PAL crystal
14.250450 MHz was chosen so that same 912-period line lands exactly on the
PAL broadcast line rate (912 × 15 625 = 14 250 000), so the long-cycle
average is 15 625 × 65 = **1 015 625 Hz** — POM2's value, right to 0.003 %.
The naive 14.25045/14 = 1 017 889 ignores the long cycle (0.22 % fast), and
MAME's 1 016 966 is just its NTSC figure scaled by the crystal ratio, so it
inherits that figure's own 0.13 % error. The comment in `CpuClock.h` had
this reasoning **backwards** (it apologised for a "deliberate deviation"
that is in fact the accurate number) and has been rewritten — a future
"align with MAME" would have made PAL worse. Noted alongside: POM2's NTSC
clock IS the naive divider and runs 0.22 % fast (guest sees 60.05 Hz vs a
real 59.92 Hz); left as-is because every NTSC-era constant, test and golden
capture is calibrated against it.

Verified correct, no change needed: the floating-bus scanner is fully
PAL-parameterised (its vertical counter runs $C8..$1FF on PAL vs $FA..$1FF
on NTSC, a faithful port of MAME `apple2video.cpp`), as are
`frameCycleToPos`, `pushVideoEventLocked`, the beam segmentation, the FLASH
counter and the 50 Hz worker pacing. The Chat Mauve's AN3 FIFO is bit-exact
with MAME (rising-edge only, 2-bit depth, COL140 reset) and the //c PAL
profile still reaches all four RGB modes after last pass's IOUDIS gating —
IOUDIS powers up *true*, so $C05E/$C05F reach the card.

Fixed:
- **Mockingboard AY replay cursor was left on the NTSC clock** — a
  regression from this same day's emuCycles queue. Under PAL the audio
  thread advanced its cursor 0.7 % faster than the CPU produced cycles, so
  it outran every queued write and applied them all at the buffer START,
  silently undoing the sub-buffer timing on exactly the PAL demos (French
  Touch / DIX) it was built for. Retuned via a new virtual
  `SlotPeripheral::setCpuClock`, applied both on a video-standard change
  and at plug time (a Slot Config "Apply" re-plugs without re-running the
  profile's standard step).
- **The //c VBL interrupt is finally asserted.** `vblIrqPending` was set
  but the CPU line was never driven, on the stated grounds that POM2 "does
  not model IOUDIS" — stale since this week: IOUDIS *is* modelled and
  $C05A/$C05B only reach the VBL mask on //c-class with IOUDIS clear, so
  the arm is now unambiguous. A //c PAL demo using the VBL IRQ as its 50 Hz
  frame sync previously spun on its wait flag forever or free-ran with
  tearing. IIe keeps the polling-only behaviour (there $C05A/B really are
  annunciators, and asserting would resurrect the original ProDOS crash).
- **AppleWin mouse VBL period used the CPU budget, not the video frame** —
  20313 instead of 20280, drifting 33 cycles/frame, a full frame of phase
  every ~12 s. A //c PAL program using the mouse VBL IRQ as a raster
  timebase watched its sync point crawl down the screen. (The card's own
  comment claimed it was locked to the beam.)
- **ClockCard TP period followed the NTSC constant** — the uPD1990AC's TP
  derives from the card's own crystal (a real-time reference), so 64 Hz TP
  ran at 63.55 Hz wall-clock under PAL.
- **Le Chat Mauve's $C0B8-$C0BB Eve registers aliased slot 3.** That range
  IS slot 3's device-select window: an SSC there drives its ACIA
  data/status/command/**control** at exactly those addresses, so a serial
  driver's `STA $C0BB` (baud setup) flipped the Eve's HGR-Duochrome bit and
  turned the picture to garbage. Now decoded only when slot 3 is empty —
  which the card's real home (//c-class, no physical slots) always is.
- **The two Eve toggles are snapshotted** (blob v2, v1 still loads). They
  were documented as "user settings, not guest-volatile" but the
  $C0B8-$C0BB decode mutates them from the guest bus, so a rewind past a
  `STA $C0BB` left the display stuck in Duochrome.
- **`pal_timing_test` now pins the floating bus under PAL** — the one
  PAL-geometry consumer nothing covered, and the one where a stray 262
  would be silent (a wrong RNG byte, a vapor-lock that never fires). RAM is
  filled with an address-revealing pattern so the probe actually
  discriminates.

## 2026-07-30 (later) — The LOW backlog is empty

All seven remaining items from the 2026-07-29 workflow hunt, fixed and
tested:

- **NMOS `NOP abs,X` pays its page-cross cycle.** New `UnoffAbsX` handler
  (reads the operand, adds X, +1 when the page changes — MAME om6502
  `nop_abx`); `$1C/$3C/$5C/$7C/$DC/$FC` route through it in NMOS mode.
  The 65C02's flat-4 entries for `$DC/$FC` stay as they were.
- **Snapshot DMA disarm is lazy.** Kicking a live bus master (SoftCard
  Z80) off the bus used to happen BEFORE a single byte was read, so an
  empty, foreign or immediately-truncated file killed CP/M for nothing.
  It now fires on the first section that actually mutates the machine —
  and a well-formed file carrying neither CPU nor MEM is reported as an
  error instead of a silent success.
- **CLI Phase-C ordering is deterministic.** The deferred actions
  (`--run` / `--paste` / `--step`) slept a fixed 250 ms while the
  positional-disk boot fired on a 30-frame countdown, so the order
  flipped with the host refresh rate (~500 ms at 60 Hz, ~208 ms at
  144 Hz — the actions then ran against a machine the boot was about to
  reset). The deferred thread now waits on a `bootDiskSettled` gate
  (bounded ~5 s so a failed boot can't wedge it), pre-set when there is
  no positional disk so that path keeps its exact old timing.
- **AY READ drives the bus.** `applyControl` counted the READ strobe and
  did nothing else, so a driver probing the chip (write a register, read
  it back — a common presence check, and how some Phasor mode detectors
  identify the board) saw the VIA's own stale port-A output. The AY now
  exposes `busOut`, `Via6522` grew a real port-A INPUT pin model
  (`readPortA` mixes `(out & ddr) | (pin & ~ddr)`, snapshotted), and both
  Mockingboard and Phasor latch the value on a READ — MAME's `m_porta`
  shadow.
- **`Apple2Display::render()` routes from the PUBLISHED frame.** It
  sampled `mem.getDisplayState()` — the live recording frame, already
  running ahead — to pick the demod / mixed-mode path for pixels
  belonging to the published frame, and stored that same wrong state in
  `lastRenderState_` (which the present path consumes). It now folds the
  published events onto the published frame-start state, and keeps the
  composite path alive when ANY band in the frame was graphics. With no
  events the two states are identical, so the non-beam-raced path is
  bit-for-bit unchanged.
- **`$C019` VBL samples at the data-fetch cycle.** The IIe beam-state
  read used bare `cycleCounter`, which only advances at end-of-
  instruction — up to 7 cycles early, enough to report the wrong side of
  a VBL boundary to beam-racing code. It now adds the in-flight
  instruction progress, the same stamp `floatingBus()` and
  `pushVideoEventLocked()` already use.
- **Z80 block-repeat X/Y flags come from PCH.** On a repeating LDIR /
  LDDR / CPIR / CPDR iteration MAME overwrites the undocumented X/Y
  flags with bits 13/11 of PC (`m_f.yx_val = PC >> 8`); POM2 kept the
  per-iteration data-derived value, so F was wrong for the entire run of
  the instruction. zexall stays clean.

## 2026-07-30 — Mockingboard emuCycles queue, MouseCard MCU snapshot, serial parity

The last four items from the workflow hunt's backlog, all pinned:

- **Mockingboard AY register writes are now emuCycles-stamped.** The audio
  thread used to snapshot both AY register banks ONCE per buffer, so every
  write inside that window collapsed to the last value — an arpeggio or
  fast envelope written at ~1 ms intervals came out quantised to the
  ~10 ms buffer (notes merged or dropped outright). That was the real
  emuCycles violation. The CPU thread now stamps each accepted AY store
  with the VIA's synced cycle and queues it; the audio thread owns its
  register bank and replays each write at its exact sample offset, using
  SpeakerDevice's cursor idiom (including the catch-up snap for
  pause/resume and turbo). A PB2 reset still resyncs the bank wholesale
  since that path zeroes registers outside the event stream.
- **MouseCard finally snapshots** — the last card without serialization.
  It needed state surfaces on its two embedded components first:
  `M68705P3` (registers, 112 B RAM, port latch/DDR/input triples, timer,
  interrupt latches — the 2 KB EPROM is ROM and stays out) and `MC6821`
  (both register pairs + the CA/CB edge latches). The card wraps them
  with its bridge state (ROM bank, PIA→MCU port shadows, quadrature
  counters, MCU pacing). Host pointer position is deliberately NOT
  serialized: that is where the user's mouse physically is, not emulated
  state, and forcing it backwards on a rewind would fight the UI.
- **Three serial/clock claims that died unverified on a spend limit were
  re-judged and all three survived**:
  - *SSC RDRF never re-armed.* Real hardware has a one-byte RDR, so MAME's
    mos6551 raises the RX interrupt for every assembled byte; POM2 holds a
    4 KB host ring and raised it once per delivered TCP chunk, so an
    interrupt-driven guest driver read ONE byte per chunk and stalled. The
    RDR read now re-arms while bytes remain queued (each re-arm consumes a
    byte, so it cannot storm).
  - *DCD/DSR polarity was inverted.* These are active-low pins: the status
    BIT is set when the line is INACTIVE. MAME inits `m_dsr(1), m_dcd(1)`
    and AppleWin is explicit ("DSR is active low (see SY6551 datasheet)").
    POM2 reported carrier-present on an idle listener and carrier-lost the
    moment a client connected. Flipped, with the test expectation.
  - *The uPD1990AC shift register is 40 bits, not 48.* MAME shifts 5 bytes
    for a non-4990A part, and disassembling the shipped
    `roms/thunderclock_u9_v1.3.bin` settles it: the nibble routine at
    $CACF emits 4 CLK pulses and the time-read path calls it 10× = 40
    pulses over sec/min/hour/day/month+dow — **there is no year on this
    card**. Reads were unaffected, but MODE_TIME_SET landed one byte out
    of alignment and committed the garbage silently (a set of 23:58:59
    Dec-31 read back as 00:59:58 day 23). The register is now 40-bit, the
    year comes from the host clock (what a real ThunderClock+ does — ProDOS
    supplies its own), and `clock_card_smoke` drives 40 pulses, making it a
    firmware-parity test instead of a model tautology.
- **NMOS KIL/JAM now really jams.** It re-pointed PC at the opcode, so
  step() still serviced IRQ/NMI and an interrupt-driven program could walk
  out of a jam real silicon never releases. It routes through the same
  `halted` latch STP uses — checked before the interrupt poll, cleared only
  by reset, and snapshotted.

## 2026-07-29 — Workflow bug hunt #2: 38 confirmed findings, 13 fixed this pass

A 10-finder / adversarial-verify agent workflow swept the subsystems the
first hunt didn't touch (CPU, paging, Z80, display, audio, storage,
SmartPort/HDV, snapshot, serial, CLI). 38 findings survived verification;
the highs and the sharpest mediums are fixed, the rest is tracked in
TODO.md ([Cards] section). Fixed here:

- **MacBinary-wrapped .dsk write-back corrupted every non-dirty track**:
  detectFormat stripped the 128-byte header at load but saveDirty
  pre-filled from file offset 0 and truncated the file to a bare image.
  The MacBinary wrapper now rides the 2IMG envelope plumbing (captured at
  load, re-emitted by every save path — .dsk/.nib/.d13/.woz), and
  MacBinary-wrapped WOZs actually load now (loadWoz re-read the file and
  demanded the magic at offset 0).
- **saveDirty wrote in place with O_TRUNC**: an ENOSPC/IO failure
  mid-write destroyed the original image, and the retry then zero-filled
  the rest. All four branches now write a sibling temp file and rename
  over the original (atomic on POSIX); failure leaves the source intact.
- **SmartPort 3.5" eject discarded dirty blocks** despite the UI
  checkbox literally saying "save on eject" — eject() now flushes first,
  mirroring SmartPortHdvUnit.
- **Rewind/snapshot-load left the beam-racing video-event log stale**:
  events stamped with pre-restore (future) cycles broke the publication
  carry loop — publishedEvents_ came out empty every frame for the whole
  rewound span while videoEvents_ grew without bound. The log is now
  resynced from the restored clock in Memory::loadSnapshotState.
- **Slot/ROM rebuilds ran outside stateMutex while the AI control server
  was live** (applyProfile steps 5-7, restartEmulationFromSettings 3-4):
  a /reset or /cpu poll during a profile switch raced the ROM rewrite
  and SlotBus unique_ptr swaps. Both spans now hold the lock.
- **STP ($DB) halt latch invisible to snapshot/rewind**: rewinding out of
  a crash kept the machine frozen; restoring a halted snapshot woke STP
  without RESET. Serialized (CPU section 16→17 B, legacy blobs default
  to not-halted).
- **Truncated snapshot half-restored and reported success** —
  restoreMachineState now surfaces the reader's error state.
- **VIA T1LH write now clears IFR.T1** (MAME 6522via.cpp VIA_T1LH; the
  old comment claimed the opposite while citing the same case).
- **13-sector WOZ never detected** (DOS 3.2 .woz couldn't boot): WOZ2
  INFO+38 boot_sector_format==2 is honoured, with a bit-aligned
  D5 AA B5-vs-D5 AA 96 track-0 sniff for WOZ1/format-0.
- **//c IOU parity set**: IOUDIS finally gates $C058-$C05F (MAME do_io
  `(m_isiic) && !m_ioudis` — mouse/VBL switches, no AN3/DHGR flips);
  $C019 on //c returns the LATCHED VBLINT flag (Tech Note #9 semantics,
  MAME c000_iic_r:2256) instead of the IIe beam state; any $C070-$C07F
  access acks the VBL interrupt; and INTC8ROM/IOUDIS/VBL-mask/pending
  ride a new length-prefixed snapshot section.
- **AI /screen vs UI demod race**: the post-stateMutex demod/pixels
  phase is now serialized by a display-owned demodMutex taken in the
  same stateMutex→demodMutex order on both threads.
- **2IMG-wrapped 5.25" floppies classify correctly** (the common Asimov
  format fell through to Unknown in classifyDiskForSlot/accept525).
- **Legacy-spun Disk II motor is promoted into the LSS on first insert**
  (motorOn=true with the LSS idle used to hang the boot PROM poll).

A second pass the same day cleared most of the LOW batch too:
- **NMOS undoc-NOP cycle counts** for the opcodes setCpuMode(NMOS)
  remaps: $14/$34/$74 → 4 (zp,X), $0C/$1C/$3C/$7C → 4 (abs/abs,X),
  $80/$89 → 2 (#imm) — MAME om6502; the generic Unoff2/Unoff3
  stand-ins drifted 1 cycle per instruction (the Mr. Robot RWTS drift
  class). Pinned in cpu_cycle_count_test.
- **VIA ACR write re-arms T1 in continuous mode** (MAME VIA_ACR:
  `m_t1_active = 1` + adjust) — a one-shot-fired T1 stayed dead after
  the guest flipped ACR to continuous.
- **Hostile-WOZ hardening**: per-track bitCount cap (1 Mi-bit) +
  aggregate 32 MB expansion budget; also fixed three stale pre-strip
  size bounds the MacBinary WOZ fix had left behind (OOB read on a
  wrapped WOZ).
- **Snapshot restore honesty**: MEX section failure now propagates
  (state was mutated with ok=true before); speaker/rewind resync runs
  even when the restore FAILS (a truncated file has already applied
  CPU+MEM — the early return skipped the resync exactly when needed);
  the snapshot's cpuMode byte is no longer applied (machine
  configuration, not state — it bypassed resolveCpuMode's
  soldered-65C02 clamp and froze a //c on an NMOS-mode blob).
- **Rewind media restore is gated on the capture predicate** — applying
  a ring frame's decoded tracks onto a drive that NOW holds a WOZ wiped
  the WOZ's canonical bit streams.

A third pass closed the per-card snapshot gaps — five of the six cards
that serialized nothing now carry their guest-visible state, pinned by
the new `card_snapshot_state` test (each case drives the card into a
distinctive state, restores into a fresh card, and re-serializes to
prove the PRIVATE fields travelled; every loader also ignores a foreign
blob):
- **CffaCard** — the whole ATA taskfile plus the in-flight PIO phase,
  LBA, sector counter, 512-byte word buffer and `wordIdx_` cursor
  (`AtaBlockDevice::append/loadSnapshotState`, CHS geometry included
  with divide-by-zero guards on restore). A rewind mid-transfer used to
  resume the guest's read loop against the live cursor.
- **ProDOSHardDiskCard** — selected block + byte cursor within it.
- **SmartPortCard** — a v1.1 tail carrying the $Cn0D protocol call
  engine (`spCollect_`/`spCollectN_`/`spResult_`/`spResultPos_`/
  `spPushPages_`/`spError_`); v1 blobs still load and reset the engine
  rather than letting the live one leak through.
- **SuperSerialCard** — the ACIA command/control decode (DTR, RX-IRQ
  enable, echo, word length, baud index), sticky status errors and IRQ
  mask. The socket, rings and printer spool stay host-side on purpose:
  a rewind cannot un-send bytes that already left the wire.
- **ClockCard** — the uPD1990AC 48-bit shift register, edge-detect
  latches, mode, user time offset and the TP/IRQ timer, re-driving the
  slot IRQ line from the restored flip-flop. A rewind mid-shift-out
  used to hand ProDOS a garbled date.

MouseCard is deliberately left for its own session (it embeds an
M68705P3 MCU + MC6821 PIA that need state surfaces first) — see TODO.md.
Also still deferred: the Mockingboard AY event-queue redesign and the
residual LOW items (KIL IRQ-permeability, Z80 block-op X/Y flags, AY
READ bus latch, NOP abs,X page-cross +1, and friends). The serial-input
finder's claims died unverified on a spend limit — parked, not judged.

## 2026-07-29 — //c+ boots and WRITES 5.25" for real (the "dual-controller" was three bugs)

The bug-hunt's 🔴 "//c+ dual-controller" entry got its repro — and the
repro showed the //c+ never even reached the disk: **every cold boot hung
at $F0FC with a blank screen**, in the alt firmware's boot drive-scan.
`tests/iicplus_boot_probe` (headless full //c+ stack: IWM + SmartPortHub +
2× Sony 3.5" + slot-6 Disk II) plus a `POM2_TRACE_IWM_SENSE=1` diagnostic
narrowed it to three distinct bugs:

1. **IWM SENSE with no selected drive** — the firmware polls the IWM
   status register with devsel=0 before enabling anything. MAME
   `iwm.cpp:129` reads `(!m_floppy || m_floppy->wpt_r()) ? 0x80 : 0` —
   no floppy → SENSE pulls HIGH. POM2's `disk_` is permanently attached
   by DiskIICard, so the read answered with the 5.25" image's
   write-protect bit: a writable disk → 0 forever → the scan's very
   first `LDA $C0EE / BPL` never fell through.
2. **Sony DSKCHG polarity** — MAME (`floppy.cpp:560/672/723`, mac wpt_r
   `!m_dskchg`) senses HIGH for an *empty* drive; POM2's `diskSwitched_`
   flip-flop read 0 ("disk in place") for an empty external 3.5", so the
   scan walked into the read-a-disk path of a drive with no disk. DIR
   init was also inverted (MAME `m_dir(0)`). A dead `SmartPortHub::
   onIwmMotor` broadcast helper (motor to BOTH Sonys, contradicting
   MAME `iwm.cpp:99-115 set_floppy` motor-follows-selection) was removed
   before it could be wired by accident.
3. **The actual dual-controller hazard, on writes** — with the boot
   fixed, DOS 3.3 booted but `SAVE` ended in **I/O ERROR**: the IWM's
   bit-cell walker (authoritative $C0EC reads) mis-frames RWTS's
   write-verify, and `IWMDevice::flushWrite` pushed 5.25" flux into the
   same DiskImage DiskIICard's LSS was writing (double write). Fixes:
   the IWM never writes 5.25" flux (DiskIICard owns it; the IWM keeps
   the 3.5" Sony write path, which no other controller sees), and
   `ioReadIWM` is authoritative **only while the hub routes to a 3.5"
   Sony** — the POM2 split of MAME's single-controller
   `recalc_active_device` model.

After: //c+ cold-boots to the DOS 3.3 banner and a full
`SAVE / LOAD / RUN` round-trip works on the //c+ profile, in both
authoritative and shadow modes. Print Shop boots to its title screen.
Pinned by `iic_plus_boot_write` (unit sense polarities + full-machine
boot + write round-trip). The MAME oracle (`apple2cp` romset assembled
from POM2's own `apple2cp.rom`, CRC-identical to 341-0625-a.256)
confirmed the expected boot banner behaviour.

Worth keeping: the "//c+ 5.25" auto-boot works" claim had silently
rotted — no test covered it, so the SENSE regressions were invisible
until the dual-controller investigation went looking. The scan hang
looked exactly like the dual-controller symptom but was three unrelated
MAME-parity deviations stacked.

## 2026-07-29 — Bug-hunt sweep: 20+ fixes across W5100, CS8900A, ImageWriter, IWM snapshots

A five-agent audit of the two Ethernet/printer commits (04890e1, f7af757)
plus their integration seams, every finding verified in code before fixing.
The ones worth remembering:

- **W5100 TCP silently lost data on a slow peer.** SEND advanced SN_TX_RD
  *before* the single non-blocking `sendto()`, so a short write or EAGAIN
  dropped the tail while the guest saw a fully-free ring. TCP now stashes
  the unsent tail per-socket (`pendingTx`) and `poll()` retries until it
  drains (1 MiB cap → honest connection close). UDP keeps fire-and-forget
  on purpose: queueing datagrams would fuse their boundaries, and dropping
  one on EAGAIN is legal.
- **Peer FIN now parks in SOCK_CLOSE_WAIT ($1C)** instead of collapsing to
  CLOSED — the guest can still SEND before DISCON, which is what every
  drain-then-disconnect W5100 driver keys on. CONNECT is gated on
  TCP-INIT (it used to "establish" UDP sockets and drop their RX header),
  and CONNECT to DIPR 0.0.0.0 closes instead of reaching 127.0.0.1 via
  Linux's `connect(INADDR_ANY)` semantics.
- **Ring geometry vs. stale state**: shrinking RMSR under staged data (or
  a crafted snapshot) underflowed the free-room math to ~64 K and let the
  RX writer stomp the neighbouring socket's ring. `clampRingState` re-fits
  the cursors on every geometry rebuild and on snapshot load; a clamped
  non-pow2 carve is rounded down to a power of two so the `& (size-1)`
  masks stay exact.
- **`romBank_` ($C028) was not snapshotted** — the highest-impact //c-class
  rewind gap: restoring a PC captured under one firmware bank while the ROM
  reader served the other. Now in the MIG blob's optional tail together
  with `migIntDrive_`/`migHdSel_`; the IWM blob likewise gained `phases_` +
  `writeDataLoaded_`, and both loaders re-fire the phases/devsel callbacks
  so the SmartPort hub is told about the restored lines (its own state is
  live, and a transition-gated callback stays silent when values happen to
  match).
- **ImageWriter parser hardening**: a non-digit in an ESC G count went
  negative → `uint32_t` cast → ~4 G bytes of "graphics" and a deaf printer
  (`paramDigit` now clamps); ESC '/ESC I left the previous command's
  parameter count armed and ate up to 6 characters; ESC r (reverse feed)
  produced *negative* pacing costs (credit grew past the cap and dumped
  the queue in one frame) and walked the head to negative Y.
- **"Clear all" UB in the paper tray panel**: `nDone` was captured before
  the front-panel buttons, so the follow logic indexed `completedPage()`
  into the vector the button had just emptied. Counted after the buttons
  now; completed-sheet texture identity also includes `droppedPageCount()`
  so the 32-page cap can't leave a dropped sheet's pixels under a new
  label.
- **Kiosk mode never pumped the ImageWriter** (early-return before
  `pumpImageWriter()`): a printing //c parked every byte in the card spool
  forever. The pump also tracks *which* source its drain cursor counts
  against — carrying it from an unplugged PrinterCard onto the SSC tap
  skipped or replayed part of the stream. Spool growth is bounded (SSC tap
  trims its consumed prefix using absolute offsets; the mechanism
  force-drains past a 1 MiB backlog).
- **CS8900A**: `UthernetCard::onReset` re-stamped `kDefaultMac`, reverting
  the guest-programmed IA on every Ctrl-Reset (MAME preserves it — pinned
  by `testMacSurvivesCardReset`); the RxEvent Extradata bit (0x4000) was
  computed after the clamp that made it dead code; the data-window
  read/write now feeds the `ioRegs_` cache so `peek()` stops reporting $00.
- **Not fixed on purpose**: the //c+ still runs the same IWM + DiskIICard
  dual-controller arrangement on $C0EC that f7af757 removed from the plain
  //c — it needs a repro (Print Shop save on //c+) before touching the
  routing, because the //c+ genuinely needs the IWM for MIG/3.5". Filed in
  TODO.md [Storage] with the mechanism spelled out. HT/VT jumping to the
  *farthest* tab stop is byte-identical to the reference implementation —
  owned as parity, not silently "fixed".

Pinned by new cases in `uthernet2_w5100_smoke` (half-close, RMSR shrink,
CONNECT gating, ≥$8000 mirror writes), `uthernet_cs8900_smoke` (MAC across
reset), `imagewriter_smoke` (parser hardening ×3) and `iwm_mig_snapshot`
(romBank round-trip + old-blob compatibility).

## 2026-07-28 — The //c prints for real: on-board IWM was fighting the Disk II

Printing on a //c through the slot-1 SSC to the host ImageWriter looked wired
up and produced blank paper. The serial path was never the problem — it is
byte-exact (600+ chars via `PR#1`, zero loss) and Print Shop's short SETUP
test print rendered correctly the whole time. What failed was **disk**.

POM2 mirrors `$C0E0-$C0EF` into the on-board IWM on //c-class profiles that
have an alt firmware bank. MAME wires its IWM as *the* slot-6 controller,
replacing the Disk II. **POM2 does not** — `iwmAuthoritative` leaves the
slot-6 `DiskIICard` answering for 5.25". So on a plain //c the mirror was a
*second* controller on the same soft switches, supplying no data path but
still running its own phase/motor handling. Two controllers stepping one
drive drifts the head, and DOS 3.3 RWTS then falls into endless seek/retry
storms — `$B948-$B956` with the head oscillating between the target track
and 0. Print Shop could neither save its setup nor load its print overlay,
so it returned to its menu without even rasterising, and nothing reached the
printer.

Gating the mirror on `isPlus_` fixes it: the IWM is consulted only where it
actually owns a drive, the //c+ MIG / Sony 3.5" path. With that, the //c
prints a full Print Shop greeting card — **104 097 bytes, 105 `ESC G`
graphics bands, 2 sheets**, border plus tiled artwork — matching the //e
byte-for-byte in structure.

The lesson worth keeping: POM2 and MAME differ on *who owns slot 6* on a
//c, so MAME's wiring cannot be copied verbatim here. Pinned by
`iic_diskii_no_iwm_conflict`, which asserts the plain //c never routes
`$C0E0-$C0EF` to the IWM (and never even ticks it) while the //c+ still
does.

Two red herrings ruled out along the way, both reverted: the SSC pins
`SR_TDRE` high (real 6551 pacing at the programmed 9600 baud changes
nothing), and asserting DCD for the printer tap actively **hangs** the //c
serial firmware in its modem path.

## 2026-07-28 — Ethernet: Uthernet I + II, and the last big functional gap closes

POM2 can now talk to the modern internet from an Apple II, which was the last
item on the backlog with no implementation at all.

**Two cards, two completely different animals.** This is the thing to
internalise before touching either file:

- **Uthernet I** (`uthernet`) is a *NIC*. Its CS8900A moves raw Ethernet
  frames and nothing else; the TCP/IP lives on the Apple side (IP65, Contiki,
  ADTPro-ethernet). It is useless without a host transport that speaks
  Ethernet.
- **Uthernet II** (`uthernet2`) is a *TCP/IP offload engine*. The guest writes
  an address and a port into W5100 registers, issues `CONNECT`, and pushes
  payload at a ring buffer — the chip does the protocol work. That maps
  one-for-one onto host BSD sockets, so **the Uthernet II needs no Ethernet
  backend at all** for TCP and UDP. Period IRC, telnet and FTP clients work on
  a stock build with no libslirp and no privileges. Only its MACRAW/IPRAW
  modes need a transport.

That asymmetry is why the host side is optional. `NetworkBackend` has three
implementations: `Null` (always present, drops everything, keeps the cards
pluggable and software-detectable), `Loopback` (transmit feeds receive — drives
both smoke tests so CI never touches a network), and `Slirp` (libslirp
user-mode NAT, gated on `POM2_HAVE_SLIRP`). libslirp rather than TAP or pcap
because both of those need root; slirp terminates the guest's IP in-process and
re-opens ordinary user-space sockets. Virtual network is QEMU's: guest
10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.

**The CS8900A is a verbatim MAME port** (`machine/cs8900a.cpp`, itself a VICE
port) with line citations throughout, plus the ~40-line card shim from
`bus/a2bus/uthernet.cpp`. Three deliberate deltas: MAME is *pushed* frames by
`device_network_interface`, which POM2 has no equivalent of, so `pumpBackend()`
pulls on the cycle hook applying the same `shouldAccept()` pre-filter (bounded
per call so a busy link can't stall the CPU thread); the `assert()`-heavy
PacketPage macros became clamped accessors, because a mis-decoded `$C0nX` must
never take the emulator down; and `peek()` replaces
`machine().side_effects_disabled()` for the debug panel.

The subtle part of that chip is that **transmit is a four-step handshake** —
TxCMD, TxLength, *read* BusST and observe `Rdy4TxNOW`, then push bytes — and
skipping any step must emit nothing. Easy to "simplify" into a bug; pinned.
Likewise reading RxEvent before draining a staged frame is an "implied skip"
that discards it, which is real hardware, not a defect.

**The W5100 had no MAME device to port**, so the reference is AppleWin's
`source/Uthernet2.cpp` cross-checked against the WIZnet datasheet. One
substantive improvement over the reference: AppleWin resolves virtual-DNS names
with a blocking `getaddrinfo()`. Under POM2's `stateMutex` that would stall
emulation for however long the resolver takes. Ours runs the lookup on a
detached thread with a 120 ms bounded wait, and a late answer lands in a
mutex-guarded mailbox that `poll()` folds into the cache on the CPU thread — so
the guest's retry succeeds instantly and the audio never glitches.

Snapshot rules worth not rediscovering: the CS8900A's inbound frame queue and
the W5100's live sockets are **deliberately excluded**. Both mirror host
network state that has moved on by the time a rewind replays. A restored TCP
socket that claimed to still be `ESTABLISHED` would hang the guest waiting on a
peer that is gone, so it comes back `CLOSED`; the raw modes carry no host state
and do return. The 4 KB PacketPage and 32 KB W5100 memory *are* saved, which
sounds expensive for a 60 Hz ring until you remember `RewindBuffer` XOR-deltas
them — an idle NIC costs a handful of bytes per frame.

`LISTEN` is decoded but unimplemented, and says so rather than pretending:
neither transport can route an inbound connection to the guest.

Pinned by `uthernet_cs8900_smoke` (MAME-parity register, handshake and filter
behaviour over a loopback backend) and `uthernet2_w5100_smoke`, which runs a
**real TCP session** — OPEN, CONNECT, SEND, RECV, CLOSE against a listener the
test opens itself, deliberately with no backend plugged, so the "TCP needs no
transport" claim is a test and not a comment.

## 2026-07-28 — The //c prints: SSC printer tap, multi-page PDF, Grappler+ pinned

Three follow-ups that close out the printing chantier.

**The //c's printer port feeds the ImageWriter.** On a real //c the printer
port is the *serial* port — there is no parallel card to plug — so a stock //c
profile could render paper only by lying about its hardware. The SSC now has a
printer tap: every byte the ACIA accepts for transmit is mirrored into a
host-visible spool with the same `drainSpoolFrom` shape as the parallel cards,
and `pumpImageWriter()` treats it as a third source (parallel cards outrank
it, so a IIe with both keeps its routing). Slot 1 taps by default — `PR#1 :
PRINT` prints with zero configuration.

The pitfall worth remembering: enabling that path exposed that POM2's
synthetic SSC ROM never initialised the ACIA outside Pascal's PINIT. A 6551
with DTR de-asserted parks its transmitter at MARK (MAME `mos6551.cpp:
317-321`), so every `PR#n : PRINT` byte was silently dropped — the TCP telnet
bridge had the same latent bug, masked whenever a host program or test wrote
the command register first. The PR#n/IN#n entries now program cmd=$0B before
hooking CSW/KSW, exactly what the real SSC firmware's DIP-switch init does.

**Multi-page PDF export** (`ImageWriterPdf.{h,cpp}`). The reference emits
PostScript; a bare `.ps` is a dead end on modern hosts, and the cost
difference vanished once the page raster existed. Each sheet embeds as an
8-bit `/Indexed /DeviceRGB` image — the ImageWriter raster already *is* one
byte per pixel with a recoverable palette — FlateDecoded through the stb zlib
compressor that was already in-repo for PNG. Zero new dependencies. Completed
sheets now carry their own `dpi` (`Page::dpi`), so a sheet ejected at 144 dpi
keeps its true `/MediaBox` even if the user then flips the printer to 288.
Pinned by `imagewriter_pdf` including a byte-exact xref audit and a Flate
round-trip through stb_image's inflater; `pdfinfo` validates the output.

**Grappler+ pinned against MAME `bus/a2bus/grappler.cpp`** with line ranges
cited at every ported block. One silent divergence found and fixed: POM2
cleared the ROM bank on reset, but the U2D bank flip-flop is not wired to bus
RESET (`reset_from_bus:536-539` touches only the ACK latch) — a reset
mid-graphics-dump must leave the high $C800 bank selected until the next
$CnXX fetch. Also added: `$CnXX` *writes* (bus conflicts on real hardware)
drop the bank like reads do (`write_cnxx:586-591`). Documented-deliberate
divergences: the 7-clock /STROBE pulse collapses to instant (nothing observes
it), the edge-driven IRQ flip-flop is derived as its equivalent level, and
`ackEffective()`'s BUSY gate is POM2's host back-pressure model, which MAME —
wired to a live centronics /ACK — does not need.

## 2026-07-28 — ROM Status panel: what you have, what's missing, what it costs

POM2 ships no ROMs, so the most common failure mode by far is a dump that is
absent, mis-named or the wrong variant — and every symptom that produces shows
up a long way from the cause. A profile silently boots the wrong firmware; a
card refuses to plug with a one-line warning in a log nobody reads; a Grappler+
prints fine but AppleWorks doesn't recognise it. The information existed, spread
across eight probe sites and a log file.

Help → ROM Status puts the whole picture in one window: every ROM POM2 probes,
in probe order, resolved against the live ResourcePaths search roots, with size,
CRC32, and — on hover — the full candidate list and *what breaks* if nothing
resolves. Machine firmware and character generators are read straight from
`profileConfig()`, so a new profile appears there with no edit; the peripheral
side lives in `RomCatalog.h`, mirroring each card's probe list at its plug site.

Three judgements, kept deliberately separate:

- **Missing** is an error (red) for machine firmware, a warning for everything
  else — most card ROMs degrade rather than fail, and the row says how.
- **Size** is the only hard check. A Disk II PROM is 256 bytes and a Grappler+
  EPROM is 4 KB, full stop; a mismatch is a wrong file, not a variant.
- **CRC32** is shown for identification always, but only *judged* where POM2 has
  a documented reference dump to compare against (the two CFFA 2.0 images).
  Asserting a checksum POM2 cannot vouch for would turn a legitimate variant
  into a false alarm.

A `(fallback)` mark flags the case that used to be invisible: the //e
Unenhanced profile resolving to `apple2e.rom`, i.e. running 1983 hardware on
Enhanced firmware because the dedicated dump is absent.

## 2026-07-28 — Slot Configuration and the media bays are two windows

Yesterday's pass added banners to both columns of Slot Configuration so the
window would stop hiding that its halves run opposite interaction models: the
left is staged (edit, then Apply — which restarts the machine — or Revert), the
right is immediate (Mount / Insert / Eject act at once). Narrating the split was
the wrong fix. Apply and Revert sit at the bottom of the assignment column and
still LOOK like window-level buttons, so "mount a disk, then Revert" reads as
undoable no matter what the banner says.

They are now separate windows: **Slot Configuration** (Machine →) is only the
per-slot card list plus Apply / Revert, and **Internal Disks & Media**
(Devices → Storage) is only the internal drives and the mountable bays of the
plugged storage cards. Each header points at the other, so neither is a dead
end. Side effects worth having: the assignment list gets the full window width
instead of 52 % of it, and the media column stops collapsing into a ~100 px
sliver when the panel is docked into a side dock — the responsive
two-column/stacked dance that existed only to survive that squeeze is gone.

Persisted as `show_media_panel`; command palette `panel.media`; both windows
are cleared in kiosk mode, and the Emulation dock preset docks both.

## 2026-07-28 — Disk II write-back: two bugs that made writing impossible

Symptom that opened it: The Print Shop hangs forever on "PRESS RETURN TO SAVE
SETUP INFO ON PRINT SHOP DISK", and every module that runs afterwards then
reads the FACTORY printer config off the disk (slot 1 / EPSON APL) and spins
in its handshake loop against whatever card is really in slot 1. Nothing about
that is a printer problem — the setup save never lands, so nothing downstream
can work. Underneath were two independent defects.

**1. The write-back opt-in never reached a running machine.** `plugDiskII`
never applied `disk_writeback[_slotN]` to the card it had just built — its
`plugHdv` / `plugCffa` siblings, twenty lines below, always did — and
`applyProfile`'s media snapshot carried the mounted PATH per slot but not the
toggle (the CFFA snapshot right beside it carries `{path, writeBack}`). Since
`applyProfile` re-plugs on every profile switch INCLUDING the one the
constructor runs at startup, whatever the MainWindow ctor restored was thrown
away moments later. `isWriteProtected()` is `fileWriteProtected || !writeBack`,
so the guest simply saw a write-protected disk: DOS 3.3 answered WRITE
PROTECTED, and Print Shop retried forever. Both sites now restore it, and
`applyProfile` carries the LIVE toggle so a mid-session change made from the
Disk II panel survives the rebuild.

**2. The flux→nibble re-pack corrupted the track it wrote.** With the first bug
fixed, DOS got as far as writing and answered I/O ERROR — and the disk was
unreadable from then on, CATALOG included. A nibble store has no angular
length, so `writeFlux` has to turn the flux the head lays down back into
nibbles. It did that by walking the PADDED cell timeline of the track that was
already there (8 cells per nibble, +2 for each $FF inside a sync run) and
overwriting the nibbles the window covered. That is only correct while the new
content pads exactly like the old one, and a sector write never does — DOS
writes its own sync run (a 40-cycle loop = a 10-cell $FF) wherever it likes, so
from the first sync byte on, the old grid and the new stream disagree and every
following nibble is assembled from its neighbours' cells. Rewriting a track
with its own contents was idempotent, which is why it survived so long; writing
an actual data field mangled 345 of its 353 nibbles and spilled into the next
sector's address field.

The head has no grid. It writes a continuous bit stream and the reader
self-syncs on it, so POM2 now FRAMES the incoming cells the same way: skip
0-cells until a 1, then that 1 plus the next seven cells are the nibble (the
two 0-cells trailing a sync $FF are skipped, which is exactly what makes them
sync), and the nibbles are laid down sequentially from the slot the head is
over. The shift accumulator lives in `DiskImage::writeFraming[track]` because
DiskIICard flushes every ~30 transitions and a nibble straddles chunks
constantly.

**And the cell grid comes from the WRITE clock, not the revolution anchor.**
The head emits one cell every `lssCyclesPerCell()` LSS cycles from the moment
write mode came on, so a burst's transitions are exact multiples of that apart.
Quantising them against the revolution phase — `(t - revolutionStart) mod
period / cyc`, which is right for READS — put the grid at an arbitrary sub-cell
offset: adjacent transitions rounded into the same cell, one of the two was
dropped, the nibble lost a bit and the framing slipped. The anchor is still
what says WHERE the burst starts; it is now consulted once, to pick the nibble
the head is over, not per transition. A mid-nibble splice leaves that nibble's
old value alone (a real write splice leaves exactly that stub) and frames into
the following slot.

Verified end to end, not just in the unit: DOS 3.3 `SAVE`, then `CATALOG`,
`LOAD` and `LIST` return the program; the host `.dsk` gets 38 bytes on tracks
10 and 17 (the file plus the VTOC/catalog). The Print Shop setup save now
completes and writes its 3 config bytes at track 11 — `01 01 01` → `07 04 03`,
slot 7 / Apple ImageWriter / Grappler+ — and a greeting card prints from a
disk that configured itself.

Pinned: `disk_writeflux_framing`. Note that `disk_write_controller_smoke`
could not have caught this: it never calls `loadLssRom`, so it exercises the
legacy 32-cycle nibble gate, while the shipped app bundles `roms/diskii_p6.rom`
and therefore always runs the LSS/flux path.

Diagnostics added: `POM2_TRACE_WRITEFLUX=1` logs each splice window (cells,
anchor nibble, framing state), and `POM2_TRACE_PC_MAX` raises the instruction
cap on `POM2_TRACE_PC` for a window that sits past a boot + menu walk.

## 2026-07-28 — ImageWriter II: the paper is continuous fanfold

The panel drew the printable raster alone, so POM2's paper read as a cut A4
sheet out of an inkjet. An ImageWriter II is fed 9.5" fanfold: the printable
body plus a 0.5" pin-feed strip each side, each strip perforated off along a
line of sprocket holes on 1/2" centres, and each sheet joined to the next by a
horizontal perforation. The strips and perforations are drawn around the page
texture, not into it, so the page bitmap and "Save sheet as PNG" stay pure
printable area.

## 2026-07-27 — UI pass 5: Slot Config stops hiding which changes are staged

The panel has always run on two different interaction models and never said so.
Left column: edit the slot combos, then Apply (which restarts the emulator) or
Revert — staged. Right column: Mount / Insert / Eject — immediate. Apply and
Revert sat at the bottom of the left child, so they read as governing the whole
window: mount a disk on the right, hit Revert on the left, and expecting the
mount to come back is a perfectly reasonable reading.

Both columns now announce their model. Changed rows get an accent dot whose
tooltip names the card actually plugged; a badge counts "N staged change(s) —
not applied yet"; Apply is disabled when nothing is staged, because a button
that restarts the machine should never be a no-op someone hits by reflex.

Two layout defects fixed while in there:

**Slot numbers trailed their own control.** `LabelText` / `BeginCombo` put the
label on the right, so the panel read "(empty) v  Slot 1" — the number, which
is exactly what the eye scans down the column for, came last. Labels now lead,
with the gutter measured off the widest one so it survives the UI zoom.

**The assignment column was a hardcoded 400 px.** Fine at the 880 px
free-floating default; once docking landed and the panel went into a side dock,
the media column got a ~100 px sliver with every label clipped to
"Mount / Inser". The columns now go side-by-side only above 46 em and stack
below it.

Also renamed `pom2::statusLed` (added in pass 1 for the status-bar drive light)
to `pom2::indicatorDot`. `StatusLed.h` already owned a `pom2::statusLed` for
*media* status with its own colour table and tooltips. Overload resolution
happened to pick correctly at every call site — exact match beats a bool→ImU32
conversion — but two same-named functions in one namespace meaning different
things is a trap set for whoever writes the next call.

## 2026-07-27 — UI pass 4: command palette, and the Disk Library becomes a browser

**Command palette (Ctrl+Shift+P).** 42 menu items across 8 menus, ~33 panels,
four keyboard shortcuts. Type "mock", "amber", "eject", "pal" and hit Enter.
New `CommandPalette_ImGui`; dispatch is one switch in `MainWindow::runCommand`.

Shift is load-bearing in that chord: plain Ctrl-P must keep reaching the guest
because CP/M under the SoftCard uses it for printer echo. The palette also joins
`isGlobalKey` so it opens from a focused text field — same reasoning as F11/F12.

Unavailable commands stay listed and greyed instead of being filtered out:
seeing "Phasor (no card plugged)" teaches where the thing lives.

**Disk Library.** A flat list of ~950 rows with full relative paths became a
nested folder tree with pinned Favourites and Recent sections. Two bugs on the
way, both the kind that look fine until you read the output carefully:

*A flat lexicographic sort does not group directories.* `demo/PLASMAG.dsk` sorts
before `demo/digidream/DD.dsk` which sorts before `demo/zzz.dsk` — so walking
the list and opening a node whenever the directory prefix changes emitted
`demo` **twice**, two `TreeNodeEx` calls with one ID, colliding in ImGui's
storage and sharing a single open/closed state. That was visible as folders
that wouldn't stay open. Now built as a real nested structure.

*ImGui applies tree indentation to the first column only.* The favourite star
had its own narrow column at index 0, which ate the whole indent and left every
filename flush left at any depth — a tree with no hierarchy to read. Name moved
to column 0; star and mounted dot became inline prefixes.

The sort selector (Name / Size / Date) is gone: the latter two forced a flat
list, because you cannot group by folder and order by size at the same time, so
they were quietly fighting the tree. Size / Date columns are now hideable, which
is what makes the panel usable in a narrow dock.

Favourites and recents live in `MainWindow`, not the panel — it has no Settings
access and no business acquiring one. Both pack into one `state.cfg` value
joined by **0x1F**: the file is flat `key=value` and a disk path can contain
spaces, commas, semicolons and colons, so the separator must be a byte a path
cannot hold. Recents track the panel's mount *requests* rather than the cards,
so a CLI or drag-and-drop mount doesn't reorder the list behind the user.

The favourite toggle sits in the right-click menu rather than being a clickable
star: the row is already a full-span selectable, and an overlapping hit target
inside it mis-fires — on a panel whose left-click cold-boots the machine, that
is not a cosmetic concern.

**`tools/dedupe_library.py`.** A duplicate on disk is a duplicate in the
browser, and this library had 20 groups / 21 redundant files / 4 MB of them:
`dsk/` ↔ `gist/` copies (sometimes renamed — `CRIME_A.dsk` is
`Le Crime du Parking A.dsk`), verbose archive names beside short hand-written
ones (`Congo Bongo (1983)(Sega)[48K].woz` = `Congo Bongo.woz`), flat files
duplicating their own per-game subfolder, and a `Copy (1)` + `Copy (2)` +
original triple. Groups by size first and hashes only within same-size buckets.
Dry-run by default; keeps the shortest path, and never keeps a `Copy (N)`.

## 2026-07-27 — UI pass 3: CRT Settings stops asking for 13 numbers

The panel opened on 13 bare numeric knobs with no starting points and one
"Reset to defaults". Almost nobody wants to dial a luminance gain; they want to
pick a look. There is now a preset row — **Clean / Composite TV / Trinitron /
Arcade** — and the sliders moved behind a collapsed `Advanced`.

Presets deliberately **preserve `palMode` and `textSharp`**. PAL composite
describes the machine being emulated (the two PAL profiles), and sharp text is
a legibility preference; a look picker that silently flipped either would be
wrong. Only the glass is a "look".

**The real bug was the messaging.** A green "CRT Effects: ON" banner sat
directly above a red "Shader unavailable — POM2 falls back to the standard NTSC
LUT". Both were true and they read as a flat contradiction, leaving no way to
tell whether the controls below did anything. They describe *different passes*:
only the OpenEmulator demodulation shader was missing, and the CRT glass stack
is a separate pass that still runs. The warning now scopes itself explicitly.

Layout fix worth recording: ImGui's `SliderFloat` puts its label on the
**right**, so the panel read "bar → number → name" and clipped the longest label
to "Phosphor curve (ga…". Labels now lead, via `SameLine(labelW)` +
`SetNextItemWidth(-FLT_MIN)`, with `labelW` measured from the widest label so it
survives the UI zoom rather than being hardcoded. Values dropped to two
decimals — `0.055` on a perceptual knob was false precision.

## 2026-07-27 — UI pass 2: docking, so 33 panels stop fighting over the screen

POM2 had ~33 free-floating panels. Opening two meant one covered the other and
usually the Apple II screen too; positions lived in `imgui.ini` as absolute
pixels, so they also went stale the moment the UI zoom changed (that gap was
called out in pass 1). There is now a **DockSpace over the viewport work area**
with a curated default layout and four task presets (View ▸ Layout).

**This changed a vendored dependency.** Docking is not on Dear ImGui `master` —
no `IMGUI_HAS_DOCK`, no `ImGuiConfigFlags_DockingEnable`. `imgui/` moved to the
`docking` branch, and because that branch is **force-pushed on every upstream
rebase**, it is pinned to a commit. The pin lives in one place,
`imgui_pin.env`, sourced by `setup_imgui.sh` *and* both CI jobs — all three
previously did an unpinned `git clone --depth 1` of master, so a fresh clone or
a CI run would have failed to compile the moment docking landed. Multi-viewport
is deliberately left off: separate OS windows for panels means per-viewport GL
contexts and a different render loop, for no benefit here.

**The chrome now reserves its own space instead of assuming offsets.** Menu
bar, toolbar and status bar are all `BeginViewportSideBar` windows, each adding
to the viewport work-area inset, and the dockspace covers what's left. The
toolbar had to be converted from a hand-positioned `SetNextWindowPos(WorkPos)`
window to get this — which also fixes the pass-1 defect where a 150 % zoom made
the toolbar taller than the saved `Apple II Screen` position and the screen
window drew over it. Both bars got `NoDocking`; without it a dragged panel can
be dropped into the one-line status strip.

Three non-obvious things worth recording:

**Seeding is gated on a persisted flag, not on "is the node empty".** By the
time we could inspect it, `DockSpaceOverViewport` has already created the node,
so emptiness can't distinguish a fresh install from a user who undocked
everything on purpose — and rebuilding each launch would silently discard their
layout. Hence `ui_dock_seeded` in `state.cfg`.

**Docking a *hidden* panel is the point, not a no-op.** The assignment is
written into the window's settings, so opening the Memory viewer later makes it
a tab in the bottom-right group instead of a window floating over the game.
That is most of the value of seeding a layout.

**The screen window's manual title-bar drag had to be disabled while docked.**
`Apple II Screen` carries `NoMove` (click-drag inside the screen must reach the
guest's Mouse Card) plus a hand-rolled title-bar drag. Docked, it has no title
bar and the dock node owns its position: the computed rect lands on the node's
tab bar and `SetWindowPos` fights the node every frame — the screen jitters and
the tab won't drag out. Guarded with `ImGui::IsWindowDocked()`.

Limitation, by construction: presets place windows by **literal title**, so the
slot-numbered panels (Disk II, 3.5", HDV, SmartPort, Printer) build their title
at runtime and can't be reached. They float on first open and stay where the
user docks them.

Migration note: moving to docking necessarily replaces any previously saved
free-floating layout — the default is seeded once on the first docking run.

## 2026-07-27 — UI pass 1: opaque theme, DPI/zoom scaling, a status bar that says something

A design audit of the running UI turned up four things worth fixing before
any layout work.

**Panels were translucent over a running game.** POM2 ran on bare
`ImGui::StyleColorsDark()`, whose `WindowBg` sits at alpha 0.94. Invisible on
a black boot screen, unreadable over HGR: the CRT Settings sliders rendered on
top of Disk Library rows. New `Pom2Theme.{h,cpp}` owns the palette and makes
every background opaque, with rounded geometry and a phosphor accent (amber
default; P31 green / cold blue / slate, `ui_accent`).

The non-obvious part was the **surface ramp ordering**. First cut had
`PopupBg` and `FrameBg` at the same value, which left the zoom slider *inside
a menu* with no visible track — just a floating grab. Popups must sit below
frames on the ramp. See DEV § Theme.

**No DPI awareness at all** — font hardcoded at 14 px, no `ScaleAllSizes`,
nothing read from the windowing system, so the whole UI was microscopic on a
HiDPI display with no way to enlarge it. Now: monitor scale × a persisted user
zoom (View ▸ Interface, 75–250 %, `ui_scale`).

Two traps here. `ScaleAllSizes()` is **cumulative**, so `applyTheme()` rebuilds
the style from a pristine `ImGuiStyle` on every call — otherwise each nudge of
the zoom slider compounds the padding. And the DPI factor must come from
`ImGui_ImplGlfw_GetContentScaleForWindow()`, **not** `glfwGetWindowContentScale()`:
on macOS, Wayland, Emscripten and Android the framebuffer already carries the
scale, the backend helper returns 1.0f there, and querying GLFW ourselves
would have scaled those platforms twice. Caught before shipping by reading the
backend rather than by testing — the dev machine is 1× X11, where both agree.

**The status bar was three fields in `TextDisabled` grey.** It now carries the
drive LED + mounted image + track, the *achieved* clock, and a host caps-lock
badge — the three questions that previously required opening a panel. The
achieved clock is sampled from `Memory::getCycleCounter()` over ≥250 ms and is
resync-guarded: rewind, snapshot restore and profile switch all roll the
counter backwards, and an unsigned delta there would print a garbage MHz.

Its warn threshold is deliberately 10 %, not 5 %: a vsynced 60 Hz host running
a 50 Hz PAL profile lands ~4–5 % short of nominal from frame-pacing jitter
alone, and a 5 % band made a perfectly healthy machine flicker green/amber.

**Toolbar** — power-cycle (the only destructive control) is the only red
glyph; run/pause tracks the action in green/amber. The `"|"` text characters
between groups became real drawn rules (`pom2::verticalRule`), and the
hardcoded combo widths (86/90/110 px) became self-measuring — the new
`FramePadding` alone clipped "//e PAL" to "//e PA", and any zoom would have
done it again.

Not addressed, and the next real constraint: 33 free-floating panels with
absolute `imgui.ini` positions. Changing zoom mid-session doesn't move panels
placed at the old scale. Docking is the fix.

## 2026-07-27 — The IWM froze after a rewind on //c-class; MIG RAM was lost

`IWMDevice` was never serialized. It holds **eight absolute emuCycles
stamps** — `now_`, `lastSync_`, `nextStateChange_`, `syncUpdate_`,
`asyncUpdate_`, `revStart35_`, `fluxWriteStart_`, `delayDeadline_` — so a
rewind rolled the machine's `cycleCounter` backwards while the controller
kept its older, *larger* `lastSync_`. `sync()`'s `while (nextSync >
lastSync_)` walker then had nothing to do, and the IWM sat frozen until
emulated time climbed back to where it had been before the rewind.

Reachable on every //c-class profile, not just the 3.5" path: `ioReadIWM`
ticks the IWM on each `$C0E0-$C0EF` access **before** testing
`iwmAuthoritative_`, so even a //c+ booting 5.25" in shadow mode — where
the data itself comes from `DiskIICard` — advances it. The visible damage
was bounded because the shadow path supplies the bytes, which is why this
never showed up as a boot failure.

The //c+ **MIG** gate array had the same gap: its 2 KB `migRam_` and the
auto-incrementing `migPage_` pointer came back zeroed, so the alt firmware
read something other than what it had written.

Both now ride in a second, length-prefixed trailer on the `Memory` blob,
each section self-identifying by magic (`IWM1`, `MIG1`). Length prefixes
so a loader can skip a section it does not understand; a blob may carry
neither, either, or both. Older snapshots simply lack the trailer and keep
the live values — exactly the pre-fix behaviour, so nothing regresses on an
existing save. `MemoryProfile` grew a pair of no-op virtuals for this;
only the //c-class profile overrides them. The MIG page pointer is masked
to `0x7FF` on the way in rather than trusted, since `migRead` indexes
`migRam_[migPage_ + (offset & 0x1F)]`.

Pinned by `iwm_mig_snapshot`, checked against the unfixed code: with the
trailer read stubbed out, `testMemoryTrailerCarriesIwm` fails on the
restored device still sitting at cycle 0. The round-trip assertion
compares a **re-serialized** blob rather than the one public accessor, so
the private stamps are actually covered.

## 2026-07-27 — Rewind timeline read 4× long on //c+; five test harnesses revived

The rewind panel divided cycle spans by a hardcoded `1022727.0`. That is
the NTSC nominal, and the //c Plus carries a 4× Zip-style accelerator —
68180 cycles per 60 Hz frame, ~4.09 MHz. A 30-second ring displayed as
**"120.0 s"**, contradicting by a factor of four the "history (s)" slider
sitting immediately beside it, and the scrub readout was wrong by the same
factor. The conversion now asks the profile: `cyclesPerFrame × refreshHz`
is what the worker actually spends per wall-clock second, accelerator
included. The frames↔seconds conversions had the matching bug in the other
direction — a hardcoded `/60` and `*60` made the slider read 20 % short on
the 50 Hz PAL profiles.

Separately, `tests/{cpu_smoke,disasm_smoke,iic_dump,rom_basic,rom_boot}.cpp`
had been unbuildable since the sources moved into `src/`: each carried a
hand-written `g++ -I. tests/foo.cpp M6502.cpp Memory.cpp` line that no
longer resolved, and `Memory.cpp` has since grown a dozen dependencies.
They are now declared in `tests/CMakeLists.txt` as `EXCLUDE_FROM_ALL`
targets, so CMake supplies the dependency list and they cannot rot
silently again — but they stay outside ctest, because they print and
assert nothing. Their headers say so, and name the test that does gate the
same ground (`klaus_6502_functional` for `cpu_smoke`,
`system_profile_smoke` for `rom_boot`, and so on). Zero cost to normal
builds and CI.

## 2026-07-27 — Printer trace was silently truncated; Grappler DIP raced the CPU

Four fixes from a review pass over the printer work, none of them
user-visible until the moment they bite.

**The trace log always ended short.** `ImageWriter` opened the trace file
but never closed it: `stopTrace()` was reachable only from the panel's
checkbox, and the class had no destructor. Bytes already `fprintf`'d
survived — the C runtime flushes stdio at exit — but the hex row still
being assembled in `traceRow_` had never reached stdio at all, so up to
15 bytes vanished, and the file ended with no `# trace closed` footer.
That is worst exactly where the trace matters most: `POM2_TRACE_PRINTER=1`
is the path you use to capture a stream for a bug report, and it *always*
ended truncated, with nothing to distinguish a complete trace from one cut
short by a crash. The printer now has a destructor that closes the trace,
and — since it owns a `FILE*` — copy construction and assignment are
deleted rather than left to `fclose` the same handle twice. Pinned by
`testTraceClosedOnDestruction`, which was checked against the unfixed
code: without the destructor it fails on the missing row.

**The Grappler's printer-type DIP crossed threads unguarded.** The
ImageWriter panel lets you change the S1 switches while the guest runs, so
`setPrinterType` fires on the UI thread during ImGui rendering; the CPU
worker reads `dipType_` in `deviceSelectRead`. The reader holds
`stateMutex`, but the writer never did, so that mutex bought nothing. In
practice the worst case on any real target is one status poll seeing the
old switch position — harmless — but it is a data race, and `busy_` three
lines below was already `std::atomic` with a comment spelling out the very
same UI→CPU crossing. `dipType_` is now atomic too, and both members say
which thread touches them (`dipMsb_` stays plain: it is written at plug
time, before the card reaches the bus).

**The status-byte comment described the old behaviour.** It still claimed
`DIP = 000 = Epson series` while the code beside it returned the S1
switches, defaulting to `101` = Apple Dot Matrix — the whole point of the
change that introduced it. A misleading comment in a MAME-parity block is
worse than none: it is what someone debugging a DIP problem reads first.

**The stall watchdog stayed armed between jobs.** `stalledFor_` was reset
whenever the queue made progress, but not when the queue was emptied by
`flushPending()` ("Print now"), by `resetPrinterHard()`, or by draining to
zero. A job that had stalled left the counter loaded, so the *next* job's
first expensive byte could trip the watchdog immediately and be forced
through ahead of its schedule. One byte, a few milliseconds early, once —
never observable, but the reset now happens on all three paths.

## 2026-07-26 — The printer no longer freezes the Apple II by default

Printing still looked like a crash. Not a wedge this time — the *real
handshake* doing its job: a Print Shop page is tens of KB of dot columns,
the printer eats them at 250 cps, and the guest sits in its firmware ACK
loop for the whole job. Measured on the captured streams: 5.2 s for the
5 KB test page, 11.7 s for an 8.7 KB screen dump, and a full greeting card
is 10-20x that — minutes of an emulator that answers nothing.

That is exactly what a real Apple II did, and it is still available:
*Printer settings → "Make the Apple II wait for the printer"*. But it is
**off by default** now. The page still builds up line by line at the
printer's real speed — which was the point — while the guest carries on.
Realism that is indistinguishable from a hang needs to be asked for, not
inflicted.

A print in progress is also shown in the status bar (`printing 4312 B`,
plus `(Apple II waiting)` when the handshake is on), so it is never a
mystery pause with no explanation on screen.

## 2026-07-26 — Ribbon cartridge modelled; read-only disks say why

Two "why doesn't it…" answers turned into settings:

* **Ribbon**: `Four-colour` (default) / `Black` in *Printer settings*.
  There is no colour "mode" on an ImageWriter II — colour is the ribbon
  you install, and software asks for a band with `ESC K`. With the black
  cartridge fitted the printer still accepts `ESC K` and prints black,
  like the real one. Worth knowing: the guest has to ask, so Print Shop
  only produces colour when its Setup names "Apple Imagewriter II **(C)**".
* **Write-back**: the Disk II panel now states, on the mounted image,
  *why* it is read-only — "the image itself is write-protected (WOZ/2IMG
  flag)" vs "write-back is off". The default stays off (running a program
  must never silently rewrite a source image, and the drive reporting
  write-protect beats accepting writes and dropping them on eject), but
  nothing connected that default to the guest's "disk is write-protected"
  message — Print Shop refusing to save its own Setup read as an emulator
  bug for exactly that reason.

## 2026-07-26 — Print Shop prints in colour: line feed after CR is detected

Print Shop's test page came out as a coloured staircase — "Welcome to The
Print Shop" with each colour pass one line lower than the last. The trace
(and the raw stream, both new this round) showed why in one glance:

```
ESC T16 CR LF          ← advance one line
ESC K1 CR  ESC G0396…  ← yellow pass
ESC K3 CR  ESC G0442…  ← cyan pass    — bare CR: SAME line
ESC K2 CR  ESC G0326…  ← magenta pass — bare CR: SAME line
```

The colour passes are separated by a **bare CR** precisely so they
overprint. POM2 defaulted SW A-8 (line feed after CR) to ON — right for a
bare `PR#n : PRINT`, wrong for every real driver (which sends CR+LF and
then double-spaces), and destructive for a colour driver.

There is no static default that satisfies all three, so `AutoFeed::Auto`
now settles it from the stream: feed on CR until the guest sends its own
LF right after one, then swallow that LF and stop feeding. A plain BASIC
listing, a Grappler+ printout and Print Shop's colour page all come out
right with nothing to configure. The switch can still be pinned On/Off.

Worth keeping: the printer was never the bug in any of this. What made it
findable was making the *byte stream* visible — the trace log said `ESC K`
+ bare CR, and at that point the answer was in the manual.

## 2026-07-26 — The printer could wedge the Apple II; printer trace log

Print Shop froze the moment it printed. Not a crash — a deadlock the
pacing work had just introduced: `tick()` capped its banked mechanism
time at 1 s, but a form feed costs `(bottomMargin - curY) / 5 ips` = up
to 2.2 s of paper transport on a Letter sheet. That byte could never be
afforded, so the queue stalled *forever*; BUSY stayed asserted; and the
guest, which now correctly waits on the Grappler's ACK bit, spun in its
firmware loop with nothing to wait for. Any page eject did it.

Two fixes, because one of them should have made the other impossible:

1. The credit cap is now `max(kMaxCredit, cost of the head byte)`.
2. A watchdog forces any byte that has waited 10 s through regardless,
   and says so in the trace. A cost-model mistake must degrade to
   "printed late" — never to a hung guest. Wiring a device that can
   block the CPU means the host side needs a floor, not just a model.

Also this round:

* **Trace log**, since "it prints nothing" and "it prints noise" are both
  protocol questions. *Printer settings → Log the printer stream to a
  file* (or `POM2_TRACE_PRINTER=1`) writes the byte stream as a hex dump
  interleaved with the decoded escape sequences, bit-image setup, page
  ejects, BUSY transitions and queue depth.
* **"Follow" now tracks the last inked sheet.** After a form feed the
  panel was showing the fresh blank sheet under the head while the
  printed one sat on the stack one click to the left — which is why a
  job that printed correctly still looked like it had printed nothing.

Field notes from driving the real *New Print Shop* (WOZ) headless, worth
keeping: its Setup carries **Printer = Apple Imagewriter II (C)** but
**Interface Card = Built-in** — the //c/IIgs on-board port, which sends
nothing at all to a card in a slot. And its program disk is
write-protected, so a corrected setup can't be saved (hence its own
"current Setup was done on a different computer" warning at every boot).
Neither is an emulation bug; both look exactly like "the printer doesn't
work".

## 2026-07-26 — Grappler+ was talking Epson to an ImageWriter

Printouts came out as pages of meaningless characters in double-width,
with the ribbon colour flipping — while the panel showed a moving head
and a healthy byte count. The printer parser was not the problem: it was
audited case-by-case against the original (`david-schmidt/gsport`
`src/imagewriter.cpp`, 84/84 commands present, same framing, same
parameter counts). The card was.

The Grappler+'s S1 DIP block tells its firmware which printer is on the
cable, and POM2 reported MAME's default: **Epson**. Captured from the real
4 KB dump, the same `^I G` screen dump emits:

```
S1=000 Epson      ESC A <07>  …  ESC K <18><01> + binary graphics
S1=001 C. Itoh    ESC T14     …  ESC S0280      + graphics
S1=101 Apple DMP  ESC T14     …  ESC G0280      + graphics
```

An ImageWriter II parses the Epson stream as "1/6 in spacing", "select
ribbon colour $18", then prints every graphics byte as a glyph — 32 sheets
of noise, exactly what the panel was showing. POM2's printer *is* an
ImageWriter, so S1 now defaults to Apple Dot Matrix (101) instead of
Epson, is settable as *Card emulates* in the ImageWriter panel (persisted,
with a warning when it is set to a dialect this printer can't read), and
the S1:1 MSB switch masks bit 7 at the latch like MAME's `data_latched`.
With that, an HGR `^I G` dump renders as the picture.

MAME is right to default to Epson — it wires the card to a generic
centronics printer. The default is only wrong once you know what is on
the other end of the cable, which POM2 does.

## 2026-07-26 — ImageWriter prints at printer speed; Grappler+ ACK handshake

The printer worked but printed *instantly*: the card spools a page in a
millisecond of emulated time and the whole sheet appeared in one frame.
`ImageWriter::queueBytes()` + `tick(dt)` now model the mechanism — bytes
wait in the printer's input buffer and land as the head reaches them, at
Apple's published rates (250 cps draft / 45 cps NLQ, *ImageWriter II
Owner's Manual*). Speed is a *Printer settings* combo (`Instant` keeps
the old behaviour); the status line shows the queue and a "Print now"
escape hatch. Pacing runs off the host frame time, not `emuCycles`: the
paper keeps moving while the guest is paused, turbo'd or rewound.

Three things worth keeping:

1. **The firmware waits on ACK, not BUSY.** Holding BUSY (bit 3) high did
   nothing — the guest printed straight through it. The genuine Grappler+
   dump spins on bit 0 instead (`$CD89 JSR $CDE1 / AND #$01 / BEQ $CD89`),
   so a full 2 KB printer buffer has to read back as *not acknowledged*
   (`GrapplerCard::ackEffective()`). With that wired, a long print job
   blocks the Apple II in its firmware loop while the paper catches up —
   which is what "printing" felt like in 1985.
2. **Draft is bidirectional.** Charging every `CR` a full carriage-return
   slew halved the throughput (3.4 lines/s instead of ~6). Draft prints on
   the return sweep; only NLQ pays the slew.
3. **A printer card in slot 3 of a //e is broken by design.** The internal
   80-column firmware keeps `OURCH`/`OURCV` in the slot-3 screen holes,
   which is where printer firmware keeps its column/line counters — so the
   Grappler reads the cursor position back as its line width and emits
   `CR LF` after *every character*. Reproduced against the real ROM dump
   on `apple2e.rom` (slots 1/2/4/5/7 are clean, and II+ slot 3 is clean).
   Faithful, not a bug: Slot Config now warns and points at slot 1.

Also fixed: `grapplerCard` / `echoPlusTmsCard` were the only non-owning
card pointers the two slot-teardown paths forgot to null, so unplugging a
Grappler+ or switching profiles left `pumpImageWriter()` dereferencing a
freed card every frame.

## 2026-07-26 — Apple ImageWriter II printer + paper-tray window

POM2 could capture printer output but only as a text spool: `PR#1` gave
you bytes, not a page. Anything that printed *graphics* — screen dumps,
Print Shop, the Grappler's `^I G` dump — spooled a pile of escape codes
and looked like garbage. `ImageWriter` (`ImageWriter.h/.cpp`) interprets
those bytes and paints the page; `ImageWriter_ImGui` shows it, with page
navigation, zoom and PNG export (*Devices → ImageWriter II (printout)*).

**Ported from greg-kennedy/ImageWriter** (`imagewriter.cpp`, the GSport /
KEGS / DOSBox lineage), which was written against Apple's ImageWriter II
and LQ reference manuals. Command dispatch, soft switches, density tables
and the ribbon encoding are line-for-line, with the reference's line
ranges cited in the code. Full detail → DEV § ImageWriter.

Non-obvious decisions worth keeping:

1. **The printer is not a card.** It has no catalog key and no slot. The
   Apple II talks to a *printer interface card*; `pumpImageWriter()`
   streams that card's spool into the printer once per frame via a new
   `drainSpoolFrom(consumed, out)` on `PrinterCard` / `GrapplerCard`.
   Modelling it as a slot card would have made "Grappler+ *and* an
   ImageWriter" unrepresentable, which is the normal 1985 desk.
2. **Auto line-feed defaults ON.** The Apple II's `COUT` emits a bare CR
   (`$8D`) and never an LF. With the ImageWriter's SW A-8 open — the
   reference's default — every printout overprints a single line into an
   unreadable smear. This was caught end-to-end (real `apple2p.rom`,
   `PR#1`, real spool), not by unit test; the first rendered page was one
   black stripe. Exposed as a checkbox because CR+LF drivers need it off.
3. **No FreeType, no SDL.** The reference needs both; POM2 links neither.
   Glyphs come from the repo's own 8×8 CP437 font
   (`hgrpaint::kBBFontCp437`), which is *closer* to the hardware — an
   ImageWriter draft cell really is 8 dots wide at the pitch's density and
   8 pins tall at 1/72 in, so text and graphics share one dot plotter.
4. **Dots are the page-pixel interval they cover**, replacing the
   reference's `pixsize` heuristic and its "Primative scaling function"
   fudge (`imagewriter.cpp:1556-1573`) — that produced seams and randomly
   doubled columns at page DPIs that aren't integer multiples of the
   graphics density. Adjacent dots now abut at any DPI.
5. **Ribbon colour is subtractive by construction.** The page is indexed
   `yyyxxxxx` (5-bit intensity + 3-bit band) with bands assigned so that
   OR-ing inks mixes them: magenta|yellow = red, cyan|yellow = green, all
   three = black. Overprinting is a plain `|=`, no blend maths.
6. **The completed-sheet stack is capped** at 32 with older sheets rolled
   off and counted. A guest that form-feeds in a loop would otherwise eat
   ~2 MB of host RAM per sheet forever.

Two deliberate bug-fixes against the reference: `resetPrinter()` leaves
bold off (the reference sets `STYLE_BOLD` to fatten a thin TrueType face —
on a dot-matrix cell it just smears), and parameter space-normalisation
touches only digit positions, so `ESC R nnn ' '` repeats a space instead
of printing zeros.

Not modelled: user-defined character sets (absent from the reference too)
and `ESC ?` "send ID string" (no printer→computer back-channel). An
ImageWriter on the Super Serial Card — the //c's real printer port — needs
a host-visible TX spool on the SSC first; only the parallel cards feed the
printer today.

Pinned: `imagewriter_smoke` (paper geometry, bit-7 strip, CR/LF + spacing,
`ESC K` overprint + palette, `ESC G`/`ESC C` bit images, `ESC R` framing,
form feed + page cap, RGBA export, and the spool→printer streaming seam
including resync after "Clear spool").

## 2026-07-12 — SoftCard/Z80 bug hunt: 6 confirmed, 6 fixed, 1 refuted-as-faithful

Adversarial review (8 finder angles + per-candidate verification) over the
day's Z80/SoftCard/DMA work. Fixed same-day:

1. **WASM link break** — `runCpuSlice` was defined inside the
   `#ifndef __EMSCRIPTEN__` worker block while the unguarded `tickFrame`
   (the browser-RAF CPU driver) calls it. Moved above the guard.
2. **Z80 `IN r,(C)` MEMPTR** — WZ was computed *after* the register
   write, so `IN B,(C)`/`IN C,(C)` latched WZ from the modified BC. Port
   address captured first now. zex can't catch this class (no I/O).
3. **Unbounded DD/FD chains** — a whole prefix run used to fold into ONE
   `Z80::step()` (a crashed guest in a $DD/$FD sea → one giant
   advanceCycles lump under stateMutex; wrapped 64 K of prefixes → host
   hang). Each prefix now retires as its own 4-T step with interrupts
   deferred to the opcode (`State::pendingPrefix`, faithful to silicon);
   SoftCard blob bumped to `SFZ2`. `Z80::run()` (dead, misleading
   contract) deleted.
4. **Snapshot-load during CP/M** — file snapshots carry no SLOTn
   sections, so restoring left a live SoftCard `enabled_` and the stale
   Z80 executed over the restored RAM. `restoreMachineState` now
   force-disarms DMA claimants first (rewind, which captures slots,
   already round-tripped correctly).
5. **Step verbs during CP/M** — debugger/CLI/AI single-step always
   stepped the parked 6502, running its post-hand-back continuation
   early (a DMA-halted CPU executes nothing). New
   `EmulationController::stepBusMaster` steps the claimant's Z80
   (`dmaRun(1)` = one instruction) instead.
6. **POM2_TRACE_HANG false positive** — the detector samples the 6502 PC
   per frame; under Z80 ownership that PC is legitimately frozen →
   guaranteed "HANG DETECTED" spam on healthy CP/M. Sampling now skips
   (and resets the ring) while a DMA claimant owns the bus.

Refuted with evidence, kept as-designed (documented in DEV § SoftCard):
Z80 writes to $C007 via the $E000 window wedging a IIe until RESET is
what real MMU silicon does (UTAIIe 5-28) — MAME's write-through there is
its own simplification. Cleanup backlog (ByteIO for the SFZ2 blob,
findResource/textRowAddress reuse in the boot test, rp-selector/ccTest
dedup in the Z80 decoder, xlate LUT) → TODO [Arch].

## 2026-07-12 — CP/M 2.2 boots to A> (SoftCard Phase 3 — plan complete)

Microsoft CP/M 2.2 now boots end-to-end: Disk II loads the system tracks,
the 6502 loader finds the SoftCard by its $CnXX write probe, the Z80 runs
CCP/BDOS/BIOS out of the six translated windows, and a live `A>` prompt
lands on the text page in ~11 M cycles. Two media-gated ctest gates:
`softcard_cpm_boot` (II+, 44K v2.20 1980 master, 40-col) and
`softcard_cpm_boot_iie` (//e, 60K v2.23, 80-col) — the latter verified
against the MAME `apple2ee -sl4 softcard` oracle with a byte-identical
banner.

The bring-up lesson: every "failure" was a **sysgen/machine mismatch,
not an emulation bug**. The 56K/60K sysgens require the IIe-class
console — on a II+ they paint $00s (MAME does the same), and their
output goes through the IIe 80-col firmware, which stores even display
columns in AUX $0400: a main-RAM screen scrape shows every second
character missing ("Sfcr PM" for "Softcard CP/M") and reads like a Z80
bug until the aux page is interleaved in. The 44K 2.20 master is the
correct II+ image. Don't re-diagnose this; check the sysgen first.

## 2026-07-12 — Microsoft SoftCard card + dual-CPU DMA arbitration (CP/M Phase 2)

`SoftCardZ80` (catalog `softcard`) ports MAME `a2bus/a2softcard.cpp`. Two
findings corrected the Phase-1 plan the moment the MAME source was read —
worth remembering because both were "obvious" wrong guesses:

- The bus toggle is a **write to $CnXX** (the slot-ROM window), not a
  DEVSEL $C0nX access, and reads never toggle.
- The Z80→6502 translation is **six windows, not `+$1000` with wrap**:
  the Z80's $B000-$DFFF lands on the Language Card ($D000-$FFFF), $E000
  reaches the I/O page (that's how the Z80 releases the bus itself), and
  $F000 wraps to the zero page. CP/M's 60K layout only exists because of
  the LC remap.

Arbitration is a generic DMA daisy-chain hook (`SlotPeripheral::
dmaActive/dmaRun`, `SlotBus::dmaClaimant`, `EmulationController::
runCpuSlice`) rather than SoftCard special-casing — MAME's a2bus has the
same abstraction, and a future Applicard reuses it as-is. Hand-over is
instruction-precise both ways (the granting STA calls `M6502::stop()`,
whose `run()` re-arms on the next call — the same yield WAI/STP uses; the
chunk remainder goes to the other CPU). The Z80's 2× clock is converted
2 T-states → 1 cycle with an odd-T carry so `emuCycles` stays in the 6502
domain — video/LSS/audio never learn a second CPU exists. Pinned by
`softcard_toggle` incl. a full tickFrame 6502→Z80→6502 round trip.

## 2026-07-12 — Z80 core lands (SoftCard/CP/M Phase 1), zexdoc+zexall 100 %

First deliverable of the Microsoft SoftCard + CP/M plan: a standalone
`pom2::Z80` core (`src/Z80.h/.cpp`), bus-abstracted behind `Z80Bus` so it
links with zero Apple II sources — the SlotPeripheral card and the
dual-CPU arbitration come in Phase 2 (see TODO [Cards]). Full opcode
coverage including the undocumented surface (IXH/IXL, DD CB write-back,
SLL, X/Y flags, MEMPTR/WZ), IM 0/1/2, NMI, EI shadow, documented T-state
totals. Pinned by `z80_core` (committed smoke) + `z80_zexdoc`/`z80_zexall`
(configure-time downloads, SHA-256 pinned, Klaus pattern) — both
exercisers pass 100 %.

The pitfall worth remembering: **zexdoc green ≠ core correct**. zexdoc
masks the undocumented X/Y flags, and the one bug the first full run
surfaced was the `BIT n,r` X/Y rule — X/Y copy bits 3/5 of the *full
tested register*, not of the masked single-bit result (and for
`BIT n,(HL)` they come from WZ's high byte, which is why the core carries
a real MEMPTR). Only zexall's silicon-captured CRCs catch this class;
any future Z80 touch-up must keep both exercisers in the gate.

## 2026-07-12 — OE composite bug hunt: GPU/CPU demod knob parity

Bug hunt on the OpenEmulator composite pipeline, GPU shader vs CPU demod.
The demod *math* checked out OE-exact (kernels recomputed from libemulation's
`chebyshevWindow × lanczosWindow` realIDFT recipe to ≤5e-6; Y'UV matrix and
sin→U / cos→V / PAL-flips-V conventions match `OpenGLCanvas.cpp` line for
line) — every real bug was in *which path applies which knob*:

- **Hue / Sharpness / PAL / Sharp-text were GPU-only.**
  `renderCompositeOeCpu` ignored all four `NtscParams` demod knobs, yet
  MainWindow still *neutralised* hue+sharpness in the CrtEffectStack pass on
  the OE-CPU branch ("the demod already applied them" — only true for the
  GPU). Net effect: the sliders were silently dead in OE-CPU mode, and —
  worse — **popped off on OE-GPU mixed frames**, whose graphics band is CPU-
  demodulated (`mixedCompositeUsesFramebuffer`): entering a splitscreen
  (game score band, BASIC) visibly dropped the user's hue/PAL. Fixed by
  mirroring the live knobs into the display each frame
  (`Apple2Display::setOeDemodParams`) and implementing hue rotation, the
  soft↔sharp chroma-kernel blend and PAL V-sign alternation in the CPU
  demod — same formulas as the GLSL, pinned pixel-identical (maxDelta 0) by
  the extended `oe_demod_gpu_cpu_parity` (now also covers
  hue+sharpness+PAL engaged, and demodulated TEXT).
- **AI `/screen` lied in OE-GPU mode.** `pixels()` returns the LUT fallback
  framebuffer there (the composite image only exists in a GL texture), so
  agent screenshots showed ColorNTSC colours, not what's on screen. New
  `Apple2Display::demodCompositeForCapture()` schedules the pixel-identical
  CPU demod after the server's render; no-op in every other mode.
- **One-frame present race.** `drawScreenImage` re-polled
  `Memory::getDisplayState()` (the CPU worker may have advanced past the
  rendered frame) to route sharp-text/demod presentation; a text↔graphics
  switch in that window flashed one LUT frame. It now reads
  `Apple2Display::lastRenderState()` — the snapshot render() actually used.
- **Golden rebaseline (15 hashes).** `display_golden_hash` was red at HEAD:
  the intentional mono lo-res dot-pattern rendering (previous entry) landed
  without re-recording the `lores/dlgr × mono*` hashes, which still pinned
  the pre-fix "mono lo-res ≡ colour hash" behaviour. Re-recorded; diff
  audited to be exactly those 15 entries.
- **Parity contract documented.** The demod now exists in three deliberate
  copies (GLSL, `renderCompositeOeCpu`, the test's re-simulation); each site
  now carries a cross-pin comment naming the other two, since an edit to one
  alone is invisible to CI until the parity test is also updated.

## 2026-07-12 — paint-editor crumbs: mono lo-res, composite canvas, DHGR sprites

The three leftovers from the 17-item batch. 133 tests (pins folded in).

- **Mono lo-res rendering** (`renderLoRes` / `renderLoResDouble`). On a mono
  monitor a lo-res nibble is NOT a grey: the colour generator keeps cycling
  at 14.318 MHz, so a block displays as its repeating 4-bit dot pattern —
  the same serialisation the composite-signal path already used
  (`fillCompositeSignal`, absolute-sample indexed). GR's 280-wide pixels
  average their two samples (grey 5 → uniform 127); DLGR renders per-dot at
  560 (grey 5 → fine stripes), aux rotation included. Standard
  max(target, prev × decay) phosphor rule; `Phosphor`/`phosphorFor` moved
  above the lo-res painters. Pinned in `dhgr_paint_model`.
- **Composite canvas pipelines.** "AppleWin NTSC (composite)" and "OE
  composite (CPU)" join the paint editor's pipeline combo (scratch display
  in AppleWin *Monitor* sub-mode — Tv's frame blend would smear a static
  canvas). This is what makes the DHGR NTSC-8-px import previewable
  faithfully: its 86 colours only exist after composite demodulation, and
  the import preview now points at the pipeline combo instead of
  apologising. The 560-wide composite output of the 280 modes reuses the
  existing pair-averaging width adapter.
- **Sprite editor DHGR target** (gated on `supportsDhgr()`). The editing
  canvas stays the mono shape; a "DHGR target" toggle re-aims Stamp / Grab /
  colour preview / ca65 export at the DHGR page: lit pixels plot as 140-px
  colour pixels in a picked 16-colour hue (transparent background,
  plane-aware pokes), Grab lifts shape + dominant hue back, and the ASM
  export emits `name_aux` / `name_main` byte-pair tables (blit at an
  aligned byte-column pair). Placement is pixel-granular (`Px X`) — DHGR
  pixels are nibble-aligned, so there is no ×2-style parity honesty problem.

## 2026-07-12 — paint-editor batch: 17 items (tools, DLGR, NTSC 8-px, sprites)

The full improvement backlog proposed after the 560-dot import landed in one
wave. 133 tests (DLGR + NTSC-8 pins folded into the two existing paint tests).
Highlights and the non-obvious bits:

- **GR/DLGR screen holes masked.** Bulk editor ops (clear/import/load) used
  to write the text-page screen holes ($x78-$x7F per 128-byte group) on the
  LIVE machine — peripheral firmware (SmartPort, mouse) scratches there, so
  a Clear Page could corrupt a running driver. Text-page loads now go
  byte-wise through pokes with holes skipped (both planes in DLGR).
- **HGR import now scores LUT row 0.** Closes yesterday's ~22 % divergence:
  the importer's decode is pinned byte-identical to `renderHiRes` ColorNTSC
  in `dhgr_convert`. POM1 parity for this file is deliberately dropped
  (comment documents it).
- **DLGR mode.** 80×48 blocks over the aux+main text pages; the aux nibble
  displays ROTATED LEFT one bit (MAME renderLoResDouble), so the model
  stores rotr4(colour) in aux nibbles — pinned against the real renderer +
  a lo-res palette cross-pin in `dhgr_paint_model`. Editor paints it in a
  560-dot logical space (7 dots per block), pxScreenW()=0.5.
- **DHGR NTSC 8-px import** (`imageToDhgrPage560Ntsc`): scores against the
  86-colour trailing-8-dot palette from ii-pix (`DhgrNtsc8Palette.cpp`,
  BSD-2-Clause data, table anchors pinned). The model is CAUSAL — no right
  context, no guessed neighbour, no refinement pass needed. The extra
  colours only appear on composite viewing targets (OE/AppleWin modes);
  the import preview warns that the MAME-LUT canvas under-sells it.
- **Save to ProDOS**: `buildVolumeFromFolder` now parses CiderPress-style
  `NAME#TTAAAA` filename tags into file_type/aux_type (pinned in
  `prodos_volume_smoke`), and the paint editor's file browser homes to
  `prodos_folder/` — a default "PIC#062000" save is BLOAD-able by name at
  $2000 after the next host-folder mount.
- **Tools**: 16-colour copy/paste (mode-tagged clip + FlipH/FlipV/Rot90),
  MacPaint 8×8 fill patterns (page-anchored so strokes tile seamlessly;
  patterned fills over the same colour are allowed), X/Y mirror symmetry
  (applyPlot level — region ops deliberately exempt), DHGR text, onion-skin
  tracing overlay (fit/crop-aware placement), page 1↔2 flipbook + ghost
  (double-buffer animation authoring), canvas pipeline selector
  (NTSC/Medium/4-bit/Chat Mauve — ChatMauve's 560-wide HGR output is
  pair-averaged down to the 280 canvas), 4:3 aspect, DHGR fringing overlay,
  session persistence, and the POM1 sprite editor (`hgrsprite/`) wired to
  the same host seam.

## 2026-07-12 — DHGR import upgraded to the true 560-dot model

Follow-up to the paint-editor port after comparing against ii-pix and
bmp2dhr: the 140-px block quantiser is bmp2dhr's model, which ii-pix
dropped as "only useful to show why this is not the right approach to
DHGR". 133 tests (one new).

- **`imageToDhgrPage560`** — ii-pix's "4-pixel colour" model with the HGR
  converter's proven architecture: per-byte-column (7 dots, 128
  candidates) branch-and-bound analysis-by-synthesis, warm-started per
  row, in-candidate linear-RGB error walk scored in CAM16-UCS via the
  local Jacobian, then monotone cross-column ICM refinement with dirty
  tracking. Exploits the full 560-dot resolution and pilots the colour
  fringing the block model suffers blindly. ~180 ms/photo (vs 4 ms for
  140-px) — live-slider friendly. Default; the block model stays as the
  "140 px blocks (Dazzle Draw)" combo choice (instant, retouch-friendly).
- **Optimisation target = canvas, bit-exact.** The candidate renderer is a
  copy of POM2's ColorNTSC DHGR decode (Apple2VideoDecode LUT row 0 +
  rotl4b(absX+1)), pinned byte-identical to `renderDhgr` in the new
  `dhgr_convert` test — something ii-pix can't do for a specific emulator
  (it scores against colour-picker-measured palettes). Also pinned: exact
  solid fields with dither off, tone conservation dithered (mean linear
  RGB within 0.06 — full-strength diffusion turns the unavoidable
  black-context edge artifact into a never-decaying ripple, an inherent
  error-diffusion property, so exactness there would be a wrong pin), and
  refinement monotonicity.
- **LUT archaeology.** The ~22 % HGR importer/canvas divergence documented
  yesterday has a root cause: POM1's GraphicsCard NTSC LUT is MAME's
  medium-color **row 1**; POM2's ColorNTSC is **row 0**. HGR keeps row 1
  verbatim (POM1 parity); the DHGR converter carries row 0.
- **Resampler pixel aspect.** `resampleToLinearRgb` gained a
  `pixelAspect` parameter (default 1.0 — every existing caller unchanged);
  the 560-dot path passes 0.5 so fit/letterbox is computed in visual space
  (a square source fills the width instead of coming out 2× too narrow).

## 2026-07-12 — HGR Paint editor ported from POM1 + DHGR mode

The portable `hgrpaint/` module (MacPaint-style editor + ii-pix-style
image importer with CAM16-UCS perceptual dithering) is copied verbatim
from POM1 and driven by a new `Pom2HgrPaintHost` (Tools → HGR Paint
Editor). 132 tests (one new).

- **Offscreen canvas trick.** The host renders the editor page through a
  private, never-clocked IIe `Memory` + `Apple2Display` pair: the scratch's
  cycle counter never advances, so its video-event log never publishes and
  `render()` always takes the fast single-state path — the canvas is
  pixel-identical to the live screen with zero beam-racing interaction.
- **Pokes bypass IIe paging on purpose.** `writeRamUnchecked` (main) / raw
  aux bank (DHGR) instead of `memWrite`, so live 80STORE/RAMWRT state can't
  silently reroute the editor's writes to the wrong plane.
- **DHGR mode (POM2-only module extension).** New DHGR/DHGR2 pages (gated
  on `host->supportsDhgr()`): 140×192 16-colour aligned block model over
  the aux+main pair, 16 KB A2FC load/save, DHGR image import (CAM16-UCS +
  error diffusion at native resolution). The nibble↔colour law
  (`colour = rotl4(nibble,1)`) was derived from MAME's square-filter
  decode and is pinned by `dhgr_paint_model` against the real
  `renderDhgr` in two colour modes + a lo-res palette cross-pin — so an
  Apple2Display palette/decode change that would desync the paint model
  fails CI, not the user's picture.
- **stb gotcha.** MainWindow's `stb_image` impl is `STB_IMAGE_STATIC`
  (TU-local); the host TU compiles the only exported stb_image +
  stb_image_write implementations, which `HgrImageDecode.cpp` links
  against. Don't add a second non-static impl.

## 2026-07-12 — wave 4 (deferred fixes cleared + real Liron implemented)

The four items wave 3 deferred, plus the Liron follow-through now that
the real ROM is public. 131 tests (one new).

- **OE-CPU demod out of `stateMutex`.** The 17-tap × 560×192 FP demod
  (~1-2 ms) ran inside the lock every UI frame, stalling the CPU worker
  (read as emulation/audio jitter, worse under disk-turbo). `render()`
  now defers it (`pendingCpuDemodRows_`) and MainWindow runs
  `finishPendingCpuDemod()` after releasing the lock — it consumes only
  display-owned buffers. The mixed text band is patched under the lock
  (it reads guest RAM) and the per-row FIR only rewrites rows [0,160) in
  mixed mode, so the patch survives; `pixels()` finishes lazily so every
  render→pixels consumer (tests, screenshots) stays correct.
- **Frame-wrap video-event off-by-one.** An instruction straddling the
  17030/20280-cycle video-frame boundary stamped its soft-switch event
  past the boundary but publication ran after the instruction — the event
  closed into frame N and applied the switch one frame early. Stamps are
  non-decreasing, so boundary-crossers form the log's tail: they now
  carry into the new recording frame. Pinned by a real-6502 straddle case
  in `video_event_publish` (STA $C055 started 2 cycles before the
  boundary).
- **SmartPort STATUS pre-flights.** $CnC0 now returns SEC+$28 on no
  media and SEC+$2B on write-protect before handing back the block count
  (TRM driver conventions) — formatters that pre-flight STATUS used to
  get CLC on an empty/WP bay. Exactly 32 bytes, fills $CnC0-$CnDF.
- **Golden table: 112 → 164 pins.** New hash-frozen scenes: FLASH-on
  phase (16 emu-frames parked), text/HGR PAGE2, HGR 80STORE+PAGE2
  (asserted **equal** to the page-1 hash — the Sather 5.10 gate),
  HGR+AN3 rev-0 bit-7 mask (would have caught wave 3's paintHgr bug),
  80COL+HIRES+MIXED without DHGR (upscale path), and the Chat Mauve
  BW560/Mixed/Chunky160/Duochrome sub-modes. All 112 pre-existing hashes
  unchanged. Deliberate gap kept: mousetext/char-ROM glyphs need a user
  ROM; PAL beam-raced splits stay behavioural.
- **Real Liron controller ROM in `roms/liron.rom` + SmartPort-protocol
  dispatch.** The BMOW dump (4 KB, SHA1 fa94ecc2…, per-slot $Cn00 pages
  at slot×256 + $C800 bank at 2048) now ships in roms/; on slot-having
  machines SmartPortCard re-bases its page on the real dump — authentic
  identity $Cn07=$00/$CnFB=$00/$CnFE=$BF/$CnFF=$0A — with the HLE
  entries overlaid (the real IWM/UniDisk code can't run without the
  drive-side 65C02; never loaded on //c-class, see
  project_iic_smartport_boot). And $Cn0D is no longer fail-closed: a
  168-byte 6502 handler in the $C800 bank implements the real SmartPort
  call convention (inline cmd + param-list pointer, RA+3, ZP $42-$45
  saved) against a C++ engine — STATUS incl. unit-0 controller status
  and the 25-byte DIB, READ/WRITE (through the legacy commit machinery),
  FORMAT/CONTROL/INIT, real error codes ($01/$04/$21/$27/$28/$2B/$2D/
  $2F). Pinned by `liron_smartport_dispatch`: the full matrix executed
  by a real 6502, in both synthetic and real-ROM identity passes.

## 2026-07-12 — wave 3 (graphics system + Liron SmartPort audit)

Targeted hunts: display decode / composite pipelines / voxel & Chat Mauve
(three review agents), and the Liron-class SmartPort card audited against
primary sources (Apple Tech Notes, ProDOS 8 TRM, and the REAL Liron
firmware — see below). ASan+UBSan build of the full suite: clean (and it
exposed a real link bug: `test_ai_control_server` was missing
`MouseCardAppleWin.cpp`; Release builds optimized the dynamic_cast typeinfo
away, sanitizer builds didn't).

**Headline discovery: the Liron controller ROM is publicly dumped.**
BMOW/Yellowstone published `LIRONALL.bin` (4 KB) + full disassembly in
2018-2019; MAME's "WANTED — never dumped" listing (which CLAUDE.md echoed
as "cannot be implemented regardless of effort") is stale — MAME just
never ingested it. Verified byte-level: `$Cn07=$00`, `$CnFE=$BF`,
`$CnFF=$0A` → ProDOS entry fixed at `$Cn0A` (independently confirms the
DIX `JSR $C50A` fix), SmartPort dispatch at `$Cn0D`. CLAUDE.md corrected.

Display/composite fixes:
- **FLASH / phosphor / Tv-blur paced by the host monitor (medium).**
  `frameCounter` was ++ per render() call — the UI renders at vsync, so a
  120/144 Hz panel blinked FLASH 2-2.4× too fast, decayed MonoAmber
  afterglow 2.4× faster, and collapsed the AppleWin Tv 50 % blend; even at
  60 Hz the PAL profiles flashed at the NTSC rate. Now derived from the
  emulated frame index (`cycleCounter / 65·scanlines`, standard-aware) —
  exactly MAME's `frame_number() & 0x10`; decay is raised to the
  elapsed-emu-frames power and the Tv stash only advances with the
  machine. `hgr_render_smoke` now pins the invariant both ways (decay on
  frame step, NO decay on same-frame re-render).
- **CRT glass vanished on mixed/sharp-text/fallback frames in OE-GPU mode
  (medium-high).** The effect-stack gate only covered the pure GPU-demod
  and OE-CPU branches; a mixed graphics+text frame (score bands, BASIC)
  presents the CPU-rendered framebuffer and got NO scanlines/mask/
  persistence — effects flickered off/on during gameplay with a stale-
  persistence ghost on re-entry. Gate is now `oeFamily && presentTex ==
  screenTexture` — every OE path that still presents the raw framebuffer
  gets the glass.
- **Composite HGR dropped the IIe rev-0 DHIRES bit-7 mask (low-med).**
  `paintHgr` hardcoded `bit7Mask=0xFF` while the RGBA twin honors
  `state.dhgr ? 0x7F : 0xFF` (MAME `bit7_mask`): with AN3 on + 80COL off,
  the composite modes rendered the half-dot delay the LUT modes correctly
  suppressed — two POM2 outputs disagreed on the same frame.
- **Settings hardening.** NTSC/CRT floats + voxel tunables now clamped to
  slider ranges on load, and all 17 sliders got `AlwaysClamp` (a
  hand-edited `ntsc_center_lighting=0` was `1/x` → `exp(-inf)` → fully
  black screen in every glass mode, surviving restarts).
- **Chat Mauve rewind/snapshot hooks** ('CM' blob: FIFO/mode/AN3/80COL
  latches) — rewinding past a BW560/Mixed switch kept rendering the later
  mode until the guest re-clocked. Panel slot label unhardcoded (any
  slot 1-7 on //e). Voxel shader program no longer leaked at shutdown
  (via new `pom2::deleteShaderProgram`).

Liron/SmartPort fixes (spec citations in the audit, `$` = ProDOS codes):
- **READ/WRITE on an empty bay silently "succeeded" (medium ×2).** A read
  streamed a $FF buffer with CLC — ProDOS ONLINE saw a garbage volume and
  `PR#5` with no media booted 512 bytes of $FF and jumped into them; a
  write dropped 512 bytes. Both now latch the I/O error → carry-set.
- **`iicSmartPortArmed_` serialized (medium)** as a backwards-compatible
  MEX trailer byte — a rewind-ring entry captured after a //c HDV/3.5"
  boot restored with the $C500 stub swapped back to real //c firmware
  under a live ProDOS (next MLI call executed unrelated ROM bytes).
- **SmartPortCard transfer state serialized (medium-low)** ('SP' blob:
  unit/block/stream offset/half-filled write buffer/error latches) — a
  rewind landing mid-512-byte stream desynced the transfer.
- **//c arming scoped + fallback leak (low-med).** `bootFromSlot` armed
  the on-board SmartPort for ANY slot (a slot-6 5.25" Library boot with
  SmartPort media re-created the dual-device "garbled banner" scenario)
  and the no-signature fallback returned without disarming.
- **Capability byte honest:** `$CnFE` $13→$17 (write bit was missing —
  capability-inspecting utilities saw a read-only device). **`$Cn0D`
  fails closed** (SmartPort-convention callers used to fall through NOP
  padding INTO the boot routine). **Bad-command error** now $27 (real
  driver code) instead of the invented $01 Filer surfaced on FORMAT.
- **Host read-only images mount write-protected** (Block512Backing +
  Disk35Image probe writability at load) — write-back on a chmod-read-only
  .hdv used to accept a whole session of writes and lose them at flush.

Verified clean (highlights): text/HGR/DHGR interleave + `frameCycleToPos`
beam math both standards (13/13 display tests), GL lifecycle/resize/
GLES-WASM in both composite paths, Mat4/camera (pinned by `voxel3d_math`),
Chat Mauve FIFO edge model, SmartPort dispatch offsets against the real
ROM's conventions, `$Cn0A` DIX path. Deferred with TODO entries: OE-CPU
demod under `stateMutex` (~1-2 ms/frame of worker stall), frame-wrap
video-event off-by-one (≤7 cycles exposure), golden coverage gaps,
SmartPort STATUS pre-flight semantics.

## 2026-07-12 — wave 2 (seams: stale rewind ring, snapshot-load audio, kiosk K leak)

Adversarial re-review of wave 1's fixes + a sweep of the cross-subsystem
seams (snapshot/rewind × profiles, AI server × kiosk, CLI boot × slot
config). Wave 1's fixes all held; the seams gave up two mediums.

- **Stale rewind ring across profile switch / bootFromSlot (medium).**
  `RewindBuffer.h` documents "drop every retained frame on cold boot /
  profile switch", but the ONLY clear site was `coldBoot()`. `applyProfile`
  and `bootFromSlot` wipe RAM/aux (and applyProfile rebuilds the card set)
  without clearing — so with rewind enabled, F6 after a II+→//e switch
  restored II+ RAM/CPU/slot state onto the //e ROM (PC into II+ Applesoft →
  crash), and `truncateAfter` made that garbage the new timeline. Now
  cleared in `bootFromSlot`, `applyProfile` (after step-1 stop) and
  `restartEmulationFromSettings` (its SLOTn sections describe the card set
  being torn down).
- **Snapshot load left the speaker dead for minutes (medium).**
  `SpeakerDevice`'s reconstruction cursor only snaps FORWARD and purges
  older-stamped toggles as stale; rewind/scrub knew this
  (`flushAudioForRewind`) but `/snapshot/load` (AI server) and CLI
  `--snapshot-load` didn't — loading a snapshot with a smaller
  cycleCounter muted audio until the counter re-passed its pre-load value
  (10 min of play ≈ 10 emulated minutes of silence). Both callers now
  flush the speaker + drop the rewind ring (whose stamps would break
  `indexForCycle` monotonicity) after a successful restore.
- **3.5" boot without 3.5" hardware failed silently (low).**
  `insertAndBootImage`'s Sony35 branch fell through to
  `controller->mount35()` — the //c+-only on-board Sony hub — on ANY
  machine without a SmartPort card, then cold-booted to the BASIC prompt
  with `true` returned and "booted disk" logged. Now errors out ("no 3.5"
  device in this config") unless a SmartPort card is present or the
  profile is the //c+.
- **Kiosk K-key leak, open direction (minor, wave-1 fix was one-sided).**
  Closing the Keys band with K was fixed, but OPENING it still typed a
  live 'k' into the running game: the GLFW char callback fires while the
  menu is still closed, then the same frame's `updateKioskMenu` opens the
  non-pausing band. K (and Ctrl-K's $0B) is now reserved in kiosk mode.
- **Swallow latch scoped to gamepad-mapped pads.** Raw pads can't drive
  the menu (nav needs a mapping), so their held fire button across a
  keyboard-driven menu close is legitimate game input — no longer eaten.
- **Adversarially re-verified, no change needed:** onKey/onChar gate (F10
  via ImGui, AI keys via `pasteText` direct, Alt tracking above the gate),
  swallow drain on unplug/Emscripten, F6 re-park (worst case one worker
  tick behind the overlay), cassette clock domain split, stickToPaddles
  math (new tests fail on the old code: 143 vs ≤132, 134 vs 128). Owned
  trade-offs: ~13-count axis-snap detent at the cone boundary (no
  hysteresis until someone reports flicker), F6 inert during the Keys
  band, stick-nav edges without the history guard (can only move a cursor
  one step).
- **Release audit.** Docs drift fixed (README documents the kiosk in-game
  menu and drops "no menu to quit"; DEV.md §kiosk rewritten against
  `openKioskStartMenu`/`updateKioskMenu`/`renderKioskMenu` + §joystick
  against the stickToPaddles pipeline and GamepadPlay mapping; CLAUDE/TODO
  note speaker+cassette ARE retimed). CI now triggers on `v*` tags — tag
  builds previously ran NO CI. Decision (2026-08-02): keep the in-repo
  dumps and **bundle the full `roms/` tree in every release artifact** —
  packaging/docs now match the tree that has been public since the
  initial commit.

## 2026-07-12 (pre-release bug hunt: kiosk input leaks, PAL cassette clock)

Adversarial review of everything landed since v0.7 (kiosk menu, square-gate
joystick, PAL speaker fix). Two real input-isolation leaks in the kiosk menu,
one sibling of the PAL speaker bug, and a fistful of minors.

- **Kiosk menu keyboard fallbacks leaked into the machine (major).** The
  menu's arrows/Enter/Esc/K are polled via `ImGui::IsKeyPressed`, but the
  overlay window never sets `WantCaptureKeyboard`, so `glfw_key_callback`
  kept forwarding every key to `MainWindow::onKey`/`onChar` → the $C000
  latch. Concretely: Enter on the key band sent the selected cell **and**
  injected $0D (every send double-typed); Esc closed the menu and delivered
  a stray $1B to the game on resume; K typed a literal 'k' into the running
  title. Fix: `onKey`/`onChar` early-return while `kioskMenuOpen_` (menu
  navigation itself is unaffected — it never used the callback path).
- **Gamepad close-press leaked PB0/PB1 into the game (major).**
  `pollJoystickAndPushToMemory` runs *before* `updateKioskMenu` and gates
  suppression on `kioskMenuOpen_`, which lags a close by one frame — and
  Circle/Cross double as the menu's B/A **and** the Apple game-port
  buttons. Dismissing the menu with B therefore fired PB0 in the game for
  several frames (ditto Cross→PB1 after Restart, and a D-pad direction held
  at close fired an arrow key). Fix: latch `kioskSwallowPad_` on the
  open→closed edge and keep faces + D-pad suppressed until the pad is fully
  released; analog paddles stay live (no edge to leak). The auto-repeat
  history (`padArrowHeld_`) resets while suppressed so a held direction
  re-arms cleanly.
- **F6 rewind unpaused the machine behind the open menu (minor).** A hold
  released while the Start menu had the worker parked ended in
  `rewindEndAndResume` → `Mode::Running`, and `kioskSetPaused` early-outs
  (it still believed "paused"), so the game ran with audio under the
  overlay until reopen. Fix: F6 is inert while the menu is open **and**
  `updateKioskMenu` re-parks the worker if anything resumed it behind a
  wanted pause.
- **PAL cassette pulse audio — same bug the speaker fix cured (minor).**
  `CassetteDevice::queueAudioSegment` converted cycle durations with the
  hardcoded NTSC `kRealtimeAudioTimebaseHz`; under PAL the queue fills
  ~0.7 % slower than the callback drains (~330 samples/s short at 48 kHz)
  → periodic level dips on sustained tones. Fix mirrors the speaker:
  atomic `realtimeTimebaseHz_` + `setCpuClock()` wired from
  `setVideoStandard`. The tape-**file** timebase stays NTSC-nominal on
  purpose — it's the format's cycle definition, not playback pacing.
- **Kiosk minors.** GAMES list now rescans on the RomDirs→List transition
  (a folder added via the browser used to stay invisible until the menu was
  reopened); the mounted-disk ● marker matches canonically (a kiosk
  launched with a *relative* path, `POM2 games/foo.dsk`, never matched the
  canonicalized scan entries — cursor landed on index 0, no ●).
- **Paddle deadzone: continuous engage + axis-snap (follow-up).** Two
  behavioral gaps left by the per-axis → radial deadzone switch: (1) the
  hard cutoff stepped ~12 counts the instant the stick left the dead zone —
  now the radial deadzone **rescales** ([dz..1] → [0..1] along the ray), so
  the reading is continuous (128 → 130 across the edge, pinned); (2) radial
  lost the old per-axis suppression of cross-axis drift — 5 % Y wobble
  during a full X push read PDL(1)≈134 and crept games. Now an
  **axis-snap notch** zeroes the small axis while it's under `dz × |big|`;
  the threshold scales with the dominant component, so diagonals are never
  notched and the square-gate corner guarantee (full diagonal → 255/255)
  survives — both pinned. The whole pipeline (invert → deadzone → notch →
  gate → 0..255) is now a pure static `stickToPaddles()` that
  `paddleValue()` routes through, closing the review's "composition not
  unit-testable" gap: `joystick_square_gate_test` pins center/NaN/corners/
  rails, the deadzone edge, drift suppression, the gate-off path and invert.
- **Joystick minors.** `edge()` now requires prior-poll history — the first
  poll after a (re)bind treated an already-held button as a fresh press
  (Start held across a rebind popped the kiosk menu). Explicit
  `#include <algorithm>` (was compiling through transitive includes).
  Removed dead `activeMouseSlot` (last -Wunused-variable in the GUI build).

## 2026-07-11 (kiosk follow-ups: PAL speaker clock, HDV/3.5 in menu, DS4 in-game mapping)

Fixes + refinements on top of the same-day in-game menu.

- **PAL speaker audio fix (real bug).** The 1-bit speaker's cycle→sample
  reconstruction hard-coded the NTSC CPU clock (`SpeakerDevice::kCpuClockHz =
  POM2_CPU_CLOCK_HZ`, 1.0227 MHz) and `setVideoStandard()` never retuned it. On
  a PAL profile (CPU actually 1.0156 MHz) the audio path consumed ~0.7 % more
  cycles/sec of toggles than the CPU produced, starving the reconstructor →
  periodic snap-forward glitches on continuous speaker music. **H.E.R.O. on the
  //e-PAL profile sounded broken; the same disk under `--preset iie` (NTSC) was
  clean** — which pinned it. Fix: `kCpuClockHz` → runtime `cpuClockHz_`
  (`std::atomic<double>`) + `SpeakerDevice::setCpuClock()`, called from
  `EmulationController::setVideoStandard()` with the standard's real clock
  (`pom2VideoTiming(s).cpuClockHz`). NTSC numbers unchanged → zero regression.
  (AY/SSI263 device clocks stay NTSC-nominal by design.) Confirmed by ear.
- **Kiosk pause no longer swallows audio on resume.** While the menu parked the
  worker (`Mode::Stopped`), the audio thread kept advancing the speaker cursor
  over silence, leaving it far ahead of the frozen production; on resume the
  catch-up purge would eat the game's first sounds for ~the pause duration.
  `kioskSetPaused(false)` now `speaker().reset()`s on the paused→running edge.
- **Mounted-disk marker.** The games list prefixes the disk currently in the
  boot drive with a ● (`ICON_FA_COMPACT_DISC`) and lands the cursor on it.
- **HDV + 3.5" reachable from the menu.** The scan no longer filters to 5.25"
  only — it accepts everything `classifyDiskForSlot` recognises (5.25"/3.5"/
  HDV). 5.25" still hot-swaps in place (flip-disk); 3.5"/HDV route through
  `insertAndBootImage` (the CLI launcher's path) and boot immediately. The ROM-
  folders browser reaches `hdv/` and `disks_3.5/` (add once, persisted).
- **DS4 in-game mapping.** New `JoystickInput::GamepadPlay` (from the standard
  gamepad layout, `valid` only when gamepad-mapped): the analog stick stays the
  Apple II paddles; **Cross/Circle → PB0/PB1**; the **D-pad → Apple II arrow
  keys** (←$08 →$15 ↑$0B ↓$0A) with //e-style auto-repeat (350 ms delay →
  ~16/s); **Square → SPACE, Triangle → RETURN** (one key per press, via
  `Memory::queueKey`). Menu-gated (no injection while the overlay is up) and
  only for gamepad-mapped pads — a raw pad keeps the legacy buttons 0/1/2 →
  PB0/1/2 fallback. The button D-pad is used here, NOT the stick (the stick is
  the joystick), so `GamepadPlay` never folds stickX/Y in the way `UiNav` does.

## 2026-07-11 (kiosk in-game menu — two-zone method)

Reworked the kiosk disk picker into a full two-zone in-game menu. Still
**exclusive to `--kiosk`** — the whole thing lives behind the `if (kiosk_)`
gate.

- **Two entry points.** **START** (pad, standard GLFW mapping) / **F10** opens
  the two-zone **Start menu**; **SELECT** (Back button) / **K** opens the
  **Keyboard band** directly, even mid-game.
- **Two-zone Start menu** — the headline UX win. A **GAMES** list and an
  **ACTIONS** column (Restart / Keyboard / ROM folders / Quit) coexist:
  **◀▶ swaps focus** between the two zones, up/down moves within the focused
  zone, **A** validates it. No more scrolling past every disk to reach an
  action. The focused zone is vivid with a green ▶; the other is dimmed.
- **Machine paused** (`Mode::Stopped`) on every Start-menu page — like
  inserting a disk on a real machine at rest — but the **Keyboard band leaves
  the game running** so injected keys land live. `kioskSetPaused()` only
  resumes a worker it parked itself, so it never fights F6-rewind's mode moves.
- **Keyboard band** = a 2D grid of Apple II keys; **A** sends one via
  `Memory::queueKey`. The Apple keyboard is a **latch+strobe**, so a one-shot
  queue with no key-up bookkeeping is correct.
- **Temporal auto-repeat** (400 ms delay → 150 ms cadence, clock-based off
  `ImGui::GetTime()`) for held directions, plus **L1/R1** fast page jump (±10)
  in the games list — needed because the paused menu loop runs unthrottled and
  would otherwise scroll unaimably fast.
- **Proximity SORT, not filter.** The old build *hid* every non-sibling disk;
  now all mountable 5.25" images are shown, with the mounted title's other
  sides sorted to the top (selection anchored on the mounted disk).
- **ROM-folders manager + gamepad directory browser** (`◀▶`/shortcuts to `/`,
  Home, removable mounts). Extra scan folders persist in a sibling
  **`kiosk_romdirs.txt`** — deliberately *outside* `state.cfg` so the kiosk's
  read-only main config is never written (keeps POM2's strict kiosk contract).
- Plumbing: `JoystickInput::UiNav` gained `left/right/select/pageUp/pageDown`
  edges + raw `*Held` levels (the latter feed the temporal repeat).

## 2026-07-10 (kiosk gamepad UX + Apple II square-gate joystick)

Kiosk mode made lean-back / controller-friendly, plus a faithful joystick fix.

- **Square-gate joystick** (`JoystickInput::applySquareGate`, default on, key
  `joystick_square_gate`). Modern analog sticks ride a *round* gate, so a full
  diagonal only reaches ~217/217 and the extreme corners are physically
  unreachable — but the original Apple II stick rode a *square* gate where full
  X **and** full Y at once (255/255) were reachable, which **Wings of Fury's
  take-off requires**. `paddleValue()` now processes the X/Y pair together
  (radial deadzone, not per-axis — a per-axis one notched the diagonals) and
  expands the inscribed circle to the full square (`s = mag / max(|x|,|y|)`),
  leaving pure-axis directions untouched. Toggle in the Joystick panel; pinned
  by the new `joystick_square_gate` test (129 → 130 ctests).
- **Kiosk gamepad disk selector.** In `--kiosk`, the pad's **Start** (standard
  GLFW gamepad mapping) — or **F10** when the pad has no SDL mapping — opens an
  on-screen picker of the 5.25" images sitting **next to** the booted disk,
  filtered by **name proximity** (longest common prefix) so only the same
  title's other sides/disks appear (Wings of Fury Side A ↔ Side B), not the
  whole 700-disk folder. D-pad/stick move, **A** mounts in-place (no reboot,
  the flip-disk gesture), and trailing **Reset** (reboot on the mounted disk)
  and **Quit** action rows finish the job — all without a keyboard.
- Pitfalls captured: the overlay first rendered *behind* the opaque
  full-viewport kiosk window (fixed by dropping `NoBringToFrontOnFocus` +
  `SetNextWindowFocus`), and the file list stayed tiny under
  `SetWindowFontScale` because a `BeginChild` is a separate ImGui window with
  its own scale (re-applied inside). The Start button silently did nothing on an
  unmapped pad — hence the F10 fallback + a one-shot `gamepad-mapped=yes/no`
  diagnostic log.

## 2026-07-09 (v0.7 — packaging, CI & desktop integration)

First tagged release. Focus on shipping, not the core emulator.

- **GitHub Actions CI** (`.github/workflows/ci.yml`): a `linux` job builds the
  full tree and runs the ~130-test ctest suite (Klaus 6502/65C02, Tom Harte,
  cpu_cycle_count, golden-hash display, boot traces) as the non-regression gate,
  plus a `wasm` Emscripten verification build. The suite was dormant — nothing
  ran it automatically before.
- **Fixed a latent packaging blocker**: `packaging/roms_README.txt` was
  referenced by `install(FILES …)` but never existed, so *every*
  `cmake --install` / `cpack` / `build_dist.sh` staging aborted. Created it (the
  ROM drop-here note). Linux install now succeeds end-to-end.
- **Desktop integration**: an application icon (`packaging/POM2.svg` +
  rasterised hicolor PNGs), a MIME type `application/x-apple2-disk`
  (`.dsk/.do/.po/.nib/.woz/.d13/.hdv/.2mg`) so a double-clicked disk opens in
  POM2, and Debian maintainer scripts that refresh the mime/desktop/icon caches.
- Metadata fixes: real homepage URL in the package (`github.com/habib256/POM2`)
  and a `.desktop` keyword typo.
- **Grappler+**: bundled the 4 KB Orange Micro Grappler+ EPROM dump so the card
  exposes its full firmware (graphics-dump entry points + ROM fingerprint)
  instead of the fallback stub.

## 2026-06-16 (Tom Harte 65x02 ProcessorTests — cycle-exact validation)

Added the Tom Harte [`SingleStepTests/65x02`](https://github.com/SingleStepTests/65x02)
suite — 10,000 random vectors per opcode pinning the full state (registers +
memory) **and** the cycle count. Data-driven harness `tomharte_cpu_test
<nmos|cmos> <dir>` (in-house JSON scanner, no vendored lib), curated +
SHA-pinned ctest gate (`tomharte_6502` / `tomharte_65c02`), download behind
`-DPOM2_FETCH_TOMHARTE=ON` (full corpus ~1.4 GB/CPU → `tests/fetch_tomharte.sh`
for exhaustive runs of all 256 opcodes).

Results: **NMOS 6502 = 100%** on 41 opcodes (410,000 vectors, decimal
included); **WDC 65C02 = 100%** everywhere except decimal SBC on **invalid**
BCD digits.

The suite flushed out **4 genuine decimal bugs** in `M6502::ADC/SBC`, all
**provably identical for valid BCD** (only invalid digits — never produced by
correct software — change), hence zero regression risk (`cpu_cycle_count_test`
+ Klaus stay green):

- **ADC low nibble**: `tmp+6` overflowed onto bit 5 when the low-nibble sum
  reached `$1A-$1F` (invalid BCD digit) → `accumulator & 0xF0` injected `$20`
  instead of the single `$10` carry, inflating the result by `$10`.
  The silicon re-packs: `((tmp+6)&0x0F)+0x10`.
- **ADC decimal carry**: tested `tmp & 0x100`, but the `+$60` high-nibble
  correction can push an invalid-BCD sum up to bit 9 (`$240`, bit 8 = 0) →
  lost carry. Fix `tmp >= 0x100` (identical for valid BCD ≤ `$190`).
  Pitfall: an `int tmp` made it look like `& 0x100` sufficed — it's the carry
  rising to bit 9 that betrays it, visible only via instrumentation.
- **ADC V (CMOS)**: was forced to the binary overflow; the WDC uses the
  "high-nibble-sum" V already computed at line ~421. Removed the overwrite.
- **SBC low nibble**: `tmp-6` left bit 4 (the borrow the high nibble reads via
  `accumulator & 0x10`) un-re-packed → lost borrow, result +`$10`.
  Fix `((tmp-6)&0x0F)-0x10`.

Documented pitfall (not fixed): the WDC 65C02 decimal SBC on **invalid** BCD
follows a silicon correction *distinct* from the NMOS (officially undefined,
data-dependent ±1 errors) that we don't model — `e9` diverges at ~3.4% on
`wdc65c02/v1`, the NMOS being exact (`6502/v1/e9` = 100%). Opcode excluded from
the CMOS gate, tracked in `tests/tomharte_wdc65c02.manifest`.

Architecture note: instruction-stepped core + non-virtual `Memory::memRead/Write`
→ we validate the final state + the cycle count (not the per-cycle bus order),
which covers exactly the class of timing bugs (cf. the historical RMW
under-count of `cpu_cycle_count_test`).

## 2026-06-12 (wave 4: remaining peripherals, UI, snapshot — MAME oracle)

Four read-only hunters over the never-audited areas (Grappler / LLE+hand
mouse, UI layer, //c-IWM-Sony stack, snapshot/rewind/cassette), then verified
fixes + pins. Suite: 127/127.

- **Grappler+: inverted register decode.** The real data port is
  `!(offset & 3)` ($C0n0/4/8/C) — POM2 spooled offset 1, which is the
  **ROM bank select** on the real card: the authentic 4 KB firmware printed
  into the void, and its status poll read $FF = "busy + out of paper"
  (the worst possible value). Rewritten per MAME grappler.cpp:
  IRQ|DIP|BUSY|PE|SELECT|ACK status, $C800 banks (A0 set / read $CnXX
  reset + A6 ACK-detection trick), A1/A2 IRQ on the bus. The ROM stub also
  wrote via offset 1 — hence green tests that pinned the stub, never the real
  ROM path (pitfall: the test validated the implementation against itself).
- **Sony 3.5": register table aligned with MAME `mac_floppy_device`.**
  Address bit 3 = HEAD-SELECT line (ssW), not the IWM drive-select;
  MotorOff is strobe 0x6 (0x3 = EjectOff, no-op — the old "boot-tuned" table
  put motor-off there: a conformant firmware killed the motor thinking it was
  cancelling an eject); the disk-change latch lives in sense 0x3 and clears
  via the DskchgClear STROBE (0xC), not on read; DIRTN polarity fixed; a
  write-protect sense (0x9) finally exists — a protected 3.5" image was
  invisible to the firmware (writes silently lost). IWM motor-off delay:
  8388608 ticks of the 7 MHz clock ≈ 1.17 s (was 1 s CPU, with a false
  comment about the IWM clock).
- **UI: Floppy Emu panel's `insertDisk` without `stateMutex`** while the
  worker streams nibbles — potential corruption/UAF on click; all neighboring
  paths locked. Slot Config state reads moved to snapshot-under-lock;
  Disk II `motorOn` made atomic (read by the UI-side auto-turbo); per-cell
  PushID in the memory viewer (hundreds of "00" cells shared the same
  ImGui ID).
- **Snapshot: cards that gained state didn't participate.**
  Grappler+ (banks/ACK/IRQ) and EchoPlus (full SSI263 — the Mockingboard
  SoundII captured the same chip from the start) now have their append/load
  hooks + round-trip pins. CLI `--snapshot-save/load` were silent **no-ops**
  documented as functional — wired onto the same mechanics as the AI server.
  Doc: the "CASS" section never existed.
- **No-Slot Clock: write cycles wired** (AppleWin parity — the DS1216E key
  bit travels on A0 of the ADDRESS, R/W indifferent: drivers feeding the key
  with STA never unlocked the clock).
- **68705: level-sensitive timer IRQ** (vector pull no longer clears the
  request while TIR=1/TIM=0 — MAME parity). **LLE mouse**: no more cursor
  jump after reset (delta counters re-primed from the host). Size caps on
  the .wav/.aci files (pre-slurp), joystick NaN guard, `POM2_AUTO_*` env
  timers fixed and cancellable.

## 2026-06-12 (bug hunt: full audit validated against the MAME oracle)

Systematic audit of the subsystems (CPU/memory, video, audio, storage,
threading/cards), each fix validated against the MAME sources (file+line
citations in comments) and pinned by a test. Suite: 127/127.

- **LSS disk writes angularly mispositioned** (the worst — silent corruption
  in the default config). `DiskImage::writeFlux` reduced the splice window
  with a raw `startLssCycle % period`, while the read (`getNextTransition`)
  is anchored on `revolutionStart` (port of MAME `find_position`). The anchor
  being arbitrary (2×cpuCycleTotal at motor-on), every bit-level write landed
  `revStart mod period` cells away from where the controller had just read the
  address — the data field overwrote another area of the track. **Pitfall #2
  on the same path**: the cell→nibble re-pack assumed 8 cells/byte, but the
  `expandTrackBits` timeline adds +2 cells of padding per sync $FF — drift of
  ~4.75 nibbles/sector on a standard .dsk. Why no test caught it:
  `diskii_lss_smoke_test::testLssWrite` **explicitly skipped the positional
  assertion**. `writeFlux` now takes the anchor (same convention as the read),
  the re-pack walks the padded timeline, and the positional pin is active
  (`disk_writeflux_anchor` + strengthened LSS test).
- **DHGR: hues rotated 90° in the composite OE demods (CPU+GPU).**
  The subcarrier phase shift was applied **twice** (sin/cos table construction
  with `(k+po)&3` AND indexing with `(xi+po)&3`). The GLSL shader comment
  documented the wrong conclusion: the old GPU formula (single application)
  was the right one, it "diverged" because the CPU was wrong. HGR (po=0) was
  unaffected — hence an "excellent" calibration that masked the bug. Pitfall:
  `dhgr_phase_signal_test` pinned the bug **tautologically** (its anchor
  replicated the buggy formula) — the test now derives its expectation from
  the independent MAME LUT path.
- **DLGR: nibble pattern restarted per 7-dot half-cell** instead of the
  absolute 14.318 MHz phase (`paintLoRes40` already did it right) — colors
  alternating per column. Pin: exact samples in absolute phase (the naive
  `sig[i]!=sig[7+i]` test is invalid: at rotl4(1)=2 from x=0 and main 1 from
  x=7 ≡ 3 (mod 4) yield the **same sequence** at different phases).
- **Sound II silent**: the SSI263 emulation (registers+IRQ) was complete but
  `fillAudioBuffer` never mixed `ssi_->fillAudio()` (only EchoPlusCard did).
  **VIA 6522**: the ORA access (reg 1) didn't clear IFR.CA1 (MAME
  `CLR_PA_INT()`) → speech IRQ stuck for drivers using the standard idiom;
  first T1 strike at N+1 instead of N+3 (the +2 bias already existed for T2,
  same DIX rationale). **Native Phasor**: MAME VIA decode
  (`$Cs10`→VIA1, `$Cs80`→VIA2, `$Cs90`→broadcast both, nothing at `$Cs00`).
  **AY**: envelope period 0 = double speed (MAME doesn't clamp to 1).
- **SSC/telnet**: `send()` without `MSG_NOSIGNAL` → a peer that disconnects
  rudely **killed the process** (SIGPIPE); EAGAIN treated as fatal + partial
  sends silently lost (breaks ADTPro/XMODEM); blocking `accept()` not woken by
  `shutdown()` on macOS/BSD (the same bug already documented+fixed in
  AiControlServer — ported the `poll()` pattern).
- **Slot Config "Apply" overwrote the slot config on //c** — exactly the bug
  fixed at release on 2026-06-10, but the Apply path lacked the `builtInSlots`
  guard. **`applyProfile` without lock**: `stop()` didn't wait for the worker
  to park and the frame loop didn't re-check `mode` → ROM/SlotBus/disks
  rebuilt while a turbo frame was still running (UB). The worker re-checks
  between 4096-cycle chunks and the switch waits for `workerParked_`.
  CLI `--speed` clamped to 2 M like the AI server.
- **2IMG: lock bit = bit 31** (spec/CiderPress/AppleWin), not bit 0 — locked
  images were writable; the DOS volume read 0 instead of 254 on a locked dump
  without bit 8. The test pinned the wrong interpretation (written from the
  code, not from the spec — classic pitfall).
- **NMOS CPU: undocumented multi-byte opcodes** ($x3, $4B/$6B/$8B/$AB/$CB,
  $1B..$FB) dispatched as 1-byte NOP → instruction-stream desync (the exact
  class already fixed for $0B/$2B/$EB); $CB/$DB were remapped
  "undefined WAI/STP on NMOS" while NMOS puts SBX #imm (2 bytes) and DCP
  abs,Y (3 bytes) there. 1-byte NOP: 1 cycle on 65C02 (fetch only, MAME
  ow65c02) vs 2 on NMOS; WAI/STP 3 cycles. NMOS decimal SBC: deterministic V
  (binary difference, MAME `do_sbc_d`) — decimal ADC already did it, V stayed
  stale on the SBC side.
- **IIe/II+ memory**: $C010-$C01F is the keyboard strobe mirror on II/II+
  (MAME `.mirror(0xf)`, read OR write — `STA $C01x` never cleared it);
  on IIe any $C01x write clears it (reads $C011-$C01F stay status-only).
  **$FE sentinel**: `iieReadStatus` returned $FE for "not a status" — but
  `0x80|transchar($7E '~')` == $FE is a legitimate read → RDRAMRD polls sent
  to the floating bus (OFF read while ON). Out-of-band signal now. **INTC8ROM**:
  arms on any $C3xx access with SLOTC3ROM=off **including under INTCXROM=on**
  (UTAIIe 5-28) and on the write path; a $C3xx write no longer steals the
  $C800 window from the legitimate card. **Video events in VBL**: stamped at
  line 192 ("end of frame") instead of being clamped to 191 — a mode switch
  thrown in VBL (the canonical anti-tearing practice) no longer paints a
  spurious split on the last visible line.
- **Misc validated**: HDV STATUS returns the block count in X/Y (the BITSY
  crash already fixed on the SmartPortCard side); a 32 MiB volume of exactly
  65,536 blocks clamped to $FFFF (read 0 before); `writeBackEnabled`
  propagated to images on snapshot restore; ClockCard no longer loses the time
  on Ctrl-Reset (battery-backed uPD1990AC); the AI server's `/mouse`
  recognizes the AppleWin HLE mouse (//c default); mouse VBL in profile cycles
  (PAL 20313); PAL clock: provenance honestly documented (locked at
  line 15625×65 by design; MAME = 1,016,966 — assumed 0.13% gap, same class
  as the "device clocks stay NTSC" approximation).

## 2026-06-10 (//c: NMOS CPU freeze, rear Chat Mauve, slot config preserved)

- **"POM2 crashes when I select the Apple //c (1984) profile"** — it was a
  **CPU freeze**, not a segfault. Diagnosis: the user's config had
  `cpu_mode_override=nmos` (sticky setting, set once on a II+).
  `resolveCpuMode` therefore returned **always NMOS**, including for the //c —
  which has a **soldered 65C02**. The //c ROM runs 65C02 opcodes
  (`LDA (zp)`=$B2…) that **decode as KIL on NMOS** (`M6502::Hang` = `PC--` →
  infinite loop) → frozen CPU → dead screen = "crashed". (Not reproducible
  headless because the symptom is frozen emulation, not a process crash;
  isolated by analyzing `M6502.cpp` + reproducing the mid-frame switch.)
  **Fix**: `resolveCpuMode` honors an **NMOS** override only if the profile is
  NMOS by default (II/II+///e-unenh); the **65C02-only** machines (//c, //c+,
  //e enhanced, PAL variants) always run in CMOS. The Machine→CPU menu
  **greys out** "NMOS 6502" on these profiles. Also answers "the CPU should
  switch NMOS↔65C02 per the //e/enhanced profile". Verified: //c resolves
  `CPU = 65C02` despite the override.
- **Le Chat Mauve on //c (rear connector).** The //c took the
  **"IIc Adapter"** Le Chat Mauve on its DB-15 video port (cf.
  fenarinarsa.com/?p=1370 + CLAUDE.md § profiles). POM2 ignored any card on a
  `noPhysicalSlots` profile. **Fix**: exception for `chatmauve` — the RGB card
  plugs into the //c-class machines (it's a video adapter, not a peripheral
  slot card). The Slot Config panel offers a combo **{(empty), Le Chat Mauve
  RGB (rear connector)}** on //c/+ (nothing else is pluggable; the duplicate
  check limits it to one adapter).
  **The "Apple //c PAL (Le Chat Mauve)" profile now wires the card in hard**
  (built-in **sl7** = "IIc Adapter") — it carried the name without plugging in
  the card. On this profile the other slots' combo is greyed out (a single
  adapter), and a redundant user `chatmauve` elsewhere is ignored (no double
  card).
- **Slot config overwritten on exit on //c.** `persistSettings` saved the
  **live** `slotCards` mapping, so quitting on //c wrote the forced built-ins
  (`mouseaw`…) over the user choice (`slot_4_card=mockingboard` lost on return
  to //e). **Fix**: don't persist profile-forced slots (built-ins + slots
  cleared by `noPhysicalSlots`, except the user-controllable Chat Mauve) — the
  user setting stays intact. Same class as
  [[pom2-cffa-profile-switch-drop]]. Verified: `slot_4_card=mockingboard`
  preserved after a clean exit on //c.

## 2026-06-10 (DROL cut-scene: $C050-$C057 reads → floating bus)

- **DROL cut-scene hang.** Disk image scan: the cut-scene overlays
  (offsets 0x14359/0x143d5/0x14be0 of `Drol.dsk`) sync via
  `LDX #$02 / LDA $C050 / CMP #$80 / BNE / DEX / BPL` — three consecutive
  reads of the **video scanner** through a display soft-switch. POM2 returned
  a hard 0 on `$C050-$C057` reads → infinite loop (LinApple's historical hang;
  AppleWin fixed it in 1.13.0 by implementing the floating bus). **Fix**: a
  `$C050-$C057` read toggles the mode AND returns `floatingBus()` (MAME
  `apple2.cpp do_io` does the same); likewise for the speaker `$C030-$C03F`
  (latch undriven). **Pitfall avoided**: the block held `stateMutex` and
  `floatingBus()` re-takes it — work scoped before the return. Pinned by
  section (d) of `vapor_lock` (the game's exact loop locks on an HGR page
  filled with $80, and the TEXT-off side effect is preserved).

## 2026-06-10 (DROL: double-buffer flips vs beam-racing; Chat Mauve: AppleWin decode)

- **DROL flicker (unsynchronized page flips)** — all display modes, NTSC as
  well as PAL. Diagnosed by probing the real disk (`tests/drol_probe.cpp`,
  WOZ): DROL flips `$C054/$C055` every ~4 frames at **drifting** positions
  (23/31 flips in the visible zone) — free double-buffering, NOT beam-racing
  (its flipper is self-modifying at `$6138`; DROL's floating bus only serves
  the cut-scene, cf. AppleWin 1.13.0 "fixed the hang at Drol's cut-scene").
  **Why it flickered**: the beam-raced replay paints the band above the flip
  from the page the game is **already redrawing** — POM2 reads RAM at render
  time, not at beam passage → half-erased sprites. (The real beam read the
  still-intact page; before per-frame publishing, these events were often lost
  → accidentally "clean" full-page render.) **Fix**: in
  `forEachBeamSegment`, a frame whose PAGE2 events all go in the SAME
  direction = buffer flip → final page applied to the whole frame (= the RAM
  actually displayable); a frame that flips in BOTH directions (DIX MODPAGE:
  page 1 left, page 2 right of the same line) keeps the exact replay. Pinned
  `drol_pageflip_render`; `dix_modpage_split` unchanged.
- **The 6 560-wide painters re-read the live state** (`renderText80`,
  `renderDhgr`, `renderLoResDouble`, `renderTextChatMauveFgBg`,
  `renderHgrDuochrome`, `renderHiResChatMauve80` → internal
  `mem.getDisplayState()` instead of the band's `state`) — same class of bug
  as the one already fixed on the legacy painters: PAGE2/ALTCHAR mid-frame
  splits were ignored in 560 (that's why "Chat Mauve didn't flicker": it
  masked the flips). Threaded signatures, state passed everywhere.
- **Chat Mauve HGR resolution**: the color decode overwrote EVERYTHING in
  aligned-pair blocks (1 color / 4 dots = 140 effective → "soft" image).
  Ported the AppleWin `RGBMonitor.cpp UpdateHiResRGBCell` algo: a pixel is
  COLOR only if it forms an isolated 010/101 pattern with its neighbors
  (its aligned pair's color, 2 dots); everything else is black/white
  **at the full 280 px resolution** — the white runs (text, sprite outlines)
  recover their sharpness, faithful to the real RGB card. `*/hgr*/chatmauve`
  goldens regenerated; semantics pinned by `le_chat_mauve_smoke` +
  updated `display_persistence_smoke`.

## 2026-06-10 (PAL beam-racing: per-video-frame publishing + 1× speeds per standard)

- **Publishing the video event log per video frame** (the 50/60 Hz hand-off).
  The old model opened the log on every worker CPU tick
  (`beginVideoEventFrame`) and the UI **stole** it at vsync (`takeVideoEvents`
  closed the bracket); any event recorded between the UI take and the next
  tick was **silently lost** (`recordVideoEvent` no-op with the bracket
  closed). **Why it mattered**: in PAL the worker runs at 50 Hz and the UI at
  60 Hz → systematic 10 Hz beat: ~1 UI render in 6 fell in the same tick and
  received an *empty* log (→ `renderInternal`, zero splits), the others a
  *partial* log — the French Touch mid-scanline effects (*Mad Effect*, DIX)
  flickered and lost bands. No test caught it (they bracket synchronously).
  **Fix**: continuous recording; `Memory::advanceCycles` **publishes**
  `{frame-start state, events}` at each crossing of a video-frame boundary
  (65 × 262 NTSC / 312 PAL cycles — aligned on the scanner geometry, not on
  the worker's 17045/20313 budget); `takeVideoEvents` returns a **copy** of
  the last published frame, re-renderable at will by the 60 Hz UI. The
  synchronous bracket remains available for tests (`legacyEventBracket_`). A
  reset purges both logs (otherwise a phantom replay against the wiped state).
  Bonus: the WASM path (which never called `beginVideoEventFrame`) gains
  beam-racing. Pinned `video_event_publish`.
- **`$C019`/VBL follows the video standard**: the VBL edge detection
  (`advanceCycles`) and the `$C019` read used a hard-coded 262 lines; a PAL
  demo that measures the VBL period saw a 17030-cycle frame while the floating
  bus swept 20280 — two contradictory machines. Pinned by extending
  `pal_timing` (§ 4: lines 262–311 = VBL under PAL, wrap at 312).
- **1×/2×/4× speed derived from the active standard** (toolbar, AI server
  `/speed` preset, disk-turbo restoration re-seeded by `applyProfile`).
  **Why**: the hard-coded 17045 ran a PAL machine at 17045 × 50 Hz = 852 kHz
  (−16%) on the first "1×" click — effects that drag, sluggish Mockingboard
  music. Remaining assumption: `MouseCardAppleWin::kCyclesPerVbl` = 17045
  (60 Hz VBL pacing of the mouse HLE even under PAL — no effect on the demos,
  to be reworked with the //c port).

## 2026-06-01 (Release v0.7)

- **Version bump v0.6 → v0.7.** Updated the version string in the
  **5 canonical locations** listed by `CLAUDE.md` § Version string locations:
  `CMakeLists.txt` (`project(... VERSION 0.7 ...)`, which also drives
  `CPACK_PACKAGE_VERSION` + the `build_dist.sh` archive name), `src/main.cpp`
  (console banner + initial window title), `src/MainWindow_Slots.cpp`
  (runtime title that overwrites `main.cpp`'s once the profile is resolved — at
  the constructor **and** at the profile switch), `src/MainWindow.cpp`
  (*About* dialog), and `README.md` (title). `CLAUDE.md` itself updated
  (`Current release: **v0.7**`). **Why note it**: the version lives in
  duplicated strings not derived from a single source, so any bump must touch
  these points en bloc on pain of drift (window title vs About vs package).
  Single CMake source → generated header = separate backlog item.

## 2026-05-31 (Composite: signal beam-racing + phosphor curve)

- **Composite signal beam-racing.** `fillCompositeSignal` read *a single*
  end-of-frame `getDisplayState()`: the mid-scanline display switches
  (text↔graphics, page flip, DHGR on/off) were invisible in **all** composite
  modes (`ColorCompositeOE` GPU, `ColorCompositeOECpu`, `ColorAppleWin`) —
  only the LUT modes benefited from beam-racing (`renderBeamRacing` on the
  RGBA side). **Why it mattered**: a demo that goes from text to HGR mid-screen
  displayed entirely in HGR under OE/AppleWin. **Fix**: `render()` takes the
  event log **once** and passes it to both consumers;
  `fillCompositeSignal(mem, events)` replays the log band by band (zero
  `signalBuf` → `getDisplayStateAtFrameStart()` → `paintSignalBand(y0,y1)`
  which reuses the same `bandRows`/`bandScanlines` clipping as
  `renderInternalBand`). The painted `state` is a *mutable* local so the
  helpers (captured by reference) see each switch. Empty log →
  `paintSignalBand(0,192)` = byte-for-byte the old dispatch (OE GPU/CPU
  goldens unchanged). **Pitfalls**: `signalPhaseOffset_` stays a per-frame
  constant (last graphics band wins → mid-frame HGR↔DHGR split approximated);
  lo-res clips at the block-row (4 lines), like the RGBA path. Pinned
  `beam_race_composite` (text→HGR frame at scanline 96: top band = text
  waveform, bottom band = HGR waveform, and **not** HGR at the top like the
  pre-fix bug).
- **Phosphor curve (CRT gamma).** The signal-level NTSC pipeline (already
  complete: FIR Y@2.0 MHz / chroma@0.6 MHz, YUV→RGB, PAL line-phase) had no
  phosphor response. Added a `phosphorGamma` (per-channel power-law `rgb^γ`)
  in `CrtEffectStack`, **after** BCS and **before** scanlines/mask (the mask
  attenuates the light the phosphor has already produced). γ = 1.0 = identity →
  **no golden/parity touched**; γ > 1 deepens the shadows, γ < 1 lifts them.
  It's the *luminance* half of the phosphor model; `persistence` is the
  *temporal* half. Slider "Phosphor curve (gamma)" 0.6–2.6, persisted
  `ntsc_phosphor_gamma`. NB: a *glass* effect, hence always active under OE and
  under the other modes when "CRT effects on all modes" is on.

## 2026-05-31 (3D voxel view — phases 0+1; toolbar rewind button)

- **3D voxel view — "Voxel Cube" rework faithful to MicroM8 (fix).**
  The first version extruded **height = luminance** on a screen laid **flat**
  (XZ plane): bright pixels turned into stalactites, catastrophic angle,
  misshapen voxels. Scraped MicroM8 (`paleotronic.com` Quick Start +
  Features): the "Voxel Cube Color" mode stands the screen **upright**
  (monitor, XY plane) and gives **each pixel a cube of the same thickness**
  extruded toward the viewer on **+Z** ("Voxel Depth"). The height is **never**
  tied to luminance; the per-color relief (`colorShift`, "Z-axis 3D offset")
  is an **option** (default 0 → a flat slab you rotate to see the thickness).
  - **Geometry**: cube footprint XY + depth Z; column→X, row→Y (row 0 = top);
    **real 4:3** plane (2.0 × 1.5) to keep the shape of Apple II pixels.
    `heightScale`→`voxelDepth` (0.06), `+colorShift`.
  - **Camera**: defaults near front-on + slight 3/4 (azimuth 0.32 /
    elevation 0.20 / distance 2.8 / fovY ~40°), target recentered at the
    origin. **Orbit on left-drag + wheel zoom** (in `drawScreenImage`, mutate
    `voxelCam_`). `voxel3d_math` stays green (the camera math is unchanged).
  - **Follow-up (same day)**: (1) **top/bottom inverted** — the
    FBO→`ImGui::Image` presentation is a vertical mirror (like the 2D NTSC
    passes); pre-flip `gl_Position.y` in the vertex shader. (2) **Native
    resolution** — `gridW/gridH` driven by `display->width()/height()`
    (280/560 × 192) → one voxel per Apple II pixel (before: 140×96, half the
    info lost); `voxelDepth`/`colorShift` passed in **cell units** to stay
    constant between 280 and 560 wide. (3) **Per-color relief enabled**
    (`colorShift` 8 cells, luminance-weighted) → requested pin-art "pop".
    (4) **CRT-independent** — the voxel taps the color image **before**
    `CrtEffectStack` via a separate `voxelSrcTex` handle (otherwise
    scanlines/mask/barrel ended up baked into the 50k cubes).
    (5) **Pan/strafe on the middle button** — `OrbitCamera::pan` slides the
    target in the camera's right/up plane, scaled in world-units/pixel for 1:1
    tracking (orbit = left-drag, zoom = wheel). (6) **Moiré removed** —
    **abutting** cubes (`cubeFill` 0.9→1.0: a flat fill becomes a continuous
    slab again, end of the grid of gaps) **+ supersampling** (`superSample`
    2×: FBO rendered at 2×, mip-chain, trilinear minify by ImGui →
    anti-aliasing without MSAA resolve).
  - **Phase 3 — settings panel** (`renderVoxelSettingsWindow`, View ▸
    "3D voxel settings…"): live sliders `voxelDepth` / `colorShift` /
    `cubeFill` / `superSample` (pushed to **3×** by default) / `ambient`, +
    Reset view / Reset settings buttons. The renderer is **owned from the
    moment settings load** (ctor without GL) so the panel and the `voxel_*`
    keys (persisted) wire directly onto `voxel3d_`, even before enabling the
    3D view. The grid resolution stays auto (= screen).
  - **P4 — WASM perf guard**: under `__EMSCRIPTEN__`, `process()` caps
    `superSample ≤ 2` + FBO ≤ 2048² (reduces the factor until it fits) and
    `MainWindow` caps `gridW ≤ 280` (halves the 560 DHGR geometry).
    Native unchanged (`ss ≤ 4`, 8192²). WASM/WebGL2 compile re-checked.
  - **Fidelity bonus — Mono + per-color-index depth**: `mono` checkbox
    ("Voxel Cube Mono", grey output, relief preserved) and `perColorDepth`
    (snap to the nearest of the 16 lo-res `kVoxelPalette` colors → discrete
    per-color relief instead of continuous luminance). 16-iteration search
    loop in the vertex shader (per instance, not per pixel); `glUniform3fv`
    added to the loader. Persisted `voxel_mono` / `voxel_percolor_depth`.
    P5 (rewind tie-in) **deferred** on request.
  - **Wheel fix in WASM**: Emscripten's GLFW port doesn't deliver `wheel`
    events to ImGui (`io.MouseWheel` stayed at 0 → 3D zoom inoperative in the
    browser). Added an `emscripten_set_wheel_callback("#canvas")` in
    `main.cpp` that feeds `io.AddMouseWheelEvent` (same scale as the ImGui
    backend) — a surgical choice so as not to touch the shell's canvas sizing
    (vs `ImGui_ImplGlfw_InstallEmscriptenCallbacks` which also hooks
    resize/fullscreen).

- **Rewind button in the toolbar** (left of Pause, mirroring Step on the
  right): `ICON_FA_BACKWARD_FAST`, **hold = live-rewind** (same gesture as
  `F6` / the Devices ▸ Rewind bar). Greyed out while there's no history.
  `F6` and the button share a single edge-tracker (`driveRewindHold`).

- **MicroM8-style 3D voxel view — foundations (phases 0+1).**
  - **Why / arch**: extrude the screen into cubes (height = pixel luminance)
    with an orbital camera. Key choice: it's an **orthogonal view axis**, not
    a `HiResMode` — a render pass that consumes the **already-decoded RGBA
    texture** (any color mode + NTSC/CRT), exactly like `CrtEffectStack`.
    Universal, free for all modes.
  - **Phase 0 — `Mat4.h`**: Vec3 + column-major Mat4 (perspective/lookAt/
    multiply) + `OrbitCamera` (azimuth/elevation/distance → view-proj). No
    dependency (no glm). Pinned `voxel3d_math` (perspective entries,
    orthonormal lookAt basis, projection of the target to the center).
  - **Phase 1 — `Voxel3DRenderer.{h,cpp}`**: **instanced** cubes
    (`glDrawElementsInstanced`, ~13k for 140×96), height+color per
    **vertex texture-fetch** of the framebuffer, shading by **screen
    derivatives** (no normal attribute → stays on the single `aPos` bound by
    the shared shader helper). FBO **with depth** (the 2D passes have none).
    Same lazy-init + GL state save/restore pattern as `NtscPostProcessor`;
    WebGL2/GLES3 compatible (instancing + VTF + derivatives, no geometry
    shader). Toggle **View ▸ "3D voxel view"** (persisted `show_3d_voxel`),
    wired into `drawScreenImage` before the final blit.
  - **To follow**: orbital camera on mouse drag + zoom (P2), settings panel +
    lighting (P3), resolution steps / heightfield (P4), rewind tie-in
    "freeze + orbit" (P5). The GL render is verified by running the app
    (no golden hash — the camera math, itself, is tested).

## 2026-05-31 (Rewind — delta codec, UI, disk state, heavy cases: phases 2→5)

- **MicroM8-style rewind completed (phases 2 to 5).** The base (phases 0+1,
  below) stored full snapshots; these phases make it actually usable.
  All pinned: `rewind_delta`, `rewind_transport`, `rewind_slot_state` (+
  `rewind_roundtrip` unchanged = the API's regression safety net).
  - **Phase 2 — XOR delta + keyframes codec** (`RewindBuffer`): a full
    keyframe every `keyframeInterval` frames (default 120 ≈ 2 s), XOR deltas
    between (only the modified spans, coalesced over gaps < 16 bytes). 30 s
    goes from ~315 MB to ~10 MB. Reconstruction = nearest keyframe ≤ i + XOR
    of the deltas. **rebase-on-evict** eviction: the front always stays a
    keyframe (the next delta is promoted before dropping). Public API
    unchanged → `rewind_roundtrip` (phase 1) passes as-is = proof of
    non-regression. *Why keyframes+delta rather than reverse-delta alone:
    XOR is its own inverse, so a single delta direction serves bidirectional
    scrubbing, and keyframes bound the cost of random seek.*
  - **Phase 3 — UI + transport + live-rewind**: `Rewind_ImGui` (Devices ▸
    Rewind) — Record toggle, timeline, transport |< / << (hold) / <| / |> /
    resume, history-duration slider; `F6` = hold-to-live-rewind everywhere
    (MicroM8 gesture). Restore served with the **worker parked**:
    `rewindBeginScrub()` sets Stopped then `waitUntilParked()` waits for
    `workerParked_` (set in the worker's Stopped CV wait) → a UI restore can't
    be overwritten by an in-flight Running frame (the Running branch exhausts
    its whole budget before re-checking the mode). `rewindEndAndResume`
    restores + `truncateAfter` (discards the abandoned future) + restarts.
    Ring emptied on `coldBoot`.
  - **Phase 4 — slot card state**: `SlotPeripheral::append/loadSnapshotState`
    (no-op by default); `DiskIICard` serializes its mechanical state + LSS
    (quarter-track head, motor, phase magnets, data register, sequencer,
    rotational timing — **not** the media or the PROMs). `MachineSnapshot`
    writes `SLOTn` sections **only if `includeSlots=true`** (rewind opt-in;
    the AI-control `/snapshot` API keeps its "disk excluded" contract — an
    archive file may survive a media change). The restore routes to the slot's
    card (magic+version → a foreign card ignores a blob that isn't its own)
    and tolerates their absence. A rewind during a disk I/O no longer leaves
    the head on the wrong nibble. Full bit-for-bit machine round-trip (incl.
    SLOT6) pinned.
  - **Post-review hardening** (multi-agent review): (a) the park handshake
    race fixed — `setMode(non-Stopped)` clears `workerParked_` on the setter
    side, otherwise a fast resume→rescrub read a stale flag; (b)
    `DiskIICard::loadSnapshotState` bounds `activeDrive` (index guard); (c)
    history slider disabled during scrub (eviction would shift the indices);
    (d) `SnapshotIO` memory backend rewritten as a zero-copy streambuf
    (`VectorOutBuf`/`ArrayInBuf`) — removes the double-copy via `stringstream`
    on each capture (~21 MB/s on IIe, much more with RamWorks).
  - **Phase 5 — heavy cases**: `maxBytes_` memory budget (default 256 MiB) on
    top of the frame cap → RamWorks (~10 MB/keyframe) bounded (less history
    rather than RAM that explodes). `flushAudioForRewind()` (speaker reset) on
    each restore → a time jump is silent, not a "pop". Capture also wired into
    `tickFrame()` (single-thread WASM path).
  - **Audio chips serialized** (closing the audio gap): `MockingboardCard` and
    `PhasorCard` serialize the register/timer state of their `Via6522` (24 b) +
    `Ay3_8910` (34 b) via the `SlotPeripheral` hook — `append/loadSnapshot`
    helpers shared by both cards, LE packing pooled in `ByteIO.h`. So the music
    survives a rewind (not just the speaker flush). The AY is a register model
    (the synthesis derives from the 16 registers) → restoring the registers
    restores the sound exactly. Pinned `rewind_audio_state` (full bit-for-bit
    machine round-trip incl. Mockingboard).
  - **SSI263 speech serialized**: `Ssi263::append/loadSnapshot` (30 b: 5
    registers + the phoneme read cursor), wired into the Sound II variant of
    `MockingboardCard` → speech also survives a rewind. Covered by
    `rewind_audio_state` (Sound II block).
  - **Disk writes undone on rewind** (Phase 6): the DiskIICard snapshot bumped
    to v2 — it carries the nibble track buffers for the loaded disks that are
    physically writable and non-WOZ
    (`DiskImage::append/loadMediaSnapshot`), so a disk write is undone by a
    rewind. The delta codec keeps the cost ~nil as long as no track is
    written; the read caches re-derive from the restored nibbles. Read-only /
    WOZ / empty disks = 1 flag byte. Pinned `rewind_disk_write` (media COW +
    write-via-card undone end-to-end).
  - **Remaining gap**: writes on a writable WOZ not undone (WOZ stores its bits
    in `wozRaw`, a distinct store; WOZ originals are generally
    write-protected). Tracked cleanly if needed. Detail → `DEV.md` § Rewind.

## 2026-05-31 (Rewind — foundations, phases 0+1)

- **MicroM8-style rewind — capture/restore state base (no UI).**
  - **Why**: record the machine state continuously to allow going back in time
    (scrub/step-back). Architecture choice: a ring-buffer of state snapshots
    (RetroArch-style) rather than deterministic input replay — decoupled from
    the CPU hot-path, robust, and reuses `SnapshotIO` as-is. The delta/keyframe
    (to reduce the ~175 KB/frame cost) is phase 2; here we store full snapshots
    to validate the capture→restore loop bit-for-bit.
  - **Phase 0 — `SnapshotIO` memory backend**: `SnapshotWriter(vector&)` /
    `SnapshotReader(ptr,len)` alongside the existing file backend, via a
    `std::stringstream` bound to a `std::ostream&`/`std::istream&` member
    (all the section/length logic reused). Binary format identical between the
    two backends. Pinned: `snapshot_memory_roundtrip` (round-trip +
    byte-for-byte parity vs the file writer).
  - **`MachineSnapshot.{h,cpp}`**: extraction of the canonical
    `CPU`/`MEM`/`MEX` sequence out of `AiControlServer` (which slims down by
    ~63 lines). Single source of truth shared by the AI-control API AND the
    rewind, so no more possible divergence. The security hardening stays: a
    16-byte length gate on the CPU section (over-read of a forged blob,
    "round 10 #3") + a 16 MiB MEX cap → `RestoreResult{false,…}` (the API
    always returns 400). Covered by `ai_control_server_smoke` (no regression).
  - **Phase 1 — `RewindBuffer.{h,cpp}`**: a `std::deque` ring of full
    snapshots, oldest-first eviction beyond `maxFrames` (default 1800 ≈ 30 s
    @ 60 Hz), `restore(i)` / `restoreToCycle(cycle)`. Capture wired at the
    `workerLoop`'s quiescent frame boundary (after the CPU budget + IWM tick),
    guarded by `enabled()` before taking `stateMtx` → zero cost when disabled
    (default). Pinned: `rewind_roundtrip` (bit-for-bit round-trip + eviction +
    `restoreToCycle` seek).
  - **Assumed gaps this phase**: card/disk state out of the snapshot (a rewind
    during disk I/O leaves the head where the live sim put it → phase 4:
    `SlotPeripheral` hook + `DiskIICard` drive state); audio chips desynced;
    no UI (phase 3); WASM not wired (phase 5). Detail → `DEV.md` § Rewind /
    time-travel.

## Earlier (≤ 2026-05-30) — archived

The entries from 2026-05-30 back to 2026-05-14 (pre-v0.7) are moved into
[`docs/archive/CHANGELOG-2026-05.md`](docs/archive/CHANGELOG-2026-05.md) to
keep this file focused on the current cycle. Full history → `git log`.
