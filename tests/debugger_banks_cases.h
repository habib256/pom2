// Bank-aware debugger integration cases, included by debugger_test.cpp.
#pragma once
#include "DebuggerSourceMap.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

static void testPhysicalBankRouting()
{
    using B=pom2::DebugBank;
    Memory m;
    m.setIIEMode(true); m.setRamWorksBanks(4);
    assert(m.debugBank(0x0800)==B{B::Main});
    m.memWrite(0xC005,0); // RAMWRT: write aux, read main
    assert(m.debugBank(0x0800)==B{B::Main});
    assert((m.debugBank(0x0800,true)==B{B::Aux,0}));
    m.memWrite(0xC003,0); m.memWrite(0xC073,3);
    assert((m.debugBank(0x0800)==B{B::Aux,3}));
    assert(m.debugBank(0x0040)==B{B::Main});
    m.memWrite(0xC009,0); // ALTZP includes aux LC
    assert((m.debugBank(0x0040)==B{B::Aux,3}));
    (void)m.memRead(0xC083); (void)m.memRead(0xC083);
    assert((m.debugBank(0xD000)==B{B::AuxLanguage,3,2}));
    assert((m.debugBank(0xE000)==B{B::AuxLanguage,3,0}));
    (void)m.memRead(0xC08B);
    assert((m.debugBank(0xD000)==B{B::AuxLanguage,3,1}));
    assert((m.debugBank(0xE000)==B{B::AuxLanguage,3,0})); // shared high RAM
    m.memWrite(0xC008,0);
    assert((m.debugBank(0xD000)==B{B::Language,0,1}));
    (void)m.memRead(0xC082);
    assert((m.debugBank(0xD000)==B{B::Rom,0}));
    assert(m.debugBank(0xD000,true)==B{B::Ignored});
    (void)m.memRead(0xC081); (void)m.memRead(0xC081);
    assert((m.debugBank(0xD000)==B{B::Rom,0}));
    assert((m.debugBank(0xD000,true)==B{B::Language,0,2}));
    // 80STORE routes text/HGR independently of RAMRD.
    m.memWrite(0xC001,0); m.memWrite(0xC054,0);
    assert(m.debugBank(0x0400)==B{B::Main});
    m.memWrite(0xC055,0);
    assert((m.debugBank(0x0400)==B{B::Aux,3}));
    m.memWrite(0xC007,0);
    assert((m.debugBank(0xC600)==B{B::Rom,0}));
    m.memWrite(0xC006,0);
    assert((m.debugBank(0xC600)==B{B::Slot,6}));
    assert(m.debugBank(0xC083)==B{B::Io});
    std::vector<uint8_t> before,after;
    m.appendSnapshotState(before);
    for (unsigned a=0;a<65536;++a) { m.debugBank(a); m.debugBank(a,true); }
    m.appendSnapshotState(after);
    assert(before==after);
}

static void testBankBreakpointsAndWatches()
{
    using B=pom2::DebugBank; using D=pom2::Debugger;
    EmulationController ctrl;
    auto st=ctrl.lockState(); auto& m=st.memory(); auto& cpu=st.cpu(); auto& d=ctrl.debugger();
    m.setIIEMode(true); m.setRamWorksBanks(4);
    d.addBreakpoint(0x0800,{B::Aux,1});
    d.addBreakpoint(0x0800,{B::Aux,2});
    assert(d.breakpointCount()==2 && d.armed());
    assert(!d.onInstruction(0x0800));
    m.memWrite(0xC003,0); m.memWrite(0xC073,1);
    assert(d.onInstruction(0x0800));
    assert((d.lastHit().pcBank==B{B::Aux,1}));
    d.clearHit(); d.armResumeFrom(0x0800);
    m.memWrite(0xC073,2);
    assert(d.onInstruction(0x0800)); // amnesty never leaks to a different bank
    d.clearHit(); d.armResumeFrom(0x0800);
    assert(!d.onInstruction(0x0800));
    assert(d.onInstruction(0x0800));
    d.clearForTimeJump();
    assert(d.breakpointCount()==2);
    d.clearBreakpoints(); assert(!d.armed());
    D::Condition c; c.value=0x42;
    d.addConditionalBreakpoint(0x0800,c,cpu,{B::Aux,2});
    cpu.setAccumulator(0x41); assert(!d.onInstruction(0x0800));
    cpu.setAccumulator(0x42); assert(d.onInstruction(0x0800));
    d.clearHit(); d.clearBreakpoints();
    d.setTransient(0x0800,D::Reason::RunToCursor,{B::Main});
    assert(!d.onInstruction(0x0800));
    m.memWrite(0xC002,0); assert(d.onInstruction(0x0800));
    d.clearHit();
    // Same address, different access routes and permissions.
    d.setWatchpoint(0x40,D::Read,{B::Main});
    d.setWatchpoint(0x40,D::Write,{B::Aux,2});
    d.setWatchpoint(0x0900,D::Write,{B::Aux,2});
    ctrl.syncDebugHook();
    m.memWrite(0x40,0x11); assert(!d.stopRequested());
    (void)m.memRead(0x40); assert(d.lastHit().reason==D::Reason::WatchRead);
    assert(d.lastHit().bank==B{B::Main}); d.clearHit();
    m.memWrite(0xC009,0); // aux zero page
    (void)m.memRead(0x40); assert(!d.stopRequested());
    m.memWrite(0x40,0x22); assert(d.lastHit().reason==D::Reason::WatchWrite);
    assert((d.lastHit().bank==B{B::Aux,2})); d.clearHit();
    m.memWrite(0xC005,0); m.memWrite(0x0900,0x33);
    assert((d.lastHit().bank==B{B::Aux,2}));
    assert(m.peekCpuWriteTarget(0x0900)==0x33); d.clearHit();
    m.memWrite(0xC073,1); m.memWrite(0x0900,0x44); assert(!d.stopRequested());
    d.setWatchpoint(0x0900,D::None,{B::Aux,2});
    assert(d.watchpointCount()==2);
    d.clearWatchpoints(); ctrl.syncDebugHook(); assert(!d.armed());
    assert(!m.hasReadWatch(0x40) && !m.hasWriteWatch(0x40));
}

static void testBankProfileAcrossSwitchingInstruction()
{
    using B=pom2::DebugBank;
    EmulationController ctrl; auto st=ctrl.lockState();
    auto& m=st.memory(); auto& cpu=st.cpu(); auto& d=ctrl.debugger();
    m.setIIEMode(true); m.setRamWorksBanks(4);
    // Bank 0 runs STA $C073: it switches its own code bank to 1.
    m.memWrite(0xC005,0); m.memWrite(0xC003,0);
    m.memWrite(0x0800,0x8D); m.memWrite(0x0801,0x73); m.memWrite(0x0802,0xC0);
    cpu.setAccumulator(1); cpu.setProgramCounter(0x0800);
    d.setProfiling(true); d.setHistoryEnabled(true); ctrl.syncDebugHook();
    cpu.run(4); assert(m.ramWorksBank()==1);
    assert((d.profile()[0].bank==B{B::Aux,0}));
    assert((d.history()[0].bank==B{B::Aux,0}));
    m.memWrite(0x0800,0xEA); cpu.setProgramCounter(0x0800); cpu.run(2);
    const auto profile=d.profile(); assert(profile.size()==2);
    assert(profile[0].cycles==4 && profile[1].cycles==2);
    assert((profile[1].bank==B{B::Aux,1}));
    assert(d.history().size()==2);
    d.clearForTimeJump(); assert(d.profile().empty() && d.history().empty());
    d.setProfiling(false); d.setHistoryEnabled(false); ctrl.syncDebugHook();
    assert(!cpu.getDebugHook());
}

static void testBankSourceMap()
{
    using B=pom2::DebugBank;
    pom2::DebuggerSymbols symbols; std::string error;
    assert(symbols.load("entry = $0800\nnamespace:name = $0900\n",error));
    pom2::DebugAddress address;
    assert(symbols.resolveLocation("aux127:entry",address));
    assert((address.bank==B{B::Aux,127}) && address.addr==0x0800);
    assert(symbols.load("entry = aux2:$0800\nother = main:$0800\nnamespace:name = $0900\n",error));
    assert(symbols.resolveLocation("entry",address) && (address.bank==B{B::Aux,2}));
    assert(symbols.label(0x0800,{B::Aux,2})=="entry");
    assert(symbols.label(0x0800,{B::Main})=="other");
    assert(symbols.label(0x0800,{B::Aux,3}).empty());
    assert(symbols.resolveLocation("namespace:name",address) && address.addr==0x0900);
    for (const auto* bad:{"aux128:0800","aux-1:0800","lc3:D000","main:10000","unknown:0800"})
        assert(!symbols.resolveLocation(bad,address));
    pom2::DebuggerSourceMap map;
    assert(map.load("# source map\n*:$0800-$0805 \"main code.s\" 8\naux1:$0800-$0802 \"aux.s\" 42\naux2:$0800 \"other.s\" 7\n",error));
    assert(map.size()==3);
    auto* entry=map.lookup({0x0801,{B::Aux,1}});
    assert(entry && entry->line==42 && entry->file=="aux.s");
    entry=map.lookup({0x0803,{B::Aux,1}}); assert(entry && entry->line==8);
    assert(!map.lookup({0x0806,{B::Main}}));
    for (const auto* bad:{"main:0800-07FF file.s 1", "main:0800 file.s 0", "main:0800 file.s 2 junk",
        "aux128:0800 file.s 2", "main:0800 file.s -1", "main:0800 file.s 2\nmain:0800 file.s 3"}) {
        assert(!map.load(bad,error)); assert(!error.empty()); assert(map.size()==3);
        assert(map.lookup({0x0801,{B::Aux,1}})->line==42);
    }
    assert(!map.load(std::string(4*1024*1024+1,'x'),error));
}

static void testBankRomAndExplicitStep()
{
    using B=pom2::DebugBank; using D=pom2::Debugger;
    const auto path=std::filesystem::temp_directory_path()/
        ("pom2-debug-rom-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".rom");
    std::vector<uint8_t> bytes(32768,0xEA);
    bytes[0x3BC0]=0; bytes[0x3BBF]=3; // plain //c
    bytes[0x1000]=0x8D; bytes[0x1001]=0x28; bytes[0x1002]=0xC0; // STA $C028
    { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); assert(file); }
    Memory m; m.setIIEMode(true); assert(m.loadAppleIIRom(path.u8string().c_str(),true));
    std::filesystem::remove(path);
    M6502 cpu(&m); D d; d.setMemory(&m); d.setProfiling(true); d.setHistoryEnabled(true);
    cpu.setDebugHook(&d); cpu.setProgramCounter(0xD000);
    assert((m.debugBank(0xD000)==B{B::Rom,0}));
    cpu.run(4);
    assert((m.debugBank(0xD000)==B{B::Rom,1}));
    assert((d.profile()[0].bank==B{B::Rom,0}));
    cpu.setProgramCounter(0xD000); cpu.run(2);
    assert(d.profile().size()==2 && (d.profile()[1].bank==B{B::Rom,1}));
    // An explicit step uses the same attribution but does not stop before its opcode.
    EmulationController ctrl;
    {
        auto st=ctrl.lockState(); auto& mem=st.memory();
        mem.setIIEMode(true); mem.memWrite(0xC005,0); mem.memWrite(0xC003,0);
        mem.memWrite(0x0800,0x85); mem.memWrite(0x0801,0x40);
        st.cpu().setProgramCounter(0x0800); st.cpu().setAccumulator(0x5A);
        ctrl.debugger().setWatchpoint(0x40,D::Write,{B::Main});
        ctrl.debugger().setProfiling(true); ctrl.syncDebugHook();
    }
    ctrl.debugStepInstruction(); ctrl.tickFrame();
    {
        auto st=ctrl.lockState(); const auto hit=ctrl.debugger().lastHit();
        assert(hit.reason==D::Reason::WatchWrite && hit.pc==0x0800);
        assert((hit.pcBank==B{B::Aux,0}) && hit.bank==B{B::Main});
        assert(st.memory().peekMainRam(0x40)==0x5A);
        assert(ctrl.debugger().profile().size()==1);
    }
    // Flat buses report their physical identity for both reads and writes.
    Memory flat; flat.setTestMode(true); D fd; fd.setMemory(&flat);
    fd.setWatchpoint(0x8000,D::ReadWrite,{B::Flat}); flat.setWatchSink(&fd);
    flat.setWriteWatch(0x8000,true); flat.setReadWatch(0x8000,true);
    flat.memWrite(0x8000,0x77); assert(fd.lastHit().bank==B{B::Flat});
    fd.clearHit(); assert(flat.memRead(0x8000)==0x77 && fd.lastHit().bank==B{B::Flat});
    fd.clearHit(); fd.setWatchpoint(0xD000,D::Read,{B::Flat}); flat.setReadWatch(0xD000,true);
    (void)flat.memRead(0xD000); assert(fd.lastHit().bank==B{B::Flat});
}

static void testWatchBankBeforeExpansionRelease()
{
    using B=pom2::DebugBank; using D=pom2::Debugger;
    struct Card final : SlotPeripheral {
        std::string_view name() const override { return "Debugger bank test"; }
        bool takesC800() const override { return true; }
    };
    Memory m; D d; d.setMemory(&m); m.setWatchSink(&d);
    m.slotBus().plug(5,std::make_unique<Card>());
    (void)m.memRead(0xC500);
    assert((m.debugBank(0xC800)==B{B::Slot,5}));
    d.setWatchpoint(0xCFFF,D::Read,{B::Slot,5}); m.setReadWatch(0xCFFF,true);
    (void)m.memRead(0xCFFF);
    assert(d.lastHit().reason==D::Reason::WatchRead);
    assert((d.lastHit().bank==B{B::Slot,5}));
    assert(m.debugBank(0xC800)==B{B::Io}); // access released its own mapping
    // Resuming a stop after changing the mapping must test the new bank.
    m.setIIEMode(true); d.clearWatchpoints(); d.clearHit();
    d.addBreakpoint(0x0800,{B::Main}); d.addBreakpoint(0x0800,{B::Aux,0});
    assert(d.onInstruction(0x0800));
    m.memWrite(0xC003,0); d.armResumeFrom(0x0800); d.clearHit();
    assert(d.onInstruction(0x0800));
    // A deliberate step consumes the amnesty even if a branch returns to itself.
    d.armResumeFrom(0x0800); d.clearHit(); d.beginStep(0x0800,true);
    assert(d.onInstruction(0x0800));
}
