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

// File ▸ Save BASIC listing: the Applesoft program in memory, as text, in
// `<user data>/listings/` and on the clipboard — the program's source,
// exactly, the way sbabcoc's ReplicApple2Plus captures it through PR#n.
// Read straight from RAM (TXTTAB $67/$68 up to VARTAB $69/$6A), so it works
// whatever the program has done to the screen or the output hooks.

#include "MainWindow.h"
#include "ApplesoftListing.h"
#include "EmulationController.h"
#include "Logger.h"
#include "ResourcePaths.h"

#include "imgui.h"

#include <ctime>
#include <filesystem>
#include <fstream>

void MainWindow::saveBasicListing()
{
    std::string text;
    {
        auto st = controller->lockState();
        Memory& mem = st.memory();
        const uint16_t txttab = uint16_t(mem.peekMainRam(0x67) | mem.peekMainRam(0x68) << 8);
        const uint16_t vartab = uint16_t(mem.peekMainRam(0x69) | mem.peekMainRam(0x6A) << 8);
        if (vartab > txttab && txttab >= 0x0200)
            text = pom2::applesoft::list(txttab, [&](uint16_t a) { return mem.peekMainRam(a); },
                                         std::size_t(vartab - txttab));
    }
    if (text.empty()) {
        basicListingStatus_ = "No Applesoft program in memory.";
        return;
    }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::filesystem::path dir = pom2::userDataDir() / "listings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (std::string("listing-") + stamp + ".txt");
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << text;
    if (!f) {
        basicListingStatus_ = "Could not write " + file.string();
        return;
    }
    ImGui::SetClipboardText(text.c_str());
    basicListingStatus_ = "Saved " + file.string() + " (and copied)";
    pom2::log().info("Listing", basicListingStatus_);
}
