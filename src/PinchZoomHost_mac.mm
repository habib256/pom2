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

// The macOS half of PinchZoomHost.h — see that header for the other hosts.
//
// A trackpad pinch reaches the app as NSEventTypeMagnify, which GLFW's
// content view (cocoa_window.m, 3.4) does not implement, so the default
// NSResponder passes it up the chain and it is lost. A LOCAL event monitor
// sees it first, on the main thread, before it is dispatched: the monitor
// queues it and swallows it. Only the two magnify types are watched, so a
// two-finger scroll (NSEventTypeScrollWheel) never passes through here and
// reaches GLFW exactly as before.

#include "PinchZoomHost.h"

#include "PinchZoom.h"

#import <Cocoa/Cocoa.h>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace {

id gMonitor = nil;

// The event's point in GLFW's cursor coordinates: window points, origin top
// left — the conversion cocoa_window.m's mouseMoved: applies.
pom2::pinch::Point glfwPoint(NSEvent* event, NSWindow* window)
{
    const NSRect content = [[window contentView] frame];
    const NSPoint p = [event locationInWindow];
    return pom2::pinch::Point{static_cast<float>(p.x),
                              static_cast<float>(content.size.height - p.y)};
}

} // namespace

namespace pom2 {

void installPinchZoomSources(GLFWwindow* window)
{
    if (gMonitor || !window) return;
    NSWindow* ours = glfwGetCocoaWindow(window);
    if (!ours) return;
    const NSEventMask mask = NSEventMaskMagnify | NSEventMaskSmartMagnify;
    gMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                    handler:^NSEvent*(NSEvent* event) {
        // A native file dialog or any other window keeps its own pinch.
        if ([event window] != ours) return event;
        if ([event type] == NSEventTypeSmartMagnify) {
            pinch::router().pushFit();
            return nil;
        }
        pinch::router().pushAt(pinch::magnificationToLog([event magnification]),
                               glfwPoint(event, ours));
        return nil;
    }];
#if !__has_feature(objc_arc)
    // Kept for the life of the process; under manual reference counting the
    // monitor must outlive the autorelease pool that returned it.
    [gMonitor retain];
#endif
}

} // namespace pom2
