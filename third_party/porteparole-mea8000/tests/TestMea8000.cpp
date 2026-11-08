// SPDX-License-Identifier: GPL-2.0-or-later
#include "Mea8000.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static int checks = 0;
static void Check(bool b, const char* what)
{
    ++checks;
    if (!b) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Send(Mea8000& m, uint8_t b)
{
    uint64_t wait = 0;
    while (!m.ReadStatus() && wait < 2000000) { m.AdvanceClocks(4); wait += 4; }
    Check(m.WriteData(b), "byte accepted after finite wait");
}
static void Frame(Mea8000& m, int fd = 0, bool noise = false)
{
    for (auto b : {0x55,0x63,0x97,0x80 | (fd<<5) | (noise ? 16 : 0)}) Send(m, uint8_t(b));
}
static void Start(Mea8000& m) { Send(m, 60); Frame(m); }

int main()
{
    Mea8000 m;
    Check(m.ReadStatus() == 0x80, "power-up ready");
    Send(m, 60);
    Check(m.GetMode() == Mea8000::WaitFirst, "pitch waits for first full frame");
    Check(!m.ReadStatus(), "request turnaround is not always ready");
    m.AdvanceClocks(11); Check(!m.ReadStatus(), "turnaround deadline before");
    m.AdvanceClocks(1); Check(m.ReadStatus() == 0x80, "turnaround deadline at");
    Send(m,0x55); Send(m,0x63); Send(m,0x97);
    Check(!m.Active(), "partial first frame cannot start");
    Send(m,0x80); Check(m.Active(), "first complete frame starts");
    Check(m.FrameSamples() == 64, "short frame native sample count");
    Frame(m); m.AdvanceClocks(12);
    Check(!m.ReadStatus(), "full next frame applies backpressure");
    auto before = m.SaveState();
    Check(!m.WriteData(99) && m.SaveState() == before, "full buffer ignores overflow");
    m.AdvanceClocks(64*480);
    Check(m.ReadStatus() == 0x80 && m.BufferedBytes() == 0, "frame end releases buffer");
    m.WriteCommand(0x10);
    Check(m.GetMode() == Mea8000::Stopped && !m.Active(), "STOP interrupts frame");
    m.AdvanceClocks(12); Send(m,80);
    Check(m.GetMode() == Mea8000::WaitFirst, "new start consumes a pitch byte");
    for (int fd=0;fd<4;++fd)
    {
        m.Reset();Send(m,60);Frame(m,fd);
        uint64_t clocks = uint64_t(64<<fd)*480;
        Check(m.FrameSamples() == (64U<<fd), "FD decodes native length");
        m.AdvanceClocks(clocks-1);
        Check(m.GetMode()==Mea8000::Playing, "frame must not end early");
        m.AdvanceClocks(1);
        Check(m.GetMode()==Mea8000::Slowing, "underflow starts fade at exact 4MHz deadline");
        Check(!m.ReadStatus(), "fade disallows data");
        m.AdvanceClocks(clocks);
        Check(m.GetMode()==Mea8000::Stopped && m.ReadStatus()==0x80, "fade ends ready for fresh pitch");
    }
    m.Reset();m.WriteCommand(0x0c);Start(m);m.AdvanceClocks(480*64*5);
    Check(m.GetMode()==Mea8000::Playing, "CONT repeats without new data");
    m.WriteCommand(0x08);m.AdvanceClocks(480*64*2);
    Check(m.GetMode()==Mea8000::Stopped, "CONT clear permits slow stop");
    m.Reset();Start(m);Frame(m,3,true);m.AdvanceClocks(17000);
    Mea8000 restored;restored.LoadState(m.SaveState());
    std::vector<int16_t> a,b;
    m.AdvanceClocks(400000,&a);restored.AdvanceClocks(400000,&b);
    Check(a==b && m.SaveState()==restored.SaveState(), "snapshot restores noise/phase/filter/fraction exactly");
    m.Reset();Start(m);restored.LoadState(m.SaveState());a.clear();b.clear();
    m.AdvanceClocks(400001,&a);
    for(int i=0;i<400001;++i) restored.AdvanceClocks(1,&b);
    Check(a==b && m.SaveState()==restored.SaveState(), "chunk boundaries do not change sound or state");
    Check(a.size()==4800, "48kHz schedule over a tenth second");
    Check(std::any_of(a.begin(),a.end(),[](int16_t x){return x!=0;}), "voiced frame produces audio");
    auto invalid=m.SaveState();invalid[3]=5;bool rejected=false;
    try { restored.LoadState(invalid); } catch(const std::exception&) {rejected=true;}
    Check(rejected, "invalid buffer count rejected");
    // Regression: a fresh silent message must not resurrect an earlier sound.
    // The v0.1 core produces 32767 here after a whole second of gated silence.
    m.Reset(); Send(m,60);
    for (int f = 0; f < 21; ++f)
        for (int byte : {0xff,0x63,0x97,0x80}) Send(m,uint8_t(byte));
    m.WriteCommand(0x10);
    m.AdvanceClocks(Mea8000::Clock);
    Check(m.ReadStatus()==0x80, "silence settling does not change ready status");
    Send(m,60);
    for (int byte : {0xff,0x63,0x90,0x00}) Send(m,uint8_t(byte));
    a.clear(); m.AdvanceClocks(64*480,&a);
    Check(std::all_of(a.begin(),a.end(),[](int16_t x){return x==0;}),
        "silent frame after STOP and silence must contain no old resonator energy");
    // Split silent time and save/restore halfway through decay must remain exact.
    m.Reset(); Start(m); m.AdvanceClocks(15000); m.WriteCommand(0x10);
    m.AdvanceClocks(10000); restored.LoadState(m.SaveState());
    m.AdvanceClocks(80000); for(int i=0;i<80000;++i) restored.AdvanceClocks(1);
    Check(m.SaveState()==restored.SaveState(), "resonator settling is snapshot/chunk invariant");
    std::printf("PASS: %d MEA8000 checks\n",checks);
}
