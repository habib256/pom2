// POM2 Apple II Emulator — GPL-3.0-or-later
//
// diskii_step_rewind — a head step still inside its 256-cycle cancel window
// (DiskIICard::seekPhaseW) belongs to the timeline it was made in. A rewind
// or session load used to keep it, and the next phase access confirmed it:
// a head-step click stamped with a cycle from the abandoned future.

#include "DiskIICard.h"
#include "FloppySoundSink.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct RecordingSink : FloppySoundSink {
    std::vector<uint64_t> steps;
    void motor(bool, bool) override {}
    void step(int, uint64_t emuCycles) override { steps.push_back(emuCycles); }
    void click() override {}
};

int failures = 0;
void expect(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int main()
{
    DiskIICard card(6);
    RecordingSink sink;
    card.setFloppySound(&sink);
    (void)card.deviceSelectRead(0x9);           // motor on
    card.advanceCycles(1000);

    std::vector<uint8_t> past;
    card.appendSnapshotState(past);             // the point we rewind to

    card.advanceCycles(50000);
    (void)card.deviceSelectRead(0x3);           // PH1 on: a step, pending
    expect(sink.steps.empty(), "the step waits for its cancel window");

    card.loadSnapshotState(past.data(), past.size());   // rewind
    card.advanceCycles(1000);
    (void)card.deviceSelectRead(0x2);           // next phase access
    (void)card.deviceSelectRead(0x5);
    (void)card.deviceSelectRead(0x4);
    for (uint64_t c : sink.steps) std::printf("  click stamped at %llu\n",
                                              static_cast<unsigned long long>(c));
    bool fromFuture = false;
    for (uint64_t c : sink.steps) fromFuture |= c >= 50000;
    expect(!fromFuture, "no click from the abandoned timeline after a rewind");

    std::printf("diskii_step_rewind: %d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
