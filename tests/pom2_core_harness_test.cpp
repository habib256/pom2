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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
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
    for (const char* name : { "accelerator", "harddisk", "ram.banks", "ram.regions", "keyboard.state", "provenance",
                              "render.cpu-composite", "render.modes", "disk2", "drive.sounds", "noslotclock", "phasor", "ramworks", "smartport35" })
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

// ramRegion is readRam's cells in place: the same bytes both ways, a stable
// pointer across a move of the Core, and no aux bank without //e memory.
void testRamRegions()
{
    using pom2::RamBank;
    pom2::Core core = makeIIe();
    const struct { RamBank bank; std::uint16_t first; std::size_t size; } kRegions[] = {
        {RamBank::Main, 0x0000, 0xC000}, {RamBank::Aux, 0x0000, 0xC000},
        {RamBank::LanguageCard1, 0xD000, 0x1000}, {RamBank::LanguageCard2, 0xD000, 0x1000},
        {RamBank::LanguageCardHigh, 0xE000, 0x2000}, {RamBank::AuxLanguageCard1, 0xD000, 0x1000},
        {RamBank::AuxLanguageCard2, 0xD000, 0x1000}, {RamBank::AuxLanguageCardHigh, 0xE000, 0x2000},
    };
    std::uint8_t tag = 0x11;
    for (const auto& r : kRegions) {
        const pom2::RamRegion region = core.ramRegion(r.bank);
        assert(region.data && region.size == r.size);
        region.data[0] = tag;
        region.data[r.size - 1] = static_cast<std::uint8_t>(tag + 1);
        assert(ram(core, r.bank, r.first) == tag);
        assert(ram(core, r.bank, static_cast<std::uint16_t>(r.first + r.size - 1)) == tag + 1);
        put(core, r.bank, static_cast<std::uint16_t>(r.first + 1), static_cast<std::uint8_t>(tag + 2));
        assert(region.data[1] == tag + 2);
        tag = static_cast<std::uint8_t>(tag + 0x10);
    }
    std::uint8_t* main = core.ramRegion(RamBank::Main).data;
    pom2::Core moved = std::move(core);
    assert(moved.ramRegion(RamBank::Main).data == main);

    pom2::CoreConfig plus;
    plus.machine = pom2::Machine::AppleIIPlus;
    pom2::Core iiPlus(plus);
    assert(iiPlus.ramRegion(RamBank::Main).size == 0xC000);
    assert(iiPlus.ramRegion(RamBank::LanguageCardHigh).size == 0x2000);
    assert(!iiPlus.ramRegion(RamBank::Aux).data && iiPlus.ramRegion(RamBank::Aux).size == 0);
    std::printf("  ok: RAM regions in place\n");
}

// Every display mode paints; Rgb needs its card and leaves the mode alone
// without one; the monochrome monitors paint nothing but their phosphor.
void testDisplayModes()
{
    using pom2::DisplayMode;
    pom2::Core core = makeIIe();
    assert(core.loadCharacterRom(gSource + "/roms/apple2e_char.rom"));
    core.coldBoot();
    core.run(1022727);   // the ROM's banner and prompt
    assert(core.displayMode() == DisplayMode::Composite);
    assert(!core.setDisplayMode(DisplayMode::Rgb) && core.displayMode() == DisplayMode::Composite);
    const DisplayMode kModes[] = {
        DisplayMode::Composite, DisplayMode::CompositeMedium, DisplayMode::CompositeSharp,
        DisplayMode::CompositeOpenEmulator, DisplayMode::AppleWinMonitor, DisplayMode::AppleWinTv,
        DisplayMode::AppleWinIdealized, DisplayMode::MonoWhite, DisplayMode::MonoGreen,
        DisplayMode::MonoAmber,
    };
    for (DisplayMode mode : kModes) {
        assert(core.setDisplayMode(mode) && core.displayMode() == mode);
        const pom2::FramebufferView frame = core.renderFrame();
        assert(frame.pixels && (frame.width == 280 || frame.width == 560) && frame.height == 192);
        std::size_t lit = 0;
        for (int i = 0; i < frame.width * frame.height; ++i) {
            const std::uint32_t p = frame.pixels[i];
            const unsigned r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
            if (r + g + b < 96) continue;
            ++lit;
            if (mode == DisplayMode::MonoGreen) assert(g > r && g > b);
            if (mode == DisplayMode::MonoAmber) assert(r > b);
        }
        assert(lit > 0 && "the banner must show in every mode");
    }
    assert(core.attachRgbCard(7));
    assert(core.setDisplayMode(DisplayMode::Rgb) && core.displayMode() == DisplayMode::Rgb);
    assert(core.renderFrame().pixels);
    std::printf("  ok: display modes\n");
}

// attachDriveSounds: a missing sample folder is refused; with the samples, the
// same boot sounds different from a machine without them, and only because of
// the drive (the two run identical guests).
void testDriveSounds()
{
    const std::string disk = gSource + "/packaging/disks/DIRTBIKE.dsk";
    // events: 1 sounds on, 0 sounds attached but their events off, -1 none;
    // resyncAt: the frame a restore would resync them (events back on).
    const auto boot = [&](int events, int resyncAt = -1) {
        pom2::Core core = makeIIe();
        assert(core.loadCharacterRom(gSource + "/roms/apple2e_char.rom"));
        if (events >= 0) {
            assert(!core.attachDriveSounds(gSource + "/no/such/folder") && !core.driveSoundsAttached());
            assert(core.attachDriveSounds(gSource + "/roms/floppy_samples") && core.driveSoundsAttached());
            core.setDriveSoundVolume(1.0f);
        }
        assert(core.attachDiskII({}));
        assert(core.insertDisk(0, disk));
        assert(core.bootDisk());
        core.setDriveSoundEventsEnabled(events == 1);
        std::vector<float> audio;
        std::vector<float> chunk(2 * 735);
        for (int frame = 0; frame < 60; ++frame) {
            if (frame == resyncAt) {
                core.setDriveSoundEventsEnabled(true);
                core.resyncDriveSounds();
            }
            core.run(17030);
            assert(core.pullAudio(chunk.data(), 735) == 735);
            audio.insert(audio.end(), chunk.begin(), chunk.end());
        }
        return audio;
    };
    const auto distance = [](const std::vector<float>& a, const std::vector<float>& b, std::size_t from) {
        double d = 0;
        for (std::size_t i = from; i < a.size(); ++i) d += std::abs(a[i] - b[i]);
        return d;
    };
    const auto quiet = boot(-1);
    const auto loud = boot(1);
    assert(distance(loud, quiet, 0) > 1.0 && "the drive's motor and steps must reach the mix");
    // Events off (a run-ahead frame): the drive is silent.
    const auto muted = boot(0);
    assert(distance(muted, quiet, 0) < 1e-3 && "with events off the drive makes no sound");
    // A restore mid-boot resyncs: the motor turning is heard from then on.
    const auto resynced = boot(0, 30);
    assert(distance(resynced, quiet, 0) < 1e-3 + distance(resynced, quiet, 30 * 2 * 735) &&
           distance(resynced, quiet, 30 * 2 * 735) > 1.0 && "a resync picks up the turning motor");
    std::printf("  ok: drive sounds\n");
}

// attachNoSlotClock with a start date: a guest reading the DS1216E by its
// own protocol gets that date, ten emulated seconds later the date ten
// seconds on, and after a snapshot restore the date the snapshot holds.
std::tm readNoSlotClock(pom2::Core& core)
{
    // LDA $C304 resets the key; 64 reads of $C300|bit send C5 3A A3 5C x2,
    // LSB first; 64 reads of $C304 shift the registers out on D0.
    const std::uint8_t key[8] = {0xC5, 0x3A, 0xA3, 0x5C, 0xC5, 0x3A, 0xA3, 0x5C};
    assert(core.writeRam(pom2::RamBank::Main, 0x0380, key, sizeof(key)));
    const std::uint8_t code[] = {
        0xAD, 0x04, 0xC3, 0xA2, 0x00,                   // LDA $C304; LDX #0
        0xBD, 0x80, 0x03, 0x85, 0x00, 0xA0, 0x08,       // key byte: LDA $0380,X; STA $00; LDY #8
        0xA5, 0x00, 0x29, 0x01, 0xF0, 0x06,             // key bit: LDA $00; AND #1; BEQ zero
        0xAD, 0x01, 0xC3, 0x4C, 0x1B, 0x03,             // LDA $C301; JMP next
        0xAD, 0x00, 0xC3,                               // zero: LDA $C300
        0x46, 0x00, 0x88, 0xD0, 0xEC,                   // next: LSR $00; DEY; BNE key bit
        0xE8, 0xE0, 0x08, 0xD0, 0xE0,                   // INX; CPX #8; BNE key byte
        0xA2, 0x00,                                     // LDX #0
        0xA0, 0x08, 0xA9, 0x00, 0x85, 0x01,             // byte: LDY #8; LDA #0; STA $01
        0xAD, 0x04, 0xC3, 0x4A, 0x66, 0x01,             // bit: LDA $C304; LSR A; ROR $01
        0x88, 0xD0, 0xF7,                               // DEY; BNE bit
        0xA5, 0x01, 0x9D, 0x90, 0x03,                   // LDA $01; STA $0390,X
        0xE8, 0xE0, 0x08, 0xD0, 0xE7,                   // INX; CPX #8; BNE byte
        0x4C, 0x40, 0x03,                               // JMP *
    };
    assert(core.writeRam(pom2::RamBank::Main, 0x0300, code, sizeof(code)));
    pom2::CpuState st = core.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;
    core.setCpuState(st);
    for (int i = 0; i < 20000 && core.cpuState().programCounter != 0x0340; ++i) core.step();
    assert(core.cpuState().programCounter == 0x0340);
    std::uint8_t r[8];
    assert(core.readRam(pom2::RamBank::Main, 0x0390, r, sizeof(r)));
    const auto bcd = [](std::uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); };
    std::tm tm{};
    tm.tm_sec = bcd(r[1]); tm.tm_min = bcd(r[2]); tm.tm_hour = bcd(r[3]);
    tm.tm_mday = bcd(r[5]); tm.tm_mon = bcd(r[6]) - 1; tm.tm_year = 100 + bcd(r[7]);
    tm.tm_isdst = -1;
    return tm;
}

void testNoSlotClock()
{
    constexpr std::int64_t start = 1700000000;   // 2023-11-14 22:13:20 UTC
    pom2::Core core = makeIIe();
    assert(!core.noSlotClockAttached());
    assert(core.attachNoSlotClock(start) && core.noSlotClockAttached());
    assert(!core.attachNoSlotClock(start) && "one chip");
    std::tm first = readNoSlotClock(core);
    const std::time_t t0 = std::mktime(&first);
    assert(t0 >= start && t0 <= start + 1 && "the guest reads the start date");
    core.run(10 * 1022727);
    std::tm later = readNoSlotClock(core);
    const std::time_t t1 = std::mktime(&later);
    assert(t1 - t0 >= 9 && t1 - t0 <= 11 && "ten emulated seconds later");

    std::vector<std::uint8_t> state;
    assert(core.saveState(state));
    std::tm atSave = readNoSlotClock(core);
    core.run(30 * 1022727);
    assert(core.loadState(state.data(), state.size()));
    std::tm restored = readNoSlotClock(core);
    assert(std::mktime(&restored) == std::mktime(&atSave) && "a restore reads the snapshot's date");

    // A machine started at another date (the next session) takes the
    // state's: the date is the machine's, like its RAM.
    pom2::Core other = makeIIe();
    assert(other.attachNoSlotClock(start + 3600));
    assert(other.loadState(state.data(), state.size()));
    std::tm adopted = readNoSlotClock(other);
    assert(std::mktime(&adopted) == std::mktime(&atSave) && "the restored machine reads the state's date");
    pom2::Core none = makeIIe();
    assert(!none.loadState(state.data(), state.size()) && "a clock is part of the configuration");
    std::printf("  ok: No-Slot Clock in emulated time\n");
}

// setAccelerator + runVideo: over the same beam time a guest counting in a
// loop gets about four times as far with a Zip Chip, and a snapshot carries
// the chip; without one runVideo is run.
long countLoop(pom2::Core& core, int videoCycles)
{
    // $0300: INC $00 / BNE / INC $01 / JMP $0300 — a 16-bit counter.
    const std::uint8_t code[] = {0xE6, 0x00, 0xD0, 0x02, 0xE6, 0x01, 0x4C, 0x00, 0x03};
    const std::uint8_t zero[2] = {0, 0};
    assert(core.writeRam(pom2::RamBank::Main, 0x0300, code, sizeof(code)));
    assert(core.writeRam(pom2::RamBank::Main, 0x0000, zero, 2));
    pom2::CpuState st = core.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;
    core.setCpuState(st);
    const int ran = core.runVideo(videoCycles);
    assert(ran >= videoCycles && ran < videoCycles + 64);
    std::uint8_t counter[2];
    assert(core.readRam(pom2::RamBank::Main, 0x0000, counter, 2));
    return counter[0] | (counter[1] << 8);
}

void testAccelerator()
{
    pom2::Core stock = makeIIe();
    assert(stock.accelerator() == pom2::Accelerator::None && stock.cpuSpeedMultiplier() == 1.0);
    const long base = countLoop(stock, 17030 * 3);

    pom2::Core fast = makeIIe();
    assert(fast.setAccelerator(pom2::Accelerator::ZipChip));
    assert(fast.cpuSpeedMultiplier() > 3.5);
    const long quick = countLoop(fast, 17030 * 3);
    const double ratio = static_cast<double>(quick) / static_cast<double>(base);
    assert(ratio > 3.5 && ratio < 4.5 && "a Zip Chip runs the guest four times as fast");

    std::vector<std::uint8_t> state;
    assert(fast.saveState(state));
    assert(fast.loadState(state.data(), state.size()));
    assert(countLoop(fast, 17030 * 3) == quick && "a restored Zip Chip keeps its speed");
    assert(!stock.loadState(state.data(), state.size()) && "the accelerator is part of the configuration");

    pom2::CoreConfig iic;
    iic.machine = pom2::Machine::AppleIIc;
    pom2::Core plainIIc(iic);
    assert(!plainIIc.setAccelerator(pom2::Accelerator::ZipChip));
    assert(fast.setAccelerator(pom2::Accelerator::None) && fast.cpuSpeedMultiplier() == 1.0);
    std::printf("  ok: accelerator, %.2fx\n", ratio);
}

// A Mockingboard Sound II whose SSI263 is speaking: saveState is allowed,
// and the restored machine replays the same samples, speech included.
void testSpeechSnapshot()
{
    pom2::Core core = makeIIe();
    assert(core.attachMockingboard(4, pom2::MockingboardModel::SoundII));
    assert(core.supportsState());
    core.coldBoot();
    core.run(1022727 / 2);   // the ROM's reset handler sets the slot switches
    // ssi263_speech_timeline's sequence at $C440+reg: powered down, filter,
    // a voiced phoneme, power up; the chip then repeats the phoneme.
    const std::uint8_t code[] = {
        0x8D, 0x06, 0xC0,               // SETSLOTCXROM: $C4xx is slot 4's
        0xA9, 0xFF, 0x8D, 0x02, 0xC4,   // VIA 1 DDRB = $FF
        0xA9, 0x04, 0x8D, 0x00, 0xC4,   // ORB = $04: the AY's /RESET released
        0xA9, 0x8F, 0x8D, 0x43, 0xC4,   // CTTRAMP = $8F (CTL high)
        0xA9, 0x80, 0x8D, 0x44, 0xC4,   // FILFREQ = $80
        0xA9, 0x0A, 0x8D, 0x40, 0xC4,   // DURPHON = $0A
        0xA9, 0x0F, 0x8D, 0x43, 0xC4,   // CTTRAMP = $0F (CTL low)
        0x4C, 0x21, 0x03,               // JMP *
    };
    assert(core.writeRam(pom2::RamBank::Main, 0x0300, code, sizeof(code)));
    pom2::CpuState st = core.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;
    core.setCpuState(st);
    std::vector<float> chunk(2 * 735);
    const auto play = [&](int frames) {
        std::vector<float> audio;
        for (int i = 0; i < frames; ++i) {
            core.run(17030);
            assert(core.pullAudio(chunk.data(), 735) == 735);
            audio.insert(audio.end(), chunk.begin(), chunk.end());
        }
        return audio;
    };
    play(30);
    std::vector<std::uint8_t> state;
    assert(core.saveState(state));
    const auto first = play(60);
    double energy = 0;
    for (float sample : first) energy += std::abs(sample);
    assert(energy > 1.0 && "the SSI263 speaks");
    assert(core.loadState(state.data(), state.size()));
    const auto second = play(60);
    assert(first == second && "a restored Sound II replays the same speech");
    std::printf("  ok: Sound II speech in snapshots\n");
}

// A //c Plus booting its internal 3.5" drive: saveState is allowed and the
// restored machine replays the same frames, its drives' media included.
void testIIcPlusSnapshot()
{
    // Apple II DeskTop's 800 KiB disk, from the test media archive.
    const std::string disk = gSource + "/disks_3.5/A2DeskTop-1.5-en_800k.2mg";
    if (!std::ifstream(disk) || !std::ifstream(gSource + "/roms/apple2cp.rom")) {
        std::printf("  skip: //c Plus snapshot (test media not fetched)\n");
        return;
    }
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIcPlus;
    pom2::Core core(config);
    assert(core.loadRom(gSource + "/roms/apple2cp.rom"));
    assert(core.loadCharacterRom(gSource + "/roms/apple2e_char.rom"));
    assert(core.insertSmartPortDisk(0, disk));
    assert(core.supportsState());
    assert(core.bootSlot(5));
    const auto frames = [&](int count) {
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < count; ++i) {
            core.run(17030);
            const auto view = core.renderFrame();
            std::uint64_t h = 1469598103934665603ull;
            for (int p = 0; p < view.width * view.height; ++p) h = (h ^ view.pixels[p]) * 1099511628211ull;
            hashes.push_back(h);
        }
        return hashes;
    };
    const auto ram = [&] {
        std::vector<std::uint8_t> bytes(0xC000);
        assert(core.readRam(pom2::RamBank::Main, 0, bytes.data(), bytes.size()));
        return bytes;
    };
    frames(120);   // ProDOS is loading from the internal drive
    std::vector<std::uint8_t> state;
    assert(core.saveState(state));
    const auto atSave = ram();
    const auto first = frames(240);
    const auto firstRam = ram();
    assert(core.loadState(state.data(), state.size()));
    const auto second = frames(240);
    assert(first == second && ram() == firstRam && "a restored //c Plus replays the same boot");
    assert(firstRam != atSave && "the guest went on loading after the save");
    std::printf("  ok: //c Plus snapshot, %zu KiB\n", state.size() >> 10);
}

// setRamWorksBanks: the guest's bank select at $C073 reaches the extra banks
// and a snapshot carries them; a ][+ has no auxiliary slot.
void testRamWorks()
{
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIeEnhanced;
    pom2::Core core(config);
    assert(core.setRamWorksBanks(4) && core.ramWorksBanks() == 4);
    assert(core.loadRom(gSource + "/roms/apple2e.rom"));
    const auto aux2000 = [&] { return ram(core, pom2::RamBank::Aux, 0x2000); };
    // Bank 2: RAMWRT on, $5A at $2000, RAMWRT off, back to bank 0.
    runGuest(core, {0xA9, 0x02, 0x8D, 0x73, 0xC0, 0x8D, 0x05, 0xC0, 0xA9, 0x5A, 0x8D, 0x00, 0x20,
                    0x8D, 0x04, 0xC0, 0xA9, 0x00, 0x8D, 0x73, 0xC0});
    assert(aux2000() != 0x5A && "bank 0 kept its byte");
    std::vector<std::uint8_t> state;
    assert(core.saveState(state));
    assert(state.size() > 4u * 64u * 1024u && "the state holds every bank");
    runGuest(core, {0xA9, 0x02, 0x8D, 0x73, 0xC0});
    assert(aux2000() == 0x5A && "the guest's write is in bank 2");
    put(core, pom2::RamBank::Aux, 0x2000, 0x00);   // spoil bank 2
    assert(core.loadState(state.data(), state.size()));
    assert(aux2000() != 0x5A && "the state had bank 0 selected");
    runGuest(core, {0xA9, 0x02, 0x8D, 0x73, 0xC0});
    assert(aux2000() == 0x5A && "the state restored bank 2");

    // The card's largest size, 8 MiB, round-trips too.
    pom2::Core big(config);
    assert(big.setRamWorksBanks(128) && big.loadRom(gSource + "/roms/apple2e.rom"));
    std::vector<std::uint8_t> bigState;
    assert(big.saveState(bigState) && big.loadState(bigState.data(), bigState.size()));

    pom2::CoreConfig plus;
    plus.machine = pom2::Machine::AppleIIPlus;
    pom2::Core iiPlus(plus);
    assert(!iiPlus.setRamWorksBanks(4));
    std::printf("  ok: RamWorks III banks in snapshots\n");
}

// attachPhasor: in its Mockingboard mode the guest's AY tone reaches the mix,
// a //c refuses it, and a snapshot replays the guest-visible machine exactly.
void testPhasor()
{
    pom2::Core core = makeIIe();
    assert(core.loadCharacterRom(gSource + "/roms/apple2e_char.rom"));
    assert(core.attachPhasor(4) && core.phasorAttached() && !core.attachPhasor(4));
    assert(core.supportsState());
    std::vector<std::uint8_t> code = {0x8D, 0x06, 0xC0,               // SETSLOTCXROM
                                      0xA9, 0xFF, 0x8D, 0x03, 0xC4,   // DDRA = $FF
                                      0xA9, 0x07, 0x8D, 0x02, 0xC4,   // DDRB = $07
                                      0xA9, 0x00, 0x8D, 0x00, 0xC4,   // /RESET low
                                      0xA9, 0x04, 0x8D, 0x00, 0xC4};  // inactive
    // AY register writes: latch the address (ORB 7), then the data (ORB 6).
    for (const auto [reg, value] : {std::pair<std::uint8_t, std::uint8_t>{0, 0xFF}, {1, 0x01},
                                    {7, 0x3E}, {8, 0x0F}}) {
        for (const std::uint8_t b : {std::uint8_t(0xA9), reg, std::uint8_t(0x8D), std::uint8_t(0x01), std::uint8_t(0xC4),
                                     std::uint8_t(0xA9), std::uint8_t(0x07), std::uint8_t(0x8D), std::uint8_t(0x00), std::uint8_t(0xC4),
                                     std::uint8_t(0xA9), std::uint8_t(0x04), std::uint8_t(0x8D), std::uint8_t(0x00), std::uint8_t(0xC4),
                                     std::uint8_t(0xA9), value, std::uint8_t(0x8D), std::uint8_t(0x01), std::uint8_t(0xC4),
                                     std::uint8_t(0xA9), std::uint8_t(0x06), std::uint8_t(0x8D), std::uint8_t(0x00), std::uint8_t(0xC4),
                                     std::uint8_t(0xA9), std::uint8_t(0x04), std::uint8_t(0x8D), std::uint8_t(0x00), std::uint8_t(0xC4)})
            code.push_back(b);
    }
    std::uint16_t at = 0x0300;
    for (const std::uint8_t b : code) put(core, pom2::RamBank::Main, at++, b);
    put(core, pom2::RamBank::Main, at, 0x4C);
    put(core, pom2::RamBank::Main, static_cast<std::uint16_t>(at + 1), static_cast<std::uint8_t>(at));
    put(core, pom2::RamBank::Main, static_cast<std::uint16_t>(at + 2), static_cast<std::uint8_t>(at >> 8));
    pom2::CpuState st = core.cpuState();
    st.programCounter = 0x0300;
    st.status = 0x24;
    core.setCpuState(st);
    std::vector<float> chunk(2 * 735);
    double energy = 0;
    for (int frame = 0; frame < 30; ++frame) {
        core.run(17030);
        assert(core.pullAudio(chunk.data(), 735) == 735);
        for (float sample : chunk) energy += std::abs(sample);
    }
    assert(energy > 1.0 && "the Phasor's AY plays the guest's tone");

    const auto timeline = [&] {
        std::vector<std::uint64_t> hashes;
        for (int frame = 0; frame < 60; ++frame) {
            core.run(17030);
            core.pullAudio(chunk.data(), 735);
            std::vector<std::uint8_t> bytes(0xC000);
            assert(core.readRam(pom2::RamBank::Main, 0, bytes.data(), bytes.size()));
            std::uint64_t h = 1469598103934665603ull;
            for (auto b : bytes) h = (h ^ b) * 1099511628211ull;
            hashes.push_back(h ^ core.cpuState().cycles);
        }
        return hashes;
    };
    std::vector<std::uint8_t> state;
    assert(core.saveState(state));
    const auto first = timeline();
    assert(core.loadState(state.data(), state.size()));
    assert(timeline() == first && "a restored Phasor replays the machine");
    pom2::Core none = makeIIe();
    assert(!none.loadState(state.data(), state.size()) && "the Phasor is part of the configuration");

    pom2::CoreConfig iic;
    iic.machine = pom2::Machine::AppleIIc;
    pom2::Core plainIIc(iic);
    assert(!plainIIc.attachPhasor(4));
    std::printf("  ok: Phasor\n");
}

int main()
{
    gSource = POM2_TEST_SOURCE_DIR;
    testBanksAreIndependentOfTheSwitches();
    testNothingElseMoves();
    testRangesAreRefusedWhole();
    testKeyboardState();
    testIdentity();
    testRamRegions();
    testDisplayModes();
    testDriveSounds();
    testNoSlotClock();
    testAccelerator();
    testSpeechSnapshot();
    testIIcPlusSnapshot();
    testRamWorks();
    testPhasor();
    return 0;
}
