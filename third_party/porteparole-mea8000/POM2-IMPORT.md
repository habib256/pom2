# Imported into POM2: PorteParole-MEA8000 by Sébastien Pourcel

- **Author:** Sébastien Pourcel — GitHub account `mocheez13`,
  <https://github.com/mocheez13> (the name was given by POM2's author on
  2026-11-08; the files themselves carry only their SPDX header)
- **Source:** <https://github.com/mocheez13/PorteParole-MEA8000>, commit
  `0984c9efa51aba53c56c56d0fbd21285d1e27ad5` (2026-11-08)
- **License:** `GPL-2.0-or-later` (SPDX header of every source file; GPL v2
  text in `LICENSE`). POM2 is GPL-3.0-or-later, which the "or later" clause
  permits.
- **Imported:** 2026-11-08, at the user's request, as the reference work for
  [CARDS-015](../../docs/backlog/cards.md#cards-015).

The imported upstream files are **unchanged** from that commit; this import
note is POM2's own addition. Two files of the upstream
repository were left out: `docs/images/porte-parole-card.png` (an AI-generated
recreation, credited as such upstream), and the `.github` workflow. Their
README still links to the image.

The portable `src/Mea8000.h` is compiled directly through POM2's adapter in
`src/Mea8000.cpp`; the imported source remains unchanged.
`tests/TestMea8000.cpp` runs as `mea8000_upstream`. POM2's
`porte_parole_reference` also compares exact PCM through the adapter and card.
`src/PorteParole.cpp/.h` and `tests/MeaBridge.cpp` are not compiled: they need
AppleWin / MSVC headers. See DEV § Porte Parole for integration details,
snapshot compatibility and the POM2-specific bus/reset wrapper.
