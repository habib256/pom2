// POM2 — GPL-3.0-or-later
#include "AiControlJson.h"

#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    using namespace pom2::aijson;
    // JSON clients encode Ctrl-H / Ctrl-L using these standard short escapes.
    assert(jsonGetString(R"({"text":"A\bB\fC"})", "text") == "A\bB\fC");
    // Every ASCII byte must survive our response encoder and request reader.
    std::string ascii;
    for (int c = 0; c < 128; ++c) ascii.push_back(static_cast<char>(c));
    const std::string body = "{\"text\":\"" + jsonEscape(ascii) + "\",\"other\":7}";
    assert(jsonGetString(body, "text") == ascii);
    assert(jsonGetString(body, "other") == "7");
    // Skipping an escaped value must still reach the following field.
    assert(jsonGetString(R"({"skip":"\b\f\"\\","text":"\b\f"})", "text") == "\b\f");

    // Status/file responses must preserve real Unicode filenames, so clients
    // can use the returned path in their next request without changing it.
    const std::string path = u8"/images/café-日本-U0001f431.dsk";
    assert(jsonEscape(path) == path && "JSON encoder changed a UTF-8 filename");
    assert(jsonGetString("{\"path\":\"" + jsonEscape(path) + "\"}", "path") == path);
    const std::string boundaries = "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF"
                                   "\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80"
                                   "\xF4\x8F\xBF\xBF";
    assert(jsonEscape(boundaries) == boundaries);
    // Arbitrary filesystem bytes must still produce well-formed JSON. Reject
    // isolated continuations, overlong encodings, surrogates and > U+10FFFF.
    for (const std::string& invalid : {
             std::string("\x80"), std::string("\xC0\xAF"), std::string("\xC1\xBF"),
             std::string("\xC2"), std::string("\xE0\x80\xAF"),
             std::string("\xED\xA0\x80"), std::string("\xF0\x80\x80\xAF"),
             std::string("\xF4\x90\x80\x80"), std::string("\xF5\x80\x80\x80"),
             std::string("\xFF")}) {
        std::string expected;
        for (unsigned char c : invalid) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04X", c);
            expected += escape;
        }
        assert(jsonEscape(invalid) == expected);
        assert(jsonEscape(path + invalid + path) == path + expected + path);
    }
    assert(jsonEscape("\xE2(\xA1") == "\\u00E2(\\u00A1");

    // Missing optional integers keep their defaults; explicitly empty values
    // are invalid and must not silently select a default drive/register.
    long drive = 1;
    assert(jsonOptionalIntOk(R"({"path":"disk.dsk"})", "drive", drive));
    assert(drive == 1);
    assert(!jsonOptionalIntOk(R"({"drive":""})", "drive", drive));
    assert(drive == 1);
    assert(!jsonOptionalIntOk(R"({"drive":null})", "drive", drive));
    assert(!jsonOptionalIntOk(R"({"drive":"no"})", "drive", drive));
    assert(drive == 1);
    assert(jsonOptionalIntOk(R"({"drive":0})", "drive", drive));
    assert(drive == 0);
    assert(jsonOptionalIntOk(R"({"drive":"1"})", "drive", drive));
    assert(drive == 1);
    assert(jsonOptionalIntOk(R"({"label":"drive","other":""})", "drive", drive));
    assert(drive == 1);
    // JSON escapes are equally valid in object keys and in string values.
    long pc = -1;
    assert(jsonGetInt(R"({"\u0070\u0063":4096})", "pc", pc));
    assert(pc == 4096);
    assert(jsonGetString(R"({"skip":"\"pc\":0","te\u0078t":"hello"})", "text") == "hello");
    assert(jsonGetString(R"({"a\"b":"quote","a\\b":"slash"})", "a\"b") == "quote");
    assert(jsonGetString(R"({"a\"b":"quote","a\\b":"slash"})", "a\\b") == "slash");
    assert(jsonGetString(R"({"\u00e9":"accent","\ud83d\udc31":"cat"})", "\xC3\xA9") == "accent");
    assert(jsonGetString(R"({"\u00e9":"accent","\ud83d\udc31":"cat"})", "\xF0\x9F\x90\xB1") == "cat");
    assert(!jsonOptionalIntOk(R"({"dr\u0069ve":""})", "drive", drive));
    std::puts("ai_control_json: OK");
}
