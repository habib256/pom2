// POM2 — SPDX-License-Identifier: GPL-3.0-or-later
#include "TextScreenCapture.h"
#include "Apple2Display.h"
#include "Memory.h"
#include <algorithm>
#include <array>
#include <cassert>

struct Watch : pom2::MemoryWatchSink {
    int reads = 0;
    void noteAccess(uint16_t, uint8_t, bool write) override { if (!write) ++reads; }
};

int main()
{
    using namespace pom2;
    assert(textScreenChar(0x01, false) == 'A'); // inverse
    assert(textScreenChar(0x41, false) == 'A'); // flashing
    assert(textScreenChar(0x81, false) == 'A'); // high-bit alias
    assert(textScreenChar(0xC1, false) == 'A'); // normal
    assert(textScreenChar(0xE1, false) == 'a');
    assert(textScreenChar(0x61, true) == 'a');  // inverse lowercase
    assert(textScreenChar(0x41, true) == '?'); // MouseText
    assert(textScreenChar(0xFF, false) == '?');
    for (int byte = 0; byte < 256; ++byte)
        for (bool alt : {false, true}) {
            const char c = textScreenChar(byte, alt);
            assert(c >= 0x20 && c < 0x7F);
        }

    // Every interleaved row on both pages, including the last column.
    std::array<uint8_t, 4096> ram;
    ram.fill(0xA0);
    TextScreenSource source;
    source.main = [&](uint16_t a) { return ram.at(a); };
    for (bool page2 : {false, true}) {
        source.page2 = page2;
        for (int row = 0; row < 24; ++row) {
            const auto at = Apple2Display::textRowAddress(row, page2);
            ram[at + 39] = 0xC1 + row;
            assert(captureTextScreenRow(source, row) ==
                   std::string(39, ' ') + char('A' + row));
        }
        const auto text = captureTextScreen(source);
        assert(std::count(text.begin(), text.end(), '\n') == 24);
    }
    assert(captureTextScreenRow(source, -1).empty());
    assert(captureTextScreenRow(source, 24).empty());

    Memory m;
    m.setIIEMode(true);
    for (uint16_t a = 0x400; a < 0xC00; ++a) m.memWrite(a, 0xA0);
    std::fill(m.auxDataMutable() + 0x400, m.auxDataMutable() + 0xC00, 0xA0);
    m.memWrite(0x400, 0xC1); m.memWrite(0x800, 0xC2);
    m.auxDataMutable()[0x400] = 0xC3;
    m.auxDataMutable()[0x800] = 0xC4;
    auto captured = captureTextScreen(m);
    assert(captured.text == "A" + std::string(24, '\n'));
    assert(captured.rows == 24 && captured.columns == 40 && captured.page == 1);
    m.memWrite(0xC055, 0); // PAGE2
    assert(captureTextScreen(m).text == "B" + std::string(24, '\n'));
    m.memWrite(0xC00D, 0); // 80COL
    captured = captureTextScreen(m);
    assert(captured.columns == 80 && captured.page == 2);
    assert(captured.text == "DB" + std::string(24, '\n'));
    m.memWrite(0xC001, 0); // 80STORE: scanner uses page 1, not aux-only
    captured = captureTextScreen(m);
    assert(captured.page == 1 && captured.text == "CA" + std::string(24, '\n'));
    m.setRamWorksBanks(4);
    m.memWrite(0xC073, 1);
    m.auxDataMutable()[0x400] = 0xDA;
    assert(captureTextScreen(m).text == captured.text); // video bank stays 0
    m.memWrite(0xC00C, 0); // 40COL always main
    assert(captureTextScreen(m).text == "A" + std::string(24, '\n'));
    m.memWrite(0xC000, 0); m.memWrite(0xC054, 0);
    m.memWrite(Apple2Display::textRowAddress(20, false), 0xD8);
    m.memWrite(Apple2Display::textRowAddress(23, false), 0xD9);
    m.memWrite(0xC050, 0); m.memWrite(0xC053, 0);
    captured = captureTextScreen(m);
    assert(captured.rows == 4 && captured.firstRow == 20);
    assert(captured.text == "X\n\n\nY\n"); // don't collapse interior blanks
    m.memWrite(0xC052, 0);
    assert(captureTextScreen(m).text.empty() && captureTextScreen(m).rows == 0);
    m.memWrite(0xC051, 0);
    Watch watch;
    m.setWatchSink(&watch); m.setReadWatch(0x400, true);
    (void)captureTextScreen(m);
    assert(watch.reads == 0);
    (void)m.memRead(0x400); assert(watch.reads == 1); // watch really armed
    m.setWatchSink(nullptr);
    m.memWrite(0xC00F, 0); m.memWrite(0x400, 0x41);
    assert(captureTextScreen(m).text.front() == '?');
    m.setIIEMode(false);
    assert(captureTextScreen(m).columns == 40);
}
