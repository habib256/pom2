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

// CardVideoSource — a slot card that generates its OWN picture and, under a
// guest-visible condition, takes the monitor over from the Apple's video.
//
// The Videx Videoterm is the case (VidexVideotermCard.h): an 80-column card
// with its own CRTC, VRAM and character generator, whose output goes to the
// monitor through a "soft video switch" instead of the motherboard's video.
// Apple2Display holds a non-owning pointer to one of these and, for a frame
// where `ownsScreen()` holds, presents the card's picture instead of painting
// the Apple's.
//
// An abstract seam rather than the concrete card on purpose: Apple2Display.cpp
// is linked by ~50 test and tool targets that list their sources by hand, and
// a direct call into the card would add the card (and its CRTC) to every one
// of those link lines. Through this header the display depends on nothing it
// has to link.

#ifndef POM2_CARD_VIDEO_SOURCE_H
#define POM2_CARD_VIDEO_SOURCE_H

#include <cstdint>

namespace pom2 {

class CardVideoSource
{
public:
    virtual ~CardVideoSource() = default;

    /// Does the card's picture replace the Apple's for a frame whose
    /// published soft switches are `textMode` (TEXT, $C050/$C051) and `an0`
    /// (annunciator 0, $C058/$C059)?
    virtual bool ownsScreen(bool textMode, bool an0) const = 0;

    /// The picture's fixed geometry, in output pixels.
    virtual int pictureWidth()  const = 0;
    virtual int pictureHeight() const = 0;

    /// Paint the whole picture into `dst` (pictureWidth() × pictureHeight(),
    /// row-major, 0xAABBGGRR like every Apple2Display buffer). `lit` is the
    /// monitor's phosphor colour for a lit dot, `dark` for an unlit one.
    /// `emuCycles` / `cpuHz` place the frame in EMULATED time, for anything
    /// that blinks. Called under stateMutex, like the rest of render().
    virtual void paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                              uint64_t emuCycles, double cpuHz) const = 0;
};

} // namespace pom2

#endif // POM2_CARD_VIDEO_SOURCE_H
