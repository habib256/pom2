# Verification report — 2026-11-07

This is an evidence-based check of the **private** repository. It does **not** certify all integration behavior or third-party licensing.

## Verified

- Repository: `mocheez13/PorteParole-MEA8000`, private; source, test, documentation and AppleWin patch files are present.
- Core `src/Mea8000.h`: compiled and ran `tests/TestMea8000.cpp` on Linux using GCC with `-std=c++14 -O2`; result: **PASS: 187 MEA8000 checks**.
- Same tests compiled with Clang, `-std=c++14 -Wall -Wextra -pedantic -O1 -fsanitize=address,undefined`; result: **PASS: 187 MEA8000 checks** with no sanitizer diagnostics in this run.
- `applewin/AppleWin-integration.patch`: Git parses it and reports changes across **9 files**. Compared its **18 original-context hunks** line by line against the actual contents of those nine files at upstream AppleWin commit `4a43bcdb8e1cb908662f4c270ca84f309a97802f`; **all 18 matched**. This provides strong evidence it applies to that commit but is **not** a successful Windows compilation or an on-machine integration test.
- License: upstream AppleWin provides GNU GPL version 2 license text, and all five exported C++ source files contain `SPDX-License-Identifier: GPL-2.0-or-later`.

## Still unverified

1. Clean Windows checkout: actual `git apply --check`, patch application, Win32 build, and runtime audio behavior with slot 4. Do this on a clean checkout using the exact upstream commit.
2. Provenance: the AppleWin card and MEA8000 emulation were developed for this project. The project's original technical notes cite earlier software materials and conceptual comparison with MAME; these references alone do not establish code reuse. **Exact third-party code provenance still requires source-level verification before public redistribution.**
3. Rights and notices: the full upstream GPLv2 text has now been added as `LICENSE`; original source SPDX headers remain in place. Independent provenance/ownership and third-party copyright review remain necessarily limited; the available source comparison is documented in `LICENSING-REVIEW.md`.
4. The original export omitted several auxiliary test scripts and data described in its documentation; full integration tests have not been reproduced here.

## Suggested reproduction commands

On Windows (PowerShell), in a **separate clean checkout** of AppleWin:

```powershell
git checkout 4a43bcdb8e1cb908662f4c270ca84f309a97802f
git apply --check "C:\path\to\AppleWin-integration.patch"
git apply "C:\path\to\AppleWin-integration.patch"
# Copy src/Mea8000.h, src/PorteParole.cpp, src/PorteParole.h into source/
# Build AppleWin Release Win32 using the relevant installed Visual Studio toolchain
```

Use only a separate clean checkout; do not overwrite any existing working projects.

## Third-party reference sources

- [AppleWin main repository](https://github.com/AppleWin/AppleWin)
- [MAME MEA8000 source](https://sources.debian.org/src/mame/0.289%2Bdfsg1-1/src/devices/sound/mea8000.cpp) — attributed to Antoine Miné, BSD-3-Clause; **not proof** that this repository copied or did not copy it.

**Release status: experimental prototype. Source license text and regression/patch checks are present, but ownership/provenance beyond the checked sources and a full integrated Windows build are not certified.**
