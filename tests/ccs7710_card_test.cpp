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

// ccs7710_card — the CCS 7710 (Ccs7710Card.h) and the SoftCard II mailbox
// decoder (SoftCard2Protocol.h).
//
//  1. The 6850 on the bus: A0-only decode (a $C0nE/$C0nF pair is the same
//     two registers), master reset, TDRE, the spool, RDRF and the receive
//     queue, the interrupt enables.
//  2. The firmware, differentially: on a ][+, Applesoft's PR#1 / PRINT and
//     an IN#1 that takes Applesoft lines from the line, once on CCS's
//     7710-03 PROMs (the LLE reference, when roms/ccs7710_03h.u5 + l.u6 are
//     there) and once on POM2's page — the bytes sent and both screens must
//     be identical.
//  3. The two nibble PROMs, when given, replace the page.
//  4. SoftCard2Protocol on synthetic traffic.

#include "Ccs7710Card.h"
#include "M6502.h"
#include "Memory.h"
#include "SoftCard2Protocol.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

std::string spool(const Ccs7710Card& c)
{
    std::vector<uint8_t> out;
    c.drainSpoolFrom(0, out);
    return { out.begin(), out.end() };
}

void testAcia()
{
    Ccs7710Card c(1);
    CHECK(c.slotRomRead(0x05) == 0x18 && c.slotRomRead(0x07) == 0x38);
    CHECK(!c.romLayoutError());
    // Powered up in master reset: TDRE, nothing received, nothing sent.
    CHECK(c.deviceSelectRead(0x0) == 0x02);
    c.deviceSelectWrite(0x1, 'X');
    CHECK(c.bytesWritten() == 0 && "held in reset: the byte is dropped");
    c.deviceSelectWrite(0xE, 0x15);                  // 8N1, /16, no IRQs
    CHECK(c.deviceSelectRead(0x2) == 0x02);          // A0 only: $C0n2 = status
    c.deviceSelectWrite(0xF, 'H');                   // $C0nF = data
    c.deviceSelectWrite(0x1, 'I');
    CHECK(spool(c) == "HI");
    c.receive('a');
    c.receive('b');
    CHECK((c.deviceSelectRead(0xE) & 0x01) && "RDRF");
    CHECK(c.deviceSelectRead(0xF) == 'a');
    CHECK(c.deviceSelectRead(0x1) == 'b');
    CHECK(!(c.deviceSelectRead(0x0) & 0x01) && c.pendingInput() == 0);
    // Interrupts: receive (CR7) and transmit (CR6:5 = 01).
    c.deviceSelectWrite(0x0, 0x95);
    CHECK(!(c.status() & 0x80));
    c.receive('z');
    CHECK(c.status() & 0x80);
    c.deviceSelectRead(0x1);
    CHECK(!(c.status() & 0x80));
    c.deviceSelectWrite(0x0, 0x35);
    CHECK(c.status() & 0x80 && "TIE with TDRE set");
    c.deviceSelectWrite(0x0, 0x03);
    CHECK(!(c.status() & 0x80) && "master reset");
    std::printf("[ OK ] 6850: decode, reset, spool, receive queue, IRQs\n");
}

uint16_t rowBase(int r) { return uint16_t(0x0400 + (r % 8) * 0x80 + (r / 8) * 0x28); }

std::string screen40(Memory& m)
{
    std::string out;
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 40; ++c) {
            const char ch = char(m.peekMainRam(uint16_t(rowBase(r) + c)) & 0x7F);
            out.push_back(ch >= 0x20 && ch < 0x7F ? ch : '.');
        }
        out.push_back('\n');
    }
    return out;
}

std::vector<uint8_t> slurp(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    std::vector<uint8_t> v;
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

struct Run { std::string sent; std::string screenAfterPr; std::string screenAfterIn; };

// One ][+ with the card in slot 1, firmware `lle` (CCS's 7710-03 PROMs) or
// POM2's: PR#1 / PRINT "HELLO" / PR#0, then IN#1 with Applesoft lines
// arriving on the line.
Run scenario(bool lle)
{
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.loadAppleIIRom("roms/apple2p.rom");
    auto card = std::make_unique<Ccs7710Card>(1);
    Ccs7710Card* c = card.get();
    if (lle) CHECK(c->loadProms(slurp(Ccs7710Card::kPromHiPath), slurp(Ccs7710Card::kPromLoPath)));
    mem.slotBus().plug(1, std::move(card));
    cpu.hardReset();
    mem.slotBus().reset();
    auto run = [&](long n) { for (long t = 0; t < n;) t += cpu.run(4096); };
    run(2'000'000);
    Run r;
    mem.pasteText("PR#1\rPRINT \"HELLO\"\rPR#0\r");
    run(3'000'000);
    r.sent = spool(*c);
    r.screenAfterPr = screen40(mem);
    mem.pasteText("HOME\rIN#1\r");
    run(1'000'000);
    for (char ch : std::string("PRINT 6*7\r\nIN#0\r")) c->receive(uint8_t(ch));
    run(3'000'000);
    r.screenAfterIn = screen40(mem);
    return r;
}

void testFirmwareAgainstProm()
{
    if (!std::filesystem::is_regular_file("roms/apple2p.rom")) {
        std::printf("[SKIP] firmware runs: needs roms/apple2p.rom\n");
        return;
    }
    const Run hle = scenario(false);
    std::string sent7;
    for (unsigned char ch : hle.sent) sent7.push_back(char(ch & 0x7F));
    CHECK(sent7.find("HELLO\r") != std::string::npos && "sent, high bit set as Applesoft gave it");
    CHECK(!hle.sent.empty() && (static_cast<unsigned char>(hle.sent[0]) & 0x80));
    CHECK(hle.screenAfterPr.find("HELLO") != std::string::npos && "echoed on the screen");
    CHECK(hle.screenAfterIn.find("42") != std::string::npos && "IN#1 ran a line from the 6850");
    if (!std::filesystem::is_regular_file(Ccs7710Card::kPromHiPath)
        || !std::filesystem::is_regular_file(Ccs7710Card::kPromLoPath)) {
        std::printf("[ OK ] POM2's firmware: PR#1 sends + echoes, IN#1 reads "
                    "(SKIP the comparison: CCS's PROMs absent)\n");
        return;
    }
    const Run lle = scenario(true);
    // The line: byte for byte (8 bits — Applesoft's high bit goes out too).
    CHECK(lle.sent == hle.sent);
    CHECK(lle.screenAfterPr == hle.screenAfterPr);
    CHECK(lle.screenAfterIn == hle.screenAfterIn);
    if (lle.sent != hle.sent || lle.screenAfterPr != hle.screenAfterPr
        || lle.screenAfterIn != hle.screenAfterIn) {
        auto hex = [](const std::string& t) {
            std::string o; char b[4];
            for (unsigned char ch : t) { std::snprintf(b, sizeof b, "%02X ", ch); o += b; }
            return o;
        };
        std::fprintf(stderr, "  CCS sent:  %s\n  POM2 sent: %s\n", hex(lle.sent).c_str(), hex(hle.sent).c_str());
        std::fprintf(stderr, "---- CCS after PR ----\n%s---- POM2 after PR ----\n%s",
                     lle.screenAfterPr.c_str(), hle.screenAfterPr.c_str());
        std::fprintf(stderr, "---- CCS after IN ----\n%s---- POM2 after IN ----\n%s",
                     lle.screenAfterIn.c_str(), hle.screenAfterIn.c_str());
    }
    std::printf("[ OK ] POM2's firmware = CCS 7710-03: same bytes on the line, same screens\n");
}

void testProms()
{
    Ccs7710Card c(3);
    std::vector<uint8_t> hi(256), lo(256);
    for (int i = 0; i < 256; ++i) { hi[std::size_t(i)] = uint8_t(0xA0 | (i >> 4)); lo[std::size_t(i)] = uint8_t(0x50 | (i & 15)); }
    CHECK(!c.loadProms(hi, std::vector<uint8_t>(255)));
    CHECK(c.loadProms(hi, lo) && c.promsLoaded());
    CHECK(c.slotRomRead(0x00) == 0x00 && c.slotRomRead(0x5A) == 0x5A && c.slotRomRead(0xFF) == 0xFF);
    std::printf("[ OK ] nibble PROMs: high nibble from u5, low from u6\n");
}

void testProtocol()
{
    std::vector<std::string> lines;
    pom2::SoftCard2Protocol p([&](const std::string& l) { lines.push_back(l); });
    // The bootstrap's 6502 -> Z80 stream before any command: ignored.
    for (int i = 0; i < 300; ++i) p.appleSent(uint8_t(i));
    CHECK(lines.empty());
    // READSEC that succeeds: status 0, then 256 bytes.
    for (uint8_t b : { 3, 17, 1, 6, 0 }) p.z80Sent(b);
    p.appleSent(0);
    CHECK(lines.empty() && "waits for the sector");
    for (int i = 0; i < 256; ++i) p.appleSent(0xEE);
    CHECK(lines.size() == 1 && lines[0] == "SC2  READSEC S6,D1 T17 sector 0 -> ok");
    // READSEC that fails: the status alone.
    for (uint8_t b : { 3, 0, 2, 6, 5 }) p.z80Sent(b);
    p.appleSent(0x40);
    CHECK(lines.back() == "SC2  READSEC S6,D2 T0 sector 5 -> $40 I/O error");
    // WRITESLOT: slot, the console status, then the character.
    p.z80Sent(6); p.z80Sent(3); p.appleSent(0); p.z80Sent('A');
    CHECK(lines.back() == "SC2  WRITESLOT S3 'A'");
    // CALLSUB with its four-byte reply.
    for (uint8_t b : { 0, 0x3A, 0xFF, 1, 2, 3 }) p.z80Sent(b);
    for (uint8_t b : { 9, 8, 7, 0x30 }) p.appleSent(b);
    CHECK(lines.back() == "SC2  CALLSUB $FF3A A=$01 X=$02 Y=$03 -> A=$09 X=$08 Y=$07 P=$30");
    // INVERT takes nothing either way; a stray byte is named and skipped.
    p.z80Sent(14);
    CHECK(lines.back() == "SC2  INVERT");
    p.z80Sent(0xA5);
    CHECK(lines.back() == "SC2  $A5 (not a 6502 BIOS call)");
    std::printf("[ OK ] SoftCard II mailbox calls decoded\n");
}

} // namespace

int main()
{
    testAcia();
    testFirmwareAgainstProm();
    testProms();
    testProtocol();
    if (g_fail) {
        std::fprintf(stderr, "ccs7710_card: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ccs7710_card: PASS\n");
    return 0;
}
