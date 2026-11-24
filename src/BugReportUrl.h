// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BugReportUrl — the "Report a bug" link of the Help menu: a new GitHub
// issue already holding what a report needs and what users leave out, the
// version, the EDITION (the browser demo and the desktop packages are not
// the same build, and reports mixed them), the platform and the machine.

#include <cstdio>
#include <string>
#include <string_view>

namespace pom2 {

/// The edition a report comes from: "web demo (browser)" or the desktop
/// platform the build targets.
inline const char* bugReportEdition()
{
#if defined(__EMSCRIPTEN__)
    return "web demo (browser)";
#elif defined(_WIN32)
    return "desktop, Windows";
#elif defined(__APPLE__)
    return "desktop, macOS";
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
    return "desktop, Linux ARM";
#elif defined(__linux__)
    return "desktop, Linux";
#else
    return "desktop";
#endif
}

/// RFC 3986 percent-encoding of a query value: unreserved bytes pass.
inline std::string urlEncode(std::string_view text)
{
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

/// https://github.com/habib256/pom2/issues/new with its body filled in.
inline std::string bugReportUrl(std::string_view version, std::string_view edition,
                                std::string_view machine, std::string_view display)
{
    std::string body;
    body += "**What happened**\n\n\n**What you expected**\n\n\n**Steps to reproduce**\n1. \n\n";
    body += "**Disk or program** (name, and where it comes from)\n\n\n";
    body += "---\n";
    body += "- POM2 " + std::string(version) + "\n";
    body += "- Edition: " + std::string(edition) + "\n";
    body += "- Machine: " + std::string(machine) + "\n";
    body += "- Display: " + std::string(display) + "\n";
    return "https://github.com/habib256/pom2/issues/new?body=" + urlEncode(body);
}

} // namespace pom2
