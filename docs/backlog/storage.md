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
| [STORAGE-002](#storage-002) | 🟡 | To verify | Secondary-bay boot selection and control API |
| [STORAGE-005](#storage-005) | 🟡 | To verify | SmartPort ProDOS multi-partition |
| [STORAGE-010](#storage-010) | 🟡 | To verify | Bug hunt #16's Disk II read residue |
| [STORAGE-012](#storage-012) | 🟡 | Ready | A WOZ write to a track the file does not have is dropped silently |
| [STORAGE-013](#storage-013) | 🟢 | Ready | Off-track quarter positions and long FLUX writes |
| [STORAGE-003](#storage-003) | 🟢 | To verify | `DiskImage` is a 242 KB object, and the stack-overflow class is only patched, not closed. |
| [STORAGE-006](#storage-006) | 🟢 | To verify | UI "Force DOS / Force ProDOS" |
| [STORAGE-007](#storage-007) | 🧊 | Frozen | Half-tracked NIB (88) |
| [STORAGE-008](#storage-008) | 🧊 | Frozen | Floppy Emu Dual-5.25" + Smartport-Unit-2 modes |

<a id="storage-002"></a>

## STORAGE-002 — Secondary-bay boot selection and control API

**Priority:** 🟡 · **State:** To verify.

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

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Enumerate and read multiple ProDOS partitions from one image using an explicit guest-visible unit mapping.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1528). Original dates and estimates are retained below.

- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.

<a id="storage-010"></a>

## STORAGE-010 — Bug hunt #16's Disk II read residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test NIB self-sync, non-WOZ half-tracks and WOZ CRC policy independently; document deliberate format restrictions.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1973). Original dates and estimates are retained below.

- 🟡 **Bug hunt #16's Disk II read residue** *(2026-09-09; read, not
  fixed)*: `.nib` bytes get flat 8-cell timing with no self-sync slip;
  half-tracks on non-WOZ images snap to the lower whole track (AppleWin's
  rule; MAME reads the neighbour's flux mix); a WOZ CRC32 mismatch is
  refused where AppleSauce/AppleWin warn and load. *2 h.*

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

## STORAGE-003 — `DiskImage` is a 242 KB object, and the stack-overflow class is only patched, not closed.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Remove large DiskImage stack temporaries at the remaining sites and run constrained-stack media tests.

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

<a id="storage-006"></a>

## STORAGE-006 — UI "Force DOS / Force ProDOS"

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Expose forced DOS/ProDOS interpretation and verify it reaches the existing backend and survives reload as specified.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1530). Original dates and estimates are retained below.

- 🟢 **UI "Force DOS / Force ProDOS"** — backend ready
  (`DiskImage::loadFile(path, SectorOrder)`, log at `DiskImage.cpp:820-827`),
  button missing in `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-detect (extension + ProDOS vol-dir sniff `0x400`/`0xB00` + DOS 3.3
  VTOC/catalog-chain sniff, both pinned by `sector_order_smoke` since
  2026-09-07) already covers 99 % of cases; manual override useful for ambiguous /
  non-standard / debug images. *~30 min.*

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
  **Applesauce CNib2 format** (detected at `DiskImage.cpp:544`, pinned
  `disk_cnib2_smoke`) — only the literal `.nib2`/`.app` extensions are
  still missing from `classifyDiskForSlot` / `accept525`.

<a id="storage-008"></a>

## STORAGE-008 — Floppy Emu Dual-5.25" + Smartport-Unit-2 modes

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1545). Original dates and estimates are retained below.

- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).

<a id="storage-012"></a>

## STORAGE-012 — A WOZ write to a track the file does not have is dropped silently

**Priority:** 🟡 · **State:** Ready (found by the 2026-10-23 bug hunt, round five).

**Acceptance criterion:** INIT or ProDOS FORMAT of a WOZ whose TMAP leaves
tracks at `$FF` either succeeds and saves the new tracks, or refuses with a
visible error; never a silent drop.

`DiskImage::writeFlux`, WOZ bit-cell branch: `if (wozQtBitCount[qt] == 0)
return;`. An Applesauce image of a half-used disk often maps its unused
tracks to `$FF`; writes there vanish with no dirty flag and no error, the
format's verify then fails, and the disk cannot be reformatted. The fix is
to allocate a TRKS slot on the first write to an empty quarter-track (about
51,200 bits; `growWozFluxSlot` has the shape: append blocks at the end of
TRKS, point TMAP qt-1/qt/qt+1 at it), WOZ1 and WOZ2 both. Failing that,
set `lastError` and report it.

<a id="storage-013"></a>

## STORAGE-013 — Off-track quarter positions and long FLUX writes

**Priority:** 🟢 · **State:** Ready (found by the 2026-10-23 bug hunt, round five).

**Acceptance criterion:** a pinned test for each case below.

- **Non-WOZ quarter-tracks round down.** `qtSlot` is `qt & ~3` and the
  non-WOZ `writeFlux` uses `qt / 4`, so a head parked at qt 4k+3 (physically
  next to track k+1) reads and writes track k. Fields written there name
  k+1, `decodeNibbles` rejects them on save, and if nothing on track k still
  decodes every save of that disk is refused. Standard RWTS / ProDOS never
  stop there. Round to the nearest track, or ignore writes at qt%4 == 2/3.
- **A FLUX write window longer than one revolution folds.**
  `DiskImageWozFlux.cpp:17-35` clamps the duration to one period but keeps
  every transition modulo the period, so a sparse burst lasting more than
  ~200 ms lays revolution two on top of revolution one. Split per
  revolution as the bit-cell path already does.
