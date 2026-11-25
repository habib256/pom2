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
    // The SDK reads a narrow path twice: the C runtime (fopen, ifstream)
    // takes it in the ANSI code page, and std::filesystem::path(std::string)
    // too under MSVC — but as UTF-8 under MinGW's libstdc++, which throws on
    // ANSI bytes. So under MinGW only a path both readings agree on will do:
    // plain ASCII, or anything when the process code page is UTF-8 (bug hunt
    // 2026-11-25).
    const auto usable = [](const std::string& narrow) {
#if defined(__GLIBCXX__)
        if (GetACP() == CP_UTF8) return true;
        for (const unsigned char c : narrow)
            if (c >= 0x80) return false;
#else
        (void)narrow;
#endif
        return true;
    };
    std::string narrow;
    if (toAnsi(path.native(), narrow) && usable(narrow)) return narrow;
    // The 8.3 short name of the FOLDER, the file's own name kept when it is
    // ASCII (every private copy's is): a short name for the file itself
    // ("ENIGME~1.DSK") would be what the SDK renames its saves onto.
    const auto shortOf = [](const std::wstring& wide, std::wstring& out) {
        const DWORD length = GetShortPathNameW(wide.c_str(), nullptr, 0);
        if (length == 0) return false;
        out.assign(length, L'\0');
        const DWORD written = GetShortPathNameW(wide.c_str(), out.data(), length);
        if (written == 0 || written >= length) return false;
        out.resize(written);
        return true;
    };
    std::wstring shortened;
    const auto leaf = path.filename().native();
    std::string leafNarrow;
    if (toAnsi(leaf, leafNarrow) && usable(leafNarrow) && shortOf(path.parent_path().native(), shortened)) {
        shortened += L"\\";
        shortened += leaf;
        if (toAnsi(shortened, narrow) && usable(narrow)) return narrow;
    }
    if (shortOf(path.native(), shortened) && toAnsi(shortened, narrow) && usable(narrow)) return narrow;
    return {};
#else
    return path.u8string();
#endif
}

} // namespace pom2::libretro
