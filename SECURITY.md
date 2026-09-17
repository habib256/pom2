# Security policy

POM2 is a desktop emulator, but three parts of it open doors, and those are
what this policy is about:

- the **AI control server** (`--ai-control`, or the AI Control panel) — an
  HTTP server on the loopback interface that can read and drive the emulated
  machine, mount media and write snapshots;
- the **network cards** — the Uthernet II's sockets are host sockets, and the
  Uthernet I / FujiNet paths go through libslirp or a host TCP/serial link;
- **media and snapshot parsing** — disk images, WOZ, 2IMG, snapshots and
  settings files come from anywhere.

The rules POM2 already enforces are written down in `CLAUDE.md` (the loopback
perimeter, the AI server's token and `Host` fence) and `DEV.md`. A report
that one of them does not hold is exactly what we want to hear about.

## Reporting a vulnerability

Please **do not open a public issue**. Use GitHub's private reporting:
the repository's **Security** tab → **Report a vulnerability**. Include the
POM2 version (`POM2 --version`), the platform, and the smallest reproduction
you have — a media file or an HTTP request sequence is ideal.

You will get an answer, and credit in `CHANGELOG.md` if you want it. A fix
ships in the next release; the release notes say what was fixed once it is
out.

## Supported versions

Only the latest release, and `main`.

## Out of scope

- Anything that requires the user to opt in first, used as documented: a
  token-less AI control server reachable from a local process, or
  `uthernet_allow_loopback` letting the guest reach the host's loopback.
- The emulated machine misbehaving under a hostile *guest program* in ways
  that stay inside the emulation.
- The commercial software and ROM dumps question — see `THIRD-PARTY.md`.
