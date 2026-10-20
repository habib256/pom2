# Resumable sessions (v7; v1–v6 compatible)

`.pom2session` bundles CPU/RAM state, supported card state and embedded
floppy/block contents and program tapes, including unsaved writes. Opening a
bundle reconstructs its machine profile, firmware, CPU, memory and supported
card topology automatically; the original ROM and media files are not needed. It complements `.pom2snap`, whose contract remains
CPU/RAM without slot restoration. Both formats deliberately have different
magic bytes: loading a session as a snapshot must fail rather than silently
dropping its cards and disks.

## Supported configuration

- All nine [machine profiles](USER-GUIDE.md#choose-your-apple-ii), including
  the unenhanced/enhanced IIe and PAL variants, with RamWorks where applicable. The profile,
  motherboard/character ROMs, memory configuration and No-Slot Clock settings
  are recorded in the bundle. These replace the active machine when needed.
- Any combination of Disk II, Mockingboard A/C, Mockingboard Sound II,
  SNES MAX, Slinky RAM expansion and ThunderClock+ in their recorded slots. Disk
  II's four PROM images and configuration, Slinky's 4 KB firmware, and the
  clock card's slot/expansion ROM bytes are embedded. Empty slots are
  reconstructed as well. Slinky's sparse 1 MB RAM and the clock's in-progress serial and
  timer state are included; host wall-clock time continues advancing.
- Read-only or writable 5.25-inch media: DOS/ProDOS
  sector images, 13-sector images and nibble images, including normalized
  supported wrappers. Both drives of every Disk II are embedded as nibble
  tracks, preserving their guest-visible bytes and sector geometry.
- WOZ1 and WOZ2, including WOZ 2.1 FLUX captures, retain the canonical WOZ
  payload instead of becoming nibble tracks. Quarter-track maps, bit-cell
  lengths, nonstandard bit timing and sub-cell flux transitions survive.
  Supported MacBinary wrappers are stripped during the original mount.

- On //c-class machines, both built-in serial ACIAs, the IOU mouse,
  Mockingboard 4c and Le Chat Mauve adapter are included. Serial ports must
  be idle, without listeners/connections or queued transport bytes.
- Read-only or writable 400K/800K media in the onboard Sony mechanisms
  and SmartPort 3.5-inch units are embedded, including WOZ/MOOF captures.
  The SmartPort chain geometry and unit types are reconstructed; transfer buffers,
  partially consumed reads and the external-port transaction are restored.

- Version 6 adds both drives of ProDOS HDV cards and HDV units in mixed
  SmartPort chains. Raw and 2IMG block contents, protection flags, dirty
  blocks and partial transfers survive without reopening source files.
  The ProDOS card's built-in slot ROM is checked. SmartPort still requires the
  built-in firmware; external Liron firmware is unsupported.

- Version 7 adds Zip Chip socket state, TransWarp firmware/shadow/timers,
  CFFA 2.0 EEPROM/ATA state and embedded block media, Phasor, SSI263 Echo+,
  TMS5220C Echo+, S.A.M. and 4play. CFFA preserves partial READ, WRITE and
  IDENTIFY transfers, including whether completing the buffer advances the
  ATA taskfile. S.A.M. retains its existing audio-only snapshot contract:
  its next CPU write supplies the DAC level.
- A ProDOS host-folder volume is captured as its current block image. It
  resumes as a detached disk, without the directory path or host syncing.
- Program tapes embed their pulse source, cursor, comparator/output state,
  rewind/pause state and recording buffer. Recording can continue after a
  restore without reopening the source. Streamed audio decks remain excluded.

Le Chat Mauve cards are also supported on slotted machines. SmartPort-based
cards, including CFFA3000 and SuperDrive, preserve their unit topology and
SuperDrive mode through the SmartPort record; they require POM2's built-in
firmware rather than an external Liron ROM. CFFA 2.0 uses its separate ATA
record.

Restored disks are detached from host files; the drive label is `(session disk)`.
Version 5 writable media retain their guest-visible protection/write-enable
state and accept further writes in RAM. Save another session (or keep browser
automatic resume enabled) to retain these changes. Autosave, flush and eject
never write a detached disk to the original file or to the private label.
The drive panels identify this mode. Read-only records from older formats
remain permanently protected. No original disk filename is reopened by the
session decoder.
Reopen the bundle explicitly on subsequent launches, or enable the browser
automatic-resume option described below. The loader recreates the machine configuration automatically.
Host gamepad input, mixer settings and window layout stay live.

Unsupported cards, unknown socket accelerators, pending eject/write-back
operations, active serial transports and streamed audio decks cause refusal. No unsupported device is
silently omitted. A session can capture unsaved floppy writes without
flushing them. Before loading, outgoing dirty host-backed media must still
be flushed or saved; outgoing detached session media can be rolled back.
Outgoing host cassette recordings must be saved/cleared before replacement;
detached session recordings may be rolled back. Remove outgoing network or
Workstation cards before requesting a different topology; the loader does
not stop their host workers under the machine lock.

## Using it

On desktop, choose **File > Save session...** or **File > Open session...**.
The command palette exposes the same actions. POM2 uses the platform file
selector where available and offers a path field otherwise. Save appends
`.pom2session` when no suffix is given, and asks before replacing an existing
file. Cancelling leaves the machine and destination untouched. A failed
operation keeps the path and error visible for correction. Opening validates an isolated candidate before replacing the active machine.
It preserves the current running/paused mode. The desktop publishes the new
profile, drive panels, display and audio inventory together; HTTP loads are
serviced by the same owner-thread queue, including in kiosk mode. Session
media paths and reconstructed slots do not overwrite the usual startup
media/slot settings. An explicit profile or slot rebuild leaves session mode.

In the browser, **File > Save session...** downloads a `.pom2session` archive;
**File > Open session...** opens the browser file selector. The same supported-device restrictions apply, with automatic reconstruction. Imports are bounded
at 32 MiB and validated before live state is replaced.

**File > Automatic resume > Resume this machine on reload** opts the current
supported configuration into checkpoints every ten seconds and on page hide.
The archive and running/stopped mode are stored under the existing IndexedDB
mount. Startup restores after the initial boot actions have settled. Hiding a
running tab saves its intended running mode, rather than the temporary pause
introduced by tab visibility. Disabling the option removes its enable marker;
the previous archive remains available. An unsupported configuration, a
corrupt archive or failed save suspends checkpointing and leaves an error in
the menu; retry explicitly after correcting the configuration. Failed restore
never falls back to a CPU/RAM-only snapshot. The default browser //c topology
and writable floppy images are supported. IndexedDB flushes are asynchronous: an
abrupt browser termination may lose changes since the last completed flush.

Checkpoint files alternate between `browser-resume.pom2session` and
`browser-resume-next.pom2session` under `/persistent`. The
`browser-resume.enabled` marker records the active file and intended
running/stopped mode. A checkpoint writes the inactive file before atomically
publishing the marker, so a failed capture/write does not replace the active
checkpoint. This publication happens in the mounted filesystem; IndexedDB
durability still depends on the asynchronous flush. A corrupt active archive
or marker suspends resume rather than silently selecting the other generation.

After selecting a supported configuration, use:

```sh
POM2 --session-save game.pom2session
POM2 --session-load game.pom2session
```

These are deferred actions: like `--snapshot-save/load`, they execute in CLI
order after startup. A load selects the recorded configuration automatically. For a machine already running with its optional
local AI-control server enabled:

```sh
curl -X POST http://127.0.0.1:6503/session/save \
  -H 'Content-Type: application/json' -d '{"path":"game.pom2session"}'
curl -X POST http://127.0.0.1:6503/session/load \
  -H 'Content-Type: application/json' -d '{"path":"game.pom2session"}'
```

Use the configured port; add `-H 'X-POM2-Token: YOUR_TOKEN'` when a token is
configured. These routes
inherit the server's working-directory path fence and request protections.
Save paths must resolve to `.pom2session`; filesystem writes publish atomically.
Unknown profile IDs and unsupported devices are refused without replacing the machine.

### Preparing a machine and resolving refusals

The default desktop enhanced PAL //e includes Grappler+ and a Mouse Card;
neither is supported in a session. In **Machine > Slot Configuration**, remove
unsupported cards and apply the configuration before saving. The default
browser //c uses supported built-in devices and Mockingboard 4c.

| Refusal | Action |
|---|---|
| Unsupported card | Remove it before capture. Expansion SSC, parallel printer interfaces, Mouse Cards, Videx, VidHD, SoftCard Z80, network and Workstation cards have no session record. The //c IOU mouse is supported. |
| Serial port not idle | Stop VSDrive/listeners/connections and drain queued transport bytes. Only built-in //c ports are supported, even when an expansion SSC is idle. |
| Outgoing medium has unsaved changes or pending writes | Let host write-back finish or explicitly sync/save the outgoing medium before loading. Detached session disks can be rolled back without writing their contents to a host file. |
| Streamed audio deck | Eject it; supported program tapes use embedded pulses. Save or clear an outgoing host cassette recording before replacing it. |
| Archive exceeds 32 MiB | Reduce the mounted media or their contents. Sparse HDVs omit clean zero blocks, but a dense 32 MiB disk plus machine state exceeds the archive limit. |
| Session path suffix | Desktop save dialogs append a missing suffix; CLI and HTTP saves require an explicit `.pom2session` destination, including after symlink resolution. |

Automatic resume uses the same capture restrictions. After correcting a refusal,
choose **File > Automatic resume > Retry with current configuration** if
checkpointing is suspended. A failed load leaves the active machine intact;
it does not replace an unsupported device with an empty slot.

## Wire layout and restoration

All integers are little-endian; maximum file size is 32 MiB.

| Field | Encoding |
|---|---|
| Magic | Eight bytes, `POM2SESS` |
| Version | u32, currently 7; versions 1 through 7 accepted |
| Machine identity | Nonzero u32 profile identity |
| Payload size | u32, exactly file size minus 24 |
| Controller configuration | u32 length + bytes (up to 64) |
| Core snapshot | u32 length + existing ENV/CPU/MEM/MEX snapshot (up to 17 MiB) |
| Cassette (v7+) | u32 length + CTS1 state (up to 9 MiB); empty means no tape |
| Eight slot records | u8 kind + length-prefixed identity (up to 16 KiB) + state (within the 32 MiB file limit; 1 MiB in v1) |
| CRC32 | u32 IEEE reversed-polynomial CRC over all preceding bytes |

Slot kinds are 0 empty, 1 Disk II, 2 Mockingboard A/C, 3 Sound II, 4 SNES MAX,
5 Slinky and 6 ThunderClock+ (v3+), 7 built-in serial, 8 IOU mouse,
9 Le Chat Mauve, 10 SmartPort and 11 onboard Sony media (v4+),
and 12 ProDOS HDV (v6+). Version 7 adds 13 CFFA, 14 TransWarp, 15 Phasor,
16 SSI263 Echo+, 17 S.A.M., 18 4play and 19 TMS5220C Echo+.
Configuration v1 is four bytes: version, No-Slot Clock present, enabled and
slot (255 means no slot). Configuration v2 appends a Zip Chip kind byte (1) and boot-fast flag.
The ENV section carries the exact firmware and geometry used to build the
candidate; standalone `.pom2snap` loads continue comparing ENV without
installing it.
Slot 0 carries kind 11 on //c-class machines and remains empty otherwise;
no actual peripheral may occupy slot 0. The language card lives in the core
memory snapshot. The Sony record contains three presence bytes followed by
three media blobs, one for each wired mechanism (v4). Version 5 follows
each media blob with a Sony write-state blob. SmartPort records contain
runtime state followed by fourteen media blobs; the identity binds the unit
count, controller mode and empty/3.5-inch/HDV bay topology (HDV requires v6).
CRC detects damage, not intentional forgery. The loader still validates all
lengths, type/firmware matches and nested state after checksum verification.
Disk II timelines and VIA counters receive semantic bounds as well, so an
otherwise canonical blob cannot request an unbounded clock catch-up or
overflow a signed timer on the next advance.

Disk II state contains its existing runtime blob followed by two embedded
media blobs. A media blob is `DRO1`, a presence byte, then (when present)
sectors-per-track, sector order and nibble-format bytes plus 35 × 6656 raw
nibbles. Empty blobs are exactly five bytes. No host paths or write-back jobs enter the archive.

Version 2 adds `DWO1` followed by the original canonical WOZ bytes, bounded
at 16 MiB per WOZ payload. The normal WOZ decoder validates these in memory,
including its own CRC when nonzero, without opening a host path. The full
file still has a 32 MiB cap; larger sessions are refused, not truncated.
Decoded media storage across slots is limited to 128 MiB. Version 1 keeps
its original limits and rejects WOZ records; old read-only nibble sessions
remain loadable. Version 2 WOZ sessions also remain loadable. New saves use
version 7. Version 3 binds Slinky state to the exact 4 KB ROM image and the
clock to its slot/expansion ROM bytes. Version 4 adds //c-class devices and
protected 3.5-inch media. Version 5 adds writable/dirty media and their
write framing. Older readers refuse the new version.

A 3.5-inch media blob starts with `D35` and byte 1, a type byte (0 empty,
1 raw 400K, 2 raw 800K, 3 raw 1.44M, 4 WOZ2, 5 MOOF), then a u32-length
payload. Captures retain their canonical container, bounded at 16 MiB;
raw/2IMG images store their decoded blocks without host paths or wrappers.
All restored media acquire a private `(session disk)` name and host notch.
SmartPort runtime media hashes are rebased to that name so an in-progress
transfer survives without referring to the source path.


Version 5 writable/modified media use `DMS` + byte 1, a protection byte,
then a u32-length legacy media payload (`DRO1` / `DWO1` / `D35` + byte 1).
Protection bits 0, 1 and 2 preserve the container lock, host notch and
write-enable policy. Unknown bits and attempts to clear a capture's own
header lock are rejected. Disk II appends a u32-length framing blob:
35 records of 32 bytes (next cycle u64, nibble index i32, shift byte u8,
shift count u8, origin u64, next cell i32, held-valid/bit u8/u8, held cell i32).
The decoder bounds clocks, indices and shift counts before applying them.
This retains partial nibble/cell writes across controller flush boundaries.
Sony write state (`SWS` + byte 1) carries the active write gate, latched
track/head, write cursor tick/cell, cached track/head/revolution period and
length-prefixed 0/1 cells. Raw GCR blocks do not yet contain an incomplete
written sector; this cache and cursor let the next IWM window continue it.
Only write-related caches are included, with a 1 MiB cap per mechanism.
The cache is restored after the core MEX and media commit without issuing
mechanical events. Legacy v4 records retain the prior mechanism contract.

Disk II's nibble runtime path hashes are rebased to the private medium name,
like SmartPort hashes, so the embedded write state can resume.

Capture serializes current WOZ/MOOF tracks using a private copy; it neither
changes the source's dirty flags nor submits file I/O or advances its
irreversible-write epoch. Restored writable disks carry an explicit RAM-only
persistence mode, which gates every take-writeback/autosave path regardless of
future write-enable or notch changes. Their data enter subsequent checkpoints.

Version 6 block media use `BMS` + byte 1, flags u8 (header lock, host notch,
write-enable in bits 0–2), loaded u8, block count u32 and record count u32.
Each record is block index u32, dirty u8 and 512 payload bytes. Records must
be strictly increasing, within geometry, and either dirty or nonzero. Omitted
blocks are zero; dirty zero blocks remain explicit. Geometry is bounded at
65536 blocks (32 MiB), with the same aggregate 128 MiB expansion limit.
2IMG container metadata is omitted after decoding; its protection flag is
preserved. ProDOS HDV records contain a runtime blob and two media blobs.
SmartPort uses these block records in HDV bays and existing floppy records
elsewhere. The complete archive still has a 32 MiB ceiling: dense HDVs or
large combinations can exceed it and are refused without truncation.
v6 did not capture host-folder volumes. v7 captures their current block
contents as detached media: directory paths and synchronization policy are
not serialized, and restoration never reopens or syncs the original folder.
Pending background or captured host commits
prevent capture/replacement until their payloads are released. Restored block
volumes suppress autosave, explicit flush, detach commits and destructor
write-back even if the write-enable policy is subsequently changed.

Capture copies state under the controller lock; encoding, CRC and file I/O
run unlocked. Load reads/checksums/decodes unlocked, then rechecks the live
configuration under the lock. Every card/media record is prepared and checked
using detached decoders before the core's existing non-mutating preflight.
Only a successful core restore is followed by card/media commit. Disk II's
IWM rebinding stays in restore mode, avoiding a sync or write flush over the
new media. Successful controller loads clear the abandoned rewind/debug/audio
timeline. Rejected loads preserve it.

`session_snapshot`, `session_file_picker`, CLI parser/runner tests and the HTTP server smoke test
exercise the path. The session test removes the original floppy before load,
compares resumed Disk II reads, resumes CPU execution, checks 13-sector/nibble
media and verifies corruption/type/firmware/dirty-media refusals. WOZ checks
cover both drives, sparse quarter-tracks, non-byte-aligned bit lengths,
nonstandard timing, FLUX transitions, host-notch protection, records above
1 MiB, unchanged recapture, legacy v1 restore and invalid nested WOZ data.

`session_iic` covers NTSC/PAL //c and //c+, built-in ACIA/IOU/4c/RGB state,
SmartPort partial reads, onboard media, missing source files, checkpoint
reloads, capture media and non-mutating rejection of malformed records.

`session_writable_media` covers dirty Disk II and raw/captured 3.5-inch media,
partial nibble writes, absent host writes, repeated checkpoint rollback and
legacy refusal. Writable WOZ/FLUX and combined //c+ media are covered by
`session_snapshot` and `session_iic`. The Chrome smoke mounts a writable nibble
image, checkpoints it, deletes its source and compares the complete archive
after IndexedDB reload.

`session_block_media` additionally checks sparse 32 MiB HDVs, both ProDOS
HDV drives, mixed SmartPort chains, partial transfers, source independence,
2IMG locks, dirty zero blocks, malformed-record atomicity and dense-image
refusal. The Chrome persistence smoke compares whole archives after deleting
both floppy and HDV source files and reloading their IndexedDB checkpoints.


## Version 7 device records

CFFA identity is a loaded flag followed by its complete 4096-byte EEPROM.
Its state is a length-prefixed CFA1/ATA runtime, one taskfile-advance flag,
and a length-prefixed BMS1 block medium. This extra flag distinguishes
IDENTIFY from READ/WRITE without changing the rewind format.
TransWarp identity is empty or its 4096-byte firmware; volatile state uses
the existing versioned snapshot, including displaced motherboard ROM.
Other v7 cards use their existing snapshot formats and empty identities.
Each is decoded on a detached validator and must round-trip canonically;
Phasor timers are range-checked before loading.

CTS1 stores flags, cassette cycle stamps, the current segment remainder,
playback index, rewind carry, and bounded loaded/recorded duration vectors
(maximum 1,048,576 transitions each). It contains no host path. Hardware
state resumes exactly; host mixer settings and audio output queues remain
host policy. Echo+ resamples to the active device's rate on restoration.

`session_machine` covers all nine profiles, cross-profile firmware/card
reconstruction, Zip/TransWarp, split ATA transfers, detached folder volumes,
program-tape playback/recording continuation, malformed payload refusal and
the owner-thread load queue. The lower-level `restoreSession` remains a
strict same-configuration primitive used by reconstruction preflight.
