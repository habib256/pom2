# Project scope and standing decisions

Maintained decisions moved from TODO on 2026-10-05. Revisit only with new
evidence or an explicit change of scope. Historical implementation claims
inside the original rationale are dated; the [backlog](../../TODO.md) tracks
work and [DEV.md](../../DEV.md) describes the current implementation.

## The scope ruling

*This answers the question that nothing in this repo answered before
2026-09-05, and it is what makes every list below finite. The value is not the
classification — it is being allowed to say "no" to the tail **in writing,
once**, instead of re-deciding it every session.*

**The criterion.** A subsystem is **core** if and only if a silent regression in
it breaks one of three things: (1) the **DIX** run and the rest of
`docs/test_corpus.md`; (2) the **`Apple //e Enhanced PAL` fresh-install
profile** booting and running a disk; (3) the **user's files** — write-back, the
atomic commit, the persistence policy that decides whether a session's work
reaches the disk. Nothing else is core, however well built. Being
verbatim-from-MAME, being pinned, being in the README, and having been expensive
to write are **not** arguments for core: they are arguments that the thing
works, which is what *supported* means.

**What each bucket obliges.** **Core** — kept at oracle parity, carries goldens
(hashes, not smoke assertions); a regression blocks a release. **Supported** —
it works and is pinned; bugs are fixed on report; **no proactive fidelity work,
no gap-closing, no backlog grooming**. **Frozen** — present and shipped,
documented as frozen in README and in the card picker; no promise; its open
backlog items are closed as *won't do* unless somebody arrives with a need.

**Core is 14 rows out of ~60. That ratio is the point.**

### Core

| Subsystem | Why | What it obliges |
|---|---|---|
| **6502 / 65C02 / Rockwell / WDC** | 100 % Tom Harte on 178 NMOS opcodes + both Klaus suites; Crazy Cycles II uses `JMP (IND)` 5-vs-6 as a CPU *identifier* | A timing change needs a Harte diff, not a smoke test |
| **Memory / IIe paging / aux / LC / floating bus** | `floatingBus()` is what makes vapor lock possible; DIX needs 128 K aux | `bus_fastpath` is the differential oracle for any `memRead` change |
| **Video standard + frame timing** | DIX is PAL-only; `DEFAULT_SYNC_TIMER=7479` places its effects vertically | `pal_timing` + `video_event_publish` are goldens; every `emuCycles` device must take `setCpuClock` (see G5-3) |
| **Display — beam-raced reconstruction** | 14 211 mode events over 2 500 DIX frames; MODPAGE ~1 switch/scanline | `display_golden_hash` (164 pins) may only grow; a re-hash needs a stated reason |
| **Composite NTSC (OpenEmulator)** | The fresh-install pipeline — the first thing every user sees | `oe_demod_gpu_cpu_parity` + `text_oecpu_crisp` block a release |
| **Speaker** | The only audio a default machine has before any card | MAME-derived reconstruction with Blackman filtering (2026-10-05); `speaker_smoke`, `speaker_overflow` and `speaker_audio_quality` block a release |
| **Mockingboard A/C (6522 + AY)** | DIX's whole frame sync is the T1 IRQ; the OLDSKOOL crash and the "DIX raster offset" were both 6522 timing | `via_t1_*`, `via_t2_timing`, `mockingboard_t1_irq_phase` are goldens. **A Mockingboard test is credible only once shown to FAIL against the reverted fix** |
| **SlotBus + wire-OR IRQ** | One aggregation bug silences the whole corpus | `slot_bus_smoke`, `irq_aggregator_smoke` block a release |
| **DiskImage / WOZ / Disk II LSS** | Real P6 PROM + flux model; the boot path of every 5.25" corpus title | Keep the MAME port verbatim; `mame_lss_parity_smoke` is the oracle |
| **SmartPort card, Liron-class HLE (`smartport35`)** | DIX's actual boot path (`disks_3.5/DIX.po` at slot 5) | The `$Cn0A`/`$Cn0D` entry convention is frozen contract |
| **Media write-back + durability** | Criterion (3) — the only defect class that destroys what a user cannot regenerate; three paths drifted in one day | **Pinned by `media_contract`** (the three leaves, both SmartPort wrappers, Disk II / HDV cards; `Block512Backing` does not fold write-back, on purpose) |
| **Host-side storage policy** | Both causes of the 2026-09-04 HDV report, and the G2 eject defect, live here | **G5-4 is a core obligation.** The pure half must test without linking the emulator |
| **System profiles + reset architecture** | Everything above is selected by it; an ordering bug presents as a defect in whatever card loaded last | `system_profile_smoke` + `slot_config_smoke` block a release; G5-6 closes the gap |
| **Le Chat Mauve — Féline + Eve variants only** | The one non-DIX admission, made deliberately: the project's differentiator, two golden suites, its own corpus section | Keep both suites frozen. **Scope is the two variants** — `rvb` and the //c-adapter quirk are frozen |

### Supported — works, pinned, fixed on report, no proactive work

Z80 core + SoftCard *(a finished subsystem — its cleanup list is quality, not
correctness: do not schedule it)* · RamWorks III above 128 K · AppleWin NTSC,
artifact-colour LUT modes, mono phosphor, CRT glass pass *(no ctest at all until
G5-10; say so)* · Cassette · Mockingboard Sound II, SSI263, Cricket/Echo ·
Phasor *(cycle-stamped playback landed 2026-09-09)* · Floppy mechanical sounds · Disk II drive 2, `.d13`, `.nib`/`.nib2`,
2MG, MacBinary, skew sniffing · CFFA 2.0 *(**CHD closed as won't-do**)* ·
ProDOS HDV card *(its write-back is core; its device model is not;
two drives since 2026-09-11, pinned `hdv_two_drives`)* · ProDOS
host folder · IWM + Sony 3.5" + SmartPort hub · **the SmartPort card's units
3-8** *(2026-09-08, A2retroNET's shape: enumerated by ProDOS 8 2.4+, pinned
`smartport_eight_units` on the //e and `iic_smartport_six_units` on the //c's
rear port; units 1-2 stay core)* · Liron card *(the fidelity
alternative to the core `smartport35`; since 2026-09-11 up to 14 units, each
3.5" or hard disk, pinned `liron_chain`)* · //c-class on-board SmartPort + the
`$C500` stub + `IIcExternalSmartPort` · Super Serial Card + telnet *(the "real
SSC ROM" move was closed as won't-do, then shipped anyway on 2026-09-26:
Apple's 341-0065-A runs when `roms/ssc_341-0065-a.bin` is present, pinned
`ssc_firmware`)* · Uthernet II *(**`LISTEN` closed as
won't-do**)* · FujiNet relay *(relay side only; three of its five open items are
upstream bugs POM2 has nothing to fix; the native device's phases 2-4 are
unscheduled)* · TNFS media · Grappler+, parallel PrinterCard, ImageWriter II +
PDF, screen dump, print history *(`printer_plan_2`'s remaining phases are
unscheduled; the `ImageWriter.cpp` file-size debt is still owed as a **ratchet**
obligation)* · ThunderClock+ and No-Slot Clock · Mouse Card, both models ·
Joystick / paddles / 4play *(**4play is complete** — mark it so)* · TransWarp ·
Videx Videoterm *(2026-09-29; ROM-gated, ][ / ][+ only)* ·
Rewind + snapshot *("redo" and writable-WOZ undo closed as won't-do)* ·
Debugger and memory panels · AI control server + SDK *(a break here is urgent —
the project's own verification method depends on it)* · CLI + kiosk · Panel
registry, theme, docking, palette · WASM build · Packaging.

### Frozen — present, shipped, no promise

| Subsystem | Why frozen |
|---|---|
| 🧊 **Echo+ TMS5220** (`echoplus_tms`) | A stub kept for detection; nobody writes the TMS5220. **Closes P3-2 as *hide*** → G3 |
| 🧊 **Apple II Workstation Card** | Steps 2 and 3 are ❌ with no network on the other end; it buys an alternative transport for PostScript the SSC already carries, and doubles emulation cost while plugged. **Close items 2 and 3** |
| 🧊 **Zilog Z8530 SCC** | Its only consumer is the card above; SDLC is datasheet-derived with no MAME oracle. `scc8530_smoke` stays as a build guard |
| 🧊 **Uthernet I + libslirp backend** | No Windows transport (vcpkg's port drags glib into CI), none on WASM, and absent from the .dmg and the x86_64 AppImage. Uthernet II covers the need everywhere with no dependency. **Close both transport items** |
| 🧊 **Le Chat Mauve `rvb` variant + the //c-adapter quirk (P4, P5)** | P4 is gated on a manual that has never surfaced; P5's only source sits behind a proof-of-work wall `WebFetch` cannot pass. Deliberately unmodelled rather than invented |
| 🧊 **3D voxel view** | A framebuffer effect with a math test and no other pin, no corpus, no reports. 431 lines that work (513 in `Voxel3DRenderer.cpp` on 2026-10-06). Close the heightfield-mesh option |
| 🧊 **HGR/DHGR paint + sprite editors** | **Unreachable by ctest by construction**, which is how three OOB accesses survived four weeks green — *and* duplicated verbatim into POM1, so every fix must be applied twice with nothing flagging the omission. Ship it, add nothing. The one admissible piece of work is the shared `EditorTestAccess` seam, and only on a report |
| 🧊 **Floppy Emu** | 4 of 6 modes; the two missing were already "out of scope for v1" |
| 🧊 **Printer mechanical sounds** | Synthesised because **no sample set exists** — there is no oracle and never will be |
| 🧊 **PostScript by delegation** | Delegates to a host Ghostscript; document the dependency, no in-process renderer |
| 🧊 **WASM browser extras** (file picker, touch input, worklet latency, 50 Hz RAF) | Open since the WASM build landed; the demo is blocked on G1, not on code |
| 🧊 **Apple ][ Original (1977) profile** | No profile-specific test, no corpus title, no report. Do not chase rev-0 quirks |
| 🧊 **The whole `a2bus` port survey** + Saturn 128K LC + Passport MIDI + 8-bit DAC + Apple II VGA + E-Z Color | **This is the tail the ruling exists to say no to.** A card leaves *Parked* only when a named piece of software needs it |

## Standing rulings

Decisions, not work. Do not re-litigate without new evidence.

- **R0 · Do not grow the god-objects.** A new card gets its panel in its own
  `*_ImGui.cpp` and **zero** business logic in `MainWindow.cpp`. `cmake` fails
  if any `src/MainWindow*.cpp` passes 2 000 lines; `tools/check_file_sizes.sh`
  fails if any first-party file passes its recorded ceiling. The rule went from
  5 590 to 11 511 lines while it was only written down — that is why it is wired
  to a mechanism. *(The 2026-08-28 decomposition is done: `MainWindow.cpp` is
  1 408 lines, every "left" sub-item moved, the family is under the cap.)*
- **R1 · Write-protect: one rule per card.** Two bays of the *same* SmartPort
  card answer "can I write?" differently. Either rule is defensible; one card
  doing both is not. Physically write-protect belongs to the medium; the
  counter-argument is that accepting a write with write-back off loses it
  silently. **Settled 2026-09-07**: inside a SmartPort card the rule is the
  card's — physically write-protected *or* no write-back opt-in, as the 3.5"
  unit and the Disk II already answered; `SmartPortHdvUnit` was aligned. The
  HDV-class *cards* keep their in-session-writable policy, and DEV.md states
  the divergence. Pinned by `media_contract`.
- **R2 · Echo+ TMS5220: ship or hide.** A detect-only stub in the catalog is the
  wrong third option. **Answered by the scope ruling: hide** → G3.
- **R3 · One `Config`** (env → CLI → Settings → defaults), consistent `pom2::`
  namespace (255/336 files today, up from 163/233). Hygiene for the second
  contributor. Post-1.0.
- **R4 · The file-size debt is owed, not forgiven.** Two ceilings were raised on
  2026-08-31 to get a gate green that had been red since 08-29 — in 15 s, before
  the Linux job compiled anything, so it was also hiding that build. Both are
  recorded at their **exact** current size, so the ratchet fails on the next line
  either gains. `src/ImageWriter.cpp` 2 501 wants splitting (the head, the paper
  tray, PDF export and the PostScript/screen-dump seams are separate concerns in
  one TU); `src/Memory.cpp` 2 442 is 40 lines, one of which is the foreign-bus
  dispatch. *~1 d for ImageWriter, less for Memory.* Post-1.0.
  **2026-09-08:** six ceilings raised again, +225 lines in all, every one a
  confirmed-and-pinned bug fix from the ProDOS rounds and bug hunt #5 (the
  deltas are itemised in `tools/file_size_budget.txt`). `src/DiskImage.cpp`
  crossed 3 000 — the DOS/ProDOS sniff pair and the WOZ writer are the seams a
  split would cut along. The debt is larger, not forgiven.
- **R5 · A card CPU gets a `Memory::ForeignBus`, never a branch in `M6502`.**
  → CLAUDE.md, `docs/PERFORMANCE.md` §§ 8.2/8.5/9.
- **R6 · MAME path drift refresher** — re-check upstream renames ~every 6
  months (recent: `wozfdc.cpp` `bus/a2bus → machine`).

## Deliberate skips (documented inline)

Conscious MAME divergences, justified in the code at the relevant spot.
Do not re-litigate without re-reading the original comment.

- 🟢 **`$C040` STRB not gated `!//c`** (MAME `apple2e.cpp:1927`) —
  no sink wired.
- 🟢 **ClockCard DATA_OUT live** vs MAME latch on CLK edge in
  MODE_SHIFT (`ClockCard.cpp:291-300`) — strict would break stock
  ProDOS.
- 🟢 **MouseCard PIA out_a/b without `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — no firmware-visible race.
- 🟢 **ClockCard offset model vs MAME `set_time`** — behaviorally
  equivalent as long as `timeFn()` is lock-step.
- 🔁 **MAME path drift refresher** — re-check ~every 6 months to
  track upstream renames (recent: `wozfdc.cpp`
  `bus/a2bus → machine`).

## Out of scope

Things we will not do unless explicitly requested + clear ROI.

- **Apple IIgs / ProDOS 16** — lives in the separate **pom2gs** project
  (Mega II + FPI + GLU + Ensoniq DOC); never in POM2.
- **Apple ///** + SOS — niche, *20-40 d*.
- **Clones** Franklin / Laser / Pravetz / Basis 108 — *2-5 d/clone*,
  low demand.
- **CFFA CompactFlash** — HDV + host folder suffices; the MAME-faithful port
  is done (`CffaCard`).


## Deliberate limits from prior findings

- 🟢 **Declined by bug hunt #3 (2026-09-07), each a decision, not a backlog
  item.** *The 489 ns LSS cell*: `lssCyclesPerCell()` is an integer shared
  by the whole 5.25" timeline, so MAME's 8.17-cycle cell is a re-basing of
  that timeline, not a constant swap; nothing in the corpus needs it.
  *SSC BREAK and the RTS line-condition modes*: a TCP stream cannot carry
  either. *A FujiNet SP authentication handshake*: fujinet-pc's protocol has
  none, and adding one breaks unmodified FujiNet software; the listener is
  loopback-only and armed only while the card is plugged. *HDV / 800K in
  `pom2_headless`*: a build change, covered by the `hdv_boot_dump` probe.
  *6522 port-B latching (ACR.1)*: it latches on CB1, which this VIA does not
  model (`Via6522.h`, the complete not-modelled list).

- 🟢 **FujiNet media bays and the modem bridge — decided against.** The
  peer's block units as `MountableMediaCard` bays would show Mount/Eject
  rows that cannot work (the images live on the FujiNet's own storage), and
  the FujiNet panel's device table already lists them. Bridging its modem
  unit into the SSC telnet path would fight the FujiNet's own network stack.

- 🟢 **The `$C800` claim and the Workstation Card's `$Cn00` page.** Every
  ROM POM2 ships opens with `LDA $CFFF` and self-heals a wrong `$C800`
  owner; the Workstation page has no `$CFFF` access, so it is the one
  exposure if the claim rule regresses. Informational; the rule is pinned
  (`SlotPeripheral::takesC800`).

- 🟢 **Two snapshot fields are deliberately not captured, and that is the
  answer, not a gap** *(recorded 2026-09-07)*. `Ay3_8910::busOut` is consumed
  within a single `applyControl`, and the VIA's `portAIn` — which *is*
  serialised — already carries everything that survives the call. The keyboard
  latch and paste FIFO are host input in flight and live in the wrong lock
  domain (`Memory::kbMutex`, not `stateMutex`); capturing them would restore
  keystrokes the user has already seen consumed. Both are documented in place
  so the next parity audit does not re-open them.

- 🟢 **Network items reviewed 2026-09-07 and declined, with reasons.** `Sn_MR`
  MULTI/ND is a W5100 feature POM2 does not offer, not a wrong answer.
  CS8900A `Skip_1` on TRANSMIT and the PacketPage frame-buffer window are MAME
  parity with **no oracle to arbitrate** them — changing either would be
  guessing against the only reference implementation we have. `RxOKA` and
  `Rdy4TxNOW`-on-the-odd-read are datasheet-correct as they stand. The SSC's
  synthetic ROM (used only when Apple's EPROM dump is absent, since 2026-09-26)
  still does not program the ACIA control register from the baud
  DIPs, and should not: it would invent a DIP→divisor mapping and rate-limit
  every `PR#n`. These stay open only as *stated limits*.

- 🟢 **SSI263 / Echo+ placement is a guess beyond MAME.** MAME centres
  the Mockingboard's speech chip and gives the Echo+ TMS5220 a
  `front_center` speaker, which is what POM2 does; where a *pair* of
  speech chips would sit (a two-SSI263 Sound II, or Phasor + Echo+ mode)
  has no oracle. Left centred until one turns up.

