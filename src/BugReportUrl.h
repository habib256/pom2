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
#include <cstddef>
#include <vector>
#include <utility>
#include <cstring>
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

/// One "key: value" fact about the session, gathered by the application.
using BugReportFact = std::pair<std::string, std::string>;

/// The issue's body. The four questions only a person can answer come first;
/// under a rule, everything POM2 can say by itself about the machine at the
/// moment of the report, so the first reply to an issue is no longer "which
/// machine, which cards, which disk, at what PC?". `maxLength` (0 = no limit)
/// keeps the body short enough to travel in a URL: the facts are listed most
/// useful first, and the ones that do not fit are dropped with a note.
inline std::string bugReportBody(std::string_view version, std::string_view edition,
                                 std::string_view machine, std::string_view display,
                                 const std::vector<BugReportFact>& facts = {},
                                 std::size_t maxLength = 0)
{
    std::string body;
    body += "**What happened**\n\n\n**What you expected**\n\n\n**Steps to reproduce**\n1. \n\n";
    body += "**Disk or program** (name, and where it comes from)\n\n\n";
    body += "**Does it happen every time?** \n\n";
    body += "---\n";
    body += "*Filled in by POM2 at the moment of the report. Remove what you do not want to share.*\n";
    body += "- POM2 " + std::string(version) + "\n";
    body += "- Edition: " + std::string(edition) + "\n";
    body += "- Machine: " + std::string(machine) + "\n";
    body += "- Display: " + std::string(display) + "\n";
    const char* const cut = "- (more in the report copied to the clipboard: paste it here)\n";
    for (const BugReportFact& fact : facts) {
        const std::string line = "- " + fact.first + ": " + fact.second + "\n";
        if (maxLength && body.size() + line.size() + std::strlen(cut) > maxLength) {
            body += cut;
            break;
        }
        body += line;
    }
    return body;
}

/// https://github.com/habib256/pom2/issues/new with its body filled in.
/// GitHub refuses a request line much beyond 8 KB and percent-encoding
/// triples most punctuation, so the body is held to 2000 characters; the
/// caller puts the full report on the clipboard.
inline std::string bugReportUrl(std::string_view version, std::string_view edition,
                                std::string_view machine, std::string_view display,
                                const std::vector<BugReportFact>& facts = {})
{
    return "https://github.com/habib256/pom2/issues/new?body=" +
           urlEncode(bugReportBody(version, edition, machine, display, facts, 2000));
}

} // namespace pom2
