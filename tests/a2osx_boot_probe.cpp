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

// a2osx_boot_probe — boot A2osX (disks_3.5/A2osX-0.95-STABLE.800.po, on a
// scratch COPY) on a //e or a //c built piece by piece, and report how far it
// gets: the 80-column screen, the PC, whether the shell prompt appeared.
// Each //c peripheral is a switch, so a hang can be pinned on one of them.
// The machine is tests/A2osxHarness.h, shared with the a2osx_boot ctest.
//
//   a2osx_boot_probe --machine iic [--mouse] [--ssc] [--mb4c] [--seconds N]
//                    [--card N=key]... [--type "command"]... [--hot] [--netlog]
// --card plugs a POM2 catalog card through SlotCardFactory (ROMs included);
// --type types a command at the shell prompt and prints the screen after;
// --disk PATH boots another A2osX image (copied first) instead of 0.95;
// --png PATH saves the screen as POM2's software display paints it;
// --launch CMD types CMD at the S-C assembler prompt (master's BUILD media
// wants `-A2OSX.SYSTEM` there).
// `--type @ssc:N:TEXT` delivers TEXT+CR on slot N's serial line (the seam
// the telnet bridge uses); `@sscread:N` prints what slot N sent; `@wait:N`
// runs N seconds paced to the wall clock (for the real network).
// `--type @send:CMD` types CMD + Return and runs nothing (a --trap follows).
// `--type @oa:5` types 5 with Open-Apple held (A2osX's screen switch).
// `--type @reboot[:N]` cold-starts again from the same (written) disk,
// giving up after N seconds when given.
// --trap HEX steps on after the commands until PC hits HEX and prints the
// PCs that led there; --netlog prints every Ethernet frame; --hot dumps where the CPU spins at the end even when the boot succeeded.
//
// Manual tool (EXCLUDE_FROM_ALL): it asserts nothing.

#include "A2osxHarness.h"
#include "Apple2Display.h"
#include "MemoryWatchSink.h"
#include "GrapplerCard.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {
// A2OSX_RWATCH=HEX: every bus read of that address, with the PC and value.
struct ReadLog : pom2::MemoryWatchSink {
    M6502* cpu = nullptr;
    int left = 200;
    void noteAccess(uint16_t addr, uint8_t value, bool write) override
    {
        if (write || left <= 0) return;
        --left;
        std::printf("[rwatch] $%04X -> %02X '%c' at PC=$%04X\n", addr, value,
                    (value >= 0x20 && value < 0x7F) ? value : '.', cpu->getProgramCounter());
    }
};
}  // namespace

int main(int argc, char** argv)
{
    std::string machine = "iie";
    bool mouse = false, ssc = false, mb4c = false, hot = false, netlog = false;
    int seconds = 30;
    long trap = -1;
    std::string disk, launch, png;
    std::vector<std::pair<int, std::string>> cards;
    std::vector<std::string> commands;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--card" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            if (eq != std::string::npos)
                cards.push_back({std::atoi(spec.substr(0, eq).c_str()), spec.substr(eq + 1)});
        }
        else if (a == "--type" && i + 1 < argc) commands.push_back(argv[++i]);
        else if (a == "--machine" && i + 1 < argc) machine = argv[++i];
        else if (a == "--mouse") mouse = true;
        else if (a == "--ssc") ssc = true;
        else if (a == "--mb4c") mb4c = true;
        else if (a == "--hot") hot = true;
        else if (a == "--netlog") netlog = true;
        else if (a == "--disk" && i + 1 < argc) disk = argv[++i];
        else if (a == "--launch" && i + 1 < argc) launch = argv[++i];
        else if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--trap" && i + 1 < argc) trap = std::strtol(argv[++i], nullptr, 16);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }
    const bool iic = machine == "iic";
    pom2test::A2osxMachine m(iic, "pom2_a2osx_probe.po", disk);
    if (!m.ready()) { std::printf("missing ROM or A2osX disk\n"); return 1; }
    m.setNetLog(netlog);
    if (ssc) { m.plug(1, "ssc"); m.plug(2, "ssc"); }
    if (mouse) m.plug(4, "iicmouse");
    if (mb4c) m.plug(3, "mb4c");
    for (const auto& [slot, key] : cards)
        std::printf("slot %d: %s %s\n", slot, key.c_str(), m.plug(slot, key) ? "plugged" : "NOT CREATED");

    Memory& mem = m.memory();
    M6502&  cpu = m.cpu();
    const bool prompt = m.boot(seconds, launch);
    std::printf("machine=%s mouse=%d ssc=%d mb4c=%d  PC=$%04X  prompt=%s",
                machine.c_str(), mouse, ssc, mb4c, cpu.getProgramCounter(),
                prompt ? "YES" : "no");
    if (prompt) std::printf(" (after %.1f s)", m.bootSeconds());
    if (mb4c) std::printf("  4c awake=%d", mem.iicExpansionAwake() ? 1 : 0);
    std::printf("\n%s", m.screen().c_str());
    // A serial port with no peer drops what it sends; connect a virtual one.
    for (int sl = 1; sl <= 7; ++sl)
        if (auto* ssc = m.card<SuperSerialCard>(sl)) ssc->onTransportConnected();
    ReadLog readLog;
    readLog.cpu = &cpu;
    if (const char* rw = std::getenv("A2OSX_RWATCH")) {
        mem.setWatchSink(&readLog);
        mem.setReadWatch(static_cast<uint16_t>(std::strtol(rw, nullptr, 16)), true);
    }
    // A2OSX_MWATCH=HEX: while the commands run, report every change of the
    // two MAIN-RAM bytes there and the instruction that made it.
    const char* mw = std::getenv("A2OSX_MWATCH");
    const long mwatch = mw ? std::strtol(mw, nullptr, 16) : -1;
    for (const auto& cmd : commands) {
        if (!prompt && cmd.rfind("@", 0) != 0) break;   // @-commands run anyway
        if (cmd.rfind("@reboot", 0) == 0) {   // cold start again, same disk (and its writes)
            // `@reboot:N` gives up after N seconds, so a --trap can catch
            // what the boot itself runs into.
            const int secs = cmd.size() > 8 ? std::atoi(cmd.c_str() + 8) : seconds;
            std::printf("--- reboot: %s\n", m.boot(secs, launch) ? "shell" : "NO SHELL");
            continue;
        }
        if (cmd.rfind("@ssc:", 0) == 0) {   // @ssc:N:text — a peer types on slot N's line
            const int sl = std::atoi(cmd.c_str() + 5);
            std::string text = cmd.substr(cmd.find(':', 5) + 1) + "\r";
            // Typed, not pasted: one character every 0.1 s, as a person on a
            // terminal would — a whole line at the line rate overruns a
            // polled driver on a real 6551 as well.
            const char* gap = std::getenv("A2OSX_SSC_GAP_MS");   // default 100 ms
            const long gapCycles = static_cast<long>(
                (gap ? std::atof(gap) : 100.0) / 1000.0 * pom2test::A2osxMachine::kHz);
            for (const char c : text) {
                if (auto* ssc = m.card<SuperSerialCard>(sl))
                    ssc->deliverTransportBytes(reinterpret_cast<const uint8_t*>(&c), 1, false);
                for (long t = 0; t < gapCycles; ) t += cpu.run(4096);
            }
            for (long t = 0; t < static_cast<long>(4 * pom2test::A2osxMachine::kHz); ) t += cpu.run(4096);
            continue;
        }
        if (cmd.rfind("@sscread:", 0) == 0) {   // @sscread:N — what slot N sent
            const int sl = std::atoi(cmd.c_str() + 9);
            std::vector<uint8_t> tx;
            if (auto* ssc = m.card<SuperSerialCard>(sl))
                for (int i = 0; i < 40; ++i) {   // the ring is paced in wall time
                    ssc->drainTransportTx(tx);
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
            std::printf("--- slot %d sent %zu bytes:\n", sl, tx.size());
            for (uint8_t c : tx)
                std::printf((c >= 0x20 && c < 0x7F) || c == '\n' ? "%c" : (c == '\r' ? "" : "<%02X>"), c);
            std::printf("\n");
            continue;
        }
        if (cmd.rfind("@wait:", 0) == 0) {   // @wait:N — N seconds at the real pace
            const auto start = std::chrono::steady_clock::now();
            const long total = static_cast<long>(std::atoi(cmd.c_str() + 6) * pom2test::A2osxMachine::kHz);
            for (long t = 0; t < total; ) {
                t += cpu.run(4096);
                std::this_thread::sleep_until(start + std::chrono::microseconds(
                    static_cast<long long>(t / pom2test::A2osxMachine::kHz * 1e6)));
            }
            std::printf("--- after waiting %s s:\n%s", cmd.c_str() + 6, m.screen().c_str());
            continue;
        }
        if (cmd.rfind("@send:", 0) == 0) {   // type it and return at once (for --trap)
            mem.pasteText(cmd.substr(6) + "\r");
            continue;
        }
        if (cmd.rfind("@oa:", 0) == 0) {   // Open-Apple held, then the key
            mem.setOpenAppleKey(true);
            mem.pasteText(cmd.substr(4));
            for (long t = 0; t < static_cast<long>(pom2test::A2osxMachine::kHz); ) t += cpu.run(4096);
            mem.setOpenAppleKey(false);
            for (long t = 0; t < static_cast<long>(3 * pom2test::A2osxMachine::kHz); ) t += cpu.run(4096);
            std::printf("--- after Open-Apple %s\n%s", cmd.substr(4).c_str(), m.screen().c_str());
            continue;
        }
        if (mwatch < 0) {
            std::printf("--- after '%s':\n%s", cmd.c_str(), m.type(cmd).c_str());
            continue;
        }
        mem.pasteText(cmd + "\r");
        const uint16_t a = static_cast<uint16_t>(mwatch);
        uint16_t last = static_cast<uint16_t>(mem.peekMainRam(a) | mem.peekMainRam(a + 1) << 8);
        for (long t = 0; t < static_cast<long>(8 * pom2test::A2osxMachine::kHz); ) {
            const uint16_t pc = cpu.getProgramCounter();
            t += cpu.run(1);
            const uint16_t now = static_cast<uint16_t>(mem.peekMainRam(a) | mem.peekMainRam(a + 1) << 8);
            if (now != last) {
                std::printf("[mwatch] '%s' +%ld cyc: $%04X %04X -> %04X by PC=$%04X flags=%03X\n",
                            cmd.c_str(), t, a, last, now, pc, mem.iieModeFlags());
                std::printf("          code:");
                for (int b = -12; b < 4; ++b)
                    std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(pc + b)));
                std::printf("  A=%02X X=%02X Y=%02X  $00..$0F:", cpu.getAccumulator(), cpu.getXRegister(), cpu.getYRegister());
                for (int b = 0; b < 16; ++b) std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(b)));
                std::printf("\n");
                // A2osX's handle table (main $0800-$0FFF, 8 bytes a handle:
                // flags, refcount, owner, type, ptr, size): who covers `a`?
                for (int h = 1; h < 256; ++h) {
                    const uint16_t e = static_cast<uint16_t>(0x800 + h * 8);
                    if (!(mem.peekMainRam(e) & 0x80)) continue;
                    const unsigned ptr = mem.peekMainRam(e + 4) | mem.peekMainRam(e + 5) << 8;
                    const unsigned size = mem.peekMainRam(e + 6) | mem.peekMainRam(e + 7) << 8;
                    if (a >= ptr && a < ptr + size)
                        std::printf("          handle $%02X: %02X %02X owner %02X type %02X ptr $%04X size $%04X\n",
                                    h, mem.peekMainRam(e), mem.peekMainRam(e + 1), mem.peekMainRam(e + 2),
                                    mem.peekMainRam(e + 3), ptr, size);
                }
                {
                    const uint16_t src = static_cast<uint16_t>(mem.peekCpuView(0) | mem.peekCpuView(1) << 8);
                    const uint16_t dst = static_cast<uint16_t>(mem.peekCpuView(2) | mem.peekCpuView(3) << 8);
                    std::printf("          src $%04X: \"", src);
                    for (int b = 0; b < 48; ++b) {
                        const uint8_t c = mem.peekCpuView(static_cast<uint16_t>(src + b));
                        if (!c) break;
                        std::printf((c >= 0x20 && c < 0x7F) ? "%c" : "<%02X>", c);
                    }
                    std::printf("\"\n          src raw:");
                    for (int b = 0; b < 20; ++b) std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(src + b)));
                    std::printf("\n          dst raw:");
                    for (int b = 0; b < 20; ++b) std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(dst + b)));
                    std::printf("\n");
                    for (int h = 1; h < 256; ++h) {
                        const uint16_t e = static_cast<uint16_t>(0x800 + h * 8);
                        const unsigned ptr = mem.peekMainRam(e + 4) | mem.peekMainRam(e + 5) << 8;
                        if (ptr == dst)
                            std::printf("          dst handle $%02X: %02X %02X owner %02X type %02X ptr $%04X size $%04X\n",
                                        h, mem.peekMainRam(e), mem.peekMainRam(e + 1), mem.peekMainRam(e + 2),
                                        mem.peekMainRam(e + 3), ptr,
                                        mem.peekMainRam(e + 6) | mem.peekMainRam(e + 7) << 8);
                    }
                }
                const uint8_t* x = mem.auxData();
                for (int h = 1; h < 256; ++h) {
                    const uint16_t e = static_cast<uint16_t>(0x800 + h * 8);
                    if (!(x[e] & 0x80)) continue;
                    const unsigned ptr = x[e + 4] | x[e + 5] << 8;
                    const unsigned size = x[e + 6] | x[e + 7] << 8;
                    if (a >= ptr && a < ptr + size)
                        std::printf("          AUX handle $%02X: %02X %02X owner %02X type %02X ptr $%04X size $%04X\n",
                                    h, x[e], x[e + 1], x[e + 2], x[e + 3], ptr, size);
                }
            }
            last = now;
        }
        std::printf("--- after '%s':\n%s", cmd.c_str(), m.screen().c_str());
    }
    // --trap: step on until PC reaches the address; print the PCs before.
    if (prompt && trap >= 0) {
        std::vector<uint16_t> ring(256);
        size_t n = 0;
        long steps = 0;
        // A2OSX_WATCH=HEX: at each pass there, note the paging flags and the
        // pointer at $10/$11 with what it points at (+4/+5) in main and aux.
        const char* w = std::getenv("A2OSX_WATCH");
        const long watch = w ? std::strtol(w, nullptr, 16) : -1;
        std::vector<std::string> notes;
        for (; steps < 40L * 1000 * 1000; ++steps) {
            const uint16_t pc = cpu.getProgramCounter();
            if (pc == trap) break;
            if (pc == watch) {
                const uint16_t p = static_cast<uint16_t>(mem.peekCpuView(0x10) | mem.peekCpuView(0x11) << 8);
                char line[160];
                std::snprintf(line, sizeof line,
                    "flags=%03X ptr=$%04X main+4=%02X%02X aux+4=%02X%02X A=%02X X=%02X Y=%02X",
                    mem.iieModeFlags(), p,
                    mem.peekMainRam(static_cast<uint16_t>(p + 5)), mem.peekMainRam(static_cast<uint16_t>(p + 4)),
                    mem.auxData()[static_cast<uint16_t>(p + 5)], mem.auxData()[static_cast<uint16_t>(p + 4)],
                    cpu.getAccumulator(), cpu.getXRegister(), cpu.getYRegister());
                std::string withStack = line;
                withStack += " SP=";
                char b[8];
                std::snprintf(b, sizeof b, "%02X:", cpu.getStackPointer());
                withStack += b;
                for (int i = 1; i <= 12; ++i) {
                    std::snprintf(b, sizeof b, " %02X", mem.peekCpuView(static_cast<uint16_t>(
                        0x100 + ((cpu.getStackPointer() + i) & 0xFF))));
                    withStack += b;
                }
                notes.push_back(withStack);
                if (notes.size() > 6) notes.erase(notes.begin());
            }
            ring[n++ % ring.size()] = pc;
            cpu.run(1);
        }
        for (const auto& n : notes) std::printf("watch: %s\n", n.c_str());
        std::printf("trap $%04lX after %ld instructions; last PCs:\n", trap, steps);
        for (size_t i = (n > ring.size() ? n - ring.size() : 0); i < n; ++i)
            std::printf(" %04X%s", ring[i % ring.size()], (i % 16 == 15) ? "\n" : "");
        // A2OSX_DUMP=HEX[,HEX]...: 24 bytes each, as the CPU sees them;
        // prefix m or x to read MAIN or AUX RAM whatever the switches say.
        for (const char* spec = std::getenv("A2OSX_DUMP"); spec && *spec; ) {
            const char bank = (*spec == 'm' || *spec == 'x') ? *spec++ : 'c';
            char* end = nullptr;
            const long a = std::strtol(spec, &end, 16);
            if (end == spec) break;
            std::printf("\n  %c$%04lX:", bank, a);
            for (long b = a; b < a + 24; ++b) {
                const uint16_t at = static_cast<uint16_t>(b);
                std::printf(" %02X", bank == 'm' ? mem.peekMainRam(at)
                                   : bank == 'x' ? mem.auxData()[at] : mem.peekCpuView(at));
            }
            spec = (*end == ',') ? end + 1 : end;
        }
        std::printf("\nSP=$%02X stack:", cpu.getStackPointer());
        for (int i = 1; i <= 16; ++i)
            std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(0x100 + ((cpu.getStackPointer() + i) & 0xFF))));
        std::printf("\n");
    }
    for (int sl = 1; sl <= 7; ++sl)
        if (auto* tc = m.card<ClockCard>(sl))
            std::printf("slot %d clock: irqEnabled=%d pending=%d tp=%d Hz\n", sl,
                        tc->interruptsEnabled(), tc->interruptPending(), tc->tpRateHz());
    {
        const auto d = mem.getDisplayState();
        std::printf("display: text=%d mixed=%d hires=%d page2=%d 80col=%d dhgr=%d\n",
                    d.textMode, d.mixedMode, d.hiRes, d.page2, d.eightyCol, d.dhgr);
    }
    // --png: the picture as POM2's software display paints it, through the
    // Chat Mauve card when one is plugged.
    if (!png.empty()) {
        Apple2Display d;
        d.setAuxMemory(mem.auxData());
        for (int sl = 1; sl <= 7; ++sl)
            if (auto* lcm = m.card<LeChatMauveCard>(sl)) d.setChatMauveCard(lcm);
        d.render(mem);
        const bool ok = stbi_write_png(png.c_str(), d.width(), d.height(), 4, d.pixels(), d.width() * 4) != 0;
        std::printf("png: %s %dx%d %s\n", png.c_str(), d.width(), d.height(), ok ? "written" : "FAILED");
    }
    // What left the machine through each card.
    auto show = [](const char* what, int sl, const std::vector<uint8_t>& b) {
        if (b.empty()) return;
        std::printf("slot %d %s (%zu bytes):", sl, what, b.size());
        for (uint8_t c : b) std::printf((c >= 0x20 && c < 0x7F) ? "%c" : "<%02X>", c);
        std::printf("\n");
    };
    for (int sl = 1; sl <= 7; ++sl) {
        if (auto* g = m.card<GrapplerCard>(sl)) show("Grappler+ printed", sl, g->spoolBytes());
        if (auto* ssc = m.card<SuperSerialCard>(sl)) {
            std::vector<uint8_t> tx;
            std::printf("slot %d SSC: %zu bytes queued\n", sl, ssc->txQueueDepth());
            ssc->drainTransportTx(tx);
            show("SSC sent", sl, tx);
            const auto in = ssc->inputLines();
            std::printf("slot %d SSC: DTR=%d DCD=%d DSR=%d CTS=%d recentTx='%s'\n", sl,
                        ssc->dtrAsserted(), in.dcd, in.dsr, in.cts, ssc->recentTxText().c_str());
        }
    }
    if (!prompt || hot) {
        // Where the time goes: 200 000 instructions, PCs bucketed by 16 bytes.
        std::map<int, long> hist;
        for (int i = 0; i < 200000; ++i) { ++hist[cpu.getProgramCounter() & 0xFFF0]; cpu.run(1); }
        std::vector<std::pair<long, int>> top;
        for (auto& [k, v] : hist) top.push_back({v, k});
        std::sort(top.rbegin(), top.rend());
        std::printf("hot:");
        for (size_t i = 0; i < top.size() && i < 12; ++i)
            std::printf(" $%04X:%ld", top[i].second, top[i].first);
        std::printf("\ntrace:");
        for (int i = 0; i < 64; ++i) { std::printf(" %04X", cpu.getProgramCounter()); cpu.run(1); }
        const uint16_t pc = cpu.getProgramCounter();
        std::printf("\ncode at $%04X:", static_cast<unsigned>(pc & 0xFFF0) - 0x20);
        for (int a = (pc & 0xFFF0) - 0x20; a < (pc & 0xFFF0) + 0x30; ++a)
            std::printf("%s%02X", (a & 15) ? " " : "\n  ", mem.peekCpuView(static_cast<uint16_t>(a)));
        std::printf("\nstack (SP=$%02X):", cpu.getStackPointer());
        for (int i = 1; i <= 12; ++i)
            std::printf(" %02X", mem.peekCpuView(static_cast<uint16_t>(0x100 + ((cpu.getStackPointer() + i) & 0xFF))));
        std::printf("\n");
        // The code a BRK was raised from: the stacked return address - 2.
        const uint16_t ret = static_cast<uint16_t>(
            mem.peekCpuView(static_cast<uint16_t>(0x100 + ((cpu.getStackPointer() + 2) & 0xFF)))
            | mem.peekCpuView(static_cast<uint16_t>(0x100 + ((cpu.getStackPointer() + 3) & 0xFF))) << 8);
        std::printf("around $%04X (cpu view):", ret - 2);
        for (int a = (ret & 0xFFF0) - 0x30; a < (ret & 0xFFF0) + 0x10; ++a)
            std::printf("%s%02X", (a & 15) ? " " : "\n  ", mem.peekCpuView(static_cast<uint16_t>(a)));
        std::printf("\n");
    }
    return prompt ? 0 : 2;
}
