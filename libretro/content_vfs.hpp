// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include <libretro.h>

namespace pom2::libretro {

// Content the core cannot open as a file. RetroArch on Android hands a disk
// picked through the Storage Access Framework as `saf://<tree>/<path>`
// (libretro-common vfs_implementation.c @ v1.22.2), which only its VFS
// opens; the SDK and the disk loaders read real files. Such content is
// copied once through the VFS into a cache under the save directory and the
// core goes on with the copy. The original is never written: guest writes go
// to the private copy, as for any disk. An M3U brings the disks it lists.

/// Ask the frontend for its VFS (version 1: open, size, read, close).
void initContentVfs(retro_environment_t environment);

/// True when `path` names a file the C++ library can open: no `scheme://`.
bool isLocalPath(const std::string& path);

/// `path` itself when local; else its copy under `cacheRoot`, made through
/// the VFS (an empty string and `error` set when that fails).
std::string localizeContent(const std::string& path, const std::filesystem::path& cacheRoot,
                            std::string& error);

} // namespace pom2::libretro
