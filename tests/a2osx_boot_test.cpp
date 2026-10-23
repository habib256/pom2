// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// a2osx_boot — A2osX 0.95 (disks_3.5) as a hardware test bench.
//
// A2osX drives hardware nothing else in the corpus touches, from an
// interrupt-driven multitasking kernel. This test boots it from a slot-5
// SmartPort card on a //e enhanced and on a //c, then has its own `insdrv`
// look for each POM2 card it has a driver for: every driver prints
// "Installed As Device" when its detection code finds the hardware it
// expects, and "...Not Found" / "No ... Found" when it does not.
//
// What it pins:
//   * the boot reaches the shell on both machines;
//   * `lsdev` sizes the SmartPort bay from STATUS — 1600 blocks for the
//     800K disk, $28 for the empty bay (the synthetic slot page used to
//     leave NOP fill at $CnFC-$CnFD, read as 60 138 blocks);
//   * ten drivers find their card: Grappler, Grappler+, Parallel Interface,
//     SSC polled and interrupt-driven, Uthernet I, Uthernet II and its
//     ARP/IP-offload variant, the Mouse Card, the Mockingboard — and the
//     DHGR driver recognises Le Chat Mauve's Eve by its CPREG auto-write;
//
//   * a ThunderClock serves as the kernel's 64 Hz interrupt source;
//   * bytes leave through the cards: `echo` to /dev/lpt1 prints on the
//     Grappler+, `echo` to /dev/com2 reaches the SSC's wire (the driver's
//     CLOSE resets the 6551 right after the last byte), and on Uthernet II
//     (MACRAW and IP-offload drivers) and Uthernet I, networkd gets a DHCP
//     lease from libslirp and `ping 10.0.2.2` is answered.
//
// Known and NOT pinned here: on a //c with the internal Mockingboard 4c the
// boot never reaches the shell. A2osX's Z80-card probe writes $C100-$C700;
// the write to $C400 wakes the 4c (POM2 follows MAME's c400_w), which then
// owns $C400-$C4FF. Recorded in TODO.md; see a2osx_boot_probe --mb4c.
//
// Skips (77) when the //e ROM or the A2osX disk is missing.

#include "A2osxHarness.h"
#include "GrapplerCard.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!ok) ++failures;
}

/// The text after the LAST echo of `command` on the screen.
std::string afterCommand(const std::string& screen, const std::string& command)
{
    const auto at = screen.rfind(command);
    return at == std::string::npos ? std::string() : screen.substr(at + command.size());
}

/// Boot a //e with `cards`, then run each `insdrv` and expect `expect` in
/// what it printed.
void driverBatch(const char* label,
                 const std::vector<std::pair<int, std::string>>& cards,
                 const std::vector<std::pair<std::string, std::string>>& runs)
{
    pom2test::A2osxMachine m(false, std::string("pom2_a2osx_") + label + ".po");
    for (const auto& [slot, key] : cards)
        check(m.plug(slot, key), std::string(label) + ": POM2 card '" + key + "' in slot "
                                 + std::to_string(slot));
    if (!m.boot()) { check(false, std::string(label) + ": boot to the shell"); return; }
    for (const auto& [command, expect] : runs) {
        const std::string out = afterCommand(m.type(command), command);
        const bool ok = out.find(expect) != std::string::npos;
        check(ok, std::string(label) + ": '" + command + "' -> " + expect);
        if (!ok) std::printf("%s\n", out.c_str());
    }
}

}  // namespace

int main()
{
    {
        pom2test::A2osxMachine probe(false, "pom2_a2osx_ready.po");
        if (!probe.ready()) {
            std::printf("a2osx_boot: SKIP (roms/apple2e.rom or the A2osX disk not found)\n");
            return 77;
        }
    }

    // ── The //e boots to the shell, and the SmartPort bays are sized right ──
    {
        pom2test::A2osxMachine m(false, "pom2_a2osx_iie.po");
        const bool booted = m.boot();
        check(booted, "//e enhanced: A2osX reaches the shell");
        if (booted) {
            std::printf("       (after %.1f emulated seconds)\n", m.bootSeconds());
            const std::string out = afterCommand(m.type("lsdev"), "lsdev");
            bool s5d1 = false, s5d2 = false;
            size_t pos = 0;
            while (pos < out.size()) {
                const size_t eol = out.find('\n', pos);
                const std::string line = out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                if (line.find(" s5d1 ") != std::string::npos)
                    s5d1 = line.find(" 1600") != std::string::npos;
                if (line.find(" s5d2 ") != std::string::npos)
                    s5d2 = line.find("[28]") != std::string::npos;
                if (eol == std::string::npos) break;
                pos = eol + 1;
            }
            check(s5d1, "lsdev: s5d1 is the 800K disk, 1600 blocks");
            check(s5d2, "lsdev: s5d2, an empty bay, answers $28");
            if (!s5d1 || !s5d2) std::printf("%s\n", out.c_str());
        }
    }

    // ── Each driver finds its card ─────────────────────────────────────────
    driverBatch("set1",
        { {1, "grappler"}, {2, "ssc"}, {3, "uthernet2"}, {4, "mouse"}, {7, "mockingboard"} },
        { {"insdrv grapplerp.drv", "Installed As Device"},
          {"insdrv ssc.drv 9600,N,8,1,X", "SSC Installed As Device"},
          {"insdrv uthernet2.drv 00:08:dc:12:34:56", "W5100 Installed As Device"},
          {"insdrv mouse.drv", "Mouse Installed As Device"},
          {"insdrv mkboard.drv", "MockingBoard Installed As Device"} });
    driverBatch("set2",
        { {1, "grappler1"}, {2, "ssc"}, {3, "uthernet2"}, {7, "pic"} },
        { {"insdrv grappler.drv", "Installed As Device"},
          {"insdrv ssc.i.drv 9600,N,8,1,X", "SSC Installed As Device"},
          {"insdrv uther2.ai.drv 00:08:dc:12:34:56", "W5100 Installed As Device"},
          {"insdrv pic.drv", "PIC Installed As Device"} });
    driverBatch("set3",
        { {7, "uthernet"} },
        { {"insdrv uthernet.drv 00:0e:3a:12:34:56", "CS8900A Installed As Device"} });
    // dhgr.drv's Eve probe: TXT16 on with CPREG = 1 (the data byte of that
    // very write), a write to MAIN $400, and the byte must read back from
    // AUX $400 — the Eve's auto-write. Without it: "Video7 80c" mode.
    driverBatch("set4",
        { {7, "chatmauve-eve"} },
        { {"insdrv dhgr.drv", "'LCM Eve' Board Detected"} });

    // ── A ThunderClock as the kernel's tick ────────────────────────────────
    // A2osX enables the card's interrupts with `$60` and no STB, relying on
    // the battery-backed uPD1990AC already pulsing TP at 64 Hz (its power-up
    // REGISTER_HOLD rate). With TP stopped the kernel's tick never moved and
    // nothing that sleeps woke: no getty, no shell.
    {
        pom2test::A2osxMachine m(false, "pom2_a2osx_clock.po");
        m.plug(4, "clock");
        const bool booted = m.boot();
        check(booted, "ThunderClock as the IRQ source: A2osX reaches the shell");
        if (booted) {
            auto* tc = m.card<ClockCard>(4);
            check(tc && tc->interruptsEnabled() && tc->tpRateHz() == 64,
                  "ThunderClock: interrupts on, TP at 64 Hz");
        }
    }

    // ── Bytes leave through the cards ──────────────────────────────────────
    {
        pom2test::A2osxMachine m(false, "pom2_a2osx_io.po");
        m.plug(1, "grappler");
        m.plug(2, "ssc");
        if (!m.boot()) {
            check(false, "io: boot to the shell");
        } else {
            m.card<SuperSerialCard>(2)->onTransportConnected();   // a peer on the line
            m.type("insdrv grapplerp.drv");
            m.type("insdrv ssc.drv 9600,N,8,1,X");
            m.type("echo HELLO PRINTER > /dev/lpt1");
            m.type("echo HELLO SERIAL > /dev/com2");
            const auto printed = m.card<GrapplerCard>(1)->spoolBytes();
            const std::string paper(printed.begin(), printed.end());
            check(paper == "HELLO PRINTER\r\n", "Grappler+: 'echo > /dev/lpt1' prints the line");
            if (paper != "HELLO PRINTER\r\n") std::printf("       printed: '%s'\n", paper.c_str());
            // The card paces its TX ring at the line rate in WALL time.
            std::vector<uint8_t> wire;
            for (int i = 0; i < 50 && wire.size() < 14; ++i) {
                m.card<SuperSerialCard>(2)->drainTransportTx(wire);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const std::string line(wire.begin(), wire.end());
            // The driver CLOSEs with a programmed reset straight after the
            // last byte; the bytes the 6551 had accepted still go out.
            check(line.find("HELLO SERIAL") != std::string::npos,
                  "SSC: 'echo > /dev/com2' reaches the wire past the CLOSE");
            if (line.find("HELLO SERIAL") == std::string::npos) std::printf("       wire: '%s'\n", line.c_str());
        }
    }

    // ── DHCP and ping through libslirp, on each Ethernet driver ────────────
    // networkd is started from etc/init, the way A2osX means it: started
    // by hand as `networkd libtcpip &`, A2osX 0.95 itself overwrites the
    // first byte of libtcpip (the subshell's argument list is copied with its
    // double NUL into a block sized for one; the 17th byte lands on the
    // library's CLD, which becomes a BRK). That is not POM2's to fix.
    if (!pom2::makeSlirpBackend("probe")) {
        std::printf("[SKIP] network: this build has no libslirp\n");
    } else {
        struct Nic { int slot; const char* card; const char* drv; };
        for (const Nic n : { Nic{3, "uthernet2", "uthernet2.drv 00:08:dc:12:34:56"},
                             Nic{3, "uthernet2", "uther2.ai.drv 00:08:dc:12:34:56"},
                             Nic{3, "uthernet",  "uthernet.drv 00:0e:3a:12:34:56"} }) {
            pom2test::A2osxMachine m(false, "pom2_a2osx_net.po");
            m.plug(n.slot, n.card);
            const std::string tag = std::string(n.drv).substr(0, std::string(n.drv).find(' '));
            if (!m.boot()) { check(false, tag + ": boot to the shell"); continue; }
            m.type(std::string("echo \"${ROOT}sbin/insdrv ") + n.drv + "\" >> ${ROOT}etc/init");
            m.type("echo \"${ROOT}sbin/networkd libtcpip\" >> ${ROOT}etc/init");
            if (!m.boot()) { check(false, tag + ": reboot with networking"); continue; }
            const std::string out = m.type("ping 10.0.2.2");
            const bool replies = out.find("bytes from 10.0.2.2") != std::string::npos;
            check(replies, tag + ": DHCP lease, then 'ping 10.0.2.2' is answered");
            if (!replies) std::printf("%s\n", out.c_str());
        }
    }

    // ── The //c, with its IOU mouse and its two serial ports ───────────────
    {
        pom2test::A2osxMachine m(true, "pom2_a2osx_iic.po");
        if (!m.ready()) {
            std::printf("[SKIP] //c: roms/apple2c-32Kv0.rom not found\n");
        } else {
            m.plug(1, "ssc");
            m.plug(2, "ssc");
            m.plug(4, "iicmouse");
            const bool booted = m.boot();
            check(booted, "//c: A2osX reaches the shell");
        }
    }

    if (failures) { std::printf("a2osx_boot: %d failure(s)\n", failures); return 1; }
    std::printf("a2osx_boot: OK\n");
    return 0;
}
