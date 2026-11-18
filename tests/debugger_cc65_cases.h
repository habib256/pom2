// cc65 debug-information import (src/Cc65DebugInfo.h), included by
// debugger_test.cpp. The first fixture is real ld65 output, not a hand-made
// imitation: cc65 2.19, `ca65 -g main.s && ld65 -C cfg --dbgfile main.dbg`
// on a program with a macro, a .proc and a cheap local.
#pragma once
#include "Cc65DebugInfo.h"
#include "DebuggerSourceMap.h"
#include <cassert>
#include <cstdint>
#include <string>

namespace cc65case {

// main.s, for reading the expectations below:
//    1        .macro  twice op         10        .proc draw
//    2        op                       11 loop:  sta $0400,x
//    3        op                       12        dex
//    4        .endmacro                13        bne loop
//    5        .segment "CODE"          14        rts
//    6        .export start            15        .endproc
//    7 start:  lda #$00                16        jsr draw
//    8 @loop:  twice inx               17        rts
//    9        bne @loop
inline const char* kLd65Main =
    "version\tmajor=2,minor=0\n"
    "info\tcsym=0,file=1,lib=0,line=14,mod=1,scope=2,seg=6,span=13,sym=4,type=0\n"
    "file\tid=0,name=\"main.s\",size=290,mtime=0x6AFDE059,mod=0\n"
    "line\tid=0,file=0,line=11,span=5\n"
    "line\tid=1,file=0,line=9,span=4\n"
    "line\tid=2,file=0,line=14,span=8\n"
    "line\tid=3,file=0,line=17,span=12\n"
    "line\tid=4,file=0,line=12,span=6\n"
    "line\tid=5,file=0,line=7,span=0\n"
    "line\tid=6,file=0,line=16,span=10\n"
    "line\tid=7,file=0,line=8,span=3\n"
    "line\tid=8,file=0,line=13,span=7\n"
    "line\tid=9,file=0,line=2,type=2,count=1,span=1\n"
    "line\tid=10,file=0,line=10\n"
    "line\tid=11,file=0,line=3,type=2,count=1,span=2\n"
    "line\tid=12,file=0,line=15\n"
    "line\tid=13,file=0,line=6\n"
    "mod\tid=0,name=\"main.o\",file=0\n"
    "seg\tid=0,name=\"CODE\",start=0x000800,size=0x0011,addrsize=absolute,type=ro,oname=\"main.bin\",ooffs=0\n"
    "seg\tid=1,name=\"RODATA\",start=0x000000,size=0x0000,addrsize=absolute,type=rw\n"
    "seg\tid=4,name=\"ZEROPAGE\",start=0x000000,size=0x0000,addrsize=zeropage,type=rw\n"
    "span\tid=0,seg=0,start=0,size=2\n"
    "span\tid=1,seg=0,start=2,size=1\n"
    "span\tid=2,seg=0,start=3,size=1\n"
    "span\tid=3,seg=0,start=2,size=2\n"
    "span\tid=4,seg=0,start=4,size=2\n"
    "span\tid=5,seg=0,start=6,size=3\n"
    "span\tid=6,seg=0,start=9,size=1\n"
    "span\tid=7,seg=0,start=10,size=2\n"
    "span\tid=8,seg=0,start=12,size=1\n"
    "span\tid=9,seg=0,start=6,size=7\n"
    "span\tid=10,seg=0,start=13,size=3\n"
    "span\tid=11,seg=0,start=0,size=17\n"
    "span\tid=12,seg=0,start=16,size=1\n"
    "scope\tid=0,name=\"\",mod=0,size=17,span=11\n"
    "scope\tid=1,name=\"draw\",mod=0,type=scope,size=7,parent=0,sym=1,span=9\n"
    "sym\tid=0,name=\"loop\",addrsize=absolute,size=3,scope=1,def=0,ref=8,val=0x806,seg=0,type=lab\n"
    "sym\tid=1,name=\"draw\",addrsize=absolute,size=7,scope=0,def=10,ref=6,val=0x806,seg=0,type=lab\n"
    "sym\tid=2,name=\"@loop\",addrsize=absolute,parent=3,def=7,ref=1,val=0x802,seg=0,type=lab\n"
    "sym\tid=3,name=\"start\",addrsize=absolute,size=2,scope=0,def=5,ref=13,val=0x800,seg=0,type=lab\n";

} // namespace cc65case

static void testCc65DebugInfoImport()
{
    using B = pom2::DebugBank;
    namespace dbg = pom2::cc65dbg;
    dbg::Conversion c; std::string error;

    // Real ld65 output → a map the existing importer accepts.
    assert(dbg::toSourceMap(cc65case::kLd65Main, c, error));
    assert(c.entries == 9 && c.skipped == 0);
    pom2::DebuggerSourceMap map;
    assert(map.load(c.text, error));
    auto line = [&](uint16_t a, B bank = {}) {
        const auto* e = map.lookup({a, bank});
        return e ? e->line : 0u;
    };
    assert(line(0x0800) == 7 && line(0x0801) == 7);
    // $0802-$0803: the `twice inx` invocation (line 8) owns its expansion
    // bytes; the macro body lines 2 and 3 cover them too and lose.
    assert(line(0x0802) == 8 && line(0x0803) == 8);
    // The `.proc` scope span ($0806-$080C) carries no line record of its own.
    assert(line(0x0806) == 11 && line(0x0809) == 12 && line(0x080C) == 14);
    assert(line(0x0810) == 17 && line(0x0811) == 0 && line(0x07FF) == 0);
    assert(map.lookup({0x0806, {}})->file == "main.s");
    // Unqualified: the fallback for every bank.
    assert(line(0x0806, {B::Aux, 3}) == 11);

    // Labels: scoped the ca65 way, cheap local under its owner.
    assert(dbg::toSymbols(cc65case::kLd65Main, c, error));
    assert(c.entries == 4 && c.skipped == 0);
    pom2::DebuggerSymbols symbols;
    assert(symbols.load(c.text, error));
    uint16_t addr = 0;
    assert(symbols.resolve("draw::loop", addr) && addr == 0x0806);
    assert(symbols.resolve("start@loop", addr) && addr == 0x0802);
    assert(symbols.resolve("start", addr) && addr == 0x0800);
    assert(symbols.label(0x0800) == "start");

    // A POM2 bank qualifier, when the caller names one.
    assert(dbg::toSourceMap(cc65case::kLd65Main, c, error, "aux1"));
    assert(map.load(c.text, error));
    assert(line(0x0800, {B::Aux, 1}) == 7 && line(0x0800, {B::Main}) == 0);

    // C over assembler: cc65's external (type=1) lines win the bytes the
    // generated .s also claims; a narrower C span wins inside a wider one.
    const char* cOverAsm =
        "version\tmajor=2,minor=0\n"
        "file\tid=0,name=\"hello.s\",size=1,mtime=0x0,mod=0\n"
        "file\tid=1,name=\"my dir/hello.c\",size=1,mtime=0x0,mod=0\n"
        "seg\tid=0,name=\"CODE\",start=0x000837,size=0x0020,addrsize=absolute,type=ro\n"
        "span\tid=0,seg=0,start=0,size=7\n"
        "span\tid=1,seg=0,start=0,size=2\n"
        "span\tid=2,seg=0,start=7,size=16\n"
        "span\tid=3,seg=0,start=9,size=2\n"
        "span\tid=4,seg=0,start=23,size=1\n"
        "line\tid=0,file=0,line=40,span=1\n"
        "line\tid=1,file=1,line=3,type=1,span=0\n"
        "line\tid=2,file=1,line=4,type=1,span=2\n"
        "line\tid=3,file=1,line=5,type=1,span=3\n"
        "line\tid=4,file=0,line=52,span=4\n";
    assert(dbg::toSourceMap(cOverAsm, c, error));
    assert(map.load(c.text, error));
    assert(line(0x0837) == 3 && map.lookup({0x0837, {}})->file == "my dir/hello.c");
    assert(line(0x083E) == 4 && line(0x0840) == 5 && line(0x0841) == 5 && line(0x0842) == 4);
    assert(line(0x084E) == 52 && map.lookup({0x084E, {}})->file == "hello.s");

    // Refusals leave the caller's state alone (the importer is never fed).
    for (const char* bad : {
             "",                                                   // no version
             "line\tid=0,file=0,line=1,span=0\n",                  // no version first
             "version\tmajor=3,minor=0\n",                         // unknown major
             "version\tmajor=2,minor=0\nfile\tid=0,name=\"x.s\n",  // unterminated string
             "version\tmajor=2,minor=0\nfile\tid=0,name\n",        // no '='
             "version\tmajor=2,minor=0\n"}) {                      // no lines
        assert(!dbg::toSourceMap(bad, c, error));
        assert(!error.empty());
    }
    // A span past $FFFF is skipped, not wrapped into zero page.
    const char* past =
        "version\tmajor=2,minor=0\n"
        "file\tid=0,name=\"a.s\",size=1,mtime=0x0,mod=0\n"
        "seg\tid=0,name=\"HI\",start=0x00FFFF,size=0x0002,addrsize=absolute,type=ro\n"
        "seg\tid=1,name=\"LO\",start=0x001000,size=0x0001,addrsize=absolute,type=ro\n"
        "span\tid=0,seg=0,start=0,size=2\n"
        "span\tid=1,seg=1,start=0,size=1\n"
        "line\tid=0,file=0,line=1,span=0\n"
        "line\tid=1,file=0,line=2,span=1\n";
    assert(dbg::toSourceMap(past, c, error));
    assert(c.entries == 1 && c.skipped == 1);
    assert(map.load(c.text, error) && line(0x1000) == 2 && line(0x0000) == 0);

    // Two labels with one name: the second is skipped, so the import still
    // loads (DebuggerSymbols refuses a whole file on one conflict). An equ
    // is not a label.
    const char* clash =
        "version\tmajor=2,minor=0\n"
        "sym\tid=0,name=\"tick\",addrsize=absolute,scope=0,val=0x900,type=lab\n"
        "sym\tid=1,name=\"tick\",addrsize=absolute,scope=0,val=0x902,type=lab\n"
        "sym\tid=2,name=\"COUNT\",addrsize=zeropage,scope=0,val=0x5,type=equ\n"
        "sym\tid=3,name=\"far\",addrsize=far,scope=0,val=0x12000,type=lab\n";
    assert(dbg::toSymbols(clash, c, error));
    assert(c.entries == 1 && c.skipped == 2);
    assert(symbols.load(c.text, error) && symbols.resolve("tick", addr) && addr == 0x0900);
    assert(!symbols.resolve("COUNT", addr) || addr == 0x0005);  // only as a hex literal
    assert(symbols.label(0x0005).empty());

    assert(dbg::isDebugFilePath("build/game.dbg") && dbg::isDebugFilePath("GAME.DBG"));
    assert(!dbg::isDebugFilePath("game.p2map") && !dbg::isDebugFilePath("dbg"));
}
