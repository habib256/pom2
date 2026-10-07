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
| [NETWORK-004](#network-004) | 🟡 | To verify | [FujiNet] the 250 ms relay timeout is sized for local media only |
| [NETWORK-005](#network-005) | 🟡 | To verify | [FujiNet] a network-backed SmartPort call freezes the emulator |
| [NETWORK-007](#network-007) | 🟡 | To verify | [FujiNet] //c-class support |
| [NETWORK-011](#network-011) | 🟡 | To verify | Bug hunt #14's network residue |
| [NETWORK-006](#network-006) | 🟢 | To verify | [FujiNet] `PR#n` before the peer attaches prints `FN ERROR` |
| [NETWORK-008](#network-008) | 🟢 | To verify | [FujiNet] embedded firmware |
| [NETWORK-013](#network-013) | 🟢 | Shipped (verify) | W5100 name resolution is still inline |
| [NETWORK-009](#network-009) | 🧊 | Frozen | Uthernet I has no host transport on Windows |
| [NETWORK-010](#network-010) | 🧊 | Frozen | Uthernet II inbound (`LISTEN`) |
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

## NETWORK-004 — [FujiNet] the 250 ms relay timeout is sized for local media only

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Test a delayed network-backed relay call with a bounded timeout and a useful guest-visible failure.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1875). Original dates and estimates are retained below.

- 🟡 **[FujiNet] the 250 ms relay timeout is sized for local media only** —
  measured 2026-08-21. `SpOverSlipLink::kDefaultTimeoutMs` is fine while the
  peer answers out of its own SPIFFS (booting `autorun.po` never approaches
  it), but every block read of a TNFS-hosted image crosses the internet and
  overruns it, so the guest gets `FN ERROR` instead of a boot. Workaround
  today is the per-slot `fujinet_timeout_ms_slot<N>` key (3000 works); it has
  no UI and nothing tells the user it is why their network disk will not
  boot. Options: raise the default, or measure the peer's round-trip at
  enumeration and size the timeout from it. *~2 h.* → `DEV.md` § FujiNet.

**Revalidation note 2026-10-06:** unchanged on main — the constant now lives
on `FujiNetTransport` (`src/FujiNetTransport.h:52`, still 250) and
`SpOverSlipLink` inherits it; the per-slot key is read in
`src/MainWindow_SlotConfig.cpp:455-458` and still has no UI.

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

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Break down the network register, relay-range, reset and teardown findings into independent reproductions with bounded completion.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2031). Original dates and estimates are retained below.

- 🟡 **Bug hunt #14's network residue** *(2026-09-09; read, not fixed)*:
  the loopback policy now lives in three files (`W5100Device::checkDestination`,
  `FujiNetNetDevice`, `SlirpBackend::transmit`) and the W5100's IPRAW path
  still bypasses `checkDestination` device-side (the backend catches it);
  the CS8900A tests the multicast I/G bit as `buffer[0] & 0x80` where 802.3
  puts it in bit 0 (looks like a faithful MAME/VICE port — needs a ruling),
  leaves `packetPagePtr_` stale on reset, never uses `RxOKA`, does not
  decode `CRCerrorA`/`RuntA`/`ExtradataA`, stamps `$0004` over PacketPage
  `$0400` on an implied skip with an empty queue, ignores `LineCTL.SerRxOn`
  (frames are buffered with the receiver off), reads `$0400-$0FFF` as 0,
  and omits `framesMissed_`/`rxMissReported_` from its snapshot while saving
  the register they baseline; `FujiNetCard::rangeIsSafe` guards `$C000-$C0FF`
  only, so a relay write to `$C100-$C7FF` reaches `slotRomWrite`;
  `SpOverSlipLink::notifyGuestReset` makes one round trip per unit (32) on
  the CPU thread on every reset with no aggregate bound; `SlirpBackend::
  receive` returns 0 on an oversized front frame and ends the drain for that
  tick; W5100 `Sn_DHAR`/`Sn_MSSR` writes are dropped; `ChildProcess::stop`
  ends in a blocking `waitpid` and the env scrub misses `LD_LIBRARY_PATH`.
  *1 d for the lot.*

**Revalidation note 2026-10-06 (code reading, not reproduction):**
`FujiNetCard::rangeIsSafe` still refuses only `$C000-$C0FF`
(`src/FujiNetCard.cpp:269-278`); the env scrub still lists `LD_PRELOAD`/
`LD_AUDIT`/`DYLD_*` but not `LD_LIBRARY_PATH` (`src/ChildProcess.cpp:211-223`);
the CS8900A still tests `buffer[0] & 0x80` (`src/Cs8900aDevice.cpp:365`).
`notifyGuestReset` now skips the printer unit and stops at the first unit
that does not reply (`src/SpOverSlipLink.cpp:866-905`), which bounds the dead-peer
case but not a live peer with many units.

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

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1926). Original dates and estimates are retained below.

- 🧊 **Uthernet II inbound (`LISTEN`)** — the W5100 `LISTEN` command is
  decoded but unimplemented: neither transport can route an inbound
  connection to the guest (libslirp is outbound-only without explicit
  port forwarding). Needs a user-configured host port to bind plus a
  slirp `hostfwd`-style mapping. The *refusal* is now cheap: it demotes to
  `SOCK_CLOSED` + TIMEOUT and warns once per socket instead of once per lap
  of the server loop it provokes (2026-09-07 — the loop was paying an
  unbuffered log line and a socket/close per iteration on the CPU worker,
  under `stateMutex`). *1 d.*

<a id="network-012"></a>

## NETWORK-012 — Uthernet I on WASM

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2050). Original dates and estimates are retained below.

- 🧊 **Uthernet I on WASM** — the CS8900A model is browser-safe but has
  no transport there (no raw sockets, and libslirp isn't in the
  Emscripten build). A websocket-proxied backend would fix both cards'
  raw modes in the browser. *2-3 d.*
