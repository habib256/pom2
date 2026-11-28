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

// The Windows and browser halves of PinchZoomHost.h (macOS:
// PinchZoomHost_mac.mm). See that header for what each host sends.

#include "PinchZoomHost.h"

#include "PinchZoom.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
// GESTUREINFO and GetGestureInfo exist from Windows 7 (0x0601); winuser.h
// gates them on WINVER.
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#  endif
#  if !defined(WINVER) || WINVER < 0x0601
#    undef WINVER
#    define WINVER 0x0601
#  endif
#  include <windows.h>
#  ifndef GLFW_INCLUDE_NONE
#    define GLFW_INCLUDE_NONE
#  endif
#  include <GLFW/glfw3.h>
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#endif

#ifdef __EMSCRIPTEN__
#  include <emscripten/emscripten.h>

/// Called by wasm/pinch_zoom.js. `ratio` = new scale / old scale (> 1 =
/// fingers apart); (x, y) in the same backbuffer units as GLFW's cursor
/// position, used when `hasFocal` is non-zero (two touches), else the
/// pointer is the focal point (Safari's trackpad gesture). `ratio` <= 0 asks
/// for nothing but is accepted, so the page can probe the export.
extern "C" EMSCRIPTEN_KEEPALIVE void pom2_pinch_zoom(double ratio, double x, double y, int hasFocal)
{
    const float v = pom2::pinch::ratioToLog(ratio);
    if (hasFocal)
        pom2::pinch::router().pushAt(v, pom2::pinch::Point{static_cast<float>(x), static_cast<float>(y)});
    else
        pom2::pinch::router().push(v);
}

/// Called by wasm/pinch_zoom.js when a second finger lands on the canvas:
/// the first one's mouse press must not end as a click (Router::cancelPress).
extern "C" EMSCRIPTEN_KEEPALIVE void pom2_pinch_touch_begin()
{
    pom2::pinch::router().cancelPress();
}
#endif

#if defined(_WIN32)

namespace {

WNDPROC gPrevProc = nullptr;
DWORD   gZoomDistance = 0;   // finger distance at the last GID_ZOOM, 0 = none

LRESULT CALLBACK pinchWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEWHEEL:
        // A precision touchpad's pinch, and a mouse's Ctrl+wheel. MK_CONTROL
        // is read off the message itself: whether the touchpad driver also
        // injects a Ctrl key press (that GLFW would turn into io.KeyCtrl) is
        // not something to depend on. Swallowed, so it never also scrolls.
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                                  static_cast<float>(WHEEL_DELTA);
            pom2::pinch::router().push(pom2::pinch::wheelToLog(notches));
            return 0;
        }
        break;
    case WM_GESTURE: {
        // A touch screen. GLFW registers no touch window, so Windows sends
        // its default gestures; only GID_ZOOM is taken, the rest (pan's
        // legacy scroll, press-and-tap) goes on to DefWindowProc.
        GESTUREINFO gi{};
        gi.cbSize = sizeof(gi);
        const HGESTUREINFO h = reinterpret_cast<HGESTUREINFO>(lp);
        if (!GetGestureInfo(h, &gi) || gi.dwID != GID_ZOOM) break;
        // ullArguments' low 32 bits: the distance between the two fingers.
        const DWORD dist = static_cast<DWORD>(gi.ullArguments & 0xFFFFFFFFull);
        if ((gi.dwFlags & GF_BEGIN) == 0 && gZoomDistance != 0 && dist != 0) {
            POINT pt{gi.ptsLocation.x, gi.ptsLocation.y};   // screen → client
            ScreenToClient(hwnd, &pt);
            pom2::pinch::router().pushAt(
                pom2::pinch::ratioToLog(static_cast<double>(dist) / static_cast<double>(gZoomDistance)),
                pom2::pinch::Point{static_cast<float>(pt.x), static_cast<float>(pt.y)});
        }
        gZoomDistance = (gi.dwFlags & GF_END) ? 0 : dist;
        CloseGestureInfoHandle(h);
        return 0;
    }
    default:
        break;
    }
    return CallWindowProcW(gPrevProc, hwnd, msg, wp, lp);
}

} // namespace

namespace pom2 {

void installPinchZoomSources(GLFWwindow* window)
{
    if (gPrevProc || !window) return;
    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;
    gPrevProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!gPrevProc) return;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pinchWndProc));
    // GID_ZOOM is in Windows' default gesture set, but ask for it anyway: a
    // window's configuration can be changed by whoever registered it, and
    // the request costs nothing. Failure is not fatal — the defaults stand.
    GESTURECONFIG zoom{};
    zoom.dwID    = GID_ZOOM;
    zoom.dwWant  = GC_ZOOM;
    zoom.dwBlock = 0;
    SetGestureConfig(hwnd, 0, 1, &zoom, sizeof(GESTURECONFIG));
}

} // namespace pom2

#elif !defined(__APPLE__) || defined(__EMSCRIPTEN__)

namespace pom2 {
void installPinchZoomSources(GLFWwindow*) {}
} // namespace pom2

#endif
