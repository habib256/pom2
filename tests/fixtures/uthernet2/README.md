# Uthernet II integration fixtures

Upstream: [sbabcoc/Uthernet2](https://github.com/sbabcoc/Uthernet2/tree/ab54dbc33c08170337ea41cbe80113a66292a741),
revision `ab54dbc33c08170337ea41cbe80113a66292a741` (retrieved 2026-11-09).
These are unmodified test assets, not part of POM2's installed application.
Keep `NOTICE` and `LICENSE` with them. `NOTICE` records the IP65 stack and
driver licenses and the font attribution, as supplied by upstream.

| File | Upstream source | SHA-256 |
| --- | --- | --- |
| `ip65test.bin` | `src/test/resources/ip65test.bin` | `98dcec34e30be78b0a3ec928ddd4e8e7175bc9d2977cb6d92cda746b162da11f` |
| `telnet.bin` | `TELNET`, extracted from `src/apple/disks/uthernet2-prodos.po` | `efda8d9554f8f35601568f962b233d8dd1428f933a56b632a676386d0ee7e9d4` |
| `gopher.bin` | `GOPHER`, extracted from the same ProDOS image | `77b5eb890a00ef4318bbe69254365569344acc3efe893c70f1af33918e635a74` |

Extraction used POM2's `pom2_prodos IMAGE get TELNET OUTPUT` and `get GOPHER`.
Both executables also match the files extracted with `pom2_dos33` from
upstream's `uthernet2-dos33.dsk` and the `src/test/resources` copies.

`ip65test.s`, `ip65test.cfg` and `Makefile.upstream` are upstream's unmodified
`src/test/asm` sources. To rebuild, use the original directory layout and
makefile, cc65 revision `77b6484`, and IP65 revision
`25a9c5aa9480ddab0e506108ec7f1ce538165d28`. The clients' complete assembly
sources and their disk build instructions are in upstream's `src/apple` at
the pinned revision above.

The tests run the actual POM2 NMOS CPU, memory, slot bus, Uthernet II and
built-in NAT, paced at 1.0205 MHz. They load the programs at their original
addresses ($0800 for IP65, $4000 for the clients), into zeroed RAM as in the
upstream harness (the IP65 fixture has no BSS initialization), with a minimal RTS-filled
Monitor substitute implementing WAIT at $FCA8. They do not boot DOS/ProDOS,
exercise a disk loader, require an Apple ROM, or access the Internet.
The private NAT instance explicitly enables loopback for the test server;
application defaults remain unchanged. DNS resolves `localhost` through
the normal host resolver. Client display assertions inspect their 80x24
backing store at $0800; they do not validate rendered pixels or VideoTerm.

Run from the repository root:

```sh
cmake --build build --target test_uthernet2_ip65
ctest --test-dir build -R '^uthernet2_(ip65|telnet|gopher)_integration$' --output-on-failure
```

- IP65: unchanged MACRAW driver, DHCP lease, DNS lookup and TCP echo;
  upstream's mailbox at $0303/$0310/$0314/$0320 reports progress/results.
- Telnet: DHCP, local BBS, VT100 and 80x24 negotiation, ANSI cursor movement
  and position reply, keyboard transmission, and remote FIN.
- Gopher: DHCP, local menu, item selection, exact selector sent to the host,
  text document and pagination across 23 lines.

## Disk loading verification, 2026-11-09

Separate manual runs also booted the operating system with the local Apple
and Disk II ROMs, then loaded each program from upstream's unchanged image
in slot 6, drive 2. Uthernet II was in slot 1, with the built-in NAT and
loopback explicitly enabled for the private local server.

| Machine and boot disk | Utility disk | Command | Result |
| --- | --- | --- | --- |
| II+, NMOS 6502, DOS 3.3 System Master | `uthernet2-dos33.dsk` | `BRUN TELNET,D2` | DHCP and local BBS banner received |
| II+, NMOS 6502, DOS 3.3 System Master | `uthernet2-dos33.dsk` | `BRUN GOPHER,D2` | DHCP and local Gopher menu received |
| Enhanced IIe, 65C02, ProDOS 2.4.3 / BASIC.SYSTEM 1.7 | `uthernet2-prodos.po` | `BRUN TELNET,D2` | DHCP and local BBS banner received |
| Enhanced IIe, 65C02, ProDOS 2.4.3 / BASIC.SYSTEM 1.7 | `uthernet2-prodos.po` | `BRUN GOPHER,D2` | DHCP and local Gopher menu received |

The ProDOS system disk normally starts Bitsy Bye; a temporary copy with
`QUIT.SYSTEM` removed was used to start `BASIC.SYSTEM` directly. The utility
images and the repository's boot disks were not modified. These manual runs
used actual ROMs rather than the automated test's Monitor substitute.

Soft-70 screenshots rendered by POM2:
[Telnet under DOS 3.3](../../../docs/audits/assets/uthernet2-2026-11-09-dos-telnet.png),
[Gopher under ProDOS](../../../docs/audits/assets/uthernet2-2026-11-09-prodos-gopher.png).
This verifies disk loading and a local network session in POM2; physical
Uthernet II hardware and remote Internet servers were not tested.
