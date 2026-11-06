# Sirius Joyport — Atari mode (INPUT-007)

POM2 emulates the digital Atari side on II/II+/IIe. The physical selector
can force either socket or let software select it. The rear Apple sockets
are empty; analogue multiplexing is outside this implementation. Default
off, configured in the Joystick panel and saved as `joystick_joyport`.

## Evidence and limits

- **B:** [Sirius Joyport User Manual](https://mirrors.apple2.org.za/ftp.apple.asimov.net/unsorted/Sirius%20Joyport%20Manual.pdf),
  pp. 10–13: AN1 off selects horizontal, on selects vertical. PB0 is fire,
  PB1 left/up, PB2 right/down. The BASIC scanner tests closed contacts with
  bit 7 clear. The physical selector overrides AN0.
- **B, contradictory:** p. 10 and BASIC lines 4100/4300 use AN0 off for
  left, on for right. Figure 5 and the Pascal comments reverse the sockets.
  POM2 follows the prose/BASIC, but **physical Atari socket polarity remains
  unverified**. The centre setting is provisional; fixed left/right do not
  depend on this disagreement. A two-socket hardware comparison is still
  needed before removing the remaining TODO.
- **C:** Wavy Navy (Sirius, 1983), Asimov's `Wavy Navy (4am crack).dsk`:
  its own menu selects Joyport with Z. `$0AE6` writes `$C05A`; the driver
  at `$17CF/$17DC` reads PB1/PB2 with active-low tests, and `$18A8` reads
  PB0. It does not select sockets via AN0. This is the validated use case
  for the fixed selector. No game code or image is distributed here.
- **C:** Bandits (Sirius, 1982), both Asimov images below: the title polls
  Space at `$0F4F`. During play `$0228` recognises NUL (`$80` with strobe),
  then `$02EB` installs movement `$03C0`, shield `$0235`, active-low fire
  `$0277`, and selects AN0 off. Its indexed accesses use Y=`$DE` plus
  `$BFxx`, so searching only absolute `$C06x` opcodes misses the driver.
  AN1 switches within the running game; movement, fire and shield are
  validated through real CPU execution, not extracted or replaced routines.
- **C, separate Apple mode:** Computer Foosball's four-controller setup
  patches the bank selector to `$C059` at `$133B`, then scans the second
  button bank at `$19DA` and `$1DA1`. It uses analogue paddles and therefore
  does not validate Atari socket polarity or work with this digital-only
  configuration. Do not count it as a passing game test.
- **D:** Casso `2d899adf`, `SiriusJoyport.cpp`, follows prose/BASIC too. Its
  500,000-cycle reset bypass is not implemented here. The real connector
  has no reset pin; see the builder's [Joyport recreation report](https://lukazi.blogspot.com/2016/09/apple-ii-4play-joystick-card-joyport.html).
  Released active-low inputs can trigger the IIe boot self-test. Enable
  after boot on IIe; no synthetic grace period hides the device.

Bandits was the first named candidate, based on the contemporary
[David H. Ahl review](https://www.atarimagazines.com/cva/v1n1/applegames.php).
**Gameplay validated on 2026-11-06** with both the Asimov Sirius collection
image and the image inside `bandits-fixed.zip`. The earlier title-only run
had sent Space at a fixed time; waiting for the guest's actual keyboard poll
and then sending NUL in play reaches the Joyport driver without a RAM patch.
SHA-256 of the two images, respectively:

```
a8f71fe9f12f5d3de61ae6caf60212794ad694d36083b41d83fc47095cd1f9ae
387c0c716865226443365221a51781e6672f39299863af1cebcd189b99a6d7fd
```

## Integration

Memory samples its existing AN0/AN1 latches on every button read, including
aliases. Only bit 7 is driven; the low bits retain the floating bus. Input
writes are ignored. With the rear sockets empty, paddle inputs stay high.
IIc excludes the device because its IOU controls are not exposed on a
16-pin game-I/O socket. The Machine menu's disconnected-port switch also
disconnects Joyport. Simultaneous Apple keys/Shift-wire modifications are
not modeled with this device; their ordinary path resumes when it is off.

Host policy: first selected controller is left, optional second controller
is right. Configured sources/inversion/deadzone apply, then half deflection
closes a digital switch; a standard D-pad also closes switches. Opposing
directions cancel. A/B (raw button 0) fire. Keyboard mode owns left directions
and either X/Z fires. Mouse, retained velocity and absolute paddle positions
do not drive Joyport. Focus loss, UI capture, menus and SNES MAX suppress
its host contacts. Joyport owns D-pad/face controls instead of typing them.

A versioned five-byte trailer carries selector, both contact masks and the
game-port connection flag in snapshots, rewind and sessions. Existing
trailers carry AN0/AN1. Validation
precedes mutation; older states without the trailer restore Joyport off.
The next host poll refreshes contact levels. Reset leaves the attached
adapter in place and resets motherboard annunciators as before.

## Reproduction

`sirius_joyport` checks bus reads/writes, aliases, floating bits, selectors,
open paddles, reset, IIc exclusion, snapshots/rewind, old blobs and malformed
state refusal. `joystick_two_players` also checks the production host routing
for digital contacts, D-pad, velocity independence, cancellation and hotplug.

The optional game probe requires locally supplied media; ordinary CTest
reports it skipped. Use a copy of the image, kept outside the repository:

```sh
POM2_MEDIA_WRITE_DEFAULT=protected build/tests/test_sirius_joyport_game \
  roms/apple2.rom roms/disk2.rom /tmp/pom2-wavy-navy.dsk
```

Image: [Asimov Wavy Navy directory](https://mirrors.apple2.org.za/ftp.apple.asimov.net/images/games/action/wavy_navy/),
`Wavy Navy (4am crack).dsk`, SHA-256
`e57abd0b8d25843ae8393dc739e55fccf615cccca6ebdb3d98240ac2d346c080`.
The probe boots, selects Z/S through the keyboard, presses fire, then moves
the boat using each fixed socket. It checks the game's own position/target
variables: the boat moves 123 → 117 → 125, with targets 16 and 225. No guest
RAM patch selects the controls or supplies the movement.

### Bandits gameplay probe

```sh
POM2_MEDIA_WRITE_DEFAULT=protected build/tests/test_sirius_joyport_bandits \
  roms/apple2.rom roms/disk2.rom /tmp/pom2-bandits-fixed.dsk /tmp/bandits
```

The optional final argument writes PPM captures of the ready state, a shot,
the shield and the prolonged gameplay run. Default CTest skips this probe
without media arguments. Sources: [Bandits directory](https://mirrors.apple2.org.za/ftp.apple.asimov.net/images/games/action/bandits/)
(`bandits-fixed.zip`) and [Sirius collection](https://mirrors.apple2.org.za/ftp.apple.asimov.net/images/games/collections/sirius/)
(`BANDITS.dsk`). The runs used the II+ ROM SHA-256
`fc3e9d41e9428534a883df5aa10eb55b73ea53d2fcbb3ee4f39bed1b07a82905`.

After boot and keyboard selection, each control case starts from the same
in-memory checkpoint, including slot state. The game first observes released
contacts to clear its fire/shield edge latches. The probe only observes game
RAM and injects keyboard/joystick inputs; it never patches a guest routine,
position, life counter or score. For each fixed socket, and for centre with
the game's own AN0-off selection, all eight cases pass:

| Input for 60 frames | Observed gameplay |
|---|---|
| Neutral | X remains 127; no shot or shield |
| Left / right | X reaches 47 / 211 |
| Fire held | Exactly one projectile launched |
| Three separate fire presses | Exactly three projectiles launched |
| Up | Shield activates (`$F4=$40`) |
| Down | No movement, shot or shield |
| Right + fire + up on the unselected socket | No effect on the player |

Position is `$F7`; the three projectile state bytes are `$F0/$D2/$D8`,
observed via write watches including the NMOS dummy-write distinction.
The probe also sees PB reads and both AN1 axis selections in every case.
A further 1,800 frames (about 30 emulated seconds) exercise repeated fire,
shield energy consumption, death and respawn. Results: 77 launches and
5→1 lives on the fixed image; 80 launches and 5→2 lives on the collection
image. The fixed-image capture reaches the bonus display with score 850.
These validate gameplay controls and ongoing play, not completion of all
28 levels. Bandits selects only AN0 off, so physical left/right polarity in
centre mode remains unproved.

For interactive play in POM2: enable **Atari: left socket**, press Space
when the introduction accepts it, then send **Ctrl+@** (NUL; **Ctrl+2** on
a US layout). The historical II shortcut is Ctrl+Shift+P, as documented by
[Mednafen 1.32.1](https://mednafen.github.io/documentation/apple2.html#Section_atari_joystick)
(**D**); POM2 reserves that host chord for its command palette. Its existing
Ctrl+@/Ctrl+2 path supplies the same guest byte. Plain Ctrl+P selects the
analogue control path instead. A/B fires; up activates the shield.

Physical controller play and a browser build remain untested. The centre
selector's unit test establishes the implemented convention, not physical
hardware truth.
