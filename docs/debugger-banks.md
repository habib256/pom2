# Bank-aware debugger

Open **View → Debugger**. The panel combines run controls, disassembly,
breakpoints/watches, an instruction assembler, symbols, source correspondence
and profiling. Addresses without a bank qualifier, such as `$0800`
or a legacy label, match every bank. To stop only in one physical bank, enter
`aux1:$0800` in the breakpoint or watch field. Hexadecimal addresses remain
16-bit; bank indices are decimal. Register conditions work with either form.
Clicking the disassembly gutter creates a breakpoint for the bank currently
shown. Run To Cursor retains the bank selected with the instruction.

| Qualifier | Physical memory |
|---|---|
| `*` | Any bank; default for unqualified addresses |
| `main` | Main RAM below `$C000` |
| `aux0` … `aux127` | Auxiliary/RamWorks RAM, including ALTZP zero page/stack |
| `lc1`, `lc2` | Main Language Card banks at `$D000–$DFFF` |
| `lc0` | Shared main Language Card RAM at `$E000–$FFFF` |
| `aux0lc1`, `aux0lc2`, `aux0lc0` | Auxiliary Language Card; replace `0` after `aux` with the RamWorks index |
| `rom0`, `rom1` | Motherboard firmware; bank 1 exists on //c models with alternate firmware |
| `slot1` … `slot7` | Card slot/expansion window, or its active TransWarp firmware shadow |
| `io` | Soft switches, MIG windows and unclaimed expansion bus |
| `flat` | Flat test/foreign CPU bus |
| `ignored` | `$D000–$FFFF` write with Language Card writes disabled |

Only installed banks are reachable. Selecting `lc1` or `lc2` does not split
`$E000–$FFFF`: those bytes belong to the shared `lc0` bank. `80STORE`, PAGE2,
HIRES, RAMRD/RAMWRT and ALTZP all participate in address resolution.

A read watch matches the read bank; a write watch matches the write bank.
For example, with RAMRD off and RAMWRT on, `main:$0800` observes reads and
`aux0:$0800` observes writes. The stop banner reports the code bank and the
access bank separately. As before, a watch stops after the instruction's
access has taken place, and opcode fetches count as reads. Explicit stepping
also records the instruction's actual PC and code bank.

The profiler keeps distinct visits/cycles for each bank and PC. History
records the code bank before execution and registers after execution. An
instruction that changes ROMBANK or RamWorks selection stays attributed to
its original code bank. Reset, session load and rewind clear measurements
and pending stops while preserving user breakpoints and watches. With all
instrumentation disabled, the ordinary CPU and production memory fast paths
remain unchanged.

## Breakpoints and watches

Enter a hexadecimal address or loaded symbol in **Breakpoints**, then choose
**Add**. To add a register predicate, enable **Conditional**, select
`A`, `X`, `Y`, `P`, `SP` or `PC`, choose `==`, `!=`, `<`, `<=`, `>` or `>=`,
and enter a hexadecimal comparison value. For example, add `main:$0800`
with `X == 10` to stop before that instruction only when X is `$10`.
Comparisons use unsigned register values; the panel accepts one predicate
per address/bank, without expressions or a configurable bit mask. Adding
the same breakpoint again replaces its predicate; adding it with Conditional
off makes it unconditional.

In **Watch**, select **R**, **W** or **RW**, enter the address or symbol,
then choose **Watch**. A watch covers one address/bank rather than an address
range. If an instruction makes several watched accesses, the first one
determines the stop banner, including the accessed value. The live PC after
the stop points to the next instruction, while the banner identifies the
instruction that performed the access. Read watches also see opcode fetches
and soft-switch reads; arming one sends CPU reads through the slower watched
path. Use **x**, **Clear all** or **Clear watches** to remove stops.

## Run control

**Step** (F7) executes one instruction. **Step Over** (F8) runs a JSR until
its outer RTS returns; other opcodes take a plain step. It decodes the current
CPU mapping without reading soft switches. Inline ProDOS/SmartPort arguments,
recursion and stack wrap are handled by following the return frame rather
than assuming the next instruction is at PC+3. **Step Out** follows the current
subroutine's return. These shortcuts are handled while the debugger panel is
open. **Run To Cursor** uses the selected disassembly instruction and bank.

A breakpoint stops before its instruction executes. Resuming with **Run**, the
toolbar or another run control lets that instruction execute once before the
same breakpoint can fire again. A watch stops after the access and does not
suppress a breakpoint on the following instruction. **Stop**, or another
debugger stop, cancels pending Step Over, Step Out and Run To Cursor operations;
user breakpoints and watches remain installed.

When a SoftCard Z80 owns the bus, a plain Step advances one Z80 instruction;
the displayed registers/disassembly and breakpoint/profiling hooks still
refer to the 6502. Once the card releases the bus, remaining slice time returns
to the common 6502 debugger path: stops, resuming a previous hit and detaching
an idle hook work there as on an ordinary slice. Step Over and Step Out track
6502 call frames, so they do not provide Z80 call stepping.

## Memory viewer

The memory viewer shows the current 64 KiB CPU read mapping. **Goto** takes
a hexadecimal address; bank qualifiers in breakpoint/watch fields filter
stops and do not select a different mapping for the viewer. **Search** accepts
hex bytes such as `A9 FF 48`, or an ASCII string. ASCII matching also accepts
characters with their high bit set. Searches continue after the previous
match, or from the displayed start address, and stop at `$FFFF` without wrapping.

Double-click a hex byte, enter its replacement and press Enter to edit;
Escape cancels. Edits go through the CPU write mapping and normal bus writes,
so RAMRD and RAMWRT can make the displayed byte and edited bank differ.
ROM protection and I/O side effects still apply. **Undo** retains up to 256
edits and records the previous byte at the write target. **Undo** and **Redo**
write the recorded value through the mapping active when clicked; they do
not restore bank switches or reverse device side effects. Pause execution
and keep the mapping stable when editing banked RAM.

## Bank-qualified symbols

Expand **Symbols**, enter a file path (or choose **Browse** where available)
and click **Load symbols**. Accepted rows include AppleWin address/name
pairs, VICE/cc65 labels and assembler assignments:

```text
0800 START
al 000810 .loop
worker EQU $0820
```

Addresses are hexadecimal and must fit in 16 bits even when padded with
leading zeroes. Symbol names are case-sensitive; `.loop` retains its leading
dot. `;` and `#` start comments. POM2 assignments can additionally specify
the bank:

```text
main_entry = main:$0800
aux_entry = aux1:$0800
shared_entry = $0900
```

Use `aux_entry` directly in a breakpoint/watch field. Bank-specific labels
appear only in their bank; unqualified labels provide a fallback. Prefixing
a label explicitly, such as `aux2:shared_entry`, overrides its bank.

Successful imports replace the previous symbol table; an invalid or
conflicting row rejects the entire import and preserves the old table.
Files are limited to 4 MiB and 65536 unique names; names contain at most
255 ASCII letters, digits or characters from `_.$@:`. A name repeated with
the same address/bank is accepted; conflicting definitions are refused.
When several names share an address/bank, the listing uses the first one.

## Assemble instruction

Expand **Assemble instruction** and stop the CPU. Enter a hexadecimal
**Address**, or choose **Use cursor / PC**, then enter one instruction.
The byte preview uses the current NMOS/CMOS instruction table. Mnemonics
and register suffixes are case-insensitive; `;` starts a comment.

Operands accept `$` hexadecimal, `%` binary or unprefixed decimal values.
For example, `LDA $10` selects zero-page addressing, while `LDA $0010`
forces absolute addressing. Branch operands are target addresses, not
relative displacements; `BNE $0800` must be within −128…127 bytes of the
following instruction. Symbols loaded in the Symbols section, arithmetic
expressions, labels, directives and multiple-instruction source files are
not accepted here.

Choose **Assemble and advance** (or press Enter in the instruction field)
to write the bytes. The complete instruction must fit below `$C000` and
uses the current CPU write mapping. The address field advances by the
instruction length; the CPU's PC does not move or execute it. Patching
clears rewind history and debugger measurements. Keep the read/write bank
mapping in mind when checking the result in the disassembly or memory viewer.

## Profiling and history

In **Profiler and execution history**, enable **Profile CPU steps**, **Keep last 256
steps**, or both. The profile table shows the twenty address/bank entries
with the largest accumulated cycle counts, with visits and symbols. History
is shown oldest first, with the instruction's original code bank/PC, next PC,
post-step registers and elapsed cycles.

Cycle totals include IRQ/NMI entry steps, so a visit is not necessarily a
retired opcode. Turning a measurement option off stops collection and keeps
its existing results; re-enabling continues collection. **Clear measurements**
empties both results without disabling collection or removing user stops.
Reset, rewind, session load and instruction patches also clear measurements.
Profiling/history keep the debugger hook active even with no breakpoint set.

## Source correspondence

The **Source** section loads a POM2 `.p2map` text file. Each nonempty row is an
inclusive hexadecimal address range, a source path and a decimal line number
starting at 1. A single address is also accepted. Lines beginning with `#`
are comments. Use quoted paths for spaces and forward slashes in paths.

```text
# address range          source path           line
main:$0800-$0801         "src/main.s"           12
main:$0802-$0804         "src/main.s"           13
aux1:$0800-$0802         "src/banked worker.s"  42
*:$0900-$0905            "src/common.s"         8
```

Paths are relative to the map's directory, or absolute. The source panel
follows the PC, or the selected disassembly instruction when PC following is
disabled. It displays nearby source lines and can install a breakpoint at
the mapped range's start. **Reload source** refreshes an edited source file.
An exact-bank range takes precedence over a wildcard range. Ranges must not
overlap within the same bank. Maps and individual source files are limited
to 4 MiB; maps contain at most 65536 ranges, source paths at most 4096 bytes,
and line numbers range from 1 to 1000000. Bank qualifiers are case-sensitive
and numeric bank indices do not accept leading zeroes. A failed import keeps the last
valid map. File reads happen outside the emulator state lock.

This format is a correspondence table for assembler/linker tooling to emit;
cc65 `.lbl` files provide symbols only. Native cc65 `.dbg` import, variable
inspection, stack locals and expression evaluation are not implemented.
The disassembly and memory viewer resolve the current CPU RAM/ROM mapping
without bus side effects. Soft-switch and //c+ MIG windows show the flat
mirror; card windows use side-effect-free card peeks where available and
otherwise show the mirror. Private RAM/ROM banks inside individual cards
and secondary shadow writes are not distinguished; their bus window is
identified by slot. Z80 instructions and other card CPU execution remain
outside the 6502 debugger's profiling/breakpoint hooks.

Implementation: `DebugAddress.h`, `MemoryCpuView.cpp`, `Debugger.*`,
`DebuggerSymbols.h`, `DebuggerSourceMap.h` and `Debugger_ImGui.*`.
Regression cases run in the `debugger` test, with the memory-routing matrix
in `memory_cpu_view`, `bus_fastpath` and `ramworks_smoke`.
