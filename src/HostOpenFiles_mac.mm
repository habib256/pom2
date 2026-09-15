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

// The macOS half of HostOpenFiles.h — see that header for why this exists.
//
// GLFW installs its own NSApplicationDelegate (`GLFWApplicationDelegate`,
// cocoa_init.m) and answers five notifications through it, none of them
// `application:openFiles:`. Replacing the delegate would take those five
// away from GLFW, so the method is ADDED to GLFW's class instead, at runtime,
// which is what the Objective-C runtime's class_addMethod is for. AppKit
// looks the selector up on the delegate at delivery time, so a method added
// after `[NSApp setDelegate:]` is found like any compiled one.

#include "HostOpenFiles.h"

#include "Logger.h"

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include <string>

namespace {

void openFilesImp(id /*self*/, SEL /*_cmd*/, NSApplication* app,
                  NSArray<NSString*>* files)
{
    for (NSString* f in files)
        pom2::pushHostOpenFile(std::string([f fileSystemRepresentation]));
    // Without the reply AppKit reports "could not be opened" for a launch
    // that in fact succeeded.
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

} // namespace

namespace pom2 {

void installHostOpenFilesHandler()
{
    // Idempotent: main() calls this BEFORE glfwInit and again after it.
    //
    // Before, because glfwInit runs `[NSApp run]` until the delegate's
    // applicationDidFinishLaunching stops it — and AppKit delivers the
    // Apple Events a launch carries (the double-clicked file, the file
    // dropped on the Dock icon of a POM2 that was not running) INSIDE that
    // run, between willFinishLaunching and didFinishLaunching. A method
    // added to the delegate afterwards catches only the opens of a POM2
    // already running. The class itself is registered with the runtime the
    // moment GLFW is loaded, so it can be found by name with no instance.
    //
    // After, as the fallback for a GLFW that renamed the class: the live
    // delegate's class is then the target, and only later opens arrive.
    static bool installed = false;
    if (installed) return;
    Class cls = objc_getClass("GLFWApplicationDelegate");
    if (!cls) {
        id delegate = NSApp ? [NSApp delegate] : nil;
        if (!delegate) return;   // before glfwInit, with no class by name
        cls = object_getClass(delegate);
        log().warn("Host", "GLFWApplicationDelegate not found by name; "
                           "a file opened AT launch may be lost");
    }
    const SEL sel = @selector(application:openFiles:);
    if (class_respondsToSelector(cls, sel)) {
        // A GLFW that grew its own handler would swallow the event; say so
        // rather than silently losing every Finder open.
        log().warn("Host", "the application delegate already answers "
                           "application:openFiles: — Finder opens are its");
        return;
    }
    // "v@:@@": returns void, takes self, _cmd, NSApplication*, NSArray*.
    if (!class_addMethod(cls, sel, reinterpret_cast<IMP>(openFilesImp), "v@:@@")) {
        log().warn("Host", "could not install application:openFiles:");
        return;
    }
    installed = true;
    log().info("Host", "Finder opens and Dock drops route to the disk "
                       "boot path");
}

} // namespace pom2
