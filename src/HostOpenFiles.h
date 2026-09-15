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

// HostOpenFiles — files the HOST asks POM2 to open after launch.
//
// A disk image dropped on the window reaches `MainWindow::onFileDrop` through
// GLFW's drop callback. A disk image dropped on the POM2 icon in the Dock or
// in Finder, or double-clicked (the bundle declares .dsk/.po/.2mg/.hdv/… in
// its Info.plist), does NOT: macOS delivers it as an `odoc` Apple Event to
// the application delegate's `application:openFiles:`, and GLFW's delegate
// (cocoa_init.m, 3.4) implements no such method — so POM2 launched, or came
// to the front, and the file went nowhere. The bundle advertised what the
// process could not receive.
//
// `installHostOpenFilesHandler` adds that method to GLFW's own delegate at
// runtime (objc class_addMethod — no delegate replacement, GLFW keeps every
// callback it registered). The paths land in a queue that main() drains once
// per frame into `onFileDrop`, on the UI thread, exactly like a window drop.
// Elsewhere than macOS the install is a no-op and the queue stays empty: on
// Linux and Windows a file opened with POM2 arrives in argv, which the CLI's
// positional-disk path already handles.

#ifndef POM2_HOST_OPEN_FILES_H
#define POM2_HOST_OPEN_FILES_H

#include <string>
#include <vector>

namespace pom2 {

/// Hook the host's "open these files" notification. Call once after
/// `glfwInit()` — the application delegate the hook attaches to exists only
/// from then — and before the first `glfwPollEvents()`, or a file opened at
/// launch is delivered before anyone listens. No-op except on macOS.
void installHostOpenFilesHandler();

/// Queue a path as if the host had asked for it. Thread-safe. The macOS hook
/// calls it; a test may too.
void pushHostOpenFile(std::string path);

/// Take everything queued since the last call, oldest first. Thread-safe.
std::vector<std::string> takeHostOpenFiles();

} // namespace pom2

#endif // POM2_HOST_OPEN_FILES_H
