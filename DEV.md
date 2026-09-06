# DEV.md

Implementation notes: MAME refs, non-obvious gotchas, pinned smoke
tests. Orientation + memory map + profile table → `CLAUDE.md`. User
walkthrough → `README.md`. Fix history + rationale → `CHANGELOG.md`.

Convention: each subsystem cites the MAME file + line range it ports
from. When MAME upstream renames a path (e.g. `wozfdc.cpp` `bus/a2bus
→ machine`), refresh refs in a pass.

## Table of contents

- [CPU](#cpu) · [Z80 core](#z80-core-z80hcpp--softcardcpm-phase-1) · [SoftCard Z80](#softcard-z80-softcardz80hcpp--cpm-phase-2)
- [Memory](#memory)
- [Display](#display)
- [Audio](#audio) · [Mockingboard](#mockingboard) · [Floppy mechanical sounds](#floppy-mechanical-sounds)
- [Slot bus & IRQ aggregation](#slot-bus--irq-aggregation)
- [Storage](#storage) · [ProDOSHardDiskCard](#prodosharddiskcard-hdv--synthetic-block-model) · [CffaCard](#cffacard-cffa-20--mame-faithful-ide) · [SmartPortCard](#smartportcard-e-liron-class)
- [IWM (//c+ on-board)](#iwm-c-on-board)
- [SmartPort 3.5" stack](#smartport-35-stack)
- [Peripherals](#peripherals) · [SSC](#super-serial-card-slot-2--telnet-bridge) · [4play](#4play-fourplaycard) · [Workstation Card](#apple-ii-workstation-card-workstationcard) · [Z8530 SCC](#zilog-z8530-scc-scc8530device) · [Network backends](#network-backends) · [Uthernet I](#uthernet-i-cs8900a) · [Uthernet II](#uthernet-ii-w5100) · [ClockCard](#prodos-clock-card-slot-4) · [MouseCard](#mouse-card) · [Joystick / paddles](#joystick--paddles)
- [UI (ImGui)](#ui-imgui)
- [Host control center](#host-control-center-slot-configuration--floppy-emu)
- [Profile switching internals](#profile-switching-internals)
- [CLI (CliDispatcher)](#cli-clidispatcher)
- [Clock & threading](#clock--threading)
- [Package payload](#package-payload--packagingbundlemanifest)
- [WebAssembly (browser build)](#webassembly-browser-build)
- [Performance & profiling](#performance--profiling)

## CPU

Full NMOS 6502 + 65C02 (STZ / BRA / INA / DEA / PHX-PLY / BIT-imm /
TSB / TRB / JMP (abs,X), zp-indirect) + Rockwell RMB/SMB/BBR/BBS +
WDC WAI/STP (PC parks, IRQ wakes). Klaus Dormann clean.
`setCpuMode(NMOS)` re-overrides four KIL column-2 entries
($02/$22/$42/$62) back to halt, remaps the eight $xB-column
immediates ($0B/$2B/$4B/$6B/$8B/$AB/$CB/$EB) as `UnoffImm`
(2-byte NOPs), and re-points `$5C/$DC/$FC` to `UnoffAbsX`
(page-cross penalty). 65C02 undoc-NOP cycles: imm=2, zp,X=4,
abs,X=4, zp=3 ($5C left at 8). Pinned: `cmos_6502_smoke_test`,
`klaus_65c02_extended_test` (PASSES @ `$24F1`),
`cpu_cycle_count_test`. `setProgramCounter()` is the Klaus harness
back-door.

**Interrupt sampling is instruction-granular (owned deviation).** Real
silicon samples IRQ/NMI in an instruction's penultimate cycle, and
`CLI` / `SEI` / `PLP` commit their new I flag *after* that point — so an
IRQ pending across a `CLI` is taken one instruction later than naive
reading suggests, and `SEI` cannot cancel an already-sampled interrupt.
MAME's `m6502` reproduces this by inhibiting interrupts for one
instruction after those opcodes. POM2 tests the I flag at the
instruction boundary in `step()`, so in the `CLI` case it vectors one
instruction **early**. This is structural: `step()` charges `cycles` to
`advanceCycles` in a single lump, so there is no penultimate cycle to
sample at — modelling it means a cycle-stepped core. Neither Klaus nor
Tom Harte drives the interrupt lines, so nothing pins it. Impact is
confined to software counting cycles through an IRQ entry; the corpus
titles depend on the VIA/timer period instead. Not to be confused with
the VIA `syncToCpuCycle` one-instruction over-count (fixed 2026-05-25,
see [Lazy timer sync](#mockingboard)).

### Tom Harte 65x02 ProcessorTests (cycle-exact gate)

`tomharte_cpu_test <nmos|cmos> <dir>` runs Tom Harte's
[SingleStepTests/65x02](https://github.com/SingleStepTests/65x02) — 10 000
randomised single-instruction vectors per opcode, each pinning full
register+memory state + the architectural cycle count (hand-rolled JSON
scanner, no vendored lib). The core is **instruction-stepped** (`run(1)` = one
opcode) and `Memory::memRead/Write` are **non-virtual** (no per-cycle bus
hook), so we validate **final A/X/Y/SP/PC + RAM + cycle count ==
`len(cycles[])`** — the 6 architectural P flags only (B/unused are phantom,
caught indirectly via stack RAM). This is exactly the timing-bug class
`cpu_cycle_count_test` was built for, generalised to every opcode × 10 000
states.

Results below are from **full 256-opcode** sweeps of both variants
(`tests/fetch_tomharte.sh <variant> <dir> --all`), not just the gated subset:

- **NMOS 6502** (`6502/v1`): **100% on all 178 documented opcodes**
  (1 780 000 vectors), incl. decimal ADC/SBC + the NMOS `JMP (ind)` page bug.
  The 78 failing files are exactly the undocumented opcodes with observable
  side effects (SLO/RLA/SRE/RRA/SAX/LAX/DCP/ISC/ANC/ALR/ARR/XAA/SBX/SHY/SHX/
  TAS/LAS + the 12 JAMs), which POM2 models as length/cycle-correct NOP
  placeholders by design. The undocumented *NOPs* ($x3/$x7/$xB/$xF, $04/$44/
  $64, $0C/$1C/…, $80/$82/$89/$C2/$E2) do pass.
- **WDC 65C02** (`wdc65c02/v1` — the only published variant with Rockwell bit
  ops AND WAI/STP, = POM2's table): **100% on 255 of 256 opcodes**
  (2 530 000 vectors), decimal SBC included — see the interdigit-carry fix
  below. `69` (ADC) is silicon-exact on both CPUs.
- **`$5C` is the one remaining CMOS divergence, and it is deliberate**: POM2
  charges 3 bytes / **8 cycles**, matching MAME (`ow65c02.lst` `nop_c_aba` =
  7 `read_pc()` + `prefetch()`) and the standard 65C02 unused-opcode tables.
  All three of Harte's 65C02 variants say 4 cycles, but that corpus is
  generated from "an implementation that conforms to available documentation"
  (upstream README) rather than from silicon, and it invites discrepancy
  reports. **MAME wins** per this project's source-of-truth rule, so
  `5c : 0/10000` in a full CMOS sweep is expected, not a regression.

**Decimal SBC is CPU-part-specific** (fixed 2026-07-30). The WDC 65C02 lets
the low nibble's `-6` decimal adjustment **borrow into the high nibble** —
MAME names it in `w65c02.cpp:28-46` (`do_sbc_cd`): *"SBC allows interdigit
carry from decimal adjustment on 65C02"*. It packs both nibble differences
first, then applies `-6`/`-$60` to the whole byte; the NMOS part corrects each
nibble in isolation and never propagates that borrow, so **the two parts return
different accumulators for the same operands**. POM2 applied the NMOS rule on
both, costing ~3.4% of every decimal SBC addressing mode
(`e1,e5,e9,ed,f1,f2,f5,f9,fd` — not just `e9`). `M6502::SBC` now branches on
`cpuMode`. Divergence is confined to invalid BCD digits, so no correct-software
behaviour changes. Pinned by `decimal_sbc_cmos` — which carries corpus vectors
inline, so it gates the rule without the 1.4 GB download.

**Harness gotcha (fixed 2026-07-30):** `runVector` re-arms the CPU's KIL/JAM +
STP `halted` latch per vector. `step()` short-circuits before the opcode fetch
while it is set and only a reset clears it, so a single vector landing on an
NMOS JAM ($02/$12/$22/…) or a CMOS STP ($DB) used to freeze the shared CPU for
**every later vector in the run** — a full NMOS sweep scored 20 000/2 560 000
because file `02` poisoned the other 254. Curated-subset runs never noticed
(no JAM opcode is in the manifest).

Five real decimal bugs the suite surfaced + fixed in `M6502::ADC/SBC` — all
**provably identical for valid BCD**, only invalid-digit edge cases change
(cpu_cycle_count_test's decimal-SBC-V pin + Klaus stay green):
0. SBC applied the NMOS nibble-isolated correction on CMOS too, dropping the
   WDC interdigit borrow (see above). Fix: branch on `cpuMode`, CMOS follows
   MAME `do_sbc_cd`.
1. ADC low nibble `tmp+6` overflowed bit 5 on invalid digits → high nibble took
   `$20` not the `$10` carry. Fix `((tmp+6)&0x0F)+0x10`.
2. ADC decimal carry tested `tmp & 0x100`, but the `+$60` high fix can push the
   sum to bit 9 (`$240`). Fix `tmp >= 0x100`.
3. ADC CMOS V was forced to binary-overflow; WDC keeps the high-nibble-sum V.
4. SBC low nibble `tmp-6` left bit 4 (the borrow the high nibble reads)
   un-repacked. Fix `((tmp-6)&0x0F)-0x10`.

Gate: curated SHA-256-pinned subset (`tests/tomharte_*.manifest`); the
configure-time download is gated behind `-DPOM2_FETCH_TOMHARTE=ON` (default OFF
— full corpus is ~1.4 GB/CPU). `tests/fetch_tomharte.sh <variant> <dir>` pulls
a full 256-opcode variant for exhaustive runs. Pinned: `tomharte_6502`,
`tomharte_65c02`, `decimal_sbc_cmos`.

> **`tomharte_6502` / `tomharte_65c02` report `Passed` in 0.00 s when
> `POM2_FETCH_TOMHARTE` is OFF** — with no corpus on disk the harness soft-skips
> (exit 0) so CI without network stays green. A green tick from those two names
> therefore does **not** mean the CPU was validated; check the elapsed time, or
> configure with `-DPOM2_FETCH_TOMHARTE=ON`. `decimal_sbc_cmos` and
> `cpu_cycle_count` carry their vectors inline and always really run.

### Z80 core (`Z80.h/.cpp` — SoftCard/CP/M Phase 1)

Standalone Zilog Z80, first deliverable of the Microsoft SoftCard + CP/M
plan. **No Apple II dependency**: all bus traffic goes through the abstract
`pom2::Z80Bus` (memRead/memWrite/ioRead/ioWrite), so the core links alone
(the test targets compile `Z80.cpp` with zero other sources). `SoftCardZ80`
(next section) implements `Z80Bus` with the SoftCard's six-window
translation over `Memory` (MAME `src/devices/bus/a2bus/a2softcard.cpp`
dma_r/dma_w — NOT a plain +$1000 wrap).

Decoder = x/y/z/p/q field decomposition (z80.info/decoding.htm — the same
structure MAME's `z80.cpp` tables flatten): the LD and ALU matrices collapse
to two generic paths, everything else is a z-keyed switch per prefix page.
Full coverage: main/CB/ED/DD/FD/DDCB pages, undocumented IXH/IXL, DD CB
register write-back, SLL, ED NONI slots, IM 0/1/2, NMI, EI shadow
instruction, HALT, R refresh counting, documented T-state totals.
`step()` is the only execution entry point (returns consumed T-states);
budget pacing lives in the caller. Each DD/FD prefix retires as its own
4-T `step()` with interrupts deferred until the opcode
(`State::pendingPrefix`) — folding the chain into one step let a crashed
guest in a $DD/$FD sea hold the host lock unboundedly (2026-07-12 bug
hunt).

The three zexall-killer undocumented behaviours are modelled exactly, not
approximated:

- **X/Y flags** mirror bits 3/5 of whatever byte each instruction
  "publishes" — the result for most ops, the **operand** for `CP r`, an
  internal `A+value`/`A-value-H` sum for LDI/LDD/CPI/CPD.
- **MEMPTR/WZ**: `BIT n,(HL)` leaks WZ's high byte into X/Y, so WZ is
  maintained across every instruction that loads it (indexed EA, 16-bit
  loads, ADD/ADC/SBC 16, EX (SP), jumps/calls, I/O, block ops, interrupts).
- **SCF/CCF** X/Y use the "copy from A" NMOS rule (no Q-register model) —
  the behaviour zexall's silicon CRCs encode.

**Block-I/O repeat flags are a separate rule** (fixed 2026-07-30).
`INIR`/`OTIR`/`INDR`/`OTDR` do NOT leave the per-iteration INI/OUTI flags in
place: on every iteration that still has work to do (B ≠ 0 after the
decrement) the Z80 re-derives **X/Y from the rewound PC's high byte** (the
LDIR/CPIR rule), **H from B's low nibble** when carry is set, and **P/V from
the parity of B±1 (or B) xored against the incoming P/V** — plus `WZ = PC+1`.
MAME calls this `block_io_interrupted_flags()` (`z80.cpp:580-604`, invoked by
the inir/otir/indr/otdr macros in `z80.lst:769-880`). POM2 used the
non-repeating formula for all eight opcodes.

**zexdoc/zexall are structurally blind to this**: they run under CP/M and never
execute an I/O block instruction, so both stayed 100 % green while all four
repeating opcodes were wrong ~99.5 % of the time. Note MAME's `pv_val` is a
LAZY field whose getter re-parities the stored byte, so the flag that actually
lands is the *inverse* of the `(pv_old ^ pv())` it stores — P/V ends up SET when
the two agree. Transcribing that expression literally gets it backwards.
Pinned by `z80_block_io_flags`.

**Tom Harte 65x02-style Z80 suite.** [SingleStepTests/z80](https://github.com/SingleStepTests/z80)
publishes the same per-opcode JSON as the 6502 corpus (1 000 vectors/opcode,
full architectural state + a T-state bus trace), and `Z80::State` exposes
everything it pins — including WZ, I, R, IM and IFF1/2 — so the mapping is
direct. A full local sweep of **1 092 opcode files / 1 092 000 vectors**
(base + CB + ED + DD + FD + DD CB + FD CB) checking A F B C D E H L, all four
shadow pairs, IX IY SP PC WZ, I R IM IFF1 IFF2, every listed RAM cell and the
T-state count gives **100 %**, with one documented exception:

- **`37`/`3f` (SCF/CCF) and their DD/FD forms** fail on **F bits 3+5 only**
  (~22 % of vectors). That is the **Q register** gap already listed as
  out-of-scope below: the undocumented X/Y result depends on whether the
  *previous* instruction wrote F. Masking those two bits takes the sweep to
  1 092 000/1 092 000. Everything else — including all of ED's block ops and
  I/O, which zexall cannot reach — is silicon-exact.

The corpus is ~1.4 GB and only publishes *defined* opcodes (undefined ED slots
404), so it is not vendored or gated; the harness used for the sweep lives in
this session's notes rather than in `tests/`, and the two rules it found are
pinned inline instead.

Pinned by four tests (all link `Z80.cpp` only):

- `z80_core` — committed smoke, no external data: spot assertions across
  every opcode page + T-state totals + IM1/IM2/NMI/EI-shadow. Fast gate.
- `z80_block_io_flags` — the INIR/OTIR/INDR/OTDR interrupted-iteration rule,
  16 Tom Harte vectors embedded inline (carry set/clear × data bit 7 set/clear
  × both H nibble edges). No download.
- `z80_zexdoc` / `z80_zexall` — Frank Cringle's exercisers (CRCs captured
  on real Zilog silicon; zexall adds the undocumented flags). Binaries are
  configure-time downloads, SHA-256 pinned (Klaus pattern), from
  anotherlin/z80emu. Each run retires ~46.7 G T-states (~50 s native,
  label `slow`). **Both pass 100 %** (67 + 67 blocks OK). zexdoc alone is
  NOT a sufficient gate: it masks X/Y, and the one bug it missed was
  exactly a BIT n,r X/Y rule (X/Y copy the full register, not the masked
  result) that only zexall's CRCs caught.

Out of (current) scope: intra-instruction bus-cycle timing (irrelevant
for the bus-master SoftCard design), the Q register (the only thing standing
between the Harte sweep above and a clean 100 % unmasked — **rule fully
characterised below** if it is ever worth doing), IM0 arbitrary-opcode
injection (only RST assumed — CP/M runs with interrupts off).

**The Q register rule, should SCF/CCF exactness ever be wanted.** Derived and
validated against `z80/v1` at **1000/1000 on each of the six affected files**
(`37`, `3f`, `dd 37`, `fd 37`, `dd 3f`, `fd 3f` — 6 000 vectors):

```
Q = F as left by the PREVIOUS instruction, if that instruction wrote F;
    0 otherwise.
SCF / CCF:  F bits 3+5  =  ((Q ^ F_before) | A) & 0x28
```

The DD/FD-prefixed forms are the same rule with **Q = 0**: each prefix retires
as its own instruction (`State::pendingPrefix`) and does not write F, so it
clears Q. `(F_before | A) & 0x28` is 1000/1000 on all four prefixed files.

Note MAME's own expression (`z80.lst:6427-6470`,
`m_f.yx_val = (m_f.yx_val & Q) | A`) does **not** transcribe literally — its
`Q` is a derived value, not a raw mask, and taken at face value it scores only
548/1000 on `37`. Same lazy-field trap as `pv_val` in the block-I/O fix.

Cost of adopting it: a `q` byte in `Z80::State` (snapshot version bump) is the
easy part; the invasive part is that **every** instruction path must then
maintain q — set it to F when it writes F, clear it otherwise, prefix
retirement included. That is a change to every opcode's epilogue for undocumented
flag bits no CP/M or Apple II software reads, which is why it stays out of scope.

### SoftCard Z80 (`SoftCardZ80.h/.cpp` — CP/M Phase 2)

Microsoft SoftCard, ported from MAME `src/devices/bus/a2bus/a2softcard.cpp`
(R. Belmont, 176 lines; line refs in the source). Catalog key `softcard`,
no ROM to probe (the hardware has none — the CP/M boot disk finds the card
by toggling slot windows). Three hardware facts drive the whole design:

- **The toggle is a `$CnXX` WRITE** (MAME `write_cnxx`, :88-109), not a
  DEVSEL access — reads of `$CnXX` float. Grant side releases the Z80's
  WAIT line and raises slot DMA; release side re-asserts WAIT, so the Z80
  **freezes in place and resumes exactly there** on the next grant. Only
  the *first* grant after a bus reset resets the Z80 to PC=$0000
  (`m_FirstZ80Boot`). The Z80 releases the bus itself by writing its own
  `$CnXX` through the $E000 window (6502 $Cn00 = Z80 $En00).
- **Six address windows** (dma_r/dma_w, :111-176), NOT a plain +$1000
  wrap: Z80 $0000-$AFFF→$1000-$BFFF, $B000-$BFFF→**$D000** (LC),
  $C000-$CFFF→$E000, $D000-$DFFF→$F000, $E000-$EFFF→**$C000 (I/O)**,
  $F000-$FFFF→$0000 (zero page). CP/M gets RAM at Z80 $0000 and its BIOS
  sits on the Language Card. All accesses go through
  `Memory::memRead/memWrite` — the real bus — so soft-switch side
  effects and LC/aux paging behave identically for both CPUs.
- **Z80 clock = 2× the 6502** (:41). `dmaRun` converts 2 T-states → 1
  6502 cycle (odd-T carry kept across slices) and feeds
  `Memory::advanceCycles` per Z80 instruction, so **emuCycles never
  leaves the 6502 domain** — video event log, Disk II LSS and audio
  pacing are CPU-agnostic.

**Arbitration** is a generic DMA daisy-chain hook, not SoftCard-specific
(mirrors MAME's a2bus DMA): `SlotPeripheral::dmaActive()/dmaRun()` +
`SlotBus::dmaClaimant()` (lowest slot wins), consumed by
`EmulationController::runCpuSlice` — the single point both `workerLoop`
and `tickFrame` now route their 4096-cycle chunks through. Hand-over is
instruction-precise in both directions: the granting `STA $CnXX` calls
`M6502::stop()` so the in-flight `run()` chunk ends at that instruction
boundary (`run` re-arms `running` on the next call), and `runCpuSlice`
gives the chunk remainder to the other CPU instead of burning dead time.
Single-step (`Mode::Step`) deliberately keeps stepping the 6502 — the
debugger is 6502-centric.

Snapshot: the full Z80 register file + enabled/firstBoot/T-carry go into
the card's `SLOTn` blob (magic `SFZ2`, hand-packed little-endian, foreign
blobs ignored). The rewind ring captures slots, so bus ownership
round-trips through rewind; **file snapshots don't** (`includeSlots=false`
by design) — `restoreMachineState` therefore force-disarms any live DMA
claimant before restoring, so loading a snapshot mid-CP/M can't leave a
stale Z80 executing over the restored RAM (2026-07-12 bug hunt). A file
snapshot *saved* mid-CP/M still won't resume the session (the parked-6502
continuation runs without the Z80's results) — inherent to slot-less
snapshots, same category as the excluded disk state. The Apple IRQ line
is **not** wired to the Z80 (matches MAME; CP/M polls).

Two behaviours that look like bugs and are hardware-faithful (2026-07-12
bug hunt, verified against UTAIIe 5-28 + MAME source): (1) on IIe-class
profiles a Z80 write through the $E000 window to $C007 (SETINTCXROM)
wedges the machine — INTCXROM masks $CnXX writes, so the Z80's own
release toggle can no longer reach the card until RESET. Real IIe MMU
inhibits I/O SELECT' the same way; MAME delivering write_cnxx under
INTCXROM is *its* view-banking simplification, not oracle behaviour.
(2) Any $CnXX write toggles the bus — including AI `/mem` pokes sweeping
the card's slot page (the endpoint is deliberately bus-faithful); an
agent that wedges the machine this way recovers via `/reset`.

Pinned by `softcard_toggle`: window math edges, write-toggles/read-doesn't,
first-boot vs resume semantics, Z80 executing from translated RAM +
zero-page window write, snapshot round-trip, and a full
`tickFrame` 6502→Z80→6502 frame through `runCpuSlice`.

**CP/M boot (Phase 3) — WORKS.** Two end-to-end gates (media-gated, skip
when absent, ROM-test pattern), both booting to a live `A>` in ~11 M
cycles:

- `softcard_cpm_boot` — II+ 40-col: `disks_5.4/dsk/cpm22.dsk` = the
  "Softcard 16-sector disk (Microsoft 1980)" 44K v2.20 master (Asimov
  `images/cpm/os/`).
- `softcard_cpm_boot_iie` — //e + IIe paging: `disks_5.4/dsk/cpm60k.dsk`
  = 60K v2.23. Validated against the MAME `apple2ee -sl4 softcard`
  oracle — banner byte-identical.

Sysgen gotchas the bring-up surfaced (they *look* like emulation bugs and
are not): the 56K/60K sysgens print through the **IIe 80-col firmware**,
which stores even display columns in AUX $0400 — a main-RAM-only screen
scrape sees every other char missing ("Sfcr PM"); and on a II+ those same
sysgens write $00s (wrong machine class — they need the IIe console, MAME
behaves identically). The 44K master is the correct II+ image. The boot
also exercises the LC heavily: 56K/60K CBIOS lives in the Z80's
$B000-$DFFF windows = 6502 $D000-$FFFF Language Card RAM.

## Memory

### What lives outside Memory now

`Memory` is still the bus, the paging and the RAM, but three concerns that
used to swell it were split out (one concern per file):

- **`Keyboard.h/.cpp`** — the keyboard latch and the host paste FIFO. The
  hot `$C000` read is `keyboard_.latchMirror()` (a lock-free atomic
  republished under Keyboard's own mutex); the cold IIe status reads
  ($C011/$C012/$C019) use `keyboard_.lastKey7()`. `Memory::pasteText`
  forwards with `foldToUpper = !iieMode`.
- **`PaddleInputs.h`** — the game port: paddles, buttons, Open/Solid-Apple +
  Shift, and the $C070 RC-discharge latch. The $C061-$C067 read path calls
  `paddles_.button0/1/2()` and `paddles_.discharging(idx, cycleCounter)`;
  the snapshot serialises `paddles_.latchCycle()`.
- **`MemoryWatch.cpp`** — the debugger's write/read watchpoint bookkeeping
  (the hot-path halves stay in Memory; see § Debugger).

Pinned behaviour-preserving by `input_io_smoke`, `paste_smoke` and
`ui_worker_contention` (2026-08-23).

### `loadAppleIIRom` dump shapes

- **16 KB**: `$C000-$FFFF` direct (MAME/AppleWin).
- **20 KB** II+ system pack: 4 KB filler skipped (loader). Pinned
  `system_profile_smoke::test20kIIPlusRomLoad`.
- **32 KB** //e "system+video": firmware at offsets `0x4000-0x7FFF`;
  lower half = video/charset (`loadCharRom`). `pickLower16KFor32K=false`.
- **32 KB** //c/+ dumps: TWO 16 KB banks side-by-side (bank 0 lower
  = cold-reset entry; bank 1 upper = alt firmware via `$C028`
  ROMBANK). `pickLower16KFor32K=true`; upper stashed into
  `IIcClassProfile::altFirmware_`. Both halves carry valid-looking reset vectors —
  profile is source of truth.

### //c-class detection (MAME `apple2e.cpp:1275-1299` content probe)

- `payload[0x3BC0]==0x00` → //c-class (`Memory::iicProfile_` set;
  forces INTCXROM on every reset; //c has no physical slots). Fires
  for both 16 KB rev-255 AND 32 KB rev-0/3/4/X //c+ dumps.
- `payload[0x3BBF]==0x05` (after //c match) → //c+
  (`IIcClassProfile::isPlus_`; gates on-board IWM + MIG). Plain //c uses `A2BUS_DISKIING` at
  slot 6 (MAME `apple2c()` `apple2e.cpp:5168-5188`).
- `IIcClassProfile::hasAltBank_` is narrower: true only on 32 KB dumps
  providing an alt-firmware bank. 16 KB rev-255 //c has `isIIcClass=true`
  but `hasAltBank_=false`.

### MemoryProfile (//c-class strategy)

All //c/+ memory quirks behind `MemoryProfile`
(`MemoryProfile.h` + `MemoryProfile_IIcClass.{h,cpp}`).
`Memory::iicProfile_` is **null on II/II+/IIe** → one `if
(iicProfile_)` branch on the hot path, zero virtual calls. Profile
owns: alt firmware (16 KB), ROMBANK flag, //c+ flag, 2 KB MIG
gate-array (`migRead`/`migWrite`, verbatim MAME
`apple2e.cpp:532-624`), IWM/SmartPort hub pointers. Dispatcher
delegates: `forcesIntCxRom`, `romBankToggle($C028)`,
`onResetSoftSwitches`, `ioReadIWM/ioWriteIWM ($C0E0-$C0EF)`,
`internalRomRead/Write` ($C100-$CFFF under INTCXROM, incl. //c+ MIG
`$CC00/$CE00` + alt-firmware bank 1), `languageCardRomRead`
($D000-$FFFF alt firmware). `Memory::setIWM/setSmartPortHub/
setIWMAuthoritative` are façades. What stays in Memory: `ioudis`
(shared with //e), `intC8Rom`, LC/paging/generic soft switches.
Pinned: `iic_boot_trace`, `iic_nodisk_boot_trace`,
`iicplus_boot_trace`, `system_profile_smoke`, `iwm_device_smoke`.

### IIe paging

`setIIEMode(true)` MUST be called BEFORE `loadAppleIIRom` (loader
split depends on the flag). Adds aux 64 KB, 4 KB `internalIORom`
for `$C100-$CFFF`, aux LC bank trio. `$C000-$C00F` switches update
`iieMemMode` bitmask; per-range routing (ALTZP `$00-$01FF`,
RAMRD/WRT `$02-$BFFF`, 80STORE+PAGE2 swap on `$04-$07FF` +
`$2000-$3FFF` when HIRES). All IIe paths gated behind `iieMode`.
Pinned: `iie_memory_smoke_test`.

### RamWorks III

Verbatim port of MAME `bus/a2bus/a2eramworks3.cpp`. Tiers 1/4/8/16/
48/128 (8 MB cap). Bus (MAME `:108-115`): writes to
`$C0n1/3/5/7` (predicate `(low & 0x09) == 0x01` over
`$C070-$C07F`) latch `bank = data & 0x7F`. Same accesses still
pulse paddle one-shot mirror.

Storage = `ramWorksBacking_`, one 80 KB slot per bank
(`kRamWorksBankStride = 0x10000 + 0x1000 + 0x1000 + 0x2000`).
Visible aux* arrays always hold the active bank (`Apple2Display`
caches `auxData()` once). `ramWorksSwapToBank` memcpys
visible→backing[prev] then backing[curr]→visible.

Bank clamp `(data & 0x7F) % ramWorksBanks_` (MAME doesn't clamp,
allocates 8 MB always). IIe-only: `setIIEMode(false)` releases
backing. Wired in `applyProfile` between `setIIEMode(true)` and
`loadAppleIIRom`. Pinned: `ramworks_smoke_test`.

### Soft switches

Read OR write triggers. `$C030-$C03F` (speaker), `$C050-$C057`
(display modes) and `$C040` (game-port STRB) all do their side effect
on a READ **and** return `floatingBus()` — every undriven `$C0xx`
read must hand back the video-scanner byte, like real hardware (MAME
`apple2.cpp do_io`). A hard 0 here hangs vapor-lock poll loops: DROL's
cut-scene spins on `LDA $C050 / CMP #$80` (three consecutive scanner
reads via a display soft switch) and never unlocked while `$C050-$C057`
returned 0 — the same hang LinApple had, fixed in AppleWin 1.13.0 with
the floating bus, fixed here 2026-06-10 (pinned `vapor_lock` §(d)).
`$C061-$C067` are paddles + buttons on II/II+ — NOT cassette aliases
(only `$C020`/`$C060` are).

**Open-Apple/Solid-Apple** OR'd into $C061/$C062 bit 7 alongside
joystick buttons (MAME `apple2e.cpp:2157-2169`); wired to host
Left/Right Alt (`Memory::setOpenAppleKey/setSolidAppleKey`); GLFW
key callback routes those even when ImGui has focus.

**IOUDIS** (`$C07E` SET / `$C07F` CLR; on //c the whole
`$C078-$C07F` range decodes SET/CLR by parity — every even address
sets, every odd clears). Init `true` every reset (MAME
`apple2e.cpp:1224`). Writes effective only on //c-class
(`Memory::iicProfile_` non-null; MAME `:2569-2587` gates `m_isiic`). Read
`$C07E` on any IIe-class returns bit-7 = ioudis state (MAME
`:2276-2278`).

**LC reset state**: `lcWriteEnable=true`, `lcReadRam=false`,
`lcBank2Active=true`, `lcPrewrite=false` (Sather Fig 5.13; MAME
`apple2e.cpp:1227-1232 + :1492-1497`). Applied universally.

### Power-on RAM pattern

MAME-faithful `00 FF 00 FF …` fill (`Memory::clearRam()` on user
RAM + LC + aux + RamWorks). MAME refs: `apple2.cpp:294-298` (II/+),
`apple2e.cpp:1014-1035` (IIe). Done at power-on / profile switch /
cold boot only; soft + hard resets preserve RAM.

### Text/HGR row interleave (Woz DRAM-refresh trick)

- text: `addr = base + 0x80*(y%8) + 0x28*(y/8)`
- HGR:  `addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64)`

### Keyboard

Latch + strobe under `kbMutex`. UI `queueKey()` sets strobe high.
CPU reads `$C000` via `softSwitchAccess()` (same mutex). Strobe
stays high until `$C010`.

### Reset architecture

- **`resetSoftSwitches()`** — full reset: display state, LC flags,
  `iieMemMode`, `intC8Rom`, `iicRomBank`, IOUDIS=true, RamWorks
  bank 0. Forces `MF_INTCXROM` on //c-class (`iicProfile_`). Called by
  `coldBoot()`, `applyProfile` step 4, and
  `resetSoftSwitchesWarm()` when `iieMode` is on.
- **`resetSoftSwitchesWarm()`** — Ctrl-Reset / F12 on II/II+; F11/F12
  on IIe. On `iieMode` delegates to full reset (MAME `apple2e.cpp:1453-
  1508`). On II/II+ does only keyboard-strobe clear — **LC + display
  switches survive** (MAME `apple2.cpp:325-331`). `hardReset()` uses
  this path too (only CPU A/X/Y are zeroed).

CPU side: `M6502::hardReset()` doesn't wipe stack `$0100-$01FF`
(MAME `reset_w` doesn't touch RAM); `M6502::softReset()` decrements
SP by 3 (faked-BRK reset semantic).

### Test/debug write helpers

`Memory::dataMutable()` is gone — the raw pointer let a stray poke
silently clobber ROM. Replacements: `writeRamUnchecked(addr, val)`
(`assert(addr < 0xC000)`, bypass IIe paging → main bank) for
targeted RAM pokes; `loadFlatTestImage(src, len)` (asserts
`testMode == true`) for Klaus 64 KB bulk loads.

## Display

Pure software renderer into 280×192 (or 560×192 in IIe 80-col)
RGBA. Reads `Memory::getDisplayState()` (mutex copy) + flat RAM.
UI uploads via `glTex(Sub)Image2D`. Text flash via
`frame_number() & 0x10` (MAME parity).

Ten `HiResMode`:
- `ColorNTSC` — 7-bit sliding artifact window →
  `kArtifactColorLut[2][128]` (verbatim MAME
  `apple2video.cpp:376-419`) rotated by `rotl4b(absX)` → lo-res
  palette; 560 sub-pixels pair-averaged to 280 (MAME
  `composite_color_mode=0`).
- `ColorCompMedium` (=1), `ColorComp4Bit` (=2, no artifact).
- `ChatMauveRGB` — only with `LeChatMauveCard`.
- `ColorCompositeOE` — OpenEmulator-style true NTSC simulation
  via GLSL shader (see § Composite NTSC shader below).
- `ColorCompositeOECpu` — the same OpenEmulator composite demod run
  on the CPU into the RGBA framebuffer (no GLSL fallback). Honours the
  same demod knobs as the GPU shader (hue / Sharpness / PAL / textSharp,
  mirrored via `setOeDemodParams`); pinned pixel-identical by
  `oe_demod_gpu_cpu_parity`.
- `MonoWhite` / `MonoGreen` (P31) / `MonoAmber` (history-buffer
  lerp).
- `ColorAppleWin` — AppleWin-style IIR-based NTSC simulation
  via 4-phase × 4096-entry CPU LUT (see § AppleWin NTSC below).

The deep per-mode comparison with each origin source — algorithm
provenance, deviations, pinned tests and side-by-side captures —
lives in [`docs/graphics_modes_comparison.md`](docs/graphics_modes_comparison.md).

### Character generators, and the Videx LOWER CASE CHIP

**French Touch custom char ROM + AN2 dual-bank** (2026-09-02): the Unenhanced
//e the French Touch corpus targets has no MouseText, so demos that draw block
art bring their own character generator. "Block ASCII Anthology" ships
`eprom2164.bin` (8 KB) and uses TWO fonts — a normal-text intro and block-glyph
art — switching between them at runtime the way a localized //e does: the char
ROM's A12 is wired to **annunciator 2**, so `$C05C` (AN2 off) / `$C05D` (AN2
on) flips the whole font (apple2history.org ch.12; the Japanese katakana toggle
is the canonical case). `Memory::loadCharRom` with a sentinel `bank = -1` keeps
both 4 KB sets (each normalised); `charRomActiveData()` / `charRomActiveSize()`
return the live set from the AN2 state, and the text renderer + frame-cache key
follow it so an AN2 flip re-renders. Catalog entry `//e — French Touch (Block
ASCII custom)` (key `iie_ft_block`, `roms/apple2e_char_ft_blockascii.rom`).
A plain 4 KB ROM leaves AN2 a no-op (as on a US //e); the 342-0274-A FR/US
entries stay single-bank (`bank` 0/1). Pinned by `char_rom_catalog`.

Text glyphs come from a dumped character ROM (`Memory::loadCharRom`,
selectable per locale from `CharRomCatalog`). Two properties of a dump are
NOT knowable from its size, and POM2 used to guess both from it:

Both now live in `CharRomDump.h/.cpp` — a fact about FILES, not about the
memory map, and worth its own translation unit because two dumps of the same
size can need opposite treatment. `Memory::loadCharRom` opens, sizes and (for
an 8 KB international part) picks the bank; `pom2::normaliseCharRom` decides
the rest.

**The inverse/flash range.** In the 4 KB //e dumps and AppleWin's
`Apple2_Video.rom`, bit 7 of each byte marks the range that renders inverse.
The Videx dump never sets bit 7 at all, so that marker is absent and the
range has to be split by OFFSET (the first 512 bytes) instead. `loadCharRom`
now scans for the marker once and picks the rule accordingly. Getting this
backwards inverts the entire normal range — and because the glyph SHAPES are
unaffected, it is invisible in a screenshot of ordinary text.

**Whether there is lowercase.** POM2 previously read "2 KB" as "no
lowercase" and folded a-z onto A-Z at render time
(`Apple2Display::lookupCsbitsGlyph`). That is correct for the stock
II/II+ generator and exactly wrong for the **Videx LOWER CASE CHIP**
(1980) — a drop-in replacement for the motherboard generator, also 2 KB,
whose entire reason to exist is those glyphs. The test comes from the Videx
manual's own description of the stock part: *"Characters 80 — BF are
identical to characters C0 — FF"*. A generator that adds lowercase breaks
that equality, so `loadCharRom` compares the two 512-byte blocks and
publishes the answer as `Memory::charRomHasLowercase()`; the display folds
on that, not on the size. Pinned by `videx_lowercase_char_rom`, which runs
both dumps and checks the fold, the bit order and the ranges.

Catalog key `videx_lc`, file `roms/Videx Lower Case Chip ROM.bin`. The chip
is a II/II+ part: on a //e lowercase is already in the machine.

### DHGR (IIe, `eightyCol && hiRes && dhgr && !textMode`)

`renderDhgr` interleaves aux (dots `c*14..+6`) with main (`+7..+13`)
per byte → 560-dot stream. Three color paths, matching MAME
`apple2video.cpp`:

- **`ColorNTSC`** — composite artifact: 7-bit sliding window over
  560 dots → `kArtifactColorLut[128]` → `rotl4b(value, absX+1)` →
  4-bit lo-res palette. `+1` = MAME `is_80_column=1` in
  `render_line_artifact_color`. Per-pixel decode.
- **`ChatMauveRGB`** — the RGB card's clean decode; every rule is the
  card's, see [§ Le Chat Mauve](#le-chat-mauve-lechatmauvecard) below.
  `renderDhgr` asks `chatMauve->dhgrMode()` and paints COL140 (4-dot cells,
  nibble rotl 1 → `kChatMauveLoResPalette`), BW560, the per-dot **mixed**
  mux, the Video-7's 160 chunky, or hands the Eve's COL280A/B / CP280 /
  blank to their own painters.
- **`Mono*`** — luminance × tint; persistence sized for 280-wide
  HGR.

Mixed = DHGR top 160 + 80-col text bottom 4 rows.

**Colour TEXT with an RGB card** (`renderTextChatMauveFgBg`): 40-col,
char code from main, the aux byte at the same address holds the cell's two
lo-res colours — the Video-7 puts the foreground in the high nibble, the Eve
(TXT16) the background; `auxHiIsForeground` picks. 7-bit glyph doubled to
14 dots. Port of MAME `text_update` (`:788-791`) + `render_line_color_array`
(`:571-583`) for the nibble math.

Pinned: `dhgr_render_smoke_test`, `video7_parity_smoke_test`,
`chatmauve_dot_rules`, `le_chat_mauve_smoke`, `dhgr_phase_signal_test`,
`dlgr_render_smoke_test`, and the `cm/<variant>/…` block of
`display_golden_hash_test`.

### Le Chat Mauve (`LeChatMauveCard`)

The French RGB adapters and their US cousin, as ONE card with a **variant**
(`chatmauve_variant` = `feline` | `iic` | `eve` | `video7`; //c-class
profiles default to `iic`, the others to `feline`). The research — manuals,
the Video-7 patent, the Eve's PLA dump, fenarinarsa's measurements, and
Purplesoft's own code — is `docs/chatmauve_plan.md`; this section is the
model as built (P0-P2, 2026-09-01).

**Division of labour.** The card owns the *state* — the patent's 2-bit mode
latch (AN3 clocks 80COL; POM2 clocks 80COL where AppleWin clocks /80COL, so
the enum numbering is the bit-inverse of AppleWin's for the same
`STA $C05E/$C05F` sequence), the Eve's eight switches and CPREG — and
answers three questions: `dhgrMode()` (what DHGR means after the variant's
fallbacks and the Eve's table IX-1), `hgrMode(an3On)`, `textMode(eightyCol,
an3On)`. `Apple2Display` routes on those answers (`renderInternalBandImpl`)
and owns the pixel rules — the Chat Mauve painters live in their own TU,
`Apple2Display_ChatMauve.cpp`, with the page/band helpers both TUs share in
`Apple2Display_Internal.h` (the file-size ratchet's first recorded win on
`Apple2Display.cpp`, 2713 → 2486 lines). `renderStateKey()` folds everything
that changes pixels without a video event into the text-frame skip key.

**The Féline / //c adapter rules** (== AppleWin `RGBMonitor.cpp`, itself
validated by fenarinarsa on a real //c adapter; pinned dot for dot by
`chatmauve_dot_rules` against ports of `UpdateHiResRGBCell` and
`UpdateDHiResCellRGB`):

- *HGR*: 140 cells of 2 dots aligned to the line, 3-dot window; `010` /
  `101` → the cell's colour (bank = bit 7 of the dot's OWN byte), anything
  else → its own bit. No half-dot shift, no fringing. The latch does not
  touch single HGR (AppleWin never consults it there — POM2 used to make HGR
  mono under BW560; gone). AN3 off → **mono** (`POKE -16290,0`).
- *DHGR mixed*: per-BYTE 560/140 mux over a free-running 4-dot cell latch.
  A colour cell that runs into a BW byte is **cut**; a BW byte that runs into
  a colour byte has its **last dot repeated** to the next cell boundary. The
  cell's colour is the raw stream's nibble on the grid wherever its bits
  come from. MAME's byte-level rule (partial cell painted from the mixed
  nibble) is what this replaced. `invertBit7` (Dragon Wars) flips the
  per-byte selector here and nowhere else.
- 160 chunky → COL140 (no such mode on the Chat Mauve boards).

**The Video-7** keeps the four patent modes (MAME `dhgr_update` rgbmode 0-3,
`video7_parity_smoke`), F/B text (AN3 on, 80COL off, hi = fg) and F/B HGR
when AN3 is off (MAME `hgr_update`'s `rgb_monitor && m_dhires && !m_80col`).

**The Eve.** Sixteen write-only switches at `$C0B0-$C0BF` (slot 3's window —
`Memory` forwards it through `SlotBus::broadcastVideoSwitch{,Write}` only
while slot 3 holds no foreign card): any access decodes the address, a write
also latches the byte into **CPREG**, all off at power-on, Ctrl-Reset clears
them unless LOCKRES. Modes from table IX-1 as **Purplesoft's `& GR 1..10`
switch tables** select them (`PURPLESOFT*` rev B, runtime `$E06F-$E0A0`):
AN3 on — 000 HRAPPLEII, 100 SPEC1, 110 SPEC2, 001 DASH, 011 HRBW; AN3 off —
000 COL140 (whatever the patent latch says — `& GR 6` leaves it at 00 and
expects COL140), 100 COL280A, 010 COL280B, 110 blank, 111 CP280 — **with
80COL off**, as Purplesoft runs it: the Eve is the aux memory and has the
attribute byte regardless, only the 560-dot modes need 80COL's doubled shift
rate — and **011 BW560** (the manual's scan read "HR3 alone"; the code says
HR2+HR3, the same pair as HRBW). Mixed and 160 → COL140. TXT16 =
colour text with **hi nibble = background**, 80COL off, no AN3 condition;
TXTGREEN = a white → green pass over the text rows (40 and 80 col, the
mixed-mode band too). CP280 = the fg/bg HGR painter with the Eve's nibble
order; COL280A/B = the 560 stream in **2-dot cells** (code = dot + 2·next),
read off the bytes Purplesoft's `& PLOT` writes (plan § 6, closed);
SPEC1/SPEC2 = the LCM rule minus an isolated colour dot on white (`11011`)
and, for SPEC2, on black (`00100`) — 5-dot window, from the manual's prose,
to confirm against the PLA (P3); DASH renders as HRAPPLEII for now.

**CPREG auto-write** is a `Memory` hook, not a video rule: `setAuxShadow`.
While TXT16 (text page) or ENHRCPREG (HGR page) is on and LOCKCPREG off, a
CPU write that lands in MAIN inside the page also deposits CPREG in AUX at
the same address — `PRINT` in colour, HPLOT in CP280, without the program
touching aux. Zero cost on the hot path: arming clears `writable[]` over the
page so `memWrite`'s own test sends the write to `memWriteSlow` (the write
watchpoints' trick); `ramWritable()` reports the page writable, and the two
diversions are pinned to coexist (`le_chat_mauve_smoke` § 7). The card
programs the hook from its switches (`setMemory` at plug time, disarmed on
unplug).

**Snapshot** blob v3 = latch + switch byte + CPREG; a v2 blob's two toggles
land on TXT16 / TXTGREEN (they were those switches under wrong labels). The
variant and `invertBit7` are user settings and stay out.

**UI**: the model is chosen in **Slot Configuration** — a "model" combo
under any slot row set to Le Chat Mauve, staged and applied like the slot
itself (persists `chatmauve_variant`); on a //c-class profile the row says
the model is fixed by the DB-15 connector (Adaptateur IIc — enforced at plug
time, whatever the setting says). The Chat Mauve panel also shows a live
variant combo, plus the latch, what the card decodes, and on the Eve the
eight switches (a click is a `STA $C0Bx`) with CPREG and whether the aux
shadow is armed.

**Beam-raced latch** (plan P6, first rung): the card appends a timestamped
(cycle, fifo-before/after) edge to a small ring at every clock, and
`forEachBeamSegment` replays the latch alongside DisplayState from the same
event log, so a mid-frame `$C05E/$C05F` reclock paints each band with the
latch of its own moment (`renderDhgr` reads the band's value through
`bandLatch_`/`dhgrModeFor`). Pinned by `chatmauve_latch_split`.

**Not yet** (plan P3-P6): the Eve's `$C0Bx` switches still land per frame
(they push no video events), the PLS100 is decoded but is a dot router —
its colour semantics live downstream (plan § 3.5.1, closed as bounded);
the RVB Graph variant models only its four documented `$C0F0-$C0F3` mode
strobes (colour/mono × white/green text — the colour registers need the
manual); the //c adapter's inferred-80COL quirk has no grounded mechanism
on record. `tests/purplesoft_eve_probe.cpp` boots the maker's demo disks
(Purplesoft, Extasie, Arlequin via `POM2_PROBE_DISK`) for the visual
check (see `docs/test_corpus.md` § 5).

### DLGR (IIe, `eightyCol && !hiRes && dhgr && !textMode`)

`renderLoResDouble` — 80 cells, aux nibble `rotl4(NIBBLE(aux),1)` +
main nibble, 560-wide frame80. Mixed = DLGR top 40 block-rows + 80-col
text bottom 4 rows. Pinned: `dlgr_render_smoke`, goldens
`iie/dlgr` + `iie/dlgrmixed` in `display_golden_hash_test`.

### Beam-racing (mid-scanline soft switches)

`Memory` logs display soft-switch edges (`$C050-$C057`, `$C05E/$C05F`,
IIe `$C00C/$C00D` 80COL, `$C000/$C001` 80STORE, `$C00E/$C00F` ALTCHAR)
with CPU-cycle timestamps. `Apple2Display::render()` replays events per
scanline band via `renderInternalBand` when the log is non-empty;
otherwise it takes the fast single-state path. That state is
`getDisplayStateAtFrameStart()` from the first published frame onwards —
the bare `getDisplayState()` survives only as the pre-first-frame fallback
(`frameCounter == 0`, which is how the display tests drive the class with no
clock). **A stopped machine publishes no frames**, so since 2026-09-07 both
paths run the frame-start state through
`Apple2Display::applyIdleSwitchOverride` (`Apple2Display.cpp:411-426`): while
`cpuIdle_`, it diffs the live `mem.getDisplayState()` against the snapshot
taken at the last run (`liveStateAtRun_`) and folds only the changed mode
fields — `textMode, mixedMode, page2, hiRes, eightyCol, an3, altChar, dhgr,
eightyStore` — onto the published state. Without it a `$C051` poked from the
debugger or the memory editor changed the content but never the mode.

**Per-video-frame publication (not per-tick)** *(2026-06-10)*. Recording is
continuous: `Memory::advanceCycles` **publishes** the completed
`{displayAtFrameStart_, events}` pair at each video-frame boundary (65 × 262 NTSC /
312 PAL cycles), and `takeVideoEvents()` returns a *copy* of the last published
frame. This replaced an earlier model that opened the log per worker CPU tick
(`beginVideoEventFrame`) and let the UI *steal* it at vsync — under PAL the
worker runs 50 Hz and the UI 60 Hz, so ~1 UI render in 6 fell twice inside one
tick and saw an **empty** log (→ `renderInternal`, no splits) → mid-scanline
effects (French Touch *Mad Effect*) flickered at the 50/60 beat. Publishing on
the video-frame boundary decouples the log from both the worker's CPU budget
(17045/20313 ≠ one video frame) and the UI's vsync; the UI re-renders the same
published frame when no new one exists (replay is deterministic + idempotent). A
reset purges both logs (no ghost replay against the wiped state). The legacy
synchronous bracket (`beginVideoEventFrame` + `takeVideoEvents` *moves* the log,
gated by `legacyEventBracket_`) is kept for the headless render tests. Pinned by
`video_event_publish`.

**Double-buffer page flips vs beam-raced page splits** *(2026-06-10)*. Replay
reads RAM at *render* time, not *beam* time — correct only while RAM is static
across the frame. Double-buffer games (DROL flips `$C054/$C055` every ~4 frames,
unsynced, drifting through the visible band) break that: the band above a
mid-frame flip would render from the page the game is **already redrawing**
(half-erased sprites → strong flicker, worse than real hardware's subtle tear).
`forEachBeamSegment` detects this — a frame whose PAGE2 events all go ONE
direction is a buffer flip → apply the final page **frame-wide** (the displayed
page at frame end is the freshly completed buffer, exactly what RAM holds) and
drop the events; a frame that flips BOTH directions (DIX MODPAGE: page 1 left,
page 2 right of the same line) keeps the exact replay. Pinned by
`drol_pageflip_render`; `dix_modpage_split` unchanged. Known trade-off *(🟢)*: a
single intentional one-direction mid-frame page split renders full-page — the
real fix is MAME-style incremental scanline rendering.

**Composite signal also beam-races.** `render()` now takes the event log
*once* and hands it to `fillCompositeSignal(mem, events)` as well as the RGBA
path, so mid-scanline switches land in the 14.318 MHz waveform the composite
modes (`ColorCompositeOE` GPU, `ColorCompositeOECpu`, `ColorAppleWin`)
consume — not just the LUT framebuffer. `fillCompositeSignal` drives the SAME
`forEachBeamSegment` decomposition the RGBA path uses (see below): it zeroes
`signalBuf`, starts from `getDisplayStateAtFrameStart()` + `applyIdleSwitchOverride`, and for each band ×
column segment sets the mutable local `state` (the per-mode paint helpers
capture it by reference) and calls `paintSignalBand(y0, y1, col0, col1)`,
reusing the same `bandRows`/`bandScanlines` clipping as `renderInternalBand`.
Empty log → `paintSignalBand(0, 192, 0, 40)`, byte-identical to the old
whole-frame dispatch (the OE GPU/CPU parity goldens are unchanged). Caveat:
`signalPhaseOffset_` stays one per-frame demod constant (last graphics band
wins), so a mid-frame HGR↔DHGR phase split is a documented approximation;
lo-res bands clip at block-row (4-scanline) granularity, same as the RGBA path.
Pinned by `beam_race_composite` (vertical TEXT/HGR split at scanline 96) and
`horizontal_split_composite` (per-scanline column strip → HGR waveform left,
TEXT waveform right, same line).

**Horizontal (mid-scanline-column) splits** *(RGBA done 2026-06-09; composite
done 2026-06-09)*. Both replays now resolve switches **per byte column**.
`VideoEvent.emuCycle` (`Memory.h:320`) already carries the CPU cycle — only the
horizontal position was discarded — so `Apple2Display::frameCycleToPos(emuCycle)`
maps it to `{scanline, byteCol}` with `byteCol = clamp((emuCycle % 65) − 24, 0,
40)` (the 40-byte visible window opens at horizontal cycle 25; the −24 is one
cycle earlier because the scanner latches a byte in phi1 of the cycle whose phi2
the CPU uses, calibrated on MAD EFFECT's `$C055`). **Per-kind offset**
(`Apple2Display_Beam.cpp::beamColForEvent`, 2026-09-02): that −24 is right for a
*fetch-side* switch — PAGE2 `$C054/$C055`, which picks the address read on the
NEXT fetch — but a *display-side* DHIRES/AN3 switch (`$C05E/$C05F`, the DHGR colour clock)
re-interprets the byte fetched NOW, one column LEFT, so **only Dhgr / An3**
events get `−25`. Without it OLDSKOOL FORT ET VERT's AN3-driven raster bands
drew one character cell right of the TV-set art (user-confirmed, both RGB and
composite). HiRes (`$C056/$C057`) is NOT shifted — it is a graphics-mode/address
switch, fetch-side like PAGE2, and MAD EFFECT flips lo/hi-res mid-line to place
its beam-raced picture; shifting HiRes pulled those regions one cell too far left
(regression caught 2026-09-02). TextMode / MixedMode / 80Col also stay on −24.
The HiRes/PAGE2 half is pinned by `raster_switch_kind_offset`; the AN3/DHGR −1 is
validated against the live demo. The shared
`forEachBeamSegment(frameStart, events, paint)` builds, per visible scanline, the
ordered list of column segments `[col0, col1)` + the display state across each
(an event subdivides its line at `byteCol`; the end-of-line state carries down),
**merges vertically-adjacent scanlines with identical segmentation into a band**,
and invokes `paint(state, y0, y1, col0, col1)` per band × segment. The RGBA path
paints through `renderInternalSegment`; the composite path through
`paintSignalBand` — one decomposition, so the two can never diverge. The merge is
what lets the common case — a program re-flipping `$C050/$C051` every scanline to
hold a vertical strip — render whole text/lo-res rows cleanly (a lone 1-scanline
segment would quantize away under `bandRows`). `render{Text,HiRes,LoRes}` and the
composite `paintText40`/`paintHgr`/`paintLoRes40` painters gained `col0,col1`
bounds (default full = byte-identical to before): text / lo-res bound their column
loop; hi-res decodes the whole scanline (the NTSC artifact sliding window keeps
its neighbour-byte context) and clips only the write-back + mono persistence. An
event-free run of scanlines collapses to one full-width paint — so existing demos
do not regress (`display_golden_hash`, `beam_race_composite`, OE parity goldens
unchanged). Pinned by `horizontal_split` (RGBA) and `horizontal_split_composite`
(signal): lower band re-flips every scanline → left window == HGR reference,
right window == TEXT reference on the same line. The per-kind column offset is
pinned separately by `raster_switch_kind_offset` (a mid-line HiRes split lands
one byte column left of a PAGE2 split on the same cycle).

The **560-wide IIe / Le Chat Mauve modes** (80-col text, DHGR, DLGR, Chat Mauve)
also split mid-line, in both outputs:
- **RGBA** (`frame80`): the LUT painters (`renderDhgr` etc.) carry cross-column
  context across several sub-paths, so threading `[col0,col1)` through each would
  be brittle. Instead `renderInternalSegment`, for a non-legacy segment,
  snapshots the band of `frame80` (+ the `persistenceL80` mono history), paints
  it **full width** through `renderInternalBand`, then restores the columns
  OUTSIDE the `[col0·14, col1·14)` window. This composes correctly across several
  560-wide segments on one line — each restores what it does not own, so a
  column's final value is whatever its owning segment painted — and keeps each
  painter's full neighbour context.
- **Composite signal** (`signalBuf`): the signal builders (`paintText80`,
  `paintDhgr`, `paintLoResDouble`) are simple per-column bit emitters (no NTSC
  artifact window — the shader demodulates downstream), so they take `[col0,col1)`
  bounds **directly**, and the split lands in the OE/AppleWin demod picture too.

Pinned by `horizontal_split_560`, which checks the IIe "DHGR left, 80-col text
right, same line" split in *both* the RGBA framebuffer and the composite signal.
**Scope-out:** a split that MIXES a 40-col (280, `frame`) and an 80-col (560,
`frame80`) segment on one scanline targets different buffers and is undefined
(the last segment's `useFrame80` wins); and the exact transition cycle within a
character clock is a later refinement.

### 80-col text

Aux RAM (cells 0,2,…) interleaved with main (1,3,…) into 560-wide
frame. Mixed (HIRES+80COL+MIXED): HGR top 20 rows doubled, 80-col
rows 20..23 overlay. ALTCHAR plumbed but no-op against built-in
fallback.

### Static-text frame skip (`TextFrameKey`)

`render()` returns without painting when the frame is full-screen TEXT that is
byte-identical to the one already in the framebuffer. Measured **93.4 → 15.0
µs/frame (−84 %)** on booted DOS; worst case (text churning every frame) is a
wash. Key terms: `DisplayState` + `isIIE` + FLASH phase + `hiResMode` + the
character ROM **by value** + `$0400-$0BFF` from main *and* aux **by value**
(the union of text/lo-res pages 1 and 2 — page routing is deliberately not
resolved, so no routing rule can be got wrong).

Three exclusions, all load-bearing:

| Excluded | Why |
|---|---|
| Beam-raced frames (`!events.empty()`) | Painted as bands with different `DisplayState`s + a column-bounded save/restore; corresponds to no single whole-frame state. Key invalidated. |
| Graphics / MIXED | Painters write phosphor persistence (`max(target, prev × decay)`), so output changes every frame from identical inputs. `renderText`/`renderText80` write none. |
| CPU demod (`cpuDemodGfx`) | AppleWin / OE-CPU overwrite `frame80` from the composite signal. Key invalidated. |

The key also carries the **Le Chat Mauve** card identity + its
`renderStateKey()` (variant, latch, the Eve's switch byte, invertBit7).
`Memory::DisplayState` is not sufficient: $C0B0-$C0BF are guest writes that
select the colour-TEXT renderer (and the 560-wide `frame80`), reach the card
via `SlotBus::broadcastVideoSwitch`, and push **no video event** — so
without them the skip served a stale screen at the wrong geometry on the //c
PAL profile's built-in slot 7. Fixed 2026-07-31; pinned by section 9 of
`display_dirty_skip`, which only bites when the card is actually **plugged into
the SlotBus** (handing it to the display alone makes the section vacuous).

PAL is automatic: FLASH derives from `frameCounter`, the *emulated* frame index
(`cycleCounter / (65 × scanlinesPerFrame)`), so 312-line/50 Hz and 262-line/60
Hz each advance the key at their own rate.

`invalidateTextFrameCache()` is public — any caller that mutates the framebuffer
behind `render()`'s back must call it. Pinned by `display_dirty_skip`, which
runs two machines in lockstep (one skipping, one forced-full) under both
standards and requires bit-identical output; its header records which key terms
are mutation-proven load-bearing and which are defensive.

### Composite NTSC shader (`ColorCompositeOE`)

OpenEmulator-inspired GPU pass: instead of decoding to RGB on the
CPU, `Apple2Display::fillCompositeSignal()` serialises the active
video mode (HGR / DHGR / 40-col text / 80-col text / 40-col lo-res /
DLGR double lo-res)
into a 1-bit 14.318 MHz luminance waveform — 560 samples × 192
lines, one byte per sample (`signalBuf`). HGR reuses the existing
`buildBitStream()` so the per-byte half-dot delay is preserved.
Lo-res emits `(nibble >> (absX & 3)) & 1` at every sample; DLGR
interleaves aux (rotl4 nibble) and main halves like `renderLoResDouble`.
The shader's NTSC demodulator recovers the 16 colours from the same
spectral mechanism a real CRT uses (no palette lookup).

GPU demod phase must match `renderCompositeOeCpu()` — see
`docs/archive/oe_gpu_cpu_parity.md` (historical) and the
`oe_demod_gpu_cpu_parity` test.

`MainWindow::drawScreenImage()` uploads `signalBuf` to an `R8` GL
texture and runs `NtscPostProcessor::process()`. The fragment shader
(`NtscPostProcessor.cpp` `kFragmentShader`):

1. Optional barrel distortion of UVs.
2. For each output fragment, 17-tap accumulation of signal taps through
   **OpenEmulator-exact FIR kernels** — a Dolph-Chebyshev(50 dB) window ×
   sinc lowpass, reproduced with libemulation's own realIDFT recipe
   (`OEVector::chebyshevWindow`/`lanczosWindow` + `OpenGLCanvas.cpp`) at
   the *AppleColor Composite Monitor IIe* config (luma 2.0 MHz, chroma
   0.6 MHz, Y'UV). Hard-coded as 9 symmetric coeffs each:
   - **`lumaK`** (sum 1) **notches fs/4** (`|H(0.25)|` ≈ 0.002, −3 dB ≈
     1.64 MHz), killing the dot-crawl the old gaussian (sigmaY 0.8,
     `|H(0.25)|` ≈ 0.46) produced.
   - **Chroma** (sum 2 = the ×2 demod gain): the **Sharpness** knob blends
     the OE-faithful soft kernel (0.6 MHz) ↔ a sharp 2.0 MHz kernel. At
     **Sharpness 0.5** (default) the GPU uses the soft kernel only — same
     as the CPU path and OE-faithful demod (avoids hue-ringed edges at
     transitions while solid fills stay correct).
   The CPU path (`Apple2Display::renderCompositeOeCpu`) mirrors `lumaK`
   and the same soft↔sharp chroma blend — MainWindow feeds the live
   hue / Sharpness / PAL / textSharp knobs to the display every frame via
   `Apple2Display::setOeDemodParams` (2026-07: they used to be GPU-only,
   leaving the sliders silently dead in OE-CPU mode and popping off on
   OE-GPU mixed frames, whose graphics band demodulates on the CPU).
3. Chroma is recovered by multiplying each tap with
   `sin(π/2 · (x + phaseOffset))` and `cos(π/2 · (x + phaseOffset))` —
   Apple II's 4× subcarrier alignment. **`phaseOffset = 1` in DHGR**
   (HGR/text = 0) so OE GPU/CPU and ColorAppleWin match MAME
   `rotl4b(lutEntry, absX+1)`.
4. YIQ → RGB via the standard NTSC matrix, then **hue** rotates the
   IQ vector, **brightness**/**contrast**/**saturation** apply
   in RGB space.
5. **Persistence** is a `max(decoded, prev * decay)` blend with the
   previous output frame held in a ping-pong FBO.
6. **Scanlines** darken odd output rows (output texture is 2× the
   signal height); the leftover **barrel** factor curls UVs at the
   edges.
7. Optional **shadow mask** post-effect: procedural RGB-stripe mask
   (`Triad` / `ApertureGrille` / `Dot`) multiplied into the pixel
   after demodulation. No texture upload — driven by `mod(outX, 3)`
   so the cost is one branch + one vec3 multiply per pixel. `Dot`
   alternates triplet phase every other row for the quincunx look.
8. Optional **PAL composite** mode: flips the sign of the Q chroma
   tap on odd scanlines. Approximates PAL's line-phase alternation
   (the cancellation of hue errors at the cost of vertical chroma
   resolution). NTSC mode by default.

**Sharp-text bypass.** TEXT under composite is faithful to a real
CRT but blurry — fine for nostalgia, awkward for everyday use. The
`textSharp` knob makes `MainWindow::drawScreenImage()` skip the
shader for the whole text screen and draw the crisp RGB framebuffer
instead. Toggled live in the CRT Settings panel; on by default.

`OpenGLShader.cpp` provides the small `compileShaderProgram()` helper
+ a lazy `glfwGetProcAddress` table on Linux/Windows (macOS gets
GL 3.x from `<OpenGL/gl3.h>`, Emscripten from `<GLES3/gl3.h>`). The
shader source is single-pass, gated on `#version 150` (desktop) /
`#version 300 es` (WebGL2). No OpenEmulator / libemulation code is
copied — the implementation is rewritten from the public NTSC spec
(FCC/CCIR §73.682) and the openemulator-explainer notebook by
Zellyn Hunter (algorithm description only).

All knobs persist under settings.json keys `ntsc_brightness`,
`ntsc_contrast`, `ntsc_saturation`, `ntsc_hue`, `ntsc_sharpness`,
`ntsc_persistence`, `ntsc_scanlines`, `ntsc_barrel`,
`ntsc_shadow_mask` (int 0..3), `ntsc_shadow_strength`, `ntsc_pal`,
`ntsc_text_sharp`, `ntsc_luminance_gain`, `ntsc_center_lighting`,
`ntsc_phosphor_gamma`. The CRT Settings panel (View → CRT Settings)
drives them live.

If shader compilation fails (driver too old, GLES2-only context,
…), `NtscPostProcessor::available()` returns false and POM2 silently
falls back to the regular `ColorNTSC` LUT framebuffer for the mode —
the menu entry stays usable but the result is indistinguishable
from `ColorNTSC` until the GL state catches up.

> **Note:** since the Phase-4 split, `NtscPostProcessor` is **demod-only**
> (steps 2–4, 8 above). The CRT *glass* — barrel geometry (step 1),
> persistence (5), scanlines (6) and shadow mask (7) — moved to the shared
> `CrtEffectStack` (below), so OE chains into it like every other mode.

### Universal CRT effect stack (`CrtEffectStack`)

`src/CrtEffectStack.{h,cpp}` applies the CRT glass on top of *any* RGBA
framebuffer (MAME LUT, Chat Mauve, mono, AppleWin) — gated by "CRT effects
on all modes" — and is the single effect implementation OE also chains into.
Effect order in the fragment shader: barrel → hue → BCS → phosphor curve →
scanlines → shadow mask → center-lighting (vignette) → luminance gain →
edge-mask → persistence (ping-pong FBO, applied last so the afterglow isn't
re-attenuated by the glass each frame). The scanline→mask→lighting→
luminanceGain ordering matches OpenEmulator's display shader
(`OpenGLCanvas.cpp:117-126`).

**GL teardown and FBO limits** *(2026-09-07)*. `~CrtEffectStack` was
`= default` and deleted nothing, opting the class out of its own teardown
contract; it calls `destroyGL()` now (`CrtEffectStack.cpp:408/416`). Both this
pass and `NtscPostProcessor` also resized their FBOs without clamping to
`GL_MAX_TEXTURE_SIZE` or re-checking completeness afterwards, so a 5K dock on a
4096-px driver (a Pi/V3D, typically) painted garbage in **silence** — the
allocation simply failed and the shader sampled nothing. Both now clamp
(`CrtEffectStack.cpp:452`) and re-check
`glCheckFramebufferStatus(...) == GL_FRAMEBUFFER_COMPLETE`, logging the limit
they hit. The bandwidth-FBO failure path leaked its program (`bwProgram = 0`
without a delete), and `GL_UNPACK_ALIGNMENT` was left at 1 for ImGui — both
fixed.

**Glass details (2026-05 parity pass).**
- **Hue** is applied here (RGB→YUV BT.601, rotate U/V by `hue·π`, YUV→RGB) so
  the knob works on every mode, not just OE. The OE demod already rotates hue,
  so MainWindow passes `hue = 0` to the stack on the OE path (no double spin).
- **Shadow mask** uses the Lottes dark/light triplet (off-channels → 0.5, lit
  channel → 1.5) so the triad preserves average luminance, instead of the old
  pure-primary `(1,0,0)` mask that crushed 2/3 channels and over-darkened.
- **Mask pitch is glass, not signal** (2026-09-04). The mask's horizontal
  coordinate is pinned to a constant `kMaskUnitsX = 1120.0` (the 560-dot line
  at 2 units/dot), *not* derived from `uSrcSize.x`. It used to be, and since
  the triad period is a fixed 3 units that put half as many triads on screen in
  the 280-wide modes (legacy `frame`: 40-col text, 40-col HGR, lo-res) as in
  the 560-wide ones (`frame80`: 80-col, DHGR, Chat Mauve, OE demod) — flipping
  video mode changed the tube. A real CRT's triad pitch is fixed in millimetres
  whatever resolution it is fed. The vertical axis deliberately keeps
  `uSrcSize.y * 2.0`: the scanline count **is** a signal property (192 real
  beam sweeps), the mask is not. 560 was chosen as the reference over 280 so
  every 560-wide mode — including both OE paths — stays pixel-identical.
  Checked by `crt_barrel_view` (exit 3 on mismatch); no ctest pin is possible
  because the shader needs a GL context.
- **Phosphor curve** (`phosphorGamma`, default 1.0 = identity) is a
  per-channel power law `rgb = rgb^γ` on the beam intensity → emitted light,
  applied after BCS and before the spatial scanline/mask modulation (which
  attenuates the light the phosphor already produced). It is the *luminance*
  half of the CRT phosphor model; `persistence` is the *temporal* half. γ > 1
  deepens shadows for more CRT-like contrast, γ < 1 lifts them. Default
  identity keeps every existing golden/parity test untouched. Slider range
  0.6–2.6, persisted `ntsc_phosphor_gamma`.
- **Analog RGB bandwidth** (`rgbBandwidthMHz`, default 0 = off) is a separate
  **pre-pass** at source resolution, ahead of the glass: a 17-tap Hann-windowed
  sinc low-pass, horizontal, per channel, run on the framebuffer's own sample
  grid. It models the video chain between machine and tube the way OpenEmulator
  models its *connection* types. The motivating case is Le Chat Mauve, which
  has two connectors (`docs/chatmauve_plan.md` § 3.7): a TTL RGB header (square
  dots — leave the knob at 0) and an analog Péritel socket where R/G/B each
  leave through a resistor ladder and a trim pot, then a cable (~5-6 MHz).
  - **Why the source grid, not the screen.** Band-limiting is an operation on
    the samples. A 560-wide `frame80` is clocked at 14.318 MHz, a 280-wide
    `frame` at 7.16 MHz, so one MHz figure is a different fraction of Nyquist
    per mode — which is exactly how one cable behaves, and is why there is no
    per-mode knob. Measured on the 560 grid (peak-to-peak out of 215):

    | cutoff | 7.16 MHz content (true 560-dot DHGR / COL280) | 3.58 MHz content (280-dot HGR doubled into `frame80`) |
    |---|---|---|
    | 3 MHz | 1 | 37 |
    | 4 MHz | 1 | 161 |
    | 5 MHz | 1 | 215 |
    | 7 MHz | 173 | 215 |

    So ~5 MHz softens a genuine 560-dot picture and leaves HGR alone; go to
    3-4 MHz to soften HGR too. On the 280-wide buffer a 5 MHz cutoff is above
    Nyquist (3.58 MHz), `applyBandwidth` returns 0 and the pass is **skipped**
    outright — a no-op that costs nothing, not a transparent filter.
  - Weights are normalised by their own sum, so DC gain is exactly 1: a flat
    field keeps its level whatever the cutoff and however the window truncates
    the kernel (checked — mean 180 → 180, span 0). Dragging the slider must
    not walk the brightness.
  - It is a windowed-sinc brickwall, not the 1st-order RC rolloff a real cable
    has. Deliberate: a knob that reaches "visibly soft" inside its range is
    more useful than a physically-shaped one that barely does anything, and the
    transition band of a 17-tap Hann is gentle enough to read as analog.
  - Graceful degradation, twice: a failed compile or an incomplete FBO zeroes
    `bwProgram` and logs, leaving the glass pass fully working with the knob
    inert — the pre-pass must never be able to take the CRT stack down.
- **Luminance gain** (`luminanceGain`, default 1.0) re-brightens post-mask,
  mirroring OpenEmulator's stage — pairs with scanlines/mask to recover
  brightness.
- **Center lighting / vignette** (`centerLighting`, default 1.0 = flat, OE's
  Apple II default): `lighting = cuv·(1/cl − 1); rgb *= exp(−dot(lighting))`,
  verbatim OpenEmulator. Lower values darken the edges.
- **Persistence** carries a `−0.5/256` noise floor (OpenEmulator) so faint
  trails decay fully to black instead of lingering at the quantization step;
  the slider stays a per-frame retention factor (POM2's documented model,
  not OE's seconds time-constant).
- **Not ported (intentional):** OpenEmulator and POM2 both run the glass in
  gamma space, so crt-lottes-style linear-light is a *beyond-OE* option, not a
  parity gap — left out.
- **Defaults are deliberately punchier than OpenEmulator** (`scanlines 0.25`,
  `shadowMaskStrength 0.5`, `persistence 0.4` vs OE's ~0.05/0.05/0). This is an
  intentional product choice (a visible CRT look out of the box), not an
  oversight; the dark/light mask keeps strength 0.5 tasteful. OE-faithful 0.05
  values remain available via the sliders.

**Anti-moiré (2026-05).** Barrel distortion warps the UVs non-linearly; the
scanline (period = 2 source-rows) and shadow-mask (period = 3 units) patterns
are high-frequency, so where the warp compresses the picture they exceed the
output Nyquist and alias into moiré "lines". Two-part fix:

- `MainWindow::drawScreenImage()` computes the on-screen target size **up
  front** and passes it to `CrtEffectStack::process(src, srcW, srcH, dstW,
  dstH)`, which renders the pass at **native output resolution** (decoupled
  from the source dims, which now only drive the *scanline* frequency via
  `uSrcSize.y`; the mask frequency is the fixed `kMaskUnitsX` above). ImGui then blits the result 1:1 — no second resample beat.
- The shader **analytically anti-aliases** the patterns: `fwidth()` of the
  scanline/mask coordinate measures how many pattern-units one output pixel
  spans; as that approaches Nyquist (which is exactly where the warp
  compresses) the modulation fades smoothly to neutral instead of moiréing.
  Scanlines also use a smooth `cos` beam rather than a hard `fract` edge, and
  the curved barrel border is a soft `fwidth`-based edge mask (no jaggies).

Inspect via the offscreen diagnostic `tests/crt_barrel_view`
(`EXCLUDE_FROM_ALL`): renders a barrel + scanline + mask test (optional PPM
source) to `/tmp/crt_barrel_{on,off}.ppm`. No CI hash — the GL path is
FP/driver-dependent, so it's eyeballed, not pinned.

### AppleWin NTSC (`ColorAppleWin`)

**Faithful port** of AppleWin's CPU-side NTSC composite simulation
(`source/NTSC.cpp::initChromaPhaseTables`, by Sheldon Simms / Tom
Charlesworth / Michael Pohoreski — GPL v2+). Per the project convention
(AppleWin = source of truth) the algorithm, IIR filter coefficients
(`NTSC.cpp:115-132`), YIQ→RGB matrix and white/black/grey special-casing
are ported line-for-line and cited inline in `src/AppleWinNtsc.cpp`.

Consumes the same 14.318 MHz luminance bitstream `fillCompositeSignal`
generates for `ColorCompositeOE`. Decoding happens through static
`[4][4096]` phase tables built once at first use by
`AppleWinNtsc::ensureInitialized()`:

- For each (colour phase 0..3, 12-bit signal history): walk the 12 bits
  *oldest first*, **2× oversampled** (`phi += 45°` per half-step, 90°
  per dot — Apple II's 4× subcarrier alignment), through three cascaded
  2-pole IIR filters: `initFilterSignal` (input low-pass),
  `initFilterChroma` (band-pass @ fs/4 — the inverted-`x[0]` zero is what
  actually isolates chroma), `initFilterLuma0/1` (luma low-pass).
- Quadrature-demodulate chroma (cos→I, sin→Q, single-pole `/8`
  smoothing), then YIQ→RGB (FCC matrix). `y0` → Monitor table; `y1`
  (luma of *signal − chroma*, a comb) → Color-TV table.
- Runtime is a pure causal 12-bit shift register + one LUT lookup per
  dot (`NTSC.cpp:331`), no window-centring.

> **Why the rewrite (2026-05):** the prior gaussian-moving-average
> approximation computed luma with a window too narrow to notch the
> subcarrier, so luma absorbed the subcarrier and `signal − luma`
> cancelled chroma inside steady colour fills — the "almost no colour"
> bug (only edge fringes survived). The dedicated band-pass fixes it.

Three sub-modes via `Apple2Display::AppleWinSubMode`:

- **Monitor** — `g_hueMonitor` (luma y0). Sharp, full composite artifacts.
- **TV** — `g_hueColorTV` (comb luma y1) + 50% blend with the previous
  frame's same scanline (`appleWinPrev80`), approximating phosphor
  persistence + comb-filter blur of a consumer TV.
- **Idealized** — POM2-only (no AppleWin equivalent): Monitor luma with
  chroma boost ×1.6 for a punchy flat-panel look.

`CYCLESTART = 45°` aligns hues to the MAME reference out of the box (no
extra phase calibration); `rebuildForPhase()` adds an offset for the
render tool's sweep.

Pinned by `applewin_ntsc_smoke` (idempotent init, all-black/all-white
sanity, $7F neutral luma, **$2A solid-fill saturation guard** — the
regression test for the no-colour bug, Idealized artifact non-black, Tv
convergence, multi-line wrapping).

Full mode-by-mode comparison vs MAME / OpenEmulator / hardware lives
in [`docs/graphics_modes_comparison.md`](docs/graphics_modes_comparison.md).

### Test framework gotcha

Tests inherit parent's `-O3 -DNDEBUG` → would strip `assert()`.
`tests/CMakeLists.txt` adds `-UNDEBUG`.

### Parser fuzz smokes — `fuzz_disk_image`, `fuzz_snapshot`

The two widest untrusted-input surfaces: a disk image and a snapshot are both
"a file the user got from somewhere", and each loader walks a structure driven
by lengths and offsets that came out of that file. Both tests are bounded
(~2.0 s and ~0.1 s), deterministic (fixed seed → a failure reproduces exactly),
and take `<seed> <iters>` for a longer soak.

Three design points, each learned the hard way:

- **Seeds are synthesised, never read from `disks_5.4/` or `hdv/`.** Not one
  disk file is tracked by git, so a corpus-reading fuzzer tests *nothing* on a
  fresh clone or in CI — and passes, which is the worst possible failure.
- **Seeds are valid containers, not noise.** Random bytes are rejected at the
  magic and never reach the walker where a bug would be. `fuzz_snapshot`
  prints its acceptance rate and asserts a floor on it, so a change that starts
  rejecting every mutant is visible instead of quietly turning the test into a
  no-op.
- **Mutation is STRUCTURE-AWARE, and this is what makes them bite.** The first
  version scattered byte-flips and could not catch a deliberately removed
  bounds check even in hundreds of rounds — the fields that matter are four
  bytes each in a 160–250 KB file, so blind flipping never lands on one. Both
  harnesses now parse the container and aim at the numbers the loader trusts:
  WOZ chunk lengths, TMAP indices and the TRKS start/count/bit-count triple;
  snapshot section lengths and names.

Verified by sabotage, which is the only evidence that matters for a fuzzer:
removing `Disk35Image::loadWoz`'s payload bounds check is caught as an ASan
fault in `cellsFromPackedBits`, and disabling `Memory::loadSnapshotState`'s
`need()` length check is caught as a heap-buffer-overflow. Note what the
snapshot one *cannot* prove: every read in `SnapshotReader` goes through an
istream over a bounded streambuf, so that layer cannot over-read by
construction — its guards exist to stop unbounded *allocation*. The real
raw-pointer parser downstream is `Memory::loadSnapshotState`, reached through
the MEX section.

They earn their keep under sanitizers; a plain build only catches an outright
crash. Run that way after touching any loader:

```bash
cmake -B build_asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build_asan -j8 --target test_fuzz_disk_image test_fuzz_snapshot
(cd build_asan && ctest -R fuzz_)
```

## Audio

`AudioDevice`: miniaudio **interleaved stereo** float32
(`kChannels = 2`). **OS-negotiated sample rate** (often 48 kHz on Apple
Silicon) — cycle-driven sources MUST query `getActualSampleRate()`.

### Stereo bus (2026-08-01)

The bus went stereo because the AY cards are stereo in hardware, and the
mono sum destroyed the pan their music writes. MAME references, all in
`bus/a2bus/a2mockingboard.cpp`:

| Device | Routing | Lines |
|---|---|---|
| Mockingboard / ayboard | one 2-channel speaker, AY1 → ch 0 (L) @0.5, AY2 → ch 1 (R) @0.5 | `:159-165` |
| Mockingboard speech (Votrax SC-01) | **both** channels @1.0 → centred | `:186-189` |
| Phasor | second 2-channel speaker; audible result L = ay1+ay2 (VIA1 pair), R = ay3+ay4 (VIA2 pair) | `:192-208` |
| Echo+ (TMS5220) | `front_center` | `:210-219` |

Two contracts, so cards migrate one at a time:

- **Mono** — `fillAudioBuffer(out, n)`, unchanged. The mixer places the
  result with `AudioSource::pan` (-1 L … 0 centre … +1 R). The law is a
  **balance**, not constant power: centre is unity on *both* channels, so
  making the bus stereo moved no existing source. Speaker, cassette and
  both floppy-sound devices stay here, with a UI pan knob (persisted:
  `speaker_pan`, `cassette_pan`, `floppy_sound_pan[_35]`).
- **Stereo** — `fillAudioBufferStereo(l, r, n)` returns true when the
  source filled two planes itself; `pan` is then ignored, because the
  card's wiring is the authority. Mockingboard and Phasor implement it;
  both keep the mono path as `0.5 * (L + R)`, which is *bit-for-bit* the
  pre-stereo render (`/3` per side folds to the old `/6`, `/6` per side
  to the old `/12`).

`AudioDevice::setMonoDownmix` folds the whole bus to `0.5 * (L + R)` on
both channels — for mono playback gear, and for anyone who does not want
a single-AY tune (DD2 never touches chip 2) arriving from the left
speaker only. Off by default; persisted as `audio_mono_downmix`. Master
metering is per channel (`getMasterPeakL/R`); `getMasterPeak()` is the
louder side. Pinned by `tests/audio_stereo_test.cpp` — pan law, stereo
passthrough, downmix, per-chip placement on both cards, the mono
fold-down identity, and a card mixed next to a centred source.

### Speaker

`SpeakerDevice` (`AudioSource`). Verbatim MAME `spkrdev.cpp:74-327`.
CPU records each `$C030-$C03F` toggle with sub-instruction timestamp
(`cycleCounter + cpu->getCurrentInstructionCycles()`) into 16 K ring.
Audio thread: rectangle integration → 4× oversample → 64-tap windowed
sinc (cutoff sr/4) → 0.995-pole DC blocker. Auto catch-up if drain >
100 ms.

### Cassette

`$C020` output toggle / `$C060` input comparator sign. Separate
`AudioSource`. `CassetteDeck_ImGui` uses Font Awesome
(`fonts/fa-solid-900.ttf`), falls back to `?` if missing.
Auto-rewind 500 ms is opt-in, default off.

### Mockingboard

Sweet Microsystems: two 6522 VIAs each driving an AY-3-8910. No ROM
— VIAs decoded in slot ROM window (`$Cn00-$Cn0F` VIA#1,
`$Cn80-$Cn8F` VIA#2) via `slotRomWrite`.

VIA → AY (Sweet, AppleWin `Mockingboard.cpp:193`):
```
Port A       → AY data bus (D0..D7)
Port B bit 0 → AY BC1
Port B bit 1 → AY BDIR
Port B bit 2 → AY /RESET (active low; 1 = running)
```
{BDIR,BC1}: 00=INACTIVE, 01=READ, 10=WRITE, 11=LATCH-ADDR. Drivers
emit PB = `$07 → $04 → $06 → $04`. PB2 stays high; `/RESET` only on
PB=`$00`.

**6522 subset**: A/B + DDR, T1 (latch + counter, one-shot +
continuous), **T2 (one-shot, timed phase-2)**, IFR/IER (T1/T2 bits
6/5; bit 7 dynamic from `ifr & ier & 0x7F`). T1CL read clears
`IFR.T1`, T2CL read clears `IFR.T2`. T1L-H ($07) write ALSO clears
`IFR.T1` (MAME `6522via.cpp` VIA_T1LH: `m_t1lh = data;
clear_int(INT_T1)` — no counter transfer, no restart; an earlier
POM2 note claimed the opposite). IER bit 7 set-vs-
clear (`$C0` enables, `$40` disables). SR, CB1/CB2 outputs + T2
PB6-count mode not modelled (PCR is partly: CA1 edge select and
CA2-mode IFR masking are honoured). **T2 underflow IRQ fires at `TIMER2_VALUE +
IFR_DELAY` (= N+3)** matching MAME `6522via.cpp:959` (POM2's
`advance()` crosses < 0 at N+1, so T2CH pre-biases the counter by
`IFR_DELAY-1 = 2`). This is the per-frame sync French Touch / DIX
drive: `T2 = 7512 − latency`, IRQ → mid-scanline beam-race. Pinned
by `via_t2_timing`.

**AY-3-8910 synthesis** runs on the audio thread inside inner
`AudioSrc`. CPU updates regs under `mtx`; the callback snapshots both
banks (32 B), releases, synthesises lock-free. The generators, mixer
and band-limiting live in **`AyPsgSynth.h`, shared with `PhasorCard`**
(extracted 2026-08-01 — the two cards had carried verbatim copies and
drifted). 17-bit LFSR `x^17 + x^14 + 1`; MAME-verbatim 4-flag envelope
state machine (all 16 shapes pinned against MAME's step sequence).
Both chips sit on the stereo bus — AY1 left, AY2 right, matching
MAME `a2mockingboard.cpp:161-165` → [§ Stereo bus](#stereo-bus-2026-08-01).

Three properties the audio path depends on, each of which was broken
until 2026-08-01 (full reasoning + numbers → `CHANGELOG.md`):

* **Band-limiting.** MAME never renders at the output rate: its stream
  runs on the chip's clock/8 grid (`ay8910.cpp:1298`) and
  `src/emu/resampler.cpp` decimates. POM2 renders straight to the
  device rate, so `renderChipSample` **box-integrates** the mixer over
  the ~2.9 base ticks each output sample spans. Point-sampling instead
  put 7 % of output power into inharmonic fold-back; integration gives
  0.51 %. Cost 0.67 % of a core per chip.
* **The event queue is a jitter buffer, not a chase.** The CPU worker
  publishes ~17045 cycles of writes per burst; one callback covers
  ~5937. Un-rendered events STAY queued and the cursor runs about one
  burst *behind* `latestAyEventCycle_`, deadbanded. Never set the
  cursor to `pending.back().cycle` — that is zero lag, and it collapsed
  ~90 % of writes onto the buffer edge.
* **DC blocking.** The channel model is unipolar, so gating channels
  and changing volumes steps the offset. 1-pole 20 Hz high-pass,
  matching MAME's default per-speaker filter
  (`src/emu/audio_effects/filter.cpp:39-44`). One per side since the
  card went stereo — MAME really does put one on each speaker, and the
  filter is linear so the fold-down is unchanged.
* **Stereo placement.** Chip 0 → left, chip 1 → right, `/3` per side
  (`a2mockingboard.cpp:161-165`); the Sound II's SSI263 is added to
  both sides at unity, because MAME centres the card's speech chip
  (`:186-189`) and there is one speech chip, not one per side. Phasor
  splits by VIA pair, `/6` per side. Full contract → [§ Stereo
  bus](#stereo-bus-2026-08-01).

The AY tick rate derives from the **live** CPU clock, not the NTSC
constant — pin 22 is the slot's phase-0 line, so PAL clocks the chip at
1 015 625 Hz (12 cents below NTSC). `PhasorCard::setCpuClock` exists (`PhasorCard.h:162`, `PhasorCard.cpp:410` —
the same body as `MockingboardCard::setCpuClock`), and `setVideoStandard` fans
it out to every plugged card, not only the Mockingboard. What `PhasorCard`
still lacks is the **event queue**: no `AyRegEvent` deque, so its register
writes are not emuCycle-replayed the way `Mockingboard.h:275-315` does.

Each VIA `irqOut() = (ifr & ier & 0x7F) != 0`; OR'd onto slot IRQ.

**Lazy timer sync** (`syncToCpuCycle()`): every `slotRomRead/Write`
catches VIAs up to `cpu_->getCycleCountNow() + 1` first. The **+1**
lands the sync on the access's DATA cycle — the last cycle of the
instruction performing it, where a real 6522 has already decremented
T1/T2 on that φ2. `getCycleCountNow() = cycleCounter + cpu.cycles` and
`cpu.cycles` does not yet count that in-flight data cycle (`SBC $C404`,
a 4-cycle abs read, syncs with `cpu.cycles == 3`), so without it every
Mockingboard MMIO counter read was one too high. Invisible to steady
music and to write-then-read detection (Nox/Skyfox read RELATIVE to
their own arm, so read+arm offsets cancel) and to TRIBU's raster (it
re-arms every link — closed loop — so a constant offset cancels, and
`via_t1_rearm_chain` / `dix_menu_raster_probe` are unchanged). It only
moves reads taken against a FREE-RUNNING underflow: French Touch's
OLDSKOOL FORT ET VERT reads `$C404` in its stable-raster T1-IRQ handler
and phase-dispatches on `mem[$03] - T1CL - $19`; the one-too-high T1CL
wrapped that phase `$00 -> $FF`, jumping a self-modified BVC into a
slice that RTS'd off the 3-byte IRQ frame -> SP=0 -> crash ~10 s in
(fixed 2026-09-02). Pinned by `mockingboard_t1_irq_phase`. **Gotcha**:
`advanceCycles` (the end-of-step batch) syncs to
`getCycleCountNow() - cycles`, not `now` — `cycleCounter` is bumped
before slot dispatch but `cpu->cycles` hasn't been zeroed yet, so the
naive `now` over-counts by one instruction (broke Nox/Skyfox/Broadside
T1 IRQ detection until 2026-05-25). MMIO (`+1`, the data cycle) and
batch (instruction end) converge for a load. Pinned:
`mockingboard_sync_smoke::testNoEndOfStepOvershoot`.

**Tear-down**: remove `AudioSource` from `AudioDevice` BEFORE
destroying the card. Persisted: `mockingboard_volume`,
`mockingboard_muted`. Pinned: `mockingboard_smoke`,
`mockingboard_sync_smoke`.

### SSI263 + Echo+ (Street Electronics)

`pom2::Ssi263` (`Ssi263.h/.cpp`) — Silicon Systems Inc. SSI263A
phoneme speech synth, shared chip model used by both
`EchoPlusCard` (standalone, slot ROM at `$Cs00-$Cs04`) and
`MockingboardCard` `Variant::SoundII` (chip at `$Cs40-$Cs44`,
A/!R wired to VIA1.CA1).

**No MAME reference**: MAME does NOT implement the SSI263 (verified
2026-05-27 — no `ssi263*` file in `src/devices/sound`). The canonical
reference is AppleWin `source/SSI263.cpp`. POM2's chip emulation is
independent code modelling the same protocol contract.

Register layout (5 registers at $00..$04 within the chip's window):

```
$00 DURPHON  bits 7:6 = mode (00=IRQ disabled, 01=frame imm. infl.,
                              10=phon. imm. infl., 11=phon. trans. infl.)
             bits 5:0 = phoneme code (0..63; 62 defined)
$01 INFLECT  inflection value
$02 RATEINF  bits 7:4 = rate (playback speed)
             bits 3:0 = inflection low
$03 CTTRAMP  bit 7    = CTL (1 = power-down/silent; 0 = run)
             bits 6:4 = articulation
             bits 3:0 = amplitude
$04 FILFREQ  filter frequency (formant 4 cutoff)
```

Reading any register returns a status byte with **bit 7 = A/!R**
(Acknowledge / not Request) — high while the chip is requesting the
next phoneme. The CPU clears A/!R by writing to one of $00..$02 (also
de-asserts IRQ). Writes to $03 (CTTRAMP) or $04 (FILFREQ) do not ack.

CTL H→L transition (power-down exit) restarts the loaded phoneme
without bumping `phonemeWriteCount`. CTL L→H clears A/!R + silences.

**Phoneme duration formula** (AppleWin parity):
```
ms = ((16 - (rate>>4)) * 4096 / 1023) * (4 - (dur>>6))
cycles = ms * POM2_CPU_CLOCK_HZ / 1000
```
Range: ~4 ms fastest (rate=15, dur=3 → ~4090 cyc), ~256 ms slowest
(rate=0, dur=0 → ~262k cyc). Pinned by `ssi263_smoke`.

`MODE_IRQ_DISABLED` (mode 00) suppresses the **host IRQ only** — A/!R
(D7) is still asserted on phoneme completion (AppleWin `SSI263.cpp`
~line 724: D7 is raised regardless of the DR1:0 mode bits; only
power-down holds it low). So polling drivers that select mode 00 and
watch the D7 status bit to detect phoneme-complete still work. On
completion the duration counter parks at 0 (it does not re-tick), so
the chip is quiescent until the next DURPHON write — it does **not**
"repeat". `Ssi263::advance()` returns `irqEnabled()` (gates the host
IRQ edge) but always sets `aRequest_`. Pinned by `ssi263_smoke`
(`testIrqDisabledMode`).

#### MockingboardCard Variant::SoundII

`MockingboardCard` accepts a `Variant` constructor parameter
(default `AC`). With `Variant::SoundII` an `Ssi263` is instantiated
and slot ROM decode carves $40-$4F (5 SSI263 regs + 3 mirrors) out
of the VIA1 mirror range — so the same card surfaces the A/C
VIAs at $Cs00-$Cs0F + $Cs80-$Cs8F AND speech at $Cs40-$Cs44, the
exact layout of real Sound II hardware.

SSI263 A/!R wires (inverted) into VIA1.CA1 → on each phoneme-end
edge, `advanceCycles` calls `via_[0]->setCa1NegativeEdge()` which
latches `IFR.CA1` if `PCR.0 == 0` (the AppleWin-faithful default
config used by Sound II drivers). Once the host CPU enables
`IER.CA1`, the slot IRQ asserts → music driver's IRQ handler
dequeues the next phoneme.

Catalog key `mockingboard_c` selects this variant in Slot
Configuration; `mockingboard` keeps the vanilla A/C decode (no
SSI263, ssi_ stays null). The Mockingboard UI panel grows an
SSI263 section at the bottom only when `hasSsi263()` returns
true.

Pinned by `mockingboard_smoke::testSoundIIVariantSSI263` —
verifies no-SSI263 on AC variant, register decode at $40-$4F,
A/!R → IFR.CA1 latching, IER.CA1 → slot IRQ.

#### EchoPlusCard (Cricket / SSI263-class — catalog `echoplus`)

`EchoPlusCard` (`EchoPlusCard.h/.cpp`) — single-SSI263 card at
$Cs00-$Cs04, A/!R wired directly to the slot IRQ line. No 6522. Open
bus ($FF) for the rest of the slot ROM page.

**Naming caveat** — historically labelled "Echo+" in POM2's UI and
settings, but the markadev/AppleII-RevEng audit (2026-05-28) confirms
the real Street Electronics ECHO+ used 2× AY-3-8913 + TMS5220, not the
SSI263. The SSI263-based Street Electronics product was the Cricket.
The catalog key stays `"echoplus"` for `settings.json` back-compat;
the user-visible label is now "Cricket / Echo (SSI263)". See
[§ EchoPlusTMS5220Card](#echoplustms5220card) for the real Echo+ chipset.

`advanceCycles` ticks the chip and asserts slot IRQ on A/!R edge;
host writes to $00/$01/$02 release the IRQ. Default slot 4, pluggable
in any slot via Slot Configuration. Pairs naturally with a
Mockingboard A/C at slot 4 + Echo+ at slot 2 (the standard "MB for
music, Echo+ for speech" combo).

**Audio**: live. `Ssi263::fillAudio` pulls samples from the 62-phoneme
PCM blob in `Ssi263PhonemeData.cpp` (~313 KB, ported verbatim from
AppleWin `source/SSI263Phonemes.h` — LGPL → GPL3 compat), resamples
from the chip's native 22050 Hz to the host audio rate via a simple
linear cursor, scales by the AMP register (R3[3:0]). Power-down
(CTL=1) and the `FILTER_FREQ_SILENCE` sentinel ($FF in R4) squelch
output. The audio thread reads the chip's register banks + playback
cursor under the host card's mutex. Pinned by `ssi263_smoke` test 6
(RMS > 0.005 on a real phoneme; 0.0 in both squelch paths).

**UI**: Devices → Echo+ panel. Mode + IRQ enable + A/!R + power-down
state + current phoneme + duration countdown (cycles + ms) + the 5
register banks.

#### EchoPlusTMS5220Card

`EchoPlusTMS5220Card` (`EchoPlusTMS5220Card.h/.cpp`) — Street Electronics
ECHO+ **as actually shipped**: 2× AY-3-8913 PSGs + TMS5220 LPC speech
chip. Distinct from the SSI263-based `EchoPlusCard` above. Catalog
key `"echoplus_tms"`, default slot 2.

**v1 scaffold — chip cores deferred.** The card registers on the slot
bus with a stub register decode at $Cs00-$Cs0F so software that probes
for the chipset finds something coherent (not open bus). TMS5220 LPC
decoding (chirp ROM, K-parameter interpolation, energy/pitch tables)
and AY synth are both stubs — audio is silent. The provisional address
map (pin to markadev's schematic on next pass):

```
$Cs00  TMS5220 status / data (rd = status, wr = command/data byte)
$Cs01  TMS5220 stop / reset
$Cs04-05  AY-3-8913 #1 (address latch / data write)
$Cs06-07  AY-3-8913 #2 (address latch / data write)
$Cs08-FF  open bus
```

Source: markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+ (index.md
states "two AY-3-8913 Programmable Sound Generator chips and a TMS5220
Speech Synthesizer chip").

**Snapshot/rewind** (added 2026-08-17, bug hunt 8). Being a scaffold does
not exempt it: the stub decode is *read back* by the guest — `$Cs00`
returns the TMS status and `$Cs04-$Cs07` the selected AY register — so the
card serializes TMS status + last write, both address latches and both full
AY-3-8913 banks, magic-tagged (`'E' 'T' 'S'` + version) and validated whole
before it mutates a chip, exactly like `MockingboardCard` / `PhasorCard`.
Without it a rewind put the guest back on registers from the timeline it had
just left. Pinned by `card_snapshot_state`.

### Phasor (Applied Engineering)

`PhasorCard` (`PhasorCard.h/.cpp`) — dual-mode successor to the
Mockingboard. 2× 6522 VIA + 4× AY-3-8913 PSG (12 voices). Same VIA +
AY hardware as Mockingboard (verbatim from `Via6522.h` + `Ay3_8910.h`,
extracted 2026-05-27 specifically so the two cards share the same VIA
timing + AY register-bank decoder).

Address map (s = slot, slotHi = $C0+s):

```
$Cs00..$Cs0F   VIA1   (drives AY1 / AY2)
$Cs10..$Cs7F   VIA1 mirrors (partial decode)
$Cs80..$Cs8F   VIA2   (drives AY3 / AY4)
$Cs90..$CsFF   VIA2 mirrors
$C0(8+s)0..F   Mode soft-switch (responds to BOTH reads and writes)
```

**Mode soft-switch** (AppleWin rules):
- Read OR write to `$C0(8+s)X` triggers the update — the address (not
  the data) drives the mode bits.
- `if offset & 0x8`: clear mode bits 2:0
- `mode |= offset & 0x7`
- Power-up = `PH_Mockingboard` (0). Canonical writes:
  - `$C0(8+s)8` → mode = 0 = MB compat
  - `$C0(8+s)D` → mode = 5 = Phasor native
  - `$C0(8+s)F` → mode = 7 = EchoPlus (acknowledged, routed as native
    in v1)

**Chip-select decode** (Phasor native only):
```
chip_sel = (~(port_b >> 3)) & 3
  0  no AY selected      (PB3=1, PB4=1)
  1  primary AY only     (PB3=0, PB4=1)  → VIA1: AY1; VIA2: AY3
  2  secondary AY only   (PB3=1, PB4=0)  → VIA1: AY2; VIA2: AY4
  3  BOTH AYs broadcast  (PB3=0, PB4=0)
```

In `PH_Mockingboard` mode the chip-select bits are **ignored**: each
VIA always drives its primary AY only (AY1 / AY3), and the secondary
AYs (AY2 / AY4) stay silent — matching the real card's compat
default. This is what lets a vanilla Mockingboard music driver run
unchanged on a Phasor.

**Clock scaling**. `clockScale() == 2` in `PH_Phasor`; 1 in MB /
EchoPlus. The audio synth multiplies the AY input clock by this
factor — same register values produce notes one octave higher in
native mode (real Phasor halves the AY divider).

**Audio synth — 4-AY mono mix**. The `AudioSrc` snapshots the 4
register banks + reset/env-write counts + the current `clockScale()`
under the parent mutex, then runs the MAME-parity AY synth loop per
chip: integer tone counter + fractional accumulator (no float-aliasing
drift), 17-bit LFSR noise with prescale (clock/16/NP effective LFSR
rate), 4-flag envelope state machine (set_shape on every R13 store
including same-value re-stores). Mono mix divides by 12 — 4 chips ×
3 channels × peak 1.0 — so a maxed-out Phasor-native signal sits at
1.0 before the volume knob. `clockScale` multiplies the per-sample
step rate for tone / noise / envelope counters so the same register
values produce notes one octave higher in native mode (chip clock
doubles; AY periods unchanged).

In PH_Mockingboard only AY1 + AY3 receive strobes (chip-select
ignored), so the effective mix sits ~6 dB lower than a real
Mockingboard. The user compensates with the volume slider. The
alternative — a dynamic divisor — would clip when Phasor-native
software hits full amplitude across all 4 chips. Predictable
headroom wins.

Pinned: `phasor_card_smoke` — dual-VIA register layout + mirrors,
mode soft-switch decode, MB-compat routing (primary AY only),
Phasor-native chip-select PB3/PB4 decode (4 cases:
pri/sec/both/none), telemetry counters, 4-AY non-silent mix +
mute path, **clockScale ×2 pitch doubling measured by
zero-crossing** (target 2.0, observed ~2.01).

**UI**: Devices → Phasor panel. Banner with current mode (MB / Phasor
/ EchoPlus — color-coded), clock multiplier, slot IRQ, volume, the
device-select mode-switch addresses for the slot. 2 columns of VIA
telemetry (T1 counter / ACR / IFR / IER), 4 columns of AY register
banks with R0/R1/R2/R3/R4/R5/R6 channel periods + R8/R9/R10 volume
decoded for quick read. In MB-compat mode the secondary AY columns
(AY1, AY3) carry a "(MB-compat: silent)" tag so the user understands
why those banks stay zero even with a music driver running.

### Floppy mechanical sounds

`FloppySoundDevice`. Port of MAME `imagedev/floppy.cpp::
floppy_sound_device`. 20 source WAVs (10 × 5.25" + 10 × 3.5") in
`roms/floppy_samples/`, BSD-3-Clause.

**`FloppySoundSink` interface** (header-only): `DiskIICard` calls
`sound_->motor()/step()` through it so smoke tests don't drag
miniaudio (`click()` is only fired by `Sony35Drive` today — the
5.25" insert/eject click has no live call site).

**Step/seek decision** (MAME parity): `step(newTrack, emuCycles)`
measures gap in emulated CPU cycles (MAME `floppy.cpp:~1532-1540`).
Wall-clock audio frames would be wrong under disk turbo (~60×):
PROM's full phase sweep lands in one audio buffer → gap=0 → buzz.

- `gap > 50 ms` (`kSeekJoinMs`) → single-step click.
- `gap ≤ 50 ms` → seek mode: pick seek sample whose nominal cadence
  is closest (2/6/12/20 ms), pitch-scale (`pitch = nominal_ms /
  gap_ms`), loop.
- No step for `kSeekTimeoutMs` → exit, final `step_1_1`.

Floor at 1 ms gap defends `mixLoop` against `INF` rate; pitch in
[1, 2] for `SEEK_2MS`.

**Wall-clock motor-off hold-off**: turbo bumps CPU ~60× → 1-sec
spin-down (`motorOffDelay = 1'022'727` cycles) becomes ~17 ms
wall-clock. Device defers audible transition by `kMotorOffHoldMs`
(default 800 ms) in **audio output frames** not cycles; fresh
`motor(true)` cancels.

**CPU ↔ audio**: mutex-guarded `std::vector<Cmd>` queue. CPU pushes
`MotorOn/MotorOff/Step/Click`; audio thread drains at top of
`fillAudioBuffer`.

**Hook points in `DiskIICard`**: `seekPhaseW` end → `step(head/4)`;
`control()` `$C0E9` MODE_IDLE→ACTIVE → `motor(true)`;
`advanceCycles()` when `motorOffDelay` expires → `motor(false)`;
`handleSwitchAccess()` legacy 32-cyc gate immediate motor toggle.

Owned by `EmulationController` (audio shutdown drains thread).
Persisted: `floppy_sound_volume`, `floppy_sound_muted`. Pinned:
`floppy_sound_smoke_test`.

## TransWarp (Applied Engineering)

Catalog `transwarp`. Port of MAME `bus/a2bus/transwarp.cpp` (R. Belmont), a
3.58 MHz accelerator for the II / II+ / //e. Full notes in
`src/TranswarpCard.h`; the parts worth having in the map:

**The one structural divergence.** MAME puts a SECOND `W65C02` on the card
and has it DMA the Apple's bus for every access, because a MAME a2bus card
cannot retime the host CPU — substituting a faster processor is the only
lever it has. POM2 has the lever: the worker's per-frame budget IS the CPU
clock, so the card publishes a multiplier (`SlotPeripheral::
cpuSpeedMultiplier`, aggregated by `SlotBus`, applied in
`EmulationController::scaledFrameBudget`) and the machine keeps its own
`M6502`. That is closer to the board — same program, same memory, faster
clock — and costs nothing on the hot path.

**The multipliers are exact rationals.** The card is clocked from the 7M
line: full = 7.159/2 = 3.579545 MHz, half = 7.159/4. Against the Apple's
14.31818/14 that is **3.5×** and **1.75×** exactly, not fitted decimals.

**No detection register.** The card has no slot ROM and no `$C0nX` window;
MAME's header says outright there is no way to detect it besides timing
vblanks. Everything is `$C072` / `$C074`, snooped off the bus — hence
`SlotPeripheral::snoopsBus()` / `busSnoop()`, cached by SlotBus as one
pointer so the snoop sites (`Memory::softSwitchAccess` for `$C07x`, the four
`SlotBus` window dispatchers) cost a null test on any ordinary machine.
`$C074` is the one address the card takes OFF the bus (MAME's `dma_w`
returns there, so it never reaches the paddle latch); `$C072` is watched and
passed through. That asymmetry is in the MAME source and is kept.

**Sampling.** The multiplier is read once per frame; the slowdown windows
(`$C090-$C0FF` and `$C100-$C7FF` → 20 µs, `$C070` → a whole PREAD) are ~20
cycles inside a ~17000-cycle frame. That is not an approximation in
aggregate — sampling a duty cycle at a rate uncorrelated with it is an
unbiased estimator of it, so the average speed converges. What it does not
reproduce is WHERE inside a frame the slow cycles fall.

**POM2 does not need the slowdowns to keep peripherals correct**, and this is
worth being explicit about: its whole time base is CPU cycles, so a Disk II
at 3.5× spins 3.5× faster in wall-clock and the nibble pacing per CPU cycle
is unchanged — the same reason the //c Plus profile runs at 4× with a working
drive. They are modelled because they are what the board does.

**ROM shadow**, ROM-gated on `roms/ae_transwarp_1.4.bin` (4096 B, CRC32
`afe37f55`, MAME `ROM_START(warprom)`; POM2 does not ship it). AE's
speed-corrected Monitor overlays `$F000-$FFFF` until software writes `$C072`
— the stock F8 delay loops are calibrated for 1 MHz and come out 3.5× short
otherwise. Implemented as a straight 4 KB swap in the ROM mirror
(`Memory::loadRomBytes`), free at run time, with the displaced Apple bytes
kept for the swap back. Without the dump the card accelerates and simply
never shadows.

DIP switches persist as `transwarp_dsw1` / `transwarp_dsw2`. Note DSW2 bit 5
defaults to 0: **slot 6 ships at stock speed** — that is the Disk II, the one
slot AE did not trust at 3.5×. Pinned by `transwarp_card`, which drives the
card through a real `Memory` + `SlotBus` because the snoop hooks live there.

## Slot bus & IRQ aggregation

`SlotBus` + `SlotPeripheral`, 8 slots. Memory routes 4 windows:

- `$C080-$C0FF` device-select (16 B/slot N at `$C080+N*16` ; slot 0
  = LC hook, 1-7 = expansion).
- `$C100-$C7FF` slot ROM (256 B/slot 1-7).
- `$C800-$CFFF` shared expansion ROM, owned by whichever slot most
  recently touched `$CnXX`. `$CFFF` deactivates active slot;
  auto-latch on slot-ROM access.

`advanceCycles()` forwards to every plugged card. Ctrl-Reset
propagates `onReset()`.

### IRQ wire-OR

`M6502::setIrqLine(sourceId, asserted)` — wire-OR. 32-bit OR'd
contributor mask: slot N (1..7) = bit N, VBL = bit 8, legacy
`setIRQ(int)` = bit 31. NMI is a single latch. Pinned:
`irq_aggregator_smoke_test`.

### `SlotPeripheral::assertIrq` API

Cards never poke `cpu->setIrqLine` directly. Protected
`assertIrq(bool)` debounces against `irqAsserted_` cache
(idempotent — only edges propagate), fans out via
`SlotBus::forwardSlotIrq(slot, asserted)` to whatever `IrqRouter`
Memory installed (`Memory::setCpu(cpu)` plants a closure).
`SlotBus::plug()/unplug()/clear()` auto-release pending IRQ
contribution. Pinned: `slot_peripheral_irq_smoke_test`.

Mockingboard keeps `cpu_` for `getCycleCountNow()` lazy-sync only;
Disk II keeps `cpu_` for sub-instruction LSS accuracy on Q6L reads.
MouseCard and SSC dropped `cpu_`.

### Hand-written slot ROMs (`SlotRomAsm.h`)

Six cards have no ROM dump and synthesise their `$Cn00` page: `SmartPortCard`,
`ProDOSHardDiskCard`, `FujiNetCard`, `PrinterCard`, `SuperSerialCard`, and
`GrapplerCard`'s fallback stub. (`ClockCard` writes nine fixed signature bytes;
`DiskIICard`'s boot PROM is a verbatim dump. Neither is assembled.)

Until 2026-08-28 all six wrote the page as a byte list with every **address in
it computed by hand**:

```cpp
0xF0, 0x37,              // BEQ write   (+55 -> $Cn91)
0x4C, 0xC0, kSlotRomHi,  // JMP $CnC0
rom[0xFF] = 0x50;        // ProDOS driver entry offset
```

Three ways to be wrong, all of which fired: a region outgrew its budget and
overwrote its neighbour (SmartPortCard's write routine ate its own ProDOS
STATUS, which answered `$27` on a healthy bay for weeks); a region *shrank* and
left a displacement pointing past the routine it named, changing no byte a
hexdump would flag; or a displacement was simply mistyped — `BEQ +55` carried a
comment recording that somebody had already re-counted it once.

`pom2::SlotRomAsm` removes the cause rather than guarding the symptom. **An
address is never typed. It is a label, and the assembler computes the byte.**

```cpp
pom2::SlotRomAsm a(rom_, slot_, "ProDOSHardDiskCard");

a.region("entry", 0x00, 0x08)
 .jmp("boot")                       // $Cn01 = $20 falls out of the operand
 .poke(0x03, 0x00);                 // platform-mandated signature byte

a.region("driver", 0x50, 0x66)
 .emit({ 0xA5, 0x42, 0xC9, 0x01 })
 .branch(0xF0, "read")              // displacement resolved in finish()
 .label("dispStatus").jmp("status");

a.region("tail", 0xFE, pom2::kSlotRomBytes)
 .emit({ 0x03 }).byteOf("driver");  // ProDOS driver entry offset

romLayoutError_ = !a.finish();
```

| call | emits | replaces |
|---|---|---|
| `region(name, start, limit)` | nothing; opens a bounded span and defines `name` at its start | a comment claiming where a routine lives |
| `label(name)` | nothing | a hand-counted offset |
| `emit({…})` | raw bytes, bounded | the same, unbounded |
| `branch(op, label)` | opcode + displacement | `0xF0, 0x37` |
| `jmp/jsr(label)` | opcode + lo + `$Cn` | `0x4C, kStatusOff, kSlotRomHi` |
| `byteOf(label)` | one byte = the label's offset | `rom[0xFF] = kDriverOff;` |
| `poke(offset, v)` | one byte at a mandated address, inside the open region | a bare `rom[0x05] = 0x38;` |

`finish()` resolves every reference and returns false on: a region over budget,
**two regions claiming the same bytes** (the SmartPort bug in its purest form —
the previous bounded builder checked each region against its own limit and
never against the others), a branch outside the −128..+127 window, a reference
to an undefined label, a duplicate label, a poke outside the open region, or a
byte emitted with no region open. The first error is kept, with a message that
names the region; cards publish it as `romLayoutError()` and every card's smoke
test asserts it is clear.

**It is not `constexpr`, and the reason matters**: a slot page is parameterised
by the slot it is plugged into (every absolute reference carries `$Cn`) and the
slot comes from settings at runtime, so there is no constant to fold. What
TODO P1-1 actually asked for — symbolic labels, declared regions, resolved
branches, a diffable listing — is all here; the checking is at build time of
the *page*, which happens once at card construction.

**The listing.** `POM2_DUMP_SLOT_ROM=1` makes every card print its page as it
is built, which is the form you want — one file per build, diffable against the
last one:

```
; ProDOSHardDiskCard — page $C500

driver  $050..$066  22 of 22 bytes
    $062  dispStatus:
    $050  A5 42 C9 01 F0 10 C9 02
    $058  F0 33 C9 00 F0 04 A9 01
    $060  38 60 4C C0 C5 EA

write  $08D..$0C0  43 of 51 bytes
```

The occupancy column is the point: `43 of 51` is how you see a routine
approaching its budget before it crosses. `write` used to read `47 of 47`.

`slot_rom_asm` tests the assembler itself, because a card test can only assert
its flag is clear — which a `finish()` of `return true` would also satisfy.

## Storage

### Read / write matrix by format

Every media format POM2 mounts, and whether it can be written back.
"Write" means a guest (or host) modification survives `saveDirty()` into
the *source file*; every one of them commits through the atomic +
durable path (§ Write-back commit), never an in-place `trunc`.

| Format | Bay / backend | Read | Write back | Pinned by |
|---|---|---|---|---|
| `.dsk` / `.do` 143 360 (DOS 3.3 skew) | 5.25" `DiskImage` | ✅ | ✅ | `disk_writeback_smoke`, `dos33_save_smoke` (real DOS 3.3 SAVE) |
| `.po` 143 360 (ProDOS skew) | 5.25" `DiskImage` | ✅ | ✅ | `prodos_save_smoke` (real ProDOS 2.4.3 SAVE) |
| `.nib` 232 960 (35 × 6656) | 5.25" `DiskImage` | ✅ | ✅ | `disk_writeback_smoke` (verbatim re-save) |
| `.nib` CNib2 223 440 (35 × 6384) | 5.25" `DiskImage` | ✅ | ✅ | `disk_cnib2_smoke` (load **and** write-back: the 6656 pad must truncate back to 6384, or the next load misdetects the format) |
| `.d13` 116 480 (13-sector, 5-and-3) | 5.25" `DiskImage` | ✅ | ✅ | `d13_roundtrip_smoke` |
| `.2mg` wrapping DOS / ProDOS / nib | 5.25" `DiskImage` | ✅ | ✅ | `disk_2mg_writeback_smoke` (header + trailer preserved byte-for-byte) |
| `.woz` 5.25" (WOZ1 / WOZ2, bit cells) | 5.25" `DiskImage` | ✅ | ✅ | `woz_writeback_smoke` — dirty quarter-tracks spliced back, header CRC32 zeroed per Applesauce 2.1 |
| `.woz` 5.25" **carrying FLUX tracks** | 5.25" `DiskImage` | ✅ | ❌ **by design** | forced WP at load: POM2 cannot serialise delta streams, and accepting writes would report a successful save while discarding them |
| `.po` / `.2mg` 819 200 (800 K) | 3.5" `Disk35Image` | ✅ | ✅ | `disk35_atomic_save` (both kinds; 2IMG envelope preserved) |
| `.dsk` / `.image` 819 200 (800 K) | 3.5" `Disk35Image` | ✅ | ✅ | `disk35_atomic_save` — writability comes from the FILE, never the extension (the old "read-only by convention" rule overrode the user's own opt-in) |
| `.woz` 3.5" (`INFO.disk_type = 2`) | 3.5" `Disk35Image` | ✅ | ❌ **by design** — but see *Convert* | `woz35_load::testWriteProtected` — giving blocks back would mean re-encoding the user's flux, which POM2 cannot do. **Convert to writable `.po`**: the 3.5" panel offers it on any mounted WOZ (`Disk35Image::exportRawTo` → `MainWindow::convertWoz35ToPo`). The decode already produced the 1600 blocks a `.po` holds, so the copy costs nothing, mounts with write-back on, and the `.woz` is left untouched as the master |
| `.hdv` (any 512-aligned size) | `Block512Backing` | ✅ | ✅ | `hdv_writeback_smoke` |
| `.2mg` hard disk (> 800 K) | `Block512Backing` | ✅ | ✅ | `hdv_writeback_smoke` (header + creator/comment trailer survive) |
| CFFA / IDE volumes | `CffaCard` → `Block512Backing` | ✅ | ✅ | shares the backing above |
| SmartPort units (`.po`/`.2mg`/`.hdv`) | `SmartPort*Unit` | ✅ | ✅ | `smartport_write_dispatch`, `smartport_mixed_units_smoke` |
| Host folder → ProDOS volume | `ProDOSVolume` | ✅ | ✅ | `prodos_volume_smoke` (decode → host files, collision-safe, idempotent) |
| `.wav` cassette | `CassetteDevice` | ✅ | ✅ | `cassette_wav_tail_smoke` |
| `.aci` cassette | `CassetteDevice` | ✅ | ✅ | `cassette_wav_tail_smoke` |

**Write-protect is the union of four independent sources**, and any one
of them alone makes a medium read-only: the user's `writeBackEnabled`
opt-in (off by default), the 2IMG header's write-protect flag, the host
file being read-only on disk, and a per-format "physically WP" rule (the
two ❌ rows above). `isWriteProtected()` folds them together, so nothing
can be written by accident and nothing is silently dropped.

Note what is NOT in that list: the file's **extension**. It used to be —
a bare 800 K `.dsk`/`.image` was marked *physically* WP because such
dumps are "sometimes read-only by convention". That is the wrong layer
for a convention: the physical tab outranks `setWriteBackEnabled`, so a
user could ask for write-back on a `.dsk`, be refused, and be told
nothing — while the byte-identical payload under a `.po` name wrote
fine. Conventions belong in what a *user* sets, not in what the format
layer decides for them.

**The two ❌ rows are refusals, not gaps.** Both are cases where POM2
could accept the write and lose it at flush — the failure mode that
forecloses on the user's only copy. Refusing at mount is the honest
behaviour; re-encoding flux is the work that would lift either.

### DiskImage

143 360-byte 5.25": `.dsk`/`.do` (DOS 3.3 skew) or `.po` (ProDOS).
Pre-nibblized into 35 × 6656-byte tracks. GCR per "Beneath Apple
DOS". Skew tables (physical → logical):

- DOS 3.3: `{0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15}`
- ProDOS:  `{0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}`

Write-back via `saveDirty()` (`.dsk`/`.do`/`.po`/`.nib` + `.2mg`
envelopes + `.woz`) opt-in via `setWriteBackEnabled(true)`.

#### Two-phase media mount (`MediaMount.h/.cpp`)

`stateMutex` is taken by the CPU worker for every 4096-cycle chunk **and** by
the UI thread to paint every frame. Anything slow held inside it therefore
freezes the machine and the window together — including the button that would
cancel it, because rendering that button needs the same lock.

Mounting a disk used to do all of its file I/O in there. Not by oversight: the
API gave the caller no choice. `DiskIICard::insertDisk(drive, path)` flushes
the outgoing medium, reads the new file, decodes it and installs it in one
call, so a caller that needed the install serialised had to serialise the read
too. A 2026-08-22 audit counted ~20 such sites across `MainWindow.cpp`,
`MainWindow_Slots.cpp` and `AiControlServer.cpp`.

Measured on a warm cache, so these are optimistic floors:

| Operation | Cost | In PAL frames (20 ms) |
|---|---|---|
| read a 32 MB `.hdv` | 12.8 ms | 0.6 |
| write 4 MB + one `fsync` | 30.1 ms | 1.5 |
| `AtomicFileReplace` commit | two `fsync` | — |

The fix is the API shape, and it dissolves every site at once:

- **Phase 1**, unlocked — `DiskIICard::prepareDisk(path, writeBack, out, err)`
  reads and decodes into a detached `DiskImage`. Touches no card state.
- **Phase 2**, locked — `DiskIICard::installDisk(drive, std::move(prepared))`
  swaps the finished object in and re-anchors the LSS. A move plus arithmetic.

`pom2::mountDiskII()` wraps the pair so a call site is one line — which also
keeps the logic out of `MainWindow.cpp`, the god-object the file-size ratchet
is now holding still. `insertDisk(path)` stays as the inline form for the
single-threaded callers (CLI, headless, tests) where it reads better.

Two things are deliberately **not** optimised **on the Disk II 5.25" insert
path** (`DiskIICard::installDisk`; block devices and 3.5" media took the other
road — see *Eject and flush are three-phase* below):

1. **The outgoing flush stays under the lock.** If the medium being ejected has
   unsaved changes, `installDisk` still commits it inline
   (`flushOutgoingForSwap`, `DiskIICard.cpp:379/394-397`) and still refuses the
   swap when that commit fails. Moving it out would mean either swapping before
   knowing whether the old medium could be written — losing the user's changes
   when it cannot — or handing the dirty image back for the caller to commit,
   which loses them if the caller drops it. Latency is worth less than the only
   copy of somebody's disk. Rare in practice: write-back is opt-in, so the
   default clean medium takes the fully unlocked path.
2. **Same-file re-insert degrades to the inline cost.** Phase 1 reads *before*
   phase 2 flushes, so re-inserting a file the guest has written to would
   install pre-flush bytes and roll the writes back. `installDisk` detects the
   collision (`getPath()` match + `hasUnsavedChanges()`), flushes, then re-reads
   under the lock. Correctness first; the case needs write-back on *and* an
   exact path match.

#### Eject and flush are three-phase *(2026-09-07)*

Mounting was two-phase from the start; ejecting was not, and an eject writes
the file the user cares about. It now has the same shape, one phase longer
because a failure has to be undoable:

* **Phase 1, locked** — `MountableMediaCard::prepareEjectBay(bay, out, err)`
  (`MountableMediaCard.h:184`) / `prepareFlushBay` (`:164`) *capture* the
  payload: the image bytes plus the dirty-block set, as a
  `Block512Backing::PendingWriteBack`. Opt-in by design — a card that does not
  override them returns false with an EMPTY payload and the caller falls back
  to the inline form. Implemented by `SmartPortCard.cpp:1138`,
  `ProDOSBlockCard.h:132`, `LironCard.cpp:426`.
* **Phase 2, unlocked** — `Block512Backing::commitWriteBack` writes and
  `fsync`s. This is where the 800 KB / 32 MB and the `fsync` live, off the
  machine lock.
* **Phase 3, locked again** — re-resolve the card from the SlotBus (it may have
  been unplugged while the lock was open, `StorageCoordinator.cpp:878-891`) and
  either retire the bay or, on failure, put the medium back:
  `DiskIICard::restoreEjected`, `restoreFlushBayDirty`. **A failed commit leaves
  the disk loaded and dirty**, so a retry loses nothing — it used to only log a
  line about a disk that was already gone.

Drivers: `StorageCoordinator.cpp:792` (HDV eject), `:872` (bay eject), `:1375`
(`ejectAllMedia` — the last inline holdout, converted the same day), `:1750`
(`flushAll`'s Liron capture). The **firmware** 3.5" eject cannot block at all,
so it hands the payload to `EmulationController::WriteBackQueue`
(`EmulationController.h:387-424`, its `Disk35WriteBack` thread spawned at
`EmulationController.cpp:444`) and `Sony35Drive::ejectPending_`
(`Sony35Drive.h:280`) holds the mechanical eject until the sink reports the
file landed. `drainDeferredWriteBacks()` is called before the settings are
written at quit, so the queue cannot outlive the process.

One caller keeps the old inline form on purpose: the profile-switch remount in
`MainWindow_Slots.cpp`, where the SlotBus rebuild and the remounts must be one
atomic step against the AI server's handlers. The stall is invisible there —
the CPU worker is already stopped and a cold boot follows.

Pinned by `two_phase_mount`. Its case 3 is the one that earns its keep, and it
took two attempts to write: the obvious assertions (the file changed, the card
has no unsaved changes) pass whether or not the collision is detected, because
a clean medium is never written back. What discriminates is what the *guest*
would see — step the head to another track, write there, flush, and check that
track 0 still carries the first burst. A stale mounted image writes its whole
self back on that second flush and silently reverts track 0. Verified
falsifiable: with the collision check forced to `false`, the test fails on
exactly that assertion.

#### The block-device half (HDV / 2IMG), converted 2026-08-22

`Block512Backing::loadImage` was 131 lines doing four things: flush the
outgoing medium, read the file, parse the 2IMG container, adopt the result. It
is now split at the seam that matters —

* `readImageFile(path, PreparedImage&, error)` — **static**, because it touches
  no object state at all: open, size gates, read, and the host-writability
  probe (a syscall, so it belongs on this side). Runs with no lock held.
* `adoptImage(PreparedImage&&)` — flush, parse, adopt. Under the lock, and it
  makes **no syscalls**: everything it needs is in the struct.

`ProDOSBlockCard` and `SmartPortUnit` each carry a matching `adoptImage`;
`CffaCard`, `ProDOSHardDiskCard` and `SmartPortHdvUnit` forward it to their
`Block512Backing` in one line each. `SmartPortUnit`'s default returns false —
a 3.5" unit has no block backing, and a stub would be a worse abstraction than
letting `pom2::mountSmartPortUnit` fall back to the inline path.

**The trap, for whoever touches this next.** `loadImageFromBytes` was already
on the interface and looks exactly like a ready-made phase 2. It is not. It is
for **synthesised** volumes: it skips the 2IMG header parse, forces `synth_`,
and ties `supportsWriteBack_` to it. Route a real `.hdv`/`.2mg` through it and
the container's 64-byte header becomes block data, while write-protect and
write-back go quietly wrong. `two_phase_block_mount` case 2 exists to fail
loudly if anyone "simplifies" back onto it — verified by doing exactly that.

**What it bought, measured on a 32 MiB image:**

| | before | after |
|---|---|---|
| held under `stateMutex` | 25.8 ms | **0.0 ms** |
| unlocked read | — | 13.5 ms |
| inline `loadImage` total | 25.8 ms | 13.4 ms |

The locked half reaching zero is not rounding: after the parse, a raw `.hdv`
(no container, payload is the whole file) **moves** the buffer phase 1 already
allocated instead of copying it — a memcpy of the medium becomes a pointer
swap. A 2IMG still copies, because its payload starts 64 bytes in and moving
then shifting would be the same memcpy wearing a different hat.

The inline path halving is a side effect of the same change, and it is free:
the CLI, the tests and the profile-switch remount all go through `adoptPrepared`
too.

#### How a media write-back commits (`AtomicFileReplace.h`)

Every write-back path — `DiskImage`, `Disk35Image`, `Block512Backing`,
`ProDOSVolume`, plus `Settings`, `PrinterHistory`, `CassetteDevice` and the
ImageWriter exports (`ImageWriterPdf.cpp:192`, through `writeFileAtomic`) —
writes a **sibling temp file** in the same directory ⇒ same filesystem ⇒ the
rename cannot fail cross-device, carries the original's permissions onto it,
then commits through `pom2::replaceFileAtomic`.

The temp name is **unique per process and per call** (`tempSiblingPath`,
`AtomicFileReplace.h:186`): `<path>.<pid>-<counter>.pom2tmp`. A fixed
`<target>.tmp` meant two POM2 instances on one `$HOME` — or one instance
writing the same image twice — truncated each other's in-flight write and
published a file made of both. `prepareTempPath` still runs on the result:
a name being unlikely is not the same as a path being safe. `replaceFileAtomic`
also **follows** a symlinked target rather than replacing the link. Never `trunc` the user's own file: an ENOSPC /
removable-media / network-share failure part-way through would leave the
ONLY copy of the disk truncated, since the rest of it lives in RAM.

`replaceFileAtomic` is where **durability** lives, not just atomicity
(2026-08-14). A rename is atomic for a *reader*; it promises nothing about a
power cut. So the helper: (1) `fsync`s the temp file's contents **before**
the rename publishes them — otherwise the directory entry can reach the
journal while the data blocks are still in page cache, and the user finds a
0-byte file where their disk image was; (2) renames (Windows:
`MoveFileExW` + `MOVEFILE_WRITE_THROUGH`); (3) `fsync`s the parent directory
so the rename itself survives, best-effort because plenty of filesystems
refuse that and none of those refusals invalidate step 1.

Failure policy: a real I/O error (EIO/ENOSPC) fails the save so the caller
keeps its dirty state and the user can retry; a filesystem that merely
*cannot* honour the flush (EINVAL/EOPNOTSUPP — network mounts, Emscripten's
MEMFS) reports success, because failing every save over a missing guarantee
is worse than saving without it. Pinned by `atomic_file_replace`.

### Format detection

`detectFormat()` + `enum ImageKind`. `loadFile(path)` slurps once,
dispatches by content. Order: MacBinary strip → 2IMG envelope → WOZ
magic → 35×6656 NIB → 35×6384 CNib2 → 143 360-byte sector. Unknown
→ false + specific `lastError`.

- **Skew sniff** (143 360 branch): validates ProDOS vol-dir key
  block at `file[0x404]` (`.po`) vs `file[0xB04]` (`.dsk`),
  overrides extension when only the other position fits. Predicate:
  `prev=0`, plausible `next`, storage_type `$F`, name chars in
  `A-Z 0-9 .`.
- **2IMG**: 64 B header → format byte (0=DOS, 1=ProDOS, 2=NIB),
  flags (bit 0 = WP, bit 8 or 31 = vol# present), dataOffset,
  dataLength. Raw header + trailer captured into
  `twoImgHeaderRaw`/`twoImgTrailerRaw`; `saveDirty()` re-emits
  both so envelope stays byte-identical.
- **MacBinary** 128 B prefix stripped (AppleWin predicate: `b[0]==0`,
  name length [1..63], terminator + reserved zeros).
- **CNib2** (35×6384): pad to 6656/track on load with `$FF` (sync),
  truncate to 6384 on save.
- **Volume number**: per-image (2IMG flags or default $FE), threaded
  through `nibblizeTrack(track, sectors, vol, skew)`.

Pinned: `disk_image_smoke`, `disk_skew_sniff_smoke`, `disk_2mg_smoke`,
`disk_2mg_writeback_smoke`, `disk_macbinary_smoke`, `disk_cnib2_smoke`,
`disk_refuse_smoke`.

`classifyDiskForSlot` (`DiskImage.*` — `DiskSlotClass` =
`Floppy525/Sony35/Hdv`) picks the controller for the positional-disk CLI,
the kiosk scan and the **drag-and-drop autoboot**. `.hdv` is HDV at any
512-aligned size. `.dsk`/`.image` at exactly 819200 are Sony 3.5", not
5.25" — `Disk35Image` takes a bare 800K payload under those names too.

**`.2mg` is classified by PARSING the header, never by the file size**
(`read2mgPayloadLength` — magic + `dataOff`/`dataLen` at bytes 24/28,
validated against the real file length). The envelope allows an arbitrary
data offset and a comment/creator trailer, which is what `Block512Backing`
has always parsed; size arithmetic refused ordinary CiderPress output
outright. Payload 143360/232960/223440 → 5.25", 819200 → 3.5", any other
512-multiple → HDV. A file that is not a readable 2IMG falls back to the
old size heuristics, so a malformed envelope degrades instead of breaking.
Pinned: `cli_kiosk`.

The Disk Library's own `accept525/accept35/acceptHdv`
(`DiskLibrary_ImGui.cpp`) mirror these rules exactly — **except** for
`.2mg`, where they keep cheap size rules on purpose: they filter every file
of a directory scan and cannot afford to open each one.

`insertAndBootImage` (`MainWindow.*`) is the single mount+boot entry point
behind all three. It auto-plugs a controller when the config lacks one —
`ensureHdvCardForBoot()` / `ensureSmartPortCardForBoot()`, both
session-local and never persisted — and it **honours `bootFromSlot`'s
`bool`**: that call degrades to a plain cold boot when the card carries no
`$Cn01/03/05` JSR-dispatch trio, so reporting "booted" without checking it
was a lie the user saw as a BASIC prompt. A drop with no main ROM loaded is
refused up front for the same reason.

### 13-sector (5-and-3, pre-DOS-3.3)

DOS 3.1/3.2/3.2.1: 13 sectors/track × 5-and-3 GCR. Image = 35×13×256
= **116480 B**. `detectFormat` maps that size → `Dos32_13` (always
DOS order); `loadSectorImageFromBuffer` calls `nibblizeTrack13` and
sets `sectorsPerTrack_=13` (`is13Sector()`).

Codec = verbatim MAME `formats/ap2_dsk.cpp` `a2_13sect_format`:
`nibblizeTrack13/writeDataField13` (encode, `kTranslate5[32]`, addr
prologue `D5 AA B5`, data prologue `D5 AA AD`, 411-nibble data) +
`decodeTrack13/kUntranslate5` (write-back). Physical interleave
`sector = (i*10)%13`. Pinned **byte-for-byte round-trip**:
`d13_roundtrip_smoke_test`.

**Boot wiring**: `DiskIICard` serves 341-0009 boot PROM
(`roms/disk2_13.rom`) at `$Cn00` while a 13s disk is mounted
(`serving13_ = any 13s && bootRom13Loaded`). 13-sector disks
**force the bit-level LSS** — the 341-0009 read loop is tighter
than the legacy 32-cycle gate. The **read sequencer stays 16-sector
P6** (341-0028); the LSS is encoding-agnostic, 5-and-3 decode is
software (boot PROM + DOS 3.2 RWTS). Pinned: `dos32_boot_trace`.

### `.woz`

Verbatim port of MAME `lib/formats/woz_dsk.cpp`. WOZ stores raw bit
cells — survives copy protections that tweak timing. WOZ1 (160 ×
6656-byte slots, `bit_count` @+6648 u16) and WOZ2 (160 × 8-byte TRK
headers, data at `starting_block × 512`, `bit_count` u32). Bits
MSB-first. All 160 TMAP quarter-track slots are unpacked (FLUX
takes precedence over TMAP for a populated slot), so sub-qt
protection positions (Locksmith, David-DOS) are preserved.

**Write-back**: `loadWoz()` snapshots file to `wozRaw` +
per-qt-track `(byteOff, byteLen, bitCount)`; `writeFlux()` splices
into `bitStream[qt]`; `saveDirty()` repacks + zeros CRC32
(Applesauce "not computed" sentinel) and commits through `writeFileAtomic`
— sibling temp + `fsync` + rename, re-emitting the 2IMG/MacBinary envelope
around `wozRaw` when `twoImgFormat` (`DiskImage.cpp:2509-2525`). It has never
rewritten in place.
`isWriteProtected()` honours both user toggle and
`INFO.write_protected`. `DiskIICard::insertDisk` forces
`useBitLss=true` when any drive holds WOZ. Pinned:
`woz_load_smoke`, `woz_writeback_smoke`.

### WOZ2 `optimal_bit_timing`

INFO+39 (units of 125 ns) — bit-cell duration. Default 32 = 4 µs =
standard cell @ 2 MHz LSS = 8 LSS cyc/cell. `loadWoz` reads when
`info_version >= 2`, clamps [8, 64], stores in `optimalBitTiming`.
`lssCyclesPerCell() = optimalBitTiming / 4`. `expandTrackFlux`
emits each "1" cell at `i*cyc + cyc/2` (centre). Pinned:
`woz_bit_timing_smoke` (obt 32/40/28 + WOZ1 fallback).

### DiskIICard

256-byte P5A boot PROM. Apple 341-0027-A (CRC `ce7144f6`) embedded
as `kBootPromDefault[256]`; `loadBootRom("roms/disk2.rom")` overrides.
PROM autodetects slot via `JSR $FF58 / TSX / LDA $0100,X`. Soft
switches `$C0E0-$C0EF`: phases, motor, drive_select, Q6L/Q6H,
Q7L/Q7H.

**Boot signature** (Apple II Ref Manual Appx C): `$Cn00` starts with
`$20 ?? $00 $03` at offsets 1/3/5 (JSR dispatch trio). `$Cn07`
distinguishes Disk II / SmartPort (`$3C`, scanned by F8 Autostart
`341-0020-00`) from ProDOS block devices (`$01` for non-removable
HDV). F8 ONLY auto-scans `$Cn07=$3C`; HDV needs `PR#N` /
`bootFromSlot`.

`bootFromSlot()` validates the JSR trio so clicking "Boot" on a
non-bootable card warns + falls back to `coldBoot`. `$Cn07=$3C` is
NOT validated — would reject HDV.

**Drive switching** via `selectDrive(int)` mirrors MAME
`machine/wozfdc.cpp:264-291`. When motor active: flush in-flight
write on old drive (= MAME `mon_w(true)`), clear OLD drive's
`revolutionStartLssCycle` to `kNeverRev`, anchor NEW drive's to
current `lssCycle` (= MAME `mon_w(false)`). Per-drive
`revolutionStartLssCycle[2]` matches MAME
`floppy_image_device::m_revolution_start_time`. Disk angular
position = `(lssCycle - revolutionStartLssCycle[drive]) mod
track_period`. Pinned: `disk_drive2_smoke`,
`mame_lss_parity_smoke`.

### DiskII multi-instances

**Four** card keys may sit in more than one slot —
`SlotConfigurationCoordinator::isMultiInstance` (`SlotConfigurationCoordinator.cpp:99-104`)
returns true for `diskii`, `cffa`, `smartport35` and `liron`; every other key
is red-flagged as a duplicate. (`firstOccurrence` is gone. Note the Slot Config
panel's own `isDuplicate` lambda, `MainWindow_Slots.cpp:171-181`, still
short-circuits on `"diskii"` alone, so it warns about a second `cffa` /
`smartport35` / `liron` the coordinator would accept — a UI-only divergence.)
Two Disk II cards load the same `disk2.rom` + `diskii_p6.rom`. Per-card
2 drives + LSS state.

**Primary**: there is no cached card list. `MainWindow::diskIICards()` queries
the SlotBus live, in slot-ascending order, and `MainWindow::primaryDiskII()`
(`MainWindow.h:1242`) is the lowest-slot one. Per-slot persistence:
`disk_path_slotN` / `disk_writeback_slotN`, the key spelled in exactly one
place (`pom2::diskIIPathSettingKey`, `StorageCoordinator.h`). Primary also
writes legacy unsuffixed keys for older builds. A profile switch captures
`StorageCoordinator::captureRebuildSnapshot(bus)` → `RebuildSnapshot`
from live cards before tear-down. **IWM wiring**: only slot-6
`DiskIICard` calls `card->setIWM(&controller->iwm())`.

### Two read paths

- **Bit-level LSS** (default when `roms/diskii_p6.rom` present) —
  verbatim port of MAME `machine/wozfdc.cpp` + flux-event subset
  of `imagedev/floppy.cpp`. MAME `cycles` = 2× CPU clock.
  `lssSync(extra)` catches up from `lssCycle` to `cyclesLimit =
  cpuCycleTotal*2 + extra`. PULSE from
  `DiskImage::getNextTransition(track, lssCycle)` (event @
  `cellIdx*8 + 4`, cell centre). Reads of `$C0EC` pass `extra=1`
  after `control()` (read-pipe latency). P6 PROM (341-0028-A)
  indexed by `(state<<4) | (Q7<<3) | (Q6<<2) | (QA<<1) | (!PULSE)`.
  Pinned: `diskii_lss_smoke`, `mame_lss_parity_smoke`.
- **Legacy 32-cycle gate** (fallback) — `kCyclesPerNibble = 32`;
  nibble every 32 cycles, `byteReady` toggles for BPL spins.
  2–3× faster than LSS in stock boots.

**An empty drive delivers noise, not silence.** Both gates have to answer a
guest that polls `LDA $C08C,X / BPL -3` on a drive with nothing in it — every
multi-disk game does this on drive 2. The legacy gate returns `$FF`
(`deviceSelectRead`); the LSS branch in `lssSync`'s `!isLoaded()` case
synthesises one pseudo-random byte per 8 bit cells (64 LSS cycles) with bit 7
set, hashed from the cycle cursor so snapshot restore and rewind replay it
identically. Freezing the data register instead — what POM2 did until
2026-08-21 — hangs the machine outright: bit 7 never comes up, so the guest
never reaches the timeout that would have turned this into an I/O error.
Ultima V's *Save Music Configuration* (which targets the BRITANNIA disk in
drive 2) froze at `$D407` on any `.woz` for exactly this reason, while
erroring out cleanly from a `.dsk`. Pinned: `diskii_empty_drive`.

### Bit-stream expansion

`DiskImage::bitAt(track, idx)` lazily walks nibble buffer, emits 8
cells per non-FF byte + 2 trailing zero cells per `$FF` inside a
run ≥ `kSyncMinRun = 5` consecutive `$FF`. Sync-FF padding lets
the LSS lose alignment in sync gaps and resync on the next prologue.
`.nib` path skips padding (every byte = 8 cells, total 53248). Cache
invalidates on `writeNibbleAt`.

The ≥5 threshold avoids matching the naturally-occurring 2-byte
in-field `$FF` pairs (4-and-4 address checksum when `vol ^ track ^
sector == $FF`, or 6-and-2 data XOR producing disk `$FF` from
source `$FF $00 $FF`).

### Flux-event view

`fluxEvents(track)` + `trackPeriod(track)` — one event per "1" cell
at LSS-cycle `cellIdx*8 + 4`. `getNextTransition` verbatim MAME
`floppy_image_device::get_next_transition`, wraps across revs.
`writeFlux(track, start, end, count, transitions)` splices flux
window back into nibble buffer.

**Write framing (non-WOZ).** A nibble store has no angular length, so
the flux the head lays down is FRAMED back into nibbles exactly as the
read sequencer frames it: skip 0-cells until a 1, then that 1 plus the
next seven cells are one nibble — the two 0-cells trailing a sync `$FF`
are skipped, which is what makes them sync. Nibbles are laid down
sequentially from the slot the head is over; a mid-nibble splice leaves
that nibble's old value (a real write splice leaves that stub) and
frames into the following slot. `DiskImage::writeFraming[track]` carries
the shift accumulator + destination slot across flushes, because
`DiskIICard` flushes every ~30 transitions and a nibble straddles chunks
constantly. **The cell grid comes from the write clock**
(`fr.origin = burst start`), not the revolution anchor: the head emits
one cell every `lssCyclesPerCell()` LSS cycles, so a burst's transitions
are exact multiples of that apart, whereas the revolution phase puts the
grid at an arbitrary sub-cell offset and rounds adjacent transitions into
the same cell. The anchor is consulted once, to pick the nibble the head
is over. Aligning the write to the OLD track's padded nibble grid (the
pre-2026-07 approach) mangled 345 of a data field's 353 nibbles the
moment the new content padded its sync run differently — see
[CHANGELOG 2026-07-28](CHANGELOG.md). Pinned:
`disk_writeflux_framing`; `POM2_TRACE_WRITEFLUX=1` dumps each splice
window. Note `disk_write_controller_smoke` exercises the **legacy**
32-cycle gate (it never calls `loadLssRom`), so it cannot cover this
path — the shipped app bundles `roms/diskii_p6.rom` and always runs the
LSS/flux one.

**Write-back opt-in plumbing.** `disk_writeback[_slotN]` has to be
re-applied by `plugSlotsFromSettings`' `plugDiskII` (like `plugHdv` /
`plugCffa` do) *and* carried through `applyProfile`'s media snapshot as
`{path, writeBack}`, because `applyProfile` rebuilds every card and the
MainWindow ctor calls it at startup. Miss either and the guest sees a
write-protected disk (`isWriteProtected() == fileWriteProtected ||
!writeBackEnabled`) — DOS 3.3 answers WRITE PROTECTED.

### ProDOSHardDiskCard (HDV — synthetic-block model)

Slot-plugged ProDOS hard disk (default slot 5, label `hdv`) backed by
`.hdv`/`.2mg`. **Deliberate divergence from MAME**: no ATA/SCSI, no
real ROM. The card fabricates its 256-byte slot ROM at runtime
(`buildRom`, hand-assembled 6502) and talks to a host-implemented
streaming protocol on `$C080+slot×16`:

```
off 0  write   block LO byte               (resets stream offset)
off 1  write   block HI byte               (resets stream offset)
off 2  read    next byte of selected 512 B block (auto-incr, wraps)
off 2  write   next byte INTO block         (write-back-gated)
off 3  read    status: bit7 = no image, bit6 = WP
```

`deviceSelectRead/Write` move bytes via host `memcpy` — no GCR, no
flux. `$Cn07=$01` (plain ProDOS block, not SmartPort `$3C`); JSR
trio `$Cn01/03/05 = $20/$00/$03`. F8 Autostart won't scan `$01` →
boot via `PR#n` / `bootFromSlot`.

**Trade-off**: mounts `.hdv`/`.2mg` directly (MAME accepts only
CHD/raw), no card-ROM dump needed; cannot execute real CFFA/SCSI
firmware. The ATA-class port now lives as `CffaCard` (below).

Storage shared with `CffaCard` via `Block512Backing.{h,cpp}`: in-mem
image, 2IMG envelope (header+trailer preserved), medium WP,
dirty-block tracking, opt-in host-file write-back, host-folder synth
volumes. Both cards implement `pom2::ProDOSBlockCard` (image-mgmt
iface) so HDV Library / disk-turbo / persistence target uniformly via
`MainWindow::hdvDevice()` (prefers CFFA when plugged). Also
implements `MountableMediaCard` as a single fixed bay.

Pinned: `hdv_card_smoke`, `hdv_writeback_smoke` (header/trailer/WP/
opt-in round-trip), `hdv_mass_storage_smoke` (32 MB boundary, 16-bit
block addressing, `.2mg` data-offset ≠ 64). Multi-partition images
(CFFA3000-style) not supported — 1 image = 1 unit = 1 volume.

### CffaCard (CFFA 2.0 — MAME-faithful IDE)

`CffaCard.{h,cpp}` + `AtaBlockDevice.{h,cpp}`. **Real 4 KB firmware
dump executed over an emulated ATA chip**, image stored as raw LBA.
Ported from MAME `bus/a2bus/a2cffa.cpp`.

- **`AtaBlockDevice`** — ATA/IDE taskfile subset over
  `Block512Backing`, isomorphic to MAME `ata_interface_device` cs0
  access: `cs0_r/cs0_w(reg)`, 16-bit data register at reg 0.
  IDENTIFY DEVICE ($EC), READ SECTOR(S) ($20/$C4), WRITE SECTOR(S)
  ($30/$C5), LBA28. Unknown commands no-op. DRQ/BSY/DRDY PIO; no
  DMA/IRQ/CHS. Reusable for future Vulcan/Zip/Focus. Pinned:
  `ata_block_device_test`.

  **Gotcha**: CFFA firmware sizes partitions from IDENTIFY **words
  57-58** ("current capacity in sectors"), NOT 60-61 (LBA28 total)
  — leaving 57-58 zero ⇒ "Could not boot partition 1 / Err $28"
  (firmware `$CD35-$CD52` reads $C0n8/$C0n0 for words 57-58).
  `fillIdentify` sets 57-58 = 60-61 = total, word 53 bit 0 (current
  fields valid). Debug: `POM2_TRACE_CFFA=1`;
  `tests/cffa_boot_dump --image X --slot N`.

- **`CffaCard`** — `SlotPeripheral + ProDOSBlockCard`. Decode mirrors
  `a2cffa.cpp`: `read_c0nx/write_c0nx` ($C0nX) drive ATA taskfile
  with 8↔16-bit latch ($C0n0=high byte, $C0n8=low byte+commit;
  $C0n3/$C0n4 toggle EEPROM WE); `read_cnxx` ($CnXX) → `rom[off +
  slot*0x100]`; `$C800` shared expansion, writes WP-gated. Real
  firmware presents `$Cn07=$3C` → **F8 Autostart boots natively** (no
  GUI shortcut).

- **ROM**: user-supplied `roms/cffa20ee02.bin` (6502) /
  `cffa20eec02.bin` (65C02), 4096 B exact (CRC `3ecafce5`/
  `fb3726f8`); plug-time probe picks variant matching CPU. Card type
  hidden from Slot Config when absent. Source: dreher.net
  `Run6_CDROM.zip` (`Firmware/V2.0/`).

- **Image**: `.hdv`/`.2mg` raw LBA (compat preserved). **CHD = phase
  2**. Mounts via HDV Library.

Pinned: `cffa_card_smoke` (ROM-gated). Full MAME oracle: `mame
apple2ee -sl7 cffa2 -hard1 <img>` (romset `~/mame_roms/cffa2/`).

### SmartPortCard (//e Liron-class)

`SmartPortCard.{h,cpp}`. Slot-plugged Apple "Disk 3.5 Controller
Card" (Liron / 670-0186) for //e / II+ / II / //c. Default slot 5.
**Block-level, no IWM** (same synthetic-block divergence as HDV).

**Device-select protocol** (`$C0nX`):
```
$C0n0 write  drive select (0 / 1)
$C0n1 write  block LO byte
$C0n2 write  block HI byte
$C0n3 read   next byte (auto-incr 512 B)
$C0n3 write  next byte INTO current block (WB-gated)
$C0n4 read   status: bit7 = no disk, bit6 = WP, bit0 = latched I/O error
$C0n5 read   STATUS block-count LO (ROM driver reads via LDX)
$C0n6 read   STATUS block-count HI (ROM driver reads via LDY)
$C0n7 write  SmartPort-call param push (cmd, then 10 param-list bytes)
$C0n9 read   SmartPort result stream (STATUS payloads, READ data)
$C0nB/C read result count lo / hi
$C0nD read   WRITE push-page count (2 → 512 bytes expected on $C0n3)
$C0nE write  SmartPort BEGIN · read = EXECUTE (returns error code)
$C0nF read   post-stream error re-poll ($27 after a failed WRITE commit)
```

**SmartPort-protocol dispatch ($Cn0D, 2026-07-12).** Real SmartPort call
convention — `JSR $Cn0D / DFB cmd / DW paramList`, error in A + carry,
return address bumped 3 — served by a 168-byte 6502 handler at **$CE00**
in the card's $C800 bank (`buildC800`): it saves ZP $42-$45, collects the
cmd + first 10 param-list bytes through $C0n7, EXECUTEs via $C0nE, then
moves data guest↔device ($C0n9 pull / $C0n3 push). Commands: STATUS $00
(unit 0 controller status; per-unit general status + 24-bit block count;
statcode $03 = 25-byte DIB with "POM2 SMARTPORT" ID + type $01 3.5"/$02
disk), READ $01, WRITE $02 (through the legacy commit machinery → real
error latching), FORMAT $03 (no-op success on a block store), CONTROL $04
(code 0 only), INIT $05. Errors per the ProDOS/SmartPort set: $01 bad
cmd (incl. extended $4x), $04 bad pcount, $21 bad status/control code,
$27 I/O, $28 no device, $2B write-protected, $2D bad block, $2F offline.

The entry itself is `BIT $CFFF` **then** `JMP $CE00` (2026-08-14), which is
what the real Liron firmware does and is load-bearing on a //e: with
SLOTC3ROM off (the default) any read in `$C300-$C3FF` latches the MMU's
INTC8ROM flip-flop, and `$C800-$CFFF` then answers from the *internal* ROM
instead of the slot's expansion bank (`Memory::memRead`, MAME
`apple2e.cpp:c300_int_r`). The 80-column firmware reads `$C3xx` constantly,
so a bare `JMP $CE00` regularly fetched motherboard bytes and ran them as
the SmartPort handler. `$CFFF` clears INTC8ROM and releases the expansion
owner; fetching the `JMP` at `$Cn10` re-claims the window for this slot
(`SlotBus::slotRomRead` latches the owner on any access to the slot page).
Pinned by `liron_smartport_dispatch` (runs the whole matrix through a
real 6502, synthetic AND real-ROM identity passes).

**Real Liron ROM (`roms/liron.rom`, optional).** The BMOW/Yellowstone dump
of the real controller firmware (4 KB: per-slot $Cn00 page at `slot×256`,
$C800 bank at 2048 — see the `liron-rom-dump` memory + CLAUDE.md § //c+
MIG). When present on a slot-having machine, `loadLironRom` re-bases the
slot page on the real dump — authentic identity `$Cn07=$00` (SmartPort
class), `$CnFB=$00`, `$CnFE=$BF`, `$CnFF=$0A` (the real fixed ProDOS
entry `$Cn0A` the DIX fix documented) — and overlays the HLE entries on
top ($Cn00 boot, $Cn0A→$Cn50, the $Cn0D-$Cn12 dispatch stub, $Cn20-$CnE2
driver block): the real firmware's IWM/UniDisk code cannot run without the
drive-side 65C02. The overlay stops at `$Cn12` — where the stub ends — so
the dump's own `$Cn13-$Cn1F` survive; it used to run to `$Cn1F` and paint
NOP padding over them, contradicting the "kept real" list.
**Never loaded on //c-class** (plug-site gate on `noPhysicalSlots`): the
on-board $C500 stub keeps the synthetic `$Cn07=$01` so the //c boot scan
never SmartPort-enumerates it (project_iic_smartport_boot).

Per-unit `streamOffset_` (one per unit, 2 units) wraps every 512 B;
drive-select latches `activeUnit_` and resets stream offset.

**Slot ROM** (`buildRom`, 256 B with slot baked in):
```
$Cn00     JMP $Cn20              (boot vector)
$Cn01     $20                    ProDOS signature byte
$Cn03     $00
$Cn05     $03
$Cn07     $01                    ProDOS non-removable block device
                                  (NOT $3C — that is the Disk II marker;
                                  see "Stub fixes" below)
$CnFE     $17                    features/units mask: read+WRITE+status,
                                  2 units (was $13 = read-only to a
                                  capability-inspecting utility)
$CnFF     $50                    driver entry offset
$Cn0A     JMP $Cn50              (real-HW driver entry, see below)
$Cn0D     BIT $CFFF              SmartPort entry: drop INTC8ROM first…
$Cn10     JMP $CE00              …then into the $C800-bank handler
$Cn20-..  boot (load blk 0 of drv 1 → $0800)
$Cn50-..  ProDOS driver
$CnE0-..  error halt
```

Driver examines ProDOS `$43` unit byte: bit 7 = drive (0 → drv 1, 1 →
drv 2). Write probes `$C0n4` bit 6 first; returns `$2B` (WP) without
touching memory if WP.

**`$Cn0A` real-hardware entry.** The Apple Disk 3.5 / Liron firmware exposes
its block driver at a *fixed* `$Cn0A`; software that bypasses the `$CnFF`
indirection hardcodes `JSR $Cn0A` with the same `$42-$47` ZP params. French
Touch **DIX** (`boot_unidisk.a`: `modread JSR $C50A`) does exactly this to
stream its menu/demos into Language-Card RAM. POM2 synthesises its dispatch at
`$Cn50`, so a bare `JSR $Cn0A` used to hit an unimplemented `$00` = BRK — and
because DIX has just enabled LC RAM read (`LDA $C083 ×2`), the BRK vector was
fetched from cold LC RAM → permanent storm (banner shown, then freeze). Fix: a
`JMP $Cn50` at `$Cn0A` (additive; `$CnFF` stays `$50`, so the //e/c boot and
ProDOS tests are untouched). The `$42-$47` convention is identical, so reads/
writes/status all work. Pinned by `smartport_unidisk_entry`. With this, DIX
boots past its banner, loads its menu into `$D000` LC RAM, and runs.

**Per-unit storage**: each `SmartPortUnit` owns its bytes. The
HDV-flavoured `SmartPortHdvUnit` wraps `Block512Backing` (2IMG/dirty/
WP/write-back for free). Per-unit settings persist as
`smartport_slotN_unitK_{type,path,writeback}`. Card implements
`MountableMediaCard` over its 2 units.

**Boot wiring**: a library click (or CLI insert+boot) routes 3.5"/HDV
to the primary `SmartPortCard` and `controller->bootFromSlot(card->
getSlot())` on every profile that has one — including //c-class
(built-in slot 5).

Pinned: `smartport_card_smoke_test`, `smartport_mixed_units_smoke_test`.

### //c-class on-board SmartPort (3.5" + HDV boot)

> **`project_iic_smartport_boot`** — the working name this body of work
> carries in code comments (`SmartPortCard`, `Memory`, `MainWindow`) and in
> `TODO.md`. It refers to this section; there is no tracker behind it.

Two paths, by ROM:

- **32 KB //c (rev 0/3/4) — the machine's own firmware, since 2026-09-01.**
  Its bank-0 `$C500` page is the disk controller firmware (the Liron's, byte
  for byte: `$C88C` in bank 1 ≡ the Liron's `$C806`), and it talks to the
  rear-port drive as an intelligent SmartPort device over the disk port.
  `IIcExternalSmartPort` answers that bus for the units of the built-in
  slot-5 card — see [§ The //c external 3.5" port](#the-c-external-35-port-iicexternalsmartport).
  Nothing is punched over `$C500` while a unit holds media
  (`IIcClassProfile::servesExternalSmartPort`): the firmware enumerates the
  drive, ProDOS lists `S5,D1/D2` next to the internal `S6`, and
  `bootFromSlot(5)` jumps into the real page and boots through it. Pinned
  `iic_external_smartport`.
- **16 KB //c (rev 255) and the //c+'s HDV/2mg — the host-served stub.** The
  16 KB dump has no 3.5" firmware at all; the //c+ boots its internal Sony
  through the MIG but reaches an HDV only this way. The **same
  `SmartPortCard`** as //e is built into slot 5 and its `$Cn00` page is
  punched through the forced INTCXROM. Why not faithful IWM/Sony there:
  the real //c-class masks all slot ROM → a normal card's `$Cn00` is
  invisible; and MAME models no 3.5"/SmartPort on the plain //c.

**Mechanism** (the stub path):

- **ROM hole** (`Memory::memRead`, //c-class INTCXROM branch):
  `$C500-$C5FF` (bank 0) returns `slots.slotRomRead(addr)` instead of
  internal ROM **iff** `iicSmartPortArmed_` AND
  `slots.peripheral(5)->exposesIicOnboardRom()` (unit holds media).
  Bank 1 handled earlier by `internalRomRead` → hole is bank-0 only
  (preserves //c+ alt firmware's `$C500` data).

- **"Armed" gate — critical subtlety** (`Memory::setIicSmartPortArmed`).
  The stub MUST NOT be visible during the //c ROM's own autostart:
  real //c rev0/3/4 keeps its SmartPort firmware at `$C500`, and a
  booted ProDOS calls into `$C5xx` entries the real firmware provides
  but the stub does not. Substituting the stub corrupts a
  multi-device boot (Disk II in slot 6 + media in on-board SmartPort)
  → "garbled Apple //c banner". So `bootFromSlot` **arms** (explicit
  GUI/CLI boot only) and every `coldBoot/softReset/hardReset`
  **disarms**. Net: normal reboot always sees real `$C500` firmware;
  on-board SmartPort boots only via Library / Slot Config "Boot".
  Trade-off: persisted SmartPort media doesn't auto-reboot.

- **Device-select** (`$C0D0-$C0DF` = slot 5) never masked — block
  stub's `$C0D0-$C0D4` protocol already reaches the bus.

- **Stub fixes** (`SmartPortCard::buildRom`): `$Cn07` = `$01` (ProDOS
  non-removable block device), NOT `$3C` — `$3C` is the Disk II
  marker and made //c treat slot 5 as a second Disk II. ProDOS STATUS
  call (cmd `$00`, `$CnC0` routine) returns block count in X/Y via
  `$C0n5/$C0n6` so ProDOS ONLINE / BITSY size it correctly.

- **Routing** (`MainWindow`): `routeMount35` uses SmartPort on all
  profiles; `routeMountHdv` + `ensureHdvCardForBoot` send //c-class
  HDV to slot-5 SmartPort (`SmartPortHdvUnit`), **never** cffa/hdv
  slot card (masked, unbootable). Profile //c gains a `smartport35`
  built-in at slot 5; //c+ already had one.

Pinned: `iic_onboard_smartport_test` (armed ROM-hole gating + block
I/O via `Memory`); `iic_dual_boot_trace` (headless diagnostic for the
garble). See `project_iic_smartport_boot`.

### 3.5" mechanical sounds

`Sony35Drive` carries `FloppySoundSink* sound_` set by
`EmulationController` to same `FloppySoundDevice` Disk II uses —
shares samples + volume/mute persistence.

**Cycle stamping**: `seekPhaseW(phases, emuCycles)` takes
CPU-cycle counter at strobe edge. `SmartPortHub::onIwmPhases`
forwards `IWMDevice::emuCycles()`. The LSTRB rising edge fires
`strobeWriteRegister(regSelect())`; the register cases (Sony GCR
map, see `Sony35Drive.cpp` header) are:
```
0x0  DirNext    directionIn_ = false (step toward cyl+1)
0x1  StepOn     moved && sound_->step(track_, lastStrobeCycle_)
0x2  MotorOn    if (!motorOn_) sound_->motor(true,  hasDisk)
0x3  EjectOff   no-op (MAME)
0x4  DirPrev    directionIn_ = true (step toward track 0)
0x6  MotorOff   if ( motorOn_) sound_->motor(false, hasDisk)
0x7  EjectOn    image->eject() ; sound_->click()
```
`moved` gates so head bumps at track 0 or 79 don't click. Motor
transitions edge-only.

`EmulationController::mount35/eject35` call
`drive->emitInsertClick()` after `notifyMediaChange()`.

### ProDOS host folder

`prodos_folder/`. `ProDOSVolume` synthesises a ProDOS volume (guest-writable in RAM; persisting back to the folder is the write-back opt-in).
Blocks 0-1 boot (zeroed), 2-5 vol-dir key + 3 ext (51 entries max),
block 6 bitmap (4096 blocks = 2 MB cap), 7+ data + sapling indexes.

Scope: flat dir; ≤ 51 files; ≤ 128 KB per file (seedling + sapling,
tree skipped); type from extension; filenames sanitised to
`A-Z/0-9/.` with collision suffixes `.1/.2`.

Wiring: HDV slot 5 panel's Library shows `[host folder] prodos_folder/`
entry. Click → `buildVolumeFromFolder` →
`ProDOSHardDiskCard::loadImageFromBytes`. **No auto-boot** — user
boots ProDOS elsewhere, then `/HOST/` appears as slot 5 drive
(`CAT,S5,D1`). Guest writes land in RAM; with write-back ON they decode back into the folder on eject/quit — host files edited AFTER the mount are preserved (mount-time stamp carried in `PendingWriteBack`), never silently reverted to the snapshot. Pinned:
`prodos_volume_smoke_test`.

**The volume has free blocks, and the mount stamp is refreshed** *(2026-09-07)*.
Two defects that together made a host folder read-only in practice while
presenting itself as writable:

* The bitmap marked **every** block within `total_blocks` as used, so ProDOS
  reported zero free blocks and the guest got DISK FULL for a two-block file
  on an otherwise empty volume. The build now adds bounded slack
  (`ProDOSVolume.cpp:695-721`): 10 % of the content, at least 64 blocks
  (32 KB) and at most 4096 (2 MB), given back first if it would push the
  volume past `kMaxVolumeBlocks`. It is usable rather than decorative because
  `decodeVolumeToFolder` walks the directory graph and writes back every
  seedling/sapling entry it finds, whoever created it, and the volume
  directory is always 4 blocks / 51 slots regardless of how many are filled.
  It is bounded rather than generous because the image is a RAM allocation
  carried in the snapshot payload.
* `mountTime_` was set once at load. The *flush* path leaves the medium
  mounted, so after the first flush the files it had just rewritten were
  "host-newer" and the guest's SECOND save was preserved away as if the user
  had edited behind POM2's back. `Block512Backing.cpp:329-338` re-stamps the
  volume with the commit's own timestamp on a successful synth flush.

Also: symlinks pointing out of the served folder are refused, and Windows
device names (`CON`, `AUX`, …) are rejected by `isHostSafeProDOSName`.

**Two ProDOS entries can want one host name** (2026-08-17, bug hunt 8
round 3). `decodeVolumeToFolder` strips trailing dots before composing a
host filename — legal in ProDOS, awkward-to-illegal on the host — so
`README` and `README.` both came out as `README` and the second write
silently REPLACED the first, with both halves reporting success. The
build path manufactures that pair without trying: `sanitiseProDOSName`
maps everything outside `A-Z 0-9 .` to `.`, so a host folder holding
`README` and `README!` becomes `README` + `README.` in the volume, and
`uniqueName` correctly sees two distinct ProDOS names. The guest can
create both directly too, so the decode is where the guard belongs.
Host names are now reserved per decoded directory (`reserveHostName`),
a clash taking a numeric suffix rather than overwriting, and the
reservation covers THIS pass only — never what is already on disk —
so a repeated write-back stays idempotent instead of accreting a fresh
`.1` on every eject. Subdirectory names go through the same gate.

### Snapshot

`SnapshotIO`. `POM2SNAP` magic, named 8-byte sections, format shared
with POM1. Captures CPU + RAM + soft-switch display state. **Disk II
deliberately excluded** — would need mounted-image identity + head
position + dirty bits per track.

**The header carries a machine identity** (2026-09-06). The 32-bit word
after `version` was written as a reserved 0 and read back with
`(void)readU32()`, so nothing in a snapshot said WHICH Apple it came
from — while CPU/MEM/MEX all restore unconditionally and
`Memory::loadSnapshotState` never checks `iieMode`. Save on //e
Enhanced PAL, switch to //c, load: PC and 64 KB of RAM land against a
different ROM and memory map, freezing or silently running the wrong
code with no diagnostic. Live rewind was already defended (`applyProfile`
clears the ring), but `--snapshot-load` and the AI server's `/snapshot`
endpoints were not — and a snapshot file is the one artefact users hand
to each other.

The word now holds `pom2::snapshotMachineId(profile)`, an FNV-1a hash of
the profile's canonical PERSISTENCE KEY (stable by contract — state.cfg
and `--preset` both carry it — where the enum is appended to and its
order is deliberately not the display order). `EmulationController::
machineId()` carries the live value, set by `applyProfile` next to
`setVideoStandard`; the CLI and AI-server load paths compare it before
touching any state and refuse a mismatch with a message that NAMES both
machines. **0 stays legal on the wire** and means "not recorded": every
snapshot written before this build still loads, and rewind frames record
no identity on purpose (the ring is cleared on a profile switch, so the
check would be dead weight on the hot capture path). This generalises the
targeted mitigation already in `MachineSnapshot.cpp`, where the CPU-mode
byte is read and deliberately discarded because an NMOS blob forcing a
//c's 65C02 ROM onto an NMOS core hits a KIL. Pinned by `snapshot_io`
(identity round-trip, legacy 0, tampered word) and `system_profile`
(ids unique, non-zero, reversible).

Two backends share one wire format: the original file backend
(`SnapshotWriter(path)` / `SnapshotReader(path)`) and an in-memory
backend (`SnapshotWriter(std::vector<uint8_t>&)` /
`SnapshotReader(const uint8_t*, size_t)`). The memory backend bumps
the bytes through an internal `std::stringstream` bound to a
`std::ostream&`/`std::istream&` member, so all the section/length
logic is reused verbatim; the writer flushes into the caller's vector
on destruction. `snapshot_memory_roundtrip` pins byte-parity between
the two backends.

`MachineSnapshot.{h,cpp}` is the single source of truth for *what a
state snapshot contains*: `captureMachineState(w, cpu, mem)` writes
the `CPU`/`MEM`/`MEX` sections, `restoreMachineState(r, cpu, mem)`
applies them. Both the AI-control `/snapshot/save|load` handlers and
the rewind ring buffer call it, so the two can never drift. The
restore keeps the security hardening that used to live inline in
`AiControlServer`: the 16-byte CPU-section length gate (crafted-blob
over-read) and the 16 MiB MEX cap (→ `RestoreResult{false,…}` so the
HTTP path still returns 400).

### Rewind / time-travel

`RewindBuffer.{h,cpp}` (storage) + `Rewind_ImGui.{h,cpp}` (UI) +
`EmulationController` transport — the MicroM8-style rewind: continuous
state recording with scrub / step-back / hold-to-rewind-live. The ring
is a `std::deque<Frame>` indexed by `emuCycles`. Pinned by
`rewind_roundtrip`, `rewind_delta`, `rewind_transport`,
`rewind_slot_state`.

**Storage — keyframes + XOR deltas** (`rewind_delta`): a full
`MachineSnapshot` blob is ~175 KB on stock IIe, so storing one per
frame is wasteful. Instead every `keyframeInterval_` (default 120 ≈
2 s) frame is a full *keyframe* and the rest are XOR *deltas* vs the
previous frame — only the changed byte spans, coalesced across gaps
< 16 B. A 30 s ring drops from ~315 MB to ~10 MB. `reconstruct(i)`
copies the nearest keyframe ≤ i and XORs the intervening deltas
forward. XOR is its own inverse, so the same delta serves either
scrub direction. A blob size change (RamWorks bank count) forces a
keyframe — deltas need equal-length neighbours.

**Eviction — rebase-on-evict**: the front is always a keyframe, so the
chain never dangles. Dropping it first promotes the next delta to a
keyframe (`applyXorDelta(front, next)`). Two caps bind, whichever
first: `maxFrames_` (default 1800) and `maxBytes_` (default 256 MiB) —
the byte budget is what keeps RamWorks (~10 MB/frame keyframes)
bounded; it just buys fewer frames of history. One frame is always
kept.

**Capture point**: `EmulationController::workerLoop()` (threaded) and
`tickFrame()` (WASM single-thread), both at the quiescent frame
boundary *after* the CPU budget is spent and the IWM is ticked.
`rewind_.enabled()` is checked before taking `stateMtx`, so a disabled
ring is zero-overhead.

**Transport / threading**: `enabled()` is atomic; every other
`RewindBuffer` method touches `frames_` and needs exclusive cpu+mem
access. The UI restores while the worker is *parked*: the controller's
`rewindBeginScrub()` sets `Mode::Stopped` then `waitUntilParked()`
spins (bounded) on `workerParked_` — set in the worker's Stopped CV
wait — so a restore can't be overrun by the in-flight Running frame
(the Running branch finishes its whole budget before re-checking
mode). `rewindSeek` / `rewindSeekToCycle` restore under the lock;
`rewindEndAndResume(i)` restores i, `truncateAfter(i)` to drop the
abandoned future, then resumes. Every restore calls
`flushAudioForRewind()` (speaker reset) so a time-jump is silent
instead of popping. `rewind_transport` pins all of this against a real
worker thread *and* the `tickFrame` path. The ring is cleared on
`coldBoot` (RAM wipe ⇒ a different machine).

**The ring's stamps are strictly increasing, and that is enforced, not
assumed** (2026-08-23). `indexForCycle` walks the deque and breaks at the
first frame past its target, so one out-of-order frame silently breaks every
seek beyond it and turns `newest - oldest` (the panel's "span" readout) into
a meaningless number. The machine *does* jump back: `rewindEndAndResume` is
only one of the ways it resumes — the toolbar Play button, Machine ▸ Run, the
`machine.run` palette command and the kiosk menu all call
`setMode(Mode::Running)` directly, leaving the abandoned future in the deque
for the next capture to append *behind*. Two guards, at the two layers that
own the two halves:

* `RewindBuffer::capture` drops every frame stamped at-or-after the incoming
  one before appending (`dropAbandonedFuture`, one compare on the hot path;
  a jump back past the oldest retained frame clears the ring so the restart
  is a keyframe, never a delta against a dead timeline). It sits in the one
  funnel every capture goes through, so the invariant also covers a snapshot
  load that forgot its `rewind().clear()` — and callers that don't exist yet.
* `EmulationController` owns the scrub itself (`scrubIndex_`, exposed as
  `rewindScrubbing()`), and `setMode(m != Stopped)` ends it. The truncation is
  deliberately *not* done there: `setMode` is reached from callers that
  already hold `stateMutex` (the Disk II Library's boot buttons), and
  `stateMutex` is non-recursive. Deferring the drop to the worker's next
  capture keeps that path lock-free.

Pinned by `rewind_roundtrip` case 5 (unit: a jump back drops the future,
survivors still reconstruct, a full abandon restarts on a keyframe) and
`rewind_transport` case 6 (a bare `setMode(Running)` ends the scrub and the
ring shrinks then stays monotonic). Both verified falsifiable.

**UI** (`Rewind_ImGui`): Devices ▸ "Rewind". Record toggle, a timeline
slider, |< / << hold / <| / |> / resume transport, history-length
slider, and `F6` = hold-to-rewind-live from anywhere (polled in
`MainWindow::render`, survives ImGui capture, no-op when recording is
off). The cursor is the panel's own state; the scrub flag is a *view* of
`EmulationController::rewindScrubbing()`, resynced (`syncScrub`) on entry to
`render` / `beginScrubIfNeeded` / `releaseHold` — a panel that kept its own
answer went on believing it was scrubbing after a toolbar Play, so its next
drag seeked a running machine (the slider visibly did nothing) and a released
`F6` hold resumed from a stale cursor. The ring and machine live in the
controller.

**Slot/disk state** (`rewind_slot_state`): `SlotPeripheral` gained
`append/loadSnapshotState`; `DiskIICard` serializes its mechanical +
LSS runtime state (head quarter-track, motor, phase magnets, data
register, sequencer, rotational timing — NOT the media or PROMs), so a
rewind during disk I/O doesn't leave an in-progress read on the wrong
nibble. `MachineSnapshot` writes these as per-slot `SLOTn` sections **only when
`captureMachineState(includeSlots=true)`** — the rewind path opts in;
the AI-control `/snapshot` file path keeps its documented "disk/slot
excluded" contract (an archival file can outlive a media swap). Restore
routes each section to the card in that slot (a card ignores a
foreign/old blob via its own magic+version) and always tolerates their
absence. On load `DiskIICard` clamps `activeDrive` and re-points the IWM
at the restored head.

**Sound chips** (`rewind_audio_state`): `MockingboardCard` and
`PhasorCard` serialize their `Via6522` + `Ay3_8910` (+ `Ssi263` on the
Sound II variant) register/timer state through the same `SlotPeripheral`
hook — `Via6522::append/loadSnapshot` (25 B; v1 blobs are 24 B,
`portAIn` absent), `Ay3_8910` (34 B),
`Ssi263` (30 B: 5 registers + phoneme playback cursor) — shared across
cards, with LE packing in `ByteIO.h`. So music *and* speech survive a
rewind, not just the speaker flush. The AY/SSI here are register/cursor
models (synthesis derives from them), so restoring the state restores
the sound exactly.

**Disk writes** (`rewind_disk_write`): DiskIICard's snapshot is v2 —
it also carries the writable nibble track buffers
(`DiskImage::append/loadMediaSnapshot`, gated on a loaded,
physically-writable, non-WOZ disk) so a disk WRITE is undone on a
rewind. The rewind delta codec keeps this near-zero until a track is
actually written; the read caches re-derive from the restored nibbles
(`invalidateAllBitStreams`). Read-only / WOZ / empty drives cost one
flag byte.

**Media a rewind cannot undo, and the policy that replaced capturing it**
*(2026-09-07)*. The ring never captures a block device (up to 32 MiB), a 3.5"
image (800 KB) or a writable WOZ (whose authoritative bits live in `wozRaw`, a
different store from the nibble buffers the v2 Disk II media snapshot covers).
Rolling RAM back over a ProDOS SAVE while the volume stayed written is a real
corruption path: the restored directory and bitmap disagree with the blocks on
the disk, and the next allocation cross-links them. Capturing megabytes per
frame was never the answer; the chosen policy is the safe minimum — **a rewind
may never CROSS such a write**. Every one of those paths bumps
`pom2::mediaWriteEpoch()` at the storage leaf (`Block512Backing.h:60-68`,
called from `Block512Backing.cpp:552/571`, `Disk35Image.cpp:265`,
`DiskImage.cpp:1876` and `PrinterCoordinator.cpp:171` — printed output is
irreversible too), and `EmulationController::noteMediaWrite`
(`EmulationController.cpp:997`) compares it at the ring's capture point and
clears the history when it moved, so the timeline restarts *after* the write.
One relaxed atomic load per captured frame, and it covers write paths that do
not exist yet, because the bump is at the leaf. **Non-WOZ Disk II nibble writes
deliberately do not bump**: those *are* captured, and a rewind is expected to
undo them. Every coordinator mount/eject clears the ring for the same reason —
a host-side media swap makes the recorded timeline a different machine.

**What else the 2026-09-07 pass added to the snapshot.** Sixteen fields were
restoring CPU + RAM against devices left on the abandoned timeline:

* **Disk II v4** (`kDiskIISnapVersion = 4`, `DiskIICard.cpp:746`) carries the
  in-flight **write burst** — `writeBuffer`/`writePosition`/`writeStartTime`/
  `writeLineActive`, variable-length, header + `count` 8-byte flux stamps
  (`DiskIICard.cpp:812-870, 955-968`). `lssCycle` was already saved; without
  the burst a rewind during a DOS SAVE punched a hole in the sector, and with
  write-back on that hole reached the `.dsk`. Pre-v4 blobs restore with an idle
  controller.
* **TransWarp v2** (`TranswarpCard.cpp:36`) serialises `displaced_`, the 4 KB
  of `$F000` ROM the card holds while it shadows — the only copy of Applesoft +
  Monitor at that moment. The loader used to re-derive `shadowing_` and a later
  `$C072` wrote stale or zero bytes over the ROM.
* **`Sony35Drive`** and **`Disk35Image`** gained hooks (the IWM already restored
  its FSM, so a //c+ / Liron 3.5" transfer came back mid-cell → I/O ERROR), and
  **`NoSlotClock`** gained one at all: its bit-serial key matcher and readout
  cursor are walked over many accesses, so half a match is a real state.
* **Memory's IOU trailer** grew two bytes at the end, under the same
  grow-at-the-end rule (`Memory.cpp:934-946`): `vblWasActive`, the edge
  detector behind the //c VBL IRQ (it defaults to true, so a restore taken
  inside the blanking interval re-armed the edge and fired one spurious IRQ —
  the frame sync a //c PAL French Touch demo races against), and
  `iicCardWindow_`, the partner latch of the already-saved
  `iicSmartPortArmed_`.
* **SmartPort identity**: `SmartPortCard` restored a primed 512-byte write
  block with no media identity, so swapping a bay and rewinding committed the
  old block to the new disk.
* Four cards now **identify the blob before resetting themselves** (they used
  to `reset()` first, so a foreign blob wiped them mid-transaction, which
  contradicts the contract in `MachineSnapshot.cpp`), and five restores clamp
  values that stalled or spun a device (TransWarp `slowCycles_`,
  WorkstationCard `timerAcc_`/`sccAcc_`, SSC `statusErrors_`/`irqState_`,
  M68705 `reg.S`, the W5100's CLOSED-demoted sockets still advertising
  RSR/FSR). The W5100 also stopped restoring `virtualDns_`, which is a user
  setting, not machine state.
* **Every** snapshot-load path — the AI server's `/snapshot/load`, the CLI's
  `--snapshot-load` and the rewind scrub — now calls `flushAudioForRewind()`
  and re-bases the cassette. Only the rewind path used to.

**Declined, and documented in place**: `Ay3_8910::busOut` (consumed within one
`applyControl`; the VIA's `portAIn`, which *is* saved, already carries what
survives the call) and the keyboard latch / paste FIFO (host input in flight,
and it lives in the wrong lock domain — `Memory::kbMutex`, not `stateMutex`).

**Known gap**: writable-WOZ writes still aren't *undone* — they are only made
un-crossable by the epoch policy above. WOZ originals are typically
write-protected anyway. A clean follow-up if a writable-WOZ workflow needs it.

### 3D voxel view

`Voxel3DRenderer.{h,cpp}` + `Mat4.h` — MicroM8's **"Voxel Cube"** view:
the screen rebuilt as an **upright 4:3 slab** of cubes, orbited by a
camera. Toggle: **View ▸ "3D voxel view"** (persisted `show_3d_voxel`).
Pinned by `voxel3d_math`.

**The model (MicroM8-faithful, fixed 2026-05-31)**: each pixel → one
cube of the **same** base thickness extruded toward the viewer on +Z
("Voxel Depth"). Height is **NOT** luminance — that earlier height-field
gave a spiky horror (bright pixels speared into stalactites) and laid
the screen flat on the floor at a catastrophic angle. `colorShift`
(MicroM8's per-colour "Z-axis 3D offset", luminance-weighted, **on by
default**) pops brighter pixels forward for pin-art relief. Column→world
X, row→world Y (row 0 = top), plane a true 4:3 (width 2.0 × height 1.5)
so voxels keep the Apple II pixel shape.

**Two gotchas** worth pinning here:
- **Resolution = native.** `MainWindow` sets `gridW/gridH` from the live
  `display->width()/height()` (280 or 560 × 192) → one voxel per Apple II
  pixel; the old 140×96 visibly threw away half the image. `voxelDepth`
  / `colorShift` are therefore in **cell-height units** (not world), so
  the look is constant whether the source is 280- or 560-wide.
- **Present flip.** `colorTex_` is shown by `ImGui::Image` with v=0 at the
  top, but GL renders y-up → a vertical mirror, same as the 2D NTSC passes.
  The vertex shader pre-flips `gl_Position.y` so the screen reads upright
  (forgetting this inverted top/bottom).
- **Moiré / anti-alias.** Two sources: a `cubeFill < 1` gap leaves a regular
  dark grid that beats against the pixels, and 50k hard cube edges alias with
  no AA. Fix = **contiguous cubes** (`cubeFill = 1.0`, so flat colour fields
  are one continuous slab) **+ supersampling** (`superSample`, default 3):
  render the FBO `ss`× the on-screen size, build a mip chain, and let ImGui's
  `LINEAR_MIPMAP_LINEAR` minify box-average it down. No MSAA resolve needed.
- **WASM perf guard.** That supersampled FBO + 100k instanced cubes is brutal
  on browser/mobile GPUs, so under `__EMSCRIPTEN__` `process()` caps `ss ≤ 2`
  and the FBO ≤ 2048² (it drops the factor until it fits), and `MainWindow`
  caps `gridW ≤ 280` (halves 560-wide DHGR geometry). Native: `ss ≤ 4`, 8192².
- **Mono + per-colour depth** (MicroM8 fidelity). `mono` greys the output
  ("Voxel Cube Mono") while keeping relief. `perColorDepth` swaps the smooth
  luminance depth for a **palette snap**: each pixel picks the nearest of the
  16 lo-res colours (`kVoxelPalette`, a verbatim copy of the private
  `Apple2Display::kLoResPalette`) and takes that colour's brightness → discrete,
  blocky per-colour relief, closer to MicroM8's per-index Z table. The nearest
  search is a 16-iter loop in the vertex shader (per instance, not per pixel).

**Camera.** Left-drag orbits, **middle-drag strafes** (`OrbitCamera::pan`
slides the target across the camera's right/up plane, scaled to world-units-
per-pixel so the grab tracks 1:1), wheel zooms. Defaults frame the slab
nearly head-on (azimuth 0.32 / elevation 0.20 / distance 2.8 / fovY ~40°).

**It's a view-geometry layer, NOT a `HiResMode`.** It consumes the
decoded **colour** framebuffer (any HiResMode / NTSC demod) and
re-presents it as geometry. **It deliberately taps the pipeline *before*
the CrtEffectStack** — `MainWindow` keeps a separate `voxelSrcTex` handle
(= `screenTexture`, or the OE demod output) so the cubes never inherit
scanlines / shadow-mask / barrel warp; CRT glass on a flat screen and CRT
glass smeared over 50k cubes look nothing alike. So the 3D view composes
with every colour mode but is **independent of the CRT effects**. Wired in
`MainWindow::drawScreenImage` just before the final `ImGui::Image`: when
on, `voxel3d_->process(voxelSrcTex, …, viewProj)` replaces the flat blit
(the CRT pass still runs for the flat fallback, then is discarded).

**Renderer** (follows the `NtscPostProcessor` pattern — lazy entry-
point loader, FBO + GL-state save/restore — plus a **depth**
attachment the 2D passes lack): a unit cube drawn `gridW*gridH` times
via `glDrawElementsInstanced`. The vertex shader derives each instance's
cell from `gl_InstanceID`, **samples the framebuffer in the vertex
stage** (vertex texture fetch) for colour, and places the equal-depth
cube; the fragment shader shades per-face via **screen-space
derivatives** (`cross(dFdx,dFdy)`) so no normal attribute is needed
(stays on the single location-0 `aPos` the shared shader helper binds).
WebGL2/GLES3-safe: instancing + VTF + derivatives are core there, no
geometry shader.

**Camera** (`Mat4.h`, header-only, no glm): column-major Mat4
(perspective / lookAt / multiply) + `OrbitCamera` (azimuth / elevation
/ distance → view-projection). Pure CPU, so it's unit-tested
(`voxel3d_math`) — the matrix layout is the classic source of a
black/garbled 3D view, caught off-GPU. The GL rendering itself is
verified by running the app (no golden hash).

Camera interaction is wired in `drawScreenImage` right after the
`ImGui::Image` (the drag/wheel reference that item), mutating `voxelCam_`;
orbit elevation is clamped to ±1.5 rad off the lookAt poles.

**Phase 3 panel** (`renderVoxelSettingsWindow`, View ▸ "3D voxel
settings…") — live controls for `voxelDepth` / `colorShift` / `cubeFill` /
`superSample` / `ambient` + `mono` / `perColorDepth` checkboxes, plus Reset
view / Reset settings. The renderer is owned up-front at settings-load (its
ctor is GL-free) so the panel and the `voxel_*` persistence keys bind straight
to `voxel3d_`, even before the view is first enabled; grid resolution stays
auto (display-driven, not a knob). Next (P5, **deferred**): a rewind tie-in
"freeze + orbit a rewound frame" — note this already works for free (the view
samples the live framebuffer, which the rewind restore updates), so it's
documentation + polish rather than new plumbing.

## IWM (//c+ on-board)

`IWMDevice.{h,cpp}` — verbatim MAME `machine/iwm.{h,cpp}`. Full state
machine (`MODE_IDLE/ACTIVE/DELAY` for `m_active`; `MODE_READ/WRITE`
for `m_rw`; `S_IDLE/SR_WINDOW_EDGE_0/SR_WINDOW_EDGE_1` for read bit
walker; `SW_WINDOW_LOAD/MIDDLE/END/UNDERRUN` for write). Drives flux
via `DiskImage::getNextTransition` (5.25") or
`Sony35Drive::nextTransition` (3.5").

**Live wiring**:

1. `EmulationController` constructs the IWM, hands it to
   `Memory::setIWM`. Reset paths (`hardReset`, `coldBoot`,
   `bootFromSlot`) call `iwm.reset()`.
2. Memory routes `$C0E0-$C0EF` on //c+ (`IIcClassProfile::isPlus_`)
   through IWM (MAME
   `apple2e.cpp:2798-2801` gating on `m_isiicplus && slot == 6`).
   Plain //c uses `A2BUS_DISKIING` at sl6. On //c+ slot-6 DiskIICard
   still observes the access (motor sound / turbo / head tracking).
   **Selective authority (2026-07-29)**: even with
   `iwmAuthoritative=true` (default) the IWM's byte is returned ONLY
   while the SmartPortHub routes to a 3.5" Sony
   (`hub->active35Selected()`); 5.25" data always comes from the
   DiskIICard LSS. The IWM's bit-cell walker mis-framed DOS 3.3 RWTS
   write-verify (//c+ `SAVE` → I/O ERROR), and its `flushWrite` no
   longer pushes 5.25" flux at all — both state machines were writing
   the same `DiskImage`. Ownership rule: one controller per drive
   class per direction. Three MAME-parity sense fixes ride along:
   status SENSE reads HIGH with no selected drive (`iwm.cpp:129` —
   with an always-attached `disk_`, a writable image made the //c+
   firmware's boot drive-scan spin at `$F0FC` forever, blank screen),
   Sony DSKCHG latch polarity (`floppy.cpp:560/672/723`, mac wpt_r
   `!m_dskchg` — empty drive must sense "changed/empty" HIGH), and
   DIR init 0 (`floppy.cpp:290`). Diagnostics: `POM2_TRACE_IWM_SENSE=1`
   logs `[SENSE]/[STROBE]/[IWMST]/[IWMMODE]` transitions;
   `build/tests/iicplus_boot_probe` boots the full //c+ stack headless
   (`POM2_PROBE_SHADOW=1`, `POM2_PROBE_KEYS='SAVE T~'`). Pinned:
   `iic_plus_boot_write`.
3. DiskIICard pushes `setFloppy(image, qt)` to IWM from `insertDisk`/
   `ejectDisk`/`selectDrive`/`seekPhaseW`. IWM's `nextTransition`
   queries `DiskImage::getNextTransition(qt, from*2) / 2` (flux events
   in LSS-cycle space; IWM in CPU-cycle space).
4. `EmulationController::tickFrame()` (and the threaded worker loop)
   calls `IWMDevice::tick(mem.getCycleCounter())` once per video frame
   so the 1-emulated-second drive-disable timer drains when //c+ alt
   firmware stops poking `$C0Ex`.

`iwmAuthoritative` toggle (`Memory::setIWMAuthoritative` or
`POM2_IWM_AUTHORITATIVE=0`) drops data path back to DiskIICard's LSS
for A/B compare. IWM state advances either way. Pinned:
`iicplus_boot_trace`.

**Window-size scaling**: MAME's `iwm.cpp:290-301 half_window_size` /
`:302-313 window_size` are IWM-clock ticks (//c+ runs IWM off
A2BUS_7M ≈ 7.16 MHz). POM2 ticks IWM at `POM2_CPU_CLOCK_HZ` (~1.02
MHz) for a single cycle counter — constants divided by ~7 to keep
"bit cell" ≈ 4 µs.

**MAME parity audit fixes** (2026-05-16): `data_w` handshake gate
(MAME `:311-318` — clear WHD bit 7 only when mode bit 0 set);
`mon_w` propagation (`:194-195/234/91` — drop old / raise new on
motor); `devsel_cb` extra moments (`:79/236/92` — `device_reset`,
MODE_DELAY entry in non-timer mode, `update_timer_tick` exit);
`set_write_splice` call site wired (`:218-221`, body still stub —
`DiskImage::setWriteSplice` TODO); `read_register_update_delay`
(`:363-366`, returned 1/1 instead of 1/2). Pinned:
`iwm_device_smoke_test`.

**Not yet ported**: Q3 fast clock (1.86 MHz, Mac/IIgs only); full
`DiskImage::setWriteSplice` body (WOZ re-master parity).

## SmartPort 3.5" stack

`Disk35Image` + `Sony35Drive` + `SmartPortHub` — full Sony GCR
read+write for //c+.

*Image+drive*. `Disk35Image` loads 800 K `.po`/`.2mg` **and `.woz`**.
`Sony35Drive`
responds to IWM phase-as-command bus (MAME
`mac_floppy.cpp::seek_phase_w` + Apple //gs HW ref) and to
MIG-driven `m_35sel/m_intdrive/m_hdsel` (MAME `apple2e.cpp:638-679
recalc_active_device`). `senseR()` returns active-low register file
(`/INSERTED`, `/TRACK0`, `/READY`, `/MOTOR ON`, `/SWITCHED`, …).

*The read path through the IWM works, and here is the test that says so*
(2026-09-01). `tests/sony35_iwm_read_path_test.cpp` drives the whole chain —
`Disk35Image` blocks → the zoned GCR encoder → flux → `IWMDevice`'s bit-cell
window walker → `$C0EC` polls → `sony35::decodeSectors` → blocks — with no
firmware anywhere in it, over all five speed zones on both heads. Set
`POM2_DUMP_GCR=1` for the per-mode and per-track tables;
`POM2_GCR_CONTROL_ONLY=1` runs only the encoder→decoder control, which is what
to reach for if it ever fails: it says in one run whether the format or the
controller broke.

It was built as a diagnostic and it found two faults, in this order, the
second only visible once the first was fixed:

1. **The IWM was clocked in whole CPU cycles.** It is wired to A2BUS_7M on a
   //c — 7.159 MHz, exactly 7× the 6502 — and POM2 had divided MAME's window
   constants by 7 (28/14 → 4/2, 36/18 → 5/2), which made two of the four
   settings identical and left no way to place a window edge inside a
   2.02-cycle Sony cell. The state machine and `Sony35Drive`'s flux timeline
   now count in `POM2_IWM_TICKS_PER_CPU_CYCLE` units (CpuClock.h) and MAME's
   constants are verbatim. 4 → 17 address fields of 24, still zero sectors.
2. **A flux query one tick late.** `nextTransition(lastSync_ + 1)` where MAME
   asks from `lastSync_`, so a transition landing exactly one tick past the
   last sync point was dropped — and `nextFluxChange` is a local reset on
   every `sync()` entry, so it was dropped for good. `lastSync_` parks on the
   caller's poll boundary whenever a `sync()` runs out of time mid-window, so
   this fired about once every 35 bytes: fine for an 8-byte address field,
   fatal for a 700-byte data field.

Only IWM mode $0A reads a Sony disk, and that is correct rather than a
limitation: it selects the 14-tick window and the cell is 14.17 ticks. $02 and
$12 (28 and 36 ticks) are the 4 µs 5.25" settings.

Consequences: snapshot blobs are **`IWM2`** (the FSM stamps are ticks — a
different number for the same instant; the loader still takes `IWM1` and
scales), and `Sony35Drive::writeFlux`/`nextTransition` take ticks, because
read and write must share one timeline or a write lands on a different cell
than the read that verifies it.

**And the //c+ firmware drives all of it** — pinned separately by
`iicplus_boot35`, which cold-boots the real ROM with an 800K image in the
internal bay and asserts ProDOS reaches the text page, the motor ran and the
head left track 0. On the pre-fix controller the same harness gets the
firmware's own `UNABLE TO FIND A BOOTABLE DISK ONLINE.` with the head on
track 0. One trap in writing such a harness: plug a `DiskIICard` into slot 6
even with no 5.25" media. `IIcClassProfile::ioReadIWM` falls through to that
card whenever a 3.5" drive is not selected, and an empty slot answers with the
floating bus — $FF, whose bit 5 reads as "drive enabled", sending the firmware
down a branch the real machine never takes.

Three traps the harness hit, all of which produce a false conclusion about the
hardware if you miss them: stepping the head while side 1 is selected silently
addresses registers 8-F (`regSelect()` is `{ HDSEL, CA2, CA1, CA0 }`, so
"step" becomes "MFM mode on" and the head never moves); the mode register is
written by an odd-offset write with **both** Q6 and Q7 set, so a sweep that
gets that wrong measures mode $00 every time; and the IWM's `sync()` walker
only moves forward, so handing it a timestamp older than one it already
reached returns an empty stream for the rest of the run.

*`LironCard` — the same hardware as `SmartPortCard`, at the other level*
(2026-09-01, catalog `liron`). `SmartPortCard` answers ProDOS's block calls
from the host and borrows only the Liron dump's identity bytes; it works and
is the quick choice. `LironCard` is the card as silicon: the real 4 KB EPROM
executing, an `IWMDevice` behind `$C0nX`, and — this is the part that took a
day to find — an intelligent drive on the other end of the port, because that
is what the firmware talks to. Wiring differs from the //c+ in exactly one
place — a Liron has no MIG, so the IWM's SEL line is head select (and bit 3
of the Sony register address), where `SmartPortHub` uses the MIG's
`$C240/$C260`.

It boots, over the SmartPort **bus**. The scan at `$C800` drives PH1 + LSTRB
high, asserts SEL and the motor, polls the status register 50× for SENSE, and
reports ProDOS `$28` on timeout; that handshake is answered only by an
intelligent device (UniDisk 3.5, own 65C02), and the scan has no dumb-drive
fallback. `SmartPortBusDevice` is that device, at the byte level — see the
next section. Pinned by `liron_boot35` (ProDOS 8 on the text page, blocks
read over the bus) and `smartport_bus_handshake` (the exchange itself). The
Sony mechanisms under the IWM are bypassed while the responder is live
(`busLive()`: enabled and a bay holds media); with it off, or nothing
mounted, the card is a Liron with an empty port and the firmware says so.

Two card-side bugs the real firmware exposed, both worth knowing when wiring
any IWM card: the phase lines must reach EVERY drive on the chain, not only
the selected one (the firmware sets the register address up before enabling a
drive); and `devsel` must not pick the drive on a Liron, because SEL is head
select there.

### The SmartPort bus (SmartPortBusDevice)

The exchange between a Liron-class controller and a UniDisk 3.5 is a byte
stream through the IWM's data register, so POM2 answers it at the byte level
rather than emulating the drive's processor — the same seam `SmartPortCard`
uses one layer up (docs/lle_vs_hle.md). Everything below was read out of
`roms/liron.rom` with POM2's own `disassemble6502`; the //c's bank 1 is the
same code, so the addresses serve both machines.

| Address | Step |
| ------- | ---- |
| `$C800` | send: PH1 + LSTRB up, mode `$07`, SEL, motor; poll status 50× for SENSE HIGH (timeout → `$28`); REQ (PH0) up; sync `3F CF F3 FC FF C3`; seven header bytes; odd section; groups; two checksum bytes; `$C8`. Then wait for SENSE LOW within ten polls (`$C943`, else `$01`) and drop REQ (`$C949`). |
| `$C960` | receive: PH1 + LSTRB up; wait SENSE HIGH; REQ up; hunt `$C3` in thirty reads; header into `$0051`..`$004B` (dest, src, type, aux, **status**, odd count, group count — each `AND #$7F`); odd bytes; groups via the slot page's reader at `$Cn21`; checksum; `$C8`; wait SENSE LOW (`$C5E8`); drop REQ (`$C5ED`). |
| `$CE00` | INIT scan: one INIT per device, dest `$81`, `$82`…; reply status zero = "more devices follow", non-zero ends the scan; the count lands in `$07F8,n` and every later unit number is checked against it (`$CCD1`). |
| `$CBE6` | calls: command contents = `$42..$4A` — command, **parameter count** (the table at `$CDE3`), buffer, 24-bit block; the **unit is the packet's destination** (`$CD02` swaps the chain number into `$5A`, `$C837` puts it on the wire). WRITE is followed by a `$82` data packet of 512. The reply's status byte is the ProDOS result — `$00` for a good READ, and for STATUS the four status bytes land at `$45` (bit 4 of the first must be set or the firmware returns `$2F`). |

Frame: `FF… $C3 | 7 header | odd section | groups | chk1 chk2 | $C8`, every
byte carrying bit 7. Contents travel as an odd section (marker + up to six
bytes) and seven-byte groups behind their own marker, most significant first
— the ROM's tables at `$CA27/$CA37/$CA47/$CA57` are just `$80`/`$00` masks
keyed on the marker's bits. Checksum = XOR of the header WIRE bytes and the
decoded contents, sent 4-and-4, recovered as `((chk2 << 1) | 1) & chk1`.
`SmartPortBusDevice` decides packet completion from the header's counts, not
by hunting `$C8` (a data byte `$48` is `$C8` on the wire).

**The ACK line is a handshake, not a presence flag** — the bug that kept the
first responder at one transaction per session. SENSE is HIGH whenever REQ is
low (device ready); it goes LOW when a packet has been taken or a reply fully
read, the host waits for that edge before it releases REQ, and the release is
what makes the device ready again — and what puts a prepared reply on the
wire. Modelling it as "high while no command is pending" made the second
probe of every session find a device that had gone away, and the firmware's
3000-retry loop looked exactly like a hang.

Three more, each of which cost a run: the write handshake's bit 6 is an
underrun flag the firmware waits to see **clear** (answer `$80`, not `$C0`, or
it parks in the drain loop at `$C92C`); the byte the sender stores after the
terminator (`$C933`) is not part of the packet and must not kill the reply
just armed for it; and the enumeration's reply buffer is a few bytes of ZERO
PAGE, so answering an INIT with 512 bytes walks over `$004B/$004C`, the very
fields that say how long the packet is, which reads like a checksum failure.
`POM2_TRACE_SMARTPORT_BUS=1` prints every byte in both directions;
`IWMDevice::setBusCapture` keeps the bytes out of the IWM's shifter so the
traffic raises no "write underrun".

Backing is a `SmartPortBusUnit` (media / block count / write-protect /
read / write) — `LironCard` adapts its `Disk35Image`s, `SmartPortCard` its
`SmartPortUnit`s. Two units per chain by default; INIT answers "last device"
on the second.

### The //c external 3.5" port (IIcExternalSmartPort)

On a //c one IWM drives the internal 5.25" and the rear connector; the enable
line picks the drive. POM2 keeps the 5.25" on `DiskIICard` (its LSS is the
write authority everywhere, and a second controller on those soft switches
once sent DOS 3.3 into seek storms — `iic_diskii_no_iwm_conflict` pins that
the machine's shared IWM stays out of it). So the external port carries its
**own** `IWMDevice`, used as nothing more than a register tracker — Q6/Q7,
enable, SEL, the phases — with no mechanism attached, and a
`SmartPortBusDevice` that answers the bytes. `IIcClassProfile::ioReadIWM /
ioWriteIWM` route `$C0E0-$C0EF` to it on the plain 32 KB //c; it tracks every
access and **claims** one only while the firmware is addressing the bus
(PH1 + LSTRB high with the port enabled) or a transaction is in flight, and
only while a unit holds media (`live()`). Everything else falls through to
the Disk II exactly as before — and a claimed access is not forwarded to the
Disk II as a side effect either: on the real machine the internal drive is
not enabled during that traffic, and forwarding it left the card's empty
second bay spinning for ever after a 3.5" session.

The units come from whatever sits in slot 5 through
`SlotPeripheral::smartPortBusUnit` — the built-in `smartport35` the //c
profiles plug there — so the media panel is unchanged: what the user mounts
on "slot 5" is what the firmware finds on the port. `SmartPortBusPort.h` is
the MACHINE↔DEVICES contract; `EmulationController` owns the port and hands
it to `Memory`, which forwards it to each //c-class profile it builds. The
port travels in snapshots as a self-identifying tail of the //c-class
profile's blob (`XSP1`: its private IWM behind a length, the bus state,
the two line bytes), and `LironCard` carries the same for its own IWM and
bus (`LIR1`). The rewind ring snapshots every frame and a block transfer
spans several, so "start clean on restore" had meant a rewind landing
inside one handed the firmware an empty reply; a blob without the tail
still restores and starts the port clean.

**The //c+** has the same connector and its own SmartPort implementation
(bank 1, trampolined through page 3; the bus send at `$C895` with the Liron's
sync table, the presence scan at `$F223` — fifty SENSE polls with SEL on
drive 2). There the shared IWM already owns the port for the MIG-routed Sony
drives, so the port does not track registers of its own: `ioReadIWM /
ioWriteIWM` perform the access on that IWM and call the contract's `shared*`
hooks around it (`sharedWantsWrite` → `setBusCapture`, `sharedAfterWrite`,
`sharedAfterRead`), same claim rule, same answers. One protocol fact only the
//c+ exposed: **chain numbers are the host's**. Each INIT names the next
device with whatever number the scan reached — 1 on a Liron or a //c, 2 on a
//c+ whose internal drive is device 1 — and a responder that assumed 1 refused
every //c+ READ with `$2F`. `SmartPortBusDevice` now assigns on INIT and
forgets on bus reset. Pinned in `iic_external_smartport` (D: empty bay boots
the external 3.5"; E: internal boot lists both units).

**Three rules bug hunt 3 added** (2026-09-01). A `$C0Ex` write the port
claims is dropped from the slot bus (`ioWriteIWM` returns the claim; the
read side already did this) — the packet bytes are the exact shape of a Disk
II write sequence and the card spliced them as flux. On a //c+ whose
firmware serves the port, `EmulationController::bootFromSlot(5)` is a reset
(`Memory::iicPlusBootsSlot5ByReset`): the real `$C500` entered directly never
scans the port, the reset-time scan at `$F223` does. And a frame whose
checksum fails gets no reply, while any change in which units hold media
resets the protocol (`live()` compares a media mask) — a stale half-frame
must never be spliced with the next transaction.

**Media persistence for cards without a keyspace** (`LironCard` today):
`StorageCoordinator::genericMediaCard` picks any `MountableMediaCard` that is
not a SmartPort, CFFA or HDV card — those keep their own keys, which are on
disk in every user's settings — and persists its bays under
`media_slotN_bayK_path` / `_writeback`, restored with the same cwd anchors
as the SmartPort units.

**The 16 KB //c (ROM 255)** has no SmartPort firmware: its `$C500` is not a
disk page (`FF 20 4D CE …`), so nothing on that machine can speak to an
intelligent drive — historically the reason for the ROM 0 upgrade. Its rear
connector takes a second 5.25", which is `DiskIICard` drive 2; a 3.5" there
is reachable only through the host-served `$C500` substitute.

*WOZ (flux) images* (2026-08-18). A `.woz` holds bit CELLS, and POM2
stores 3.5" media as a flat block array with no GCR *encoder* — so a flux
dump has nothing to be mounted as unless it is decoded at LOAD time.
`Disk35Image::loadWoz` walks the WOZ2 chunks (TMAP indexes `track*2+side`
on a double-sided 3.5"; TRKS entries give startBlock/blockCount/bitCount)
and runs each track through `Sony35Gcr` — the **same** decoder
`Sony35Drive::decodeAndCommit` uses when the guest writes a track, moved
out of `Sony35Drive.cpp` so there is one copy of MAME's tables and
checksum walk rather than two.

Three things that decide whether it works:

* The track is a **circle**. Walking it once loses the sector straddling
  the seam — ~1 per track-side, ~150 blocks on an 800K disk. The loader
  walks one revolution plus an overlap and de-duplicates by block.
* A WOZ mounts **write-protected**, and `setWriteBackEnabled` does not
  override it: handing blocks back means re-encoding the user's flux.
* `classifyDiskForSlot` reads `INFO.disk_type` rather than sniffing size
  (flux size describes the dump, not the payload), so an 800K 3.5" WOZ
  reaches the Sony bay instead of the Disk II. A 5.25" WOZ mounted in the
  3.5" bay is refused by name, and vice versa.

Pinned by `woz35_load`: a synthetic WOZ2 built with an INDEPENDENT
encoder (written from MAME `build_mac_track_gcr`, not POM2's tables — a
test sharing them could not catch a bad table), round-tripped byte-exact
across a zone boundary on both heads.

*IWM wiring*. `IWMDevice` exposes `phasesCb_/devselCb_/sel35Cb_`
(MAME `iwm_device::phases_cb/devsel_cb/sel35_cb`); wired via
`SmartPortHub::attach`. `nextTransition()` dispatches between
`DiskImage*` and `Sony35Drive*` via `setFloppy/setSony35`. `$C0EE`
WPT bit consults `Sony35Drive::senseR()`.

*No-disk noise flux*. With no media `nextTransition()` would return
`INT64_MAX` → read FSM shifts only 0-bits, `data_` stays `$00`, bit-7
never asserts → boot's wait-for-byte loop spins forever. Falls back
to `noiseTransition()` — deterministic LCG keyed on read-window
index, straddles `windowSize()` boundaries so SR accumulates 1s/0s
and emits garbage bytes with bit-7 set. Lets //c reach **"Check Disk
Drive."** and //c+ reach **"UNABLE TO FIND A BOOTABLE DISK ONLINE."**
at power-on. Pinned: `iic_nodisk_boot_trace`.

*GCR encoder* (verbatim MAME `flopimg.cpp::build_mac_track_gcr
2017-2106`). Five speed zones (`kCellsPerRev[5] = {76950, 70695,
64234, 57749, 51388}`, MAME `:2019-2027`), per-zone CPU-cycles-per-rev
= `60 × POM2_CPU_CLOCK_HZ / RPM`, 64-entry `kGcr6fw[]` (MAME line
967), `gcr6Encode(va,vb,vc)` 3-in-4-out packer (MAME line 512).
Per-sector: 8× self-sync (384 cells) + D5AA96 addr prologue + 5 GCR
header + DEAAFF addr epilogue + 2× self-sync + D5AAAD data prologue +
174× 3-in-4-out + 4-byte checksum + DEAAFFFF epilogue = 6208 cells.
Block-to-physical 2:1 interleave (`si = (si+2) % ns; if(si==0) si++`).

*Flux write-back*. `Sony35Drive::writeFlux` splices flux into cached
cell buffer, runs GCR→blocks decoder (MAME `flopimg.cpp:2107
extract_sectors_from_track_mac_gcr6`). Recovered sectors that differ
push via `writeBlock`; image flushes to `.po` via `saveDirty()` on
`eject35` or shutdown. WP honoured. Nibbliser port of `flopimg.cpp:
1530 generate_nibbles_from_bitstream`. **Gotcha**: cycle↔cell
rounding uses round-to-nearest on decode side; encoder uses floor on
`cycleForCell = i × period / n`. Without symmetric rounding,
integer-truncated `2.024 → 2` pushed every transition one cell early
and lost the first sector's addr marker.

*UI / CLI / persistence*. `Disk35Controller_ImGui` (2 Sony slots:
internal = on-board //c+; external = SmartPort daisy-chain).
Mount/Eject, last-error, scanner picks up `.po`/`.2mg` of right size
under `disks35/` (falls back to `disks/`). Toggle
`show_disk35_panel`. CLI: `--35-disk1/--35-disk2`; settings:
`disk35_path_1/_2`.

Pinned: `smartport_35_smoke_test` — load + size guard, SENSE
empty/in-slot, motor strobe, hub recalc (devsel=1+35sel=true AND
devsel=2+intdrive=true), phase fwd, marker placement (12+12 on track
0), full encode→flux→splice→decode→block-readback round trip, WP
short-circuit.

## Peripherals

### Super Serial Card (slot 2) + telnet bridge

6551 ACIA at `$C0A8-$C0AB` (data/status/cmd/ctrl). Status bit 4 =
TDRE (always 1), bit 3 = RDRF (RX queue), bits 5/6 = DCD/DSR (TCP
state). Unconnected `$C0A8` returns 0.

Slot ROM `$C200-$C2FF`: autodetect bytes (`$Cn05=$38`,
`$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$31`); `JMP $Cn20` skips them.
PR#2 hooks CSWL/CSWH (`$36/$37`) → `$C2B0`; IN#2 hooks KSWL/KSWH
(`$38/$39`) → `$C2E0` (load + ORA #$80). Reset clears rings.

**Pascal 1.1 ID block** at `$Cn0D-$Cn10` (NOT `$CnFB-$CnFF` — TODO
note was wrong): offsets of PINIT/PREAD/PWRITE/PSTATUS routines
after the `$Cn0B=$01`/`$Cn0C=$31` signature. Layout + calling
convention per real SSC ROM (6502disassembly.com/a2-rom/SSC). Pinned:
`ssc_acia_smoke::testPascalIdBlock`.

TCP listener on `127.0.0.1:port` (default 6502); one client. 4 KB
rings; telnet IAC (WILL/WONT/DO/DONT + 2-byte + `$FF $FF` literal)
handled by `processTelnetRx` so stock `telnet` connects. Swallowing an
option request is not the same as answering it — a client whose DO/WILL got
no reply stays in **line mode**, which is what made typing feel wrong.
`processTelnetRx` now **replies** (2026-09-07). And the ACIA's **transmit
interrupt** was computed and then thrown away (`(void)txIrqEnable`), so a
driver using command `$05`/`$09` and sleeping on TX IRQ never woke;
`IRQ_TDRE` is raised now. `TCP_NODELAY` on. Auto-plugged at startup; listener starts only when
`ssc_listening=true`. LF→CR RX symmetric; raw-mode toggle (default
OFF). Port + state persisted. Pinned: `ssc_acia_smoke`.

### ProDOS clock card (slot 4)

ThunderClock+ compatible. **ProDOS does NOT route through slot ROM**
— boot copies hardcoded driver to RAM (~$D742), patches
`$BF06-$BF08` to JMP it, then driver speaks device-select. Slot ROM
only needs detection signature.

Slot ROM `$C400-$C4FF`: signature bytes `$08, $28, $58, $70` at
offsets 0/2/4/6. Odd-offset fillers form benign fall-through;
`$Cs08 = RTS`.

**uPD1990AC bit-bang at `$C0C0`**:
```
write bit 0 = DATA_IN; bit 1 = CLK; bit 2 = STB; bits 3..5 = C0/C1/C2;
      bit 6 = IRQ enable ($40)
read  bit 5 = IRQ asserted; bit 7 = DATA_OUT (LSB of shift register)
```

Mode `0b011` = `MODE_TIME_READ`: arm via `$C0C0=$18`, pulse STB
(`$1C`) to latch host time into 48-bit shift register, drop STB,
read bit 7 + pulse CLK (`$1A`/`$18`) 48 times → 6 BCD bytes (sec,
min, hour, day, (month<<4)|dow, year). Mode `0b010` = `MODE_TIME_SET`:
load 48 bits via DATA_IN + 48 CLK, then STB-in-TIME_SET commits via
`commitTimeSetFromShiftReg()` (`std::mktime`, delta captured as
`userOffsetSeconds`).

**TP interrupts** (POM2-original — MAME's `a2thunderclock.cpp` never
binds `tp_callback`). Wiring per ThunderClock Plus manual ch. V:
`$C0n0` bit 6 (`$40`) is enable latch; TP rising edge sets request FF
→ `assertIrq(true)` while enabled; **any** device-select read/write
clears request (enable latch persists, periodic source keeps ticking);
read `$C0n0` bit 5 = "interrupt asserted" flag; RESET disables.
Rates decode latched C0/C1/C2 on STB rising edge: dividers 512/128/
16/8 against 32.768 kHz XTAL → **64/256/2048/4096 Hz** (modes 4-7),
plus 64 Hz for REGISTER_HOLD. Interval timers (1/10/30/60 s, modes
8-15) need uPD4990A 4-bit serial, unreachable on parallel uPD1990AC —
not modelled. Pinned: `clock_card_smoke` (TP rates, IRQ enable, bit-5
flag, reset).

**MODE_SHIFT lax-gating divergence**: POM2 shifts on **every** CLK
rising edge regardless of mode (MAME `upd1990a.cpp:312-327` gates on
`m_c == MODE_SHIFT`). ProDOS's hardcoded driver pulses CLK while
still in MODE_TIME_READ; strict gating breaks stock ProDOS. Observed
HW permits the shortcut. Pinned: `testShiftLaxAcrossModes`.

**Optional real ROM dump.** Drop `roms/thunderclock_u9_v1.3.bin` (also
accepted: `thunderclock_u9.bin`, `thunderclock.rom`,
`Thunderware_REV_1.3_ROM_U9.bin`) and `ClockCard` swaps the synthetic
slot-ROM stub for the dumped U9 EPROM. Accepts 256 B (slot ROM only)
or 2 KB (slot ROM + $C800 expansion ROM mirroring the same chip into
both windows so the firmware's $C8nn JMP continuations resolve).
Source: markadev/AppleII-RevEng/Thunderware-Thunderclock-Plus. The
load path validates the $08/$28/$58/$70 ProDOS signature at
offsets 0/2/4/6 and falls back to the synth ROM if absent.

### //c on-board IWM vs the slot-6 Disk II

`MemoryProfile_IIcClass.cpp` (`ioReadIWM` / `ioWriteIWM`) mirrors
`$C0E0-$C0EF` into the on-board IWM — but **only on the //c+**
(`isPlus_`), and that gate is load-bearing.

MAME wires `A2BUS_IWM` at sl6 for 32 KB //c-class machines as *the*
controller, replacing the Disk II. POM2 does not: `iwmAuthoritative`
leaves the slot-6 `DiskIICard` (the MAME-parity LSS) answering for 5.25"
media. So on a plain //c the mirror contributed **no data path** while
still running the IWM's own phase/motor handling — a second controller on
the same soft switches. Two controllers stepping one drive drifts the
head, and DOS 3.3 RWTS then loops in seek/retry (`$B948-$B956`, head
oscillating between the target track and 0).

The visible symptom was in a completely different subsystem: Print Shop
on a //c could not save its setup or load its print overlay, so it
returned to its menu without rasterising and **printing produced nothing**
— while the SSC → ImageWriter path was provably byte-exact. Worth
remembering when a //c bug looks like it belongs to whatever subsystem
noticed it first.

Pinned by `iic_diskii_no_iwm_conflict` (plain //c must not claim — or
even tick — the IWM; //c+ must still route to it).

### Host sockets (POSIX / Winsock)

`src/SocketCompat.h` is the ONE place that answers "POSIX or Winsock?".
Four TUs consume it — `W5100Device` (Uthernet II TCP/UDP),
`SuperSerialCard` (telnet bridge), `AiControlServer` (HTTP control API),
`SpTcpTransport` (the FujiNet SP-over-SLIP TCP pipe) —
and `SocketUtil.h` (the accept/SIGPIPE idioms) is built on top of it.
`POM2_HAS_SOCKETS` is now 0 for **Emscripten only**; Windows is a full
host-socket target since 2026-08-01.

Winsock is the same stack behind a different API, and its differences are
**silent** — code that compiles clean against it can still be wrong.
Seven traps, each removed by a helper rather than by remembering:

| # | Trap | Helper |
|---|---|---|
| 1 | `SOCKET` is **unsigned**; failure is `INVALID_SOCKET`, not -1 — so `fd >= 0` is always true and `fd = -1` marks a socket *valid* | `socket_t`, `kInvalidSocket`, `isValidSocket()` |
| 2 | Errors bypass `errno` (`WSAGetLastError`, `WSAEWOULDBLOCK`, no `strerror`) | `lastSocketError()`, `errWouldBlock/InProgress/Interrupted()`, `socketErrorText()` |
| 3 | `close()` closes a CRT fd, not a socket; no `fcntl(O_NONBLOCK)` | `closeHostSocket()`, `setNonBlocking()`, `shutdownBoth()` |
| 4 | The stack needs `WSAStartup` before the first call | `ensureSocketStack()` |
| 5 | A member `closeSocket()` shadows a namespace-scope one — class scope wins, `socket_t`→`size_t` converts silently, infinite recursion | the helper is named `closeHostSocket`, deliberately |
| 6 | `SO_REUSEADDR` on Winsock lets another local process hijack a listener | `setListenerBindPolicy()` |
| 7 | Winsock reports datagram-scoped errors (`WSAEMSGSIZE`, ICMP-derived `WSAECONNRESET`) on unconnected UDP | `errDatagramDiscard()`, `disableUdpConnReset()` |

Trap 5 is not Winsock's fault and bit this port anyway: `W5100Device`
already had a chip-level `closeSocket(size_t)` (the CLOSE command), so
`closeSocket(s.fd)` inside that class compiled clean and blew the stack —
caught by `uthernet2_w5100_smoke` as a segfault with 74 000 identical
frames.

**Readiness waits use `select()` on Windows, not `WSAPoll()`**, and the
reason is `W5100Device::poll()`: it waits for WRITE on a socket with a
non-blocking connect in flight and must learn about a *refused*
connection, not only a successful one. On Winsock the documented channel
for that is `select()`'s `exceptfds`. A wait that could only report
success would leave a guest polling `SN_SR` forever on a refused
connection. `waitSocket()` folds the exception set into "ready" so the
caller does what it does on POSIX: wake, then ask `getsockopt(SO_ERROR)`
which of the two happened (`connectResult()`).

Two more Windows-only details worth keeping: `SO_RCVTIMEO` takes a
`DWORD` of milliseconds there, **not** a `timeval` (passing a timeval is
accepted and then read as garbage), and there is no `SIGPIPE`, so
`disableSigpipe`/`sendNoSignal` are no-ops. `inet_ntoa` is avoided
entirely (static buffer, two threads logging at once splice each other's
addresses; MSVC deprecates it) in favour of `peerAddressText()`.

Verification is by **cross-compilation**: `x86_64-w64-mingw32-g++
-fsyntax-only` over every `src/*.cpp`, which is what proves the include
order is safe in the big consumers too. `SocketCompat.h` turns the one
remaining ordering hazard — a TU that pulled `windows.h` in first, so
winsock v1 is already loaded — into a single `#error` instead of fifty
redefinition errors.

### No-Slot Clock (`NoSlotClock` — DS1216E SmartWatch)

The DS1216E is a 28-pin socket that physically sits *under* a ROM chip and
intercepts reads to it — the canonical "clock without using a slot" for a //c
(no expansion slots) or for any //e / II+ owner out of slots. POM2 models the
full chip: `src/NoSlotClock.h/.cpp`, MAME ref `ds1216.cpp`, protocol verified
against AppleWin's `NoSlotClock.cpp`. On by default (`nsclock_enable`), and a
no-op for software that never walks the magic key.

**Protocol.** The host drives the chip through *addresses*, not data. A2
selects the operation and A0 carries the payload bit:

- **A2 = 0** — "write" cycle: feed the next bit of the 64-bit magic key, taken
  from A0 (so `$F800` sends 0, `$F801` sends 1, …). 64 consecutive matches
  move the chip into clock-readout phase.
- **A2 = 1** — "read" cycle: emit the next clock-register bit on D0. During
  pattern-matching a read instead **resets** the matcher.
- A single wrong A0 bit **disables further writes** and the matcher stays dead
  until a read (or a reset) clears it — the Dallas datasheet's sticky
  bad-pattern rule.

Reads and writes drive the *same* matcher, because the key bit rides on the
address and R/W is irrelevant to it (AppleWin's `CNoSlotClock::Write(address)`
calls the identical pair). POM2 hooks both: some NSC drivers feed the key with
`STA`, and with only a read hook they never unlock the clock.

**Where the hook sits depends on the machine** — the chip is under whichever
ROM the era's drivers probe:

| Machine | Window | Why |
|---|---|---|
| II / II+ | `$F800-$FFFF` (Monitor ROM), gated on LC-ROM-mapped | no internal slot-3/8 ROM to hide under; matches AppleWin's `!SW_HIGHRAM && !SW_WRITERAM` |
| //e, //c-class | `$C300-$C3FF` and `$C800-$C8FF` | where ProDOS 8 ≥ 2.0.3 and GS/OS actually scan (AppleWin `IsPotentialNoSlotClockAccess`) |

That split is why `Memory`'s inline ROM-read fast path carries
`!(noSlotClock_ && !iieMode && addr >= 0xF800)` (`Memory.h:204`) — it only has
to step aside for the II/II+ window; the //e / //c hooks live inside the
INTCXROM / SLOTC3ROM branches that were already on the slow path.

Time source is injectable (`NoSlotClock::TimeFn`) so the test can pin a
deterministic clock. Pinned by `no_slot_clock_smoke`
(`tests/no_slot_clock_test.cpp`).

### AI control server (`AiControlServer`)

An HTTP/1.1 listener on **loopback only** (`INADDR_LOOPBACK`, default port
**6503** — deliberately one off the SSC's 6502) that lets an external process
drive POM2 the way a human drives the UI. Written for AI agents: `curl` or an
MCP driver types at the keyboard, resets, mounts disks, peeks/pokes RAM, takes
snapshots and grabs the framebuffer. Inspired by `paleotronic/microm8-cln`
(`remint/`, `fastserv/`).

**Off by default** — three settings keys gate it: `ai_control_enable`,
`ai_control_port`, `ai_control_token`. Authentication is an optional shared
secret in an `X-POM2-Token` header; with an empty token configured, requests
are accepted unauthenticated, on the grounds that a loopback-only listener
already limits exposure to local processes.

Endpoints (the header comment on `AiControlServer.h` is the source of truth
for the exact JSON shapes):

| Route | Verb(s) | Does |
|---|---|---|
| `/status` | GET | profile, cpu_mode, mode, cycles_per_frame, CPU regs, mounted disks |
| `/cpu` | GET / POST | register dump / set `pc`,`a`,`x`,`y` |
| `/mem?addr=N&len=N` | GET / POST | hex read (len ≤ 4096) / bulk RAM write |
| `/reset` | POST | `{"kind":"soft\|hard\|cold"}` — the three verbs in [CLAUDE § Reset](CLAUDE.md#reset-architecture) |
| `/keyboard` | POST | `{"text":…}` / `{"raw":…}` → the paste queue |
| `/disk`, `/eject` | POST | insert / eject by `{slot, drive, path}`; the endpoints drive the **primary** (lowest-slot) Disk II — `slot` may be omitted, and when given must match that card's real slot (validated and echoed back; a hard-coded "6" used to touch the slot-5 primary while confirming slot 6) |
| `/snapshot/save`, `/snapshot/load` | POST | save path **must** end `.pom2snap` so an agent cannot clobber an unrelated file; load magic-byte-checks the blob |
| `/speed` | POST | `{"cycles_per_frame":N}` or `{"preset":"1x\|2x\|max"}` |
| `/screen.ppm` | GET | binary PPM of the live framebuffer |
| `/mouse` | POST | signed delta (±127/call) or absolute counter + button, straight into the Mouse Card's host-motion input |

**Threading**: one worker thread, one client at a time. Each request takes
`EmulationController`'s state lock for exactly the slice that touches
CPU/Memory/slot state — the same rule the UI thread follows. `/keyboard` is
the exception and needs no state lock: `Memory`'s own paste-queue mutex covers
it.

The JSON parser is hand-rolled (`AiControlServer.cpp` `jsonParseValueAt`) — a
request reader, not a document parser, on the same "minimum external deps"
policy that kept the SSC's TCP listener to 200 lines. On Emscripten the whole
listener degrades to a stub (`start()` returns false) — see
[§ WASM socket stubs](#webassembly-browser-build).

### Host serial ports (`SerialPort`)

`src/SerialPort.h/.cpp` is to serial what `SocketCompat.h` is to sockets: one
compat pair covering POSIX `termios` and Win32 `DCB`, with no knowledge of what
speaks the protocol on the other end. POM2 had no serial code before the
FujiNet USB transport needed it — `SuperSerialCard` emulates a 6551 and bridges
it to *TCP*, it never opens a host device. `POM2_HAS_SERIAL` is 0 under
Emscripten only.

**Three traps it exists to remove**, all silent:

1. **The ESP32 auto-reset circuit.** Every ESP32 USB bridge wires **DTR → EN
   (reset)** and **RTS → IO0 (boot select)** through the two-transistor
   auto-reset circuit — that is exactly how `esptool` enters the ROM
   bootloader with no button press. An `open()` that lets the OS assert those
   at their defaults **reboots the user's FujiNet, or strands it in the
   bootloader, every single time POM2 opens the port**, and the symptom ("my
   FujiNet restarts when I launch the emulator") points nowhere near the
   emulator. `open()` de-asserts both before the first byte and clears
   **`HUPCL`**, so that *closing* the port — i.e. quitting POM2 — does not
   drop DTR and reset the board either.
2. **Raw mode is not optional.** SLIP frames carry `$11`/`$13`, which `IXON`
   eats, and `$0D`/`$0A`, which `ICRNL`/`ONLCR` rewrite. `cfmakeraw` plus an
   explicit re-clear of those flags; on Win32, `fBinary` with every flow-control
   field disabled.
3. **macOS: `/dev/cu.*`, never `/dev/tty.*`.** The `tty.` twin blocks on
   carrier detect, which a USB CDC device never raises, so the open never
   returns. `enumerate()` only ever reports `cu.*`.

Two smaller ones: Win32 needs the `\\.\` prefix or `COM10` and above resolve
to nothing, and on Linux a user outside the `dialout` group gets a bare
`EACCES` — `lastError()` spells out the fix rather than reporting "open
failed".

Enumeration prefers `/dev/serial/by-id/*` on Linux (those names survive a
replug; `ttyACM0` does not), `cu.usbmodem*` / `cu.usbserial*` on macOS, and
`HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` on Windows.

Pinned by `tests/serial_port_test.cpp`, which uses a **pty pair**
(`posix_openpt`/`grantpt`/`unlockpt` yields a real slave device node). Honest
limit: a Linux pty has no modem-control lines — `TIOCMGET` fails on both ends —
so the harness pins the *termios* half of trap 1 (HUPCL clear, CLOCAL set) and
the binary round trip, and only asserts the DTR/RTS levels when the device
actually has them. The line-state half is on the manual checklist in
[docs/fujinet_plan.md](docs/fujinet_plan.md).

### 4play (`FourPlayCard`)

Port of MAME `src/devices/bus/a2bus/4play.cpp`. Four **digital** joysticks on
an Apple II, one byte each at `$C0nX`.

Why it exists at all: the Apple II's own game port is *analogue* and carries
two paddles, so two players with sticks is the ceiling and reading them means
timing an RC discharge ($C064-$C067). This card is four reads with no timing
and no calibration — which is how an Apple II gets four players.

**It is modern homebrew, not period hardware** (Lukazi, 2016; MAME's header
links the blog). So the software that uses it is the current Apple II scene —
worth stating plainly, because "MAME has it" and "there is software for it"
are different claims and only the first is automatic.

The whole device is `read_c0nx`: offsets 0-3 are players 1-4, everything else
is `$FF`. No write side, no ROM, no state — MAME's `device_start()` is empty.
The one trap is the bit layout (`4play.cpp:41-48`): everything is active HIGH
**except bit 5**, which is `IP_ACTIVE_LOW IPT_UNKNOWN` and therefore reads
back **set**. An untouched stick is `$20`, not `$00`; a test that assumed zero
would pass against a card that never updates.

| bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| | B1 | B2 | *always 1* | B3 | right | left | down | up |

**Threading follows `PaddleInputs`**: the four bytes are `std::atomic`, the UI
thread writes them from `pollJoystickAndPushToMemory`, the CPU worker reads
them in `deviceSelectRead`, and neither takes `stateMutex`. Host pads 1-4 map
to players 1-4 in order, with a 0.5 gate turning each analogue stick into
directions. The pad bound to the analogue game port keeps that job too — a
real desk with one stick and one card behaves the same way.

**Deliberately not in the snapshot.** Joystick position is host input, not
emulated state, exactly like the game-port paddles: a rewind must not put
somebody's thumb back where it was. `onReset()` is an explicit no-op for the
same reason, so the absence reads as a decision rather than an oversight.

Pinned by `fourplay_card`: the bit layout including the active-low bit, four
independent players, out-of-range players ignored rather than smearing into a
neighbour, offsets 4-15 reading `$FF`, and the whole thing again through a
real `SlotBus` at `$C0C0-$C0CF`.

### Apple II Workstation Card (`WorkstationCard`)

The board that put a IIe on LocalTalk. **It is a coprocessor, not a ROM
card** — a 65C02 of its own, 28 KB of RAM, a Zilog 8530 SCC, an interval
timer, and 64 KB of ROM banked into a 32 KB window. The Apple II never
executes that firmware; it reaches the card through a shared RAM page.

There is **no MAME oracle**: MAME has no Workstation Card at all. Every line
of the map in `WorkstationCard.h` was read out of the 341-0358-A dump with a
disassembler and then confirmed by *running* the firmware — the derivation is
in [printer plan 2 § 5.2](docs/printer_plan_2.md#52-the-memory-map-the-dump-implies)
and the header states the evidence per line. Two examples of the shape of that
evidence, because it is what makes the map trustworthy:

* **`$7A00` is five bits wide.** The card's own POST writes `$FF` into it with
  a `DEC` and then compares against `#$1F` (`$F131-$F139`). The firmware
  documents its own hardware.
* **`$7C00` is the ROM bank select.** Proved by a trampoline the firmware
  relocates *into RAM* at `$42D1`: `LDA $40BB / STA $7C00 / JSR $CC32 /
  LDA $029A / STA $7C00`. It has to live in RAM precisely because it switches
  the ROM out from under itself — which is also why the card crashed in the
  probe until the banking was modelled.

**The `$Cn00` page is a window on card RAM `$0200`, not a ROM page.** The
driver proves it: its own `$Cn00` code sets `$FB/$FC` to `$Cn00` and then
stores *through* it. What the guest reads there is what the card published,
copied out of ROM at boot (`$C0DA: LDX #$E3 / LDA $C3D9,X / STA $01FF,X`).
The correspondence is checkable — the driver reads `$Cn9A`, and `$029A` is
where the card keeps its ROM-bank value. `$C800-$CFFF` is a plain ROM slice
at file `$C800`, entered by the page's `JMP $CC00`.

**`advanceCycles` interleaves; the slice is correctness, not tuning.** The
slot bus hands out ~4096 cycles at a time. Running the card CPU for all of
them before the SCC moves makes the chip stand still for four byte-times while
the firmware burns the fixed poll budget of its self-test, and the POST then
fails on a timeout no real card would see. `kSliceCycles = 24` is below one
poll iteration. Widen it and `workstation_card_smoke` says so.

**What it costs**: a second 6502 at the Apple II's own rate, so roughly double
the emulation work while plugged. That is what a coprocessor board is.

**The host handshake works, and it is worth understanding once.** AppleShare's
`ATINIT` calls the card at `$Cn14` in ProDOS-MLI style —
`JSR $Cn14 / .BYTE cmd / .WORD block` — and POM2 services it: the AppleShare
IIe Workstation disk boots, passes the card's power-up diagnostics and reaches
its menu. The mechanism is that **the card rewrites the host's code**. It
releases the host's spin loops by writing `$38` (`SEC`) over the `$18` (`CLC`)
the host is executing in the shared page; it patches `$CnBB`/`$CnBC` — the
operands of the host's own `JMP` — to steer where the host goes next, and
`$CnC3`/`$C4`/`$C6`/`$C7`, the address operands of the host's block-move loop,
to say what it copies. Data crosses a byte at a time through `$CnEA` with
`$02A9` as the handshake.

**One number cost three sessions**, so it is written down: the
`$C800-$CFFF` window is served from file **`0xC400`**, not `0xC800`. With the
wrong base the page's `JMP $CC00` lands on a block-copy loop instead of the
driver prologue and the two CPUs deadlock with nothing pointing at the cause.
Nine bases were swept; only `0xC400` completes a transaction. On the way there
a plausible theory — that the `$C08x` strobe must set bit 6 of `$02EE`,
because the card waits on it and nothing else writes it — was wired up and
**moved the card one step further**, which made it look right. It was not: with
the base corrected everything completes with no strobe modelling at all.
A change that unsticks a stuck system is not evidence that it is correct.

`$C0nX` therefore still answers `$FF`, and `hostStrobeLog()` records what a
guest does with it. → `TODO.md` § Workstation Card

Pinned by `workstation_card_smoke`: cold reset enters `$C000` in the high ROM
half, the card's 65C02 completes the POST, the SCC ends in SDLC at 230400
bit/s, **the card acquires a LocalTalk node address and transmits LLAP
frames**, the bank trampoline is where the map says, `$Cn00` round-trips to
card RAM both ways, `$C800` serves the host driver, the snapshot round-trips
(chip included) and rejects foreign blobs, reset re-boots cleanly, and —
plugged into a real `SlotBus` — the guest reads the shared page through
`Memory::memRead`.

And pinned from outside POM2 entirely by `workstation_card_cardcat`, which
boots **CardCat** off `disks_3.5/CardCat 1.94.po` on an enhanced //e and reads
the answer off the text screen:

```
 4   Apple II Workstation Card
```

CardCat walks the slots looking at each one's firmware signature bytes, so if
the `$Cn00` window served the wrong page — or served ROM where the card serves
RAM — it would say "Unknown" or "No Firmware Card Detected". The test carries
a **negative control**: the same boot with the slot empty must not print the
name, so the string has to be coming off POM2's bus rather than out of
CardCat's own text. Two 40 M-instruction boots, hence the `slow` label.

#### Foreign bus (`Memory::ForeignBus`)

POM2 has exactly one 6502 core and `M6502` reaches memory through `Memory`.
A coprocessor card needs that core over a completely different map. Giving
`M6502` a bus abstraction was measured out of the question before it was
attempted: PERFORMANCE §§ 8.2/8.5 put a branch on the bus path at **+13-16 %**
and *merely testing a flag* at **+7.2 %**.

So nothing was added. `flatBus_` **replaces** the `testMode` test that both
slow paths and `memWrite`'s fast path already made — one byte load for
another — and `foreignBus_` folds into the three derived read gates exactly
the way `readDivert_` does, so `memRead` is untouched. A Memory with a foreign
bus takes the slow path for everything, which costs nothing because nothing
else in POM2 shares that instance. Measured on both `pom2_bench` workloads,
interleaved best-of-5, two passes each: −1.2 % to −2.3 %, with RAM and
framebuffer hashes byte-identical. The honest reading is **no measurable
cost**; the sub-2 % is layout luck, not a speedup.

One thing the setter does beyond the flags: it parks `vblNextEventCycle_` at
the end of time, so a card's Memory never runs the Apple II's beam-race
bookkeeping on every instruction of a machine that has no beam.

### Zilog Z8530 SCC (`Scc8530Device`)

Port of MAME `src/devices/machine/z80scc.{h,cpp}` at commit
`588eeb33707f8d392701716c41b0420a48c41f28` (2026-08-29). The 8530 is the
two-channel USART behind the Macintosh and IIgs serial ports and behind the
**Apple II Workstation Card**, which is why it landed: the card's firmware
drives it at `$7500-$7503` in the card's own address space
([printer plan 2 § 5.1](docs/printer_plan_2.md#51-the-341-0358-a-dump)).

**Variant fold.** MAME's `z80scc_device` covers eight parts behind an
`m_variant` bitmask. POM2 instantiates exactly one, the NMOS `TYPE_SCC8530`,
and folds the mask away: no Z-Bus accessors (`SET_Z80X30`), no extended read
or WR7' (`SET_ESCC`/`85C30`), 3-byte receive FIFO and **one-slot** transmit
buffer. Every folded branch is marked at its site, so re-widening the port to
the ESCC is a matter of un-folding, not of re-deriving.

One consequence is worth knowing before debugging against the datasheet:
after the fold, `scc_register_read`'s NMOS remap is unconditional, so RR4-RR7,
RR9 and RR11 are permanently images of RR0/RR1/RR2/RR3, RR13 and RR15. There
are no handlers for them because they are unreachable, not because they were
skipped.

**Two pin orderings, both exposed.** `readAbDc`/`writeAbDc` (A1 = A//B,
A0 = D//C) and `readDcAb`/`writeDcAb` (the other way round), matching MAME's
`ab_dc_*` and `dc_ab_*`. The Workstation Card wires the first: `$7500` is
channel B control, `$7502` channel A control, `$7503` channel A data. That is
how the ROM dump identified itself — `LDA #$03 / STA $7502 / LDA $7502` is a
poll of **RR3**, the interrupt-pending register, which exists in channel A
only, and `$10`/`$30` written to `$7500` are Reset External/Status and Error
Reset.

**Where the abstraction level changes.** The register file, the interrupt
block and the status bits are LLE, verbatim. The wire is **byte-granular**:
MAME shifts individual bits through `device_serial_interface`, POM2 holds a
byte in the shift register for exactly the frame time the same programming
would take (`frameHalfBits` reproduces the `set_data_frame(1, data, parity,
stop)` call, in half-bit units so 1.5 stop bits lands on a boundary) and hands
it over whole at the end. Nothing on an Apple II can observe the difference,
and the seam is where a host-side LocalTalk endpoint wants to attach anyway.
→ [docs/lle_vs_hle.md](docs/lle_vs_hle.md)

**`tick()` counts PCLK, not CPU cycles.** Every rate in the chip descends
from PCLK, so making the caller scale is the only way to keep the BRG
arithmetic identical to the datasheet's `TC = clock / (2 × rate × mode) − 2`.
Both internal clocks run on an exact integer accumulator — `acc += ticks ×
rate`, then whole periods drawn out of it — so a caller that ticks once per
4096-cycle chunk loses no edges and accumulates no drift against one that
ticks every cycle. The Zero Count timer draws them with a modulo and raises
once: it can be programmed faster than the caller's tick granularity (time
constant 0 is PCLK/2), and since its interrupt is a level in RR3 rather than
a counter, N raises and one raise are indistinguishable.

**SDLC is the one part that is NOT a MAME port.** MAME does not model it —
`do_sccreg_wr4` logs *"SDLC - not implemented"* and every CRC reset code in
WR0 is a no-op there — so POM2 models it from the Zilog SCC/ESCC user manual
(UM010902) instead. That is a **deliberate, documented deviation from the
MAME-is-the-oracle rule**, and every site that does it is marked
`SDLC (datasheet, not MAME)` so the boundary is visible while reading.

What is modelled is what a byte seam can carry: frame delimitation, the Tx
Underrun/EOM latch that closes a frame, Sync/Hunt and Enter Hunt, address
search against WR6 with the `$FF` broadcast, Send Abort, End Of Frame with its
residue code as a special receive condition (so the FIFO locks until Error
Reset), and the CRC as a *guarantee* rather than two bytes on a wire —
`receiveFrame` takes a `crcError` flag for a caller that wants to present a
bad FCS. What is not modelled is what only exists *between* the bytes: bit
stuffing, the flag patterns, and the FM0/DPLL line coding. No register can see
any of it.

One number is worth knowing because it is easy to inherit wrong: **an SDLC
byte is exactly its data bits.** MAME's `update_serial` always hands diserial
a start bit, which is harmless there because MAME does not do SDLC at all;
carrying that over would have made every byte 9/8 too long and put a
230.4 kbit/s LocalTalk driver 12 % off. `frameHalfBits` special-cases it.

**It works, and the proof is not a unit test.** With SDLC in place, the
Workstation Card's own firmware — Apple's LAP driver, on the card's 65C02 —
starts putting real LocalTalk frames on the wire: `0B 0B 81` is **lapENQ**,
LLAP node-address acquisition, and once the node has claimed `$0B` it
broadcasts `FF 0B 84` and short DDP datagrams `FF 0B 01 00 06 …`. The chip's
address filter follows: WR6 ends up holding `$0B`. `workstation_card_smoke`
pins that.

Two things learned by watching it that are worth not rediscovering:

* **The card disables its receiver while it transmits** — LocalTalk is
  half-duplex — and re-enables it afterwards. A host-side endpoint that
  answers a frame must wait for WR3 D0 to come back, not just for an
  inter-frame gap; a reply 100 cycles after the frame closes is dropped
  because `receiveFrame` correctly finds the receiver off.
* **Answering lapENQ with lapACK does not (yet) move the card off `$0B`.**
  The frame is accepted — the FIFO fills and the interrupt fires — but the
  driver keeps enquiring. Whether that is a timing window, a missing status
  bit, or simply what this firmware does with no real network is open.
  → `TODO.md`

**One MAME divergence deliberately kept.** On receive overrun MAME writes the
offending byte into the slot the write pointer is parked on and sets Overrun
in the *error* FIFO, but never advances past it — so that slot is unreachable
and RR1 D5 never surfaces through a data read. Real silicon discards the byte
and reports the error. POM2 reproduces MAME, and `scc8530_smoke` pins that
choice so a future change to it is deliberate.

Pinned by `tests/scc8530_smoke_test.cpp` (`scc8530_smoke`): reset values, the
two-step register pointer including Point High, the card firmware's RR3 poll,
BRG arithmetic (TC 10 → 9600 baud at 3.6864 MHz ×16), byte-time-accurate
local loopback, the receive FIFO, vector modification + IUS + the MIE gate,
Zero Count, both pin orderings, and the WR9 reset commands.

**And pinned against real firmware**, which is the stronger of the two.
`scc8530_workstation_firmware` runs Apple's 341-0358-A ROM — the Workstation
Card's own 65C02 image — until it finishes its power-on self-test and
configures the chip. The POST is not a register poke: it is a **255-byte
loopback ping-pong on both channels inside a fixed 8000-poll budget**, so it
fails unless the transmit timing, the TBE/RxCA status bits and the one-slot
transmit buffer all behave together. It passes, and the firmware goes on to
leave the chip in SDLC at **230400 bit/s** — LocalTalk, configured by Apple's
driver against POM2's chip.

Two facts about the card fell out of that test rather than out of reading the
dump, and they are why it is worth its weight: the POST's fixed budget puts a
**ceiling** on the card CPU relative to the chip clock (it passes up to 2.0 MHz
against a 3.6864 MHz SCC and fails from 2.05 MHz — an upper bound, not a
measurement of the real rate), and `3686400 / (6 + 2) / 2 = 230400` with the
firmware's own divisor is what identifies the crystal exactly. The harness borrows `Memory` in
flat test mode and shims the I/O page by decoding effective addresses around
each step — deliberately not a card emulation, and documented as disposable in
the test's header. → [printer plan 2 § 5.2](docs/printer_plan_2.md#52-the-memory-map-the-dump-implies)

### FujiNet (SP-over-SLIP relay)

[FujiNet](https://fujinet.online/) is an ESP32 peripheral whose headline
feature is the **`N:` network device**: a deported TCP/IP stack the guest
drives with simple commands, so an Apple II gets HTTP(S), TNFS, FTP, SSH,
Telnet and a JSON parser without running a byte of TCP/IP itself. On the Apple
II **every** FujiNet function is a SmartPort unit — block storage, `N:`, the
clock, the printer, the modem, CP/M — which is why POM2 relays *one* protocol
and gets all of them, rather than porting six devices.

`FujiNetCard` is a **relay, not an emulation**. It presents a SmartPort
controller to the guest and forwards every call verbatim to a real FujiNet.

**Sources.** MAME has no FujiNet device, so the project's usual "MAME = source
of truth" rule does not apply; this is stated in the file headers so the
deviation is deliberate rather than sloppy. The references are the FujiNet wiki
page *"Apple II SP over SLIP"* (revision of 2025-01-25, normative for framing
and the request/response tables), the FujiNet fork of AppleWin
(`source/SmartPortOverSlip.cpp`, `source/devrelay/**`,
`firmware/SPoverSLIP/spoverslip.s` — GPL-2.0-**or-later**, so GPLv3-compatible;
consulted, not copied), and the Apple IIc Technical Reference ch. 6 / IIgs
Firmware Reference ch. 7 for the call convention the spec supplements.

**Four layers, four concerns.**

| File | Concern |
|---|---|
| `SlipFramer.h` | SLIP encode/decode. No I/O, no threads, no protocol. |
| `SpTransport.h` + `SpTcpTransport.cpp` / `SpSerialTransport.cpp` | Byte pipe. TCP (POM2 listens on `127.0.0.1:1985`, a FujiNet *desktop build* connects in) or USB CDC-ACM (POM2 opens a serial device, a *physical board* answers). |
| `SpOverSlipLink.*` | Session: sequence numbers, the bounded round trip, device enumeration, the peer-watching worker. |
| `FujiNetCard.*` | `SlotPeripheral`: synthetic slot ROM, the `$C0n2` trap, RAM/register marshalling. |

**The trap.** The synthesised slot ROM does almost nothing — both entry points
funnel into the same four-instruction tail: `LDA #$65` (or `#$66` for ProDOS) /
`STA $C0n2` / `CMP #$01` / `RTS` (a `SEC`/`BCS` preamble picks the magic byte). `deviceSelectWrite` sees that store and does everything on
the host: decode the parameter list out of emulated RAM, **rewrite the return
address on the stack** past the three inline bytes a SmartPort call carries, do
the round trip, write the response back into RAM, set A/X/Y. The trailing
`CMP #$01` turns the status byte into the carry flag both conventions expect.

**Two things that will bite anyone touching this code:**

- **The stack index must wrap inside page 1.** The AppleWin fork's `regs.sp` is
  already a full `$01xx` address, so it indexes `mem[regs.sp + 1]`. POM2's
  `getStackPointer()` is the 8-bit register, so `0x0100 + ((sp + 1) & 0xFF)` is
  mandatory. Written without the mask, a call made with SP near the bottom of
  the page writes the fixed-up address to `$0200` — corrupting unrelated memory
  and leaving the stale address on the stack. Pinned with `SP = $01`.
- **A buffer inside `$C0xx` is refused.** Marshalling goes through
  `Memory::memRead/memWrite` (mandatory: only the real dispatcher knows whether
  a ProDOS buffer currently resolves to main or aux RAM), and "reading memory"
  in the I/O page toggles soft switches. A malformed parameter list would
  otherwise flip video mode or bank state as a side effect.

**Boot.** The card's ROM reads block 0 of unit 1 to `$0800` and runs it. When
no FujiNet answers it **continues the autostart slot scan** (`JMP $FABA`)
instead of erroring — without that, a FujiNet card in slot 7 would break
booting from the Disk II in slot 6 whenever the FujiNet is not running. Slot 7
is the default precisely because the //e scans it first, so a machine with a
FujiNet attached boots straight into its CONFIG.

**Enumeration** is the SmartPort daisy-chain sweep: INIT unit 1, 2, … until one
answers non-zero. *Deliberate divergence from the reference:* the AppleWin fork
registers a device even on the iteration whose INIT failed (it inserts before
testing its own `still_scanning` flag), so its count runs one high; POM2
registers only units that answered `$00`. `sp_over_slip_link_test` keeps that
from being "fixed" back into a bug.

**Threading.** `transact()` blocks the CPU thread under `stateMutex` — the
emulated 6502 is parked inside its `JSR` for the whole round trip, so this is
what the hardware does too, and the alternative (a completion register the ROM
polls) would change the guest-visible protocol and break "any FujiNet software
works unmodified". Bounded by a **250 ms** default timeout: ~50× headroom over
a real USB round trip, and a dead peer costs one dropped frame rather than a
full second. Only peer acquisition and enumeration live on the worker thread.

`SpOverSlipLink` has **two** peer-teardown entry points on purpose —
`peerLostLocked()` for callers already holding `callMtx_` (that is `transact()`
itself) and `handlePeerLost()` for everyone else. `std::mutex` is not
recursive, and taking it twice there deadlocks the CPU thread with the 6502
parked mid-SmartPort-call.

**Reset and rewind.** `onReset()` bumps the sequence number (so a response in
flight for the pre-reset request cannot be mistaken for the next answer) and
sends `Control` code `$00` to each device, which is what the spec asks the
Apple II side to do so a modem drops its connection and a printer ejects a
partial page. **Rewind is the honest limitation**: the peer is a live external
device, so rewinding past a `WriteBlock` does not un-write its SD card and
rewinding past an HTTP POST does not un-post it. The ring keeps working; the
card simply does not roll back with it, and `loadSnapshotState` only
resynchronises the link — deliberately *not* `notifyGuestReset()`, because the
user rewound, the machine did not reset, and hanging up somebody's modem
because they scrubbed the timeline would be wrong.

**Not supported: //c-class.** Their forced INTCXROM masks all slot ROM. On a
real //c the FujiNet *is* the SmartPort on the disk port, so the correct
integration is to hang the relay off the on-board `$C500` hole — separate work.

**Printer tap.** Bytes the guest WRITEs to the peer's printer unit are also
spooled to POM2's `ImageWriter`, through the same
`bytesWritten()`/`drainSpoolFrom()` contract `PrinterCard` and `GrapplerCard`
use, so `pumpImageWriter`'s shared `printerFeedCursor` handover rules apply
unchanged. Ranked between the parallel cards (which outrank it) and the SSC tap
(which it outranks). The peer still prints its own copy — this is a tap.

The unit is identified by its DIB **name**, not its type byte: the firmware's
`iwmPrinter::create_dib_reply_packet` (lib/device/iwm/printer.cpp:32) sets
`dib.type = SP_TYPE_BYTE_FUJINET_MODEM`, so **the printer advertises itself as
a modem**. That is an upstream copy-paste bug; the name ("PRINTER") is right.
The correct type byte ($14) is accepted too, so this survives an upstream fix,
and `fujinet_card_smoke_test` reproduces the bug so the workaround cannot be
tidied away by someone who has not met it.

**Helper process.** POM2 can launch a FujiNet desktop build itself
(`ChildProcess`, `FujiNetCard::startHelper`) rather than making the user start
one by hand, and terminates its whole **process group** on exit — killing only
the direct child leaves grandchildren holding the loopback port, which is what
would make the *next* POM2 session fail to bind 1985. POM2 does not touch the
helper's `fnconfig.ini` (WiFi credentials live there, and the firmware's Apple
default for Bus-over-IP is already 127.0.0.1:1985). Vendoring the firmware into
POM2's build was considered and rejected — see docs/fujinet_plan.md § 8.

**Driving it against a real peer — what bites** (measured 2026-08-21, against
the `fujinet-go-apple2-desktop` firmware serving a TNFS-hosted image):

- **250 ms is not enough for network-backed media.** `SpOverSlipLink`'s
  default response timeout (`kDefaultTimeoutMs`) is sized for a FujiNet
  answering out of its own SPIFFS — booting its `autorun.po` never comes close
  to it. A disk mounted from a TNFS server travels Apple → POM2 → firmware →
  the internet → back, and every block read blows straight through 250 ms: the
  card reports `kSpNoDevice`/IO error, its boot block never lands, and the
  guest sees **`FN ERROR`**. Raise it per slot with
  `fujinet_timeout_ms_slot<N>` (accepted range 50–5000; 3000 made a TNFS boot
  read reliably).
- **`FN ERROR` is OUR message, not BASIC's.** The card ROM prints it at
  `$Cn42` when the boot read fails (`buildRom`, text stored reversed at
  `$CnF0`). Seeing it means the `JSR $Cn60` came back with carry set — no
  peer, or a read that failed — *not* that the guest mistyped `PR#n`.
- **A network-backed SmartPort call freezes the whole emulator.** `transact()`
  runs on the CPU thread inside the call, under `stateMutex` (deliberate — see
  the threading note in `SpOverSlipLink.h`). While a read is in flight neither
  the UI nor the AI control server can take the lock, so POM2 looks hung for
  the duration. At the 250 ms default that is invisible; at 3000 ms over a real
  network it very much is not.
- **Attach the peer BEFORE booting from the card.** The //e autostart scans
  slot 7 at power-on; with no peer yet the card correctly steps aside and lets
  the scan carry on to slot 6, but a later `PR#7` then prints `FN ERROR`
  instead of booting. The order that works: reset to the BASIC prompt, attach
  the FujiNet, *then* `PR#7`.
- **The desktop peer is a shared library, not a daemon.** The "FujiNet Go
  Apple II" bundle ships `libfujinet.dylib` plus a runtime tree
  (`fnconfig.ini` + `SD/` + `data/`); the entry point is
  `fujinet_desktop_start_runtime(root, config, sd, data, port)` (non-zero =
  started), with `fujinet_desktop_stop_runtime()` to match. Its stock
  `fnconfig.ini` already carries `[BOIP] host=127.0.0.1 port=1985`, which is
  exactly what this card listens on. Do NOT poll
  `fujinet_desktop_copy_recent_log()` on a timer to tail its log — doing so
  killed the runtime a few seconds after every start, right after it answered
  the device enumeration.
- **Three POM2 bugs used to stand between the guest and the FujiNet.** All
  fixed 2026-08-21; recorded because each was invisible from the guest side
  and looked like a broken peer.
  - **`CONTROL` went out without the control list's 2-byte length prefix.**
    The peer skips exactly `11+2` bytes before reading the list, so a list
    shorter than two bytes ran its iterator past the end of the packet: it
    threw `std::length_error`, did not catch it, and **aborted the whole
    FujiNet process**. That was every "the firmware keeps dying" symptom in
    this subsystem. A longer list did not crash it but lost its first two
    bytes to the length field, which is why CONFIG showed empty host and
    drive slots while the peer's web UI showed them populated. Fixed in
    `SpOverSlipLink::control`, pinned by `sp_over_slip_link`.
  - **The DIB name arrives malformed from upstream.** `disk.cpp:106` builds
    it as `"FUJINET_DISK_" + std::to_string(disk_num)` where `disk_num` is a
    `char` holding an ASCII digit, so `std::to_string` promotes it to int and
    the device that means to be `FUJINET_DISK_0` calls itself
    **`FUJINET_DISK_48`**. Guest software looks for the exact name, so NETCAT
    printed "FUJINET_DISK_0 NOT FOUND" and stopped. Repaired in the relayed
    status (`repairDibName`), narrowly enough that it evaporates when
    upstream fixes the `to_string`.
  - **`kMaxUnits` was 8, and 8 is not a FujiNet.** Its SmartPort chain is the
    disk slots FOLLOWED BY the Fuji control device, `NETWORK`, the clock, the
    printer and CP/M. The sweep stopped after the disks, and because the
    guest's "how many devices?" is answered locally from that count the guest
    never probed further either — so every non-disk function was invisible.
    Raised to 32; the sweep still stops at the first unit that does not
    answer, so a short chain costs nothing.
  Verified end to end afterwards: NETCAT reports `NET DEV IS 11` and reaches
  `CONNECTED to N:HTTP://THEOLDNET.COM/`, and the panel lists all 13 devices.
- **`POM2_TRACE_FUJINET=1` traces every call, and peer death is now loud.**
  Three moving parts in two processes means the only question that matters
  when something breaks is which one went quiet, and a peer dying used to
  slip past as one INFO line among hundreds. The loss is now a WARNING
  carrying the session's lifetime and how much it served —
  `peer LOST after 22 s — 40 call(s) served, 0 timeout(s)` — because a peer
  that dies after two calls is a different bug from one that dies after ten
  thousand. That line plus the per-call trace localised the printer-unit
  crash below in a single run.
- **POM2's own guest-reset broadcast was killing the peer.**
  `notifyGuestReset()` sends Control $00 to every device — the courtesy the
  spec asks for, so a modem drops its line and a printer ejects its page. The
  printer unit ABORTS on it (below), and POM2 sends that broadcast on EVERY
  guest reset, so every Ctrl-Reset and every boot killed the FujiNet a moment
  later. The guest then reported whatever it was doing when the corpse
  stopped answering — "connection error", "FujiNet not found", a browser that
  loads and then cannot fetch — and none of those point at a reset that
  happened seconds earlier. The printer is now skipped in the broadcast, and
  a guest-issued reset to it is answered locally: `isPrinter()` matches on the
  DIB name, so a fixed firmware needs no change here.
- **A `CONTROL` to the peer's PRINTER unit kills it** (upstream). The packet
  is byte-identical in shape to the ones the neighbouring units answer
  normally, yet the peer aborts out of `Request::from_packet`. Same unit
  whose DIB already carries the modem's type byte. Nothing to fix here — but
  expect it, and expect the trace to name it.
- **POM2 serves `N:` ITSELF now** (`FujiNetNetDevice`, on by default;
  `fujinet_builtin_network<slot> = false` turns it off). The peer's own `N:`
  is inert on the desktop build (below), so relaying faithfully to it means
  the guest can never fetch anything — this is the difference between a
  machine that browses and one that does not. Disks, CONFIG and the clock
  still go to the peer.
  It is a real device to the guest, not a shim over one: it answers the DIB
  as "NETWORK" and is counted in the unit-0 device count, so a guest finds it
  **even with no peer attached at all** — which matters, because the peer
  dies easily and the device list is cleared when it does.

  **Where it sits in the chain is not a free choice.** A SmartPort chain is
  contiguous `1..N` — what unit 0 answers is a COUNT, not a highest-unit
  number — and every standard chain walk, including POM2's own
  (`SpOverSlipLink::enumerateDevices`), stops at the first unit that answers
  "no device". Parking the device at a fixed unit 11 therefore made it
  invisible to the very scan meant to find it: with no peer the guest probed
  unit 1, got nothing, and never looked further. So: the peer's NETWORK unit
  when it has one (overridden in place), otherwise the slot just past its
  last device. Three rules follow, each of which was a live bug —
  · never answer for a unit the peer really holds (a remembered unit plus a
  peer that came back with a different chain = ProDOS reporting an I/O error
  on a good volume);
  · never claim a unit while a CONNECTED peer is still enumerating — its list
  is empty for the whole sweep and it is about to publish;
  · hold the unit steady while the guest has a session open, so a peer dying
  mid-fetch does not move the device under its feet.
  Two things cost debugging time and are worth knowing. The devicespec is NOT
  the whole control list: the guest sends aux1 (open mode), aux2
  (translation), THEN the spec — `04 00 4E 3A 68 74 74 70 …` off the wire
  from the FujiNet Contiki browser — and taking the list verbatim put two
  binary bytes in front of every URL. And guest read loops end when STATUS
  stops saying "connected", so that flag has to track "bytes remaining", not
  "socket open". Scope is HTTP over plain TCP, which is what the retro web
  serves; no TLS, no SSH, no JSON.

  **Every wait is bounded, and one deadline covers the whole exchange** — DNS,
  connect, request and body together. This *used* to be the difference between
  a working window and a frozen one: the fetch ran on the CPU thread inside
  `runCpuSlice`, which holds `stateMtx`, so an unbounded fetch was an
  unpaintable window whose own Stop button was out of reach. It runs on its
  own guarded worker now (`FujiNetNetDevice.cpp:364-372`, tag `FujiNetN`), and
  the deadline stayed for the reason that outlives the lock: a fetch that
  cannot end is a socket and a thread that never go away. `cancel` is noticed
  within one `kWaitSliceMs = 100` slice (`FujiNetNetDevice.cpp:169`). Two traps, both
  measured rather than assumed: `SO_SNDTIMEO` does **not** bound `connect()`
  (macOS, 192.0.2.1 — 75 s against an 8 s request), and a per-recv timeout
  never bounds a *transfer*, so a server drip-feeding one byte just inside it
  holds on for ever. `getaddrinfo` is unbounded too and there is no portable
  async resolver, so the lookup runs on its own thread and is abandoned if it
  overruns. Finally, a short read is reported as an ERROR, never as a short
  page: a truncated document the guest cannot distinguish from a whole one is
  the one failure nobody can diagnose from the Apple II side. Pinned by
  `fujinet_net_device`, including a stalled server and a blackholed host.
  Verified: the FujiNet Contiki browser fetches theoldnet.com through it,
  77 169 bytes, with no peer running.
- **The desktop firmware's `N:` device answers "connected" without
  connecting.** The Apple II gets `CONNECTED to N:HTTP://…` and no outbound
  socket is ever opened, watched live on the peer's descriptors. Not the
  relay: the same firmware opens real TCP for TNFS on the same machine. Its
  WiFi is a `DummyWiFiManager`. `N:` wants a real board over USB.
- **Its web admin UI (127.0.0.1:64001) is the reliable way to mount media**,
  far more so than driving the guest-side CONFIG program: `/browse/host/N`
  walks a TNFS host and `?action=newmount&slot=N&mode=r` fills a drive slot.
- **A host-side per-application firewall can look exactly like a relay bug.**
  On a Mac running Little Snitch, every outbound *UDP* datagram from the
  firmware process was dropped while TCP was allowed. TNFS tries TCP first and
  falls back to UDP, so the visible symptom was a 28 s stall and
  "File System error" — with POM2 nowhere in the path. Isolate it by browsing
  from the firmware's own web UI, which never touches the emulator.

CLI: `--fujinet[=PORT]`, `--fujinet-serial[=DEVICE]`, `--fujinet-slot N`.
Panel: View ▸ FujiNet. Design notes and the remaining phases:
[docs/fujinet_plan.md](docs/fujinet_plan.md).


### Network backends

`NetworkBackend.h` — the host-side transport that carries raw Ethernet
frames for the two Uthernet cards. Shape follows AppleWin's
`source/Tfe/NetworkBackend.h` (GPL2+).

**Who actually needs it.** Only the paths that move *frames*:

| Card | Mode | Needs a backend? |
|---|---|---|
| Uthernet I (CS8900A) | all | **yes** — it is a plain NIC |
| Uthernet II (W5100) | TCP, UDP | **no** — host sockets |
| Uthernet II (W5100) | MACRAW, IPRAW | yes |

That table is the single most important thing about this subsystem: the
Uthernet II is fully functional for IRC / telnet / FTP with **no backend
at all**, because its W5100 is a TCP/IP offload engine, not a NIC — POM2
maps its four sockets straight onto host sockets. Only the Uthernet I
(whose guest software — IP65, Contiki, ADTPro-ethernet — carries its own
stack and hands the card whole frames) is hard-gated on a transport.

**Platform coverage** (2026-08-01). Host sockets — hence Uthernet II
TCP/UDP, the SSC telnet bridge and the AI control server — now work on
**Windows** as well, through `src/SocketCompat.h` (see [§ Host
sockets](#host-sockets-posix--winsock)). The libslirp backend is still
Linux/macOS only, so **Uthernet I has no host transport on Windows**:
vcpkg does carry a libslirp port, but `SlirpNetworkBackend`'s poll loop
is written against POSIX `poll()` over the fds libslirp returns, and that
port cannot be verified without a Windows libslirp build to test against.
CMake therefore does not even look for libslirp on WIN32 — a documented
absence beats a wall of missing-header errors.

Three implementations:

- **`NullNetworkBackend`** — always available, `isValid()` false. Frames
  are dropped, nothing arrives. Keeps the cards pluggable and
  software-detectable on a build with no host networking.
- **`LoopbackNetworkBackend`** — `transmit()` feeds `receive()`. Drives
  both pinned smoke tests (no real network in CI) and is selectable at
  runtime as a self-test mode.
- **`SlirpNetworkBackend`** (`SlirpNetworkBackend.h/.cpp`) — libslirp
  user-mode NAT. **Optional build dep**, gated on `POM2_HAVE_SLIRP`
  which CMake sets when `pkg-config` finds `slirp`.

**Why libslirp and not TAP/pcap.** Both classic ways to bridge Ethernet
need root (`CAP_NET_ADMIN` / `CAP_NET_RAW`). libslirp terminates the
guest's IP inside our process and re-opens ordinary user-space sockets:
no privileges, no host configuration, identical behaviour in CI. The
cost is slirp's documented limits — outbound only (no inbound without
explicit port forwarding), ICMP only where the host allows unprivileged
ping sockets, and the guest is unreachable from the LAN.

Virtual network (libslirp defaults, same as QEMU `-net user`):

```
10.0.2.0/24   the virtual network
10.0.2.2      virtual router / gateway (the host)
10.0.2.3      virtual DNS server
10.0.2.15     what the DHCP server hands out
```

Configure IP65 / Contiki with `10.0.2.15 / 255.255.255.0 / 10.0.2.2 /
10.0.2.3` if you skip DHCP.

`resolveMac()` synthesises `52:55:<ip bytes>`, which is exactly what
libslirp's own ARP responder replies for its virtual network (see
libslirp `src/arp_table.c`), so an IPRAW frame is well-formed without
paying an ARP round-trip the guest never observes.

**Threading.** Backends are driven from the CPU thread under
`stateMutex`, via `SlotPeripheral::advanceCycles`. Nothing may block:
`receive()` returns <= 0 when idle and `poll()` uses a zero timeout.
Both cards throttle to one `poll()` per ~2048 CPU cycles (≈ 500 Hz),
well inside any Ethernet deadline and cheap enough to leave
unconditional in the cycle hook.

Settings key `ethernet_backend`: `slirp` (default) | `loopback` | `none`.
Takes effect on the next plug (profile switch or Slot Config change).

### Uthernet I (CS8900A)

`UthernetCard.h/.cpp` (card, catalog key `uthernet`) +
`Cs8900aDevice.h/.cpp` (chip). Port of MAME
`src/devices/bus/a2bus/uthernet.cpp` (BSD-3, R. Belmont) over
`src/devices/machine/cs8900a.cpp` (GPL-2.0+, Rhett Aultman, from Spiro
Trikaliotis' VICE model). POM2 is GPL-3.0, which GPL-2.0+ permits. Every
function in `Cs8900aDevice.cpp` carries the MAME line range it mirrors.

The card is a ~40-line shim, exactly as in MAME: `$C0nX` forwards the low
nibble straight to the chip, and that is the *whole* address decode. There
is no slot ROM — a2RetroSystems left the CS8900A's boot-PROM interface
unpopulated — so `slotRomRead` keeps the SlotPeripheral `$FF` default and
every driver is loaded from disk. Presenting a signature here would make
ProDOS probe a device that does not exist.

**Chip shape.** 16 bytes of I/O space hiding a 4 KB indirect register
file (the *PacketPage*). Set a pointer at `$C0nA/B`, read/write the data
window at `$C0nC/D`; pointer bit 15 enables auto-increment (by **one**,
not two — odd pointers are legal). Frames are not DMA'd: a received frame
lands at PacketPage `$0400` and is read out byte-at-a-time through the
RXTXDATA window at `$C0n0/1`.

**Transmit is a four-step handshake** (`cs8900a.cpp:210-215, 839-904`)
and skipping a step must emit nothing — pinned:

1. write TxCMD (`$0144`) → `GOT_CMD`
2. write TxLength (`$0146`) → `GOT_LEN`, if `4 <= len <= 1518`
3. **read** BusST (`$0138`) and observe `Rdy4TxNOW` → `READ_BUSST`
4. push `len` bytes through RXTXDATA; the last one releases the frame

**Receive is polled.** Reading RxEvent (`$0124`) pops the next accepted
frame into PacketPage and flips to `GOT_FRAME`. Reading RxEvent *again*
before the payload is drained is an "implied skip" that discards it —
real hardware behaviour, modelled. Payload readback order is the
datasheet's: RxStatus H/L, RxLength H/L, then payload L/H per word.

**The ISQ had to be synthesised** *(2026-09-07)*. The card's interrupt-status
queue at `$C0n8` was hard-wired to 0, so the driver idiom "read `$C0n8` until
it returns 0" saw nothing and **no frame was ever noticed**; `TxEvent` never
signalled `TxOK` either. Both are synthesised from the event registers now.
The inbound queue was also wrong in the direction that hurts: 4096 *entries*
(~6 MB, minutes of backlog) dropping the **oldest**, where the chip has ~4 KB
and drops the **arriving** frame while counting `RxMISS` + BufEvent. It is now
byte-capped and drops the newcomer. And a reset reloaded `RxCTL` without
re-decoding the address filter it drives. The `readRxBuffer` advance asymmetry
is **not** a defect — it is MAME's order, deliberate and pinned; `Skip_1` and
the PacketPage frame-buffer window are left as MAME parity, with no oracle to
arbitrate them.

**Deltas from MAME**, all deliberate:

- MAME is *pushed* frames by `device_network_interface::recv_start_cb`
  (`cs8900a.cpp:1483-1512`). POM2 has no such bus, so `pumpBackend()`
  pulls from the `NetworkBackend` on the cycle hook and applies the same
  `shouldAccept()` pre-filter before queueing (bounded to 32 frames per
  call so a busy link can't stall the CPU thread inside one
  `advanceCycles`).
- MAME's `assert()`-heavy PacketPage macros become clamped accessors: a
  mis-decoded `$C0nX` must never take the emulator down.
- `machine().side_effects_disabled()` has no POM2 analogue; `peek()`
  gives the debug panel the same side-effect-free read.
- The multicast hash filter needs MAME's `util::crc32_creator::simple`;
  `crc32Ieee()` is a local standard IEEE 802.3 CRC-32 (reflected, poly
  `0xEDB88320`), which is the same function.

**Snapshot**: the whole 4 KB PacketPage rides along. That sounds heavy
for a 60 Hz rewind ring until you notice `RewindBuffer.cpp:20-88`
XOR-deltas it — a mostly-idle NIC costs a handful of bytes per frame. The
*inbound frame queue* is deliberately NOT saved: it mirrors host network
state that has moved on by the time a rewind replays, and restoring it
would re-deliver packets the guest already consumed.

Pinned by `uthernet_cs8900_smoke`.

### Uthernet II (W5100)

`UthernetIICard.h/.cpp` (card, catalog key `uthernet2`) +
`W5100Device.h/.cpp` (chip). **MAME has no W5100 device** — its Apple II
Ethernet support stops at the Uthernet I — so the reference is AppleWin
`source/Uthernet2.cpp` + `source/W5100.h` (GPL-2.0+, Andrea Odetti),
cross-checked against the WIZnet W5100 datasheet v1.2.8 and the Uthernet
II manual (2018-11-17). Citations in `W5100Device.cpp` are AppleWin line
numbers.

**This is not a packet-level model, and that is the point.** The W5100 is
a TCP/IP *offload engine*: the guest writes a destination address and
port into registers, issues `CONNECT`, then pushes payload at a ring
buffer. All protocol work happens inside the chip. That maps one-for-one
onto host BSD sockets — so each of the four sockets in TCP or UDP mode
owns a real non-blocking host socket, and the card needs **no Ethernet
backend at all** for the traffic anyone actually cares about.

Memory map (32 KB, reached through the indirect window):

```
$0000-$002F  common registers (mode, gateway, subnet, MAC, our IP,
             retry timing, RX/TX memory-size allocation)
$0400-$07FF  four 256-byte socket register banks (S0..S3)
$4000-$5FFF  8 KB TX buffer, carved between sockets by TMSR
$6000-$7FFF  8 KB RX buffer, carved between sockets by RMSR
```

Bus decode: **only A0 and A1 reach the card**, so the four registers
repeat four times across `$C0nX`. The canonical group is `$C0n4` mode,
`$C0n5` addr-hi, `$C0n6` addr-lo, `$C0n7` data. The aliasing is real
hardware behaviour and drivers rely on it, so POM2 masks rather than
range-checks. Auto-increment (mode bit 1) wraps *inside* each 8 KB buffer
instead of spilling into the next region (manual p.12).

**Per-protocol RX header.** The chip prepends a header to received data
in the RX ring, and its size is what `SN_RX_RSR` counts:

| Socket mode | Header |
|---|---|
| TCP (`ESTABLISHED`) | none — raw stream |
| UDP | source IP (4) + source port (2) + length (2) |
| IPRAW | source IP (4) + length (2) |
| MACRAW | length (2) — and it **includes the two length bytes** |

**Virtual DNS** (`Uthernet2.cpp:32-37`) is an AppleWin extension the real
card does not have: bit 3 of a socket's protocol nibble means "the
destination is a hostname". The length-prefixed name lives at socket
offset `$2A-$FF`, `OPEN` resolves it into `DIPR`, and software detects
the extension by reading `PTIMER` as 0. POM2 keeps it — it is what lets a
guest reach `irc.libera.chat` without carrying a resolver — but **resolves
off the CPU thread**: a plain blocking `getaddrinfo()` under `stateMutex`
could stall emulation for seconds. The lookup runs on a detached thread
with a bounded `kDnsWaitMs = 120` wait; on timeout the answer still lands
in a mutex-guarded mailbox that `poll()` folds into the cache on the CPU
thread, so the guest's retry (every practical client retries a failed
connect) succeeds instantly. Toggle: `uthernet2_virtual_dns`.

**What is deliberately not implemented.** `LISTEN` is in the W5100 command
set but POM2 does not open a host listener for it: an inbound connection
cannot reach the guest through either supported transport (libslirp is
outbound-only without explicit port forwarding). It no longer just logs,
though — leaving `Sn_SR` at `SOCK_INIT` made a server driver spin forever
with nothing to time out on. `listenSocket()` (`W5100Device.cpp:509-531`)
warns, then `clearSocket(i)` (→ `SOCK_CLOSED`) and raises TIMEOUT in
`Sn_IR`: the datasheet §5.2.3 failure every W5100 server loop already
handles. Note `Sn_PORT` **is** bound on the host socket for **UDP**
(`W5100Device.cpp:385-397`; IPRAW/MACRAW never reach that path — they go
through the `NetworkBackend`, not a host socket) — and deliberately not for a TCP client, because
WIZnet drivers reuse one fixed source port and the second connect would hit
`EADDRINUSE` against the first 4-tuple's TIME_WAIT. The bind goes through
`setListenerBindPolicy()`, never a raw `SO_REUSEADDR` (`SocketCompat.h`).

**Interrupt registers, and the SEND paths the datasheet arbitrates**
*(2026-09-07)*. `Sn_IR` and the common `IR`/`IMR` did not exist — reads
returned 0 and the write-1-to-clear was dropped — so the stock WIZnet
`send()` spun on SEND_OK for ever, and a driver polling CON/TIMEOUT never
woke. They exist now, with the usual W1C semantics. Three SEND defects went
with them, all reachable from the stock driver:

* A SEND of **exactly** `Sn_TX_FSR` bytes transmitted nothing: the read and
  write pointers were masked into the ring *before* being differenced, so
  `rd == wr` read as "empty" while `Sn_TX_RD` was advanced anyway and FSR
  reported full again. That is the driver's max-throughput path.
* A SEND in a **non-transmitting** state (SYN_SENT, say) still advanced
  `Sn_TX_RD`, deleting the first request after a non-blocking connect.
* `SEND_MAC` (`$21`) and `SEND_KEEP` (`$22`) fell through to `default:` —
  UDP driven through SEND_MAC lost every datagram and the ring filled.

Also fixed: `Sn_RX_RSR` pulled a packet on *both* byte reads and therefore
tore, and `RTR`/`RCR`/`IMR`/`PMAGIC` writes were dropped.

**Declined with reasons** (reviewed, not defects): `Sn_MR` MULTI/ND is a
feature POM2 does not offer rather than a wrong answer; the PacketPage
frame-buffer window and `Rdy4TxNOW`-on-the-odd-read are datasheet-correct or
MAME-parity with no oracle to arbitrate.

**Snapshot**: only the datasheet-defined regions are saved — the reserved
holes (`$0030-$03FF`, `$0800-$3FFF`) carry nothing and would just bloat
the rewind delta. Buffer geometry is *derived*, rebuilt from RMSR/TMSR on
load rather than stored. A live TCP connection or UDP binding **must come
back CLOSED**: the peer moved on while the ring was rewound and the fd is
gone, so pretending to still be `ESTABLISHED` would hang the guest. The
raw modes carry no host state and do come back. Pinned.

Pinned by `uthernet2_w5100_smoke`, which includes a **real TCP session**
against a loopback listener the test opens itself (OPEN → CONNECT → SEND
→ RECV → CLOSE, deliberately with no `NetworkBackend` plugged, proving
the no-backend claim above).

**WASM**: there is no usable BSD-socket API in the browser, so the
TCP/UDP paths compile out and those modes stay `CLOSED` (same treatment
`SuperSerialCard` gives its telnet listener). The register model, the
rings and MACRAW/IPRAW are unaffected.

### Printer card (parallel, synthetic)

`PrinterCard` (`PrinterCard.h/.cpp`) — host-side spool that captures every
byte the Apple II "prints" through `PR#n` into a `std::vector<uint8_t>`
the UI saves to `.txt` (PDF deferred — see TODO). No PROM dump
required; the synthetic 256-byte slot ROM only does the PR#n CSWL/CSWH
hook + a 4-byte output trampoline.

Slot ROM layout (s = slot, slotHi = $C0+s):

```
$Cn00  4C 20 ss   JMP $Cn20            (skip the Pascal sig region)
$Cn05  38         Pascal 1.1 sig 1     (SEC)
$Cn07  18         Pascal 1.1 sig 2     (CLC)
$Cn0B  01         Pascal firmware rev
$Cn0C  00         Pascal device class = printer
$Cn20  A9 31      LDA #$31             ; CSWL low byte
$Cn22  85 36      STA CSWL
$Cn24  A9 ss      LDA #slotHi
$Cn26  85 37      STA CSWH
$Cn28  60         RTS
$Cn31  8D 91 c0   STA $C0(8+s)1        ; data port write
$Cn34  60         RTS
```

Data port at `$C0(8+s)0` **and** `$C0(8+s)1`: write enqueues the byte
verbatim (no high-bit strip — the UI/spoolText() does that), read returns
$FF (always ready) on every offset. Other device-select offsets ignore
writes.

Two offsets, because there are two ways in, and this section used to
describe neither correctly (it claimed offset 0 with a `!(low4 & 0x03)`
mirror mask, and a `romBankHigh_` flip that belongs to the Grappler):

* **Offset 1** is what the trampoline above writes — the `PR#n` + COUT
  path, i.e. how BASIC, DOS and the Monitor print. For a long time it was
  the only offset the card decoded.
* **Offset 0** is the data latch on the real Apple Parallel Printer
  Interface, and it is where software that drives the card DIRECTLY
  writes. Graphics software does exactly that: The Print Shop's "Apple
  Parallel Interface" driver never reads our ROM at all — character to
  offset 0, a copy to offset 2 as the strobe, poll offset 4 for ready. With
  offset 1 alone its whole `ESC G` page was dropped while offset 4 answered
  "ready", so it reported the job printed and nothing came out
  (2026-08-18).

**Offset 2 is deliberately NOT a data port**: the strobe carries a copy of
the byte, so decoding it too would double every character. Pinned by
`printer_card_smoke::testDirectDriveParallelStream`, which replays the
traced Print Shop access pattern and requires exactly one spooled byte per
character.

The full Pascal 1.1 entry block (PINIT/PREAD/PWRITE/PSTATUS at
$Cn0D-$Cn10) is **not** implemented — BASIC `PR#n` is the only
documented use case for a printer card in the POM2 software corpus,
and Pascal printer drivers were rare. Signature bytes alone are
enough to keep ProDOS's device scanner happy.

**Free-slot pick on II / II+ / //e** via the Slot Configuration panel.
It is **not** a //c/+ built-in: those profiles place a real serial
`ssc` at slot 1 ("printer port") and slot 2 ("modem port"), matching
the //c hardware (`SystemProfile.cpp:155,209`, `cfgAppleIIc /
cfgAppleIIcPlus`). POM2 *used to* substitute a synthetic parallel
PrinterCard at the //c slot-1 built-in, but that diverged from the
real //c serial printer port (and from MAME's `apple2c`) and was
reverted — see the comment at `SystemProfile.cpp:152`.

Pinned: `printer_card_smoke` — ROM fingerprint + data-port spool
semantics + CPU-driven `PR#1` + 3 COUT-style writes flow.

### Grappler+ (Orange Micro)

`GrapplerCard` (`GrapplerCard.h/.cpp`) — ROM-gated parallel printer
card. Catalog key `"grappler"`, default slot 1. Adds two things over
`PrinterCard`:

* **4 KB real ROM** (`roms/grappler_plus.bin`, also accepted:
  `roms/grappler+.bin`, `roms/grappler.bin`). First 256 B map at
  `$CnXX`; the lower 2 KB of the 4 KB EPROM are mirrored into the
  shared expansion-ROM window at `$C800-$CFFF` so Grappler-aware
  software (e.g. AppleWorks "Printer = Grappler+") finds the ROM
  fingerprint. Wrong-size or missing dumps are rejected with a log
  warning; the card falls back to a synthetic stub identical in
  shape to `PrinterCard` so `PR#n` still works.
* **Spool semantics identical to PrinterCard.** Data port at
  `$C0(8+s)0` enqueues bytes verbatim (masked to 7 bits when the S1:1
  MSB switch is open, MAME `data_latched`); the host UI saves the spool
  as `.txt` and the ImageWriter renders it as paper.

**The S1 printer-type DIP decides which dialect the firmware speaks** —
and getting it wrong is the single most confusing failure mode this card
has. Bits 2-0 of S1 read back at status bits 6-4; the firmware branches
on them. Captured from the real 4 KB dump, `^I G` (HGR screen dump) on
the same picture:

| S1 2-0 | Printer type | Bytes the firmware emits |
|---|---|---|
| 000 | Epson series (MAME's default) | `ESC A <07>` … `ESC K <18><01>` + **binary** graphics |
| 001 | NEC 8023 / C. Itoh 8510 / DMP 85 | `ESC T14` … `ESC S0280` + graphics |
| 101 | Apple Dot Matrix | `ESC T14` … `ESC G0280` + graphics |

POM2's printer is an ImageWriter II, which speaks the C. Itoh dialect:
`ESC G nnnn` with **ASCII digit** counts. Fed the Epson stream it reads
`ESC A` as "1/6 in line spacing" (no parameter), `ESC K <18>` as a ribbon
colour change, and then prints every graphics byte as a character —
32 sheets of noise in double-width, which is exactly what a real desk
with the switches set wrong would produce. So POM2 **defaults S1 to
Apple Dot Matrix (101)**, not to MAME's Epson, and exposes the switch as
*Card emulates* in the ImageWriter panel's *Printer settings* (persisted
as `grappler_printer_type`), with a warning when it is set to a dialect
this printer does not speak.

**BUSY/ACK**: see [§ ImageWriter](#imagewriter-ii-printer-host-side) —
the firmware's per-byte wait loop spins on the ACK bit, so the host
printer's input-buffer state is what throttles a printing guest.

**Bank switching is modelled.** Real Grappler+ exposes the upper
2 KB of its 4 KB EPROM via a bank-select write. POM2 mirrors this:
a data-port write with `low4 & 0x01` set raises `romBankHigh_`
(`GrapplerCard.cpp:134`); the expansion window then serves the upper
2 KB (`rom_[(offset & 0x7FF) | 0x800]`, `GrapplerCard.cpp:172`).
Any `$CnXX` **read or write** drops the bank low; the flag round-trips
through snapshot. `grappler_card_smoke` asserts both banks are
distinguishable.

**Pinned against MAME `bus/a2bus/grappler.cpp`** (2026-07-28 audit,
line ranges cited at every ported block in the .cpp): status byte
layout (`read_c0nx:699-709`), register decode incl. the A1-before-A2
IRQ priority (base `write_c0nx:547-575` + overlay `:711-745`), ROM
side effects (`read_cnxx:578-583` — bank drop + ACK-gated A6 mask;
`write_cnxx:586-591` — a bus-conflict write also drops the bank, now
modelled via `slotRomWrite`), `$C800` banking (`read_c800:123-126`,
`set_rom_bank:160-165`), S1 DIPs (`INPUT_PORTS:498-511`). The audit
fixed one silent divergence: **reset no longer clears the ROM bank** —
MAME's `reset_from_bus` (`:536-539`) and `device_reset` (`:777-787`)
touch only the ACK latch and IRQ flip-flop; the U2D bank flip-flop is
not wired to bus RESET. Deliberate divergences, documented in the code:
the 7-clock /STROBE pulse timer (`:795-808`, `:839-849`) collapses to
instant (the synthetic printer consumes at latch time, no observer);
MAME's edge-driven IRQ flip-flop is derived as the equivalent level
`ack && !disable`; and `ackEffective()` (ACK gated by host BUSY) is
POM2's back-pressure model, not MAME's (MAME reads a live centronics
/ACK line POM2 has no equivalent of).

Source: markadev/AppleII-RevEng/Orange-Micro-Grappler+ (4 KB
EPROM dump). Pinned: `grappler_card_smoke` — stub ROM fingerprint
+ data-port spool + ROM-load size gate + bank-select round-trip
(incl. reset-keeps-bank + write_cnxx drop) + S1 printer-type/MSB DIP
+ the BUSY→ACK handshake.

### Print history (`PrinterHistory`)

Printouts that outlive the session. `ImageWriter`'s own sheet stack is capped
at 32 and dies with the process; this writes every ejected sheet to
the per-user `printouts/history/` as a PNG plus a tab-separated index carrying the printer,
ribbon, paper size and raster dimensions. The ImageWriter panel lists them
newest first and puts one back on the canvas when clicked.

**Not JSON**, deliberately: POM2 has only a flat one-level JSON *extractor*
(`AiControlServer.cpp` `jsonParseValueAt`), not a document parser, and growing
one to read a few dozen index lines is not worth
it. Tab-separated text also survives a truncated final line and can be read in
a terminal.

**Four traps, all pinned by `printer_history`:**

- **Archive against the MONOTONIC eject counter** (`ImageWriter::sheetsEjected`,
  added for this), not `completedPageCount()`. The page stack is capped and
  reused, so a form-feed burst between two frames pushes sheets off it and a
  count-based archiver loses them silently. When the archiver cannot reach
  everything it logs how many it missed rather than pretending.
- **Archiving runs on every path** through `pumpImageWriter`, including the
  "no source this frame" early return: a job already in the printer's buffer
  keeps ejecting after its card is unplugged.
- **The filename counter resumes** from the index on load. Restarting it at 1
  would clobber an existing PNG and show two history rows of the same image.
- **The index is written to a temp file and renamed.** A crash mid-write
  leaves the previous index intact; an unrecognised index yields an EMPTY
  history rather than rows pointing at files POM2 cannot vouch for.

**A bad index must not delete the printouts** *(2026-09-07)*. Those last two
rules combined into a defect: `open()` ignored `readIndex()`'s result, and the
sweep that removes every PNG the index does not mention then ran behind an
*empty* index — so one truncated write or one foreign version tag turned "POM2
cannot read this file" into "POM2 deleted up to 200 of your printouts",
silently, on open. The sweep now runs **only behind an index that actually
parsed** (`PrinterHistory.cpp:163-180`); a bad one is renamed
`<index>.bad-<stamp>` (the user may want to look at it, and a fresh index is
written on the next page) and every PNG is left where it is. The writer thread
also has a liveness predicate now: if it died, the wait fails instead of
hanging the window.

Capped at 200 pages, deleting the PNGs as well as the rows — an emulator left
running must not quietly fill a disk.

### Printer sound (`PrinterSoundDevice`)

The head buzz, the carriage-return sweep and the platen motor. **Synthesised,
and for once that is not a fallback**: the floppy sounds are MAME's WAV set,
but no equivalent free ImageWriter set exists — and mikedaley/web-a2e, the
reference for the rest of the printer work, **ships no audio assets at all**
(checked, not assumed). Its `printer-sound.js` synthesises, and this is a port
of that model (MIT, Shawn Bullock).

**Why grains.** A dot-matrix impact is a short broadband NOISE click, not a
tone: the head energy sits near the basic printing frequency (~900-1000 Hz)
with a skirt to ~5 kHz and no clean fundamental. Using an oscillator is what
makes a printer emulation *sing* rather than clack, so every voice is
bandpassed noise with a wide Q.

One **grain** per printed character (11 ms, ~1500 Hz) or line feed (40 ms,
~950 Hz), and grains are **spaced along the audio timeline** instead of all
starting when the event arrived. That is the whole trick: at print rate they
overlap and fuse into the familiar continuous buzz, while a lone character is
one tick — so print density drives the texture with no extra machinery.

**Web Audio → pull mixer.** The reference schedules on an `AudioContext`
timeline. POM2's mixer pulls a mono float buffer, so the timeline becomes an
audio FRAME COUNTER the audio thread advances and the UI thread reads when it
stamps a grain — the same arrangement `FloppySoundDevice` documents for its
step cadence.

**The load-bearing detail** is the cap on how far ahead the scheduling cursor
may run (0.2 s). A full-black screen dump is tens of thousands of strikes
arriving in one UI frame; at 5 ms spacing that is *100 seconds* of buzz still
rattling long after the page is finished. With the cap the burst simply THINS.
`printer_sound` fires 20 000 strikes and asserts the noise is over in ~0.2 s.

**No `emuCycles` here, unlike the floppy** — and the printer plan's § 9 was
wrong to say otherwise. `FloppySoundSink::step` carries a cycle stamp because
the guest drives the stepper directly and disk turbo collapses the wall-clock
gaps. `ImageWriter` consumes its queue on its own wall-clock pacing
(`tick(double dt)` at the head's cps), so a job fired in at any speed still
prints at 180 cps and these events are already in real time.

### Screen dump (`PrinterScreenDump`)

"Print what is on screen", as the dot-matrix bit-image stream a period driver
would have produced. Ported from the design in mikedaley/web-a2e's
`src/js/printer/screen-dump.js` (MIT).

**The rule that makes it honest: it synthesises a WIRE FORMAT and pushes it
through the printer's real parser.** Nothing paints a page pixel. The dump
emits the same `ESC G` bytes a Grappler ROM or Print Shop drives the head
with, hands them to `ImageWriter::queueBytes`, and lets the existing bit-image
path do the work — so it obeys the ribbon, the pacing and the paper, lands in
the tray and the PDF export, and cannot drift from what a real driver sends.

The head prints a **band** at a time: 8 vertical dots packed into one column
byte, one byte per horizontal position, then a feed to the next band. The
framebuffer walk is the same for every printer in the family; only four things
differ between heads, which is why they sit in one place rather than through
the loop:

| | C. Itoh / ImageWriter / DMP | Epson FX-80 |
|---|---|---|
| Graphics command | `ESC G` | `ESC *` |
| Column count | 4 ASCII digits | 2 binary bytes |
| Top-dot bit | **bit 0** | bit 7 |
| Band feed | `ESC T 16` (16/144 in) | `ESC 3 24` (24/216 in) |

`ESC n` (9 cpi, 72 dpi graphics) is selected first so one screen pixel is one
dot and the aspect ratio survives; 16/144 in is exactly 8 dots at 72 dpi, so
bands abut with no seam and no overprint.

**Auto-invert** picks polarity from lit density: the screen is light-on-dark
and paper is the reverse, so dumping a text screen without inverting floods
the sheet.

Pinned by `printer_screen_dump`, whose central assertion is a **round trip** —
build the stream from a known framebuffer, run it back through `ImageWriter`,
and check the picture comes out. That pins scanner and parser against each
other; a dump that agreed only with itself would be a screenshot with extra
steps.

### ImageWriter II printer (host-side)

**Character ROMs (2026-08-10).** Glyphs no longer come from POM2's bundled
CP437 font. `src/ImageWriterRom.h` is GENERATED by
`tools/import_printer_roms.py` and carries ten banks — ImageWriter II
correspondence / draft / NLQ, each fixed and proportional where the hardware
had one, plus the ImageWriter I pair, the Apple DMP pair and the Epson FX-80
face — with the seven locale substitutions per bank. Consequences:

- **`ESC a n` now changes the page** (0 = correspondence, 1 = draft, 2 = NLQ,
  Table 4-1). It used to be swallowed, and "NLQ" was only the host's *pacing*
  knob. `Quality` (guest, `ESC a`) and `Speed` (host, cps) are now separate on
  purpose.
- **Proportional is proportional.** `ESC p` / `ESC P` advance by the glyph's
  own escapement over the pitch's dot unit (144 / 160 per inch), not by a
  fixed cell. `hmi_` still outranks both — an explicit motion index is the
  guest saying "move exactly this far".
- **International sets are the ROM's own substitutions**, not the nearest
  CP437 stand-in.

The CP437 font is KEPT as the fallback for anything a bank does not carry, and
as the one-file revert if the provenance question below is ever answered "no".

**Provenance.** The tables are transcribed from the dot patterns Apple
*published* in the ImageWriter / ImageWriter II Technical Reference Appendix C
(via mikedaley/web-a2e, MIT). They are not chip dumps; the transcription is
MIT, the typeface design is Apple's. Same class as the AppleWin SSI263 phoneme
blob — see `docs/lle_vs_hle.md` and `docs/printer_plan.md` § 3.

**Two generator traps**, both of which compiled clean and printed wrong:
a backslash ending a `//` comment continues it onto the next line (`$5C` is
`\`), which silently dropped one glyph per bank into a fixed-size array; and
the row comments contain `{` and `}`, which broke a brace-walk parser.

**Four heads (phase C).** `IwModelProfile` is a four-row table —
ImageWriter II, ImageWriter I, Apple DMP, plus the Epson FX-80 (whose row
exists mainly to carry `escP` and its own bank — see below) — and
`setModel()` power-cycles into one. The three C. Itoh heads share the
8510 command set (the II is a backward-compatible
superset), so what varies is DATA, not code paths: which ROM banks exist,
whether there is a four-band colour ribbon, which ESC codes the head has no
hardware for, the power-on pitch (the DMP comes up at Pica 10 cpi, the two
ImageWriters at Elite 12), and the carriage rate.

An absent command is **consumed with its parameter bytes and dropped** — the
manuals' rule is that an unrecognised code goes away with its ESC, and letting
the parameter fall through prints it as text. The trap when implementing that:
the early return must still clear `escCmd_`, or the parser stays armed and eats
the rest of the job. (It did, first time.)

Note the ImageWriter I and DMP correspondence banks are currently
byte-identical to the II's upstream — web-a2e seeds them from it. POM2 keeps
them as separate banks anyway so a future divergence lands automatically, and
`printer_glyph` deliberately does not assert that the faces differ.

**The Epson FX-80 row is NOT another C. Itoh data variation.** It
is a different lineage, so `IwModelProfile::escP` routes it to a SECOND PARSER
(`processEpsonChar` / `execEpsonEscape`) over the same page, dot plotter,
ribbon, pacing and paper. The two grammars collide outright — `ESC G` is
graphics on the C. Itoh family and double-strike on ESC/P, `ESC A` is 1/6 in
spacing on one and n/72 in on the other — which is precisely why they cannot
share a dispatch, and the test asserts that collision directly.

Its graphics take **two binary count bytes and bit 7 as the top dot**, both the
opposite of the C. Itoh spelling; `bitGraph_.msbTop` carries that. Getting it
backwards mirrors every image vertically in 8-pixel stripes and still looks
like a picture, so it is pinned by a round trip rather than by eye.

Commands POM2 does not implement (user-defined characters, the VFU, nine-pin
graphics, tab lists, margins) are CONSUMED WITH THEIR PARAMETERS. That is the
difference between a missing feature and gibberish on the page. And
`resetPrinter()` clears the ESC/P collector, or a reset mid-command leaves the
parser eating the start of the next job.

**Front panel (phase D).** `setPowered()` / `setOnline()` gate the input path
and nothing else — switching a real printer off does **not** eject the sheet or
wipe it, which is exactly what distinguishes them from `powerCycle()`.
`setPaperDimensions()` takes any size in 1/4" steps, clamps to the tractor's
range, and writes back what it *committed* so a caller cannot end up believing
it got a size the printer refused.

Design notes and the remaining phases: [docs/printer_plan.md](docs/printer_plan.md).


`ImageWriter` (`ImageWriter.h/.cpp`) is the **printer**, not an interface
card — it never appears in the slot catalog. Whatever printer interface
card is plugged (`PrinterCard`, `GrapplerCard`) spools bytes; the UI's
per-frame `MainWindow::pumpImageWriter()` streams them into the printer
via `drainSpoolFrom(consumed, out)`, and `ImageWriter_ImGui` shows the
resulting page. Card → cable → printer, in that order, exactly like the
real desk.

**Source of truth: greg-kennedy/ImageWriter** (`imagewriter.cpp` — the
GSport / KEGS / DOSBox lineage by Christopher G. Mason), itself written
against Apple's *ImageWriter II Technical Reference Manual*
(ISBN 0-201-17766-8) and *ImageWriter LQ Reference Manual*
(ISBN 0-201-17751-X). Command dispatch, the soft-switch model, the
density tables and the colour encoding are line-for-line ports with the
reference's line ranges cited in the code.

**Page raster** — one byte per pixel, `yyyxxxxx`: low 5 bits = ink
intensity (31 = full), top 3 bits = ribbon band. The bands are chosen so
that OR-ing two inks mixes them the way overprint on a real four-band
ribbon does:

```
001 magenta   010 cyan     100 yellow
011 blue      101 red      110 green      111 black
```

so magenta|yellow = red, cyan|yellow = green, all three = black; index 0
is blank paper. `ESC K n` picks the band. `pageToRgba()` expands through
`indexToRgb()`, which is `FillPalette` (`imagewriter.cpp:101-114`) in
closed form.

**The carriage stops at the right margin — in graphics too** (2026-08-17,
bug hunt 8 round 2). `printBitGraph` used to advance `curX_` per dot column
with no margin test, unlike every other head-motion path here (the text
advance wraps on `rightMargin_`; `ESC F`/`ESC '` refuse to cross it), so an
over-long bit-image run walked the head off the sheet: `ESC V 9060 <col>` at
80 dpi parks it 113 inches out on an 8.5-inch page, and the pacing model
charged carriage travel for all 9 060 columns — 22 emulated seconds of BUSY
printing nothing, since `fillDots` clips to the raster. A column starting at
or past `rightMargin_` is now **discarded** (never wrapped — wrapping a bit
image corrupts it; the hardware loses the excess of an over-long graphics
line), the head parks against the stop, and `byteCost` charges nothing for a
carriage that is not moving. `rightMargin_` is only ever the paper width, so
this drops exactly what the raster clip was already dropping: the ink on the
page is unchanged, which is the property `imagewriter_smoke` pins (an
over-long run must produce the byte-identical page an exactly-fitting run
produces).

**Colour is a ribbon, not a mode.** `Ribbon::FourColour` (default) /
`Ribbon::Black` models which cartridge is fitted: with the black one the
printer still accepts `ESC K` and prints band 7 anyway, exactly like the
hardware. Nothing host-side "enables" colour — the guest has to ask, and
most drivers only do so when their own setup names a colour printer
(Print Shop emits `ESC K` only for "Apple Imagewriter II **(C)**"; its
(M) driver never does). Persisted as `imagewriter_ribbon`.

**Text is dot-matrix, not TrueType.** The reference needs SDL 1.2 +
FreeType; POM2 links neither — glyphs come from the transcribed character
ROM banks (`ImageWriter::romGlyph` / `currentBank()`, see § Character ROMs
above), with the repo's 8×8 CP437 font as the fallback for anything a
bank does not carry. That is *closer* to the hardware, not further: an
ImageWriter draft cell really is 8 dots wide at the pitch's density and
8 pins tall at 1/72 in, so a character and a graphics column go through
the same plotter (`fillDots`). Bold = 1.5× dot width (what a
half-dot-offset second pass leaves on paper); italics shear one dot over
the cell height; double-width halves `actcpi`. Proportional mode
(`ESC p` / `ESC P`) advances by the glyph's own ROM escapement
(`ImageWriter::glyphAdvance`), not a fixed cell.

**Dots are painted as the page-pixel interval they cover**, replacing the
reference's `pixsize` + "Primative scaling function" fudge
(`imagewriter.cpp:1556-1573`). Adjacent dots abut at any page DPI, so
graphics dumps have no seams and no doubled columns.

**Bit images**: `ESC G/S/g nnnn` (8-pin columns, 72-160 dpi) and
`ESC C nnnn` (24-pin LQ columns over 3 bytes, 216 dpi vertical);
`ESC V` / `ESC U` are the repeat-column forms. Bit 7 is **never** masked
inside a bit image — it is pin 8. Everywhere else soft switch B-6 strips
it, which is what makes Apple II `COUT` output (always bit-7 set) print
as plain ASCII.

**Line feed after CR is auto-detected** (`AutoFeed::Auto`, the default).
SW A-8 has three right answers and no user should have to guess which:

| Sender | Sends | Printer must |
|---|---|---|
| `PR#n : PRINT` from BASIC | CR only | feed — else the listing overprints one line |
| Any real driver, and the Grappler+ firmware | CR **+** LF | not feed — else everything double-spaces |
| A colour driver (Print Shop) | bare CR **between passes** | not feed — the yellow/cyan/magenta passes must overprint the same line |

`Auto` feeds on CR until it sees the guest send its own LF immediately
after one; that LF is then swallowed (one advance, not two) and CR stops
feeding for the rest of the job. All three cases come out right with
nothing configured. Getting this wrong is not subtle: with the printer
always feeding, Print Shop's colour passes march down the page as a
coloured staircase instead of forming one line. `On`/`Off` pin the switch;
a power cycle re-arms the detector — and so does the guest's own `ESC c`
("initialize printer"), which is the **only** thing a guest can send that
does. That matters because the latch was otherwise scoped to the host
session rather than the job: once one CR+LF driver had latched it, every
later `PR#n : LIST` printed its whole listing overprinted onto one black
line with nothing in the guest able to clear it. `ESC c` is safe as the
re-arm point precisely because Print Shop separates its colour passes
with a bare CR and never sends it. Pinned by `imagewriter_smoke`
(`testAutoLineFeedDetection`, cases 7 and 8).

**Deliberate deviations** from the reference. The first two are cosmetic
ports; the four after them are reference *bugs* that put visibly wrong
ink on paper, checked against the *ImageWriter II Technical Reference*
and pinned by `testCommandBoundsAndTabs`:

- `resetPrinter()` leaves bold off (the reference sets `STYLE_BOLD` at
  `imagewriter.cpp:289` to fatten a thin TrueType face — on a dot-matrix
  cell that just smears).
- `spacesToZeros` normalises only the *digit* positions of a parameter
  string, so `ESC R nnn ' '` repeats a space instead of printing zeros.
- **HT/VT go to the nearest stop**, not the farthest. The reference
  (`imagewriter.cpp:1131-1141`) keeps overwriting its candidate as it
  scans, so with stops at 10/20/30 the first TAB jumped to 30 and every
  later one was a no-op — one ragged column instead of a table.
- **`ESC 1`..`ESC 6` add n/120″ of intercharacter space**, they are not an
  absolute head position. The reference assigns `curX_ = n/unit`
  (`imagewriter.cpp:665-678`), so `ESC 3` mid-line threw the head from
  1.25″ back to 0.02″ — outside the left margin — and destroyed every
  justified line a proportional driver produced.
- **`ESC c` ejects the sheet on the platen instead of binning it.** The
  reference discards it (`imagewriter.cpp:315`) and can afford to: it
  wrote each page to disk as it went. Here the sheet exists nowhere else,
  so a short report with no trailing form feed vanished the moment the
  next program sent its init. A blank platen still does not eject, the
  same rule the FORM FEED button follows.
- **`ESC H` and `ESC L` clamp to the sheet.** `ESC H 0000` set a
  zero-length page so every line feed ejected — three LFs, three sheets,
  and a real job rolled the whole 32-page stack away in blanks. `ESC L
  999` put the left margin 83″ out and every page came out blank; `ESC L
  000` gave a negative margin that clipped the first character. All
  silent. A margin or page length off the paper is a garbled parameter,
  not an instruction.

**What the guest cannot reach**: bit 7 of either soft switch. The bit-7
mask (`printCharInternal`, following `imagewriter.cpp:1260-1263`) is
applied before the escape parser sees the byte, so `ESC D`/`ESC Z` can
only set bits 0-6. Left alone deliberately — neither bit 7 is wired to
anything here. A-8 is the "LF after CR" switch, which POM2 models with
`AutoFeed` above rather than the switch byte, and B-8 is unused
(`updateSwitch` reads only the charset field, B-1 and B-6).

**Paper handling**: `FF` ($0C) and a full page both eject onto the
completed stack; the FORM FEED button will not eject a blank sheet. The
stack is capped at `kMaxPages` (32) with older sheets rolled off and
counted in `droppedPageCount()` — a guest that form-feeds in a loop must
not exhaust host RAM. Page size = paper size (points/72) × page DPI;
Letter at the default 144 dpi is 1224×1584.

**The mechanism prints at its own speed.** The card hands bytes over at
bus speed — a `PRINT` loop spools a page in a millisecond of emulated
time — so `queueBytes()` parks them in the printer's input buffer and
`tick(dt)` releases them at the rate the head can actually lay them
down, driven by the host frame time (`ImGui::GetIO().DeltaTime`), not by
`emuCycles`: the paper keeps moving while the guest is paused, turbo'd or
rewound, exactly like the real desk. Speeds are Apple's published figures
(*ImageWriter II Owner's Manual*, "Specifications"): **250 cps draft**,
**45 cps NLQ**, both quoted at the 12 cpi default pitch, so the carriage
crosses `cps/12` inches per second. `byteCost()` charges per byte from
that: one character = `1/cps`; a bit-image byte = one dot column at the
active density on a unidirectional (half-rate) pass; `CR` = a direction
change in draft but the full return slew in NLQ (draft is bidirectional);
`LF` = `lineSpacing / 5 ips` of paper transport; `FF` = whatever is left
of the sheet. Escape-sequence bytes and other control codes are free —
they land in a register, not on paper. `Speed::Instant` restores the old
print-everything-this-frame behaviour.

**No byte may ever be unaffordable.** `tick` banks elapsed time and spends
it byte by byte, capped so a hidden window can't dump half a page at once
— but the cap has to leave room for the byte at the head of the queue. A
flat 1 s cap against a form feed that costs `(bottomMargin - curY)/5 ips`
= 2.2 s on a Letter sheet meant that byte was never affordable: the queue
stalled forever, BUSY stayed asserted, and the guest hung in its firmware
ACK loop. Print Shop froze on every page eject. The cap is now
`max(kMaxCredit, cost of the head byte)`, and a watchdog forces any byte
that has waited through anyway, logging it — a cost-model mistake must
degrade to "printed late", never to a hang. The watchdog's patience is
`max(kStallSeconds, 1.5 × the head byte's cost)`, not a flat 10 s: since
the credit cap already grows to the head cost, a byte that is merely
*slow* becomes affordable on its own, and a flat threshold cut a
legitimately long form feed short and logged a STALL for a printer that
was working correctly. Pinned by `imagewriter_smoke`
(`testNoUnaffordableByte`, `testCommandBoundsAndTabs` case 6).

**The credit cap bounds seconds, not work.** Those are different, and
conflating them was a freeze. Past `kMaxBacklog` (1 MiB) the mechanism
gives up on Draft/NLQ pacing — but it must catch up *across ticks*.
`queueBytes()` used to call `flushPending()` and print the whole backlog
synchronously, on the UI thread, from `pumpImageWriter()`: measured
852 ms for plain text and **301 s for a form-feed storm, in one frame**,
while audio and the CPU worker carried on — a hard freeze. `catchUp_` now
arms instead, and `tick()` drains a bounded slice: `kCatchUpBytes`
(16 KiB) **and** `kCatchUpSheets` (4), budgeted separately because an
eject copies a whole page raster (~1.9 MB at Letter/144) and so is orders
of magnitude dearer per byte than a glyph. Worst frame: ~14 ms, and a
1 MiB backlog clears in about a second. It stays armed until the queue is
*empty*, not merely back under the threshold — stopping at the threshold
would hand ~512 KiB back to the 250 cps model and spend half an hour of
wall clock on it. Past a hard ceiling (`kHardBacklog`, 4 MiB) the oldest
input is dropped and counted in `droppedInputBytes()`, the same rule the
page stack and the SSC tap spool already follow: a guest that sustainably
outruns even the catch-up rate must not grow the heap without bound.
Truncating a printout is bad; freezing the emulator is worse. Pinned by
`testBoundedCatchUp`.

**One printer, four possible feeds.** `pumpImageWriter()` arbitrates —
`PrinterCard` outranks `GrapplerCard`, the parallel cards outrank the
FujiNet printer unit, which outranks the SSC tap — and keeps ONE drain
cursor. Everything hard about that is the handover,
so it lives in `printerFeedCursor()` (`PrinterFeedCursor.h`), header-only
and dependency-free so it can be pinned without an ImGui context. A
changed source re-seats the cursor at the new source's **current total**,
not at 0. Re-seating at 0 re-drained everything that source had ever
spooled: the SSC tap's spool outlives its source status (nothing clears
it), so a single frame with "Feed ImageWriter printer" unticked reprinted
the whole session on the next frame — and on a //c, where slot 1 is the
printer port and slot 2 the modem port and both are SSCs, unticking slot 1
handed the source to slot 2 at 0 and printed the entire modem transcript
onto paper. Adopting also means bytes spooled *while not the source* never
print, which is the physically right answer: the cable was out. Slot
Config lets a `PrinterCard` and a `Grappler+` coexist (different catalog
keys, so its duplicate check does not object) and the loser then feeds
nothing, so the panel's source line names it. Pinned by `testSpoolSeam`.

**Trace log.** A printout that comes out as noise is a protocol
disagreement, and the only way to see it is the byte stream, decoded.
`startTrace(path)` writes an interleaved hex dump (`RX`), completed
escape sequences with their parameters (`CMD`), bit-image setup (`GFX`),
page ejects (`PAGE`) and host events (`HOST` — queue depth, BUSY
transitions, watchdog stalls). Enable it from *Printer settings → Log the
printer stream to a file* (→ the per-user printouts directory) or set
`POM2_TRACE_PRINTER=1` (or `=<path>`) before launch to catch a printout
that happens during boot.

**The paper is continuous fanfold, not a cut sheet.** An ImageWriter II is
fed 9.5" pin-feed stock: the printable body plus a 0.5" tractor strip each
side, each strip perforated off along sprocket holes on 1/2" centres, and
each sheet joined to the next by a horizontal perforation. The panel draws
the strips, holes and perforations AROUND the page texture
(`ImageWriter_ImGui`, `ImDrawList`), never into it, so the page raster and
the "Save sheet as PNG" export stay pure printable area.

**"Follow" tracks the last inked sheet, not the sheet in the mechanism.**
After a form feed the sheet under the head is blank and the interesting
one is on the stack — following the blank one made a one-page job look
like it had printed nothing at all.

**BUSY closes the loop back to the guest — opt-in.** A stock ImageWriter II
buffers `kInputBufferBytes` (2 KB) and then stops acknowledging; the pump
pushes that state to the card with `GrapplerCard::setPrinterBusy()`, and
`ackEffective()` folds it into the status byte's bit 0. That is the bit
the genuine Grappler+ firmware spins on — **not** BUSY (bit 3):

```
$CD89  JSR $CDE1      ; read $C08n status
$CD8C  AND #$02       ; SELECT? no → give up
$CD93  AND #$01       ; ACK latch
$CD95  BEQ $CD89      ; spin until the printer acknowledges
```

so a guest printing a long job blocks in its firmware wait loop while the
paper catches up, instead of blasting a page into a host queue.

`MainWindow::printerBackPressure` gates it, **default off**
(`imagewriter_backpressure`). It is faithful — 5.2 s for Print Shop's
5 KB test page, minutes for a full card — but an emulator that stops
answering for minutes is indistinguishable from a hang, and the printout
paces itself identically either way. The status bar shows any print in
progress, and `(Apple II waiting)` when the handshake is holding the
guest. The
synthetic `PrinterCard`'s ROM never polls (its handler is `STA`/`RTS`), so
it is not throttled — its queue simply drains at printer speed.

**Rewind vs. paper (accepted design)**: the printer chain (card spools,
`ImageWriter::pending_`, the paper stack) lives entirely outside
`MachineSnapshot` — paper is a host-side artefact and deliberately does
not travel back in time. Consequence: rewinding across a print and
replaying re-executes the guest's print code, and the replayed bytes are
interpreted against whatever parser state the first pass left behind
(a rewind mid-`ESC G` data run makes replayed text land as dot columns).
The printout produced across a rewind is therefore best-effort; use the
panel's **Reset printer** to re-arm a clean parser. Spool growth is
bounded: the SSC tap trims its consumed prefix past 1 MiB (absolute
drain offsets, `SuperSerialCard.cpp`), and the mechanism force-drains
once `pending_` backlog passes 1 MiB (`ImageWriter::queueBytes`).

**Slot 3 on a //e is a trap** (and is one on real hardware too): the
internal 80-column firmware keeps `OURCH`/`OURCV` in the *slot-3* screen
holes (`$0578+3`, `$05F8+3`, …), which is exactly where printer firmware
keeps its column and line counters. A Grappler+ in slot 3 reads the
cursor position back as its line width and emits `CR LF` after every
character, plus a perforation skip every few. Slot Config warns; slots
1/2/4/5/7 print correctly (verified against the real 4 KB dump on both
`apple2p.rom` and `apple2e.rom`).

**Not modelled**: user-defined character sets (`ESC '` / `ESC I`, absent
from the reference too) and `ESC ?` (send ID string — POM2 has no
printer→computer back-channel).

**Super Serial Card feed (the //c's real printer port).**
`SuperSerialCard::setPrinterTap(true)` mirrors every byte the ACIA
accepts for transmit (i.e. past the DTR gate — a byte the transmitter
drops never reaches the paper either) into a host-visible spool with the
exact `drainSpoolFrom` shape of the parallel cards, and
`pumpImageWriter()` consumes it as a third source with parallel cards
outranking it (a IIe with both keeps parallel routing). The tap defaults
ON for slot 1 (the printer-port convention — a stock //c profile prints
via `PR#1` with zero configuration) and is persisted per slot as
`ssc_printer_tap_slotN`. Enabling that path surfaced a real firmware
gap: POM2's synthetic SSC ROM only initialised the ACIA in the Pascal
PINIT entry, so a plain `PR#n : PRINT` wrote the TDR with DTR
de-asserted and the 6551 (correctly, MAME `mos6551.cpp:317-321`)
dropped every byte. The PR#n/IN#n entries now program cmd=$0B first,
like the real SSC firmware's DIP-switch init.

**An armed tap is a device on the pins.** On a //c, `$C100` is *internal*
ROM: `PR#1` runs the machine's own printer-port firmware, not the card's
synthetic ROM, and that firmware gates every character on the 6551 status
register — it spins until `status & (DCD|TDRE)` reads "carrier present,
transmitter empty". DCD/DSR are active-low *device-present* pins, so the
status read reports them **inactive when nothing is attached** (MAME
`mos6551.cpp:37-39` inits `m_dsr(1), m_dcd(1)`; AppleWin
`SerialComms.cpp:864` returns `ST_DSR|ST_DCD`). "Nothing attached" is the
operative phrase: an ImageWriter cabled to the port *is* a DCE sitting
there, and a printer has no carrier to acquire. Answering those pins from
the telnet connection alone told the //c its printer was absent —
`PR#1` wedged the guest inside the firmware and not one byte reached the
spool, on all three //c profiles, with no workaround (nothing else is
pluggable on a machine with no slots). `deviceAttached()`
(= telnet peer **or** armed printer tap) is what the pins answer to.
Pinned by `iic_printer_port`.

The tap's spool is capped at 1 MiB and trims its oldest half when the
host falls behind — the drain cursor speaks absolute offsets
(`printerSpoolBase_ + index`) so trimming never desynchronises the
consumer. It now **warns once per session** when it fires: half a
megabyte out of the middle of a printout is silent data loss otherwise,
and all the paper shows is a job that stops mid-sentence.

**PDF export** (`ImageWriterPdf.h/.cpp`): "Save PDF" writes every
completed sheet (plus the sheet in the platen if printed on) as one
multi-page PDF. Each sheet embeds as an 8-bit `/Indexed /DeviceRGB`
image — the page raster already is exactly that — compressed with
`/FlateDecode` via stb's `stbi_zlib_compress` (in-repo for PNG; a zlib
stream is what FlateDecode consumes), so there is no new dependency.
`Page::dpi` records each sheet's raster density at eject time, so the
`/MediaBox` stays at true physical size even if the host changes the
printer DPI mid-session.

Pinned: `imagewriter_smoke` — paper geometry, glyph ink + bit-7 strip,
CR/LF + `ESC A/B` spacing, `ESC K` bands + subtractive overprint +
palette, `ESC G`/`ESC C` bit images, `ESC R` framing and the byte
odometer, form feed + page cap, RGBA export, the
`PrinterCard::drainSpoolFrom` streaming/resync seam, and the mechanism
pacing (draft/NLQ rates, `flushPending`, power-cycle drops the buffer).
`grappler_card_smoke` pins the BUSY → ACK handshake and the MAME
register/bank parity (see § Grappler+). `ssc_acia_smoke` pins the
printer tap and the PR#/IN# ACIA init; `imagewriter_pdf` pins the PDF
serialiser (xref byte accounting, per-sheet MediaBox, Flate round-trip).
`iic_printer_port` pins the //c route end-to-end — the DCD/DSR
device-present contract, the firmware's status wait-loop shape, and a real
DOS 3.3 boot where `PR#1` + `PRINT` land bytes in the spool on all three
//c ROMs (ROM/disk gated: skips when the user-provided media is absent).
`ssc_acia_smoke` alone could not catch the //c hang — it drives the tap
through the *card's* synthetic `PR#n` ROM, which only checks TDRE and is
blind to DCD.

### Mouse Card

Verbatim port of MAME `bus/a2bus/mouse.cpp`. Pieces:
- **M68705P3** MCU (Apple 341-0269, 2 KB mask ROM). Paced at 2× CPU
  clock from `advanceCycles()` via fractional accumulator.
- **MC6821** PIA — bus side at `$C0n0-$C0n3`.
- **8516 EPROM** — 2 KB slot ROM (Apple 341-0270-c), bank-switched
  into `$Cn00-$CnFF` via PIA PortB bits 1-3 (`bank = (PortB & 0x0E)
  << 7`).

PIA ↔ MCU bridge:
```
PIA PortA  ↔ MCU PortA            (bidir, pull-ups)
PIA PB4-7  ↔ MCU PC0-3
PIA PB1-3  → EPROM A8-10          (bank select)
MCU PB6    → slot IRQ (active low; cached, transitions only)
MCU PB7    ← mouse button (active low)
MCU PB0=X dir, PB1=X gate, PB2=Y dir, PB3=Y gate (quadrature)
```
POM2 labels X pair `X0/X1` lower-bit-first (X0=PB0=dir, X1=PB1=gate);
MAME's `mouse.cpp` uses opposite digits (X1=0x01=dir, X0=0x02=gate).
Same bits, same behaviour — only label differs; Y labels match MAME.
`updateAxis` line-for-line MAME `update_axis<>`.

Host routing: `MainWindow::onMouseMove/onMouseButton` →
`setHostMouse(rawX, rawY, button)` (clipped to screen rect). MCU
computes deltas via 8-bit subtraction with wrap; POM2 emits **at most
one quadrature edge per axis per MCU PortB read** (matches MAME
`m_last`/`m_count`).

**Put it in slot 4, not slot 3.** On a //e with `SLOTC3ROM` off (the
reset default) the motherboard owns `$C300-$C3FF` and slot 3's I/O
SELECT never asserts, so a card there has no `$Cs00` page at all —
`Memory.cpp` models this exactly and `iie_memory_smoke_test` pins it.
Software finds the mouse by scanning slots for the Apple signature
(`$Cn05=$38`, `$Cn07=$18`, `$Cn0B=$01`, `$Cn0C=$20`); measured with the
same card and ROM, slot 3 reads `00 00 00 00` and slot 4 reads
`38 18 01 20`. **French mouse software often does not scan at all**:
Extasie reads the entry-point table at `$C412+` and calls the firmware by
self-modified `JSR $C4xx` (DET `$75FA/$7639`) — slot 4 or nothing, which is
why slot 4 is the fresh-install default and Slot Config warns on any other
(`tests/extasie_mouse_probe.cpp` is the reproduction). So A2DeskTop / MousePaint / MultiScribe find the
80-column firmware where the signature should be, decide there is no
mouse and run keyboard-only. Real hardware is identical — Apple sold the
mouse for slot 4. The same window kills anything else that needs it: a
Mockingboard addresses its VIAs through `$Cs00`
(`MockingboardCard::slotRomRead`) and goes silent in slot 3; a card that
only uses its `$C0nX` soft switches is unaffected. Slot Config flags it
inline on the row, seeded from the live config so an existing setup is
marked as soon as the panel opens.

#### Pointer capture ("mouse grab") — `MouseGrab.h`

Both cards are **relative** quadrature devices, so uncaptured the host
pointer hits the edge of the screen widget long before the guest cursor
reaches the edge of its firmware clamp window — every further delta is
dropped and the guest cursor can end up unable to reach a menu bar.
Capturing fixes it at the source: `GLFW_CURSOR_DISABLED` hides the OS
cursor and unbounds the reported position, so deltas keep flowing
forever. `glfwSetInputMode(GLFW_RAW_MOUSE_MOTION)` rides along on native
(the desktop's acceleration curve is tuned for a screen-sized target;
the browser's pointer lock already delivers raw `movementX/Y`).

- **In**: `Ctrl+Alt+G`, a **middle click**, View ▸ Capture mouse, or
  `view.mousegrab` in the palette. A **left click never captures**. It
  used to (`mouse_click_to_grab`), and the capturing press then had to be
  **swallowed** — the guest cursor is not under the host pointer, so
  forwarding it would click at an arbitrary spot — which meant an
  ordinary click both vanished and silently changed what every later
  click meant. The setting is gone with the behaviour: it could no longer
  gate anything, and a preference that changes nothing is worse than
  none. Stale `mouse_click_to_grab` keys in an existing `state.cfg` are
  simply never read.
- **Out**: `Ctrl+Alt+G` (in the unconditional key set in `main.cpp`, and
  tested in `onKey` above the Ctrl-letter path that would inject $07),
  **middle click**, window focus loss (`glfw_window_focus_callback`),
  **leaving kiosk for the windowed GUI** (`setKioskModeRuntime`, first
  thing in the else branch — kiosk is where a captured pointer costs
  nothing, the GUI is where it costs the user their menus and panels;
  doing it before the monitor change also keeps a `GLFW_CURSOR_DISABLED`
  pointer out of the full-screen → windowed transition the OS re-warps
  it across), or the card going away (`render()` releases when both
  pointers are null, which covers slot config / profile switch /
  snapshot restore at once). **Entering** kiosk deliberately leaves the
  grab alone — a full-screen game wants the mouse.
  `shouldToggleGrab` refuses to *capture* without a card, and under the
  3D voxel view where middle-drag pans the camera — but it never refuses
  to *release*: an escape hatch any state can block is not one.
- **Nothing is drawn on the emulated screen.** Two captions used to be
  ("Click to capture the mouse", and a how-to-get-out reminder), both
  there to paper over click-to-grab: a click that silently changed the
  mouse had to announce itself, and an accidentally-captured user had to
  be told the way out. Capture is now only reachable by the same two
  gestures that leave it, so whoever is captured already knows the way
  out. The indicator is the status bar's `GRAB` chip, with the way out
  spelled out beside it for 30 s (`mouseGrabHintUntil_`).
- **While captured**: `ImGuiConfigFlags_NoMouse` — io.MousePos tracks the
  virtual cursor and would hover panels the user can't see. The GLFW
  backend skips its own cursor-shape updates under `GLFW_CURSOR_DISABLED`
  (`ImGui_ImplGlfw_UpdateMouseCursor`), so the two never fight over the
  input mode. The AppleWin **absolute closed-loop sync is off** (a virtual
  position projects onto nothing) — capture is always the relative path.
  Both edges reset `mouseInited` + the sub-pixel accumulators, and leaving
  clears a held button so the guest can't be stranded with one down.

A grab is host-side only, like kiosk: the machine never sees it, so
nothing about it is snapshotted. The policy itself lives in
`MouseGrab.h` — GLFW-free, so `mouse_grab_policy` pins it with no
windowing stack; `MainWindow_Input.cpp` static_asserts its mirrored
GLFW tokens against the real header.

**ROM gating**: BOTH ROMs required. Slot-config UI greys entry when
missing; `plugSlotsFromSettings` refuses with a Mouse log warn.
Defaults: `roms/mouse_341-0270-c.bin` + `roms/mouse_341-0269.bin`.

**Not modelled** (firmware-invisible): PAL16R4 chip-select sequencer
U2A, PIA PortB bit 0 sync latch, motion clamping (MCU does it).
Pinned: `mouse_card_smoke`, `mouse_card_quadrature_smoke`, and
`mouse_card_axis_parity_test` — the latter boots **real firmware**
(both ROMs) on a full M6502+Memory, drives ProDOS
`InitMouse/SetMouse/ReadMouse` from a stub, asserts identical host
ramp moves X and Y equally (caught X==Y==800 for a +800 px ramp).

#### AppleWin HLE variant — `MouseCardAppleWin` (card key `mouseaw`)

Alternative implementation, verbatim from AppleWin
`source/MouseInterface.cpp` (CMouseInterface). Same SlotPeripheral,
same `setHostMouse(rawX,rawY,button)` UI plumbing, **same slot
EPROM** (`mouse_341-0270-c.bin`) — but **no MCU mask ROM**: the
68705P3 side is a C++ command-byte state machine. Plug as
`"mouseaw"`; mutually exclusive with MAME `"mouse"`.

Protocol (mirrored from AppleWin `OnCommand`/`OnWrite` — opcodes are
high nibble of first command byte):
```
$00 MOUSE_SET     1 B   set mode (MOUSE_ON / INT_VBL / INT_BUTTON / INT_MOVEMENT)
$10 MOUSE_READ    6 B   reply Xlo, Xhi, Ylo, Yhi, status
$20 MOUSE_SERV    2 B   pending-IRQ source + CpuIrqDeassert
$30 MOUSE_CLEAR   1 B   wipe position + state
$40 MOUSE_POS     5 B   set absolute position (X16, Y16)
$50 MOUSE_INIT    3 B   clamp 0..1023, position = 0, canned $FF reply
$60 MOUSE_CLAMP   5 B   set X or Y clamp window (cmd byte bit 0 = axis)
$70 MOUSE_HOME    1 B   re-home to (iMinX, iMinY) — the top-left of the
                        CLAMPING window, per Apple's HOMEMOUSE, not (0,0).
                        The one deliberate deviation from AppleWin here
                        (`MouseCardAppleWin.cpp:314-329`); the two agree only
                        while the clamp is still the power-on 0..1023.
$90 MOUSE_TIME    1..4 B no-op
```

PIA Port B as 2-line handshake (AppleWin `On6821_B`): BIT5 (PB5) =
write-strobe (firmware → "MCU"), BIT4 (PB4) = read-strobe. BIT6/BIT7
driven back to firmware for poll loops. BIT1..BIT3 still
slot-ROM bank-select (`bank = (by6821B << 7) & 0x0700`).

VBL interrupt: `OnMouseEvent(true)` fires once per ~17045 cycles
(60 Hz @ 1 MHz) from `advanceCycles`; host-input poll
(`pollHostInput`) drains atomic shadow each `advanceCycles` so
movement/button changes raise IRQ immediately when mode bits allow.
`CpuIrqAssert(IS_MOUSE)` → `assertIrq(true)`; `CpuIrqDeassert` (in
MOUSE_SERV) → `assertIrq(false)`.

Pinned: `mouse_card_applewin_smoke` — slot-ROM bank-select round
trip, size/missing-file rejection, BIT5 strobe → `OnCommand`
(MOUSE_INIT writes canned $FF to PRA).

Why ship both? `mouse` (MAME) is preferred — it boots verbatim Apple
ROMs. But the MCU mask ROM (`mouse_341-0269.bin`) is not always
available; `mouseaw` lets users with just the slot EPROM get a
working mouse.

### Joystick / paddles

`JoystickInput` polls all 16 GLFW slots each UI frame (hot-plug).
One binding drives PADL(0/1) + PB0/1/2. PADL(2/3) read centred
(128). **Paddle RC** in `Memory::softSwitchAccess`: `$C064-$C067`
returns `0x80` while `(cycleCounter - paddleLatchCycle) <
paddleValue × 11`. `$C070` arms latch. 11-cycle constant = rough
Apple II RC step.

**Square gate (`applySquareGate`, default on, key `joystick_square_gate`).**
A modern analog stick rides in a *round* gate: a full diagonal only reaches
(~0.707, ~0.707), so both paddles top out near 217/255 at once and the four
extreme corners are physically unreachable. The original Apple II stick rode a
*square* gate, so the corners (full X **and** full Y = 255/255) were reachable —
which some titles need (e.g. Wings of Fury's take-off). The whole pipeline is
the pure static `stickToPaddles()` (`paddleValue()` just reads the hardware
and routes through it): **invert** → **rescaled radial deadzone** (kill by
magnitude — per-axis would notch the diagonals — then remap [dz..1] → [0..1]
along the ray so the reading is continuous across the engage threshold; a
hard cutoff stepped ~12 counts) → **axis-snap notch** (zero the small axis
while it sits under `dz × |dominant|`, so 5 % cross-axis drift during a full
single-axis push reads 128 instead of ~134, while diagonals — comparable
components — are never notched) → **square gate**: scale the vector out along
its own ray until its largest component hits the square edge,
`s = mag / max(|x|,|y|)`, mapping the inscribed circle onto the full square
(45° → (1,1)) while leaving pure-axis directions untouched (`s = 1`) →
`axisToPaddle01` ([-1..1] → 0..255, center 128). Toggle in the Joystick
panel. Pinned by `joystick_square_gate` (gate + mapping + the full
composition: deadzone-edge continuity, drift suppression, gate-off, invert).

**In-game gamepad mapping (`JoystickInput::GamepadPlay`).** When the bound
pad has a standard GLFW/SDL gamepad mapping (`play().valid`), the analog
stick stays the Apple II paddles and the digital controls route as:
**Circle → PB0, Cross → PB1** (`setPaddleButton`), **Square → SPACE,
Triangle → RETURN** (one `queueKey` per press), **D-pad → Apple II arrow
codes** (←$08 →$15 ↑$0B ↓$0A) with //e-style auto-repeat (350 ms, then
~16/s). Raw (unmapped) pads fall back to buttons 0/1/2 → PB0/1/2 only.
Suppressed while the kiosk menu is open (+ swallow latch across the close —
see § Host control).

## UI (ImGui)

`MainWindow` — menu bar + screen + emulation panel + on-demand
panels. Owns the screen GL texture. Auto-plugs Disk II in slot 6 if
`roms/disk2.rom` exists. F9 (screenshot), F11 (soft reset), F12
(hard reset) routed unconditionally even when ImGui has focus.

### Slot Configuration: two interaction models, made visible

`MainWindow_Slots.cpp`. The panel runs on two *different* models and used to
say nothing about it: the left column is **staged** (edit combos, then Apply /
Revert, where Apply restarts the emulator) and the right column is
**immediate** (Mount / Insert / Eject act at once). Because Apply and Revert sat
at the bottom of the left child, they read as governing the whole window — a
user could mount a disk on the right, hit Revert on the left, and reasonably
expect the mount to come back.

Now: the header states both models; the media column carries "Mount / Insert /
Eject take effect immediately"; each changed slot row gets an accent dot whose
tooltip names the card currently plugged; a badge reads "N staged change(s) —
not applied yet"; **Apply is disabled when nothing is staged** (a button that
restarts the machine should never be a reflex no-op) and its label counts the
changes; Revert is disabled when clean, and its tooltip says it does not touch
mounted media.

`pending` counts only user-editable slots — the rows force-feed the draft with
the profile's built-in cards, so those can never register as pending.

**Slot numbers lead their control.** `LabelText` / `BeginCombo` put their label
on the right, so the panel read "(empty) v  Slot 1" — the number, which is
exactly what the eye scans down, trailed its own control. Rows now emit the
label, `SameLine(gutter)`, then a full-width `##`-id combo, with the gutter
measured off the widest label ("AUX slot") so it survives the UI zoom.

**Columns are responsive.** The assignment child was a hardcoded 400 px, fine
in the 880 px free-floating default but leaving the media column a ~100 px
sliver once the panel is docked into a side dock — every label in it clipped to
"Mount / Inser". Side-by-side now requires `avail > 46 em`; below that the two
sections stack: the assignment child is a plain `ImGuiChildFlags_Borders`
child sized `ImVec2(0,0)` (`MainWindow_Slots.cpp:119`), so the media section
starts right under it.

### Panel registry (`PanelCatalog.h`, `PanelRegistry.*`, `MainWindow_Panels.cpp`)

The UI's god-object problem is not that `MainWindow.cpp` is long. It is that
a panel used to be described in **six** places at once, none of which could be
checked against the others:

| The list | Where it lived | Rows |
|---|---|---|
| load its visibility | `MainWindow.cpp` ctor | 32 |
| save its visibility | `~MainWindow` | 32 |
| offer it in the palette | `renderCommandPalette` | 38 |
| dispatch that command | `runCommand` | 38 |
| its menu row (label, tip, greyed-when) | 6 different menus | 37 |
| hide it on the browser build | the WASM chrome-light block | 28 |

Splitting the file into `MainWindow_<Area>.cpp` moves those rows around; it
does not remove one of them. They had already drifted, in ways that were
invisible precisely because nothing held them together:

* **Seven panels had no settings key at all.** The palette opened them and
  they were gone next launch — including the Debugger and the memory viewer.
  Nothing recorded whether that was a decision.
* **The WASM chrome-light block named 28 panels**, so every panel added after
  it was written stayed open on the browser build. A list that can only rot.
* **The Help menu attached ROM Status's tooltip to Abstraction Levels** (two
  `IsItemHovered` blocks after the same `MenuItem`), so one row showed the
  other's tip and one showed none.
* The palette and the menus carried **different labels for the same window**
  ("Disk II drive" vs "Disk II (slot 6)").

**`PanelCatalog.h` is now the one list**: 38 rows of `{id, title, menu group,
settings key, shortcut, tooltip}`. `PanelRegistry` binds each row to the `bool`
that holds its visibility, plus two optional runtime bits — an availability
predicate (`smartPortCard != nullptr` greys the row) and a dynamic title (the
label that carries a slot number). `MainWindow_Panels.cpp` holds that binding
table and the functions the rest of the UI is derived from:

```
show(PanelId::Debugger)                     // was `bool showDebugger`, ×38
panelMenuGroup(PanelGroup::DevStorage)      // a whole menu section
panelMenuItem(PanelId::Crt)                 // one row, anywhere
forEachPanelCommand(add)                    // the palette's Panel category
runPanelCommand(id)                         // its dispatch
loadPanelVisibility() / savePanelVisibility()
renderPanels(delta)                         // was 43 renderXxxWindow() calls
hideAllPanels()                             // the WASM chrome-light
```

The Devices menu went from 157 lines of hand-written rows to eight: four
`SeparatorText` headers and four `panelMenuGroup` calls. `MainWindow.cpp` lost
**312 lines** net and, more to the point, stopped being where panel *facts*
live. Adding a panel is a catalog row plus a `bind` line; forgetting the bind
is caught at startup by `PanelRegistry::unbound()`, which logs the panel by
name instead of leaving a menu row that toggles nothing.

**Order comes from the catalog, not from bind order.** Menus, palette and the
settings file all iterate the registry, so a sequence that depended on when a
binding happened would reshuffle the user's menus whenever an unrelated
binding moved. `bind()` inserts in catalog position; `panel_registry` pins it.

**The storage moved in too** (same day, second pass). `MainWindow` no longer
has 38 `bool showXxx` members: visibility is one `std::array<bool,
PanelId::Count>` in the registry, reached as `show(PanelId::Debugger)` — a
`bool&`, so the ~92 call sites that read or took the address of a member
changed name and nothing else. `PanelId` is a plain enum; correspondence with
the catalog is by an explicit field in each row, not by position, and
`panelCatalogIsComplete()` is a `static_assert` that every enumerator has
exactly one row. A forgotten row, a duplicate, or a copy-pasted enumerator is
a build failure rather than a menu entry that toggles the wrong window.

Fresh-install visibility moved with it: `defaultOpen` in the catalog replaced
three `= true` member initialisers three hundred lines apart in the header.

**And the render block became a loop.** `MainWindow::render` had ~43 calls —
some gated by the caller, most gating themselves, in the order somebody
happened to add them in. They are now `renderPanels(delta)` → `drawAll()`,
which walks the catalog and calls each panel's `draw` while it is visible.
What stayed behind is the code that is *not* a panel: the modal file dialogs,
`pumpImageWriter()` (a side effect that must run whether or not its window is
open), the About box, the status bar, and the palette overlay, which must
still be last so it draws above everything.

One panel is drawn **while closed**, and the exception earns its flag: the
//e keyboard latches Open-Apple / Solid-Apple, and a latch that outlives the
window showing it as down is a key the guest holds forever with nothing left
to release it. `Runtime::drawAlways` keeps `renderKeyboardPanel` called every
frame so it can act on the close edge, exactly as it did when the call was
unconditional. `panel_registry` pins that, because it is the kind of thing a
loop silently eats.

**What is left.** Nothing. Adding a panel is a catalog row, a `draw` line,
and — only if it sits behind a card — a `bind` for its label and availability.
The panel *bodies* moved into their own translation units on 2026-08-28
(see below); it was a move rather than a rewrite, exactly because nothing
outside a body refers to it.

### The MainWindow family

`MainWindow.cpp` was 8316 lines. It is ~1470, and holds only what a
composition root should: construction, destruction, the dock, and the frame
loop. Everything else is a sibling TU named for what it owns.

| file | owns |
|---|---|
| `MainWindow.cpp` | ctor/dtor, `render()`, the DockSpace and its layout presets, theme + DPI |
| `MainWindow_SlotConfig.cpp` | `plugSlotsFromSettings` — the peripheral composition root, including the runtime seams a DEVICES card may not build for itself |
| `MainWindow_Slots.cpp` | the Slot Config + Internal Disks windows, profile switching |
| `MainWindow_Chrome.cpp` | menu bar, status bar, command palette, `runCommand` |
| `MainWindow_Screen.cpp` | framebuffer upload, `drawScreenImage`, screenshots |
| `MainWindow_Input.cpp` | keyboard, paste, pointer grab, mouse, joystick, file drop |
| `MainWindow_Kiosk.cpp` | the whole kiosk mode: render path, in-game menu, GUI ⇄ full-screen |
| `MainWindow_Media.cpp` | mount/eject/boot policy and the SlotBus queries — no UI |
| `MainWindow_StoragePanels.cpp` | every storage window and file dialog — no policy |
| `MainWindow_*Panels.cpp` | audio, device, settings, misc panel bodies |
| `MainWindow_MemoryMaps.cpp` | the memory-map views |
| `MainWindow_Panels.cpp` | the panel registry binding |
| `MainWindow_Session.cpp` | session persistence — `persistSession()`, settings + window geometry writes, and the IDBFS sync point on the browser build |

The split that matters most is **Media vs StoragePanels**: one decides what
happens to a disk image, the other draws. The panels call in; nothing in
`MainWindow_Media.cpp` calls back out to ImGui.

### The coordinators

The other half of the split went sideways rather than down. Ten
`pom2::*Coordinator` classes hold the *policy* that used to live as `MainWindow`
members, so it is reachable from a headless test and cannot accidentally reach
ImGui. None of them owns a card: `SlotBus` does, and a coordinator that needs
one re-resolves it under `lockState()` at the moment it acts — the rule that
survives a topology rebuild.

* **`SlotConfigurationCoordinator`** (`SlotConfigurationCoordinator.h`, 209 lines)
  — the two *non-topology* configuration values: `effectivePlan_` (the settings
  after profile fixtures and the multi-instance policy) and `draft_` (staged UI
  edits, never applied implicitly). Owns `kDefaultCards[]` and `isMultiInstance`.
  SlotBus stays the sole authority for what is actually plugged.
* **`SlotRebuildCoordinator`** (104 lines) — the *sequencing* of a topology
  rebuild, as a `Phase` state machine (Stable / Prepared / Rebuilding) plus
  eight `Hooks`. It exists to make it impossible to clear the SlotBus before
  consumers are detached, or to publish AI endpoints before the replacement
  topology is coherent.
* **`SlotProvisioningCoordinator`** (179 lines) — additive, session-only slot
  provisioning for explicit boot intent (`ensureHdvBootTarget`,
  `ensureSmartPortBootTarget`). It never tears a topology down and never
  rewrites the user's slot plan.
* **`StorageCoordinator`** (1973 lines, the largest and the only one near the
  2000-line watch band) — storage topology and lifecycle: cross-card media
  discovery, the three-phase flush/eject policy above, session-only
  auto-provisioning state, and the one definition of the
  `disk_path_slot<N>[_drive2]` settings key (`pom2::diskIIPathSettingKey`).
* **`AudioCoordinator`** (444 lines) — host audio policy: the whole
  `AudioSource` registration inventory (Mockingboard / Phasor / EchoPlus /
  EchoPlusTms5220) plus per-card volume and mute, resolving live cards under the
  machine lock so a profile rebuild cannot leave the audio callback holding a
  dangling card.
* **`PrinterCoordinator`** (280 lines) — host printer-cable policy: it discovers
  every interface card (PrinterCard / Grappler / FujiNet / SuperSerial), picks
  exactly one by physical priority, drains it, and reports the sources it
  ignored. Its drain also bumps `pom2::mediaWriteEpoch()` — printed output is
  irreversible, so a rewind may not cross it.
* **`NetworkCoordinator`** (263 lines) — the host side of the FujiNet relay: one
  snapshot/apply pair for the panel with the card re-resolved under the lock
  each time, plus the host-only state that has no emulated counterpart (serial
  device scan, helper program paths, status line).
* **`DevicePanelCoordinator`** (314 lines) — the frontend's immutable view of
  slot devices: an `InventorySnapshot` of which slot holds Chat Mauve /
  SmartPort / FujiNet / Uthernet I & II / printer / Grappler / clock / serial,
  captured under `lockState()` so an ImGui panel sees only Snapshot +
  FrameResult and never a raw card pointer.
* **`MouseCoordinator`** (120 lines) — the renderer-free host-mouse boundary:
  which mouse card kind is plugged (None / MAME / AppleWin), the AppleWin
  inspector snapshot and the ProDOS screen-holes snapshot, and the routing of
  host pointer input — without retaining a SlotBus alias.
* **`DebugCoordinator`** (70 lines) — the memory viewer instance and its
  two-phase contract at the emulation-state boundary: read under the lock,
  write after unlocking.

### Coverage, and its floor

`tools/coverage.sh` measures line coverage with clang **source-based**
coverage (regions, not gcov lines: a half-taken `a && b` shows). It exists
because the 2026-08-28 plan named three subsystems as untested —
`TnfsClient`, `FujiNetNetDevice`, `FloppyEmuDevice` — and all three had test
suites. Reading the tree got it wrong three times out of three.

```
tools/coverage.sh              # measure, report, check the floor
tools/coverage.sh --update     # re-record the floor
tools/coverage.sh --html DIR   # browsable per-line report
```

The denominator is **the code the tests link**, not the whole program:
including the ImGui frontend that no headless test can reach would put ~15 000
unreachable lines in it and make the floor a measure of how much UI exists.
The floor is recorded half a point below the measurement, because two runs of
the same tree differ by ~0.1 % and a ratchet that fails on noise is one
somebody switches off.

First measurement 78.90 %. `pom2_core_sdk_consumer` is excluded: it links a
separate project against the installed archive with plain flags, which cannot
work against an instrumented one, and it measures the export contract rather
than POM2's code.

**A skipped test says SKIPPED, not PASSED** *(2026-09-07)*. Coverage measures
what the tests *reach*; this is about the tests admitting when they reach
nothing. ~57 sites did `printf("SKIP …"); return 0;`, so a missing ROM or
fixture produced a green "Passed 0.00 sec" for a binary that verified nothing
— and that is how five registered tests came to be permanent no-ops, their
fixture paths (`disks_5.4/dos33_master.dsk`, `ProDOS_2_4_3.po`) left behind by
the `dsk/` move, including the **only** PROM-driven DOS 3.3 + ProDOS boot
test. Fixing the paths exposed a second defect underneath: `disk_boot_smoke`'s
break condition never fired, so it compared page `$08` after DOS had booted
and run HELLO. The convention is now uniform — **77 means skipped**, every
other non-zero value means failed — and it is applied as a **directory-wide
sweep** over the `TESTS` directory property (`tests/CMakeLists.txt:6817-6833`)
rather than test by test, so a new `add_test()` cannot forget it. Related, and
for the same reason: `ci.yml` asserts the suite still declares **241** tests
(the download-gated Klaus/zexall oracles used to deregister in silence behind
an `if(EXISTS)`), its "no test depends on machine ROM dumps" line was false
(~70 test files open `roms/`) and is gone, `cpu_cycle_count` fails without
relying on `assert()`, `POM2_HAVE_SLIRP` is defined on `pom2_core` and
`pom2_core_test` too (so the tests and the SDK compile the backend that ships
rather than its 315-line stub), and fixed `/tmp` names and ports 36502/36503
gave way to `tests/TestTempPath.h` and a free port picked at run time.

> **Naming note.** ~27 source comments pin a test by its *file* name
> (`prodos_volume_smoke_test`, `vbl_smoke_test`, `storage_coordinator_test`, …).
> `add_test(NAME ...)` drops the `_test` suffix, so those names do **not**
> resolve with `ctest -R`: strip the suffix (`prodos_volume_smoke`,
> `vbl_smoke`, `storage_coordinator`) or run
> `ctest -N | grep <stem>`. Left as-is rather than mass-edited — a comment
> pointing at `tests/<name>_test.cpp` is still pointing at a real file.

Two mechanisms keep it this way, and they are different on purpose:

* `tools/check_file_sizes.sh` is the general ratchet — a recorded ceiling per
  file, which may fall and may not rise. `MainWindow.cpp` left it entirely:
  at ~1470 it is below the 2000-line watch threshold, as is every sibling
  (the largest is `MainWindow_StoragePanels.cpp` at ~1740).
* `pom2_enforce_mainwindow_line_limit()` is a **hard cap at configure time**:
  any `src/MainWindow*.cpp` over 2000 lines fails `cmake`. Family-wide on
  purpose — the failure mode was never "MainWindow.cpp grows", it was "the
  file that grows is whichever one is convenient".

**Deliberately a table, not self-registration.** Static registrars in 40
translation units would scatter the UI surface back across the codebase; the
whole value here is that one file answers "what panels does POM2 have, where
do they live, and what persists". Same argument the palette makes for its
non-panel commands, one level up.

### Command palette (`CommandPalette_ImGui`)

Ctrl+Shift+P fuzzy launcher over every menu item, panel toggle, profile,
display mode, layout preset and machine action. Exists because POM2 has 42 menu
items across 8 menus, ~33 toggleable panels, and only four keyboard shortcuts —
reaching "Mockingboard" meant remembering it lives under Devices ▸ Sound.

**Shift is load-bearing in the binding.** Plain Ctrl-P must keep reaching the
guest: CP/M under the SoftCard uses it for printer echo. The chord is also in
`main.cpp`'s `isGlobalKey` set, so the palette opens even when an ImGui text
field has the keyboard — same rationale as F11/F12, the user always needs a way
out.

**The palette knows nothing about what a command does.** The host fills a
`{id, label, category, shortcut, enabled, checked}` list every frame the palette
is open (so `enabled`/`checked` track live machine state) and dispatches the
returned id in `MainWindow::runCommand`. One list, one switch — deliberately not
a callback registry, because the value of the palette is that every command is
visible in one place when you read the source. The ~38 **panel** toggles are the
exception, and for the same reason rather than against it: they come from the
panel registry above, which is itself one readable table — keeping them here
meant a second copy of it, and a third in `runCommand`'s dispatch.

Unavailable commands stay in the list, greyed, rather than being filtered out:
seeing "Phasor (no card plugged)" teaches where the thing lives; silently
omitting it does not.

**Scoring** (`fuzzyScore`, case-insensitive subsequence): +10 per matched char,
+15 at a word boundary, +8×streak for consecutive runs, −1 per skipped char.
Word-boundary and streak bonuses are what make "mock" rank
"Mockingboard (VIA + AY state)" above a label that merely contains m-o-c-k.
Matching runs against `"Category Label"` so "devices mock" works and a bare
category name lists its commands.

**Window height follows the match count**, capped at 10 rows — safe *because
the window is anchored near the top*, so it grows and shrinks downwards and the
query field the user is typing into never moves. A centre-anchored palette would
need a fixed height instead.

### Disk Library: tree, favourites, recents

`DiskLibrary_ImGui`. Was a flat list of ~950 rows carrying full relative paths,
with Size and Date columns in prime position.

**Real nested tree.** Two bugs were fixed getting here, both worth remembering:

1. *A flat lexicographic sort does not group directories.* `demo/PLASMAG.dsk`
   (dir `demo`) sorts before `demo/digidream/DD.dsk` (dir `demo/digidream`)
   which sorts before `demo/zzz.dsk` (dir `demo` again). Walking that and
   opening a node on each prefix change emitted `demo` **twice** — two
   `TreeNodeEx` calls with the same ID, which collide in ImGui's storage and
   share one open/closed state. The tree is now built as an actual nested
   structure (`TreeNode` with a `std::map` of children, so siblings come out
   name-ordered for free), folders before files at each level.
2. *ImGui applies tree indentation to the FIRST column only.* The first cut put
   a narrow favourite-star column at index 0, which swallowed the entire indent
   and left every filename flush left regardless of depth — a tree with no
   readable hierarchy. Name is now column 0; the star and the mounted dot are
   inline prefixes.

Tree is used unless a search filter is active; a filtered view shows a flat list
of hits with full paths, which is what someone searching wants.

**Favourites and recents are host-owned.** The panel has no `Settings` access
and no business acquiring one, so `MainWindow` holds both lists (persisted as
`library_favourites` / `library_recents`) and the panel reports a toggle through
`Result` — same contract as the mounted-path list. Recents are driven off the
panel's mount *requests*, not off the cards, so a CLI or drag-and-drop mount
doesn't silently reorder the list behind the user's back.

Both persist into a single `state.cfg` value joined by **0x1F** (ASCII unit
separator): the file is flat `key=value`, and a disk path can legitimately
contain spaces, commas, semicolons and colons, so the separator has to be a byte
a path cannot hold.

**The favourite toggle is in the right-click menu, not a clickable star.** The
row is already a full-span selectable; an overlapping hit target inside it is a
reliable source of mis-clicks, and on a panel whose left-click cold-boots the
machine that matters.

**No sort selector.** It offered Name / Size / Date, and the latter two forced a
flat list — you cannot group by folder and order by size at once, so they
quietly fought the tree. The header row is worth more as space for search.
Size / Date columns can be hidden entirely (`library_hide_sizedate`), which is
what makes the panel usable in a narrow dock.

**`tools/dedupe_library.py`** removes byte-identical images from `disks_5.4/`,
`disks_3.5/` and `hdv/` — a duplicate on disk is a duplicate in the browser.
Groups by size first and hashes only within same-size buckets, so a
1000-file library costs a handful of full reads. Dry-run by default.

### CRT Settings panel UX

`MainWindow::renderNtscSettingsWindow`. The panel is a master ON/OFF toggle
plus the 13 glass/demodulation knobs:

- **No look presets.** A preset row (Clean / Composite TV / Trinitron / Arcade)
  used to be the primary control; it was removed because one click overwrote
  the entire glass block, which made the panel's state hard to reason about.
  The struct defaults plus **"Reset to defaults"** are the only starting
  points now.
- The 13 sliders sit under an **`Advanced`** header opened by default
  (`ImGuiTreeNodeFlags_DefaultOpen`) — with the presets gone they are the only
  controls, so a collapsed header would leave the panel empty on open. Grouped
  `Picture` / `Phosphor` / `Glass` / `Demodulation`.
- **Default barrel is `0.02`** (`NtscParams::barrel`, `NtscPostProcessor.h`) —
  a hint of tube curvature rather than the visible `0.05` warp it shipped with.
- **Labels lead the sliders.** ImGui's native `SliderFloat` puts its label on
  the *right*, so the panel read "bar → number → name" and clipped the longest
  one ("Phosphor curve (ga…"). Now: `TextUnformatted(label)` +
  `SameLine(labelW)` + `SetNextItemWidth(-FLT_MIN)`. `labelW` is *measured*
  from the widest label so it survives the UI zoom.
- Two decimals, not three — `0.055` on a perceptual knob was false precision.

**The contradictory status messaging is the substantive fix.** A green
"CRT Effects: ON" banner sat directly above a red "Shader unavailable — POM2
falls back to the standard NTSC LUT", which left the user unable to tell whether
any control below did anything. The two statements are about different passes:
only the OpenEmulator *demodulation* shader was missing; the CRT glass stack
(`CrtEffectStack`) is a separate pass that still runs. The warning now says so,
and the master toggle became a low-alpha tinted band with coloured text instead
of a saturated full-width slab.

### Docking + layout presets

POM2 hosts a **DockSpace over the viewport work area** so its 38 panels become
tabs in a persistent layout instead of a pile of overlapping windows.
`MainWindow::renderDockSpace()` creates it; `applyDockLayout()` seeds layouts.

**Dependency.** Requires the Dear ImGui **`docking` branch** — `master` has no
`ImGuiConfigFlags_DockingEnable` and no `IMGUI_HAS_DOCK`. The pin lives in
`imgui_pin.env` (repo + branch + commit), sourced by `setup_imgui.sh` and both
CI jobs so the three can't drift. Pinned to a *commit* because `docking` is
force-pushed on every upstream rebase. **Multi-viewport stays off**
(`ConfigDpiScaleViewports` / `ViewportsEnable`): it would move panels into
separate OS windows, meaning per-viewport GL contexts and a different render
loop, for no gain here.

**Chrome reserves its own space.** The main menu bar, the toolbar and the
status bar are all `BeginViewportSideBar` windows, each of which adds to the
viewport's work-area inset. `DockSpaceOverViewport` then covers exactly what's
left, so the chrome is never overlapped and no offset is hardcoded anywhere.
The toolbar was converted from a hand-positioned `SetNextWindowPos(WorkPos)`
window for precisely this reason — at 150 % UI zoom it grew taller than the
saved `Apple II Screen` position and the screen window covered it.
Toolbar and status bar both carry `NoDocking`: they're chrome, and without it a
dragged panel can be dropped into the one-line strip.

**`PassthruCentralNode`** on the dockspace: with nothing docked centrally, the
central node would otherwise paint a grey slab over the whole work area.

**Presets dock by literal window title.** `DockBuilderDockWindow` hashes the
name the same way `Begin` does (`ImHashStr` restarts its CRC at `###`, so
passing the full `"Super Serial###sscPanel"` literal is correct). Consequence:
only panels whose title is a fixed string can be placed. The slot-numbered
panels — Disk II, 3.5", HDV, SmartPort, Printer — build their title at runtime
(`"Disk II (slot 6)"`), so presets can't reach them; they float on first open
and stay wherever the user docks them.

Docking a **hidden** panel still matters: the assignment is written into the
window's settings, so when the user later opens e.g. the Memory viewer it
appears as a tab in the bottom-right group instead of floating over the screen.
That is most of the value of seeding a layout at all.

**Seeding is gated on a persisted flag** (`ui_dock_seeded` in `state.cfg`), not
on "is the node empty". By the time `renderDockSpace` could check,
`DockSpaceOverViewport` has already created the node, so emptiness cannot tell
"fresh install" from "user undocked everything on purpose" — and rebuilding on
every launch would throw away the user's layout.

`applyDockLayout` calls `DockBuilderSetNodeSize` before the first split: split
ratios are computed against the node's size and are unreliable without it.
`DockBuilderRemoveNode` first, so windows the new preset doesn't mention end up
floating rather than stranded in a stale node.

**The screen window's manual title-bar drag is disabled while docked.**
`Apple II Screen` carries `NoMove` (so click-drag inside the screen reaches the
guest's Mouse Card) plus a hand-rolled title-bar drag. Docked, it has no title
bar of its own and the dock node owns its position — left enabled, the computed
rect lands on the node's tab bar and `SetWindowPos` fights the node every
frame: the screen jitters and the tab won't drag out. Hence the
`if (!ImGui::IsWindowDocked())` guard.

Presets: **Reset** (screen centre; **Disk Library / Slot Configuration /
ImageWriter II** tabbed top-right; inspector tab group bottom-right),
**Emulation** (widest screen, one storage column, no debug
tools), **Debug** (memory viewer + maps right, horizontal map along the bottom),
**Audio** (Mockingboard/Phasor/Echo+ right, mixer + tape bottom-right). The
menu entries are actions with no checkmarks — the moment a tab is dragged, the
"active" preset stops describing what's on screen.

**Reset is also the fresh-install startup layout**, so its top-right trio is
what a first launch opens on: all three of `PanelId::DiskLibrary`,
`PanelId::SlotConfig` and `PanelId::ImageWriter` carry `defaultOpen = true`
in `PanelCatalog.h` (the per-panel `bool` is bound by `PanelRegistry`; the old
hand-kept `showDiskLibrary` / `showSlotConfigPanel` / `showImageWriterPanel`
members are gone) — seeding a dock node for a panel that is hidden would place
the tab but show nothing. `Disk Library` is docked first, which makes it the
selected tab. Cassette Deck and Floppy Emu moved down into the inspector
group: still assigned (so they never float over the screen), just not part of
the opening set.

Known gap: kiosk mode bypasses the dockspace entirely (it returns before
`renderDockSpace`), which is correct — kiosk is chrome-free by definition.

### Theme + UI scaling (`Pom2Theme`)

`Pom2Theme.{h,cpp}` owns the whole ImGui look: colour palette, widget
geometry, and the scale chain. It replaced a bare `ImGui::StyleColorsDark()`.

**Opaque backgrounds are a requirement, not a taste.** The stock dark theme
leaves `WindowBg` at alpha 0.94. Over a black boot screen that's invisible;
over a running HGR game every panel turns translucent and the content behind
bleeds through (CRT Settings sliders were legible *on top of* Disk Library
rows). Every background in the palette is alpha 1.0.

**Surface ramp — the ordering carries meaning.** `kBg0` window → `kBg1`
popup → `kBgBar` menu bar → `kBg2/3/4` raised (frames, buttons, tabs, in
hover/active order). Two constraints: popups sit *below* frames on the ramp,
otherwise a slider inside a menu has no visible track (both were `kBg1` at
first and the View ▸ Interface zoom slider rendered as a bare grab on
nothing); and frames match buttons so "interactive surface" is one step.

**Accents are phosphor colours** (amber default, P31 green, cold blue, slate)
— persisted as `ui_accent`. Accent is reserved for *state* (checked,
selected, active, focused title bar); buttons stay neutral, so an accented
control always means something is on.

**Scaling contract.** `applyTheme(accent, uiScale, dpiScale)` rebuilds the
style from a default-constructed `ImGuiStyle` every call. That's deliberate:
`ScaleAllSizes()` is *cumulative* (it multiplies live values and folds the
factor into `_MainScale`), so re-theming a live style compounds the padding.
Rebuilding makes the call idempotent, which is what lets the zoom slider
re-apply on every nudge. Geometry scales by `uiScale × dpiScale`; fonts go
through `style.FontScaleMain` / `FontScaleDpi`, which ImGui 1.92's dynamic
font system applies at draw time — **no atlas rebuild** on a scale change.

**DPI source: use the backend helper.** `ImGui_ImplGlfw_GetContentScaleForWindow(window)`,
*not* `glfwGetWindowContentScale`. They differ exactly where it matters: on
macOS, Wayland, Emscripten and Android the framebuffer is already larger than
the window, ImGui's `DisplayFramebufferScale` path handles HiDPI, and the
helper returns 1.0f — querying GLFW directly reports 2.0 there and scales the
UI twice. The helper also preserves the 0.0 that virtual/accessibility
monitors report (imgui #7902); `MainWindow::setDpiScale` clamps it back to 1.
Call it only *after* `ImGui_ImplGlfw_InitForOpenGL` — the Wayland branch reads
backend data.

**Shared chrome primitives.** `verticalRule()` and `statusLed()` live here so
the toolbar and status bar speak one visual language. `verticalRule` replaced
literal `"|"` text characters in the toolbar, which inherited the text colour
and baseline and so read as content rather than structure.

Known gap: window positions in `imgui.ini` are absolute pixels, so changing
the zoom mid-session does not move panels placed at the previous scale — a
tall-enough toolbar can end up behind the Apple II Screen window. Docking
(with a scale-relative layout) is the real fix.

### MainWindow Pimpl-light

`MainWindow.h` is forward-decl-only for every plugin/panel/controller
— includes only `M6502.h`, `Apple2Display.h` (HiResMode), `Mat4.h`
(`OrbitCamera` member), `MouseGrab.h`, `Pom2Theme.h`,
`PrinterScreenDump.h` (each for a by-value member) and `imgui.h`.
32 owning members behind
`std::unique_ptr<T>` (plus a `vector<unique_ptr<>>` of disk panels);
ctor/dtor/accessor bodies out-of-line so
unique_ptr destruction sees a complete type. Compile-time: `touch
CassetteDeck_ImGui.h` → 2 TUs rebuild; `touch MainWindow.h` → 4 TUs.

No `*Card` pointer is cached at all any more: the accessors
(`MainWindow::primaryDiskII()`, `primaryHdvCard()`, `hdvDevice()`, …) resolve
from the SlotBus on each call, and any pointer they hand back is non-owning
and raw — `SlotBus` owns the cards, and a topology rebuild must not leave a
stale one behind.

- **MemoryViewer_ImGui** — hex + ASCII over 64 KB. Reads via
  `Memory::data()` under `stateMutex` (held by MainWindow during
  `render()`) so viewer never triggers soft-switch side effects.
  Edits go through `Memory::memWrite` (ROM protection applies).
  Per-byte change-flash via frame-counter delta. Search: hex
  sequences and ASCII (raw + high-bit-set).
- **Disassembler6502** — stateless `(mem*, pc) → mnemonic + length`.
- **main.cpp** — GLFW char/key callbacks gated by ImGui keyboard
  capture so editing widgets don't leak into Apple II.
- **Screenshot (F9)** — `screenshot_NNN.ppm` in cwd.

### HGR / DHGR Paint editor (hgrpaint/, shared with POM1)

Tools → *HGR Paint Editor* / *HGR Sprite Editor*. The editor itself (`src/hgrpaint/`, ~5 k lines:
canvas/tools/undo/clipboard + the ii-pix-style image importer with CAM16-UCS
perceptual dithering) is the **portable module shared verbatim with POM1** —
it only talks to the emulator through the `hgrpaint::IHgrPaintHost` seam.
POM2's side is `Pom2HgrPaintHost`:

- **Pokes** — `PaintCardBatcher` coalesces bulk edits (fill/paste/undo/import)
  into one `stateMutex` hold; bytes land via `Memory::writeRamUnchecked`
  (main) / the raw aux bank (DHGR), deliberately bypassing 80STORE/RAMWRT so
  the editor always edits the plane it says it does. Freehand strokes stay
  unbatched so they appear live on screen.
  **Strokes nest, and the counter is why** *(2026-09-07)*: `beginStroke` /
  `commitStroke` used to bracket on a plain `bool`, so a `Ctrl+X`/`Ctrl+C`
  (or the Copy/Cut buttons) during a batched shape drag opened a second
  stroke, the outer `commitStroke` skipped `endBatch()`, and
  `PaintCardBatcher` stayed stuck at depth 1 — every later poke queued into a
  batch nobody committed, so the canvas kept updating while the Apple screen
  froze for the rest of the session and `batch_` grew without bound. Strokes
  now nest exactly, and Copy/Cut flush an open drag the way `Ctrl+V` already
  did.
- **Canvas render** — a private, never-clocked IIe `Memory` + `Apple2Display`
  pair (`renderScratch`): page bytes staged at $2000/$0400 + soft switches
  per regime (HGR / GR / DHGR), rendered with ColorNTSC (colour) or MonoWhite
  (mono preview, decay 0 → no ghosting). Because the scratch's cycle counter
  never advances, its video-event log never publishes → always the fast
  `renderInternal` path, and the canvas is pixel-identical to the live screen.
- **setDisplayMode** — real $C050-$C05F (+$C00C/D, $C05E/F) writes on the
  live machine so the screen follows the page selector.
- **Files** — raw page dumps and PNGs go through the host's checked
  `saveBytes()`, which POM2 backs with `writeFileAtomic`; `publishBytes` and
  `savePng` take `prepareTempPath` too. The sprite editor's raw/ASM saves used
  to `fopen("wb")` in place and discard `fclose`'s return, so a full disk got
  "Saved sprite" over a file that had just been truncated to nothing. PNG
  encoding is `stb_image_write` (impl compiled in `Pom2HgrPaintHost.cpp` —
  MainWindow's stb_image impl is `STB_IMAGE_STATIC`, so the host TU owns the
  only exported stb symbols, which `HgrImageDecode.cpp` links against).

> **The `hgrpaint/` and `hgrsprite/` changes must be mirrored into POM1's
> copies.** The module is shared *verbatim*, and a fix that lands only here
> makes the two diverge silently.

**DHGR extension (POM2-only additions to the portable module).** Six pages:
HGR/HGR2/GR/GR2 + DHGR/DHGR2 (shown iff `host->supportsDhgr()` = IIe-class).
The model (`HgrPaintModel`) treats DHGR as the **aligned block model**:
140×192 16-colour pixels, each 4 dots of the 560-dot line; dot d lives in
byte-column d/7 (even = AUX plane, odd = MAIN) at bit d%7. A page is one
16 KB pair buffer `[aux 8 KB][main 8 KB]` (= A2FC file order, `.a2fc` load/
save via `loadDhgrImage`/`saveDhgrImage`). The nibble↔colour mapping is
`colour = rotl4(nibble, 1)` — derived from MAME's square-filter decode
(every dot of an aligned group reduces to that rotation) and **pinned by
`dhgr_paint_model`** against the real `renderDhgr` in ColorComp4Bit AND
ColorNTSC plus a lo-res palette cross-pin. Undo entries carry a 17-bit
address (bit 16 = aux plane); selection/text/palette-shift are 280-HGR-only
and disabled on every 16-colour page.

**One gate for "is this an HGR page", `sixteenMode()`.** `switchPage` sizes
the shadow per mode: DHGR takes the 16 KB pair, **DLGR the 2 KB pair**, and
everything else (HGR *and* GR) keeps the legacy 8 KB scratch. The HGR byte
interleave (`hgrByteOffset`) runs to `$1FF7`, so DLGR is the one page whose
buffer that overruns — GR is excluded from the interleave sites for meaning,
DLGR for memory safety. Anything indexing the shadow through the interleave
must therefore gate on `!sixteenMode()`, the single member enumerating the
16-colour pages. Three sites (palette-shift paint, the palette-seam overlay,
the `POM1HGR` save tag) spelled the test out as `grMode || dhgrMode`, which
the later-added DLGR page satisfied: out-of-bounds reads and writes on its
2 KB shadow, plus `emitShadowEdit` / the save tag poking the live machine
across text page 2, user RAM and HGR page 1 (fixed 2026-08-14). Unpinned —
those three are private members reachable only through `render()`, and
`hgrpaint/` has no headless harness; see TODO § Arch.

**DHGR image import — two models** (combo in the import preview):

- **560 dots (lookahead, default)** — `imageToDhgrPage560`: ii-pix's
  "4-pixel colour" model (ii-pix dropped 140px conversion in v1.1 as
  fundamentally wrong for DHGR). Every dot is chosen by per-byte-column
  analysis-by-synthesis: 128 candidate patterns per 7-dot column, searched
  by a branch-and-bound DFS (`DhgrColSearcher`, warm-started from the
  previous row) with the in-candidate linear-RGB error walk scored in
  CAM16-UCS via the local Jacobian, then 1-2 cross-column ICM refinement
  passes with ±2-column dirty tracking — the exact architecture of the HGR
  converter, simplified (no palette bit, no bit-doubling, no half-dot
  carry; the right context is candidate-independent). Candidates render
  through the module's own copy of **POM2's exact ColorNTSC DHGR decode**
  (`kDhgrNtscLut` = `Apple2VideoDecode.h` LUT row 0 + `rotl4b(absX+1)`),
  so the optimisation target is bit-identical to what the canvas shows —
  pinned by `dhgr_convert` (decode parity vs `renderDhgr` on random
  planes, exact solid fields with dither off, tone conservation dithered,
  monotone refinement). ~180 ms per photo conversion (vs ~4 ms for the
  block model) — fine for the live-slider preview. The resampler runs with
  `pixelAspect = 0.5` so fit/letterbox stays correct at 560 dots.
- **140 px blocks (Dazzle Draw)** — `imageToDhgrPage`: the aligned block
  quantiser (GR at DHGR resolution). Instant, produces clean 4-dot blocks
  that are easy to retouch with the editor tools, but half the resolution
  and fringing at colour seams.

**LUT provenance (2026-07-12).** POM1's GraphicsCard NTSC LUT turned out to
be MAME's **medium-color row 1** while POM2's ColorNTSC decodes **row 0** —
the source of a ~22 % importer/canvas divergence. POM2's copy of the HGR
scorer now carries row 0 (pinned byte-identical to `renderHiRes` in
`dhgr_convert`); POM1 parity for that array is deliberately dropped. The
16-colour quantisers' `kPalette` RGB values equal
`Apple2Display::kLoResPalette` — cross-pinned in `dhgr_paint_model`.

**2026-07-12 batch (17 items).** Everything below landed in one wave; the
"why" lives in CHANGELOG:

- **Modes**: DLGR pages (80×48 blocks over aux+main text pages; the aux
  nibble displays rotl4'd, so the model stores rotr4 — pinned vs
  `renderLoResDouble`), painted in a 560-dot logical space. Mode selector is
  now `switchPage(mode 0-3, page2)`, `Session::mode` matches.
- **Import models** (DHGR combo): 560-dot lookahead / 140-px blocks / 560
  mono / **NTSC 8-px chroma** (`imageToDhgrPage560Ntsc` — scores the
  86-colour trailing-8-dot ii-pix palette, `DhgrNtsc8Palette.cpp`
  BSD-2-Clause; causal model → no refinement; composite-target only, the
  preview warns). DLGR import = the GR quantiser at 80×48.
- **Tools**: 16-colour clip (copy/cut/paste in GR/DHGR/DLGR + FlipH/V/Rot90,
  mode-tagged `Clip::sixteen`), MacPaint 8×8 patterns (page-anchored;
  brush + filled shapes + 16-colour floods), X/Y mirror symmetry (only
  `applyPlot` — region ops use `applyPlotRaw`), DHGR text (fat 140-px
  glyphs), palette-shift & HGR-parity logic untouched — but DLGR joins the
  other 16-colour pages behind `sixteenMode()` at the three interleave
  sites (see the gate note above).
- **Canvas**: pipeline selector (host `canvasPipelines()` — NTSC / Medium /
  4-bit / Chat Mauve RGB; ChatMauve HGR's native 560-wide frame80 output is
  pair-averaged to the 280 canvas), 4:3 aspect option (all X maths goes
  through the `xs`/`af` factors; minimap has split X/Y scales), DHGR
  fringing overlay (rendered dots vs block colour), onion-skin tracing
  layer (fit/crop-aware placement + UV), flipbook page 1↔2 at N Hz + ghost
  overlay of the sibling page.
- **Screen holes**: GR/DLGR bulk ops mask $x78-$x7F per 128-byte group
  (peripheral scratch); text-page loads go through hole-skipping pokes.
- **Files**: DLGR = 2 KB aux+main pair; `browseDir()` homes to
  `prodos_folder/`, and `buildVolumeFromFolder` parses `NAME#TTAAAA` tags
  (type + aux/load address, pinned in `prodos_volume_smoke`) — the tagged
  default save names make pictures BLOAD-able by name in the synthesised
  ProDOS volume.
- **Sprite editor** (`src/hgrsprite/`, POM1 port, same host seam):
  scratch-page sprite drawing, grab/stamp vs the live screen, ca65 export.
  **DHGR target** (POM2): the mono shape stamps/grabs/previews/exports as
  140-px 16-colour pixels on the DHGR pair (transparent background;
  export = `name_aux`/`name_main` byte-pair tables).
- **Mono lo-res**: `renderLoRes`/`renderLoResDouble` render nibbles as
  their repeating 14 MHz bit patterns through the phosphor on the Mono*
  modes (absolute-sample indexing, same rule as `fillCompositeSignal`) —
  pinned in `dhgr_paint_model`. The canvas pipeline combo also offers the
  two composite demods (AppleWin Monitor / OE-CPU), which is how the
  NTSC-8-px import is previewed faithfully.
- **Session**: mode/page/zoom/NTSC/aspect/pipeline/dir persisted
  (`hgr_paint_*` settings keys).

## Host control center (Slot Configuration + Floppy Emu)

Two host-side facilities above the slot bus — neither is a bus
device. Both are data-in / actions-out ImGui panels driven from a
snapshot `MainWindow` builds under `stateMutex` and apply the
returned actions itself (mount/eject/persist/restart).

### MountableMediaCard + SlotCardCatalog

`MountableMediaCard.h` is the capability mix-in that lets the GUI
drive *any* card with mountable media bays generically — no
`if (cardKey == "...")` ladder. Orthogonal host-side interface
(NOT a bus concern). API: `bayCount()`, `bayInfo(bay) →
MediaBayInfo`, `mountBay/ejectBay/setBayWriteBack`, plus
`bayTypeOptions/setBayType` for bays whose kind the user may pick.

- `ProDOSBlockCard` implements as a single fixed bay → both
  HDV-class cards (`ProDOSHardDiskCard`, `CffaCard`) gain a bay
  free.
- `SmartPortCard` implements directly over its 2 units, advertising
  per-bay type (`""` empty / `"35"` 3.5" / `"hdv"` HDV).

`SlotCardCatalog.h` is the single list of user-assignable card types
(`kCardTypes`, index 0 = empty) + ROM-presence probes
(`mouseRomsPresent()`, `cffaRomPresent()`) that gate conditional
entries (Mouse needs both mouse ROMs, CFFA needs
`cffa20ee02/eec02.bin`).

### Slot Configuration + Internal Disks & Media

**Two windows, because they run opposite interaction models** —
`MainWindow_Slots.cpp` holds both. *Slot Configuration* (Machine →,
`renderSlotConfigPanel`) is **staged**: edits sit in a draft until
Apply, which restarts the machine. *Internal Disks & Media* (Devices →,
`renderMediaPanel`) is **immediate**: Mount / Insert / Eject act on the
running machine. They were one two-column window from 2026-05-25 (when
it absorbed the standalone "Slot Manager" — `SlotManager_ImGui.*`
removed) until **2026-07-28**. Sharing a window made Apply / Revert,
which sat at the bottom of the left column, read as governing the media
column too: mount a disk on the right, hit Revert on the left, and
expecting the mount to come back was a perfectly reasonable reading.
Banners (2026-07-27) narrated the split model; separate windows remove
it. Each window points at the other in its header text.

- **Slot Configuration — card assignment.** AUX 80-col row (IIe-class) + slots 1-7.
  Each slot a `kCardTypes` dropdown, EXCEPT profile built-ins
  (`builtInSlots[s]`) which render as locked, greyed `LabelText` with
  "card — built-in …" badge. `diskii` is multi-instance (never a
  duplicate); other keys red-flag duplicates and disable Apply.
  Apply persists `slot_N_card` and calls
  `restartEmulationFromSettings()`.

- **Internal Disks & Media — internal disks + mountable ports.** Live SlotBus walk
  (`bus.peripheral(s)`, no global `*Card` pointers, so correct with
  multi cards of a kind). For each plugged card:
  - `dynamic_cast<MountableMediaCard*>` → render bays inline:
    status dot (grey empty / orange WP / green loaded / red error),
    per-bay type select (SmartPort), path InputText + Mount/Eject,
    write-back, Boot slot. Covers SmartPort (2 units), CFFA + HDV
    (1 bay).
  - else `dynamic_cast<DiskIICard*>` → internal 5.25" drives (1-2),
    each with path + Insert/Eject, Boot slot. Drive 1 persists to
    `disk_path_slotN`; drive 2 is session-only.

  Each media action takes `stateMutex` and calls `persistMediaBay()`
  (per-unit/per-slot/global keys), then `settings->save()`.

Settings: `show_slot_config` + `show_media_panel` (both persisted;
both cleared in kiosk). Command palette: `panel.slotconfig`,
`panel.media`. Pinned: `slot_multi_card_smoke_test`.

### Abstraction Levels panel (LLE / HLE)

`AbstractionLevels_ImGui.{h,cpp}` (Help → Abstraction Levels,
`show_abstraction`, palette `panel.abstraction`), fed by
`MainWindow::renderAbstractionPanel`. It is the live face of
[`docs/lle_vs_hle.md`](docs/lle_vs_hle.md) — read that for the taxonomy; what
follows is only the split.

**Static catalog vs live state.** The subsystem table (id, group, level, what
is modelled, why not lower, files) is static data next to the window, mirroring
the doc's master table. Everything machine-dependent comes in as a `Snapshot`
the caller fills, so the panel needs no emulator headers and takes no lock.
Plug state is read from `slotCards[]` rather than from the dozen `*Card`
pointers: one uniform test that also covers the cards MainWindow keeps no
pointer to.

**The "Now" column is the reason it exists.** Every ROM-driven low level in
POM2 degrades *silently* to a working higher one when its dump is absent, and
`docs/lle_vs_hle.md` § "Keeping a level once you have it" names that as a
structural hole. The column reports **degraded**, not merely missing, from the
card accessors: `DiskIICard::usingBitLss` (the honest test — a mounted WOZ
forces the bit-level path even with no `diskii_p6.rom`, using the embedded
default P6), `ClockCard::romFromDump`, `GrapplerCard::isRomLoaded`,
`SmartPortCard::isLironRomLoaded`.

**Four switchable boundaries**, expressed as a choice of *level* rather than of
catalog key: Mouse Card (L0 MAME ⇄ H1 AppleWin), ProDOS block storage (L2 CFFA
⇄ H1 HDV), printer interface (L2 Grappler+ ⇄ H1 synthetic), colour pipeline
(L1 OpenEmulator ⇄ H1 artifact LUT). The first three go through
`MainWindow::swapSlotCardVariant`, which swaps the card **in place, in the slot
it already occupies** (moving it would be a second unasked-for change, and slot
numbers are baked into most software) and then rebuilds via
`restartEmulationFromSettings` under the same persist/rollback contract as Slot
Config's Apply. The fourth is a render-path change and is instant. A side whose
dump is missing is greyed: offering a switch that would silently land on the
fallback would repeat the exact mistake the panel exists to expose.

### Apple //e keyboard panel

`Keyboard_ImGui.{h,cpp}` + the generated `AppleIIeKeyboardLayout.{h,cpp}`
(Devices → Apple //e Keyboard, `show_keyboard`, palette `panel.keyboard`).
A photo of a real //e keyboard (`pic/Keyboard_AppleIIe.jpeg`, shipped via
`packaging/bundle.manifest`) with one hotspot per cap. Point: the keys a host
keyboard has nowhere to put — Open-Apple, Solid-Apple, the //e's own Reset —
are reachable with the real legends on them.

**The hotspots are measured, not drawn.** `tools/gen_keyboard_layout.py` reads
the photo, takes a **75th-percentile** column profile through the middle of
each key row and cuts at the dark valleys between caps. The percentile matters:
this is a European //e whose caps carry two legends (French over US), and the
glyphs put enough dark pixels mid-cap to split one key into two under a median.
Rects are stored as **fractions of the 2578×908 image**, so they track the
picture at any window size and a re-crop means re-running the script instead of
nudging constants. `Show hitboxes` in the panel is the visual check.

Three details the layout encodes:

- The **L-shaped Return** is two rects sharing one `id` — hover tests per rect,
  highlights per id, so either arm lights both.
- The extra **ISO key** at the left of row 4 (legends `> |` / `< \`) is the US
  backslash; **Reset** gets an absolute rect because it sits in its own recess,
  mounted lower than the row-1 caps.
- The photo draws **both** horizontal arrow caps pointing left. That is an
  error in the picture: the table follows the hardware (← then →) and the
  tooltips name each one.

**Latches, not chords** — a mouse has one pointer, so Ctrl+Reset cannot be
clicked simultaneously. Shift and Control are one-shot (cleared by the next
character); Caps Lock and the two Apple keys stay down until clicked again.
Caps Lock defaults **on**, like the real machine's mechanical latch, and
uppercases only A-Z (it is a letter latch, not a shift — which is why the
number row still needs Shift for its symbols). The Apple keys are *levels*:
`renderKeyboardPanel` pushes them to `$C061`/`$C062` every frame while latched,
and releases them when the window closes, so a latched Open-Apple cannot
outlive the window that shows it as down.

**Two sources on one wire — `AppleKeyLatch.h`.** `$C061`/`$C062` bit 7 is
pressed by both the host's Left/Right Alt (`onKey`) and this panel's latches,
so the two halves are stored apart and OR'd in `pushAppleKeys()`; no writer can
express "…and release the other one". They used to assign the shared latch
directly, and because the panel republishes every frame — and because
`keyboardPanel` is never destroyed once built, so its *closed* branch went on
clearing both wires for the rest of the session — **opening the keyboard window
once and closing it disabled Left/Right Alt permanently**: Open-Apple+Ctrl+Reset
stopped cold-booting and every title reading bit 7 as button 0/1 stopped seeing
the keys. It read as an emulator fault rather than a UI one because
`Memory::memRead` ORs the paddle buttons into the same case, so a real joystick
kept working throughout. The close-time release is now edge-triggered on
`kbPanelWasOpen_` as well. Header-only and emulator-free, pinned by
`apple_key_latch`.

**Reset refuses to fire without Control**, exactly as the hardware does — RESET
is wired through the encoder's Ctrl line so a stray knock cannot reboot the
machine. Ctrl+Reset → `softReset()`, Open-Apple+Ctrl+Reset → `hardReset()`,
the same two verbs as F11 / F12. The Del cap sends **$7F**, which is what the
//e's DELETE key generates — not the $08 the host Backspace injects (that is
the left arrow's code, which is what a II/II+ had instead of a DELETE key).

### ROM Status panel

`RomStatus_ImGui.{h,cpp}` + `RomCatalog.h` + `RomFetch.{h,cpp}`
(Help → ROM Status, `show_rom_status`, palette `panel.romstatus`).
Host-side only: stats files, hashes bytes, takes no lock, and rescans
on demand (open / Rescan) rather than per frame.

**Download missing from RetroBIOS.** The button fetches the Apple II
dumps POM2 actually probes from
[Abdess/retrobios](https://github.com/Abdess/retrobios/tree/main/bios)
(`bios/Apple/Apple II` plus the MAME card zips under
`bios/Arcade/MAME`). The mapping lives in `romFetchCatalog()` — every
`destRel` is a path `SystemProfile` / `RomCatalog` / `CharRomCatalog`
already looks for (pinned by `rom_fetch`). Existing files are skipped
(`findResource`); new ones land in `writableRomsDir()` (the first
writable `roms/` on the search path, else the per-user data dir).
HTTPS is the system `curl`; MAME zips go through `unzip` or `tar`.
The collection has no //c / //c+, Liron or TransWarp dump — those
rows stay missing. Disabled under Emscripten (no helper processes).

**Two sources, neither duplicated.** Machine firmware and character
generators are read from `profileConfig()` (`romProbeOrder` /
`charRomProbeOrder`) for every entry of `allProfiles()`, so a new
profile shows up with no edit here. The peripheral side is
`RomCatalog.h`, which mirrors each card's probe list at its plug site
(MainWindow.cpp / ClockCard.cpp) and adds the two things the code
can't express: the required size and *what POM2 does when the dump is
absent* — most card ROMs degrade (synthetic stub, embedded default)
rather than fail, and that is the column users actually need.

Verdicts are deliberately unequal:

- **Missing** — error for machine firmware (the profile can't start),
  warning elsewhere.
- **Size** — the only hard check. 256 B PROM, 4 KB EPROM: a mismatch is
  a wrong file, not a variant.
- **CRC32** — always shown for identification, *judged* only where
  `RomCatalogEntry::knownCrc` names a dump POM2 can vouch for (the two
  CFFA 2.0 images from dreher.net). Asserting an unverified checksum
  would turn legitimate variants into false alarms.
- **`(fallback)`** — the probe resolved, but not to its first choice.
  This is the //e Unenhanced profile silently running Enhanced firmware
  when `apple2e_unenh.rom` is absent — previously only a log line.

CRC-32 (IEEE, reflected) is implemented locally: POM2 links no zlib,
and the WOZ path only ever writes the "not computed" sentinel, so
there was nothing to borrow.

### Floppy Emu (BMOW)

`FloppyEmuDevice.{h,cpp}` + `FloppyEmu_ImGui.{h,cpp}` — model of the
BMOW **Floppy Emu** (bigmessowires.com): SD-card + OLED + 3-button
gadget that plugs into the disk port and *becomes* a drive. POM2
already emulates every drive type the Emu presents → the class models
the device's *defining* behaviour, not another FDC:

- persistent emulation **MODE** (NVRAM): 4 `FloppyEmuMode`s mapped
  onto POM2's drives: `Disk525` (140 K, Disk II), `Disk35` (800 K
  dumb 3.5"), `Unidisk35` (800 K smart, ejectable), `SmartportHD`
  (≤32 MB ProDOS block). Dual-5.25 and Smartport-Unit-2 (IIgs
  daisy-chain) modes out of scope.
- SD-card **file explorer** — bounded to SD root, `..` + dirs-first,
  case-insensitive, format-filtered per mode (`acceptsFile`: 5.25 →
  dsk/do/po/nib/woz/2mg; 3.5/Unidisk → dsk/do/po/2mg; Smartport →
  po/hdv/2mg).
- **favorites** — `favdisks.txt` in SD root: optional `automount N`
  first line (0 never / 1 first / 2 most-recent) then one image path
  per line (relative to SD root or absolute), matching real device.

Actual mounting **routed by MainWindow** into existing controller
cards (`DiskIICard` for 5.25/3.5, `SmartPortCard` units for HDV) —
device only picks the image + the mode. Core is UI/emulator-agnostic
(no ImGui / MainWindow / SlotBus) so format filtering, SD navigation,
and favdisks parsing unit-test in isolation. Ref: BMOW Floppy Emu
Model C manual §3 + §5.

`FloppyEmu_ImGui` draws the device's face: stylised 128×64
blue-on-black OLED + 3 hardware buttons (PREV / NEXT / SELECT), two
OLED views (SD File Explorer + Settings → Disk Emulation Mode).

Virtual "SD card" = `floppyemu/` (separate from Disk Library folders).
Settings: `floppyemu_mode`, `floppyemu_sd_root`, `show_floppy_emu`.
Pinned: `floppy_emu_smoke_test`.

## Profile switching internals

`SystemProfile.h/.cpp`. Pinned: `system_profile_smoke_test`.

**Fresh-install default: `iie-pal`.** With no `system_profile` key in
`state.cfg`, `MainWindow`'s ctor feeds `"iie-pal"` as the *default value* of
that `getString` rather than initialising `activeProfile` to it. That is
deliberate: `activeProfile` is set from the ROM auto-probe (//e if an //e ROM
resolved, else II+), and the branch below it only runs `applyProfile` when the
resolved key **differs** from the probe. Expressing the default as a saved-key
stand-in makes it differ, so `applyProfile` runs — and `applyProfile` is what
actually installs the PAL video standard, the 20313-cycle frame budget and the
`setCpuClock` sweep over every slot card. Setting `activeProfile =
AppleIIePAL` directly would skip all of that and leave a //e running at 60 Hz
while the UI claimed PAL. The default degrades to the auto-probe when no //e
ROM was found (`iiePresent == false`), and `--ii-plus` still wins over both.

The matching **default slot map** is `kDefaultCards[]` in
`SlotConfigurationCoordinator.cpp:49`, consumed by its `resolve()` at `:113`
and applied by `MainWindow::plugSlotsFromSettings` (which now lives in
`MainWindow_SlotConfig.cpp:105`): sl1 `grappler`, sl2 `mockingboard`, sl3
empty, sl4 `mouseaw`, sl5 `smartport35`, sl6 `diskii`, sl7 `chatmauve` — a
default only, overridden by any `slot_N_card` key. **Slot 3 is empty by
design**: on a //e the 80-column card is not a slot card at all (internal
`$C300` firmware + the AUX-connector ext80, both carried by `iieMode`), and a
card there also fights the SLOTC3ROM switch. One consequence of moving slot 4
off `clock`: the legacy `clock_card_enable=false` opt-out in the same function
is now inert unless the settings file also names `clock` in slot 4.

**32 KB ROM disambiguation**: //e and //c dumps share 32 KB but
encode firmware in OPPOSITE halves. `loadAppleIIRom` takes a
`pickLower16KFor32K` flag set by `applyProfile`:

- //e (`apple2e.rom`): firmware in UPPER 16 KB (file `0x4000-0x7FFF`),
  lower = character ROM. `pickLower=false`.
- //c / //c+ (`apple2c-32Kv0.rom`, `apple2cp.rom`): TWO 16 KB banks.
  Bank 0 in LOWER half (mapped at reset, cold-start at $FA62), bank 1
  in upper (alt firmware: AppleTalk, MouseText, SmartPort).
  `pickLower=true`; upper stashed into `IIcClassProfile::altFirmware_`.

Both halves can carry valid-looking reset vectors → can't auto-detect
from bytes. **Profile is source of truth.** When the generic
`apple2.rom` fallback resolves because no profile-specific dump is
present, the loader emits a warning.

**$C028 ROMBANK** (//c-class): MAME `apple2e.cpp:1907-1923` flips
`m_romswitch` on any `$C02x` access when `m_isiic`. POM2 mirrors via
`isIIcClass`. Alt-firmware read paths additionally require
`IIcClassProfile::hasAltBank_` (32 KB only). `resetSoftSwitches` clears `iicRomBank`
so cold-boot starts in bank 0. On II/II+/IIe, `$C02x` falls through
to cassette. Pinned: `system_profile_smoke::testIicRomBankSwitch`.

**//c-class INTCXROM override**: //c/+ have no physical slots →
internal ROM always at `$C100-$CFFF`. POM2 gates `internalIORom`
dispatch on `(MF_INTCXROM || //c-class)` (MAME `apple2e.cpp:1619-1631
update_slotrom_banks`). `loadAppleIIRom` and `resetSoftSwitches` set
`iieMemMode |= MF_INTCXROM` on //c-class. Pinned:
`testIicInternalRomAlwaysMapped`.

**Built-in slot locks** (`ProfileConfig::builtInSlots`): each profile
carries `std::array<std::optional<BuiltInSlot>, 8>`. //c and //c+ both
lock the same five slots — sl1 + sl2 (the two on-board serial ports as
`ssc`), sl4 (`mouseaw`), sl5 (`smartport35`) and sl6 (`diskii`); sl3
and sl7 stay free. `plugSlotsFromSettings` overrides `slotCards[s]` with forced
cardKey regardless of persisted `slot_N_card`. `renderSlotConfigPanel`
renders locked slots disabled with "built-in" badge. Pinned:
`testBuiltInSlots`.

**//c+ MIG + IWM handshake** (//c+-only): alt firmware (bank 1)
drives a MIG gate-array + IWM. POM2 models the minimum for cold boot:

- **MIG** (MAME `apple2e.cpp:598-704 mig_r/mig_w`). Profile hosts
  `migRam[0x800]`, `migPage`, `migIntDrive`, `migHdSel`; routes two
  MIG windows in bank-1 expansion ROM **only when `isIIcPlus &&
  iicRomBank`**:
  - `$CC00-$CCFF` → `migOffset 0x000-0x0FF` (drive enable/disable,
    IWM reset)
  - `$CE00-$CEFF` → `migOffset 0x200-0x2FF` (MIG RAM + auto-incr,
    3.5" head select, MIG page reset)

  3.5"-side decodes → `SmartPortHub::setMig35Sel`/`setMigIntDrive`;
  hub's `recalc_active_device` (verbatim MAME `apple2e.cpp:724-770`).
  MAME `:1917-1922` resets `migPage + m_intdrive + m_35sel` on
  ROMSWITCH → bank 0; POM2 mirrors. `migWrite(0x40)` calls
  `iwmDevice->reset()` so alt firmware's per-boot IWM reset clears
  stale state.

- **IWM mode register + WHD handshake** on `DiskIICard` (MAME
  `iwm.cpp:103-114 read / 256-269 mode_w`). DiskIICard tracks
  `iwmMode` + resting `iwmWhd = 0xBF` and intercepts `$C0nE/$C0nC/
  $C0nF` combos in both LSS path and legacy gate. Plain Disk II
  software never drives Q6+Q7 to mode-set state, so existing tests
  unaffected; alt firmware's IWM probe at `$E512-$E522` and
  write-ready loop at `$C8A6-$C8A9 / $C960-$C965` both clear with
  these hooks. Without them //c+ Monitor cold-reset hangs before any
  banner.

**Profile switching = full cold reset** via
`MainWindow::applyProfile(SystemProfile)`. Order matters. The steps below are
numbered as the `applyProfile` body numbers them (**0-13**), so a "step N" in
this file, in `CLAUDE.md` or in a code comment always means the same step:

0. Commit `activeProfile` — **before** step 7, which reads it to force the
   profile's built-in locked slots.
1. Stop worker (already stopped above the numbered block, before the media
   flush) and clear the rewind ring: the ring recorded the previous machine.
2. Snapshot the currently-mounted media so step 8 can re-mount it.
3. Tear down slot cards under state mutex (Mockingboard's
   `AudioSource` detached from `AudioDevice` FIRST).
4. Cold-reset memory: wipe RAM/aux/LC + reset soft switches, with
   **`setIIEMode(...)` FIRST** (so `clearRam()` sees the right aux
   configuration, and so `loadAppleIIRom` at step 5 lands in the right map).
5. Load the main ROM (with `pickLower16KFor32K` for the //c / //c+ 32 KB
   two-bank dumps).
6. Char ROM — the toolbar's `charRomLocale` wins over the profile probe.
7. Re-plug slots from settings (built-in locked slots override
   `slot_N_card`).
8. Re-mount the media preserved at step 2.
9. `resolveCpuMode()` (honours `cpu_mode_override`, clamped on //c-class).
10. Default cycles/frame **+ `setVideoStandard()`** (NTSC 60 Hz / PAL 50 Hz).
11. `hardReset()`, then restart the worker.
12. Persist `system_profile` (skipped under kiosk, which is read-only).
13. Refresh GLFW window title.

CLI `--preset` triggers the same path (after legacy auto-probe —
wins). Aliases: `apple2`, `apple2plus`, `iie-u` / `iieunenhanced` /
`apple2e-1983`, `apple2e`, `apple2c`, `apple2cplus`, `//e-u`, `//e`,
`//c`, `//c+`, plus the PAL keys `iie-pal` / `iiepal` / `apple2e-pal` /
`//e-pal` and `iic-pal` / `iicpal` / `apple2c-pal` / `//c-pal` /
`chatmauve`. `cpu_mode_override` = `auto|nmos|65c02`.

## CLI (CliDispatcher)

`CliDispatcher` (parser, no `EmulationController` dep) + `CliRunner`
(Phase-C runner — split out so parser is unit-testable). Three
phases: **A** parse, **B** pre-boot (preset / ROM / display / speed),
**C** post-boot deferred actions (`--load addr:file`,
`--snapshot-load`/`--snapshot-save`, tape ops, paste, run, step).

Flags: `--preset ii|ii+|iie-u|iie|iic|iic+|iie-pal|iic-pal`, `--speed`,
`--cpu-max`, `--display`, `--ii-plus`,
`--tape`, `--save-tape`/`--save-tape-format aci|wav`,
`--35-disk1 path`/`--35-disk2 path`, `--load addr:file`,
`--run`, `--paste`, `--step`, `--play`/`--rec`/`--rewind`,
`--snapshot-save`/`--snapshot-load`,
`--fujinet[=PORT]`/`--fujinet-serial[=DEV]`/`--fujinet-slot N`,
`--rgb-card-invert-bit7[=on|off]`, `--trace-brk` (logged no-op).
`printUsage()` is the source of truth.

**Positional disk + `--kiosk`**. First non-flag arg → `CliPlan::
bootDiskPath`; `--kiosk` → `CliPlan::kiosk`. `main.cpp`:

- Picks slot by content via `classifyDiskForSlot(path)`, then calls
  `MainWindow::insertAndBootImage(path, err)` (shared with Disk
  Library UI; `routeMount35`/`routeMountHdv` are `MainWindow` methods
  so both callers route identically — SmartPort unit auto-create,
  //c+ on-board hub, HDV card vs SmartPort unit 0). 5.25" →
  `DiskIICard::insertDisk` + `bootFromSlot`.

- **HDV auto-provision**: an HDV needs an HDV/SmartPort card. A saved
  config may have only Disk II cards. `ensureHdvCardForBoot()` plugs a
  `ProDOSHardDiskCard` into a free slot (prefers 7) for the session
  if none present. Plug **not persisted** — user's GUI config stays
  untouched.

- **No persistence in kiosk**: `~MainWindow`'s `settings->save()` is
  gated `if (!kiosk_)`. `imgui.ini` is also disabled. Bare `POM2
  <disk>` in GUI *does* persist.

- Defers boot to small frame countdown in main loop (UI thread between
  frames, after worker is up + slots plugged) → no race with CPU
  thread.

- `--kiosk` → exclusive full-screen from primary monitor's video mode
  (`glfwGetVideoMode` + `glfwCreateWindow(.., monitor, ..)`, copying the
  mode's bit depths + refresh into the hints so it's a windowed-fullscreen
  with no mode switch); `io.IniFilename = nullptr`; `setKioskMode`.
  No monitor / video mode → warns + falls back to a windowed canvas.
  `render()` short-circuits to `renderKiosk()` — one borderless
  full-viewport window (`drawScreenImage()` letterboxed on black), no menu
  / toolbar / panels / dialogs. **Quit = Alt-F4**, handled explicitly in
  `glfw_key_callback` (`main.cpp`): `key == GLFW_KEY_F4 && PRESS && (mods &
  GLFW_MOD_ALT)` → `glfwSetWindowShouldClose`, so it works even in exclusive
  full-screen where no chrome offers a way out and some WMs don't intercept
  the combo. Feeds the normal clean-shutdown path (pending saves / tape
  dumps still run). No Escape-to-quit.

- **What still runs in kiosk**: `render()` calls
  `pollJoystickAndPushToMemory()` + `updateAutoTurbo()` before the
  `if (kiosk_)` short-circuit, and the short-circuit itself keeps
  `driveRewindHold(F6)`, so joystick/paddles, disk auto-turbo, and F6
  hold-to-rewind behave identically **while the in-game menu is closed** —
  F6 is deliberately inert with the menu open (a `releaseHold` would end in
  `rewindEndAndResume` → `Mode::Running` behind the paused overlay;
  `updateKioskMenu` also re-parks the worker every frame a pause is wanted,
  as a belt-and-braces against anything else resuming it). The
  unconditional global keys (F11/F12 reset, F9 screenshot, Left/Right Alt =
  Open/Solid Apple — `main.cpp` `isGlobalKey`, routed even when ImGui has
  keyboard focus) still reach the guest — except while the menu is open:
  `onKey`/`onChar` early-return on `kioskMenuOpen_`, because the menu's
  keyboard fallbacks are polled via `ImGui::IsKeyPressed` and the overlay
  never captures the keyboard, so every menu navigation key would otherwise
  ALSO land in the $C000 latch (Enter double-typed, Esc delivered a stray
  $1B on resume). Only the chrome (menu/toolbar/panels, and their
  toolbar-only actions) is gone.

- **`POM2_AUTO_QUIT=<N>`** (env, `main.cpp`) requests
  `glfwSetWindowShouldClose` after N seconds — a general headless-run /
  automation self-quit hook.

- **Kiosk in-game menu** (`openKioskStartMenu` / `updateKioskMenu` /
  `renderKioskMenu`, MainWindow; pages `KioskPage::{List,Keys,RomDirs,
  Browse,Quit}`). The pad's **Start** (standard GLFW gamepad mapping via
  `JoystickInput::UiNav`) — or **F1** as a fallback when the pad has no SDL
  mapping (F10/Ctrl+Alt+F are the full-screen toggle, so using either here
  would open the menu in the same frame the user entered kiosk) — opens a
  two-zone Start menu: **GAMES** lists every image
  `classifyDiskForSlot` recognises (5.25"/3.5"/HDV) across the booted
  disk's folder + the persisted extra ROM folders (`kiosk_romdirs.txt`,
  outside the read-only `state.cfg`), sorted by name-proximity so the
  mounted title's other sides float to the top (● marks the mounted disk,
  matched canonically so relative launch paths still hit). **ACTIONS**
  holds Restart (`bootFromSlot`) / Keyboard / ROM folders / Quit.
  Activating a 5.25" hot-swaps it **in place, no reboot** (`insertDisk`
  under `stateMutex` — flip-disk gesture) and keeps the menu open so a
  Restart can follow; a 3.5"/HDV routes through `insertAndBootImage` and
  boots immediately. **Select** (or **K**) toggles the live keyboard band
  (machine keeps running; grid cells go through `Memory::queueKey`).
  **B/Esc/Start** dismiss. Every Start-menu page parks the worker
  (`kioskSetPaused` → `Mode::Stopped`, speaker flushed on resume); the Keys
  band does not. Menu→game input isolation is two-sided: while open,
  paddles/buttons are fed centred/released and `onKey`/`onChar` are gated;
  on close, `kioskSwallowPad_` keeps the shared face buttons + D-pad
  (Circle/Cross double as menu B/A **and** Apple PB0/PB1) suppressed until
  the pad is fully released, because the poll samples `kioskMenuOpen_` one
  frame behind `updateKioskMenu`. The overlay omits
  `NoBringToFrontOnFocus` and calls `SetNextWindowFocus()` so it sits above
  the opaque full-viewport kiosk window; text is `SetWindowFontScale(5.0f)`
  — re-applied inside the list child (a child is a separate ImGui window
  with its own scale).

Pinned: `cli_kiosk_test` — a **parser-only** smoke test (links against just
`DiskImage.cpp`): it asserts `parseCli` captures the positional disk +
`--kiosk` flag and `classifyDiskForSlot` picks the slot; it does not drive
the full-screen window.

## Clock & threading

`POM2_CPU_CLOCK_HZ = 1 022 727` (14.31818 MHz / 14). 65-cycle "long
cycle" TV alignment NOT modelled. Three modes in
`EmulationController`: **Stopped** (50 ms idle), **Running**
(`cyclesPerFrame` per 60 Hz tick), **Step** (one instruction).
`M6502::run(maxCycles)` returns *actual* cycles → passed to
`Memory::advanceCycles()` so paddle RC stays synced. Single
`stateMutex` guards CPU + Memory.

CPU → audio/UI events carry an `emuCycles` stamp. Consumers measure
cadence in emulated CPU cycles, not wall-clock frames (disk-turbo
bumps the CPU to ~60×, which collapses wall-clock gaps to zero
across an audio-buffer tick). Canonical example:
`FloppySoundDevice::drainCommands` uses the cycle stamp passed by
`DiskIICard::seekPhaseW`.

### Debugger (`Debugger.h/.cpp`, `Debugger_ImGui.*`)

Breakpoints, single-step, step-over, run-to-cursor, and a stop reason. What it
is *for* is not the end user: it is the instrument POM2 is debugged with.

The 2026-08-22 architecture audit put a number on the gap. `tests/` had
accumulated **24 one-off trace / dump / probe binaries, 5 535 lines**, of which
**4 129 lines were registered as no test at all** — built on every build,
executed by nothing (`choplifter_iie_trace`, `cffa_boot_dump`, `dd2_ay_trace`,
`iic_boot_trace`, `drol_probe`, `hero_probe`, `u5_woz_save_probe`, …). Every
parity hunt was paying for a throwaway binary because there was no way to stop
a running machine and look at it. That is the cost this closes.

**Layering.** The CPU does not know about the debugger. `M6502.h` declares a
two-method interface, `M6502DebugHook`, and `Debugger` implements it — so the
dependency runs one way and `M6502.cpp` needs no new include.
`EmulationController` owns the `Debugger`, and `Debugger_ImGui` owns every
decision the panel makes. `MainWindow.cpp` gained **six lines**: three to own
and show the panel, three to register it with the palette and the View menu.
That was not free — the file-size ratchet failed the build and made the budget
edit visible in the same commit, which is exactly what it is for.

**Where the stop happens.** `runCpuSlice` is the single funnel both drivers
(the worker thread and the WASM RAF tick) go through, so the check lives there
once instead of in each. The worker's inner loop re-reads `mode` at the top of
every iteration, so parking inside the funnel stops the machine within one
4096-cycle chunk.

**A breakpoint stops BEFORE its instruction.** The hook is called with the PC
still on the instruction, which has not run. That is the difference between a
register dump showing the state going *in* and one showing the state coming
*out*, and only the first is useful. It has a second consequence that is easy
to get wrong: resuming has to grant the current PC one instruction of amnesty
(`armResumeFrom`), or Run re-triggers the same breakpoint forever and the
button looks dead.

The `debugger` test pins both, and case 2 took two attempts to write. "Check
before the instruction at PC" and "check after the previous instruction" leave
the machine in the *same* state everywhere except one place: a breakpoint on
the entry PC of a run. Check-before stops immediately with nothing executed;
check-after runs the instruction first and then never matches, missing the
breakpoint entirely. That is run-to-cursor on the current line, and a loop
re-entering its own head — so the test now breaks on `kStart`, and a hook moved
after `step()` fails it.

**Cost when nobody is debugging: none, measured.** `M6502::run` picks between
two loops once per *call*, so the un-armed cost is one branch per 4096-cycle
chunk. `syncDebugHook()` keeps the hook detached until something is armed.
Three `pom2_bench` workloads, identical RAM hashes, no measurable change —
docs/PERFORMANCE.md § 8.

**An idle hook is detached at the next run slice** *(2026-09-07)*. "Keeps the
hook detached until something is armed" was true only of the paths that call
`syncDebugHook`. `M6502::step` gates its interrupt-entry split on
`debugHook_ != nullptr`, **not** on "is anything armed" — the split changes how
the entry's 7 cycles reach `memory->advanceCycles` (two small advances instead
of one sum), and that moves the sub-instruction phase every lazily-synced
peripheral derives from: Mockingboard/Phasor T1, the Disk II LSS, the video
beam. A step-over or run-to-cursor attaches the hook, and the transient that
armed it is dropped by `setMode(Mode::Stopped)`, which deliberately does not
re-sync (it must not take the lock). So after **one** step-over plus Run the
machine kept a debugged CPU for the rest of the session, with nothing armed
and the interrupt phase perturbed — the phase OLDSKOOL races against.
`runCpuSlice` (`EmulationController.cpp:1104-1106`) now reconciles instead:
two relaxed loads per 4096-cycle chunk, running exactly once per transition
(afterwards the hook *is* null), and covering every resume path — toolbar
Play, Machine ▸ Run, the palette, the kiosk menu — instead of only the
debugger panel's. Pinned by `debugger` and `mockingboard_t1_irq_phase`.

**Write watchpoints, and the trick that made them free** (2026-08-23). The
naive tap — one pointer test at the top of `memRead`/`memWrite`, call a sink
when it is set — measured **+13.4 % / +16.5 % / +9.2 %** and was thrown away
(PERFORMANCE § 8.2). What shipped instead adds *nothing at all* to the fast
path, because it does not test for a watchpoint there: it **removes the
address from the fast path**. `memWrite`'s hot case already consults a
per-address `writable[]` byte, so arming a watch clears that byte and the
write falls into `memWriteSlow` on its own. The slow path reports the access
and performs the write using the REAL permission, shadowed beside the armed
bit in `Memory::writeWatch_` (empty, and allocated, only while somebody is
debugging). Measured on three `pom2_bench` workloads with identical RAM
hashes: no change — PERFORMANCE § 8.3.

Three things that design has to get right, all pinned by `debugger` cases 7-9:

* **The write must still land.** A diverted address is write-protected as far
  as `writable[]` is concerned; forgetting the shadow would silently corrupt
  the machine under the debugger's nose rather than merely failing to stop.
* **The diversion must not invent permission.** A watch on a ROM address
  reports the access and still drops the write, and `markRomRegion` — which a
  profile switch calls while a watch may be armed — updates the shadow, not
  just `writable[]`.
* **A state restore must ignore it.** `restoreMainRam` skips non-writable
  cells so a snapshot cannot clobber the ROM mirror; it consults
  `ramWritable()` so a watched byte is not the one cell a rewind silently
  refuses to restore.

$C000 and above needs no diversion at all — those writes already reach
`memWriteSlow` — so soft switches, slot I/O and the language card are watchable
for free. The watch is on the ADDRESS, not the bank: on a //e it fires whichever
of main/aux the paging picks. And it fires on the ACCESS, including a write the
machine then drops.

`Memory` reports through `MemoryWatchSink` (`MemoryWatchSink.h`), a two-line
interface, rather than calling `Debugger` directly: `Memory.cpp` is linked into
two dozen test binaries and a benchmark, and none of them should have to pull
the debugger in behind it. The arm / disarm bookkeeping for both halves lives
in `MemoryWatch.cpp` (linked beside `Memory.cpp` everywhere, like
`MemoryProfile_IIcClass.cpp`): a concern of its own, and `Memory.cpp` is a
god-object the file-size ratchet refuses to grow. The two hot-path pieces stay
where they must — the `writable[]` test in `memWrite`, and the `memReadSlow`
report wrapper in `Memory.cpp`, whose slow body is force-inlined into it.

The stop lands at the first instruction boundary AFTER the access, which cannot
be un-done — so `Hit::pc` (latched by `onInstruction` as `curPc_`) names the
instruction that *wrote*, while the machine's live PC is the instruction after
it. Both facts are in the stop banner.

**Read watchpoints** (2026-08-23) work by a coarser mechanism, because
`memRead`'s fast path has no per-address table to hide a watch in. One flag,
`Memory::readDivert_`, is true while ANY read watch is armed; while it is,
every read falls through to `memReadSlow`, which performs the read and then
reports the watched ones (`Memory::readWatch_`, one byte per address, empty
until the first arm). The flag is never *tested* on the fast path — a test
there measured +7.2 % — it is folded into three derived bytes that replace
tests the path already made (`plainRead_` for `!iieMode || testMode`,
`iieFastRead_` for `!bankTrace_`, `romFastRead_` for `!iicProfile_`), kept
current by `refreshReadFastFlags()` at every writer of a source flag. Un-armed
cost: none (+0.0 % / −3.0 % / −0.5 %); armed: every bus read out of line,
+11 % to +55 % depending on the workload, paid only while armed. Numbers and
the two traps (the flag test, the un-inlined slow body) in
[PERFORMANCE § 8.5](docs/PERFORMANCE.md). A read watch fires on the bus
ACCESS after the read, opcode fetches included — a watch on `$FBB3` answers
"who checks the ROM ID byte" — and soft-switch reads included, with their
side effects, as on the real bus; the memory viewer peeks `mem[]` and never
fires. The panel offers R / W / RW and defaults to W, which is free in both
states. Pinned by `debugger` cases 10 (stop, reader named, value carried,
opcode fetch, soft switch, disarm reopens the fast path) and 11 (an unwatched
read under an armed watch does not stop).

**Known limit.** Step-over reads the opcode at the PC with `peekMainRam`, not
`memRead` — a debugger must never flip a soft switch to inspect the machine —
so it sees main RAM only, the same view the Disasm panel and MemoryViewer
already show. On a //e running from aux, or under a Language Card bank, it can
misread. Both failure directions are benign: a missed JSR becomes a single
step, and a phantom JSR arms a transient that never fires.

### Thread exception barrier (`ThreadGuard.h`)

An exception that escapes the callable of a `std::thread` propagates nowhere:
it calls `std::terminate()`, which kills the process with **no log line, no
message and no snapshot**. To the user that is indistinguishable from a
segfault, and to you it is a bug report with nothing in it.

POM2 spawns a dozen long-lived threads — the CPU worker
(`EmulationController.cpp:559`), the 3.5" write-back queue (`Disk35WriteBack`,
`EmulationController.cpp:444`), the SSC telnet worker
(`SuperSerialTcpTransport.cpp:103`), the FujiNet SP link
(`SpOverSlipLink.cpp:140`), the FujiNet HTTP fetch worker
(`FujiNetNetDevice.cpp:365`), the print-history writer
(`PrinterHistory.cpp:373`), the AI control server (`AiControlServer.cpp:515`),
the LaserWriter/Ghostscript spooler (`PostScriptRender.cpp:501`), the
RetroBIOS ROM fetch (`RomStatus_ImGui.cpp:107`), the two detached DNS lookups
(`W5100NameResolver.cpp:128`, `SocketUtil.h:178`), the CLI deferred-action /
autoboot threads (`main.cpp:851`, `:881`) and the `ChildProcess` reapers
(`ChildProcess.cpp:409`, `:755`, guard hand-written — see the comment at
`:402`). An audit on
2026-08-22 found the rule written down at exactly one of them, in `main.cpp`'s
CLI deferred-action thread, and applied at two. The most exposed was the one
with no guard at all: `workerLoop()` calls `rewind_.capture()`, which grows
multi-MB vectors against a 256 MiB budget, so `bad_alloc` there is a live
possibility rather than a theoretical one.

`ThreadGuard.h` is that rule factored out. Spawn through
`pom2::guardedThread(tag, fn)`, or wrap an existing body in
`pom2::runGuarded(tag, fn)` when the thread is constructed some other way (the
two detached lookups need this: both have a promise or a refcount to settle on
the failure path, which the guard cannot do for them).

What it does **not** do: restart the thread, or repair state the dying thread
was halfway through mutating. It converts an unobservable process death into a
logged, observable dead thread. A caller with a coherent "this subsystem is
stopped" state to publish should do so right after the guard returns — the CPU
worker sets `Mode::Stopped` and `workerParked_`, so `waitUntilParked()` returns
at once instead of burning its 200-step poll on a thread that will never park.

Pinned by `thread_guard`. That test cannot assert the usual way — a regression
kills the process rather than failing an assertion — so the throwing cases run
in a **forked child** whose exit status the parent checks. Verified falsifiable:
with the barrier removed from a copy of the header, the test exits 1 and
reports `child died on signal 6`.

## Package payload — `packaging/bundle.manifest`

**One list, four consumers.** What ships inside a package used to be spelled
out by hand in four places — `CMakeLists.txt`'s `install()` rules,
`package_macos_release.sh`, `package_windows_release.bat` and the WASM
`--preload-file` block — and they had already drifted: the browser bundle
carried `floppyemu/` and the whole `fonts/` + `pic/` folders (4 MB of
photography nothing reads), the desktop packages carried neither.

`packaging/bundle.manifest` is now the single source. Format is
`<kind> <source> [<destination>]`:

| kind | meaning |
|---|---|
| `dir`  | copy the whole tree (`roms`, `fonts`) |
| `file` | copy one file, optionally renaming (`packaging/roms_README.txt roms/README.txt`) |
| `wasm` | browser build ONLY — never in a desktop package |
| `deny` | must NEVER appear in any package; the guard asserts it |
| `denyglob` | same, by file-name pattern, *inside* a copied `dir` |

`denyglob` exists because a `dir` entry copies the **working tree**, not what
git tracks: two ROM `.zip` archives sitting in `roms/` shipped in a
locally-built WASM bundle, where nothing would have noticed them (CI builds
from a clean checkout, so its bundle differed from the committed one). Those
two archives **are** git-tracked — the comment that called them untracked was
wrong, and was corrected on 2026-09-07; untracking them is the maintainer's
call, and the manifest keeps them out of packages either way.

**`deny` is enforced by all three parsers, not only the verifier**
*(2026-09-07)*. It used to be checked by `--verify` alone — at `-maxdepth 4`,
case-sensitively, files only — so a denied folder nested deeper inside an
allowed `dir` walked straight through `install(DIRECTORY roms/)` and
`stage()`, and the WASM preload appended `disks_3.5` the manifest denies. Both
lists are now applied by the `install()` rules, by the emcc preload and by
`stage()`, matched **case-insensitively against directories as well as files,
at any depth** (`foo.ZIP/` used to be invisible twice over: wrong case, and a
directory). CMake gets there by translating each glob into a case-insensitive
REGEX — `install(DIRECTORY)`'s `PATTERN` is a case-sensitive *name* glob and
does not apply to directories, so `pom2_glob_to_ci_regex` builds a `[aA]`
class per letter (`CMakeLists.txt:39-60`). `--verify` also now **fails a
payload directory that holds only its own README**: that is a package with
zero ROMs, and the old non-empty check passed it. Six new negative controls in
`--self-test` cover the cases.

Consumers:

- **CMake** parses it near the top of `CMakeLists.txt` into
  `POM2_BUNDLE_DIRS` / `POM2_BUNDLE_FILES` / `POM2_BUNDLE_WASM`, then derives
  *both* the `install()` rules (so the AppImage, staged with
  `cmake --install`, and the `.deb` inherit it) and the WASM
  `--preload-file` list. Two parsing gotchas are load-bearing and commented at
  the call site: `file(STRINGS)` needs **`ENCODING UTF-8`** (it otherwise
  treats a multi-byte character as binary and *splits the line*, handing back
  the tail of a prose comment as an entry), and the comment is stripped with
  `FIND`/`SUBSTRING` rather than a regex (CMake's `.` does not match the bytes
  of a multi-byte character, so `#.*$` stops at the first em-dash).
- **`packaging/stage_data.sh <dest>`** stages the same list for the packagers
  that do not go through `cmake --install` — the macOS `.app`, the Windows
  `.zip` (which calls it through Git Bash).
- **`packaging/stage_data.sh --verify <dest>`** is the guard the six release
  packaging jobs run against their staged trees. Both failure modes it catches are silent: a missing font drops
  the UI to ImGui's bitmap face with blank icon boxes, and a leaked
  `disks_5.4/` turns a 6 MB download into a 200 MB one carrying media that is
  not ours to redistribute.

Pinned by **`bundle_manifest`** (`--self-test`): stage into a temp dir, verify,
then plant a deny-listed folder and require the verifier to *reject* it — a
guard that always passes is worse than none, because it reads as a guarantee.

**Why `floppyemu` is `wasm`-only**: it is 33 MB and `wasm/shell.html` boots
`floppyemu/Total Replay v6.1.hdv` by default, so it *is* the live demo's boot
disk. Desktop users mount their own media, so charging every download 33 MB
for one HDV would be a poor trade.

**Boot smoke.** `pom2_headless --frames 300 --screenshot out.ppm` runs N frames
inline through `tickFrame()` (no worker thread, no sleeps, deterministic),
renders one `Apple2Display` frame and fails with exit 3 if every pixel matches
the top-left one. It resolves ROMs through `pom2::findResource`, so running it
from a package's `usr/` exercises the package's *own* asset resolution —
which is why every release job runs it against the packaged binary, and why
`POM2 --help` (which passes just as happily with an empty `roms/`) is not
enough. Pinned locally by `headless_boot_capture`. With no disk it also skips
plugging the Disk II: an empty drive parks the II+ autostart in the boot PROM
forever and photographs the uninitialised text page.

**The browser demo deploys from CI, not from the branch** (2026-08-23).
`ci.yml`'s `wasm` job stages the bundle it just built under `_site/wasm/`
and the `pages` job ships it with `actions/deploy-pages` on every push to
`main`, so the demo at `habib256.github.io/pom2/wasm/` is always the commit
it claims to be. Before that, Pages served a committed `wasm/` copy and
`tools/wasm_stamp.sh` fingerprinted the sources to catch it going stale — which
turned every push touching `src/` red until someone re-committed 38 MB of
binaries (`POM2.wasm`: 39 commits, packfile 383 MiB). Both the guard and the
committed copy are gone; `wasm/` keeps only `shell.html` and `serve.py`, and
the staged outputs are `.gitignore`d.

## WebAssembly (browser build)

Driver: `build_wasm.sh` → `wasm/{index.html, POM2.js, POM2.wasm,
POM2.data}` (untracked; the shell template is `wasm/shell.html`, the dev
server `wasm/serve.py`). User-facing summary lives in `README.md`
§ "🌐 WebAssembly".

**Single-threaded by design**. No `std::thread`, no `SharedArrayBuffer`,
no COOP/COEP — runs on any static host (GitHub Pages, Cloudflare
Pages, plain S3). The CPU worker thread is replaced by
`EmulationController::tickFrame()` called from the render loop in
`main.cpp` (look for `#ifdef __EMSCRIPTEN__`). Trade-off vs the
native build: no parallel audio thread, but miniaudio's Web Audio
backend runs in a browser-managed worklet anyway, so the difference
is invisible in practice.

**CMake Emscripten branch** at `CMakeLists.txt:474-560`:

- `-sUSE_GLFW=3 -sUSE_WEBGL2=1 -sFULL_ES3=1` — Emscripten ships
  GLFW3 + WebGL2 ports built-in, so the ImGui GLFW/OpenGL3 backends
  link unchanged.
- `-sINITIAL_MEMORY=134217728` (128 MiB) `-sALLOW_MEMORY_GROWTH=1` —
  grows on demand; 128 MiB is enough for a IIe with RamWorks III
  + a few mounted HDV images.
- `-lidbfs.js` + `-sFORCE_FILESYSTEM=1` — IndexedDB-backed filesystem
  mounted at `/persistent` by the shell preRun hook
  (`wasm/shell.html`), and **that is where `state.cfg` and `imgui.ini`
  go** since 2026-09-01 (`pom2::userConfigDir()`). See
  [§ Browser persistence](#browser-persistence-idbfs) for the three
  parts, none of which is optional.
- `--preload-file roms@/roms …` — `roms`, plus the default extras
  `fonts;pic;floppyemu` (`POM2_WASM_BUNDLE`), baked into `POM2.data`
  at build time. The 3.5" library is opt-in via
  `-DPOM2_WASM_BUNDLE_DISKS=ON` (appends `disks_3.5`; `disks_5.4` +
  `hdv` are excluded — too large).
- `pom2_headless` target is skipped under EMSCRIPTEN
  (`if(NOT EMSCRIPTEN)` at `CMakeLists.txt:665`) — no TCP listener,
  no terminal.

**Compile-out gates** (host-socket bridges, guarded by
`#if POM2_HAS_SOCKETS` — defined in `Pom2Build.h`; 0 only under
Emscripten now that Windows is a full host-socket target):

| Subsystem | Stub behaviour | Apple II side |
|---|---|---|
| Super Serial Card TCP listener (`SuperSerialCard.cpp:168`, `:225`, `:255`, `:270`, `:451`) | `startListening` (`SuperSerialCard.h:94`) returns false + logs; the transport side (`SuperSerialTransport.h:43-50` — `start`/`stop`/`isListening`/`port`) and `drainTransportTx` (`SuperSerialCard.h:168`) no-op | ACIA still emulated — software inside the Apple II can still PR#2 / read $C0A9; just no host network bridge |
| AiControlServer HTTP listener (`AiControlServer.cpp:305+`) | `start()` returns false; `stop()` no-op | None — entire feature is a host-side control plane |

### Browser persistence (IDBFS)

Three parts, and the build is broken in a different way if any one of them is
missing. Landed 2026-09-01; the reasoning is in `CHANGELOG.md`.

1. **Where** — `pom2::userConfigDir()` (`ResourcePaths.cpp`) returns
   `/persistent` under `__EMSCRIPTEN__` and the platform config dir
   elsewhere. `Settings::resolveStorePath()` and main.cpp's `imgui.ini`
   both go through it; they used to be two copies of the platform dance,
   and the copies had already drifted under Emscripten.
2. **When it becomes durable** — `PersistentFs.h`. A write to IDBFS lands
   in the mount's memory image and reaches IndexedDB only on
   `FS.syncfs(false, …)`. Writers call `markPersistentStateDirty()`; the
   frame loop calls `pumpPersistentState()`, which debounces to one flush
   per 2 s and never overlaps two. `flushPersistentStateNow()` skips the
   debounce for the page's `visibilitychange`/`pagehide`.
3. **Who calls the writer** — nobody, without
   `MainWindow::persistSession()` (`MainWindow_Session.cpp`). The browser
   never destroys its MainWindow:
   `emscripten_set_main_loop_arg(..., simulate_infinite_loop=1)` unwinds
   `main()` without running the destructors of its locals, by design. The
   desktop calls `persistSession()` from `~MainWindow()`; the browser calls
   it on a 10 s heartbeat and from `pom2_persist_now()` (exported
   `EMSCRIPTEN_KEEPALIVE`, wired to the page's lifecycle events). The
   heartbeat is affordable because `Settings::save()` skips a save whose
   content matches the last one it wrote.

**The startup ordering, and why it carries a watchdog.** `FS.syncfs(true, …)`
is asynchronous, so the shell holds `run()` back with `addRunDependency` until
the populate finishes — otherwise `Settings::load()` can read an empty mount.
The failure mode of that fix is worse than the bug: a callback that never
fires means the dependency is never released and **the emulator never starts**
(splash up, no frame). A 5 s watchdog releases it regardless. Seen for real
during testing, not theorised.

**Testing it.** Two traps, both cost time:

- Headless Chrome reports the page as `hidden`, so `requestAnimationFrame`
  never fires and POM2's frame loop — CPU, render, persistence — does not run
  at all. CDP `Emulation.setFocusEmulationEnabled {enabled:true}` fixes it.
- `wasm/shell.html` is **not** a link dependency. Editing it alone rebuilds
  nothing and you keep serving the previous page. Touch a source file.


The symbols stay declared so every caller still links — only the
implementation degrades. **Rule for editors of these two files**
(`Pom2Build.h:70-78`): guard host-socket code with
`#if POM2_HAS_SOCKETS`, **not** `#ifndef __EMSCRIPTEN__` (the latter
silently assumed "not a browser therefore POSIX", which broke the
Windows build); new socket calls must have a no-socket branch
returning a safe sentinel (`false`/0/empty), not `#error`.

**Asset resolution**. `ResourcePaths` searches CWD-relative paths
(`./roms/apple2.rom`, etc.). Under Emscripten the CWD is `/` and
preloaded folders live at `/roms`, `/fonts`, `/disks`, … — same
relative shape, so probes resolve unchanged. The native
exec-relative path (added in d582b2f for Linux dist) is also
applied via the IDBFS mount path for future user uploads.

**Known gaps** (tracked in `TODO.md`):

- IDBFS settings persistence not wired → `state.cfg`/`imgui.ini`
  reset on every page reload.
- No file picker / drop-zone for user disks (`.dsk`/`.woz`/`.hdv`).
- No touch input on mobile (GLFW3-EM doesn't synthesise
  touch→mouse outside the canvas).
- Audio worklet latency not tuned.

**No CI yet** — `./build_wasm.sh --clean` can regress silently on
refactors of `main.cpp`, `MainWindow.cpp`, `EmulationController.cpp`,
`AiControlServer.cpp`, `SuperSerialCard.cpp`, or `CMakeLists.txt`.
Run it manually after touching any of those.

---

## Performance & profiling

The measurements, the optimisations already applied and the PGO/LTO build
recipe live in their own document — **[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)**.
Read it *before* touching anything on the hot path: it records what was tried,
what it was worth, and — for the items deliberately left alone — why.

The short version of the tooling:

- **`pom2_bench`** (`src/pom2_bench.cpp`, target `pom2_bench`) is the
  deterministic subject. N frames of `cyclesPerFrame`, no threads, no audio
  device, no sockets, no wall-clock pacing → two runs retire the same
  instruction count. It prints FNV-1a hashes of RAM and of the framebuffer;
  **those hashes are the contract**: an optimisation that moves one is a
  behaviour change, and POM2 is cycle-accurate. `pom2_headless` is a telnet
  console and cannot be used for this.
- **callgrind** over that binary, `--auto=yes` for line-level attribution.
  Profile *two* shapes at least — the hot spots with the drive spinning and
  without it are not the same code.
- **PGO** is the largest single lever and touches no emulation code:
  `packaging/raspberry/build_native_pi.sh --pgo`. Two failure modes there cost
  the entire gain *silently* (`.gcda` naming by absolute object path; the
  training driver and the shipped binary being different CMake targets over the
  same sources) — both are closed in the script and explained in the doc.

Current hot spots, for orientation: `DiskIICard::lssSync` +
`DiskImage::getNextTransition` dominate any disk-active workload;
`M6502::executeOpcode` and `Memory::advanceCycles` dominate the rest.
