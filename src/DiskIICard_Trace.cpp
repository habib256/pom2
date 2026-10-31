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

// DiskIICard — the head trace ("DISK" lines of the OS-call trace, written
// through CardTraceSink while it is hung on the card). See DiskIICard.h.

#include "DiskIICard.h"

#include <algorithm>
#include <string>

// Every soft-switch access goes through here; untraced, straight on.
void DiskIICard::handleSwitchAccess(uint8_t low4)
{
    if (!cardTrace()) { switchAccess(low4); return; }
    const int  drive = activeDrive;
    const int  head = headQuarterTrack[drive];
    const bool motorWas = spinning();
    const bool writeWas = writeMode;
    switchAccess(low4);
    traceSwitch(low4, drive, head, motorWas, writeWas);
}

namespace {
std::string quarterTrack(int q)
{
    static const char* const frac[4] = { "", ".25", ".5", ".75" };
    return std::to_string(q / 4) + frac[q & 3];
}
} // namespace

void DiskIICard::flushRecal()
{
    if (!recalBounces_) return;
    const int n = recalBounces_;
    recalBounces_ = 0;
    if (pom2::CardTraceSink* t = cardTrace()) {
        const std::string l = "DISK S" + std::to_string(busSlot()) + ",D" +
            std::to_string(activeDrive + 1) + " recalibrate against track 0 (" +
            std::to_string(n) + " bounces)";
        t->onCardLine(l.c_str());
    }
}

void DiskIICard::traceLine(const std::string& line)
{
    flushRecal();
    if (pom2::CardTraceSink* t = cardTrace()) t->onCardLine(line.c_str());
}

void DiskIICard::flushSeekRun()
{
    if (!seekRun_.open) return;
    seekRun_.open = false;
    const int lo = std::min(seekRun_.from, seekRun_.to), hi = std::max(seekRun_.from, seekRun_.to);
    if (lo == 0 && hi <= 2) { ++recalBounces_; return; }   // a bounce at the stop
    traceLine("DISK S" + std::to_string(busSlot()) + ",D" +
              std::to_string(seekRun_.drive + 1) + " seek " + quarterTrack(seekRun_.from) +
              " -> " + quarterTrack(seekRun_.to));
}

void DiskIICard::traceSwitch(uint8_t low4, int drive, int head, bool motorWas, bool writeWas)
{
    const std::string where = "DISK S" + std::to_string(busSlot()) + ",D";
    const int now = headQuarterTrack[drive];
    if (now != head) {
        const int dir = now > head ? 1 : -1;
        if (seekRun_.open && seekRun_.drive == drive && seekRun_.dir == dir) {
            seekRun_.to = now;
        } else {
            flushSeekRun();
            seekRun_ = SeekRun{ true, drive, head, now, dir };
        }
        return;
    }
    if (low4 == 0xC) { flushSeekRun(); flushRecal(); return; }   // the data latch: the seek is over
    const bool motorNow = spinning();
    if (motorNow && !motorWas) {
        flushSeekRun();
        traceLine(where + std::to_string(activeDrive + 1) + " motor on, track " +
                  quarterTrack(headQuarterTrack[activeDrive]));
    }
    if (activeDrive != drive) {
        flushSeekRun();
        traceLine(where + std::to_string(activeDrive + 1) + " selected");
    }
    if (writeMode && !writeWas && motorNow) {
        flushSeekRun();
        traceLine(where + std::to_string(activeDrive + 1) + " write, track " +
                  quarterTrack(headQuarterTrack[activeDrive]));
    }
}

