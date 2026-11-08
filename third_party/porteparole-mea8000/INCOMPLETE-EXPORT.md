# Incomplete elements and release checklist

This package was rebuilt from a 10-file user-provided export, not the full AppleWin working tree. It is **not** a standalone AppleWin build.

The original project documentation references `test/PorteParole/BuildTests.cmd`, `test/PorteParole/validate_drivers.py`, and some binary test fixtures. These files **were not included** in the export and have not been recreated. `Build-PorteParole.ps1` was not packaged: it uses an absolute local Visual Studio installation path and assumes a complete AppleWin checkout.

The exported `AppleWin-integration.patch` was corrected by removing four malformed XML encoding-only hunks. Git recognizes its syntax; applying it to the exact historical AppleWin revision and building it still requires validation.

Before making a public repo:

1. Confirm authorship/provenance and compatible redistribution rights for all five C++ source files, including any model algorithms adapted from other projects.
2. Supply complete license text and required attributions/notices after the legal review.
3. Apply the patch in a clean AppleWin checkout and verify Windows compilation, slot-4 card selection, audio and snapshots.
4. Optionally include missing original test harness scripts if their licensing and origin are confirmed.
5. Review documents for obsolete local paths and pre-existing mojibake; do not silently alter source historical notes.

No files from unrelated projects were included.
