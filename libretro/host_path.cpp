// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host_path.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pom2::libretro {

#ifdef _WIN32
namespace {

// The wide string in the process's ANSI code page, or false when a character
// has no equivalent there (no best-fit guess: "é" is not "e").
bool toAnsi(const std::wstring& wide, std::string& out)
{
    out.clear();
    if (wide.empty()) return true;
    const UINT page = GetACP();
    // A UTF-8 code page (a manifest's activeCodePage): every path maps, and
    // the flags below are refused for it.
    const bool utf8 = page == CP_UTF8;
    BOOL lossy = FALSE;
    const int size = WideCharToMultiByte(page, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         utf8 ? nullptr : &lossy);
    if (size <= 0 || lossy) return false;
    out.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(page, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()),
                        out.data(), size, nullptr, utf8 ? nullptr : &lossy);
    return !lossy;
}

} // namespace
#endif

std::string sdkPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    std::string narrow;
    if (toAnsi(path.native(), narrow)) return narrow;
    // The 8.3 short name: plain ASCII where the volume generates them (the
    // system drive does by default). The file must exist.
    const DWORD length = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (length > 0) {
        std::wstring shortPath(length, L'\0');
        const DWORD written = GetShortPathNameW(path.c_str(), shortPath.data(), length);
        if (written > 0 && written < length) {
            shortPath.resize(written);
            if (toAnsi(shortPath, narrow)) return narrow;
        }
    }
    return {};
#else
    return path.u8string();
#endif
}

} // namespace pom2::libretro
