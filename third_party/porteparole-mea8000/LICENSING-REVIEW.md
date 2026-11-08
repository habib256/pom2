# Licensing and provenance

## License supplied

The complete **GNU General Public License, version 2** text is now included in [LICENSE](LICENSE), sourced from the upstream [AppleWin LICENSE](https://github.com/AppleWin/AppleWin/blob/4a43bcdb8e1cb908662f4c270ca84f309a97802f/LICENSE).

The five standalone C++ source/test files in this repository carry the existing SPDX header `GPL-2.0-or-later`; those headers have been retained. The full GPLv2 text supplies the version 2 terms, while the individual source notices retain the stated "or later" option. The AppleWin patch modifies GPL-covered upstream source and inherits applicable upstream license/notice obligations.

## Provenance observations

- The project owner identifies the AppleWin integration and MEA8000 implementation as developed with Codex for this emulator project.
- The original technical notes identify Philips/Ediciel technical documentation and a conceptual comparison with MAME. This documentation does not by itself establish reuse of third-party source code.
- A direct comparison against MAME's current `src/devices/sound/mea8000.cpp` and `mea8000.h` (Antoine Miné, BSD-3-Clause) found **zero identical significant code lines (minimum 55 characters)** and **zero shared 12-token sequences** in the implemented checks. This is a limited comparison, not a proof against all historical versions or other source projects.
- **No original commercial disk images, voice banks or proprietary applications are included** in this repository.

## Open limitations

Automated comparisons cannot prove that code was independently written or grant permission to relicense contributions from other authors. The copyright owner(s) of each contribution must have authorized its stated terms. No complete audit of every historical MEA8000 implementation was performed.

The GitHub Actions workflow has passed the portable unit tests and `git apply --check` for the exact AppleWin base commit, but **the integrated AppleWin Windows build and physical-hardware fidelity have not been independently validated**. The README describes this as an experimental prototype.

References: [AppleWin source](https://github.com/AppleWin/AppleWin) · [MAME MEA8000 source](https://github.com/mamedev/mame/blob/master/src/devices/sound/mea8000.cpp).
