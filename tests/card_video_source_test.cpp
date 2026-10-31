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

// card_video_source — pins the two decisions CardVideoSource.h makes for the
// cards with a picture of their own (Videx Videoterm, UltraTerm, VidHD,
// E-Z Color), header-only and with fakes (bug hunt 2026-10-31):
//
//  1. Which card's picture the Apple II Screen gets. The display holds one
//     source; every plug site used to overwrite it, so with two such cards
//     the higher slot won. A ][+ with a Videoterm in slot 3 and an E-Z Color
//     in slot 4 lost its 80 columns entirely: PR#3 turned TEXT + AN0 on and
//     the screen stayed on 40 columns, the E-Z Color never taking the screen
//     by its own rule. A card that can take the main screen now wins it over
//     one that cannot (`takesMonitor`), and the Card Monitor panel prefers
//     the other way round — the card's own second monitor.
//  2. When the Card Monitor tab offers itself. It compared card POINTERS, and
//     every Slot Config apply or profile switch rebuilds every card, so a tab
//     the user had closed came back each time.

#include "CardVideoSource.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

struct Fake : pom2::CardVideoSource {
    bool takes;
    explicit Fake(bool t) : takes(t) {}
    bool ownsScreen(bool, bool) const override { return takes; }
    int  pictureWidth()  const override { return 1; }
    int  pictureHeight() const override { return 1; }
    void paintPicture(uint32_t*, uint32_t, uint32_t, uint64_t, double) const override {}
    bool takesMonitor() const override { return takes; }
};

} // namespace

int main()
{
    Fake videoterm(true), ultraterm(true), ezcolor(false);
    using pom2::preferScreenSource;
    using pom2::preferPanelSource;

    // 1. The Apple II Screen: whatever the slot order.
    CHECK(preferScreenSource(nullptr, &ezcolor) == &ezcolor && "alone, the E-Z Color");
    CHECK(preferScreenSource(nullptr, &videoterm) == &videoterm);
    CHECK(preferScreenSource(&videoterm, &ezcolor) == &videoterm && "Videoterm sl3, E-Z sl4");
    CHECK(preferScreenSource(&ezcolor, &videoterm) == &videoterm && "E-Z sl2, Videoterm sl3");
    CHECK(preferScreenSource(&videoterm, &ultraterm) == &ultraterm && "two takers: the later, as before");
    CHECK(preferScreenSource(&videoterm, nullptr) == &videoterm);
    //    …and the Card Monitor panel: the card's own monitor.
    CHECK(preferPanelSource(&videoterm, &ezcolor) == &ezcolor && "the two-monitor set-up");
    CHECK(preferPanelSource(&ezcolor, &videoterm) == &ezcolor);
    CHECK(preferPanelSource(nullptr, &videoterm) == &videoterm && "alone, whatever it is");
    CHECK(preferPanelSource(&videoterm, &ultraterm) == &ultraterm);

    // 2. The tab: offered when a card appears, not when it is rebuilt.
    using pom2::shouldOfferCardMonitor;
    CHECK(shouldOfferCardMonitor("", "3:videoterm") && "plugged");
    CHECK(!shouldOfferCardMonitor("3:videoterm", "3:videoterm") && "a rebuild, same card");
    CHECK(shouldOfferCardMonitor("3:videoterm", "4:ezcolor") && "another card");
    CHECK(!shouldOfferCardMonitor("3:videoterm", "") && "unplugged");

    if (g_fail) {
        std::fprintf(stderr, "card_video_source: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("card_video_source: PASS\n");
    return 0;
}
