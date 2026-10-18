// POM2 — GPL-3.0-or-later
#include "Tms5220.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
namespace {
std::vector<uint8_t> phrase() {
    std::vector<uint8_t> bits;
    auto field = [&](unsigned value, int width) {
        for (int i = width - 1; i >= 0; --i) bits.push_back((value >> i) & 1);
    };
    field(10,4); field(0,1); field(20,6);
    for (int width : {5,5,4,4,4,4,4,3,3,3}) field(1u << (width - 1), width);
    for (int i = 0; i < 8; ++i) { field(10,4); field(1,1); field(20,6); }
    field(15,4);
    std::vector<uint8_t> bytes((bits.size() + 7) / 8 + 2, 0);
    for (size_t i = 0; i < bits.size(); ++i) bytes[i / 8] |= bits[i] << (i % 8);
    return bytes;
}
}
int main() {
    pom2::Tms5220 chip;
    assert(chip.status() == 0x60 && chip.ready());
    chip.write(0x60);
    const auto bytes = phrase();
    for (int i = 0; i < 8; ++i) chip.write(bytes[i]);
    assert(chip.status() == 0x40);
    chip.write(bytes[8]); assert((chip.status() & 0x80) != 0);
    size_t fed = 9;
    std::vector<int16_t> samples;
    for (int i = 0; i < 4000; ++i) {
        while (fed < bytes.size() && chip.ready()) chip.write(bytes[fed++]);
        int16_t sample; chip.process(&sample, 1); samples.push_back(sample);
    }
    assert(fed == bytes.size() && !(chip.status() & 0x80));
    assert(std::count_if(samples.begin(), samples.end(), [](int16_t v) { return std::abs(int(v)) > 500; }) > 100);
    assert(std::all_of(samples.end() - 200, samples.end(), [](int16_t v) { return v == -1; }));
    // FIFO full writes are ignored; a $70 in external mode is speech data.
    chip.reset(); chip.write(0x60);
    for (int i = 0; i < 16; ++i) chip.write(0);
    assert(!chip.ready());
    std::vector<uint8_t> full; chip.appendSnapshot(full);
    chip.write(0x70);
    std::vector<uint8_t> after; chip.appendSnapshot(after); assert(after == full);
    // Emptying a FIFO stops speech; BL/BE recover without a host audio device.
    std::array<int16_t, 8000> silence{}; chip.process(silence.data(), unsigned(silence.size()));
    assert(chip.status() == 0x60 && chip.ready());
    chip.write(0x70); assert(chip.status() == 0x60);
    // Exact continuation at an arbitrary point inside an interpolated frame.
    chip.write(0x60);
    for (size_t i = 0; i < 16; ++i) chip.write(bytes[i]);
    chip.process(silence.data(), 471);
    std::vector<uint8_t> saved; chip.appendSnapshot(saved);
    pom2::Tms5220 restored;
    assert(restored.loadSnapshot(saved.data(), saved.size()));
    std::array<int16_t, 600> one{}, split{};
    chip.process(one.data(), unsigned(one.size()));
    for (size_t i = 0; i < split.size(); ++i) restored.process(&split[i], 1);
    assert(one == split && chip.status() == restored.status());
    // Invalid state is refused without mutation.
    saved[16] = 255;
    std::vector<uint8_t> before; restored.appendSnapshot(before);
    assert(!restored.loadSnapshot(saved.data(), saved.size()));
    after.clear(); restored.appendSnapshot(after); assert(before == after);
    std::cout << "TMS5220C: FIFO, voiced synthesis, stop/underflow and exact continuation OK\n";
}
