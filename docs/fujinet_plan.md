# FujiNet in POM2 — implementation plan

**Status: ALL PHASES SHIPPED (2026-08-10).** Both transports, the card, the
panel, the CLI flags and four pinned tests are in the tree — see `DEV.md`
§ FujiNet for what the code actually does. Phase 2 shipped partially (two
items deliberately dropped — [§ 7](#7-phase-2--pom2-native-integrations)) and
Phase 3 shipped by a different route than proposed
([§ 8](#8-phase-3--helper-process-and-why-not-embedded-firmware)); both are
recorded as ✅ DONE in `TODO.md`.

This document was the design + work-breakdown, and is kept as the rationale
record: why a relay rather than a port, why blocking on the CPU thread is the
right answer, what rewind cannot do, and the manual test passes that CI cannot
run. **What changed during implementation** is recorded in
[§ 15](#15-what-implementation-changed).

## Table of contents

- [1. Scope and non-goals](#1-scope-and-non-goals)
- [2. What FujiNet is, in POM2 terms](#2-what-fujinet-is-in-pom2-terms)
- [3. Reference material and licensing](#3-reference-material-and-licensing)
- [4. Architecture](#4-architecture)
- [5. File layout](#5-file-layout)
- [6. Phase 1 — the relay](#6-phase-1--the-relay)
- [7. Phase 2 — POM2-native integrations](#7-phase-2--pom2-native-integrations)
- [8. Phase 3 — helper process (and why NOT embedded firmware)](#8-phase-3--helper-process-and-why-not-embedded-firmware)
- [8b. Embedded firmware (rejected)](#8b-embedded-firmware-rejected)
- [9. Threading and the frame-hitch budget](#9-threading-and-the-frame-hitch-budget)
- [10. Reset, snapshot and rewind semantics](#10-reset-snapshot-and-rewind-semantics)
- [11. Pinned tests](#11-pinned-tests)
- [12. Documentation duties](#12-documentation-duties)
- [13. Risks and gotchas](#13-risks-and-gotchas)
- [14. Effort and ordering](#14-effort-and-ordering)
- [15. What implementation changed](#15-what-implementation-changed)

## 1. Scope and non-goals

**In scope.** A new slot card, `fujinet`, that presents a SmartPort controller
to the guest and relays every SmartPort call to an external FujiNet, using the
project's published **SP-over-SLIP** protocol. That single protocol carries
*all* FujiNet functions, because on the Apple II every FujiNet device — block
storage, the `N:` network device, clock, printer, modem, CP/M — is a SmartPort
unit.

**Two transports, both in Phase 1:**

- **TCP** — POM2 listens on `127.0.0.1:1985`; a FujiNet *desktop build* running
  alongside connects in. The everyday case.
- **USB CDC-ACM serial** — POM2 opens a serial device (`/dev/ttyACM0`,
  `/dev/cu.usbmodem*`, `COM3`) and talks to a **physical FujiNet board** over
  its USB port. The spec explicitly blesses this ("any medium providing a
  transparent, duplex, lossless byte stream"), and it is what makes POM2 able
  to drive real hardware rather than only a software peer.

**Not in scope.**

- Reimplementing the FujiNet firmware's network stack (TNFS/HTTP/SSH/JSON…)
  natively in POM2. That is tens of thousands of lines and buys nothing: the
  relay gets the real thing.
- The Disk II side of FujiNet (WOZ served over the 5.25" bus). Real FujiNet
  does this by driving the Disk II connector; it is read-only, and POM2 already
  mounts WOZ directly.
- //c-class machines in Phase 1 (see [13](#13-risks-and-gotchas)).
- Emulating FujiNet's ESP32.

**Non-goal that is worth stating explicitly:** POM2 does not become a FujiNet
*host*. It becomes a machine that a FujiNet can be plugged into — exactly the
relationship a real //e has with a real FujiNet.

## 2. What FujiNet is, in POM2 terms

FujiNet is an ESP32 board that hangs off the Apple II SmartPort bus and answers
SmartPort commands. Its distinguishing feature is the **`N:` network device**:
a unit whose `Open`/`Read`/`Write`/`Close`/`Status`/`Control` calls carry URLs
and payloads, so an 8-bit machine gets TCP, UDP, HTTP(S), TNFS, FTP, SMB, SSH,
Telnet, WebDAV plus JSON/XML parsing without running a single byte of TCP/IP
itself.

Everything else it offers — SmartPort block devices (PO/HDV/2MG), a ProDOS
clock, a printer, a modem, and a RunCPM environment — is also addressed as
SmartPort units. **One transport, all functions.** That is what makes a relay
worth building rather than a per-device port.

Guest-side software (`fujinet-apps`, `fujinet-lib`, the `CONFIG` program, the
BBS/telnet/weather/high-score clients) runs unmodified: it only ever issues
SmartPort calls.

## 3. Reference material and licensing

| Source | Use |
|---|---|
| [SP-over-SLIP spec (wiki)](https://github.com/FujiNetWIFI/fujinet-firmware/wiki/Apple-II-SP-over-SLIP) | **Normative.** Framing, sequence numbers, all 10 request/response layouts. |
| [`FujiNetWIFI/AppleWin`](https://github.com/FujiNetWIFI/AppleWin) | Working reference implementation in an Apple II emulator: `source/SmartPortOverSlip.{h,cpp}`, `source/devrelay/**`, `firmware/SPoverSLIP/spoverslip.s`. |
| [`fujinet-go-apple2-desktop`](https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop) | Proof of the end-to-end product shape (AppleWin core + firmware as a shared library over loopback TCP 1985, card in slot 7). |
| [`fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware) | The desktop build the user runs alongside POM2 (the old `fujinet-pc` repo is archived and folded in here). |
| Apple IIc Technical Reference ch. 6; Apple IIgs Firmware Reference ch. 7 | The SmartPort call convention the spec supplements. |

**Licensing.** POM2 is GPLv3. `fujinet-firmware` is GPLv3. The AppleWin files
carry *"GPL v2 … or (at your option) any later version"*, so lifting code from
the fork into POM2 is legally clean. **Recommendation: implement clean-room
from the wiki spec anyway.** The spec is complete, the AppleWin `devrelay/`
tree is verbose and shaped around AppleWin's globals (`mem[]`, `regs`,
`memdirty[]`), and POM2's one-concern-per-file rule wants a different cut.
Cite the spec and the AppleWin file/line in comments the way the project cites
MAME.

**Note on the "MAME = source of truth" convention:** it does not apply here.
MAME has no FujiNet device. The source of truth is the published protocol
spec plus the AppleWin fork, and every ported file must say so in its header
comment so the deviation is deliberate and visible.

## 4. Architecture

### 4.1 Four layers

```
  guest 6502              POM2 host                              external
  ──────────              ─────────                              ────────
  JSR $Cn63    FujiNetCard      SpOverSlipLink    SpTransport
  (SP call)    ├ synth. ROM     ├ seq numbers     ├ TcpTransport ─► FujiNet
       │       ├ $C0n2 trap     ├ 250 ms timeout  │  (listen :1985)  desktop
       │       ├ RAM marshal    ├ enumeration     └ SerialTransport─► FujiNet
       ▼       └ A/X/Y/flags    └ SlipFramer         (CDC-ACM)        board
  STA $C0n2 ───────► trap ───────► request ──────────────────────────► device
  RTS      ◄──────── result ◄───── response ◄─────────────────────────  device
```

- **`SlipFramer`** — pure function object: bytes ⇄ SLIP frames. No I/O, no
  threads, no protocol knowledge. Trivially unit-testable.
- **`SpTransport`** — abstract byte pipe: `read` / `write` / `close` /
  `isOpen`, with a poll-with-timeout. Two implementations, `SpTcpTransport`
  and `SpSerialTransport`. This is the seam that makes the serial path a
  drop-in rather than a fork of the whole link layer — and it mirrors the
  AppleWin fork's own `Connection` / `TCPConnection` / `COMConnection` split.
- **`SpOverSlipLink`** — session: allocates request sequence numbers, does the
  bounded-timeout round trip, enumerates units when a peer appears, owns the
  connection-management worker thread. Knows nothing about the 6502 and
  nothing about *which* transport it holds.
- **`FujiNetCard`** — `SlotPeripheral`: synthesises the slot ROM, traps the
  magic write, marshals between emulated RAM/CPU state and `SpOverSlipLink`
  requests. Knows nothing about sockets, serial ports or framing.

This is the project's one-concern-per-file rule applied to the obvious seams,
and it makes each hard part independently testable: framing without I/O,
protocol without a CPU, CPU without a peer, and each transport against a
loopback of its own kind.

### 4.2 Where it sits on the LLE/HLE scale

Per [`docs/lle_vs_hle.md`](lle_vs_hle.md): **H1 (synthetic firmware) + relay to
a real external device.** POM2 hand-assembles the slot ROM (like
`ProDOSHardDiskCard` and `PrinterCard`), and behind the invented register
protocol sits not a host implementation but a *genuine FujiNet*. The failure
mode is therefore neither "incomplete LLE hangs" nor "out-of-contract HLE
diverges" — it is "peer absent or slow", which is a new third category and
should be documented as such.

Add a master-table row:

```
| **FujiNet card** (`FujiNetCard`) | **H1 + relay** | Synthetic 256 B slot ROM
whose only job is to trap into the host; every SmartPort call is forwarded
verbatim to a real FujiNet over SP-over-SLIP | Nothing below the protocol
exists to model — the device is real and off-box |
```

*Discharged — the row is in `docs/lle_vs_hle.md`'s master table.*

### 4.3 One SmartPort call, end to end

1. Guest: `JSR` to the SmartPort entry — the driver sits at `$Cn60` (found
   via `$CnFF`), SmartPort entry = ProDOS entry + 3, i.e. `$Cn63`
   (`FujiNetCard.cpp:220`) — followed inline by `cmd` byte and a 2-byte
   pointer to the parameter list.
2. ROM at `$Cn63` jumps to the driver body, which is three instructions:
   `LDA #$65` / `STA $C0n2` / `CMP #$01` / `RTS`.
3. `SlotBus` dispatches the write → `FujiNetCard::deviceSelectWrite(0x2, 0x65)`.
4. Card reads the pushed return address from page 1, decodes `cmd`,
   `param_count`, `unit_number`, `payload_loc` from emulated RAM.
5. Card **rewrites the return address on the stack**, advancing it by 3 so the
   ROM's `RTS` lands after the inline bytes.
6. Card builds the SP-over-SLIP request, calls `SpOverSlipLink::transact()`.
7. Response data is written back into emulated RAM at `payload_loc`;
   `A` = status, `X`/`Y` = transfer length, Z/C flags set from status.
8. ROM's `CMP #$01` / `RTS` returns to the guest with the carry already right.

The whole thing is invisible to the guest: it looks exactly like a fast
SmartPort controller.

## 5. File layout

New files (all under `src/`, one concern each):

| File | Lines (est.) | Responsibility |
|---|---|---|
| `SlipFramer.h` | ~120 | SLIP encode/decode, incremental decoder with truncation detection. Header-only is fine. |
| `SpTransport.h` | ~90 | Abstract byte pipe + the `SpTcpTransport` declaration. |
| `SpTcpTransport.cpp` | ~180 | Listening socket, accept, per-connection read/write. |
| `SerialPort.h` / `.cpp` | ~330 | **New host primitive:** cross-platform raw serial port (POSIX `termios` / Win32 `DCB`), device enumeration, DTR/RTS control. The serial sibling of `SocketCompat.h`, reusable beyond FujiNet. |
| `SpSerialTransport.cpp` | ~140 | `SpTransport` over a `SerialPort`, plus device-node presence polling for hot-plug. |
| `SpOverSlipLink.h` / `.cpp` | ~430 | Session: sequence numbers, `transact()` with timeout, unit enumeration (`Status`/DIB sweep), worker thread, transport selection. |
| `FujiNetCard.h` / `.cpp` | ~700 | `SlotPeripheral`; ROM builder, `$C0n2` trap, SmartPort + ProDOS marshalling, snapshot hooks. |
| `FujiNet_ImGui.h` / `.cpp` | ~280 | Panel: transport selector, link state, peer, enumerated units, last error, counters. |

New tests under `tests/`:

| File | What it pins |
|---|---|
| `slip_framer_test.cpp` | Escaping of `$C0`/`$DB`, round trip, truncated-frame rejection. |
| `sp_over_slip_link_test.cpp` | Round trip against an in-process fake TCP peer; stale-response discard; timeout path. |
| `serial_port_test.cpp` | POSIX pty loopback: raw mode round trip, timeout, DTR/RTS left de-asserted on open. Skipped on Windows (no pty). |
| `fujinet_card_smoke_test.cpp` | ROM signature bytes; a real 6502 executing the boot path and a `JSR` to the SmartPort entry (`$Cn63` = driver + 3) against a fake peer; stack fixup; A/X/Y/flags. |

Modified files:

| File | Change |
|---|---|
| `SlotCardCatalog.h` | Add `{ "fujinet", "FujiNet (SP over SLIP)" }` with the usual rationale comment. |
| `MainWindow.cpp` | `plugFujiNet(s)` lambda next to `plugUthernetII` / `plugSmartPort35`; dispatch entry in the `kind ==` chain (`MainWindow_SlotConfig.cpp:668`). |
| `MainWindow.cpp` (settings) | Per-slot keys `fujinet_port<sk>`, `fujinet_enabled<sk>`, `fujinet_timeout_ms<sk>` — same `+ sk` suffix pattern — plus `fujinet_helper_path<sk>` (`MainWindow_Session.cpp:247` (write) / `MainWindow_SlotConfig.cpp:422` (read)). |
| `MainWindow.cpp` (panels) | Register the FujiNet panel in the DockSpace + View menu. |
| `CliDispatcher.cpp` | `--fujinet[=port]` → plug the card into the first free slot (or slot 7) and start listening. |
| `tests/CMakeLists.txt` | Four `add_executable` + `add_test` blocks, modelled on `test_smartport_card` (`tests/CMakeLists.txt:2066-2086`); `serial_port_test` guarded to non-Windows. |
| `CLAUDE.md`, `DEV.md`, `TODO.md`, `docs/lle_vs_hle.md`, `CHANGELOG.md` | See [12](#12-documentation-duties). |

## 6. Phase 1 — the relay

### 6.1 `SlipFramer`

Straight from the spec:

- Frame delimiter `$C0`. Inside the payload, `$C0` → `$DB $DC`, `$DB` → `$DB $DD`.
- Decoder is incremental (fed from `recv()` chunks), emits complete frames, and
  reports a *truncated* frame (delimiter arriving mid-escape) as an error
  rather than silently accepting it — the spec calls this out as the mechanism
  for detecting an Apple II reset during transmission.

No allocation on the hot path: encode into a caller-provided `std::vector<uint8_t>`.

### 6.2 `SpTransport` and the TCP transport

The interface is deliberately tiny — everything above it only ever needs a
byte pipe:

```cpp
class SpTransport {
public:
    virtual ~SpTransport() = default;
    virtual bool isOpen() const = 0;
    /// Blocking write of the whole buffer. false = link died.
    virtual bool writeAll(const uint8_t* p, size_t n) = 0;
    /// Read up to `n` bytes, waiting at most `timeoutMs`.
    /// >0 = bytes read, 0 = timeout, <0 = link died.
    virtual int  readSome(uint8_t* p, size_t n, int timeoutMs) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;   // for the panel/logs
};
```

**TCP direction.** POM2 **listens**; the FujiNet connects in. This matches the
AppleWin fork (`devrelay/service/Listener.cpp`: *"Creates a Connection object,
which is how SP device(s) will register itself with our listener"*) and
`fujinet-go-apple2-desktop` (loopback TCP **1985**). Keep 1985 as the default
so an existing FujiNet configuration works against POM2 untouched.

Accept lives on the link's worker thread using `pollAcceptOnce` from
`SocketUtil.h` (200 ms poll), so shutdown never hangs the UI thread — the
macOS shutdown-vs-accept trap the `SuperSerialCard` header documents applies
verbatim. `SocketCompat.h` covers POSIX/Winsock.

### 6.3 `SerialPort` and the USB CDC-ACM transport

This is a **new host primitive** for POM2 — there is no serial code in the tree
today (`SuperSerialCard` emulates a 6551 and bridges it to *TCP*, it never
opens a host serial device). `SerialPort` is to serial what `SocketCompat.h` is
to sockets: one compat header/impl pair, no FujiNet knowledge, reusable later
(a real modem on the SSC, ADTPro over a serial cable, a hardware Disk II
emulator).

**API.**

```cpp
class SerialPort {
public:
    struct Info { std::string path, description; };
    static std::vector<Info> enumerate();          // candidate devices

    bool open(const std::string& path, int baud = 115200);
    bool isOpen() const;
    void close();
    bool writeAll(const uint8_t* p, size_t n);
    int  readSome(uint8_t* p, size_t n, int timeoutMs);
    void setDtr(bool on);
    void setRts(bool on);
};
```

**POSIX implementation** (Linux + macOS): `::open(path, O_RDWR | O_NOCTTY |
O_NONBLOCK)`, then `tcgetattr` → `cfmakeraw` → `tcsetattr`. Raw mode is
non-negotiable: SLIP frames contain `$11`/`$13`, which `IXON` software flow
control would eat, and `$0D`/`$0A`, which `ICRNL`/`ONLCR` would rewrite.
`VMIN = 0`, `VTIME = 0` with `poll()` doing the waiting, so `readSome`'s
timeout is honoured to the millisecond. Baud is meaningless for CDC-ACM (the
USB pipe runs at USB speed regardless) but a value must be set anyway; 115200
is the conventional choice.

**Win32 implementation:** `CreateFileA("\\\\.\\COM3", …)`, `GetCommState` →
`DCB` with `fBinary = TRUE`, no parity, 8N1, **`fOutxCtsFlow`/`fOutxDsrFlow`/
`fDtrControl`/`fRtsControl` all disabled**, then `SetCommTimeouts` with
`ReadIntervalTimeout`/`ReadTotalTimeoutConstant` derived from the caller's
timeout. Note the `\\.\` prefix is required for `COM10` and above — a classic
Win32 trap.

**The ESP32 auto-reset gotcha — the single most important detail here.**
FujiNet is an ESP32 board, and every ESP32 dev-board USB bridge wires **DTR and
RTS to `EN` (reset) and `IO0` (boot mode)** through the standard two-transistor
auto-reset circuit — that is exactly how `esptool` puts the chip into its
bootloader without a button press. A naive `open()` that lets the OS assert DTR
and RTS at their defaults will therefore **reboot the FujiNet, or drop it into
the ROM bootloader, every time POM2 opens the port.** So:

- De-assert DTR and RTS explicitly *before* the first byte, on both platforms.
- On POSIX, clear `HUPCL` in the termios flags, otherwise closing the port
  drops DTR and resets the board on POM2 exit.
- Pin this in `serial_port_test` (assert the modem-control lines are low after
  `open()`), because the failure mode — "my FujiNet reboots whenever I start
  the emulator" — is baffling from the user's side.

**Device enumeration.** Linux: glob `/dev/ttyACM*` and `/dev/ttyUSB*`, and
prefer entries under `/dev/serial/by-id/` containing `FujiNet` or the bridge
chip name when present (stable across replug, unlike `ttyACM0`). macOS:
`/dev/cu.usbmodem*` and `/dev/cu.usbserial*` — **`cu.` not `tty.`**, since
opening `tty.*` blocks waiting for carrier detect. Windows: enumerate
`HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`. The panel shows the list; the user picks,
or POM2 takes the single candidate if there is exactly one.

**Hot-plug.** No `accept()` to wait on, so the worker thread instead polls for
the device node every ~500 ms while disconnected, opens it when it appears, and
tears the link down on the first `readSome` returning "link died" (unplug shows
up as `ENXIO`/`EIO` on POSIX, `ERROR_BAD_COMMAND`/`ERROR_DEVICE_REMOVED` on
Win32). Re-enumeration on reconnect is the same path a fresh TCP connection
takes.

**One more difference from TCP worth designing for:** a serial line has no
connection establishment, so there is no "peer just connected" event to hang
enumeration off. Treat *opening the port* as the connect event, and be ready
for the first `Status`/DIB sweep to time out because the board is still booting
— retry the sweep up to 3 times before declaring the link dead.

### 6.4 `SpOverSlipLink`

**`transact()`.** Called on the CPU thread. Sends the request through whichever
`SpTransport` is live, waits for a response with a bounded timeout (**default
250 ms**, configurable 50 ms – 5 s), decodes frames, and **discards any
response whose sequence number does not match the request** — the spec's stated
purpose for the sequence number, and the reason a reset mid-call cannot desync
the link permanently. On timeout it returns a failure the card turns into
SmartPort error `$27` (I/O error), never a hang.

The timeout matters more on serial than on TCP: loopback answers in
microseconds, a real board over USB in single-digit milliseconds, so 250 ms is
~50× headroom for the normal case while keeping a dead peer to a quarter-second
hitch. See [9](#9-threading-and-the-frame-hitch-budget).

**Enumeration.** When a peer appears (TCP accept, or serial port opened), sweep
`Status` with status code `$03` (DIB) over units to learn what it offers, and
cache `unit → (transport, remote device id)`. The special case
`Status(unit 0, code 0)` = "device count" is answered **locally** from that map,
exactly as AppleWin does — it must work before any peer exists so a scanning
guest sees "no devices" rather than an error.

**Threading.** One worker thread owns peer lifetime for both transports:
`pollAcceptOnce` when in TCP mode, device-node polling when in serial mode.
The live transport pointer is `std::atomic`-guarded so the CPU thread's
`transact()` never touches a half-torn-down peer.

**Mode is exclusive.** TCP *or* serial, not both — two peers would mean two
device-number spaces to merge, for no real use case. The panel's transport
selector switches modes and re-establishes.

### 6.5 The slot ROM

POM2 **synthesises** its own 256-byte ROM in a `buildRom()` following
`SmartPortCard::buildRom()` (`SmartPortCard.cpp:556`) and
`ProDOSHardDiskCard`. No third-party binary is shipped, no ROM dump is
required, and the H1 pattern is the house style.

Layout (offsets within `$CnXX`):

| Offset | Bytes | Meaning |
|---|---|---|
| `$00-$01` | `E0 20` (`CPX #$20`) | `$Cn01 = $20` — ProDOS signature |
| `$02-$03` | `A2 00` (`LDX #$00`) | `$Cn03 = $00` |
| `$04-$05` | `E0 03` (`CPX #$03`) | `$Cn05 = $03` |
| `$06-$07` | `E0 00` (`CPX #$00`) | `$Cn07 = $00` — SmartPort class |
| … | boot code | read block 0 of unit 0 into `$0800`, `JMP $0801`; on failure, continue the autostart scan via `$FABA` if `$00/$01` show we are in a slot scan, else print an error and drop to BASIC |
| `drv` | `38 B0 xx` (`SEC` / `BCS`) | **ProDOS entry**; SmartPort entry is `drv+3` (the Apple convention) |
| `drv+3` | `A9 65` | **SmartPort entry**: `LDA #$65` |
| | `8D n2 C0` | `STA $C0n2` — **the trap** |
| | `C9 01` | `CMP #$01` — turns `A != 0` into carry-set |
| | `60` | `RTS` |
| `$FE` | `$F7` | ProDOS capability byte (read/write/status/format, removable) |
| `$FF` | `drv` | ProDOS driver entry offset |

The ProDOS branch uses magic `$66` instead of `$65` through the same
`STA $C0n2`.

Three facts worth pinning in a comment:

1. The signature trio matches what `bootFromSlot` validates (`$Cn01 = $20`,
   `$Cn03 = $00`, `$Cn05 = $03`) and `$Cn07` is *not* validated by POM2 — so
   "Boot from slot N" works with no change to `EmulationController`.
2. Blocks-total at `$Cn00`-relative end is left `$00 $00` deliberately, which
   forces ProDOS to issue a `Status` call to learn the size — correct for a
   device whose media can change under us.
3. The card must be in a slot **scanned before Disk II** to auto-boot into
   FujiNet's `CONFIG`. `fujinet-go-apple2-desktop` uses **slot 7**; make that
   POM2's default suggestion too.

### 6.6 The trap and the marshalling

The one genuinely new thing this card needs, relative to every other POM2 slot
card: **access to emulated RAM and CPU registers.** `SlotPeripheral`
deliberately exposes neither.

There is already a precedent to copy — `SoftCardZ80` (`SoftCardZ80.h` (`setMemory`/`setCpu`))
takes `setMemory(Memory*)` and `setCpu(M6502*)`, injected by the host at plug
time (`MainWindow_SlotConfig.cpp:297-298`), and does all its bus work through
`Memory::memRead` / `memWrite` — *"the real bus"* — so paging is honoured.
`FujiNetCard` does the same:

```cpp
void setMemory(Memory* m) { mem_ = m; }
void setCpu(M6502* c)     { cpu_ = c; }
```

Going through `Memory::memRead/memWrite` rather than a raw RAM pointer is not
optional: a ProDOS buffer can live in aux under `80STORE`/`RAMRD`/`RAMWRT`, and
only the real dispatcher knows. **Guard rail:** refuse (and log) any payload
address in `$C000-$C0FF` — marshalling through the I/O page would toggle soft
switches as a side effect of a "memory" read.

Register access uses the existing `M6502` accessors: `getStackPointer`,
`getAccumulator`/`setAccumulator`, `setXRegister`, `setYRegister`,
`getStatusRegister`/`setStatusRegister`.

**SmartPort call decode** (`deviceSelectWrite(0x2, $65)`), following
`SmartPortOverSlip::handle_smartport_call`:

```
sp   = cpu_->getStackPointer()
lo   = mem_->memRead(0x0100 + ((sp + 1) & 0xFF))
hi   = mem_->memRead(0x0100 + ((sp + 2) & 0xFF))
rts  = lo | (hi << 8)                       // points at cmd_byte - 1
cmd          = memRead(rts + 1)
cmd_list_loc = memRead(rts + 2) | memRead(rts + 3) << 8
param_count  = memRead(cmd_list_loc)
unit_number  = memRead(cmd_list_loc + 1)
payload_loc  = memRead(cmd_list_loc + 2) | memRead(cmd_list_loc + 3) << 8
// stack fixup — skip the 3 inline bytes
rts += 3
memWrite(0x0100 + ((sp + 1) & 0xFF), rts & 0xFF)
memWrite(0x0100 + ((sp + 2) & 0xFF), rts >> 8)
```

**Note the difference from AppleWin:** its `regs.sp` is already a full `$01xx`
address, so it indexes `mem[regs.sp + 1]` directly. POM2's `getStackPointer()`
returns the 8-bit SP, so the `0x0100 +` and the `& 0xFF` page-1 wrap are
mandatory. This is exactly the kind of off-by-a-page that silently corrupts a
different program's stack; the smoke test must cover an SP near `$00`/`$FF`.

**Result marshalling.** `A` = SmartPort status (`$00` = OK), `X`/`Y` = byte
count where the command defines one (`X` low, `Y` high; `$00/$02` for a
512-byte block), Z set iff status is `$00`, C set iff status is non-zero (the
ROM's `CMP #$01` does that part, but set the flags anyway so a caller that
jumps straight into the driver body still sees a coherent state).

**Command coverage.** All ten: `Status $00`, `ReadBlock $01`, `WriteBlock $02`,
`Format $03`, `Control $04`, `Init $05`, `Open $06`, `Close $07`, `Read $08`,
`Write $09`. Extended (`$4x`) calls are rejected with `$01` (bad command) in
Phase 1, matching what `SmartPortCard::spExecute()` already does — the //e
never issues them.

### 6.7 The ProDOS entry

Magic `$66` on the same trap. Read the classic zero-page block:

| ZP | Meaning |
|---|---|
| `$42` | command (0 = status, 1 = read, 2 = write, 3 = format) |
| `$43` | unit — bit 7 = drive 2, bits 6-4 = slot |
| `$44/$45` | buffer address |
| `$46/$47` | block number |

Map drive 1 / drive 2 onto the peer's first two block units, issue
`ReadBlock`/`WriteBlock`/`Status`, and return ProDOS codes: `$00` OK, `$27` I/O
error, `$28` no device, `$2B` write-protected. `SmartPortCard.cpp` already
carries the correct codes and the rationale for why `$01` is wrong — reuse the
reasoning, not the code.

### 6.8 Host wiring

- **Catalog** (`SlotCardCatalog.h`): `{ "fujinet", "FujiNet (SP over SLIP)" }`,
  with a comment saying it needs an external FujiNet (desktop build over TCP,
  or a real board over USB) and no ROM dump.
- **Plug** (`MainWindow_SlotConfig.cpp`, beside `plugUthernetII` at `:348` —
  `plugFujiNet` sits at `:368`; the CLI/unlocked variants are
  `MainWindow_DevicePanels.cpp:786` / `:854`): construct, `setMemory` / `setCpu`, apply
  settings, plug on the bus, remember the pointer for the panel.
- **Settings**: per-slot suffix keys — `fujinet_transport` (`tcp` | `serial`),
  `fujinet_port`, `fujinet_serial_path`, `fujinet_serial_baud`,
  `fujinet_timeout_ms`, `fujinet_enabled`, plus `fujinet_helper_path`
  (`MainWindow_Session.cpp:247` / `MainWindow_SlotConfig.cpp:422`).
- **CLI** (`CliDispatcher`, pre-boot phase):
  - `--fujinet[=PORT]` — TCP mode, default 1985.
  - `--fujinet-serial[=DEV]` — serial mode; with no argument, auto-pick when
    exactly one candidate device is present, else list them and exit with an
    error rather than guessing.
  - `--fujinet-slot N` — override the default slot 7.
- **Multi-instance**: **no.** One card per machine, like the real thing. Follow
  the `hdv` precedent (single-instance keys), not the
  `cffa`/`smartport35` multi-instance list — a second listener on the same port
  or a second opener of the same device would just fail.

### 6.9 Panel

`FujiNet_ImGui` — small, modelled on `SmartPort_ImGui`:

- **Transport selector**: TCP (port field) / Serial (device combo populated by
  `SerialPort::enumerate()`, plus a Refresh button).
- Link state: *listening on :1985* / *connected (peer 127.0.0.1:xxxxx)* /
  *port open (/dev/ttyACM0)* / *waiting for device* / *idle*.
- Enumerated units: index, device type from the DIB, name string.
- Counters: calls, bytes in/out, timeouts, last error.
- Buttons: connect/disconnect, and a link to the FujiNet web UI opened in the
  host browser (TCP mode only — over serial there is no IP to point at, so grey
  it out rather than showing a dead link).

## 7. Phase 2 — POM2-native integrations

**Status: shipped 2026-08-10, partially — and the omissions are decisions,
not leftovers.** Item 1 landed. Items 2-4 were dropped after implementation
made their cost/benefit clear; the reasoning is recorded inline below.

Each of these is small and independent once Phase 1 lands.

1. ✅ **Printer unit → `ImageWriter`.** SHIPPED. One wrinkle the plan did not
   foresee: the unit cannot be identified by its DIB **type byte**, because
   the firmware's `iwmPrinter::create_dib_reply_packet`
   (lib/device/iwm/printer.cpp:32) sets `dib.type =
   SP_TYPE_BYTE_FUJINET_MODEM` — the printer advertises itself as a modem.
   That is an upstream copy-paste bug; POM2 keys on the DIB **name**
   ("PRINTER") and accepts the correct type byte as well, and the smoke test
   reproduces the bug so the workaround cannot be "cleaned up" by accident.
2. ❌ **Modem unit → the SSC telnet bridge.** DECIDED AGAINST. The plan already
   suspected this was duplication; implementing the relay settled it. FujiNet's
   modem unit *already* reaches the network through the FujiNet's own stack, so
   POM2 dialling out in parallel would not add reach — it would fight the
   connection state the guest believes it owns.
3. ❌ **Block units → `MountableMediaCard`.** DECIDED AGAINST. The interface
   exists to mount **host files into bays**; a FujiNet's images live on its own
   SD card or a TNFS server, which POM2 has no path to write. Implementing it
   would put rows in *Internal Disks & Media* whose Mount and Eject buttons
   cannot work — worse than absent. The user-visible goal ("see what the
   FujiNet has mounted") is already met by the FujiNet panel's device table,
   which lists every unit with its name, type and size.
4. ✅ **Clock unit.** SHIPPED as the one-line panel note it deserved.

## 8. Phase 3 — helper process (and why NOT embedded firmware)

**Status: shipped 2026-08-10, by a different route than this section
proposed.** The user-visible goal — *do not make the user start a second
program by hand* — is met by POM2 launching and reaping an EXISTING FujiNet
desktop binary (`ChildProcess`, `FujiNetCard::startHelper`). The firmware is
**not** vendored, and the rest of this section is why.

**What vendoring actually costs, measured rather than guessed.** The reference
implementation's recipe (`fujinet-go-apple2-desktop`,
`tools/fujinet/build-fujinet-desktop.sh`) is **856 lines of bash** that:

- clone and build **mbedTLS 3.6.5 from source** whenever the system copy is not
  3.x (4.x drops `mbedtls/md5.h`, which the firmware includes);
- stage the firmware tree and apply roughly a dozen **text-anchored patches**
  to upstream sources — each one `sys.exit`s the build when its anchor moves;
- build the result into a shared library, "first build takes a few minutes";
- require `bash` even on Windows (MSYS2), which POM2's build does not today.

Adding that to POM2 means an untestable-in-CI build path, a submodule pin, and
a patch set that breaks on every upstream bump. Against that, the *only* thing
it buys over launching the binary is not having to install the binary.

**What the helper does instead.** POM2 spawns the program (auto-detected as
`fujinet` on PATH, or a path the user sets), tracks it, and terminates its
whole process group on exit — a stray helper would otherwise hold the loopback
port the next session wants. POM2 deliberately does **not** touch the helper's
`fnconfig.ini`: that file holds the user's WiFi credentials, and it does not
need touching, because the firmware's Apple default for Bus-over-IP is already
`127.0.0.1:1985` (`CONFIG_DEFAULT_BOIP_PORT`, lib/config/fnConfig.h) — exactly
what the card listens on.

**The trap that cost a debugging round.** Killing only the direct child leaves
its own children running, still holding POM2's stdout pipe. It surfaced as
`child_process` passing standalone and hanging under `ctest` (which waits for
every process on the pipe); in production it would be a stray FujiNet still
holding port 1985 after POM2 "stopped" it. `stop()` signals the process group,
which is what `setpgid()` in the child was there for.

### The original proposal, for the record

## 8b. Embedded firmware (rejected)

`fujinet-go-apple2-desktop` builds the firmware as a shared library and
`dlopen`s it into the emulator process, joined over loopback. POM2 could do the
same behind a CMake option:

```
option(POM2_WITH_FUJINET_EMBEDDED "Build and embed the FujiNet firmware" OFF)
```

**Default OFF, and probably permanently.** It drags in the firmware's whole
dependency tree (mbedtls, expat, …), a pinned submodule, and a patch set
anchored to exact upstream text — `fujinet-go-apple2-desktop` maintains
`build-fujinet-desktop.sh` patches and a `FUJINET_COMMIT` pin precisely because
that coupling is brittle. It also complicates the release packaging
(`packaging/bundle.manifest` + the per-platform jobs in
`.github/workflows/release.yml`), which is still settling. The Phase 1
relay against a separately-installed FujiNet desktop build gives 100 % of
the functionality with 0 % of that maintenance burden.

Revisit only if user feedback says "having to install a second program is the
blocker".

## 9. Threading and the frame-hitch budget

The uncomfortable part, and the one to decide before writing code.

`SlotPeripheral` callbacks run **on the CPU thread under
`EmulationController::stateMutex`** (`SlotPeripheral.h` (the file-header contract)). A SmartPort
call is inherently synchronous — the 6502 is parked inside a `JSR` waiting for
its result — so `transact()` blocks that thread, and therefore the UI's access
to machine state, for the duration of the round trip.

- On **loopback** (the normal case: FujiNet desktop on the same machine) a
  round trip is well under a millisecond. Invisible.
- On **USB CDC** (a real FujiNet board) expect single-digit milliseconds per
  call — under a frame at 60 Hz, and the real machine is slower still, because
  a real SmartPort call is not fast either.
- On **LAN**, tens of milliseconds are possible: a dropped frame or two during
  a disk read. Acceptable.
- On **peer death**, the timeout bounds the damage to one visible hitch,
  followed by a clean I/O error.

**Decisions.**

1. **Block on the CPU thread with a bounded timeout.** This is what AppleWin
   does, it is what the hardware does, and the alternatives are worse: pumping
   the CPU while a call is in flight would need the ROM to poll a completion
   register, which changes the guest-visible protocol and breaks the "any
   FujiNet software works unmodified" promise.
2. **Default timeout = 250 ms**, panel-configurable from 50 ms to 5 s. That is
   ~50× headroom over a real USB round trip while keeping a dead peer to a
   quarter-second stall — one dropped frame's worth of pain, once, instead of
   a full second.

**What must not block:** `accept()`, connection teardown, and enumeration. Those
live on the worker thread (`pollAcceptOnce`, 200 ms poll), exactly as the SSC
does it, so quitting POM2 with a FujiNet connected never hangs.

Also: nothing here touches audio, so **no `emuCycles` stamping is required** —
but the card should still take `advanceCycles()` if Phase 2 drives the floppy
sound device on block transfers, and then the stamp rule applies as usual.

## 10. Reset, snapshot and rewind semantics

**Reset.** The spec is explicit: the Apple II side should send a `Control` with
control code `$00` to connected devices on a 6502 reset, so a modem drops its
connection and a printer ejects a partial page. Wire this in
`FujiNetCard::onReset()`. Also bump the request sequence number so any response
still in flight for the pre-reset request is discarded — that is the whole
reason the sequence number exists.

**Snapshot.** `appendSnapshotState` writes a tagged blob (magic + version, per
the `SlotPeripheral.h:90` (the snapshot-hook contract) contract) containing only the *local* state: the
sequence number, the listening port, and a flag for "was connected". It must
**not** try to serialise sockets or peer state.

**Rewind.** This is the honest limitation and it needs to be documented, not
papered over: **the external device cannot be rewound.** Rewinding the guest
past a `WriteBlock` does not un-write the peer's SD card; rewinding past an
HTTP POST does not un-post it. The rewind ring keeps working — the card simply
does not roll back with it.

Proposed policy:

- `loadSnapshotState` restores the sequence number and marks the link
  *desynchronised*, forcing the next call to re-enumerate.
- The panel shows a persistent note when a rewind has crossed FujiNet traffic.
- Document it in `DEV.md` under Rewind, and in the panel's tooltip.

Do **not** try to be clever (journalling calls for replay, blocking rewind while
connected). The user understands "the internet doesn't rewind".

## 11. Pinned tests

Per the project convention, every ported behaviour gets a smoke test under
`tests/`, registered in `tests/CMakeLists.txt` the way `test_smartport_card`
(`tests/CMakeLists.txt:2066-2086`) is, with a `TIMEOUT`. Socket tests have precedent:
`socket_compat_test.cpp`, `ssc_acia_smoke_test.cpp`,
`uthernet2_w5100_smoke_test.cpp`.

**`slip_framer_test`** — no sockets, no CPU.
- Round-trip a payload containing `$C0` and `$DB` and assert the wire form is
  `$DB $DC` / `$DB $DD`.
- Feed a frame in three arbitrary chunks; assert one frame out.
- Feed a delimiter mid-escape; assert the decoder reports truncation and
  resynchronises on the next delimiter.

**`sp_over_slip_link_test`** — in-process fake peer on `127.0.0.1`, no FujiNet.
- Fake peer answers a `Status`/DIB sweep with two units; assert enumeration.
- `ReadBlock` round trip returns the 512 bytes the peer sent.
- Peer replies with a **wrong sequence number** first, then the right one;
  assert the stale response is discarded and the correct one is returned.
- Peer never replies; assert `transact()` returns failure within the timeout
  and the link stays usable afterwards.

**`serial_port_test`** — POSIX only (`posix_openpt` / `grantpt` / `unlockpt`
gives a master + a real slave device node the `SerialPort` can open; Windows
has no equivalent, so the test is compiled out there the way other
platform-gated tests are).
- Round-trip a payload containing `$11`, `$13`, `$0D`, `$0A` and assert it
  arrives byte-identical — this is the assertion that catches a missing
  `cfmakeraw`, and those four bytes are exactly the ones `IXON`/`ICRNL`/`ONLCR`
  would corrupt. SLIP payloads contain them routinely.
- `readSome` with nothing to read returns 0 within the requested timeout
  (±20 ms), not immediately and not forever.
- After `open()`, **DTR and RTS are de-asserted** (`TIOCMGET`) — the ESP32
  auto-reset pin. Also assert `HUPCL` is clear in the termios flags.
- Closing the port while a reader is blocked does not hang.

Running the whole `SpOverSlipLink` over a pty pair is possible and tempting,
but the fake peer would then need its own SLIP encoder; keep the pty test at
the `SerialPort` level and let `sp_over_slip_link_test` cover the protocol over
TCP. The transport abstraction is what makes that split safe.

**`fujinet_card_smoke_test`** — the headline pin.
- ROM signature: `$Cn01 == $20`, `$Cn03 == $00`, `$Cn05 == $03`, `$CnFF` points
  at the driver, driver+3 is the SmartPort entry.
- With a fake peer serving block 0: run a real `M6502` from `$Cn00`, assert
  `$0800` holds the block and PC reaches `$0801` (this is also the
  `bootFromSlot` path).
- Hand-assemble `JSR $Cn63` (the SmartPort entry, driver + 3) + inline `cmd`/pointer in guest RAM, run it, and
  assert (a) the response landed at the payload address, (b) `A`/`X`/`Y` and
  the Z/C flags are right, (c) **the PC after `RTS` is the instruction after
  the 3 inline bytes** — the stack fixup.
- Repeat that last case with `SP = $01` and `SP = $FE` to pin the page-1 wrap.
- With **no peer connected**: `Status(unit 0, code 0)` returns device count 0
  and does not hang.

The stack-fixup and page-1-wrap assertions are the ones that will actually
catch a regression; write them first.

## 12. Documentation duties

*Discharged — every item below landed with Phase 1 (2026-08-10).*

- `CLAUDE.md` — two rows in the subsystem map: `FujiNet (SP-over-SLIP relay,
  TCP + USB CDC) | FujiNetCard.*, SpOverSlipLink.*, Sp*Transport.*,
  SlipFramer.h | § FujiNet`, and `Host serial ports (POSIX termios / Win32 DCB)
  | SerialPort.h/.cpp | § Host serial` — the latter next to the existing
  "Host sockets" row, because it is the same kind of primitive.
- `DEV.md` — a `## FujiNet` section: the protocol summary, the trap mechanism,
  the stack fixup (with the AppleWin-vs-POM2 SP-width gotcha spelled out), the
  threading decision and its rationale, the rewind limitation. Plus a short
  `## Host serial` section carrying the ESP32 DTR/RTS auto-reset warning and
  the `cu.` vs `tty.` macOS rule, since those will bite anything else that
  opens a serial port later.
- `docs/lle_vs_hle.md` — the master-table row from [4.2](#42-where-it-sits-on-the-llehle-scale),
  plus a paragraph in *"Where the HLE seams show"* about the new
  "peer absent or slow" failure category.
- `TODO.md` — Phase 2/3 items, and a note that MAME has no FujiNet device so
  the parity dashboard has no entry for it.
- `README.md` — a short "Using FujiNet" section: install the FujiNet desktop
  build, point it at `127.0.0.1:1985`, put the card in slot 7, boot.
- `CHANGELOG.md` — the *why*: one transport carries every FujiNet function
  because all its Apple II devices are SmartPort units.

## 13. Risks and gotchas

No open questions remain: the timeout is settled at 250 ms
([9](#9-threading-and-the-frame-hitch-budget)) and the USB CDC transport is in
Phase 1 ([6.3](#63-serialport-and-the-usb-cdc-acm-transport)). What follows is
what will bite during implementation.

1. **//c-class machines.** The forced `INTCXROM` masks all slot ROM, so the
   card is meaningful on II+ / //e only in Phase 1. On a real //c the FujiNet
   *is* the SmartPort on the disk port, so the correct //c integration is to
   hang the relay off the existing on-board SmartPort `$C500` hole
   (`exposesIicOnboardRom()`, see `project_iic_smartport_boot`) rather than as
   a card. **Defer, and say so in the panel** ("not available on //c-class
   profiles"). Doing it properly is its own phase.
2. **`SmartPortCard` overlap.** POM2 already has a SmartPort card with its own
   `spExecute()` engine. Resist the temptation to bolt FujiNet onto it: that
   card is block-only (`kMaxUnits = 2`, `SmartPortUnit` has only
   `readBlock`/`writeBlock`), whereas FujiNet needs 8 units and full
   character-device semantics (`Open`/`Close`/`Read`/`Write`) for `N:`. A
   separate card is cleaner and leaves the tested one alone.
3. **Serial device permissions.** On Linux, `/dev/ttyACM*` is typically
   `root:dialout` 0660, so a user not in `dialout` gets `EACCES` and no
   explanation. Detect that specific errno and say so in the panel ("add your
   user to the `dialout` group and log out/in") rather than reporting a generic
   open failure. On macOS no group membership is needed. This is a
   documentation-and-error-message problem, not a code problem, but it is the
   most likely first-contact failure for the serial path.
4. **Serial baud rate is a lie.** CDC-ACM ignores it, but some USB-serial
   bridges (a genuine FTDI/CP210x in front of the ESP32 rather than the ESP32's
   native USB) do not. Keep the setting exposed and default it to 115200; do
   not "helpfully" hide it.
5. **Protocol drift.** The spec is a wiki page and the firmware moves. Pin the
   wiki revision date in the header comment (currently: last edited
   25 Jan 2025, 30 revisions) and add a note to re-check when a FujiNet
   firmware release notes an Apple II protocol change.
6. **Testing against the real thing.** The pinned tests use fake peers and a
   pty. Before calling Phase 1 done, two manual passes that CI cannot do (no
   network, no firmware build, no hardware):
   - **TCP:** against a FujiNet desktop build — boot `CONFIG`, mount a disk
     from a TNFS host, run an `N:` client (weather or telnet app).
   - **Serial:** against a physical FujiNet board over USB — same three steps,
     plus unplug/replug mid-session to exercise hot-plug, plus confirm the
     board does **not** reboot when POM2 opens and closes the port (the DTR/RTS
     check from [6.3](#63-serialport-and-the-usb-cdc-acm-transport)).

   Checklist items in the PR description, not tests.

## 14. Effort and ordering

| Step | Deliverable | Estimate |
|---|---|---|
| 1 | `SlipFramer` + `slip_framer_test` | 0.5 d |
| 2 | `SpTransport` + `SpTcpTransport` | 0.5 d |
| 3 | `SpOverSlipLink` + fake-peer test | 1 d |
| 4 | `SerialPort` (POSIX + Win32) + `serial_port_test` | 1 d |
| 5 | `SpSerialTransport` + hot-plug + enumeration | 0.5 d |
| 6 | `FujiNetCard` ROM + boot path + smoke test | 1 d |
| 7 | SmartPort trap, marshalling, stack fixup, flags | 1.5 d |
| 8 | ProDOS entry path | 0.5 d |
| 9 | Host wiring (catalog, plug, settings, CLI) | 0.5 d |
| 10 | Panel (incl. transport selector + device combo) | 0.75 d |
| 11 | Docs + manual passes (TCP peer **and** real board) | 0.75 d |
| | **Phase 1 total** | **~8.5 days** |
| | Phase 2 (printer / media bays / modem) | 2 d |
| | Phase 3 (embedded firmware) — only if asked | 3-5 d + ongoing |

Order matters. Steps 1-5 are testable with no emulator at all, step 6 is
testable with no I/O, and only step 7 needs both. That keeps every stage
independently verifiable, which is what makes the stack-fixup bug findable
instead of mysterious.

The serial work (steps 4-5, ~1.5 d) is deliberately placed **before** the card:
it is the piece with the most unknowns (platform APIs, permissions, the ESP32
reset circuit) and the least dependency on anything else, so discovering a
problem there costs nothing already built. It is also the only part that
produces something reusable outside FujiNet — `SerialPort` is a host primitive
POM2 does not currently have, and a real modem on the SSC or ADTPro over a
cable would both want it.

---

*Sources: [SP-over-SLIP spec](https://github.com/FujiNetWIFI/fujinet-firmware/wiki/Apple-II-SP-over-SLIP),
[FujiNetWIFI/AppleWin](https://github.com/FujiNetWIFI/AppleWin),
[fujinet-go-apple2-desktop](https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop),
[fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware),
[Apple II & III FujiNet Quickstart Guide](https://github.com/FujiNetWIFI/fujinet-firmware/wiki/Apple-II-&-III-FujiNet-Quickstart-Guide).*

---

## 15. What implementation changed

Recorded because a plan that silently diverges from the code is worse than no
plan. Everything else landed as written.

**1. The serial test cannot pin the ESP32 reset lines.** The plan said
`serial_port_test` would assert "DTR/RTS left de-asserted on open". It cannot:
a Linux pty has **no modem-control lines** — `TIOCMGET` fails on both ends of
the pair, so the harness has nothing to read back. What the test pins instead
is the half that *is* observable and regresses just as easily (`HUPCL` clear,
`CLOCAL` set, raw-mode flags, and a binary round trip through `$11`/`$13`/
`$0D`/`$0A`), plus a conditional arm that tightens automatically on a device
that really has lines. `SerialPort::modemControlSupported()` was added so the
distinction is explicit rather than a silent `false`. The line-state assertion
lives in the manual checklist ([§ 13](#13-risks-and-gotchas), item 6).

**2. Peer teardown needed two entry points.** Not foreseen. `transact()`
discovers a dead peer while it already holds `callMtx_`, and `std::mutex` is
not recursive, so the single `handlePeerLost()` the plan implied **deadlocked
the CPU thread with the emulated 6502 parked mid-SmartPort-call**. Split into
`peerLostLocked()` (lock held) and `handlePeerLost()` (lock not held). Found by
`sp_over_slip_link`'s clean-shutdown case, and only intermittently — it passed
standalone and hung under `ctest`.

**3. The card smoke test steps rather than runs.** The plan assumed a landmark
could be planted at `$FABA` to catch the "continue the slot scan" fallback. No
Apple II ROM is loaded in that harness, so `$D000-$FFFF` reads as zeroes and
the Language Card swallows the write. The test single-steps and watches the PC
pass through instead (`Machine::runUntilPc`).

**4. Enumeration retries only the first unit.** The plan said "retry the sweep
up to 3 times" for a board still booting. Retrying the *whole* sweep would
multiply a dead peer's cost by three; retrying only unit 1 — the one that
distinguishes "still booting" from "nothing there" — gets the same benefit for
one unit's worth of timeout.

**5. No `openInBrowser` helper exists in POM2.** The panel's "Web UI" button
surfaces the address for the user rather than shelling out to a browser.

**6. `SerialPort::enumerate()` prefers `/dev/serial/by-id/`.** Not in the plan,
but `ttyACM0` does not survive a replug and a user who saved that path in
settings would silently target somebody else's device after a reboot.

**7. The page-1 wrap is pinned at `SP = $01` only.** [§ 11](#11-pinned-tests)
asked for both `SP = $01` and `SP = $FE`; the smoke test covers only the `$01`
case.

Line counts came in close to the estimate: `SlipFramer.h` ~180,
`SerialPort.*` ~600, the transports ~430, `SpOverSlipLink.*` ~640,
`FujiNetCard.*` ~700, the panel ~320, tests ~1200.
