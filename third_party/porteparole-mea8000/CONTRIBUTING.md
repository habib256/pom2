# Contributing

Thanks for helping preserve the Ediciel Porte-Parole.

This repository packages an experimental peripheral and speech-chip emulation, **not** a complete AppleWin distribution.

- Keep the standalone synthesizer (`src/Mea8000.h`) free of AppleWin and Windows dependencies.
- Keep platform bus, audio and snapshot integration in the host adapter.
- Add or update tests when changing synthesis timing, frame buffering or audio rendering.
- Document whether observations come from a primary manual, a period advertisement, a software test, or measurements of a physical card.
- Do not treat different boards pictured in historical photographs as interchangeable revisions without hardware evidence.
- Do **not** commit commercial disk images, ROMs, voice banks, third-party photographs without permission, or code copied from other emulators without documented rights.
- Before public release, verify source provenance, full licensing notices, and a clean Windows AppleWin build.

The GitHub Actions workflow checks the portable C++14 regression tests and whether the patch applies against its exact AppleWin base commit. **It does not compile an integrated AppleWin executable.**
