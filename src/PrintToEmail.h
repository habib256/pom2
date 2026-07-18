// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrintToEmail — "print with e-mail" for the printer spool.
//
// Composes an RFC 6068 `mailto:` URL from the printer spool text and
// hands it to the host's default mail client (`xdg-open` on Linux,
// `open` on macOS, `ShellExecute` on Windows, the browser itself on the
// WASM build). No SMTP, no credentials, no network code in POM2 — the
// user's own mail client does the sending, exactly like the print-to-PDF
// workflow relies on the host's file system.
//
// The URL composer (`buildMailtoUrl`) is a pure function so the escaping
// rules are unit-testable without spawning a mail client; pinned by
// `printer_email_smoke`.
//
// mailto body limits
// ------------------
// mailto: URLs travel through argv / the URL bar, so mail clients cap
// them (practical floor ≈ 8-32 KB depending on client). The composer
// truncates the body at `bodyCap` raw characters and appends a marker
// telling the user to "Save as .txt" for the full printout; the caller
// gets a `truncated` flag to surface that in the UI.

#ifndef POM2_PRINT_TO_EMAIL_H
#define POM2_PRINT_TO_EMAIL_H

#include <cstddef>
#include <string>
#include <string_view>

namespace pom2 {
namespace printmail {

/// Raw-body cap (characters before percent-encoding). Chosen well under
/// the ~32 KB Windows ShellExecute URL limit even at worst-case 3×
/// percent-encoding expansion.
constexpr size_t kDefaultBodyCap = 8000;

/// RFC 3986 percent-encoding. Unreserved characters (ALPHA / DIGIT /
/// "-" / "." / "_" / "~") pass through; every other byte becomes %XX
/// (uppercase hex). `extraAllowed` lists additional bytes to pass
/// through verbatim (e.g. "@" for the addr-spec part of a mailto).
std::string percentEncode(std::string_view s, std::string_view extraAllowed = "");

/// Minimal sanity check for the UI: exactly one '@' with non-empty
/// local + domain parts, no whitespace / control / quote characters.
/// Deliberately lenient — real validation belongs to the mail client.
bool looksLikeEmail(std::string_view addr);

struct Mailto {
    std::string url;                ///< complete mailto: URL, fully encoded
    bool        truncated = false;  ///< body was cut at bodyCap
};

/// Compose `mailto:<to>?subject=<subject>&body=<body>`. The body's LF
/// line endings are normalised to CRLF first (RFC 6068 §5 requires
/// %0D%0A line breaks), then everything is percent-encoded.
Mailto buildMailtoUrl(std::string_view to,
                      std::string_view subject,
                      std::string_view body,
                      size_t bodyCap = kDefaultBodyCap);

/// Hand the URL to the host's default mail client. Returns false and
/// fills *err (if non-null) when the launcher itself fails; a launcher
/// that succeeds but has no mail client configured is the host's
/// problem to report. Safe to call from the UI thread — the launchers
/// dispatch and return without waiting for the mail client.
bool openMailClient(const std::string& mailtoUrl, std::string* err = nullptr);

} // namespace printmail
} // namespace pom2

#endif // POM2_PRINT_TO_EMAIL_H
