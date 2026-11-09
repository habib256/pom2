# Optional PLASMA / PLEIADES test

This test downloads no media automatically and adds nothing to distribution
packages or the default CI corpus. It uses the standalone 140 KiB FORTH
disk, not the 32 MiB full installation. On macOS/Linux, fetch it to the
temporary path named in the manifest:

```sh
curl -fsSL https://raw.githubusercontent.com/dschmenk/PLASMA/f209e30e87fa11e375a511a7376e0b9dfa800162/images/apple/PLFORTH.po -o /tmp/pom2-PLFORTH.po
build/pom2_corpus --manifest tests/corpus/plasma.manifest --expected tests/corpus/plasma.expected --frames /tmp/pom2-plasma-frames
```

The manifest checks the image SHA-256. Missing media or different firmware
skip the title, as described in [README.md](README.md). For other platforms,
adjust the media path in a local copy of the manifest. The runner mounts a
scratch copy; the downloaded disk is preserved.

Validated 2026-11-09 with the existing POM2 1.2.0 corpus binary
(`d8341ca7bb96d6f8b1f55cf282da5ccc51d14c7c-dirty`), enhanced IIe:

- After 120 emulated seconds, the graphical startup finishes at the FORTH prompt.
- Typing `123 456 + .` returns `579 OK`; both PNG checkpoints were inspected.

Thirty seconds was insufficient: the startup banner was still visible and
the arithmetic result had not appeared. Allow the full startup interval.
These frame checks cover boot and interactive arithmetic, not PLASMA source
compilation, filesystem writes, networking, or comprehensive VM correctness.
No successful IIc result is claimed by this test.

Upstream: [PLASMA](https://github.com/dschmenk/PLASMA).

## Guest compiler end-to-end test (recommended)

The full image is an external test fixture only: it is neither tracked nor
packaged, and this test is not added to the default CI corpus.

```sh
curl -fsSL https://raw.githubusercontent.com/dschmenk/PLASMA/f209e30e87fa11e375a511a7376e0b9dfa800162/images/apple/PLEIADES-2.21.po -o /tmp/pom2-PLEIADES.po
python3 tools/test_plasma_compile.py /tmp/pom2-PLEIADES.po --frames /tmp/plasma-compile-frames
```

The runner verifies the pristine image SHA-256, copies it to a temporary
directory, inserts only `plasma-smoke.pla` as `BLD/POMTEST.PLA`, and boots
an enhanced IIe. PLEIADES compiles that source using `+PLASM`, writes the new
module through ProDOS, then loads and executes it using `+POMTEST`.
The downloaded image is never modified. Use `--build DIR` for another build
containing `pom2_prodos` and `pom2_corpus`.

The program calls a square function, fills a five-word array, sums it in a
second loop and checks the result. Expected output: `POM2 PLASMA PASS: 55`.
The compiler completion and result frames are checked against visually
inspected baselines. These are exact frame comparisons, not OCR or a direct
semantic assertion; a firmware mismatch skips the test (exit 77).

Validation on 2026-11-09 with the same existing binary as above: 2/2 checks
passed, 72 emulated seconds. A negative control changing the expected sum
from 55 to 54 preserved the compiler checkpoint but failed the result
checkpoint (exit 1), confirming that an incorrect result is rejected.
This exercises source reads, module creation and reload as part of compilation;
it is not a general filesystem or full PLASMA language conformance test.

## Optional GitHub Actions workflow

`PLASMA compatibility` (`.github/workflows/plasma.yml`) runs manually through
Actions → Run workflow, or every Monday at 04:43 UTC on the default branch.
It does not run on pushes or pull requests. The image is pinned to an upstream
commit, cached by SHA-256 and verified even on cache hits. Firmware uses the
existing cached test-media action. Only the two required tools are built.
Skipped tests fail this job rather than reporting a successful validation.
The console log and checkpoint PNGs are retained for 14 days; no disk image
is uploaded as an artifact. The schedule becomes active when the workflow
is committed to the repository's default branch.
