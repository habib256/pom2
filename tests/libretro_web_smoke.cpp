// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
// The static web core (LIBRETRO_STATIC, RetroArch's web build) run under
// Node, as RetroArch links it: boot a disk, check the picture, and check that
// a restored state replays the same frames. Built and run by the libretro
// workflow's web job; it links the archive, so it is not a ctest. Bad
// content first: RetroArch's web build cannot catch an exception, so a load
// that fails must fail by returning false, not by ending the page.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "libretro.h"

static std::string sysdir, savedir;
static std::vector<uint8_t> frame; static unsigned fw, fh; static size_t fpitch;
static unsigned pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static void logcb(enum retro_log_level, const char* fmt, ...) { va_list v; va_start(v, fmt); vfprintf(stderr, fmt, v); va_end(v); }
static bool env(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: static_cast<retro_log_callback*>(data)->log = logcb; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *static_cast<const char**>(data) = sysdir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data) = savedir.c_str(); return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixfmt = *static_cast<const unsigned*>(data); return true;
    default: return false;
    }
}
static void vid(const void* d, unsigned w, unsigned h, size_t p) {
    if (!d) return; fw = w; fh = h; fpitch = p;
    frame.assign(static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + p * h);
}
static void aud(int16_t, int16_t) {}
static size_t audb(const int16_t*, size_t f) { return f; }
static void poll() {}
static int16_t st(unsigned, unsigned, unsigned, unsigned) { return 0; }
static uint64_t hashFrame() { uint64_t h = 1469598103934665603ull; for (auto b : frame) { h ^= b; h *= 1099511628211ull; } return h; }
static size_t lit() {
    size_t n = 0; const unsigned bpp = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    for (unsigned y = 0; y < fh; ++y) for (unsigned x = 0; x < fw; ++x) {
        const uint8_t* p = frame.data() + y * fpitch + x * bpp; bool any = false;
        for (unsigned i = 0; i < bpp - (bpp == 4); ++i) any |= p[i] != 0; n += any; }
    return n;
}
int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: web_smoke SYSDIR SAVEDIR DISK\n"); return 2; }
    sysdir = argv[1]; savedir = argv[2];
    retro_set_environment(env); retro_set_video_refresh(vid); retro_set_audio_sample(aud);
    retro_set_audio_sample_batch(audb); retro_set_input_poll(poll); retro_set_input_state(st);
    retro_init();
    retro_system_info si{}; retro_get_system_info(&si);
    std::printf("core %s %s\n", si.library_name, si.library_version);
    // A zip holding no disk (an empty archive: its end record alone) and a
    // playlist naming a disk that is not there.
    const std::string emptyZip = savedir + "/nothing.zip", badList = savedir + "/broken.m3u";
    { std::ofstream z(emptyZip, std::ios::binary); const char eocd[22] = {'P', 'K', 5, 6}; z.write(eocd, sizeof eocd); }
    { std::ofstream m(badList, std::ios::binary); m << "no such disk.dsk\n"; }
    for (const auto& bad : {emptyZip, badList}) {
        retro_game_info badGame{bad.c_str(), nullptr, 0, nullptr};
        if (retro_load_game(&badGame)) { std::printf("FAIL: %s loaded\n", bad.c_str()); return 1; }
        std::printf("refused %s, page alive\n", bad.c_str());
    }
    retro_game_info gi{argv[3], nullptr, 0, nullptr};
    if (!retro_load_game(&gi)) { std::puts("FAIL: load"); return 1; }
    for (int i = 0; i < 600; ++i) retro_run();
    std::printf("frame %ux%u, %zu lit pixels\n", fw, fh, lit());
    if (lit() < 1000) { std::puts("FAIL: the picture is (nearly) black"); return 1; }
    std::vector<uint8_t> state(retro_serialize_size());
    if (!retro_serialize(state.data(), state.size())) { std::puts("FAIL: serialize"); return 1; }
    for (int i = 0; i < 120; ++i) retro_run();
    const uint64_t a = hashFrame();
    if (!retro_unserialize(state.data(), state.size())) { std::puts("FAIL: unserialize"); return 1; }
    for (int i = 0; i < 120; ++i) retro_run();
    const uint64_t b = hashFrame();
    std::printf("state %zu bytes, replay %s\n", state.size(), a == b ? "identical" : "DIFFERENT");
    retro_unload_game(); retro_deinit();
    if (a != b) return 1;
    std::puts("PASS");
    return 0;
}
