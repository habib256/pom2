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

// Diagnostic (NOT ctest): calibrate the OpenEmulator-style composite demod
// phase against the MAME LUT reference (ColorNTSC), using OE's actual decode
// (libemulation OpenGLCanvas.cpp): chroma = composite·(sin φ, cos φ) → U,V,
// then the YUV→RGB decoder matrix
//   R = Y + 1.139883·V ; G = Y − 0.394642·U − 0.580622·V ; B = Y + 2.032062·U.
// Solid HGR colours use ALTERNATING bytes (7-bit/2-dot misalignment):
//   VIOLET $55/$2A · GREEN $2A/$55 · BLUE $D5/$AA · ORANGE $AA/$D5.
//
//   build/artifact_probe

#include "Apple2Display.h"
#include "AppleWinNtsc.h"
#include "Memory.h"

#include <cmath>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
uint8_t R(uint32_t c){return c&0xFF;} uint8_t G(uint32_t c){return (c>>8)&0xFF;} uint8_t B(uint32_t c){return (c>>16)&0xFF;}
void wr(Memory& m,int y,int col,uint8_t v){ uint16_t a=0x2000+0x400*(y&7)+0x80*((y>>3)&7)+0x28*(y>>6); m.memWrite(uint16_t(a+col),v); }
struct Named{const char* n;int r,g,b;};
const Named kRef[]={{"black",0,0,0},{"white",255,255,255},
    {"VIOLET",0xe6,0x28,0xff},{"GREEN",0x19,0xd7,0x00},{"BLUE",0x19,0x90,0xff},{"ORANGE",0xe6,0x6f,0x00}};
const char* classify(int r,int g,int b){ long best=1L<<60; const char* bn="?";
    for(auto&k:kRef){ long d=(long)(r-k.r)*(r-k.r)+(g-k.g)*(g-k.g)+(b-k.b)*(b-k.b); if(d<best){best=d;bn=k.n;} } return bn; }
const char* avgHue(const Apple2Display& d,int y){ long r=0,g=0,bl=0;int n=0;const uint32_t* p=d.pixels();int w=d.width();
    for(int x=112;x<168;++x){uint32_t c=p[y*w+x];r+=R(c);g+=G(c);bl+=B(c);++n;} return classify(int(r/n),int(g/n),int(bl/n)); }
// OpenEmulator YUV decode at phase offset phi0.
const char* avgDemodOE(const uint8_t* sig,int sw,int /*sh*/,int line,float phi0){
    const float PI=3.14159265358979f; const float sgY=0.8f,sgC=1.75f; const int N=8;
    auto S=[&](int x){return (x<0||x>=sw)?0.0f:sig[line*sw+x]/255.0f;};
    long rr=0,gg=0,bb=0;int cnt=0;
    for(int X=240;X<340;++X){ float Y=0,U=0,V=0,wY=0,wC=0;
        for(int i=-N;i<=N;++i){ float s=S(X+i); float d=float(i);
            float gy=std::exp(-0.5f*d*d/(sgY*sgY)), gc=std::exp(-0.5f*d*d/(sgC*sgC));
            float ph=PI*0.5f*(X+i)+phi0;
            Y+=s*gy; U+=s*std::sin(ph)*gc*2.0f; V+=s*std::cos(ph)*gc*2.0f; wY+=gy; wC+=gc; }
        Y/=wY;U/=wC;V/=wC;
        auto cl=[](float v){return v<0?0.f:(v>1?1.f:v);};
        rr+=int(cl(Y+1.139883f*V)*255); gg+=int(cl(Y-0.394642f*U-0.580622f*V)*255); bb+=int(cl(Y+2.032062f*U)*255); ++cnt; }
    return classify(int(rr/cnt),int(gg/cnt),int(bb/cnt));
}
}

int main(){
    Memory mem; Apple2Display disp;
    mem.memRead(0xC050); mem.memRead(0xC057); mem.memRead(0xC054);
    struct Fld{const char* want; uint8_t even,odd;};
    const Fld F[]={{"VIOLET",0x55,0x2A},{"GREEN",0x2A,0x55},{"BLUE",0xD5,0xAA},{"ORANGE",0xAA,0xD5}};
    // 4 horizontal bands of 48 rows each (violet/green/blue/orange) so the
    // dumped signal renders as visible colour bands through the real shader.
    for(int y=0;y<192;++y){ int f=y/48; for(int c=0;c<40;++c) wr(mem,y,c,(c&1)?F[f].odd:F[f].even); }

    const int bandRow[4]={24,72,120,168};
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC); disp.render(mem);
    std::string ref[4]; int nR[4],nG[4],nB[4]; std::printf("NTSC reference:");
    for(int f=0;f<4;++f){ ref[f]=avgHue(disp,bandRow[f]);
        { long r=0,g=0,b=0;int n=0;const uint32_t* p=disp.pixels();int w=disp.width();
          for(int x=112;x<168;++x){uint32_t c=p[bandRow[f]*w+x];r+=R(c);g+=G(c);b+=B(c);++n;} nR[f]=int(r/n);nG[f]=int(g/n);nB[f]=int(b/n);} }
    for(int f=0;f<4;++f) std::printf(" %s=%s", F[f].want, ref[f].c_str());
    std::printf("\n\nOE YUV decode, phase sweep (16 steps of pi/8):\n");

    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE); disp.render(mem);
    const uint8_t* sig=disp.signal(); int sw=disp.signalWidth(), sh=disp.signalHeight();
    const float PI=3.14159265358979f;
    for(int k=0;k<16;++k){ float phi=PI*k/8.0f; int ok=0; std::string d;
        for(int f=0;f<4;++f){ const char* h=avgDemodOE(sig,sw,sh,bandRow[f],phi); if(ref[f]==h)++ok; d+=" ";d+=h; }
        std::printf("  phi=%4.2f*pi  %s  %d/4%s\n", k/8.0, d.c_str(), ok, ok==4?"  <== MATCH":""); }

    // Numeric RGB per band: NTSC LUT framebuffer vs OE-YUV demod (phi=0).
    // (sig is still the OE signal here — we have NOT switched modes back.)
    std::printf("\n            NTSC LUT (ref)      OE-YUV demod (phi=0)\n");
    auto demRGB=[&](int line,int&R_,int&G_,int&B_){ const float PIl=3.14159265358979f; const float sgY=0.8f,sgC=1.75f; const int N=8;
        auto S=[&](int x){return (x<0||x>=sw)?0.0f:sig[line*sw+x]/255.0f;}; long rr=0,gg=0,bb=0;int cnt=0;
        for(int X=240;X<340;++X){ float Y=0,U=0,V=0,wY=0,wC=0; for(int i=-N;i<=N;++i){ float s=S(X+i),d=float(i);
            float gy=std::exp(-0.5f*d*d/(sgY*sgY)),gc=std::exp(-0.5f*d*d/(sgC*sgC)); float ph=PIl*0.5f*(X+i);
            Y+=s*gy;U+=s*std::sin(ph)*gc*2.0f;V+=s*std::cos(ph)*gc*2.0f;wY+=gy;wC+=gc;} Y/=wY;U/=wC;V/=wC;
            auto cl=[](float v){return v<0?0.f:(v>1?1.f:v);};
            rr+=int(cl(Y+1.139883f*V)*255);gg+=int(cl(Y-0.394642f*U-0.580622f*V)*255);bb+=int(cl(Y+2.032062f*U)*255);++cnt;}
        R_=int(rr/cnt);G_=int(gg/cnt);B_=int(bb/cnt); };
    for(int f=0;f<4;++f){ int dr,dg,db; demRGB(bandRow[f],dr,dg,db);
        std::printf("  %-7s  (%3d,%3d,%3d)      (%3d,%3d,%3d)\n",F[f].want,nR[f],nG[f],nB[f],dr,dg,db); }

    // ── AppleWin YUV decode, phase sweep ─────────────────────────────────
    std::printf("\nAppleWin YUV decode, phase sweep (g_phaseShift):\n");
    std::vector<uint32_t> awBuf((size_t)sw*sh);
    for(int k=0;k<16;++k){ float phi=PI*k/8.0f;
        pom2::AppleWinNtsc::rebuildForPhase(phi);
        pom2::AppleWinNtsc::renderFrame(sig, awBuf.data(), sw, sh,
                                        pom2::AppleWinNtsc::SubMode::Monitor);
        int ok=0; std::string d;
        for(int f=0;f<4;++f){ long r=0,g=0,b=0;int n=0; const uint32_t* p=awBuf.data();
            for(int x=240;x<340;++x){uint32_t c=p[bandRow[f]*sw+x];r+=R(c);g+=G(c);b+=B(c);++n;}
            const char* h=classify(int(r/n),int(g/n),int(b/n)); if(ref[f]==h)++ok; d+=" ";d+=h; }
        std::printf("  phi=%4.2f*pi  %s  %d/4%s\n", k/8.0, d.c_str(), ok, ok==4?"  <== MATCH":""); }

    // Dump the 4-band composite signal for the real-GLSL viewer (oe_signal_view).
    { std::FILE* fp=std::fopen("/tmp/test4_signal.bin","wb");
      if(fp){ std::fwrite(sig,1,(size_t)sw*sh,fp); std::fclose(fp);
              std::printf("\nwrote /tmp/test4_signal.bin (%dx%d) — bands violet/green/blue/orange\n",sw,sh); } }
    return 0;
}
