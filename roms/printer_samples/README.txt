POM2 — ImageWriter II mechanical sound recordings
==================================================

Recordings of a real Apple ImageWriter II, played by POM2's
PrinterSoundDevice when "Recorded ImageWriter II sounds" is ticked in the
Mixer (the default when this folder is present). Without them the printer is
synthesised, as it always was.

Credit and licence
------------------
ImageWriter-Sounds (c) by Scott Lawrence.

These files are licensed under a Creative Commons Attribution 4.0
International License: https://creativecommons.org/licenses/by/4.0/

Source: https://github.com/BleuLlama/ImageWriterIISimulator  (Sounds/)
Fetched at commit e7c6df5346084e252371240220d8d8616652b4e7 (2021-07-22).

The 21 files are UNMODIFIED copies of the upstream MP3s. POM2 trims the
encoder padding of the three carriage loops and cross-fades their seam when
it loads them, in memory only.

They are NOT covered by POM2's GPL-3.0-or-later: they keep their own
licence, which requires this attribution wherever they are redistributed.

Files, and what POM2 plays them for
-----------------------------------
print_draft_loop.mp3     the head, draft quality          (looped while printing)
print_medium_loop.mp3    the head, standard quality       (looped while printing)
print_nlq_loop.mp3       the head, near letter quality    (looped while printing)
line_feed_01..03.mp3     one line feed (the three in turn)
page_feed_short.mp3      a feed of 1 to 3 inches
page_feed_medium.mp3     a feed of 3 to 7 inches
page_feed_long.mp3       a longer feed (a form feed on a nearly empty sheet)
paper_tear_01..05.mp3    the sheet torn off ("Clear all" in the printer panel)
power_switch.mp3         the printer switched off
powerup_01..06*.mp3      the printer switched on (the six takes in turn)

Which take goes with which event is POM2's choice, not the recordist's.
