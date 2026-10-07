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
| [STORAGE-011](#storage-011) | 🟡 | To verify | Bug hunt #15's SmartPort residue |
| [STORAGE-003](#storage-003) | 🟢 | To verify | `DiskImage` is a 242 KB object, and the stack-overflow class is only patched, not closed. |
| [STORAGE-004](#storage-004) | 🟢 | To verify | `decodeTrack` trusts the address field |
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

## STORAGE-011 — Bug hunt #15's SmartPort residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Reject malformed FORMAT/INIT parameter counts; document unsupported flux and image formats without claiming support.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1992). Original dates and estimates are retained below.

- 🟡 **Bug hunt #15's SmartPort residue** *(2026-09-09)*: `FORMAT` and
  `INIT` skip the parameter-count check; WOZ 2.1 FLUX tracks, 400K and
  DiskCopy 4.2 images are refused with a clear message rather than
  supported. *½ day.*

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
  at all. Until then the NOTE on `class DiskImage` is the only guard.

<a id="storage-004"></a>

## STORAGE-004 — `decodeTrack` trusts the address field

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test malformed track address fields without unsafe indexing or corrupted decode results.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1512). Original dates and estimates are retained below.

- 🟢 **`decodeTrack` trusts the address field** *(management audit
  2026-08-08)* — the write-back decoder reads vol/track/sector/checksum
  as 4-and-4 but validates none of them: the checksum is discarded, and
  the address field's TRACK number is ignored in favour of the buffer
  index. A guest that rewrites a whole track with a different track
  number in its address fields (sector editors, Locksmith-style
  copiers) therefore lands its sectors at the wrong file offset. `$D5`
  is not a legal GCR data byte so a spurious prologue match can't
  happen, which is why this has never bitten in practice. *~1 h.*

**Revalidated 2026-10-06 — partly shipped.** The decoder is now
`DiskImage::decodeNibbles` (`decodeTrack` is its per-track wrapper,
`src/DiskImage.h:713`). The address-field checksum is validated since
21dfaf3 (bug hunt #5, 2026-09-08): a field whose `vol ^ trk ^ sec` does
not match is skipped (`src/DiskImage.cpp:2546-2550`). The TRACK byte is
still ignored in favour of the buffer index — that half remains open.

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

<a id="shipped-pending-removal"></a>

## Shipped — pending removal

Found already shipped on current main during the 2026-10-06 documentation
audit. Per the [task lifecycle](../../TODO.md#task-lifecycle) they leave the
active backlog once the result is recorded in `CHANGELOG.md`; each names the
entry that covers it. Kept here, anchors intact, until that removal.

<a id="storage-001"></a>

### STORAGE-001 — `Sony35Drive::monW` keeps the controller's enable line wired to the motor, and upstream does not

**Priority:** 🟡 · **State:** Shipped — pending removal.

**Acceptance criterion (after revalidation):** Exercise motor strobes through the MIG path while keeping liron_boot35 and iicplus_boot35 green.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-826). Original dates and estimates are retained below.

**Shipped — evidence (2026-10-06):** `Sony35Drive::monW` is now a no-op,
as in MAME (`src/Sony35Drive.cpp:682-689`; the motor follows the CA/LSTRB
MotorOn/MotorOff commands). Landed in 2fbff64 (2026-09-27) without a
separate MIG strobe sequencer; `liron_boot35` was updated to match in the
2026-09-29 bug hunt. CHANGELOG: *2026-09-27 — External Sony drives and
intelligent UniDisk chains* ("Sony motor control follows CA/LSTRB commands
rather than IWM enable"). Still to do for the acceptance criterion: a
green `liron_boot35` / `iicplus_boot35` run on current main.

- 🟡 **`Sony35Drive::monW` keeps the controller's enable line wired to the
  motor, and upstream does not** (bug hunt #4). MAME's
  `mac_floppy_device::mon_w(int) {}` is a no-op (`floppy.cpp:2835-2838`): a
  Sony's motor answers only its own MotorOn (0x2) / MotorOff (0x6) register
  strobes, so `/MOTORON` and `/READY` should follow the mechanism rather than
  the IWM. The alignment was made and reverted the same day: `liron_boot35`
  fails with *"the drive's motor never ran"*, because POM2's Liron and //c+
  paths reach the mechanism through `IWMDevice`'s enable line rather than
  through a modelled MIG strobe sequencer. That sequencer comes first.

<a id="storage-009"></a>

### STORAGE-009 — Bug hunt #17's //c residue

**Priority:** 🟡 · **State:** Shipped — pending removal.

**Acceptance criterion (after revalidation):** Round-trip SmartPortHub sel35 across a motherboard snapshot without regressing native IOU mouse behavior.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1947). Original dates and estimates are retained below.

**Shipped — evidence (2026-10-06):** the //c+ external-3.5" select is
saved as an `S35` tail on the MIG section and pushed back into the hub on
restore (`IIcClassProfile::appendSnapshotState` / `loadSnapshotState`,
`src/MemoryProfile_IIcClass.cpp:346-350`, `:393-403`), landed in 0843c88
(2026-09-17). Pinned by `iwm_mig_snapshot` (`tests/iwm_mig_snapshot_test.cpp:293-299`).
CHANGELOG: *2026-09-17 — Bug hunt: five reviewers, fifteen fixes*
("The //c+ external-3.5" select is now saved").

- 🟡 **Bug hunt #17's //c residue** *(updated 2026-09-10)*:
  `SmartPortHub::sel35_` is in no snapshot where MAME saves `m_35sel`
  (a `MIG1` → `MIG2` bump). The mouse IOU gap is now closed: native
  axis/edge/IRQ registers and motherboard snapshots, with unmodified //c
  firmware (`iic_mouse_lle`).

<a id="storage-012"></a>

### STORAGE-012 — The notch on a mounted 5.25" or 3.5" image

**Priority:** 🟡 · **State:** Shipped — pending removal.

**Acceptance criterion (after revalidation):** Write, toggle the image notch and flush mounted 5.25/3.5 media without losing earlier dirty data.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2026). Original dates and estimates are retained below.

**Shipped — evidence (2026-10-06):** probed and fixed in bug hunt #21:
`Disk35Image::takeWriteBack` and `DiskImage::saveDirty` no longer fold the
notch in, so a 5.25" nibble and an 800K block notched while dirty both reach
the file. Pinned by `media_notch` (and the shared rule in `media_contract`).
CHANGELOG: *2026-09-15 — Bug hunt #21: a notch that dropped the save, and a
ratchet that never armed*.

- 🟡 **The notch on a mounted 5.25" or 3.5" image** *(2026-09-09, bug hunt
  #14)*: `Block512Backing` no longer strands blocks written before the
  notch was flipped (`block512_notch_flush`); `Disk35Image` and `DiskImage`
  gate their write-back on the same combined protect and are reachable by
  `setMediaNotch` on a mounted disk — same shape, not yet probed. *2 h.*
