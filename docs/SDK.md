# POM2 core SDK policy

POM2 exposes one optional C++17 facade: `include/pom2/core.hpp`, implemented by
`src/Pom2Core.cpp`. It is a PIMPL wrapper over `Memory` + `M6502` + the small
set of devices an embedding host needs (display, speaker, cassette, Disk II,
3.5" SmartPort, hard disk, Mockingboard, Apple Mouse, the //c's built-in mouse),
so a consumer never sees an internal definition.

Its contract is pinned by the `pom2_core_api` test, which drives the facade
through a boot, a display frame, audio pulls, a disk mount, a slot-5 boot off an
800 KiB image with a write that is flushed and read back from the host file, and
a full cassette record/save/clear cycle, and asserts the header stays
self-contained. The hard disk half has its own test, `pom2_core_hdd`.

## Scheduling, input and frontend boundaries

`Core` is single-threaded and has no wall-clock pacing or host device loop.
The embedding host schedules `run(minCycles)` or `step()`; `run` returns the
actual executed cycles and may overshoot the requested budget at an instruction
boundary. `CoreConfig` selects CPU silicon, PAL/NTSC timing and IIe memory
at construction, with an initially empty expansion bus — or a named
`Machine` (see [Machines](#machines-and-the-c)).

The default `CoreConfig{}` selects NMOS 6502, NTSC and non-IIe memory. It does
not select the desktop's enhanced PAL //e profile. Instances are movable,
noncopyable and must not be accessed concurrently. `run(0)` or a negative
budget returns zero without executing an instruction.
Returned cycles measure elapsed emulated time, not retired instructions:
a halted CPU still advances device time in two-cycle idle steps. Check
`cpuState().halted` when deciding whether guest execution can make progress;
reset clears that halt state.

`loadRom` and `loadCharacterRom` load the paths supplied by the host; they do
not run the desktop profile-selection workflow or start execution. Choose the
construction settings, load firmware, attach devices and then call `coldBoot`
or a boot method. `read`/`write` perform real emulated bus accesses, including
soft-switch side effects; they are not memory-inspector peeks. Failed
load/attach/save/boot operations report their error through `lastError()`;
queries such as `diskInserted()` can return `false` as an ordinary result.

System ROM loading recognizes 12/16/20/32 KiB layouts: 12 KiB maps at
`$D000`, 16 KiB at `$C000`, and a 20 KiB dump skips its first 4 KiB before
mapping the remaining 16 KiB. For 32 KiB, `lowerBankFor32K=false` selects
the upper firmware half of a combined //e dump; `true` selects //c bank 0
and retains its alternate bank. Other accepted sizes from 2–64 KiB use
best-effort placement at the top of memory; acceptance alone does not make
an arbitrary dump compatible with the chosen machine. Character ROM data
is loaded separately through `loadCharacterRom`.

`setPaddle(0..3, value)` and `setPaddleButton(0..2, pressed)` expose the full
game port. `setJoystickAxes(x, y)` is a convenience for paddles 0/1 only;
deadzone, inversion, right-stick mapping and host gamepad discovery belong
to the embedding frontend. `queueKey`/`pasteText` inject characters, while
`setKeyHeld`/`releaseHeldKeys` supply physical IIe AKD state separately.

`queueKey` models a single hardware latch: without pending paste bytes,
another call replaces an unread character. While paste bytes remain queued,
live keys append after them. Values are masked to 7 bits; II/II+ memory mode
also folds ASCII lowercase to uppercase. Use `pasteText` for a whole string
rather than calling `queueKey` repeatedly before running the guest.

`pasteText` normalizes CR/LF/CRLF to CR, drops controls below `$20` except
CR and tab, and applies the same II/II+ case fold. A well-formed UTF-8 buffer
uses best-effort ASCII transliteration of supported accents/punctuation;
unsupported Unicode characters are omitted. Other buffers use Apple
high-ASCII masking. New paste input is limited by a 4096-character capacity
including pending input and can be truncated. The return value counts
accepted output characters, not consumed input bytes or characters already
read by the guest; it cannot serve as a UTF-8 retry offset. Delivery follows
guest keyboard polling: after clearing the strobe, the next `$C000` read
promotes a queued character. `pasteText` does not run the CPU or wait for
the text to be consumed.

`renderFrame()` returns a borrowed CPU-rendered framebuffer without OpenGL;
the next render, move-assignment or destruction invalidates the view. What that
picture is, and is not, is [§ Rendering](#rendering). For audio, configure the sample rate before running cassette code. Pull either
the speaker-only stream or the complete stereo mix for a given span of guest
time, because both consume the same speaker timeline.

Framebuffer pixels are packed as `0xAABBGGRR` in `uint32_t` values; use the
returned width and height. `pullSpeakerAudio` takes one float per frame;
`pullAudio` takes two interleaved floats per frame, left then right. Their
default rate is 44100 Hz. Allocate the caller-owned buffers accordingly.

Both audio pulls fill and return the requested frame count, including silent
samples; the return value is not an available-audio count. A null buffer,
zero frame count or zero sample rate returns zero. Pulling audio does not
advance the CPU. The host must coordinate emulation and output scheduling;
the facade exposes no desktop pause/suspend or master-mixer controls.

`pullAudio` uses the application's shared mix law with unity master gain,
unmuted stereo output and a final clamp to `[-1, +1]`. It visits the speaker,
cassette and every attached card audio source. `pullSpeakerAudio` bypasses
that final mixer clamp and returns the filtered speaker signal with its
source gain applied; it does not promise the same amplitude bound. Apply
any host output limit required by the destination format.

Disk II drive indices, SmartPort unit indices and hard disk unit indices are
**0 and 1**, while expansion slot numbers are **1 through 7**. The facade
provides one Disk II controller with its two drives (slot 6 by default), one
optional SmartPort controller (slot 5 by default), one optional hard disk device
(slot 7 by default), and multiple Mockingboards in separate empty slots (slot 4
by default).

`attachDiskII(bootRom, sequencerRom, slot)` takes any slot from 1 to 7: the
P5 PROM finds its own slot at run time (`JSR $FF58`), so a controller in
slot 5 boots at `$C500` and hands the boot sector X = `$50`. A second call
replaces the controller after successfully loading the new PROMs, moving it
when the slot differs; it does not retain the mounted disks. A different card
in the requested slot refuses attachment, and a //c, whose disk port is wired
to slot 6, refuses any other. SmartPort can be attached only once, and
SmartPort/Mockingboard attachment requires an empty destination slot.
`bootDisk()` cold-boots and enters `$Cn00` of the controller's slot; its
success means that the controller/PROM/media preconditions passed, not that
the guest has completed booting. It needs a disk in **drive 0**: the PROM
boots drive 1 and nothing else, so a disk in drive 1 alone is refused rather
than left spinning. As `bootSlot` does, it returns false after the cold boot
when the machine maps its own firmware over that page (slot 3 on a //e).
`bootSlot(n)` on the controller's slot is `bootDisk()`.

Both drives take `insertDisk`, `ejectDisk` and `diskInserted`.
`flushDisk(drive)` writes the guest's writes back to the image file without
ejecting, as `flushSmartPortDisk` does. `setDiskWriteProtected(drive, on)` is
a sticker over the mounted disk's notch: the guest reads the drive's
write-protect line and nothing reaches the image or its file, until the disk
is ejected or replaced. Protecting first writes back what the guest already
wrote. It never touches the file's read-only bit, which stays the
application's notch (`MediaNotch.h`), and lifting it cannot unprotect a
read-only file, a write-protected WOZ/2IMG, or a run under
`POM2_MEDIA_WRITE_DEFAULT=protected`. `diskWriteProtected(drive)` reports the
protection the guest sees, whatever its cause.

Soft reset preserves A/X/Y, but reloads PC from the reset vector, updates P
and decrements SP. Hard reset clears A/X/Y while preserving RAM; cold boot
also resets RAM to POM2's power-on pattern. Reset calls do not reopen media
or reload firmware files.

The desktop/browser `.pom2session` workflow, automatic machine reconstruction,
bank-aware debugger and source panel are not methods on this public facade.
They belong to the application/controller layer. Linking `POM2::core` does
not expose them through `include/pom2/core.hpp`; see
[sessions](session-format.md) and [debugging](debugger-banks.md) for those
application contracts.

## CPU registers

`setCpuState(state)` overwrites A, X, Y, P, S and PC at the current
instruction boundary; the next `step()`/`run()` executes at the new PC.
`cycles` and `halted` in the struct are ignored, and nothing else moves: the
cycle counter, every soft switch (80STORE, RAMRD/RAMWRT, ALTZP, the language
card, video) and a pending IRQ stay as they were — what a trampoline through
the RESET vector cannot promise, since a //e reset clears them. P's bit 5
reads 1 on every 6502 and is forced. A host calls a guest routine by writing
the return address minus one on page 1, setting S and PC, and stepping until
the PC comes back (pinned by `pom2_core_api`, NMOS and 65C02).

## Observing the bus

`setBusWatch(fn)` receives each **CPU** bus access to a watched address, after
it was performed, with the value read or written and the cycle it lands on —
the instruction's own data cycle, the stamp the video soft-switch log uses,
on the clock `cpuState().cycles` counts. `watchAddresses(first, last, reads,
writes)` arms a range (ranges add up); `clearWatchedAddresses()` disarms
everything and an empty `setBusWatch({})` turns reporting off. Opcode fetches,
interrupt stack pushes and soft-switch reads count. The host's own
`read()`/`write()`, and the facade's peeks (boot signatures, the mouse's
screen holes), do not: reporting is limited to `run()`/`step()`. Write watches
cost nothing on the fast path; a read watch sends every bus read out of line
while one is armed (`Memory.h` § Read watchpoints), so arm only what you
watch. Do not call `run()` or `step()` from the callback.

`speakerToggleCount()` counts speaker toggles — accesses to `$C030-$C03F` —
since the Core was created, a host `read()`/`write()` there included: on the
bus it clicks too.

## Display state and the RGB card

`displayState()` returns the video soft switches as the motherboard holds
them, without side effects: `text`, `mixed`, `page2`, `hiRes`, and on a //e
`eightyColumn`, `doubleHiRes` (the DHIRESON latch, `$C05E`), `altCharSet` and
`eightyStore`; `an3` follows `$C05E`/`$C05F` on every machine. On a ][+ — which
has no `$C01A-$C01F` status reads — the //e fields stay false. The screen
shows double hi-res only when `doubleHiRes`, `eightyColumn` and `hiRes` are
set and `text` is not.

`attachRgbCard(slot, model)` installs a Le Chat Mauve. `LeChatMauveFeline` is
the //e's aux-slot card (slot 7 by convention; it needs //e memory);
`LeChatMauveIIcAdapter` is the "Adaptateur IIc" on the //c's DB-15 and needs
`Machine::AppleIIc`, where `slot` is ignored and the adapter is kept on slot
7, as in the application. One card per core. Its mode latch is the
patent's two-bit shift register: each `$C05E`→`$C05F` edge pushes the current
80COL level, so `$C00C,$C00D` gives the mixed mode, `$C00C,$C00C` 560 dots
and `$C00D,$C00D` 140 colours (the power-on and reset mode).
`rgbDhgrMode()` reports `Color140`, `Mono560` or `Mixed` — `Color140` with no
card. The card has no slot ROM and no I/O space, so a program cannot detect
it, which is why a host checks it from here. `renderFrame()` keeps painting
the composite picture; the card changes what `rgbDhgrMode()` reports, not the
software framebuffer.

## E-Z Color (TMS9918A)

`attachEzColor(slot, siliconStrict = true)` installs Steve Ciarcia's E-Z
Color Graphics Interface (BYTE, August 1982) in an empty slot of a ][+ or a
//e — a //c refuses, one card per core. It has no ROM: `$C0n0` is the VDP's
data port, `$C0n1` its register / status port, every other `$C0nX` reads
`$FF`, so a program finds it by probing those two. The VDP is POM1's
TMS9918; its `/INT` is the slot IRQ (R1 bit 5 with the frame flag F, dropped
by a `$C0n1` read), and a bus reset resets it.

- `ezColorState()` reads R0-R7, the status (F, 5S, C, fifth-sprite number),
  the VRAM pointer, `/INT`, the mode and the drop counter **without touching
  the chip**: F stays set until the guest reads `$C0n1`.
- `ezColorVram(out, len, from)` copies VRAM (16 KB, wrapping).
- `renderEzColorFrame()` is the card's own monitor, 288 × 216 (the 256 × 192
  picture at x 16, y 12 inside the R7 border), `0xAABBGGRR`, the same lifetime
  as `renderFrame()`. The VDP paints it line by line as its beam passes, so
  below the beam it still shows the previous frame; sample it at a fixed point
  of the frame (after the guest's VBlank wait) for comparable captures.
- **The access window.** With `siliconStrict` (the default, and what the
  application runs) a port access the chip has not had time to serve is
  dropped, as on the part, and counted in `droppedAccesses`;
  `resetEzColorDropCount()` zeroes it. The gap is not one fixed figure: it
  comes from openMSX's TMS9918 access-slot tables, so it depends on the mode
  and on where the beam is. Measured over three frames of data writes
  (`ezcolor_card` pins it): **8 cycles apart is never dropped** in any mode;
  7 drops in Graphics I / II, while Multicolor needs 7 and Text 3; in VBlank
  or with the display off about 2. A program keeping ≥ 18 cycles is well
  inside. A burst that comes too close keeps only its newest byte, which the
  chip writes at its slot — at the address current then. Strict mode also
  makes R1 bit 7 select 4 KB / 16 KB addressing, as the silicon does: set it.
- **Time.** The VDP runs on CPU cycles (it advances with each instruction,
  catching its raster up to the beam at every write), on its own NTSC frame of
  17 062 cycles (59.94 Hz, 262 lines, 192 active) whatever the Apple's video
  timing — the card has its own crystal. Same cycles, same VRAM, same picture,
  same drops.

## Mouse

`attachMouse(slot, model, romDirectory)` installs the Apple Mouse Interface
Card in an empty slot (4 by convention), reading `mouse_341-0270-c.bin`
(and, for `MouseModel::AppleMouse`, the MC68705 mask ROM
`mouse_341-0269.bin`) from `romDirectory`. `AppleMouse` runs Apple's firmware
on both chips, as MAME does; `AppleMouseAppleWin` runs the slot EPROM
against AppleWin's high-level MCU. Movement, button and VBL interrupts
(at the beam's line 192) reach the 6502 through the slot bus, as in the
application. One mouse per core.

`moveMouse(dx, dy, button)` is relative motion — what the card's hardware
receives — and exact on both models; on `AppleMouse` keep each axis within
±127 between two run slices, because the MCU reads an 8-bit counter.
`setMouse(x, y, button)` aims at an absolute position in the firmware's clamp
window (the units of the slot's screen holes `$0478+n`/`$04F8+n`). It is a
closed loop, the application's (`MouseSync.h`): each call pushes at most ±127
per axis toward the target, and pushes nothing while the previous correction
has not been consumed and published — the MCU replays a push as quadrature
over several frames, and correcting against a stale position overshoots.
Call it once per frame; it returns true when the card reports the target.
Pinned through Apple's own firmware (INITMOUSE, SETMOUSE, READMOUSE, and
about one VBL interrupt per frame) on both models.

## Machines and the //c

`CoreConfig::machine` defaults to `Machine::Custom`, which takes `cpu` and
`iieMemory` as given (the struct's last member, so the positional
`{cpu, timing, iieMemory}` form keeps its meaning). `AppleIIPlus`,
`AppleIIe` (NMOS) and `AppleIIeEnhanced` (65C02) set those two fields;
`AppleIIe` takes the unenhanced //e ROM (`roms/apple2e_unenh.rom`, with
`apple2e_char_us_unenh.rom`), the machine a "65C02 required" check runs on.
`AppleIIc` selects the 65C02 and //e memory, loads a 32 KiB system ROM as a
//c dump (bank 0 at reset; 16 KiB rev-255 dumps load as usual) and plugs the
//c's IOU mouse on port 4: its firmware is in the system ROM, so
`moveMouse`/`setMouse` drive it with no `attachMouse`, which refuses on a
//c. The ROM's IRQ handler integrates the motion, so it moves while the
guest runs with interrupts enabled. `attachMockingboard()` on a //c is the
Mockingboard 4c on the internal connector, whatever slot is passed: dormant
until the guest writes into `$C400-$C4FF`, then answering that page — over
the mouse firmware, entry table included — until a power cycle (a Ctrl-Reset
leaves it awake, as MAME's `reset_w` does). Pinned with the 32 KiB rev-0
and 16 KiB rev-255 ROMs. The //c's serial ports are not part of the facade, and
its external SmartPort chain is reached only through `attachHardDisk`.

`AppleIIcPlus` is the //c+ (`roms/apple2cp.rom`): a //c whose mouse firmware
sits at `$C700`, with the MIG, the IWM and the two Sony 3.5" drives its ROM
drives itself; `attachHardDisk` adds the rear port. They answer the SmartPort calls of the next section — unit 0
the internal drive, unit 1 the external one, which joins the chain when a
disk is first put in it — with nothing to attach (`attachSmartPort` refuses).
`bootSlot(5)` is a cold boot through the ROM's own scan, as the application's
slot-5 boot is: internal drive first, the external one when the internal bay
is empty. A Disk II sits in slot 6 from the start, sharing the IWM — the ROM
reads the 5.25" port even with no 5.25" disk — so `insertDisk` works without
`attachDiskII`, and `bootDisk` needs no PROM (`$C600` is the system ROM, as on
the //c). `attachMockingboard()` is the 4c+, the same rule as the 4c, over the
ROM's memory-expansion page. The facade runs it at 1 MHz: the CGGA
accelerator, which the ROM switches on at every reset, is the application's.
The ROM's slot-6 ProDOS block driver at `$C64E` (the slot-5 SmartPort code
called as slot 6) serves the 5.25" disk. Pinned by `pom2_core_api`: a 3.5"
boot from the internal drive (X = `$50`), and through `$C64E` the numbers of
M.G.'s "Apple IIc Plus: SmartPort Secrets" — STATUS A = `$00`, 280 blocks,
and block 2 read back.

## 3.5" SmartPort

On a //c+ the calls below reach its built-in drives (previous section).
`attachSmartPort(slot)` plugs POM2's block-level `SmartPortCard` with two
`SmartPort35Unit`s (ProDOS drive 1 / drive 2 of the slot) and its **synthetic**
slot ROM; no Liron dump is read. It identifies as a ProDOS block device
(`$Cn07 = $01`, `$CnFF` → driver), so ProDOS 8 drives it through the slot
driver and never enumerates SmartPort units 3+. `insertSmartPortDisk` takes an
underlying `Disk35Image`: raw ProDOS-order 400/800 KiB media, supported 2IMG
wrappers and GCR WOZ/MOOF captures decoded as blocks. It is not limited to the
800 KiB `.po` used by the API regression. The facade does not enable SuperDrive
MFM/1.44 MB mode and provides no HDV-unit or chain-size setter; its two units
remain distinct from the application's configurable 2–14-unit chain. A volume
of any other size is a hard disk: see the next section.

`bootSlot(n)` checks the card's `$Cn01/$Cn03/$Cn05` boot signature before
resetting, then cold-boots and checks that the ROM is visible on the CPU bus.
A missing signature leaves RAM untouched; failure of the post-reset visibility
check returns `false` after the cold boot. A Disk II left in slot 6 does not
prevent a slot-5 SmartPort boot.

**Timing**: the card has no mechanical model — no seek, no rotation, no motor
spin-up. A block costs exactly the CPU cycles of the slot ROM's 512-byte copy
loop against `$C0n3`, so the same image booted twice reaches the same state on
the same cycle (pinned).

**Writes** to writable block media land in the unit's in-memory image and reach the file on
`flushSmartPortDisk(unit)` (atomic temp + rename), on an insert over the disk,
on an eject, and when the `Core` is destroyed. The facade runs no autosave
thread — it has no threads. The write default follows `MediaWritePolicy.h` like
the Disk II's: writable, unless the image file is read-only or
`POM2_MEDIA_WRITE_DEFAULT=protected`.
GCR WOZ/MOOF captures are write-protected through this block interface; the
application's native-IWM capture writes are a separate path. A failed explicit
flush/eject/replacement reports an error and keeps the outgoing medium mounted.
Flush explicitly when the host needs to observe save errors before destroying
the core.

## Inspecting RAM and the keyboard

`read` and `write` are the bus: a read of `$C010` clears the strobe, a read of
`$0400` answers whichever bank the switches select. A harness needs to look
without touching, so these do not go through it.

`readRam(bank, address, out, length)` and `writeRam(bank, address, data, length)`
reach RAM by physical bank, whatever RAMRD, RAMWRT, ALTZP, 80STORE, PAGE2 and the
language card's switches select. They move nothing: no soft switch, no strobe, no
CPU cycle, no bus watch.

| `RamBank` | Addresses | The cells |
|---|---|---|
| `Main` | `$0000-$BFFF` | motherboard RAM |
| `Aux` | `$0000-$BFFF` | auxiliary 64 KiB, //e memory only |
| `LanguageCard1`, `LanguageCard2` | `$D000-$DFFF` | the card's two 4 KiB banks, as `$C08x` names them |
| `LanguageCardHigh` | `$E000-$FFFF` | its shared upper RAM, which has no bank |
| `AuxLanguageCard1`, `AuxLanguageCard2`, `AuxLanguageCardHigh` | the same | the same in auxiliary memory, //e memory only |

A range that is empty or leaves its bank is refused whole, with nothing copied and
the reason in `lastError()`: `$C000` and above are not `Main`, because a program
reaches those addresses through the language card's own names. The `$C000-$CFFF`
I/O space and ROM are not RAM and are not offered. `writeRam` is a debugger's
write. It lands in the cell as a CPU store never could, and neither a card nor the
beam-time video log sees a bus cycle for it; a later `renderFrame` does show it.

`keyboardState()` returns the latched character, the strobe and the count of paste
bytes queued behind it, and acknowledges nothing: reading it clears no strobe and
promotes no byte. **A key was taken once `strobe` and `queued` are both clear**,
which a host can wait for under a cycle budget instead of guessing with a delay or
a stable screen. That says the guest cleared the strobe, not what it did with the
key: a program that empties the keyboard before its prompt takes a key too. `queueKey`
takes a byte, so ESC (`$1B`) and RETURN (`$0D`) need no escaping.

## Hard disk

`attachHardDisk(slot)` installs the machine's own hard disk device, with two
empty units:

| Machine | Device | Units | `bootSlot` enters |
|---|---|---|---|
| ][+, //e, `Custom` | `ProDOSHardDiskCard` in an empty `slot` (7 by default), synthetic ROM, no file read | ProDOS drive 1 and drive 2 of the slot | the card's `$Cn00` |
| //c, 32 KiB ROM | two HDV units on the rear SmartPort connector; `slot` is ignored | units of the built-in slot 5 | the //c's own `$C500`, which finds them over the bus |
| //c, 16 KiB rev-255 ROM | the same | the same | the card's page, armed by the boot: this ROM has no SmartPort firmware |
| //c+ | two HDV units on the rear connector, behind its 3.5" drives; `slot` is ignored | numbered by its firmware, below | the ROM's reset scan: the internal 3.5" if it holds a disk, else the rear port |

`hardDiskSlot()` says where it landed, 0 for nowhere. On a //c it plugs a Disk II
in slot 6 if none is attached, because the port is decoded off that card's soft
switches and the firmware's bus probe gets no answer without it. A slot-5
`attachSmartPort` and a //c's hard disk exclude each other: each wants slot 5.

A //c+ numbers its chain from the internal 3.5", device 1 and ProDOS drive 1,
whether or not it holds a disk. So unit 0 is **drive 2** of slot 5, and booting it
hands the boot sector X = `$D0`, which needs the internal bay empty. An external
3.5" (`insertSmartPortDisk(1, …)`) joins the chain before the rear port and takes
drive 2 for itself. A ProDOS driver call then cannot name the hard disk, and
neither can it name unit 1 at any time: those stay reachable by native SmartPort
calls only.

`bootSlot(hardDiskSlot())` boots through that firmware. Nothing is injected into
RAM: the firmware reads block 0 to `$0800` and enters `$0801` with X naming the
slot and drive, and ProDOS and its `.SYSTEM` program take it from there. As for
every `bootSlot`, success means the firmware was entered, not that the guest
finished booting, and an empty unit is the firmware's to report.

**Media.** `insertHardDisk(unit, path)` takes a raw ProDOS-order file (`.hdv`,
`.po`) or a ProDOS-order 2IMG, told apart by the file's magic and never by its
name. A `.2mg` is a container, so it implies no 3.5" geometry. Block order cannot
be detected in a raw file: a DOS-order `.dsk` mounts, and reads as garbage. A
2IMG's data offset and length are honoured, and its header and trailer survive
every write-back. Refused, with the reason in `lastError()`, the file left alone
and the outgoing volume still mounted:

- a file that cannot be read, is empty, or exceeds the 64 MiB read limit;
- a 2IMG whose header is cut short, whose data window leaves the file or overlaps
  its header, whose format is not ProDOS, or whose non-zero block count disagrees
  with its data length (a block count of 0 is accepted);
- a capacity that is not a whole number of 512-byte blocks, or above 65536.

**Capacity.** `hardDiskBlockCount(unit)` is the real size. A ProDOS volume
declares its size in 16 bits, so **65535 blocks** (33 553 920 bytes, last block
`$FFFE`) is the largest one a guest can fully describe, and on it block `$FFFF`
fails with `$27` rather than wrapping. A physical image of **65536 blocks**
(exactly 32 MiB, as real dumps are) is also accepted: its STATUS is held at
`$FFFF`, block `$FFFF` stays addressable, and only `hardDiskBlockCount` reports
65536. 65537 is refused. Nothing is truncated.

**Writes** land in the unit's in-memory volume and reach the file on
`flushHardDisk(unit)` (temp file + rename, whole-file atomic), on an insert over
it, on an eject, and when the `Core` is destroyed or move-assigned over.
`hardDiskUnsavedBlocks(unit)` counts what is pending. Inserting the same file
again saves first and then reads the result. A failed save on a flush, insert,
eject or `setHardDiskWriteProtected` reports the error, leaves the volume mounted
and keeps its blocks pending for a retry; only the destructor has nobody to tell,
so flush first when the host must observe it. There is no background autosave:
the facade has no threads, and until a flush the file is exactly as it was. A host
that needs to keep a disk pristine mounts a private copy.

**Protection.** A guest write to a protected volume fails with `$2B`, and neither
the volume nor its file changes. `hardDiskWriteProtected(unit)` reports what the
guest sees, whatever the cause: a locked 2IMG, a read-only file (both found at
mount), `POM2_MEDIA_WRITE_DEFAULT=protected`, or
`setHardDiskWriteProtected(unit, true)`, a sticker that first saves pending writes
and lasts until the volume is ejected or replaced. Lifting the sticker cannot get
past the other three.

**Timing.** There is no mechanical model — no seek, rotation or spin-up — and no
wall-clock input. On the card a block costs the cycles of the slot ROM's copy loop
against `$C0n2`. On a 32 KiB //c it costs the cycles of the firmware's own bus
protocol. One driver READ, entry to return, measured 2026-11-04 and not pinned:
7746 cycles on the card, 19783 on the //c. Run the same script with the same ROMs,
volume and keystrokes, and the guest reaches the same RAM on the same cycle
(pinned, through a real ProDOS boot). Compare cycle counts between the two devices
only knowing this: the difference belongs to the device model, not to the disk.

`pom2_core_hdd` pins all of the above through guest calls: JSR to the entry `$CnFF`
names, and CREATE/OPEN/WRITE/CLOSE/READ through a real ProDOS MLI on a 65535-block
2IMG, with the catalog and data then read back out of the host file.

## Mockingboard and cassette

`attachMockingboard` accepts multiple cards in different empty slots. The
complete audio pull mixes all attached cards; `setMockingboardVolume` applies
to every attached card, not only the last one. The public models are A/C and
Sound II; the desktop card catalogue is not the facade's attachment API.

`saveCassette` passes its path directly to the cassette device: `.wav` selects
WAV and other suffixes select ACI. It does not append a suffix as the desktop
CLI does. Supply `.aci` or `.wav` explicitly when saving through the SDK.

## Rendering

`renderFrame()` paints with `Apple2Display` in its default mode, `ColorNTSC`: POM2's
port of MAME's composite colour table, on the CPU, into a 280- or 560-wide
framebuffer. It shares with the application the class, the memory it reads, FLASH
derived from the emulated frame, and the text and hi-res painters of that mode.

It is **not** what the application shows by default. The application's default
display is `ColorCompositeOE`: the bitstream demodulated by POM2's OpenEmulator-style
GLSL shader, then the CRT glass pass (scanlines, mask, vignette, phosphor gamma,
`CrtEffectStack`). Both need a GL context, the facade has none, and the facade offers
no call to select another mode, so neither runs here:

| Picture | Application | `renderFrame()` |
|---|---|---|
| MAME composite table (`ColorNTSC`) | selectable | **this** |
| OpenEmulator composite (GPU, or `ColorCompositeOECpu`) | default | no |
| CRT glass pass | default | no |
| AppleWin NTSC, monochrome phosphors | selectable | no |
| RGB card picture | when the card is plugged | no: composite, see § Display state |

The capabilities say the same: `render.cpu-composite` is present and there is no
`render.crt`. Use a frame as technical evidence (this mode changed, these two frames
match, this region holds what the guest drew). It does not stand for the look a
player gets on the default display, and a judgement of an image belongs in the
application or a tool built to show that pipeline.

## Delivery

`tools/package_core_sdk.sh <build-dir> <out-dir>` makes one
`pom2-core-sdk-v<version>-<host>.tar.gz`, for `darwin-arm64` or `linux-x86_64`:

- `include/pom2/core.hpp`, `lib/libpom2_core.a` and `lib/cmake/pom2_core/`;
- the conversion package below;
- this document;
- `MANIFEST.json`: version, commit, host, capabilities, system dependencies, and
  the SHA-256 of every other file in the tarball.

Its commit and capabilities come from running the installed archive, so they
describe those bytes. It refuses a tree with uncommitted changes unless given
`--allow-dirty`, and the manifest then says `-dirty`. The `core-sdk` job of
`release.yml` builds both hosts and attaches them to each tagged release.

`Core::sdkInfo()` gives a running host the same three facts. `commit` is re-read on
every build, with `-dirty` for uncommitted changes and `unknown` outside a Git
checkout. A capability name is added with its API and never reused:

| Capability | API |
|---|---|
| `cassette` | `loadCassette` and the transport |
| `disk2` | `attachDiskII`, `insertDisk`, `bootDisk` |
| `ezcolor` | `attachEzColor` and its calls |
| `harddisk` | `attachHardDisk` and its calls |
| `keyboard.state` | `keyboardState` |
| `machine.iic`, `machine.iicplus` | `Machine::AppleIIc`, `Machine::AppleIIcPlus` |
| `mockingboard` | `attachMockingboard` |
| `mouse` | `attachMouse`, `moveMouse`, `setMouse` |
| `provenance` | `sdkInfo`, `sha256`, `sha256File` |
| `ram.banks` | `readRam`, `writeRam` |
| `render.cpu-composite` | `renderFrame`, as § Rendering describes |
| `rgbcard` | `attachRgbCard` and its calls |
| `smartport35` | `attachSmartPort` and its calls |

**System dependencies**, for linking: libslirp and zlib everywhere, plus CoreAudio,
AudioToolbox and AudioUnit on macOS, or libdl, pthread and libm on Linux.
`find_package(pom2_core)` finds them. miniaudio opens no device on a headless run.

**ROMs**: none ship, and the facade reads no file it was not handed.

| Machine | `loadRom` | `loadCharacterRom` |
|---|---|---|
| `AppleIIPlus` | `apple2p.rom` | `apple2_char.rom` |
| `AppleIIe` | `apple2e_unenh.rom` | `apple2e_char_us_unenh.rom` |
| `AppleIIeEnhanced` | `apple2e.rom` | `apple2e_char.rom` |
| `AppleIIc` | `apple2c-32Kv0.rom` or `apple2c-16K.rom` | `apple2e_char.rom` |
| `AppleIIcPlus` | `apple2cp.rom` | `apple2e_char.rom` |

The hard disk, SmartPort and E-Z Color take no ROM. A Disk II's are optional
(`disk2.rom`, `diskii_p6.rom`); `attachMouse` reads `mouse_341-0270-c.bin` and, for
the MCU model, `mouse_341-0269.bin`. `tools/fetch_test_media.py` is how this
repository's tests get them.

**Recording what ran.** A test records `sdkInfo()`, `sha256File` of each ROM and
volume, taken before mounting since a volume changes as the guest writes, and
`sha256` of `readRam` for the program in memory.

## Image conversion package

`pom2_imageconv` is the conversion half of `hgrpaint`, which owes nothing to the
emulator: a program that turns images into HGR or DHGR pages needs this and no POM2
checkout.

```bash
cmake --build build --target pom2_imageconv
cmake --install build --prefix /tmp/pom2-sdk --component pom2_imageconv_sdk
```

```cmake
find_package(pom2_imageconv CONFIG REQUIRED)
target_link_libraries(my_converter PRIVATE POM2::imageconv)
```

The target's include paths give `"hgrpaint/HgrConvert.h"`, `"hgrpaint/HgrPaintModel.h"`,
`"Cam16.h"` and the bundled `"stb_image.h"` and `"stb_image_write.h"`, with the same
names a tool compiled against the sources uses. It has no dependency beyond C++17. Its
API follows the sources and carries no stability promise of its own; version
compatibility is `SameMinorVersion`, as for the core. It runs in CI as the
`pom2_imageconv_consumer` ctest, from `examples/pom2_imageconv_consumer/`.

## What ships

The installable half **landed** with the core-boundaries work (`77a57c9`) and is
in this tree:

- **`pom2_core`**, a static archive built from `POM2_CORE_SOURCES`, with
  **`POM2::core`** as its in-tree alias. `EXPORT_NAME core` makes the *installed*
  package spell it `POM2::core` as well — without that a consumer would have to
  write `POM2::pom2_core` and the two spellings would diverge silently.
- **`install(EXPORT pom2_coreTargets ... NAMESPACE POM2::)`** plus a
  `configure_package_config_file` / `write_basic_package_version_file` pair, so
  `find_package(pom2_core)` works against an install tree. Version compatibility
  is `SameMinorVersion`.
- Everything is one install **component**, `pom2_core_sdk`: `cmake --install
  --component pom2_core_sdk` gives a consumer the archive, the headers and the
  package files and nothing else — no ROMs, no fonts, no emulator binary. The
  conversion package is its own component, `pom2_imageconv_sdk`, and
  `tools/package_core_sdk.sh` delivers the two together (§ Delivery).
- **`examples/pom2_core_consumer/`** is a standalone project (its own
  `CMakeLists.txt` + `main.cpp`) that `find_package`es the installed package and
  links `POM2::core`. It runs in CI as the **`pom2_core_sdk_consumer`** ctest,
  which is what makes the export contract a tested claim rather than a written
  one. It is deliberately excluded from `tools/coverage.sh`: it links a separate
  project against the installed archive with plain flags, which cannot work
  against an instrumented build, and it measures the export contract rather than
  POM2's code.
- `pom2_core_test` is the same source list built for the test binaries. When
  libslirp is found it carries `POM2_HAVE_SLIRP` alongside `pom2_core`, so the
  tests and the SDK compile the network backend that actually ships rather than
  its stub.

## What is deliberately not here

For the installed-consumer recipe, see [Building a consumer](#building-a-consumer).

- **No shared library, and no stable ABI.** `pom2_core` is a static archive
  compiled by the consumer's toolchain against the same C++17 headers. There is
  no versioned `.so`/`.dylib`/`.dll`, no symbol-visibility map, and no promise
  that two POM2 releases are binary-compatible — `SameMinorVersion` on the
  package file is a *source*-compatibility statement.
- **No C API and no language bindings.** The facade is C++17 only.
- **No package-manager recipe.** There is no vcpkg port, Conan recipe or system
  package for `pom2_core`; `vcpkg.json` in the root describes POM2's own
  dependencies, not a published POM2 package.
- **No registry of external consumers and no compatibility matrix.** That is not
  proof no external user exists, but it is not evidence for maintaining several
  public binary interfaces either. Until one appears, the policy stays: one
  facade, one archive, source compatibility, and the consumer example as the
  only contract that must keep passing.

## Building a consumer

After configuring POM2 using the [build guide](USER-GUIDE.md#build-from-source),
build and install only the SDK component, then configure the standalone example:

```sh
cmake --build build --config Release --target pom2_core --parallel 3
cmake --install build --config Release --prefix /tmp/pom2-sdk --component pom2_core_sdk
cmake -S examples/pom2_core_consumer -B /tmp/pom2-core-consumer -DCMAKE_PREFIX_PATH=/tmp/pom2-sdk
cmake --build /tmp/pom2-core-consumer --config Release
```

Use writable staging paths appropriate to the host. The example links
`POM2::core` through `find_package(pom2_core CONFIG REQUIRED)` and needs no
ROM to exercise basic RAM, stepping, framebuffer and audio API calls. Loading
and booting actual Apple II software requires the embedding host to supply
its firmware and media through the facade.
