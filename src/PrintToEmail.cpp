// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrintToEmail — see header for the mailto composition rules.

#include "PrintToEmail.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#endif

namespace pom2 {
namespace printmail {

namespace {

bool isUnreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

} // namespace

std::string percentEncode(std::string_view s, std::string_view extraAllowed)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (isUnreserved(c) || extraAllowed.find(ch) != std::string_view::npos) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

bool looksLikeEmail(std::string_view addr)
{
    const size_t at = addr.find('@');
    if (at == 0 || at == std::string_view::npos || at == addr.size() - 1)
        return false;
    if (addr.find('@', at + 1) != std::string_view::npos)
        return false;
    for (char ch : addr) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F)  return false;   // space / control
        if (c == '"' || c == '\'' || c == '<' || c == '>' ||
            c == ',' || c == ';' || c == '(' || c == ')')
            return false;
    }
    return true;
}

Mailto buildMailtoUrl(std::string_view to,
                      std::string_view subject,
                      std::string_view body,
                      size_t bodyCap)
{
    Mailto m;

    // Truncate on the raw text (before CRLF expansion / encoding) so the
    // cap is easy to reason about; the marker tells the user where the
    // full printout lives.
    std::string bodyText(body);
    if (bodyText.size() > bodyCap) {
        bodyText.resize(bodyCap);
        // Marker must hold on every build — the WASM panel has no
        // "Save as .txt", but the full spool always stays in the panel.
        bodyText += "\n[spool truncated - the full printout remains in "
                    "the POM2 printer panel]\n";
        m.truncated = true;
    }

    // RFC 6068 line breaks are %0D%0A — normalise LF (the spoolText()
    // convention) to CRLF before encoding. Stray CRs are left alone
    // (spoolText never emits them).
    std::string crlf;
    crlf.reserve(bodyText.size());
    for (char ch : bodyText) {
        if (ch == '\n') crlf += "\r\n";
        else            crlf.push_back(ch);
    }

    // '@' stays verbatim in the addr-spec ("+" tags and dots are already
    // unreserved or listed); subject/body get the strict encoding.
    m.url  = "mailto:" + percentEncode(to, "@+");
    m.url += "?subject=" + percentEncode(subject);
    m.url += "&body=" + percentEncode(crlf);
    return m;
}

bool openMailClient(const std::string& mailtoUrl, std::string* err)
{
#ifdef __EMSCRIPTEN__
    // The browser is the launcher: navigating to a mailto: URL opens the
    // user's configured mail handler without leaving the emulator page.
    EM_ASM({ window.location.href = UTF8ToString($0); }, mailtoUrl.c_str());
    return true;
#elif defined(_WIN32)
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteA(nullptr, "open", mailtoUrl.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL));
    if (r <= 32) {
        if (err) *err = "ShellExecute failed (code " + std::to_string(r) + ")";
        return false;
    }
    return true;
#else
#ifdef __APPLE__
    const char* launcher = "open";
#else
    const char* launcher = "xdg-open";
#endif
    // The URL is fully percent-encoded, so it contains no quote or shell
    // metacharacters — single-quoting is belt and braces. Detached with
    // '&' so xdg-open's open_generic fallback (which waits for the
    // handler to exit) can never block the UI thread; the shell then
    // returns as soon as the launcher is spawned.
    const std::string cmd = std::string(launcher) + " '" + mailtoUrl +
                            "' >/dev/null 2>&1 &";
    const int rc = std::system(cmd.c_str());
    if (rc == -1) {
        if (err) *err = "could not spawn a shell to run " +
                        std::string(launcher);
        return false;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
        // With '&' the shell itself exits 0 unless the command line was
        // unrunnable; decode properly rather than echoing waitpid bits.
        if (err) *err = std::string(launcher) + " failed to start (shell "
                        "exit " + std::to_string(WEXITSTATUS(rc)) + ")";
        return false;
    }
    return true;
#endif
}

} // namespace printmail
} // namespace pom2
