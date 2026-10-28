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

// Static-text frame-skip equivalence test.
//
// Apple2Display skips repainting a full-screen text frame when the video RAM,
// the DisplayState, the FLASH phase, the colour mode and the character ROM are
// all byte-identical to the frame already in the framebuffer (see TextFrameKey
// in Apple2Display.h). This pins the ONE property that makes that legal:
//
//     the framebuffer must be bit-identical to what a full repaint produces,
//     every frame, whatever the guest does.
//
// Rather than assert specific pixels, it runs TWO machines side by side over
// the same scripted sequence — one display allowed to skip, one forced to
// repaint every frame via invalidateTextFrameCache() — and compares the whole
// framebuffer after each frame. Any divergence, anywhere, fails.
//
// Two SEPARATE Memory instances, not one shared by two displays: render()
// consumes the video-event queue (`mem.takeVideoEvents()`), so a second
// display rendering the same Memory would see an empty queue and never take
// the beam-racing path at all — the test would pass while testing nothing.
// The script is deterministic, so driving two machines with identical calls
// keeps them in lockstep.
//
// The script deliberately walks the cases the skip must NOT swallow:
//   * plain static text                 (the case being optimised)
//   * a single byte changing            (must repaint)
//   * the FLASH phase flipping          (must repaint, and it is time-driven,
//                                        not RAM-driven, so it is the easiest
//                                        one to get wrong)
//   * page 1 <-> page 2 flips
//   * 80-column and ALTCHAR toggles
//   * TEXT -> graphics -> MIXED -> TEXT
//   * BEAM-RACED frames (mid-frame soft-switch writes), which must never skip
//   * host-side COLOUR-MODE changes, which no guest write can signal — only
//     the key's hiResModeId sees them, and the Le Chat Mauve card is installed
//     because its colour-TEXT path is the one text renderer whose pixels
//     actually depend on the colour mode (every other mode draws text
//     hard-coded white-on-black)
//   * the card's OWN $C0B8-$C0BB "Eve" registers, which are guest writes that
//     change a full-screen text frame yet touch neither DisplayState nor the
//     video-event log (section 9) — the case that caught a real regression
//
// A second, separate script (section 10) covers the other side of the same
// contract: a caller that rewrites the framebuffer AFTER render() returned.
// demodCompositeForCapture() — the AI control server's `GET /screen` — is the
// only production one, and the display must come back to its own image on the
// next frame instead of skipping onto the capture's pixels. Section 11 then
// checks that the skip is still actually TAKEN: every assertion above is an
// equivalence, and a display that repainted unconditionally would satisfy all
// of them while quietly throwing away the optimisation.
//
// Mutation-tested 2026-07-31: deleting the flash-phase, video-RAM,
// DisplayState, colour-mode, Chat-Mauve-state or beam-raced-frame terms from
// the key each makes this test fail. Two terms survive deletion and are
// therefore DEFENSIVE, not load-bearing, today: the `mixedMode` exclusion
// (renderInternalBand's `if (state.textMode)` short-circuits before any mixed
// handling, in both the 40- and 80-column paths, so MIXED cannot alter a
// full-text frame) and the `iie` flag (a machine only changes IIe-ness across
// a profile switch, which rebuilds the display). The character-ROM term is
// untestable here — the harness loads no ROM, and ROMs are user-provided so no
// test may require one.
//
// NOTE on section 9, because it is the trap this file exists to remember: the
// card must be PLUGGED INTO THE SlotBus, not merely handed to the display via
// setChatMauveCard(). Its registers arrive through
// SlotBus::broadcastVideoSwitch, so with an unplugged card the guest writes go
// nowhere and the section passes while testing nothing — which is exactly what
// it did on the first attempt.
//
// and it runs the whole script under BOTH video standards, because the FLASH
// phase derives from the emulated frame index — cycleCounter / (65 *
// scanlinesPerFrame) — so PAL's 312-line frame advances it at a different rate
// than NTSC's 262-line one. A skip keyed on a frame counter that ignored the
// standard would drift on PAL only.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

struct Machine {
    Memory           mem;
    Apple2Display    disp;
    LeChatMauveCard* chat = nullptr;   // owned by the SlotBus

    explicit Machine(VideoStandard vs)
    {
        mem.setIIEMode(true);
        mem.setVideoStandard(vs);
        disp.setAuxMemory(mem.auxData());
        // The card must be PLUGGED, not merely handed to the display: its
        // $C0B8-$C0BB "Eve" registers arrive via SlotBus::broadcastVideoSwitch,
        // so a card the bus does not know about never sees a guest write and
        // section 9 would silently test nothing. Slot 7 mirrors the //c PAL
        // profile's built-in; slot 3 must stay EMPTY or Memory's collision
        // guard (chatMauveBlockedBySlot3) suppresses the whole decode.
        // An EVE: the only variant with registers at $C0B0-$C0BF.
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        chat = card.get();
        mem.slotBus().plug(7, std::move(card));
        disp.setChatMauveCard(chat);
    }
};

// What the UI and the screenshot paths actually consume: the active buffer for
// this frame (frame or frame80), with any deferred demod completed. Geometry is
// appended so an 80-column mismatch also shows up as a difference.
std::vector<uint32_t> grab(Apple2Display& d)
{
    const uint32_t* p = d.pixels();
    const size_t    n = static_cast<size_t>(d.width()) * static_cast<size_t>(d.height());
    std::vector<uint32_t> out(p, p + n);
    out.push_back(static_cast<uint32_t>(d.width()));
    out.push_back(static_cast<uint32_t>(d.height()));
    return out;
}

void compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
             const char* what, int frame, const char* standard)
{
    if (a == b) return;
    size_t       first = 0, differing = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) { if (!differing) first = i; ++differing; }
    std::printf("FAIL [%s] %s @frame %d: %zu/%zu words differ (sizes %zu/%zu), "
                "first at %zu (skip=%08X full=%08X)\n",
                standard, what, frame, differing, n, a.size(), b.size(), first,
                first < n ? a[first] : 0u, first < n ? b[first] : 0u);
    ++failures;
}

void runScript(VideoStandard vs, const char* standard)
{
    Machine skip(vs);   // allowed to take the fast path
    Machine full(vs);   // forced to repaint every frame

    Machine* const both[2] = { &skip, &full };

    auto poke = [&](uint16_t addr, uint8_t v) {
        for (Machine* m : both) m->mem.writeRamUnchecked(addr, v);
    };
    auto sw = [&](uint16_t addr) {
        for (Machine* m : both) m->mem.memWrite(addr, 0);
    };
    auto adv = [&](int cycles) {
        for (Machine* m : both) m->mem.advanceCycles(cycles);
    };

    const auto& vt = pom2VideoTiming(vs);
    const int frameCycles = 65 * vt.scanlinesPerFrame;

    // Seed a text screen with plenty of FLASH-range bytes ($40-$7F), so a
    // mishandled flash phase cannot hide in an all-normal screen.
    for (uint16_t a = 0x0400; a < 0x0C00; ++a)
        poke(a, static_cast<uint8_t>((a & 0x40) ? (0x40 + (a & 0x3F))
                                                : (0xA0 + (a & 0x3F))));

    int f = 0;
    auto frame = [&](const char* what) {
        full.disp.invalidateTextFrameCache();     // reference: always repaints
        skip.disp.render(skip.mem);
        full.disp.render(full.mem);
        compare(grab(skip.disp), grab(full.disp), what, f, standard);
        adv(frameCycles);
        ++f;
    };

    // 1. Static text — the case the skip exists for. Well past one FLASH
    //    half-period so the phase flips underneath the cache several times.
    for (int i = 0; i < 40; ++i) frame("static text");

    // 2. One byte changes, in the middle of a screen the cache is holding.
    poke(0x0400 + 17, 0xC1);
    for (int i = 0; i < 3; ++i) frame("one byte changed");

    // 3. Page flip, then back.
    sw(0xC055); for (int i = 0; i < 3; ++i) frame("page2 on");
    sw(0xC054); for (int i = 0; i < 3; ++i) frame("page2 off");

    // 4. 80-column and ALTCHAR — these change both the geometry and the glyph
    //    source, and 80-column also pulls half the cells from aux RAM.
    for (uint16_t a = 0x0400; a < 0x0800; ++a)
        for (Machine* m : both) m->mem.auxDataMutable()[a] = static_cast<uint8_t>(0xC1 + (a & 0x1F));
    sw(0xC00D); for (int i = 0; i < 3; ++i) frame("80col on");
    sw(0xC00F); for (int i = 0; i < 3; ++i) frame("altchar on");
    sw(0xC00E); for (int i = 0; i < 3; ++i) frame("altchar off");
    sw(0xC00C); for (int i = 0; i < 3; ++i) frame("80col off");

    // 5. TEXT -> graphics -> MIXED -> TEXT. Only the text frames may skip;
    //    graphics writes phosphor persistence and must always repaint.
    for (uint16_t a = 0x2000; a < 0x4000; ++a)
        poke(a, static_cast<uint8_t>(a * 7));
    sw(0xC050);                                        // CLRTEXT -> graphics
    sw(0xC057);                                        // SETHIRES
    for (int i = 0; i < 4; ++i) frame("hires graphics");
    sw(0xC053); for (int i = 0; i < 4; ++i) frame("mixed");
    sw(0xC052);                                        // CLRMIXED
    sw(0xC051); for (int i = 0; i < 4; ++i) frame("back to text");   // SETTEXT

    // 6. BEAM RACING: soft-switch writes DURING the frame produce video events,
    //    so the frame is painted as bands with different DisplayStates. These
    //    must never take the fast path, and the band boundaries are computed
    //    from the standard's scanline count — the PAL run is what would catch a
    //    262-hardcoded mapping.
    int beamFrames = 0;
    for (int i = 0; i < 6; ++i) {
        const int third = frameCycles / 3;
        adv(third);
        sw(0xC050);                 // CLRTEXT -> graphics, mid-frame
        adv(third);
        sw(0xC051);                 // SETTEXT -> text, mid-frame
        adv(frameCycles - 2 * third);  // cross the frame boundary: the events
                                       // are only PUBLISHED to the display at
                                       // the boundary, so rendering before this
                                       // would silently see an empty log and
                                       // test nothing.
        // takeVideoEvents() is a non-destructive copy in published mode (it
        // only moves under the legacy synchronous bracket, which this test
        // never opens), so peeking here does not rob the displays.
        if (!skip.mem.takeVideoEvents().empty()) ++beamFrames;
        full.disp.invalidateTextFrameCache();
        skip.disp.render(skip.mem);
        full.disp.render(full.mem);
        compare(grab(skip.disp), grab(full.disp), "beam-raced split", f, standard);
        ++f;
    }
    if (beamFrames == 0) {
        std::printf("FAIL [%s] beam-racing section produced NO video events — "
                    "the split path was never exercised\n", standard);
        ++failures;
    }

    // 7. Back to a quiet static screen — the skip must re-engage cleanly after
    //    all of the above rather than latch onto a stale key.
    for (int i = 0; i < 10; ++i) frame("static text again");

    // 8. Colour-mode changes on an otherwise frozen text screen. This is a
    //    HOST-side change — no guest write, no soft switch, nothing in video
    //    RAM moves — so it is invisible to every other term of the key. Only
    //    hiResModeId catches it. All of these render text through the
    //    renderInternal path (the CPU demods deliberately keep full-screen
    //    text crisp), so the framebuffer is a pure function of the mode and
    //    the two displays stay comparable.
    static const Apple2Display::HiResMode kModes[] = {
        Apple2Display::HiResMode::ColorCompMedium,
        Apple2Display::HiResMode::ChatMauveRGB,
        Apple2Display::HiResMode::MonoGreen,
        Apple2Display::HiResMode::ColorComp4Bit,
        Apple2Display::HiResMode::MonoAmber,
        Apple2Display::HiResMode::ColorAppleWin,
        Apple2Display::HiResMode::MonoWhite,
        Apple2Display::HiResMode::ColorNTSC,
    };
    sw(0xC05E);   // AN3 -> DHGR on: arms the card's colour-TEXT path
    for (auto m : kModes) {
        for (Machine* mm : both) mm->disp.setHiResMode(m);
        for (int i = 0; i < 3; ++i) frame("colour-mode change");
    }
    sw(0xC05F);

    // 9. The Le Chat Mauve Eve's switches, $C0B0-$C0BF. These are GUEST
    //    writes that change what a full-screen TEXT frame looks like —
    //    $C0B9 (TXT16) turns colour TEXT on, which switches the renderer to
    //    renderTextChatMauveFgBg AND the output buffer to the 560-wide
    //    frame80; $C0BB (TXTGREEN) tints the text — yet they do NOT touch
    //    Memory::DisplayState and, unlike
    //    $C05E/$C05F, they push NO video event (they reach the card through
    //    broadcastVideoSwitch, not the display soft-switch decode). So the
    //    frame after such a write has an EMPTY event log and an unchanged
    //    DisplayState: nothing the rest of the key looks at has moved, and a
    //    key that ignores the card's own state happily serves a stale screen.
    //    This is the //c PAL profile's built-in slot-7 card — the French
    //    Touch / DIX target hardware — so it is not a hypothetical corner.
    for (Machine* mm : both) mm->disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    for (int i = 0; i < 2; ++i) frame("chat mauve armed");
    sw(0xC0B8); for (int i = 0; i < 3; ++i) frame("eve TXT16 OFF");
    sw(0xC0B9); for (int i = 0; i < 3; ++i) frame("eve TXT16 ON");
    sw(0xC0B8); for (int i = 0; i < 3; ++i) frame("eve TXT16 OFF again");
    sw(0xC0BB); for (int i = 0; i < 3; ++i) frame("eve TXTGREEN ON");
    sw(0xC0BA); for (int i = 0; i < 3; ++i) frame("eve TXTGREEN OFF");
    for (Machine* mm : both) mm->disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);

    std::printf("[%s] %d frames compared\n", standard, f);
}

// A quiet full-screen 40-column TEXT machine — the state the skip is built for.
void seedStaticText(Memory& mem)
{
    mem.memRead(0xC051);                       // SETTEXT
    for (uint16_t a = 0x0400; a < 0x0C00; ++a)
        mem.writeRamUnchecked(a, static_cast<uint8_t>(0xA0 + (a & 0x3F)));
}

// 10. THE SCREEN-CAPTURE PATH. `GET /screen` (AiControlServer::handleScreen)
//     runs render(), then demodCompositeForCapture() so the PPM shows the
//     composite image the GPU shader puts on screen rather than the LUT
//     fallback framebuffer. That second call rewrites all 192 rows of frame80
//     and flips the published buffer to 560 wide — behind render()'s back, on
//     a frame the skip key still describes. With the key left standing, every
//     later render() skipped, useFrame80 was never recomputed, and re-checking
//     "Sharp text" presented the capture's soft demodulated text instead of the
//     crisp framebuffer until the guest happened to touch a byte of the screen.
//
//     So: after a capture, the very next render() must republish the display's
//     OWN image — same pixels, same geometry, for either setting of the
//     sharp-text knob (with it on, the capture is a documented no-op and the
//     skip may legitimately stay engaged; with it off, the capture mutates and
//     the next frame must repaint).
void runCapturePath(bool textSharp)
{
    const char* const what = textSharp ? "capture, sharp text ON"
                                       : "capture, sharp text OFF";
    Memory mem;
    seedStaticText(mem);

    Apple2Display d;
    d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    Apple2Display::OeDemodParams p;
    p.textSharp = textSharp;
    d.setOeDemodParams(p);

    d.render(mem);
    const std::vector<uint32_t> onScreen = grab(d);   // crisp 280-wide text
    d.render(mem);                                    // steady state
    compare(grab(d), onScreen, "steady state before the capture", 0, what);
    if (!d.signalProduced()) {
        std::printf("FAIL [%s] no composite signal — demodCompositeForCapture() "
                    "is a no-op and this section tests nothing\n", what);
        ++failures;
        return;
    }

    d.demodCompositeForCapture();
    const std::vector<uint32_t> captured = grab(d);
    if (textSharp) {
        // Sharp text bypasses the demod: the framebuffer already IS the capture.
        compare(captured, onScreen, "the capture must not mutate anything", 1, what);
    } else if (captured == onScreen) {
        std::printf("FAIL [%s] the capture returned the untouched framebuffer — "
                    "the demod never ran\n", what);
        ++failures;
    }

    d.render(mem);
    compare(grab(d), onScreen, "display image after a capture", 2, what);
}

// 11. The skip must still be TAKEN. Compares a display left to its own devices
//     against one forced to repaint (invalidateTextFrameCache) over the same
//     motionless text screen. The real ratio is ~600x — a full repaint decodes
//     960 character cells at ~887 host instructions each, a skipped frame is a
//     ~16 KB memcmp — so a threshold of 3x has three orders of magnitude of
//     headroom and only fires if the skip stopped happening at all. Best-of-3
//     so a scheduler hiccup inside the (sub-millisecond) fast loop cannot
//     manufacture a failure.
void runSkipStillEngages()
{
    Memory mem;
    seedStaticText(mem);

    Apple2Display skip, full;
    skip.render(mem);
    full.render(mem);

    constexpr int kFrames = 400;
    double best = 1e30, bestFull = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) skip.render(mem);
        auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) {
            full.invalidateTextFrameCache();
            full.render(mem);
        }
        auto t2 = std::chrono::steady_clock::now();
        best     = std::min(best,
                       std::chrono::duration<double, std::milli>(t1 - t0).count());
        bestFull = std::min(bestFull,
                       std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    const double ratio = bestFull / std::max(best, 1e-6);
    std::printf("[skip] %d static text frames: skipping %.2f ms, "
                "forced repaint %.2f ms (%.0fx)\n",
                kFrames, best, bestFull, ratio);
    if (ratio < 3.0) {
        std::printf("FAIL [skip] the static-text fast path is no longer taken "
                    "(expected a large speed-up, got %.1fx)\n", ratio);
        ++failures;
    }
}

}  // namespace

// The ][ and ][+ flash from a 555 at 2 Hz in real time, the //e from the
// frame count (MAME apple2video.cpp:1170-1178). On PAL the two part at once:
// a quarter second is 12.5 frames, so the ][+ text first changes at frame
// 13, the //e's at frame 16.
int firstFlashFrame(bool iie)
{
    Memory mem;
    mem.setIIEMode(iie);
    mem.setVideoStandard(VideoStandard::PAL);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    for (uint16_t a = 0x0400; a < 0x0800; ++a)
        mem.writeRamUnchecked(a, static_cast<uint8_t>(0x40 + (a & 0x1F)));
    const int frameCycles = 65 * pom2VideoTiming(VideoStandard::PAL).scanlinesPerFrame;
    disp.render(mem);
    const auto first = grab(disp);
    for (int f = 1; f < 40; ++f) {
        mem.advanceCycles(frameCycles);
        disp.render(mem);
        if (grab(disp) != first) return f;
    }
    return -1;
}

void runFlashTimers()
{
    const int plus = firstFlashFrame(false), iie = firstFlashFrame(true);
    if (plus != 13 || iie != 16) {
        std::printf("FAIL flash timers on PAL: ][+ first flips at frame %d "
                    "(want 13, the 555), //e at %d (want 16)\n", plus, iie);
        ++failures;
    }
}

int main()
{
    std::puts("=== display static-text frame-skip equivalence ===");
    runFlashTimers();
    runScript(VideoStandard::NTSC, "NTSC");
    runScript(VideoStandard::PAL,  "PAL");
    runCapturePath(false);
    runCapturePath(true);
    runSkipStillEngages();

    if (failures) {
        std::printf("display_dirty_skip_test: FAIL (%d)\n", failures);
        return 1;
    }
    std::puts("display_dirty_skip_test: OK (skip is bit-identical to full repaint)");
    return 0;
}
