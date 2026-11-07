// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// host_file_import — HostFileImport.h: what a host file becomes on its way
// onto an Apple volume. One decision shared by pom2_dos33 and pom2_prodos.

#include "AppleSingle.h"
#include "ApplesoftTokenizer.h"
#include "HostFileImport.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pom2;

namespace {
int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

std::vector<uint8_t> bytesOf(const std::string& s) { return { s.begin(), s.end() }; }
}  // namespace

int main()
{
    const std::string listing = "10 PRINT \"HI\"\n20 GOTO 10\n";
    std::vector<uint8_t> program;
    std::vector<applesoft::TokenizeError> errors;
    CHECK(applesoft::tokenize(listing, program, errors));

    // What is a listing and what is not.
    CHECK(looksLikeBasicListing(bytesOf(listing)));
    CHECK(looksLikeBasicListing(bytesOf("\r\n  10 REM\r\n")));
    CHECK(!looksLikeBasicListing(program));                    // $00 terminators, tokens
    CHECK(!looksLikeBasicListing(bytesOf("PRINT 1\n")));       // no line number
    CHECK(!looksLikeBasicListing({}));
    CHECK(!looksLikeBasicListing(bytesOf("\n\n")));

    ImportedFile f;
    std::string error;

    // A typed listing under an Applesoft name becomes the program.
    CHECK(importHostFile("/tmp/x/HELLO.bas", bytesOf(listing), f, error));
    CHECK(f.name == "HELLO" && f.type == 0xFC && f.aux == 0x0801 && f.auxStated);
    CHECK(f.data == program);
    CHECK(f.note.find("tokenized") != std::string::npos);

    // The program as the disk stores it is left exactly as it is.
    CHECK(importHostFile("HELLO.bas", program, f, error));
    CHECK(f.data == program && f.type == 0xFC && f.note.empty());
    CHECK(!f.auxStated);                                       // nothing said the aux

    // …and so is a listing, on request.
    CHECK(importHostFile("HELLO.bas", bytesOf(listing), f, error, /*tokenize=*/false));
    CHECK(f.data == bytesOf(listing));

    // A listing under another type's name is not touched: only Applesoft is.
    CHECK(importHostFile("HELLO.txt", bytesOf(listing), f, error));
    CHECK(f.type == 0x04 && f.data == bytesOf(listing) && f.name == "HELLO");

    // The tag carries the pair, and a tagged Applesoft listing is tokenized
    // AT THE ADDRESS THE TAG NAMES.
    CHECK(importHostFile("GAME#062000", { 1, 2, 3 }, f, error));
    CHECK(f.name == "GAME" && f.type == 0x06 && f.aux == 0x2000 && f.auxStated);
    CHECK(importHostFile("HI.BAS#FC4001", bytesOf(listing), f, error));
    std::vector<uint8_t> at4001;
    CHECK(applesoft::tokenize(listing, at4001, errors, 0x4001));
    CHECK(f.name == "HI.BAS" && f.aux == 0x4001 && f.data == at4001 && f.data != program);

    // An untagged .bin states no aux: the tool's default applies, and the
    // tools' default is the one `get` would have written (0), never a guess.
    CHECK(importHostFile("PIC.bin", { 9 }, f, error));
    CHECK(f.name == "PIC" && f.type == 0x06 && f.aux == 0 && !f.auxStated);

    // AppleSingle states the type itself and wins over the name.
    applesingle::File wrapped;
    wrapped.data = { 0xA9, 0xC1, 0x60 };
    wrapped.hasProdosInfo = true;
    wrapped.fileType = 0x06;
    wrapped.auxType  = 0x0300;
    CHECK(importHostFile("prog.as", applesingle::encode(wrapped), f, error));
    CHECK(f.name == "prog" && f.type == 0x06 && f.aux == 0x0300 && f.auxStated);
    CHECK(f.data == wrapped.data);
    CHECK(f.note.find("AppleSingle") != std::string::npos);
    CHECK(importHostFile("thing.txt", applesingle::encode(wrapped), f, error));
    CHECK(f.type == 0x06 && f.name == "thing");                // the wrapper, not ".txt"

    // A wrapper with no ProDOS info: the bytes are unwrapped, the name types them.
    applesingle::File bare;
    bare.data = bytesOf("plain\n");
    CHECK(importHostFile("NOTE.txt", applesingle::encode(bare), f, error));
    CHECK(f.type == 0x04 && f.data == bare.data);

    // A wrapped Applesoft LISTING is unwrapped, then tokenized at its aux.
    applesingle::File wrappedListing;
    wrappedListing.data = bytesOf(listing);
    wrappedListing.hasProdosInfo = true;
    wrappedListing.fileType = 0xFC;
    wrappedListing.auxType  = 0x0801;
    CHECK(importHostFile("HELLO.as", applesingle::encode(wrappedListing), f, error));
    CHECK(f.type == 0xFC && f.data == program);

    // A resource fork is half of a GS/OS extended file: dropping it is the
    // user's call, never a silent one (bug hunt 2026-11-07).
    applesingle::File forked;
    forked.data = { 1, 2, 3 };
    forked.resource = { 9, 9, 9, 9 };
    forked.hasProdosInfo = true;
    forked.fileType = 0xB3;
    forked.auxType  = 0xDB07;
    {
        ImportedFile keep;
        keep.name = "KEEP";
        CHECK(!importHostFile("APP.as", applesingle::encode(forked), keep, error));
        CHECK(error.find("resource fork") != std::string::npos && keep.name == "KEEP");
    }
    CHECK(importHostFile("APP.as", applesingle::encode(forked), f, error, true,
                         /*dropResourceFork=*/true));
    CHECK(f.data == forked.data && f.type == 0xB3 && f.note.find("DROPPED") != std::string::npos);

    // The leading-dot escape, as the ProDOS host folder and `pom2_prodos get`
    // write it, comes back as the guest's `.PROFILE` — on a TAGGED name only
    // (HostFileTag.h); an untagged `%2e` name keeps the name it always had.
    CHECK(importHostFile("%2ePROFILE#040000", bytesOf("X\n"), f, error));
    CHECK(f.name == ".PROFILE" && f.type == 0x04);
    CHECK(importHostFile("%2EPROFILE#040000", bytesOf("X\n"), f, error));
    CHECK(f.name == ".PROFILE");
    CHECK(importHostFile("%2eNOTES.txt", bytesOf("X\n"), f, error));
    CHECK(f.name == "%2eNOTES" && f.type == 0x04);

    // Refusals name the cause, and leave the output alone.
    ImportedFile untouched;
    untouched.name = "KEEP";
    std::vector<uint8_t> broken = applesingle::encode(wrapped);
    broken.resize(30);
    CHECK(!importHostFile("prog.as", broken, untouched, error));
    CHECK(error.find("AppleSingle") != std::string::npos && untouched.name == "KEEP");
    CHECK(!importHostFile("BAD.bas", bytesOf("10 PRINT 1\n99999 PRINT 2\n"), untouched, error));
    CHECK(error.find("line 2") != std::string::npos && untouched.name == "KEEP");

    if (failures) { std::printf("host_file_import: %d check(s) failed\n", failures); return 1; }
    std::printf("host_file_import: OK\n");
    return 0;
}
