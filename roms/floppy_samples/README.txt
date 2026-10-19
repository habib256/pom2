POM2 — Floppy drive mechanical sound samples
=============================================

Two sample sets from the MAME project drive POM2's FloppySoundDevice
(a port of MAME's src/devices/imagedev/floppy.cpp::floppy_sound_device):

  525_*.wav  — Disk II 5.25" mechanisms, including cards outside slot 6
  35_*.wav   — Sony 3.5" / SmartPort mechanisms. Both form factors are
               loaded by MainWindow and wired to their drive sound sinks.

The optional Virtual ][ recordings are a separate local-only bank. They are
not part of this sample directory's package contract and are excluded from
release payloads. Select available banks/levels in the audio controls.

Source
------
https://github.com/mamedev/mame/tree/master/samples/floppy
(fetched 2026-05-14 from master)

Files (per form factor — prefix `525_` for 5.25", `35_` for 3.5")
-----------------------------------------------------------------
{prefix}_seek_2ms.wav           short seek (track-to-track)
{prefix}_seek_6ms.wav           medium seek
{prefix}_seek_12ms.wav          long seek
{prefix}_seek_20ms.wav          very long seek
{prefix}_step_1_1.wav           single head-step click (also reused for
                                disk insert/eject click)
{prefix}_spin_start_empty.wav   motor spin-up, no media
{prefix}_spin_start_loaded.wav  motor spin-up, disk inserted
{prefix}_spin_empty.wav         motor steady-state, no media (looped)
{prefix}_spin_loaded.wav        motor steady-state, disk inserted (looped)
{prefix}_spin_end.wav           motor spin-down

License
-------
The MAME project as a whole is distributed under the BSD-3-Clause license
(with the project-wide aggregation falling under GPL-2.0+). These audio
assets are committed in-tree without a separate license notice, so they
inherit the per-file BSD-3-Clause terms:

  Copyright the MAME team. Original authors of the floppy device:
    Nathan Woods, Olivier Galibert, Miodrag Milanovic.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.
    3. Neither the names of the copyright holders nor the names of its
       contributors may be used to endorse or promote products derived
       from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO
  EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
