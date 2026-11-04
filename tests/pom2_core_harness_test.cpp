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

// What a test harness asks of the public facade, through nothing but
// <pom2/core.hpp>: RAM by physical bank, the keyboard, and the archive's own
// identity. The point of each is what it does NOT do to the machine it
// inspects, so every test compares the machine before and after.

#include <pom2/core.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string gSource;

pom2::Core makeIIe()
{
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIeEnhanced;
    pom2::Core core(config);
    assert(core.loadRom(gSource + "/roms/apple2e.rom"));
    return core;
}

std::uint8_t ram(pom2::Core& core, pom2::RamBank bank, std::uint16_t at)
{
    std::uint8_t value = 0;
    const bool ok = core.readRam(bank, at, &value, 1);
    assert(ok);
    return value;
}

void put(pom2::Core& core, pom2::RamBank bank, std::uint16_t at, std::uint8_t value)
{
    const bool ok = core.writeRam(bank, at, &value, 1);
    assert(ok);
}

/// Run guest code from $0300 to the JMP-to-self that closes it.
void runGuest(pom2::Core& core, std::initializer_list<std::uint8_t> code)
{
    std::uint16_t at = 0x0300;
    for (const std::uint8_t b : code) put(core, pom2::RamBank::Main, at++, b);
    put(core, pom2::RamBank::Main, at, 0x4C);
    put(core, pom2::RamBank::Main, static_cast<std::uint16_t>(at + 1), static_cast<std::uint8_t>(at));
    put(core, pom2::RamBank::Main, static_cast<std::uint16_t>(at + 2), static_cast<std::uint8_t>(at >> 8));
    pom2::CpuState st = core.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;
    core.setCpuState(st);
    for (int i = 0; i < 2000 && core.cpuState().programCounter != at; ++i) core.step();
    assert(core.cpuState().programCounter == at);
}

// The guest writes one distinct byte into each physical bank; the host then
// finds each byte in its bank and the others untouched, whatever the switches.
void testBanksAreIndependentOfTheSwitches()
{
    using pom2::RamBank;
    pom2::Core core = makeIIe();
    core.coldBoot();

    runGuest(core, {
        // Main $0400 and aux $0400, through the //e's own paging.
        0xA9, 0x11, 0x8D, 0x00, 0x04,       // LDA #$11 / STA $0400       main
        0x8D, 0x05, 0xC0,                   // STA $C005                  RAMWRT on
        0xA9, 0x22, 0x8D, 0x00, 0x04,       // LDA #$22 / STA $0400       aux
        0x8D, 0x04, 0xC0,                   // STA $C004                  RAMWRT off
        // The main language card: bank 2, bank 1 and the upper RAM.
        0xAD, 0x83, 0xC0, 0xAD, 0x83, 0xC0, // $C083 twice: RAM, write-enable, bank 2
        0xA9, 0x33, 0x8D, 0x00, 0xD0,       // STA $D000
        0xA9, 0x55, 0x8D, 0x00, 0xE0,       // STA $E000
        0xAD, 0x8B, 0xC0, 0xAD, 0x8B, 0xC0, // $C08B twice: RAM, write-enable, bank 1
        0xA9, 0x44, 0x8D, 0x00, 0xD0,       // STA $D000
        // The aux language card, through ALTZP.
        0x8D, 0x09, 0xC0,                   // STA $C009                  ALTZP on
        0xA9, 0x77, 0x8D, 0x00, 0xD0,       // STA $D000                  bank 1
        0xA9, 0x88, 0x8D, 0x00, 0xE0,       // STA $E000
        0xAD, 0x83, 0xC0, 0xAD, 0x83, 0xC0, // bank 2
        0xA9, 0x66, 0x8D, 0x00, 0xD0,       // STA $D000
        0x8D, 0x08, 0xC0,                   // STA $C008                  ALTZP off
        0xAD, 0x82, 0xC0,                   // $C082: ROM back
    });

    const auto checkAll = [&core] {
        assert(ram(core, RamBank::Main, 0x0400) == 0x11);
        assert(ram(core, RamBank::Aux, 0x0400) == 0x22);
        assert(ram(core, RamBank::LanguageCard2, 0xD000) == 0x33);
        assert(ram(core, RamBank::LanguageCard1, 0xD000) == 0x44);
        assert(ram(core, RamBank::LanguageCardHigh, 0xE000) == 0x55);
        assert(ram(core, RamBank::AuxLanguageCard2, 0xD000) == 0x66);
        assert(ram(core, RamBank::AuxLanguageCard1, 0xD000) == 0x77);
        assert(ram(core, RamBank::AuxLanguageCardHigh, 0xE000) == 0x88);
    };
    checkAll();

    // Every switch that redirects a bus access: the banks do not move, and
    // the switches are still as the guest left them afterwards.
    core.write(0xC003, 0);                  // RAMRD on
    core.write(0xC005, 0);                  // RAMWRT on
    core.write(0xC009, 0);                  // ALTZP on
    core.write(0xC001, 0);                  // 80STORE on
    core.write(0xC055, 0);                  // PAGE2 on
    (void)core.read(0xC08B);                // language card: bank 1, RAM
    const pom2::DisplayState shown = core.displayState();
    const std::uint8_t seenRamrd = core.read(0xC013);
    const std::uint8_t seenAltzp = core.read(0xC016);
    checkAll();
    assert(core.read(0xC013) == seenRamrd);
    assert(core.read(0xC016) == seenAltzp);
    assert(core.displayState().page2 == shown.page2);
    assert(core.displayState().eightyStore == shown.eightyStore);

    // A bus read at this moment answers aux; readRam still answers each bank.
    assert(core.read(0x0400) == 0x22);
    core.write(0xC054, 0);
    core.write(0xC000, 0);
    core.write(0xC002, 0);
    core.write(0xC004, 0);
    core.write(0xC008, 0);
    assert(core.read(0x0400) == 0x11);
    (void)core.read(0xC082);

    // A write lands in the bank named, and in no other.
    put(core, RamBank::Aux, 0x2000, 0xA5);
    put(core, RamBank::Main, 0x2000, 0x5A);
    assert(ram(core, RamBank::Aux, 0x2000) == 0xA5);
    assert(core.read(0x2000) == 0x5A);
    put(core, RamBank::LanguageCard1, 0xD123, 0xC1);
    assert(ram(core, RamBank::LanguageCard1, 0xD123) == 0xC1);
    assert(ram(core, RamBank::LanguageCard2, 0xD123) != 0xC1);

    // A range: whole, in order, and the screen sees a debugger's write.
    std::vector<std::uint8_t> page(1024);
    for (std::size_t i = 0; i < page.size(); ++i) page[i] = 0xA0 + (i & 0x1F);
    assert(core.writeRam(RamBank::Main, 0x0400, page.data(), page.size()));
    std::vector<std::uint8_t> back(page.size());
    assert(core.readRam(RamBank::Main, 0x0400, back.data(), back.size()));
    assert(back == page);
    const pom2::FramebufferView frame = core.renderFrame();
    assert(frame.pixels && frame.width > 0);
    std::printf("  ok: eight banks, by the guest's writes, whatever the switches\n");
}

void testNothingElseMoves()
{
    using pom2::RamBank;
    pom2::Core core = makeIIe();
    core.coldBoot();
    core.run(2000);

    unsigned watched = 0;
    core.setBusWatch([&watched](std::uint16_t, std::uint8_t, bool, std::uint64_t) { ++watched; });
    core.queueKey('Q');
    const pom2::CpuState before = core.cpuState();

    std::uint8_t screen[1024];
    assert(core.readRam(RamBank::Main, 0x0400, screen, sizeof(screen)));
    assert(core.readRam(RamBank::Aux, 0x0400, screen, sizeof(screen)));
    assert(core.writeRam(RamBank::Main, 0x0400, screen, 1));

    const pom2::CpuState after = core.cpuState();
    assert(after.cycles == before.cycles && after.programCounter == before.programCounter);
    assert(after.accumulator == before.accumulator && after.status == before.status);
    assert(watched == 0);
    assert(core.keyboardState().strobe);             // the key is still unread
    core.setBusWatch({});
    std::printf("  ok: no cycle, no bus watch, no strobe\n");
}

void testRangesAreRefusedWhole()
{
    using pom2::RamBank;
    pom2::Core core = makeIIe();
    std::uint8_t two[2] = { 0x12, 0x34 };
    const std::uint8_t kept = ram(core, RamBank::Main, 0xBFFF);

    const auto refused = [&core, &two](RamBank bank, std::uint16_t at, std::size_t n) {
        assert(!core.readRam(bank, at, two, n));
        assert(!core.lastError().empty());
        assert(!core.writeRam(bank, at, two, n));
        assert(!core.lastError().empty());
    };
    refused(RamBank::Main, 0xBFFF, 2);               // leaves it by one
    refused(RamBank::Main, 0xC000, 1);               // the I/O page is not RAM
    refused(RamBank::Main, 0xD000, 1);               // named by its bank instead
    refused(RamBank::Aux, 0xC000, 1);
    refused(RamBank::LanguageCard1, 0xCFFF, 1);
    refused(RamBank::LanguageCard1, 0xDFFF, 2);
    refused(RamBank::LanguageCard2, 0xE000, 1);
    refused(RamBank::LanguageCardHigh, 0xDFFF, 1);
    refused(RamBank::LanguageCardHigh, 0xFFFF, 2);
    refused(RamBank::AuxLanguageCardHigh, 0xFFFF, 2);
    refused(RamBank::Main, 0x0400, 0);               // nothing asked is not a read
    assert(ram(core, RamBank::Main, 0xBFFF) == kept); // nothing partial landed
    assert(!core.readRam(RamBank::Main, 0, nullptr, 1));
    assert(!core.writeRam(RamBank::Main, 0, nullptr, 1));

    // The ends of each bank are inside it.
    assert(core.readRam(RamBank::LanguageCard1, 0xDFFE, two, 2));
    assert(core.readRam(RamBank::LanguageCardHigh, 0xFFFE, two, 2));
    assert(core.readRam(RamBank::Main, 0xBFFE, two, 2));

    // A ][+ has the language card and no aux memory.
    pom2::CoreConfig plus;
    plus.machine = pom2::Machine::AppleIIPlus;
    pom2::Core iiPlus(plus);
    assert(iiPlus.readRam(RamBank::LanguageCard1, 0xD000, two, 1));
    assert(!iiPlus.readRam(RamBank::Aux, 0x0400, two, 1));
    assert(iiPlus.lastError().find("//e") != std::string::npos);
    assert(!iiPlus.readRam(RamBank::AuxLanguageCard1, 0xD000, two, 1));
    std::printf("  ok: a range leaving its bank is refused whole\n");
}

void testKeyboardState()
{
    pom2::Core core = makeIIe();
    core.coldBoot();

    pom2::KeyboardState state = core.keyboardState();
    assert(!state.strobe && state.queued == 0);

    // Looking does not acknowledge, however often.
    core.queueKey(0x1B);
    for (int i = 0; i < 3; ++i) {
        state = core.keyboardState();
        assert(state.key == 0x1B && state.strobe && state.queued == 0);
    }

    // RETURN is the command byte too, not a line ending to translate.
    core.queueKey(0x0D);
    state = core.keyboardState();
    assert(state.key == 0x0D && state.strobe);

    // The guest takes it: the strobe goes, the character stays latched.
    (void)core.read(0xC010);
    state = core.keyboardState();
    assert(state.key == 0x0D && !state.strobe && state.queued == 0);

    // Queued paste bytes are counted, and looking does not promote them.
    assert(core.pasteText("ABC") == 3);
    const pom2::KeyboardState pasted = core.keyboardState();
    assert(pasted.queued + (pasted.strobe ? 1u : 0u) == 3);
    state = core.keyboardState();
    assert(state.queued == pasted.queued && state.strobe == pasted.strobe);

    // Only the guest's own loop moves them: "a key was taken" is observable.
    unsigned taken = 0;
    for (int guard = 0; guard < 20; ++guard) {
        state = core.keyboardState();
        if (!state.strobe && state.queued == 0) break;
        const std::uint8_t latch = core.read(0xC000);
        if (latch & 0x80) { ++taken; (void)core.read(0xC010); }
    }
    assert(taken == 3);
    state = core.keyboardState();
    assert(!state.strobe && state.queued == 0 && state.key == 'C');
    std::printf("  ok: keyboard state, read without acknowledging\n");
}

void testIdentity()
{
    const pom2::SdkInfo info = pom2::Core::sdkInfo();
    assert(!info.version.empty());
    // A Git checkout, which is what builds this test: 40 hex digits and maybe
    // "-dirty". A build from a bare source tree says "unknown", never a guess.
    if (info.commit != "unknown") {
        const std::string hex = info.commit.substr(0, info.commit.find('-'));
        assert(hex.size() == 40);
        assert(hex.find_first_not_of("0123456789abcdef") == std::string::npos);
        assert(info.commit.size() == 40 || info.commit.substr(40) == "-dirty");
    }
    assert(std::is_sorted(info.capabilities.begin(), info.capabilities.end()));
    assert(std::adjacent_find(info.capabilities.begin(), info.capabilities.end())
           == info.capabilities.end());
    const auto has = [&info](const char* name) {
        return std::find(info.capabilities.begin(), info.capabilities.end(), name)
            != info.capabilities.end();
    };
    for (const char* name : { "harddisk", "ram.banks", "keyboard.state", "provenance",
                              "render.cpu-composite", "disk2", "smartport35" })
        assert(has(name));
    // Declared by its absence: the SDK has no CRT profile (docs/SDK.md § Rendering).
    assert(!has("render.crt"));

    // [B] FIPS 180-2 appendix B.1 and B.2 — the published test vectors, and the
    // empty message, whose padding is the case a length-counting bug misses.
    const auto text = [](const char* s, std::size_t n) {
        return pom2::Core::sha256(reinterpret_cast<const std::uint8_t*>(s), n);
    };
    assert(text("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56)
           == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(text("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(pom2::Core::sha256(nullptr, 0) == text("", 0));

    const std::string path = "pom2_core_harness_sha.bin";
    { std::ofstream out(path, std::ios::binary); out << "abc"; }
    assert(pom2::Core::sha256File(path) == text("abc", 3));
    assert(std::remove(path.c_str()) == 0);
    assert(pom2::Core::sha256File("pom2_core_harness_missing.bin").empty());

    // The program actually in memory: readRam, then its digest.
    pom2::Core core = makeIIe();
    const std::uint8_t program[3] = { 'a', 'b', 'c' };
    assert(core.writeRam(pom2::RamBank::Main, 0x6000, program, sizeof(program)));
    std::uint8_t loaded[3];
    assert(core.readRam(pom2::RamBank::Main, 0x6000, loaded, sizeof(loaded)));
    assert(pom2::Core::sha256(loaded, sizeof(loaded)) == text("abc", 3));
    std::printf("  ok: version, commit, capabilities, SHA-256\n");
}

} // namespace

int main()
{
    gSource = POM2_TEST_SOURCE_DIR;
    testBanksAreIndependentOfTheSwitches();
    testNothingElseMoves();
    testRangesAreRefusedWhole();
    testKeyboardState();
    testIdentity();
    return 0;
}
