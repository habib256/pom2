// PrintToEmail smoke test — pins the "print with e-mail" mailto composer
// the printer panel relies on:
//
//   1. percentEncode — RFC 3986 unreserved set passes through, everything
//      else becomes uppercase %XX (including 8-bit bytes), extraAllowed
//      opt-outs work.
//   2. buildMailtoUrl — URL shape (`mailto:to?subject=...&body=...`),
//      '@' kept verbatim in the addr-spec, LF → %0D%0A line breaks
//      (RFC 6068 §5), and the body-cap truncation marker + flag.
//   3. looksLikeEmail — the lenient UI gate: one '@', non-empty sides,
//      no whitespace / quotes / angle brackets / shell-relevant chars.
//
// The launcher (openMailClient) is deliberately NOT exercised — it
// spawns the host mail client.

#include "PrintToEmail.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace pom2::printmail;

namespace {

void testPercentEncode()
{
    // Unreserved set passes through untouched.
    assert(percentEncode("AZaz09-._~") == "AZaz09-._~");

    // Reserved / special bytes are encoded, uppercase hex.
    assert(percentEncode(" ") == "%20");
    assert(percentEncode("a&b=c?d") == "a%26b%3Dc%3Fd");
    assert(percentEncode("'\"") == "%27%22");
    assert(percentEncode("\r\n") == "%0D%0A");

    // 8-bit bytes (Apple II spool with bit 7 unexpectedly set) survive
    // as %XX, not sign-extended garbage.
    const char high[] = { static_cast<char>(0xE9), 0 };
    assert(percentEncode(high) == "%E9");

    // extraAllowed passes listed bytes through.
    assert(percentEncode("user@host+tag", "@+") == "user@host+tag");
    assert(percentEncode("user@host", "") == "user%40host");

    std::printf("  ok: percentEncode\n");
}

void testBuildMailtoUrl()
{
    const auto m = buildMailtoUrl("gist974@gmail.com", "POM2 printout",
                                  "HELLO\nWORLD\n");
    assert(!m.truncated);
    // '@' verbatim, space in subject encoded, LF normalised to CRLF.
    assert(m.url == "mailto:gist974@gmail.com"
                    "?subject=POM2%20printout"
                    "&body=HELLO%0D%0AWORLD%0D%0A");

    // No shell metacharacters can survive encoding — the launcher
    // single-quotes the URL, so a quote or backtick in the spool must
    // never appear raw.
    const auto evil = buildMailtoUrl("a@b", "s",
                                     "'; rm -rf / #`$(boom)\"");
    for (char c : evil.url) {
        assert(c != '\'' && c != '`' && c != '$' && c != '"' &&
               c != ';' && c != ' ');
    }

    std::printf("  ok: mailto URL shape + encoding\n");
}

void testTruncation()
{
    const std::string body(100, 'A');
    const auto m = buildMailtoUrl("a@b", "s", body, /*bodyCap=*/10);
    assert(m.truncated);
    // 10 kept 'A's, then the CRLF-encoded marker.
    assert(m.url.find("body=AAAAAAAAAA%0D%0A%5Bspool%20truncated") !=
           std::string::npos);

    // At the cap exactly → no truncation.
    const auto m2 = buildMailtoUrl("a@b", "s", std::string(10, 'A'), 10);
    assert(!m2.truncated);

    std::printf("  ok: body-cap truncation\n");
}

void testLooksLikeEmail()
{
    assert( looksLikeEmail("gist974@gmail.com"));
    assert( looksLikeEmail("a@b"));
    assert( looksLikeEmail("user+tag@sub.domain.org"));

    assert(!looksLikeEmail(""));
    assert(!looksLikeEmail("nodomain@"));
    assert(!looksLikeEmail("@nolocal"));
    assert(!looksLikeEmail("no-at-sign"));
    assert(!looksLikeEmail("two@@ats"));
    assert(!looksLikeEmail("a@b c"));            // space
    assert(!looksLikeEmail("a@b\tc"));           // control
    assert(!looksLikeEmail("\"quoted\"@b"));     // quotes
    assert(!looksLikeEmail("<angle@b>"));        // display-name syntax
    assert(!looksLikeEmail("a@b,c@d"));          // list

    std::printf("  ok: looksLikeEmail gate\n");
}

} // namespace

int main()
{
    std::printf("PrintToEmail smoke test\n");
    testPercentEncode();
    testBuildMailtoUrl();
    testTruncation();
    testLooksLikeEmail();
    std::printf("PASS\n");
    return 0;
}
