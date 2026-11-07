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

// File ▸ Save BASIC listing: the BASIC program in memory, as text, in
// `<user data>/listings/` and on the clipboard — the program's source,
// exactly, the way sbabcoc's ReplicApple2Plus captures it through PR#n.
// Read straight from RAM, so it works whatever the program has done to the
// screen or the output hooks.
//
// Both BASICs, since 2026-11-01. POM2 ships an `Apple ][ Original (1977)`
// profile whose BASIC *is* Integer BASIC, and this menu item had nothing to
// offer it:
//
//   * **Applesoft** — TXTTAB `$67/$68` up to VARTAB `$69/$6A`, stored
//     UPWARD from the bottom.
//   * **Integer BASIC** — PP `$CA/$CB` up to HIMEM `$4C/$4D`, stored
//     DOWNWARD from the top (Woz, "Apple II Basic Structure").
//
// Applesoft is tried first, so nothing about the existing behaviour moves.
// Both pointer pairs are ordinary zero-page bytes that hold something else
// on a machine running the other BASIC, so the Integer attempt is gated on
// `looksLikeProgram`, which requires the chain of line lengths to land
// exactly on HIMEM. Reading the screen is not an option POM2 needs: the
// capture-the-LIST-output route the transfer card takes has to keep the
// cursor still, because LIST breaks its lines on the cursor column.

#include "MainWindow.h"
#include "ApplesoftListing.h"
#include "BrowserFiles.h"
#include "IntegerBasicListing.h"
#include "EmulationController.h"
#include "Logger.h"
#include "ResourcePaths.h"

#include "imgui.h"

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

void MainWindow::saveBasicListing()
{
    std::string text;
    const char* which = "";
    pom2::applesoft::ListReport report;
    {
        auto st = controller->lockState();
        Memory& mem = st.memory();
        const auto peek = [&](uint16_t a) { return mem.peekMainRam(a); };

        const uint16_t txttab = uint16_t(peek(0x67) | peek(0x68) << 8);
        const uint16_t vartab = uint16_t(peek(0x69) | peek(0x6A) << 8);
        if (vartab > txttab && txttab >= 0x0200) {
            text = pom2::applesoft::list(txttab, peek,
                                         std::size_t(vartab - txttab), &report);
            if (!text.empty()) which = "Applesoft";
        }
        if (text.empty()) {
            // Integer BASIC: [PP, HIMEM). The program sits high in memory,
            // so a PP below the text page is not one.
            const uint16_t pp    = uint16_t(peek(0xCA) | peek(0xCB) << 8);
            const uint16_t himem = uint16_t(peek(0x4C) | peek(0x4D) << 8);
            if (himem > pp && pp >= 0x0800 &&
                pom2::integerbasic::looksLikeProgram(
                    pp, peek, std::size_t(himem - pp))) {
                text = pom2::integerbasic::list(pp, peek,
                                                std::size_t(himem - pp));
                if (!text.empty()) which = "Integer BASIC";
            }
        }
    }
    if (text.empty()) {
        basicListingStatus_ = "No BASIC program in memory.";
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
#ifdef __EMSCRIPTEN__
    // The browser: MEMFS loses the file on reload and emscripten's GLFW
    // clipboard is a no-op, so "saved to <path> (and copied)" claimed two
    // things that did not happen (WASM-4). Download it instead.
    {
        std::string derr;
        basicListingStatus_ = pom2::downloadBrowserFile(file.string(), file.filename().string(), derr)
            ? std::string("Downloaded ") + which + " listing as " + file.filename().string()
            : std::string("Could not download the listing: ") + derr;
    }
#else
    ImGui::SetClipboardText(text.c_str());
    basicListingStatus_ = std::string("Saved ") + which + " listing to " +
                          file.string() + " (and copied)";
#endif
    // Bytes text cannot hold (ApplesoftListing.h): say so, or the file
    // passes for a copy of the program.
    if (report.untypable > 0)
        basicListingStatus_ += " - " + std::to_string(report.untypable) +
            " byte(s) cannot be typed back, first in line " +
            std::to_string(report.firstLine);
    pom2::log().info("Listing", basicListingStatus_);
}
