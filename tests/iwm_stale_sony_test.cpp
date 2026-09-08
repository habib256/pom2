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

// The IWM must not keep writing into a 3.5" drive the hub has deselected.
//
// MAME's `recalc_active_device` ends with an unconditional
// `set_floppy(m_cur_floppy)`, nullptr included. POM2 splits the two form
// factors across `setFloppy` / `setSony35`, and the hub only ever SET a Sony
// on the 3.5" branch — so after the MIG routed to a 5.25" drive (or to
// nothing) `IWMDevice::sony_` still pointed at the last Sony and `flushWrite`
// spliced the next burst into that disk's cell stream (bug hunt #6). The hub
// now calls `releaseSony35()`. Two runs of the same burst: one with the
// internal drive still selected (cells must change — the detector works),
// one after the reroute (no Sony attached, cells byte-identical).

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string makeImage(const char* leaf, uint8_t fill)
{
    const fs::path p = fs::temp_directory_path() / leaf;
    std::vector<uint8_t> b(pom2::Disk35Image::kBytesPerImage, fill);
    b[2 * 512] = 0; b[2 * 512 + 1] = 0; b[2 * 512 + 4] = 0xF3;   // a vol-dir key block
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p.string();
}

// Returns the number of cells of the INTERNAL drive's track that the burst
// changed, and whether the IWM still had a Sony attached when it flushed.
size_t burst(bool reroute, bool& sonyAttachedAtFlush)
{
    pom2::Disk35Image a, b;
    assert(a.loadFile(makeImage("pom2_stale_int.po", 0x11)));
    assert(b.loadFile(makeImage("pom2_stale_ext.po", 0x22)));
    a.setWriteBackEnabled(true);
    b.setWriteBackEnabled(true);
    pom2::Sony35Drive di, de;
    di.setImage(&a);
    de.setImage(&b);
    pom2::IWMDevice iwm;
    pom2::SmartPortHub hub;
    hub.attach(&iwm);
    hub.setSony35(&di, &de);
    uint64_t cyc = 1000;
    auto tick = [&](int n) { cyc += n; iwm.tick(cyc); };

    hub.setMigIntDrive(true);
    iwm.write(0x0B, 0); tick(2);        // SEL   → devsel 2
    iwm.write(0x09, 0); tick(2);        // ENABLE → MODE_ACTIVE, hub picks internal
    assert(hub.active35() == &di);
    di.seekPhaseW(0x02, cyc); di.seekPhaseW(0x0A, cyc); di.seekPhaseW(0x02, cyc);
    tick(200);
    if (reroute) { hub.setMigIntDrive(false); hub.setMig35Sel(true); }
    assert((hub.active35() == nullptr) == reroute);

    const auto before = di.debugCellStream();
    iwm.write(0x0F, 0); tick(4);        // Q7 → MODE_WRITE
    iwm.write(0x0D, 0); tick(4);        // Q6 → control window
    for (int i = 0; i < 400; ++i) { iwm.write(0x0F, static_cast<uint8_t>(0x96 + i)); tick(30); }
    iwm.write(0x0C, 0); tick(4);
    sonyAttachedAtFlush = (iwm.getSony35() != nullptr);
    iwm.write(0x0E, 0); tick(4);        // Q7 low → flushWrite
    const auto after = di.debugCellStream();
    size_t d = 0;
    const size_t n = std::min(before.size(), after.size());
    for (size_t i = 0; i < n; ++i) if (before[i] != after[i]) ++d;
    return d;
}

} // namespace

int main()
{
    bool attached = false;
    const size_t control = burst(/*reroute=*/false, attached);
    assert(attached && control > 0 && "control: a selected drive must take the burst");

    const size_t stale = burst(/*reroute=*/true, attached);
    assert(!attached && "after the hub routes away, no Sony may stay attached to the IWM");
    assert(stale == 0 && "a deselected 3.5\" drive must not receive the burst");

    std::printf("iwm_stale_sony: selected drive changed %zu cells, deselected drive 0 OK\n",
                control);
    return 0;
}
