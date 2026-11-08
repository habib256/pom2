// SPDX-License-Identifier: GPL-2.0-or-later
#include "Mea8000.h"
static Mea8000 chip;
static std::vector<int16_t> audio;
extern "C" {
__declspec(dllexport) void pp_reset() {chip.Reset();audio.clear();}
__declspec(dllexport) void pp_advance(uint32_t clocks) {chip.AdvanceClocks(clocks,&audio);}
__declspec(dllexport) int pp_read() {return chip.ReadStatus();}
__declspec(dllexport) int pp_write(int reg,int value) {
 if(reg&1){chip.WriteCommand(uint8_t(value));return 1;}
 return chip.WriteData(uint8_t(value));
}
__declspec(dllexport) size_t pp_size() {return audio.size();}
__declspec(dllexport) void pp_copy(int16_t* out) {std::copy(audio.begin(),audio.end(),out);}
__declspec(dllexport) int pp_mode() {return int(chip.GetMode());}
}
