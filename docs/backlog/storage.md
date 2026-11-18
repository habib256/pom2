# Backlog — Storage

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Reorganised 2026-10-05. Priorities are inherited from the original TODO;
older reports are **To verify**, not confirmed defects on current main.
Before implementation, reproduce the described behavior on current main and
record the result. If already resolved, remove the active entry and link its
resolution in the changelog. Frozen work requires a named software need or
an explicit request before scheduling.

<a id="historical-findings"></a>

## Task index

| Task | Priority | State | Subject |
|---|---|---|---|
| [STORAGE-002](#storage-002) | 🟡 | Partial: API gap; needs decision | Secondary-bay boot selection and control API |
| [STORAGE-005](#storage-005) | 🧊 | Frozen — triage 2026-10-28 | SmartPort ProDOS multi-partition |
| [STORAGE-010](#storage-010) | 🧊 | Frozen — deliberate limit | A `.nib` track is not normalised to one 200 ms revolution |
| [STORAGE-007](#storage-007) | 🧊 | Frozen | Half-tracked NIB (88) |
| [STORAGE-008](#storage-008) | 🧊 | Frozen | Floppy Emu Dual-5.25" + Smartport-Unit-2 modes |
| [STORAGE-016](#storage-016) | 🟠 | In progress | Fractional WOZ bit cells (optimal_bit_timing 33) |
| [STORAGE-017](#storage-017) | 🟢 | Ready | A2R 3.x flux import, ShrinkIt archives, multi-disk archives |
| [STORAGE-018](#storage-018) | 🧊 | Frozen — needs a re-bless | 5.25" timeline re-based on the true LSS clock (8.17 cycles a cell) |

<a id="storage-014"></a>

## STORAGE-014 — ProDOS host folder write-back

**Priority:** ✅ · **State:** Done 2026-11-17; removed from the active index.

`storage_folder_eject_race` injects guest writes at the existing commit
barrier after phase-1 capture, without adding a runtime test hook. It covers
zero, one, three and four racing writes through the real coordinator, HDV
card, block backing and ProDOS decoder, and verifies the exact host bytes,
number of captures, final mounted/dirty state and absence of conflicts.

The test reproduced a real defect: the first successful unlocked decode did
not update the mounted backing's folder timestamp. The next pass treated
POM2's own previous save as a host edit, kept the older main file and sent
the later guest version to `.pom2-conflicts`. Success now adopts the decode's
timestamp under the machine lock, in both single-bay and eject-all commands, as partial failure already did, only for
the original mount identity. Failure restores dirty blocks only to that mount.

**Retained limit:** after four captures, guest writes made during the fourth
commit are flushed inline by the final locked eject. The deterministic fourth
write is retained and tested. A five-byte, 72-block synthetic volume measured
about 0.65 ms for that final eject on the local Mac; this is not a latency
bound for a large host directory. The lock/I/O tradeoff remains explicit.
See [the evidence](../audits/core-priorities-2026-11-17.md).

By design, not a defect: a file the host deleted while mounted and the guest
CHANGED is written back (the guest's data wins over a delete it never saw);
an unchanged one stays deleted. A deleted host directory comes back empty.

<a id="storage-002"></a>

## STORAGE-002 — Secondary-bay boot selection and control API

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): `bootMountedMedia` refuses secondary bays, so the wrong-medium risk is gone; a slot+bay `/media` endpoint needs a decision.

**Acceptance criterion (after revalidation):** Select and boot a secondary bay through both the UI and control API, with consistent bay numbering.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1476). Original dates and estimates are retained below.

- 🟡 **Secondary-bay boot selection and control API** *(2026-09-27).*
  `/disk` still only mounts Disk II; add explicit Sony / SmartPort slot-and-bay
  control with the same duplicate and failed-write-back safeguards. Entering
  a slot ROM or reset firmware does not guarantee it boots a requested
  secondary bay when an earlier bootable disk is present. A physical-chain
  editor should also validate Apple's three-external-floppy combinations
  separately from logical SmartPort HD units; the current panels expose
  these mechanisms independently.

<a id="storage-005"></a>

## STORAGE-005 — SmartPort ProDOS multi-partition

**Priority:** 🧊 · **State:** Frozen — [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). One image is one volume (`kMaxBlocks = 0x10000`). Reopen when a named multi-partition image is needed.

**Acceptance criterion (after revalidation):** Enumerate and read multiple ProDOS partitions from one image using an explicit guest-visible unit mapping.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1528). Original dates and estimates are retained below.

- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.

<a id="storage-010"></a>

## STORAGE-010 — `.nib` track length

**Priority:** 🧊 · **State:** Frozen — deliberate limit ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)). A rescale would re-base the LSS timeline and every golden, and no software needs it. Reopen if a title is shown to depend on the 200 ms revolution.

Fixed 2026-10-27: `.nib` sync bytes follow MAME's rule (`ap2_dsk.cpp`
`a2_nib_format::generate_levels_from_nibbles`) — only "FF … FF D5" is a
sync pattern, 10 cells per $FF; other $FF stay 8; a bit-7-clear byte is
transparent (`disk_cnib2_smoke`).

Left: MAME then rescales the whole track to one 200 ms revolution
(`flopimg.cpp` `generate_track_from_levels`, `200000000 * time / total`).
POM2 keeps 4 µs cells, so a `.nib` revolution is 6656 × 8 cells plus the
syncs' 2 each — about 213 ms. Rescaling re-bases the LSS timeline
(`DiskImage.cpp`'s period comment) and every golden hash with it.

<a id="storage-011"></a>

## STORAGE-011 — SmartPort image-format restrictions (documented)

**State:** Done (2026-10-20); retained here for stable incoming links.

**Acceptance criterion:** Document unsupported flux and image formats without claiming support.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1992).

The original report also named missing FORMAT/INIT parameter-count checks.
Reproduced and fixed 2026-10-12, along with INIT accepting nonzero units;
see [CHANGELOG](../../CHANGELOG.md). `liron_smartport_dispatch` covers the
guest-visible error codes and valid calls. 5.25-inch WOZ 2.1 FLUX tracks now
support write-back with a save/reload regression. Native 3.5-inch GCR
WOZ/MOOF writes also preserve 125 ns timing and container metadata
(2026-10-18, `disk35_flux_write`). MFM MOOF block writes now splice existing CRC-valid
sectors and preserve untouched 125 ns transitions (`moof35_load`); GCR HLE
block mounts still require raw conversion.

**Source reconciliation 2026-10-20:** `Disk35Image::loadFile` accepts raw
400/800 KiB images, ProDOS-order 2IMG payloads of those sizes, and supported
WOZ/MOOF captures. `moof35_load` includes a single-sided 400K capture,
export and raw reload. 1.44 MB requires the SuperDrive capability. DiskCopy
4.2 header/tag decoding remains unsupported; raw `.image` files must have
a supported payload size. These boundaries are now documented in
the [user guide](../USER-GUIDE.md#pick-the-right-image-and-drive). This closes
the format-documentation criterion; it does not claim a new test execution.

<a id="storage-003"></a>

## STORAGE-003 — Heap-backed DiskImage track storage

**Priority:** ✅ · **State:** Done 2026-11-09; removed from the active task index on
2026-11-13 and retained for incoming links. The track buffers are heap-backed;
ARM64 object size is 20688 bytes instead of 253624. Copies/moves/reloads and
three simultaneous values run under a constrained 512 KiB stack. Session
accounting includes the buffers, and validators eject moved-from media before
destruction. [Validation and timing limits](../audits/core-consolidation-2026-11-09.md).

The original rationale below describes the pre-change implementation.

**Acceptance criterion:** Remove the large in-object buffers and run constrained-stack media tests plus before/after benchmarks.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1499). Original dates and estimates are retained below.

- 🟢 **`DiskImage` is a 242 KB object, and the stack-overflow class is only
  patched, not closed.** *1 d, measure first.* The 2026-08-23 macOS SIGBUS
  fix heap-allocates the six insert-path temporaries; any future
  `DiskImage` local on a secondary thread (512 KB on macOS) reintroduces the
  crash, and `diskii_insert_thread_stack` only pins the insert path. Closing
  the class means moving `tracks` (35 × 6656 B, in-object) to the heap —
  which also turns every `DiskImage` move (the install under `stateMutex`)
  from a 233 KB memcpy into a pointer swap. It adds one indirection to the
  bit-stream rebuild and to `writeFlux`, neither per-nibble-hot, but the
  LSS is the emulator's hottest disk code: interleaved best-of-9 on the
  three `pom2_bench` workloads (PERFORMANCE § 8) before and after, or not
  at all. The remaining stack local in the AI control server's `/disk`
  handler was moved to the heap on 2026-10-18; the class-wide change and
  broader constrained-stack coverage remain open. Until then the NOTE on
  `class DiskImage` is the guard for new callers.

<a id="storage-004"></a>

## STORAGE-004 — Track-address validation (resolved)

**State:** Done (2026-10-18); retained here for stable incoming links.

`DiskImage::decodeNibbles` and `decodeNibbles13` validate the address-field
checksum and require its track byte to match the physical track being saved.
Valid-checksum headers that name another track are skipped instead of being
written to the wrong location. Regression cases cover both 16- and 13-sector
write-back. This is not an active missing-validation task.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1512)
and [CHANGELOG](../../CHANGELOG.md).

<a id="storage-007"></a>

## STORAGE-007 — Half-tracked NIB (88)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1537). Original dates and estimates are retained below.

- 🟢 **Half-tracked NIB (88)** — deliberately out of scope as long as
  WOZ covers it. Its two former companions are done: **Disk II in
  snapshot** (snapshot v2 with nibble track buffers,
  `DiskIICard` snapshot v2, pinned `rewind_disk_write` — see [UI/UX]
  Rewind) and the
  **Applesauce CNib2 format** (detected in `DiskImage.cpp`'s CNib2 branch of the `.nib` loader, pinned
  `disk_cnib2_smoke`) — only the literal `.nib2`/`.app` extensions are
  still missing from `classifyDiskForSlot` / `accept525`.

<a id="storage-008"></a>

## STORAGE-008 — Floppy Emu Dual-5.25" + Smartport-Unit-2 modes

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1545). Original dates and estimates are retained below.

- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).

<a id="storage-016"></a>

## STORAGE-016 — Fractional WOZ bit cells (optimal_bit_timing 33)

**Priority:** 🟠 · **State:** In progress (2026-11-18): patch prepared, lands after the current commit.

`DiskImage::lssCyclesPerCell()` is `optimalBitTiming / 4` in whole LSS
cycles, so an INFO value of 33 (8.25 cycles, a 4.125 µs cell) truncates to
the 32 of every other image. Prisoner 2's WOZ ([AppleWin
#1005](https://github.com/AppleWin/AppleWin/issues/1005)) needs it: its
weak-bit check wants `$C9` at Y=`$2A` and POM2 measures `$29` — 513-526
cycles where 526-539 are needed ([protected.md](../../tests/corpus/protected.md)).
The fix builds the cell grid in quarter LSS cycles (the unit of the field):
cell `i` centred at `(i·q + q/2)/4`, period `(bits·q + 2)/4`, writes at
`⌊4t/q⌋`, which is the exact integer grid for q = 32.

**Acceptance criterion:** every corpus and display golden unchanged
(byte-identical for 32); the Prisoner 2 image patched to 33 passes its
protection check (`Y=$2A` at `$57C3`) and reaches its title; a WOZ write
splice at 33 reads back what was written. Then the title joins
`protected.manifest` with the patched image's SHA-256 documented.

<a id="storage-017"></a>

## STORAGE-017 — A2R 3.x flux import, ShrinkIt archives, multi-disk archives

**Priority:** 🟢 · **State:** Ready (research of 2026-11-18).

Three exchange formats other emulators read and POM2 does not:
- **A2R 3.x** (Applesauce raw flux, [spec](https://applesaucefdc.com/a2r/)):
  read-only, feeding the existing FLUX-track engine (`DiskImageWozFlux.cpp`);
  Apple2TS and izapple2 import it.
- **ShrinkIt** `.shk` / `.sdk` / `.bxy` (NuFX): a disk image inside a NuFX
  archive, through the same `MediaSource.h` cache the `.zip`/`.gz` path uses.
- **Multi-disk archives**: a `.zip` holding several images (Second Reality's
  two disks, a game's sides) mounts the first and offers the others as a
  per-drive swap list, instead of only the first image today.

**Acceptance criterion:** an A2R 3.x capture of a title already in the corpus
boots to the same checked frame as its WOZ; a NuFX `.sdk` and a two-image zip
mount through `MediaSource` with the archive never written.

<a id="storage-018"></a>

## STORAGE-018 — 5.25" timeline re-based on the true LSS clock (8.17 cycles a cell)

**Priority:** 🧊 · **State:** Frozen: a deliberate, documented limit.

The bit-cell platter turns 2.2 % fast: 8 LSS cycles of 488.9 ns per nominal
4 µs cell (the block above `DiskImage::lssCyclesPerCell`). MAME runs it at
real time. Fixing it moves the byte cadence every pinned disk test, the
DIX/OLDSKOOL raster probes and the write-splice geometry are calibrated
against, so it is a re-basing pass with a full corpus re-bless, not a
constant swap. STORAGE-016 (fractional cells) is the smaller step it builds on.

**Scheduling condition:** a title that fails because of the 2.2 %, shown to
pass at 8.17; then re-bless the corpus with the reason recorded.
