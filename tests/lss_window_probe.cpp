// Black-box timing of the read latch: how long each nibble keeps bit 7 set.
#include "DiskIICard.h"
#include <cstdio>
#include <map>
int main(int argc, char** argv) {
    DiskIICard c(6);
    c.setWriteBackEnabled(false);
    if (argc > 2) c.loadLssRom(argv[2]);
    if (!c.insertDisk(0, argv[1])) { std::puts("insert failed"); return 1; }
    c.deviceSelectRead(0x9);                 // motor on
    c.deviceSelectRead(0xE);                 // Q7L
    for (int i = 0; i < 200000; ++i) c.advanceCycles(1);
    std::map<int,int> runs; int run = 0, nibbles = 0, gapMax = 0, gap = 0;
    uint64_t hash = 14695981039346656037ULL;   // FNV-1a over (cycle, value)
    uint8_t last = 0;
    for (int i = 0; i < 400000; ++i) {
        c.advanceCycles(1);
        const uint8_t v = c.deviceSelectRead(0xC);
        if (v & 0x80) {
            if (!(last & 0x80)) {
                ++nibbles; if (gap > gapMax) gapMax = gap;
                for (uint64_t x : { uint64_t(i), uint64_t(v) }) { hash ^= x; hash *= 1099511628211ULL; }
            }
            ++run; gap = 0;
        }
        else { if (run) ++runs[run]; run = 0; ++gap; }
        last = v;
    }
    std::printf("nibbles %d, longest gap %d cycles, stream hash %016llx\n"
                "bit7 run length (CPU cycles) : count\n", nibbles, gapMax,
                static_cast<unsigned long long>(hash));
    for (auto& [k, v] : runs) if (v > 50) std::printf("  %3d : %d\n", k, v);
}
