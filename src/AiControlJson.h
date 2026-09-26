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

// AiControlJson — the query-string and JSON helpers the AI control server's
// handlers share. Moved out of AiControlServer.cpp's anonymous namespace so
// the endpoint files beside it (AiControlServer_Printer.cpp) use the same
// parser instead of growing a second one. A deliberately small, lenient
// reader for flat request bodies — not a general JSON library.

#ifndef POM2_AI_CONTROL_JSON_H
#define POM2_AI_CONTROL_JSON_H

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace pom2::aijson {

// ─── String / parsing helpers ────────────────────────────────────────────

inline std::string toLowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::vector<std::pair<std::string,std::string>> parseQuery(const std::string& q)
{
    std::vector<std::pair<std::string,std::string>> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        size_t eq = q.find('=', i);
        std::string key, val;
        if (eq == std::string::npos || eq > amp) {
            key = q.substr(i, amp - i);
        } else {
            key = q.substr(i, eq - i);
            val = q.substr(eq + 1, amp - eq - 1);
        }
        out.emplace_back(std::move(key), std::move(val));
        i = amp + 1;
    }
    return out;
}

inline std::string queryParam(const std::string& q, const std::string& key)
{
    for (const auto& kv : parseQuery(q)) {
        if (kv.first == key) return kv.second;
    }
    return {};
}

/// Read the four hex digits of a `\uXXXX` escape starting at `at`.
inline bool jsonHex4(const std::string& s, size_t at, uint32_t& cp)
{
    if (at + 4 > s.size()) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = s[at + k];
        uint32_t d;
        if      (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    cp = v;
    return true;
}

/// Append one code point as UTF-8 — the encoding the host filesystem and the
/// Apple paste queue both take bytes in.
inline void jsonAppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) { out.push_back(static_cast<char>(cp)); return; }
    if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

/// Minimal extractor for `"key":<literal>` and `"key":"<quoted>"` shapes.
/// POM2's API surface uses flat one-level JSON only — no nested objects or
/// arrays — so a hand-rolled scanner is enough and avoids dragging in a
/// JSON dependency. Accepts unquoted tokens (numbers, true/false), quoted
/// strings with `\"`, `\\`, `\n`, `\r`, `\t` escapes. Unknown keys → empty
/// string; the caller supplies the default.
// Parse a JSON value (quoted string with escapes, or a bare token) that
// starts at body[pos] (pos just past the key's ':'). Returns unescaped text.
inline std::string jsonParseValueAt(const std::string& body, size_t pos)
{
    const size_t n = body.size();
    while (pos < n && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= n) return {};
    if (body[pos] == '"') {
        ++pos;
        std::string out;
        while (pos < n && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < n) {
                switch (body[pos + 1]) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'u': {
                        // NOT a nicety: RFC 8259 § 7 forbids a raw control
                        // byte inside a JSON string, so `\u0003` is the ONLY
                        // legal spelling of Ctrl-C for /keyboard — and
                        // `json.dumps` (ensure_ascii, its default) spells
                        // every non-ASCII byte that way too, which is how an
                        // accented /disk path arrived as `cafu00e9.dsk`.
                        // Without this arm the default below dropped the
                        // backslash and the machine was typed "u0003".
                        uint32_t cp = 0;
                        if (!jsonHex4(body, pos + 2, cp)) {
                            out.push_back(body[pos + 1]);   // malformed — as before
                            break;
                        }
                        size_t next = pos + 6;
                        // A code point above the BMP arrives as a surrogate
                        // PAIR; decoding the halves separately yields two
                        // invalid sequences instead of one character.
                        if (cp >= 0xD800 && cp <= 0xDBFF && next + 1 < n &&
                            body[next] == '\\' && body[next + 1] == 'u') {
                            uint32_t lo = 0;
                            if (jsonHex4(body, next + 2, lo) &&
                                lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) +
                                     (lo - 0xDC00u);
                                next += 6;
                            }
                        }
                        // A lone half is not a character; U+FFFD keeps the
                        // string well-formed instead of emitting a byte
                        // sequence no filesystem call can use.
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                        jsonAppendUtf8(out, cp);
                        pos = next;
                        continue;                      // pos already advanced
                    }
                    default:   out.push_back(body[pos + 1]); break;
                }
                pos += 2;
            } else {
                out.push_back(body[pos]); ++pos;
            }
        }
        return out;
    }
    std::string out;
    while (pos < n && body[pos] != ',' && body[pos] != '}' &&
           !std::isspace(static_cast<unsigned char>(body[pos]))) {
        out.push_back(body[pos]); ++pos;
    }
    return out;
}

// Return the index just past the value at body[pos] (past ':'), so the key
// scan can skip a non-matching field's value without mistaking the value's
// contents for a key.
inline size_t jsonSkipValueAt(const std::string& body, size_t pos)
{
    const size_t n = body.size();
    while (pos < n && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos < n && body[pos] == '"') {
        ++pos;
        while (pos < n && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < n) pos += 2; else ++pos;
        }
        if (pos < n) ++pos;                 // past closing quote
    } else {
        while (pos < n && body[pos] != ',' && body[pos] != '}') ++pos;
    }
    return pos;
}

/// Extract `key`'s value from flat one-level JSON. Walks the body skipping
/// over string VALUES so the key only matches at an object-key position
/// (immediately followed by ':') — a value containing another field's name
/// as a substring no longer hijacks the match. Unknown key → empty string.
inline std::string jsonGetString(const std::string& body, const std::string& key)
{
    const size_t n = body.size();
    size_t i = 0;
    while (i < n) {
        if (body[i] != '"') { ++i; continue; }
        // Read the string token at i (a key candidate).
        std::string name;
        size_t j = i + 1;
        for (; j < n && body[j] != '"'; ++j) {
            if (body[j] == '\\' && j + 1 < n) { name.push_back(body[j + 1]); ++j; }
            else                              { name.push_back(body[j]); }
        }
        if (j >= n) break;                  // unterminated string
        size_t k = j + 1;
        while (k < n && std::isspace(static_cast<unsigned char>(body[k]))) ++k;
        if (k < n && body[k] == ':') {       // `name` is an object key
            if (name == key) return jsonParseValueAt(body, k + 1);
            i = jsonSkipValueAt(body, k + 1);
        } else {
            i = j + 1;                       // `name` was a value string
        }
    }
    return {};
}

inline bool jsonGetInt(const std::string& body, const std::string& key, long& out);

/// An optional integer key: absent is fine (`out` untouched), present and
/// not a whole integer is an error the caller answers with a 400.
inline bool jsonOptionalIntOk(const std::string& body, const std::string& key, long& out)
{
    long v = 0;
    if (jsonGetString(body, key).empty()) return true;
    if (!jsonGetInt(body, key, v)) return false;
    out = v;
    return true;
}

/// A query-string number: decimal, or hex with `0x`. The whole token, no
/// sign, no whitespace. `std::stol(..., 0)` read a leading 0 as OCTAL and
/// dropped trailing garbage, so `/mem?addr=0800` read $0000 and `addr=0300`
/// read $C0 (bug hunt 2026-09-17). Returns -1 when absent or malformed.
inline long parseQueryNumber(const std::string& s)
{
    const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    const std::string digits = hex ? s.substr(2) : s;
    if (digits.empty() || digits.size() > 10) return -1;
    for (char c : digits)
        if (!(hex ? std::isxdigit(static_cast<unsigned char>(c))
                  : std::isdigit(static_cast<unsigned char>(c)))) return -1;
    try { return std::stol(digits, nullptr, hex ? 16 : 10); } catch (...) { return -1; }
}

inline bool jsonGetInt(const std::string& body, const std::string& key, long& out)
{
    const std::string s = jsonGetString(body, key);
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        // Accept decimal or 0x… hex for convenience.
        const int base = (s.size() > 2 && s[0] == '0' && (s[1]=='x'||s[1]=='X'))
                       ? 16 : 10;
        const long v = std::stol(s, &pos, base);
        // The WHOLE token has to be a number. `pos > 0` accepted a partial
        // parse, and the value was mangled BEFORE the careful range checks
        // downstream ever saw it: `{"cycles_per_frame":2.5e6}` — legal JSON —
        // parsed as 2, passed the [1, 2000000] check, and set the machine to
        // ~120 emulated cycles per second while answering 200 OK. Same shape
        // for `{"pc":1e3}` -> $0001 and `{"drive":"1x"}` -> 1.
        // And `out` is written only on success: callers that ignore the
        // return value (`/disk`'s drive and slot) kept the partial number
        // (bug hunt 2026-09-17).
        if (pos != s.size() || !(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-'))
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool fromHex(char c, uint8_t& nib)
{
    if (c >= '0' && c <= '9') { nib = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { nib = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { nib = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

inline bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi, lo;
        if (!fromHex(hex[i], hi) || !fromHex(hex[i + 1], lo)) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

inline std::string bytesToHex(const uint8_t* data, size_t n)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

inline std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                } else {
                    // Escape everything above ASCII rather than passing the
                    // raw byte through. Filenames on macOS and Linux may hold
                    // bytes that are not valid UTF-8, and one of them inside a
                    // mounted image's path made every subsequent /status reply
                    // undecodable — the agent's polling broke permanently,
                    // until the disk was ejected, with nothing to point at.
                    // \u00XX is valid JSON and round-trips the byte value.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                }
        }
    }
    return out;
}

}  // namespace pom2::aijson

#endif  // POM2_AI_CONTROL_JSON_H
