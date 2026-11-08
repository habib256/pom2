// POM2 — GPL-3.0-or-later
// Host diagnostics must not flood a release or change the guest handshake.
#include "IWMDevice.h"
#include "DiskImage.h"
#include "Sony35Drive.h"
#include <cassert>
#include <cstdio>
#include <string>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

static void underrun(pom2::IWMDevice& iwm, uint64_t& clock)
{
    iwm.write(0xE, 0); // Q7 low: finish the previous sequence
    iwm.write(0xF, 0xAA); // Q7 high, Q6 high: data loaded
    iwm.tick(clock += 200);
    assert(!(iwm.whd() & 0x40) && "underrun must still clear the write gate");
}

int main()
{
    FILE* capture = std::tmpfile();
    assert(capture);
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    assert(saved >= 0 && dup2(fileno(capture), fileno(stderr)) >= 0);
    pom2::IWMDevice iwm;
    iwm.write(0xD, 0);
    iwm.write(0xF, 3); // byte handshake, eight-bit write state machine
    iwm.write(0x9, 0);
    uint64_t clock = 0;
    for (int i = 0; i < 20; ++i) underrun(iwm, clock);
    DiskImage shadow;
    iwm.setFloppy(&shadow, 0);
    for (int i = 0; i < 20; ++i) underrun(iwm, clock);
    std::fflush(stderr);
    assert(std::ftell(capture) == 0 && "probe and Disk II shadow traffic must be quiet");
    pom2::Sony35Drive sony;
    iwm.setSony35(&sony);
    for (int i = 0; i < 20; ++i) underrun(iwm, clock);
    std::fflush(stderr);
    assert(dup2(saved, fileno(stderr)) >= 0);
    close(saved);
    std::rewind(capture);
    char line[512];
    assert(std::fgets(line, sizeof(line), capture));
    assert(std::string(line).find("write underrun") != std::string::npos);
    assert(!std::fgets(line, sizeof(line), capture) && "retries must produce one diagnostic");
    std::fclose(capture);
    std::puts("IWM: quiet shadow traffic, one real underrun diagnostic, handshake preserved");
}
