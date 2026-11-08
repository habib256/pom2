# Porting guide

The MEA8000 core (`src/Mea8000.h`) has no AppleWin or Windows includes and exposes:

- `Reset()`
- `ReadStatus()`
- `WriteCommand(uint8_t)`
- `WriteData(uint8_t)`
- `AdvanceClocks(clocks, &audio)`
- `SaveState()` / `LoadState()`

See `src/PorteParole.cpp` for the host glue. It ties device clock advancement to AppleWin's CPU cycle counter, routes mono samples to DirectSound, implements slot I/O, and handles save-state serialization. For a different emulator, replace this glue with that emulator's bus and audio interfaces rather than copying AppleWin-specific code verbatim.

At the original slot-4 test configuration, data/status is exposed at `$C0C0`, command at `$C0C1`. Mirroring by A0, read status low bits, and request turnaround are **provisional hardware assumptions** in this prototype, not certified historical facts.

The synthesized output sample rate is 48 kHz, while the model internally follows MEA8000 timing. Check the source comments and original documentation for known differences from silicon.

No source media are bundled. Validate the port against your own lawful test data and hardware references.
