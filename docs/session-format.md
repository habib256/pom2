# Resumable sessions (v3; v1/v2 compatible)

`.pom2session` bundles CPU/RAM state, supported card state and embedded
read-only floppy contents. It complements `.pom2snap`, whose contract remains
CPU/RAM without slot restoration. Both formats deliberately have different
magic bytes: loading a session as a snapshot must fail rather than silently
dropping its cards and disks.

## Supported configuration

- Apple II, II+ and IIe, including PAL and RamWorks, with the same profile,
  motherboard/character ROMs, memory configuration and No-Slot Clock settings
  selected before loading.
- Any combination of Disk II, Mockingboard A/C, Mockingboard Sound II,
  SNES MAX, Slinky RAM expansion and ThunderClock+ in the same slots. Disk
  II's four PROM images and configuration, Slinky's 4 KB firmware, and the
  clock card's slot/expansion ROM bytes must match. Empty slots must match
  as well. Slinky's sparse 1 MB RAM and the clock's in-progress serial and
  timer state are included; host wall-clock time continues advancing.
- Clean, physically write-protected 5.25-inch media: DOS/ProDOS
  sector images, 13-sector images and nibble images, including normalized
  supported wrappers. Both drives of every Disk II are embedded as nibble
  tracks, preserving their guest-visible bytes and sector geometry.
- WOZ1 and WOZ2, including WOZ 2.1 FLUX captures, retain the canonical WOZ
  payload instead of becoming nibble tracks. Quarter-track maps, bit-cell
  lengths, nonstandard bit timing and sub-cell flux transitions survive.
  Supported MacBinary wrappers are stripped during the original mount.

Restored disks are detached and permanently write-protected. No original
disk filename is reopened or written; the drive label is `(session disk)`.
Reopen the bundle explicitly on subsequent launches. It does not recreate the
machine configuration automatically or persist an active session at shutdown.
Host gamepad input, mixer settings and window layout stay live.

Other unsupported cards, //c and //c+, socket accelerators, writable/dirty
floppies, pending eject/write-back operations, cassette content/recording
and loaded onboard 3.5-inch media cause refusal. No unsupported device is
silently omitted. Flush or save outgoing dirty media before loading a bundle.

## Using it

On desktop, choose **File > Save session...** or **File > Open session...**.
The command palette exposes the same actions. POM2 uses the platform file
selector where available and offers a path field otherwise. Save appends
`.pom2session` when no suffix is given, and asks before replacing an existing
file. Cancelling leaves the machine and destination untouched. A failed
operation keeps the path and error visible for correction. Opening resumes
the saved state in the currently selected matching configuration; it does
not change the running/paused mode. These desktop actions are hidden in the
browser build, which does not expose host filesystem paths.

After selecting a supported configuration, use:

```sh
POM2 --session-save game.pom2session
POM2 --session-load game.pom2session
```

These are deferred actions: like `--snapshot-save/load`, they execute in CLI
order after startup. Restore requires the matching configuration to have
already been selected. For a machine already running with its optional
local AI-control server enabled:

```sh
curl -X POST http://127.0.0.1:6503/session/save \
  -H 'Content-Type: application/json' -d '{"path":"game.pom2session"}'
curl -X POST http://127.0.0.1:6503/session/load \
  -H 'Content-Type: application/json' -d '{"path":"game.pom2session"}'
```

Use the configured port and authentication header when enabled. These routes
inherit the server's working-directory path fence and request protections.
Save paths must resolve to `.pom2session`; filesystem writes publish atomically.
Automatic card recreation is not supported.

## Wire layout and restoration

All integers are little-endian; maximum file size is 32 MiB.

| Field | Encoding |
|---|---|
| Magic | Eight bytes, `POM2SESS` |
| Version | u32, currently 3; versions 1, 2 and 3 accepted |
| Machine identity | Nonzero u32 profile identity |
| Payload size | u32, exactly file size minus 24 |
| Controller configuration | u32 length + bytes (up to 64) |
| Core snapshot | u32 length + existing ENV/CPU/MEM/MEX snapshot (up to 17 MiB) |
| Eight slot records | u8 kind + length-prefixed identity (up to 16 KiB) + state (within the 32 MiB file limit; 1 MiB in v1) |
| CRC32 | u32 IEEE reversed-polynomial CRC over all preceding bytes |

Slot kinds are 0 empty, 1 Disk II, 2 Mockingboard A/C, 3 Sound II, 4 SNES MAX,
5 Slinky and 6 ThunderClock+ (both v3 only).
Slot 0 must be empty; the language card lives in the core memory snapshot.
CRC detects damage, not intentional forgery. The loader still validates all
lengths, type/firmware matches and nested state after checksum verification.
Disk II timelines and VIA counters receive semantic bounds as well, so an
otherwise canonical blob cannot request an unbounded clock catch-up or
overflow a signed timer on the next advance.

Disk II state contains its existing runtime blob followed by two embedded
media blobs. A media blob is `DRO1`, a presence byte, then (when present)
sectors-per-track, sector order and nibble-format bytes plus 35 × 6656 raw
nibbles. Empty blobs are exactly five bytes. No host paths, dirty flags or
write-back jobs enter the archive.

Version 2 adds `DWO1` followed by the original canonical WOZ bytes, bounded
at 16 MiB per WOZ payload. The normal WOZ decoder validates these in memory,
including its own CRC when nonzero, without opening a host path. The full
file still has a 32 MiB cap; larger sessions are refused, not truncated.
Decoded media storage across slots is limited to 128 MiB. Version 1 keeps
its original limits and rejects WOZ records; old read-only nibble sessions
remain loadable. Version 2 WOZ sessions also remain loadable. New saves use
version 3, which binds Slinky state to the exact 4 KB ROM image and the clock
to its slot/expansion ROM bytes. Older readers refuse the new version.


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
