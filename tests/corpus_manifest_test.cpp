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

// corpus_manifest — the compatibility corpus's two files (VALIDATION-006),
// with no machine: the manifest grammar and each of its refusals, the
// expectations round trip, and the frame hash's fixed values. A refusal that
// went quiet would let a typo'd manifest run a title nobody meant to run, or
// none at all, and report a pass rate over it.

#include "CorpusManifest.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

using namespace pom2::corpus;

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool parses(const std::string& text, Manifest& m, std::string& error)
{
    std::istringstream in(text);
    return parseManifest(in, m, error);
}

// Each must be refused, and the error must name the line.
void refuses(const std::string& text, int line, const char* what)
{
    Manifest m;
    std::string error;
    const bool ok = parses(text, m, error);
    const std::string tag = "line " + std::to_string(line) + ":";
    if (ok || error.rfind(tag, 0) != 0) {
        std::printf("FAIL: %s — %s (error: '%s')\n", what,
                    ok ? "accepted" : "wrong line", error.c_str());
        ++failures;
    }
}

}  // namespace

int main()
{
    // ── A full title.
    const std::string good =
        "# comment line\n"
        "title dix-menu iie-pal ci dix   # trailing comment\n"
        "  mockingboard 2\n"
        "  smartport 1 disks_3.5/DIX-fix.po sha256="
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "  rgb 7 feline\n"
        "  mouse 4 applewin\n"
        "  run 25\n"
        "  check menu\n"
        "  key LEFT\n"
        "  key $0d\n"
        "  key Y\n"
        "  type \"RUN \\\"X\\\"\\r\"\n"
        "  run 0.5\n"
        "  check after\n"
        "\n"
        "title boot-hdv iie\n"
        "  hdd 2 hdv/x.hdv\n"
        "  boot 7\n"
        "  run 3\n"
        "  check prompt\n";
    Manifest m;
    std::string error;
    expect(parses(good, m, error), "a well-formed manifest parses");
    if (m.titles.size() == 2) {
        const Title& t = m.titles[0];
        expect(t.id == "dix-menu" && t.profile == "iie-pal", "id and profile");
        expect(t.hasTag("ci") && t.hasTag("dix") && !t.hasTag("comment"),
               "tags stop at the comment");
        expect(t.devices.size() == 4, "four devices");
        expect(t.devices[1].kind == "smartport" && t.devices[1].unit == 1 &&
               t.devices[1].media == "disks_3.5/DIX-fix.po" &&
               t.devices[1].sha256.size() == 64, "media line");
        expect(t.devices[2].model == "feline" && t.devices[2].slot == 7, "card line");
        expect(t.bootSlot == 0, "no boot line is a cold boot");
        expect(t.steps.size() == 8, "eight steps");
        expect(t.steps[0].kind == Step::Kind::Run && t.steps[0].seconds == 25, "run");
        expect(t.steps[2].kind == Step::Kind::Key && t.steps[2].key == 0x08, "LEFT = $08");
        expect(t.steps[3].key == 0x0D, "$0d");
        expect(t.steps[4].key == 'Y', "a printable character");
        expect(t.steps[5].kind == Step::Kind::Text && t.steps[5].text == "RUN \"X\"\r",
               "quoted text with escapes");
        expect(m.titles[1].bootSlot == 7 && m.titles[1].devices[0].unit == 2,
               "boot slot and drive 2");
    } else {
        expect(false, "two titles");
    }

    {
        Manifest swaps;
        expect(parses("title flip iie\ndiskii 1 a.woz\nrun 30\nswap 1 b.woz\nkey SPACE\ncheck game\n", swaps, error),
               "a scripted disk flip parses");
        if (!swaps.titles.empty()) {
            const auto& flip = swaps.titles.front();
            expect(flip.devices.size() == 2 && flip.devices[1].deferred,
                   "side B is not mounted at boot");
            expect(flip.steps[1].kind == Step::Kind::Swap && flip.steps[1].deviceIndex == 1,
                   "swap retains its place and media index");
        }
        refuses("title flip iie\nswap 1 b.woz\ncheck x\n", 2, "swap without initial mount");
        refuses("title flip iie\ndiskii 1 a.woz\nswap 2 b.woz\ncheck x\n", 3, "swap wrong drive");
        refuses("title flip iie\ndiskii 1 a.woz\nswap 1 b.woz sha256=no\ncheck x\n", 3, "swap bad hash");
    }

    // ── Refusals, each on its line.
    refuses("run 5\n", 1, "a step before any title");
    refuses("title a iie\n  frob 1\n  check x\n", 2, "an unknown keyword");
    refuses("title a iie\n  check x\ntitle a iie\n  check y\n", 3, "a duplicate title");
    refuses("title a iie\n  check x\n  check x\n", 3, "a duplicate check");
    refuses("title a iie\n  run 5\n", 1, "a title with no check");
    refuses("title a iie\n  smartport 3 x.po\n  check x\n", 2, "drive 3");
    refuses("title a iie\n  mockingboard 8\n  check x\n", 2, "slot 8");
    refuses("title a iie\n  mockingboard 4 phasor\n  check x\n", 2, "an unknown model");
    refuses("title a iie\n  key NOPE\n  check x\n", 2, "an unknown key name");
    refuses("title a iie\n  key $80\n  check x\n", 2, "a key above $7F");
    refuses("title a iie\n  run -1\n  check x\n", 2, "a negative run");
    refuses("title a iie\n  run 5s\n  check x\n", 2, "a run with a unit");
    refuses("title a iie\n  type \"open\n  check x\n", 2, "an unterminated string");
    refuses("title a iie\n  diskii 1 a.dsk sha256=ABC\n  check x\n", 2, "a short sha256");
    refuses("title a iie\n  diskii 1 a.dsk\n  diskii 1 b.dsk\n  check x\n", 3,
            "one drive given twice");
    refuses("title a\n  check x\n", 1, "a title without a profile");
    // Ids and check names become path components and the scratch is
    // remove_all'd per title: `..` removed $TMPDIR (bug hunt 2026-11-07).
    refuses("title .. iie\n  check x\n", 1, "a title id of '..'");
    refuses("title /abs iie\n  check x\n", 1, "an absolute title id");
    refuses("title a/b iie\n  check x\n", 1, "a title id with a separator");
    refuses("title .hidden iie\n  check x\n", 1, "a title id starting with '.'");
    refuses("title a iie\n  check ../x\n", 2, "a check name with a separator");
    refuses("title a iie\n  check ..\n", 2, "a check name of '..'");
    {
        Manifest ok;
        std::string err;
        expect(parses("title v1.2_x-y iie\n  check a.b_c-1\n", ok, err),
               "a plain id with '.', '_' and '-' is still accepted");
    }

    // ── Expectations: round trip, sorted, refusals.
    Expectations e;
    e[{"b", "two"}] = {"00000000000000ff", "abc123abc123"};
    e[{"a", "one"}] = {"0123456789abcdef", "abc123abc123"};
    const std::string text = formatExpectations(e);
    expect(text.find("a one 0123456789abcdef fw=abc123abc123\nb two") != std::string::npos,
           "expectations are written sorted, one per line");
    {
        Expectations back;
        std::istringstream in(text);
        expect(parseExpectations(in, back, error) && back.size() == 2 &&
                   back[{"a", "one"}].hash == "0123456789abcdef" &&
                   back[{"b", "two"}].firmware == "abc123abc123",
               "expectations round-trip");
    }
    {
        Expectations back;
        std::istringstream in("a one 0123 fw=x\n");
        expect(!parseExpectations(in, back, error), "a short hash is refused");
        std::istringstream dup("a one 0123456789abcdef fw=x\na one 0123456789abcdef fw=x\n");
        expect(!parseExpectations(dup, back, error), "a check recorded twice is refused");
    }

    // ── The frame hash: size counts, every pixel counts, fixed values.
    const std::uint32_t px[4] = {0xFF000000u, 0xFFFFFFFFu, 0xFF0000FFu, 0xFF00FF00u};
    const std::uint64_t h22 = frameHash(px, 2, 2);
    expect(h22 != frameHash(px, 4, 1), "the frame size is hashed");
    std::uint32_t px2[4] = {px[0], px[1], px[2], px[3] ^ 1u};
    expect(h22 != frameHash(px2, 2, 2), "the last pixel is hashed");
    expect(hashHex(0x1234u) == "0000000000001234", "hashHex is 16 lowercase digits");
    // Fixed values, computed independently (Python, struct '<I'): changing
    // the hash invalidates every recorded frame, so it must be a decision.
    expect(frameHash(nullptr, 0, 0) == 0x47fe0d7eaf8e51e3ULL,
           "FNV-1a 64 of an empty 0x0 frame");
    expect(hashHex(h22) == "5ff5a5be848c344c", "FNV-1a 64 of the 2x2 frame");

    if (failures) return 1;
    std::printf("corpus_manifest: OK\n");
    return 0;
}
