# Backlog — Network and FujiNet

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
| [NETWORK-001](#network-001) | 🟡 | To verify | [FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21. |
| [NETWORK-002](#network-002) | 🟡 | To verify | [FujiNet] a `CONTROL` to the peer's PRINTER unit kills it |
| [NETWORK-003](#network-003) | 🟡 | To verify | [FujiNet] the desktop firmware's `N:` device never opens a socket |
| [NETWORK-004](#network-004) | 🟡 | Policy/UI implemented; remote-media validation remains | [FujiNet] delayed network-media calls |
| [NETWORK-005](#network-005) | 🟡 | To verify | [FujiNet] a network-backed SmartPort call freezes the emulator |
| [NETWORK-007](#network-007) | 🟡 | To verify | [FujiNet] //c-class support |
| [NETWORK-011](#network-011) | 🟡 | Partial: corrected findings separated from remaining reports | Bug hunt #14's network residue |
| [NETWORK-006](#network-006) | 🟢 | To verify | [FujiNet] `PR#n` before the peer attaches prints `FN ERROR` |
| [NETWORK-008](#network-008) | 🟢 | To verify | [FujiNet] embedded firmware |
| [NETWORK-013](#network-013) | 🟢 | Shipped (verify) | W5100 name resolution is still inline |
| [NETWORK-009](#network-009) | 🧊 | Frozen | Uthernet I has no host transport on Windows |
| [NETWORK-010](#network-010) | 🟡 | Partial: local TCP implemented | Uthernet II inbound (`LISTEN`) |
| [NETWORK-012](#network-012) | 🧊 | Frozen | Uthernet I on WASM |

<a id="network-001"></a>

## NETWORK-001 — [FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21.

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Define the next native FujiNet phase with CONFIG/slot-state acceptance tests while keeping the external relay functional.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1836). Original dates and estimates are retained below.

- 🔵 **[FujiNet] built-in FujiNet, native in POM2 — DECIDED 2026-08-21.**
  Chosen architecture: a **native `FujiNetDevice` in POM2's own C++ covering
  the disk perimeter** (host slots, drive slots, a TNFS client, images served
  as SmartPort block devices), driven from the FujiNet panel, **coexisting**
  with the existing SP-over-SLIP relay for a real USB board or a full external
  firmware. The panel gets a source selector: *Built-in / USB board / External
  firmware* — the same "choose the level, not the catalog key" shape as the
  Abstraction Levels panel.
  Why this over the alternatives: hosting the vendor firmware (prebuilt or
  built from source) does **not** remove the class of bug that the CONFIG
  breakage belonged to (fixed 2026-08-21, → `CHANGELOG.md`), because the guest
  still reaches the device through the relay's control plane; when POM2 *is*
  the device, that class of bug cannot exist. Building the firmware into POM2's
  build was re-costed and re-rejected (`docs/fujinet_plan.md` § 8).
  Out of scope for the native path, deliberately: the `N:` network device
  (HTTP/SSH/JSON), the modem and CP/M — those stay the relay's job.
  Phases: (1) TNFS client + tests — **done**, and since 2026-08-28 it has a
  caller: `TnfsMedia.*` fetches an image from a TNFS server into a local cache
  and `POM2 tnfs://host/path/image.po` boots it like any other disk. That is
  not phase 3 — the guest sees an ordinary local image, not a block device
  backed by the network — but it makes the client reachable and useful now;
  (2) the Fuji control device (host/drive slots) so CONFIG sees real state;
  (3) block serving of a mounted image; (4) the panel's source selector.

<a id="network-002"></a>

## NETWORK-002 — [FujiNet] a `CONTROL` to the peer's PRINTER unit kills it

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Reproduce PRINTER CONTROL against the peer; record the peer version and place the fix in the component that fails.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1859). Original dates and estimates are retained below.

- 🟡 **[FujiNet] a `CONTROL` to the peer's PRINTER unit kills it** — measured
  2026-08-21, reproducible three runs out of three. The packet is
  byte-identical in shape to the ones units 10-12 answer normally
  (`04 03 0D 00 00 00 …`, an empty control list), yet the peer throws
  `std::length_error` out of `Request::from_packet` and aborts. Upstream bug
  in the printer device — the same unit whose DIB already advertises the
  modem's type byte. POM2 relays faithfully and now REPORTS the death
  (`peer LOST after N s — M call(s) served`), which is what localised it in a
  single run. Worth reporting upstream; POM2 has nothing to fix.

<a id="network-003"></a>

## NETWORK-003 — [FujiNet] the desktop firmware's `N:` device never opens a socket

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Trace an N: open request through the peer and record whether failure belongs to firmware or the POM2 relay.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1868). Original dates and estimates are retained below.

- 🟡 **[FujiNet] the desktop firmware's `N:` device never opens a socket** —
  it answers the guest's open with success (`CONNECTED to
  N:HTTP://THEOLDNET.COM/` appears on the Apple II) and then no outbound TCP
  is ever created, watched live on the peer's own descriptors. NOT POM2: the
  same firmware, same machine, opens real TCP for TNFS. Its WiFi is a
  `DummyWiFiManager` and giving it an SSID does not help. A real FujiNet
  board over USB is the path for `N:`; the relay is unchanged for it.

<a id="network-004"></a>

## NETWORK-004 — FujiNet delayed network-media calls

**Priority:** 🟡 · **State:** Timeout policy and UI implemented; remote-media evidence remains.

**Acceptance criterion:** Run a delayed TNFS-backed mount/read/write against
an identified peer version, record successful completion and bounded failure,
and check UI responsiveness separately under [NETWORK-005](#network-005).

**Historical evidence:** [original 250 ms finding](../archive/todo-2026-10-05.md#source-1875)
and the 2026-10-06 recheck predate the current policy.

**Code reconciliation 2026-10-20:** local calls default to 250 ms. Selected
FujiNet host/directory operations and block I/O for images marked mounted use
at least 5000 ms in `SpOverSlipLink::transact`; an explicit outer call budget
can cap that. One deadline covers sending and receiving. The FujiNet panel
has a **Timeout (ms)** slider from 50 to 5000, persisted per slot as
`fujinet_timeout_ms_slot<N>`. The old claims of a uniform 250 ms budget and no
UI are superseded.

`sp_over_slip_link` contains `testRemoteFujiDirectoryCanOutlastLocalIoBudget`
and `testMountedFujiImageCanReadSlowNetworkBlocks` for delayed fake peers.
That source-level coverage is not a fresh TNFS/USB
run, and it does not establish that every third-party network command fits
within five seconds. Keep the remaining validation task scoped to that evidence.

<a id="network-005"></a>

## NETWORK-005 — [FujiNet] a network-backed SmartPort call freezes the emulator

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Run a delayed SmartPort network operation while checking UI responsiveness and safe guest completion.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1884). Original dates and estimates are retained below.

- 🟡 **[FujiNet] a network-backed SmartPort call freezes the emulator** —
  `transact()` blocks the CPU thread under `stateMutex` by design (see the
  threading note in `SpOverSlipLink.h`, and § 9 of the plan for why that was
  the right call). Invisible at 250 ms over loopback; very visible once the
  timeout is raised for a peer whose media lives on the internet — the UI and
  the AI control server both stall for the length of every read. Wants at
  least a "waiting on FujiNet" indication, and possibly a bounded pump of the
  UI while a call is outstanding.

<a id="network-007"></a>

## NETWORK-007 — [FujiNet] //c-class support

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Boot a //c-class profile with the proposed FujiNet path without displacing required motherboard firmware.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1905). Original dates and estimates are retained below.

- 🟡 **[FujiNet] //c-class support** — the card is II+ / //e only: a
  //c's forced INTCXROM masks all slot ROM. On real hardware the FujiNet
  *is* the SmartPort on the disk port, so the correct integration hangs
  the relay off the on-board `$C500` hole (`exposesIicOnboardRom`,
  see `project_iic_smartport_boot`) rather than a slot card. *~1-2 d.*

**Revalidation note 2026-10-06:** premise still holds — `fujinet` is a slot
card (`src/SlotCardCatalog.h:128`). Since 2026-09-27 the 32 KB //c and the //c+
answer their rear-port SmartPort **bus** through `IIcExternalSmartPort`
(`src/IIcExternalSmartPort.h`), serving the units of the built-in slot-5
card; that bus responder, not only the `$C500` stub, is now the seam a //c
FujiNet would hang off.

<a id="network-011"></a>

## NETWORK-011 — Bug hunt #14's network residue

**Priority:** 🟡 · **State:** Partly resolved; current implementation summary reconciled 2026-10-19.

**Acceptance criterion (after revalidation):** Break down the network register, relay-range, reset and teardown findings into independent reproductions with bounded completion.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2031). Original dates and estimates are retained below.

**Current status, reconciled 2026-10-19:** The original report is not a
current defect list. Implemented corrections include CS8900A multicast and
receiver gating, PacketPage reset/receive reads, RxMISS snapshot state, RxCTL
length/status filtering and receive-event handling; W5100 socket-register
writes; FujiNet's `$C000-$CFFF` buffer fence and bounded reset sweep; and
`LD_LIBRARY_PATH` filtering for helper processes. The dated evidence below
records their regression cases.

Remaining reports to verify independently include W5100 IPRAW policy routing,
CS8900A implied-skip behavior and child-process teardown. CRC-error acceptance
is constrained by the backend's already-validated frames. The reported slirp
oversized-front-frame issue is not reachable with the current card callers'
1518-byte receive buffers. See the original report for historical details;
do not reintroduce the corrected items as work still to implement.

**Bug hunt 2026-10-18:** The CS8900A multicast classifier and receive-enable
gate were reproduced and fixed. A frame with first destination octet `$01`
was rejected as unicast while `$80` could pass as multicast; the chip uses
bit 0 for the individual/group distinction. Separately, frames arriving
with `LineCTL.SerRxON` clear were queued and appeared after the guest enabled
the receiver. `uthernet_cs8900_smoke` fails on each reverted defect and now
checks that pre-enable frames are discarded. The other findings in this entry
remain to verify independently.

**Bug hunt 2026-10-18 (continued):** FujiNet's buffer guard now rejects
`$C000-$CFFF`, including slot and expansion ROM, and the POSIX helper
environment filter removes `LD_LIBRARY_PATH`. The `fujinet_card` and
`child_process` tests failed before each fix and pass afterward. Other
findings in this entry remain open for independent verification.

**Bug hunt 2026-10-18 (PacketPage reset):** The CS8900A reset now restores
`packetPagePtr_` to `$3000`, including after SelfCtl RESET. Before the fix,
the next PacketPage data access used the previous driver's address and
auto-increment flag. `uthernet_cs8900_smoke` failed on the stale pointer and
now covers both reset paths. Other findings above remain open.

**Bug hunt 2026-10-18 (RxMISS snapshot):** The chip snapshot now stores
`framesMissed_` and `rxMissReported_`. Previously the saved `RxMISS` register
could say two misses were pending while the underlying total had reset to
zero; the next miss then reported one. The test reproduces this and covers
both the pending and read-cleared states. Version 1 chip snapshots remain
readable, with their pending `RxMISS` value as the recoverable starting count.

**Bug hunt 2026-10-18 (W5100 socket registers):** Writes to `Sn_DHAR` and
`Sn_MSSR` now stick on all four sockets. The register-level test failed before
the fix and verifies reset defaults afterward. The slirp oversized-front-frame
note is not reachable through current card callers: both provide a 1518-byte
buffer, the maximum frame size the backend admits. Other findings above
remain to verify independently.

**Bug hunt 2026-10-18 (PacketPage receive location):** Reads through the
PacketPage data window now return the staged receive location at
`$0400-$09FF`, including `RxStatus`, `RxLength` and frame bytes. The receive
test failed on a zero `RxStatus` before the fix and now confirms these values
match the RXTXDATA stream. Other findings above remain to verify.

**Bug hunt 2026-10-18 (RxCTL status acceptance):** `RxOKA` now gates frames
with valid lengths, while `RuntA` gates short frames independently of the
destination-address filter. The Uthernet I test failed with `RxOKA` clear
before the fix. CRC-error acceptance remains limited by the host backend,
which supplies frames after CRC validation.

**Bug hunt 2026-10-18 (runt status):** An accepted short frame now sets the
`Runt` status without `RxOK`, and ISQ reports that frame even though `RxOK`
is clear. The direct-event test failed on the incorrect `RxOK` bit before
the fix; a separate ISQ test guards delivery to polling drivers.

**Bug hunt 2026-10-18 (queued receive decision):** The CS8900A no longer
filters an accepted frame a second time when the guest reads `RxEvent`.
`pumpBackend()` retains the arrival-time address classification with the
frame, so later `RxCTL` or IA writes cannot discard it or alter its status.
The register-level test failed before the fix with both settings changed.

**Bug hunt 2026-10-18 (RxCFG Skip_1):** A write to `RxCFG.Skip_1` now frees
the committed receive frame; it no longer cancels an in-progress transmit.
The [CS8900A datasheet](https://www.futurlec.com/Datasheet/Others/CS8900A.pdf)
§4.4.6 and §5.2.5 resolve the earlier MAME-parity uncertainty. The smoke
test failed before the fix and now checks both the next queued frame and the
transmit handshake.

**Bug hunt 2026-10-18 (empty RxEvent):** Reading `RxEvent` with no queued
successor now leaves the committed frame's `RxStatus` intact in PacketPage
and RXTXDATA. Previously the empty event value `$0004` overwrote it while
`rxState_` still exposed the frame. The register-level test failed on the
stale status before the fix.

**Bug hunt 2026-10-18 (hash receive status):** The CS8900A now reports
`Hashed` in bit 9 and the hash-table index in bits 10–15 for accepted
multicast and individual frames. Multicast previously discarded the index,
and individual frames shifted it into the `Hashed` bit. Table 26 of the
[datasheet](https://www.farnell.com/datasheets/88476.pdf) also confirms that
an accepted runt uses the ordinary error status. Fixed register-level
vectors cover all three cases.

**Bug hunt 2026-10-18 (minimum receive length):** `RuntA` now accepts
frames from eight bytes upward, while shorter frames are always discarded
as specified by [RxCTL in the datasheet](https://www.farnell.com/datasheets/88476.pdf).
The loopback test reproduced acceptance of seven bytes before the fix and
checks that eight bytes still produce a Runt event.

**Bug hunt 2026-10-18 (minimum transmit length and padding):** The CS8900A
now accepts a three-byte transmit request and pads short frames to 60 data
bytes unless `TxPadDis` is set. The earlier four-byte floor and unpadded
backend output disagreed with [the datasheet](https://www.farnell.com/datasheets/88476.pdf)
§4.4.11 and Table 34. The smoke test failed at the three-byte handshake
before the fix and checks both padding modes afterward.

**Bug hunt 2026-10-18 (reset courtesy latency):** `notifyGuestReset()` now
shares a 400 ms budget across its device-control notifications. A responsive
16-unit fake peer taking 80 ms per control previously stalled guest reset for
over a second. The regression test checks the aggregate bound and confirms
that the next guest request ignores a late reply from the sweep.

<a id="network-006"></a>

## NETWORK-006 — [FujiNet] `PR#n` before the peer attaches prints `FN ERROR`

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test PR#n before and after peer attachment and define a useful pending/error indication.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1892). Original dates and estimates are retained below.

- 🟢 **[FujiNet] `PR#n` before the peer attaches prints `FN ERROR`** — the
  autostart slot scan handles the no-peer case correctly (the card steps
  aside and the scan carries on to slot 6), but a manual `PR#n` in the same
  state just fails. The card could wait briefly for a peer, or say *why* it
  failed. Cosmetic, but it is the first thing anyone hits.

<a id="network-008"></a>

## NETWORK-008 — [FujiNet] embedded firmware

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Validate any approved embedded firmware build against CONFIG and relay compatibility; follow the existing architecture decision first.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1910). Original dates and estimates are retained below.

- 🟢 **[FujiNet] embedded firmware** — `fujinet-go-apple2-desktop` builds
  the FujiNet firmware as a shared library and `dlopen`s it, so the user
  needs no second program. Deliberately NOT done: it drags in mbedtls,
  expat, a pinned submodule and a patch set anchored to exact upstream
  text. Revisit only if "having to install a second program" turns out to
  be the real blocker.

<a id="network-013"></a>

## NETWORK-013 — W5100 name resolution is still inline

**Priority:** 🟢 · **State:** Shipped (verify) — premise false on 2026-10-06.

**Revalidation 2026-10-06:** the lift this item asks for landed the same day it
was written — `0ff494e` (2026-08-27, "Lift the W5100 virtual-DNS resolver out of
the device"). `W5100NameResolver` (`src/W5100NameResolver.h/.cpp`, behind the
`W5100Resolver` interface in `src/W5100Resolver.h`) owns the cache, the mailbox,
the in-flight cap and the bounded off-thread lookup; `W5100Device` keeps only the
register half (`src/W5100Device.cpp:1440-1450`, `kDnsWaitMs` = 120 in
`src/W5100Device.h:227`); the production resolver is injected in
`src/MainWindow_SlotConfig.cpp:427`. Pinned by `w5100_name_resolver`. No
CHANGELOG entry names the lift; record one when this entry is removed.

**Acceptance criterion (after revalidation):** Test bounded asynchronous name resolution, cache behavior and cancellation without stalling guest register reads.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2241). Original dates and estimates are retained below.

- 🟢 **W5100 name resolution is still inline** *(2026-08-27)* — deliberately
  left in `W5100Device` when the socket seam landed: an async mailbox with an
  in-flight cap, a bounded wait and its own cache, wired to register reads.
  Its own pass, and not on anything's critical path.

<a id="network-009"></a>

## NETWORK-009 — Uthernet I has no host transport on Windows

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1917). Original dates and estimates are retained below.

- 🧊 **Uthernet I has no host transport on Windows** — libslirp is the
  only backend that moves raw frames, and CMake deliberately does not
  look for it on WIN32. vcpkg *does* carry a libslirp port (4.9.1), so
  the library is obtainable; what is missing is POM2's side —
  `SlirpNetworkBackend`'s poll loop is POSIX `poll()` over the fds
  libslirp returns, and porting it cannot be verified without a Windows
  libslirp build to test against. Note the vcpkg port pulls **glib**,
  which is a heavy addition to the Windows CI job. Uthernet II is
  unaffected (hardware TCP/IP on host sockets). *1-2 d + CI budget.*

<a id="network-010"></a>

## NETWORK-010 — Uthernet II inbound (`LISTEN`)

**Priority:** 🟡 · **State:** Local TCP implemented 2026-10-17; LAN mapping remains open.

The user explicitly requested the remaining non-IIgs parity work. Desktop
W5100 sockets now accept `LISTEN` on `127.0.0.1:Sn_PORT` when
`uthernet_allow_loopback` is enabled. INIT → LISTEN → ESTABLISHED publishes
the peer IP/port and CON interrupt; receive/send use the existing TCP rings.
Ports below 1024, 1985, 6502 and 6503 remain refused. Cancel/close/reset
release the socket; denied/failed requests retain CLOSED + TIMEOUT.

Checks: `w5100_socket_seam` covers pending accept, success, failure and port
policy; `uthernet2_w5100_smoke` includes an actual inbound TCP exchange.
Remaining: explicit LAN-interface/host-port configuration and libslirp forwarding.

<a id="network-012"></a>

## NETWORK-012 — Uthernet I on WASM

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2050). Original dates and estimates are retained below.

- 🧊 **Uthernet I on WASM** — the CS8900A model is browser-safe but has
  no transport there (no raw sockets, and libslirp isn't in the
  Emscripten build). A websocket-proxied backend would fix both cards'
  raw modes in the browser. *2-3 d.*
