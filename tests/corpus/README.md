# Compatibility corpus

`pom2_corpus` boots every title of `corpus.manifest` headless, plays its
script, hashes the frame at each `check` and compares it with
`corpus.expected`. It prints a pass rate that can be compared between
versions ([VALIDATION-006](../../docs/backlog/validation.md#validation-006)).

```bash
cmake --build build --target pom2_corpus
build/pom2_corpus                          # every title, from the source tree
build/pom2_corpus --tag ci                 # what ctest runs (corpus_ci)
build/pom2_corpus --title dix-auto --frames /tmp/f   # one title, frames as PNG
build/pom2_corpus --report docs/audits/corpus-<date>.md
build/pom2_corpus --bless                  # record this run's frames
```

Exit 0: nothing failed. Exit 1: a frame changed, a check is unrecorded, or a
setup failed. Exit 2: usage or manifest error. Exit 77: no title could run.

## What a check compares

A check records `Core::renderFrame()` from the SDK facade
(`include/pom2/core.hpp`): POM2's composite colour table, drawn on the CPU from
the last video frame the beam finished, beam-raced splits included. It is
**technical evidence that a frame did or did not change**. It is not the
picture the application shows, which goes through the OpenEmulator shader and
the CRT pass ([SDK § Rendering](../../docs/SDK.md#rendering)), and it does not
say whether a frame is *right*. A human decided that when the check was
blessed, by looking at the `--frames` output.

A frame depends on the firmware as much as on POM2. Every expectation carries
the **firmware fingerprint** of its run: SHA-256 over the name and contents of
each dump loaded (system ROM, character ROM, and the Disk II PROMs or mouse
ROMs when the title uses them). A run with other dumps **skips** the title
and names both fingerprints. It does not fail it.

The runner uses the facade's machine: single-threaded, no wall clock, cards
attached through the SDK calls, and a SmartPort with no mechanical timing.
Two runs of one build give identical hashes. A change to the CPU, the video
scanner, a card's timing or the renderer can change them. That is the point:
a check that changes is a question to answer, either a regression or an
improvement to re-bless.

## Workflow

1. A check fails: run the title with `--frames DIR` and look at the PNGs.
2. A regression: fix it. An improvement, or a change to the picture you have
   checked by eye: run `--bless` (all titles, or `--title ID`) and commit
   `corpus.expected` with the change that moved it, saying why.
3. A release candidate: run it in full with `--report
   docs/audits/corpus-<date>.md` and commit the report. That is the
   per-version record.

Media are **copied** to a scratch directory before they are mounted, so a
guest that writes never touches the tree. A title whose media is absent is
skipped and names the file. `tools/fetch_test_media.py` fetches the archived
ones. A `sha256=` on a media line skips the title when the file differs.

## Manifest grammar

One statement per line. `#` starts a comment, and indentation is free.

```
title <id> <profile> [tag ...]       # starts a title; profile = ii, ii+, iie-u,
                                     #   iie, iic, iic+, iie-u-pal, iie-pal, iic-pal
diskii    <1|2> <path> [sha256=<hex>]  # Disk II, slot 6 (Apple's PROMs when in
                                       #   roms/, else POM2's own)
smartport <1|2> <path> [sha256=<hex>]  # 3.5" SmartPort, slot 5 (the //c+'s own drives)
hdd       <1|2> <path> [sha256=<hex>]  # ProDOS hard disk, slot 7
mockingboard <slot> [ac|soundii]
mouse        <slot> [apple|applewin]
rgb          <slot> [feline|iic]       # Le Chat Mauve
boot <slot>|cold                       # default: cold boot, the ROM's own scan

run   <seconds>                        # emulated seconds, at the profile's clock
key   <RETURN|LEFT|RIGHT|UP|DOWN|ESC|SPACE|TAB|DELETE|c|$hh>
type  "<text>"                         # pasted; \r \n \t \\ \" escapes
check <name>                           # hash the frame now
```

The parser refuses an unknown keyword, a duplicate title or check, a slot
outside 1-7, a drive other than 1-2, an unknown key or card model, and a
title with no check. Each refusal is pinned in `corpus_manifest`.

`corpus.expected` holds one line per check, `<title> <check> <hash>
fw=<fingerprint>`. `--bless` writes it sorted and replaces only the titles
that ran.
