# Ediciel Porte-Parole / Philips MEA8000

![AI-enhanced recreation of the Ediciel Porte-Parole Apple II speech card](docs/images/porte-parole-card.png)

*AI-enhanced recreation based on a historical photograph attributed to apple2.org. For illustration only; not a reliable component-level reference.*

**Preserving a French-speaking Apple II expansion card from 1984.**

> Porte-Parole spoke French on an Apple II long before speech synthesis became a standard desktop feature. This repository documents and emulates its expansion card and Philips MEA8000 speech synthesizer.

## A little history

**Porte-Parole** was a French speech-synthesis expansion card and software package distributed by **Ediciel / Matra-Hachette** for the **Apple II family**. It was already commercially available in **October 1984**: the contemporary magazine *Golden* (issue 10, November 1984, p. 33) reviewed the card, software and book as a package priced at **2,000 French francs**, with the card alone offered at **1,395 francs**.

The supplied software converted typed French text into audible speech and allowed users to edit speech parameters graphically. Speech could be embedded into other Apple II programs; the package also offered ways to explore expressive intonation, musical notes and sound effects. The manufacturer's period advertisement presented it as a way to give the Apple a virtually unlimited French vocabulary.

### Audio connections on the Ediciel board

The documented Ediciel board has **one RCA audio connector** and a separate **two-pin connector for the Apple II internal speaker** (hardware connection identified by the project owner).

At the heart of the speech-generation technology is the **Philips MEA8000**, a formant-based speech synthesizer. Rather than storing whole sentences as sampled audio, it produces speech from compact control frames describing pitch, amplitude and vocal resonances. The MEA8000 uses a voiced or noise excitation source and four resonant filters.

This project recreates the **card and speech chip behavior** in software, so that historical Apple II applications can use a simulated Porte-Parole. The emulator implementation was developed for an AppleWin fork.

### Period references

- *Golden*, no. 10, November 1984, p. 30-33: contemporary review, commercial availability, prices and speech editor. [Digitized issue](https://mirrors.apple2.org.za/ftp.apple.asimov.net/documentation/magazines/golden/golden10.pdf).
- Ediciel period advertising/catalogue: original product description, French text-to-speech, programming use and graphical voice editor. [Period catalogue scan](https://www.mocagh.org/sir-tech/ediciel-catalog.pdf).
- *Haut-Parleur*, no. 1722, November 1985, p. 85: technical description of MEA8000 control parameters. [Magazine scan](https://retronik.silicium.org/DOCUMENTS/revues/haut-parleur/1985/HP1722_11-1985.pdf).
- [Philips MEA8000 technical background](https://www.wide-dot.com/mea8000.html): formant synthesis and further technical references.

## Preservation project

This repository provides a **standalone C++14 MEA8000 synthesis model**, an **AppleWin-specific Porte-Parole expansion-card implementation**, a patch for a known AppleWin revision, regression tests and porting notes for other Apple II emulator developers.

No commercial Apple II disk images, speech banks or proprietary original software are distributed. A photograph of the original hardware can be added later, once a suitable rights-cleared image is supplied.

## Contents

- `src/Mea8000.h` — standalone synthesis device model.
- `src/PorteParole.cpp` and `src/PorteParole.h` — AppleWin-specific card implementation: slot I/O, CPU cycle synchronization, DirectSound, save state.
- `tests/TestMea8000.cpp` — portable regression tests for the synthesis model.
- `tests/MeaBridge.cpp` — Windows DLL bridge used in the original testing workflow; requires MSVC and is not portable as-is.
- `applewin/AppleWin-integration.patch` — AppleWin-specific registration, CLI and build-file changes.
- `docs/` — original technical notes, retained as supplied. `PorteParole.md` contains pre-existing character-encoding corruption.
- `PORTING.md`, `LICENSING-REVIEW.md`, `INCOMPLETE-EXPORT.md` — integration and release notes.

## Quick check (Linux/macOS with g++)

```sh
g++ -std=c++14 -O2 -Isrc tests/TestMea8000.cpp -o test_mea8000
./test_mea8000
```

Observed on package preparation: `PASS: 187 MEA8000 checks`. This checks software behavior, not complete analog fidelity to a real chip or integrated AppleWin behavior.

## AppleWin integration

Target upstream revision: `4a43bcdb8e1cb908662f4c270ca84f309a97802f`. Apply the patch to a **clean separate checkout** of that revision and copy the three files in `src/` to its `source/` directory. Read `applewin/../docs/PATCH-NOTES.md` and the original technical documentation.

With a compatible build, the original test launch used `-model apple2e -s4 porteparole` (Porte-Parole in slot 4). Behavior in another slot depends on the software's slot addressing.

**Status:** experimental source release; portable unit tests and patch applicability have been checked. A complete Windows AppleWin build and hardware-level fidelity remain unverified. The GPLv2 license text is provided in `LICENSE`; see `LICENSING-REVIEW.md` for attribution/provenance limits and `INCOMPLETE-EXPORT.md` for missing integration test material.
