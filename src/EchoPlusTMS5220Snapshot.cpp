// POM2 — GPL-3.0-or-later
#include "EchoPlusTMS5220Card.h"
#include "M6502.h"
#include "ByteIO.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
namespace {
using Synth = pom2::ay::ChipSynthState;
template<class F> void visit(Synth& s, F field) {
    field(s.tickPhase);
    field(s.toneCounter[0]);
    field(s.toneCounter[1]);
    field(s.toneCounter[2]);
    field(s.toneOut[0]);
    field(s.toneOut[1]);
    field(s.toneOut[2]);
    field(s.noiseCounter);
    field(s.noiseLfsr);
    field(s.noiseOut);
    field(s.noisePrescale);
    field(s.envCounter);
    field(s.envStep);
    field(s.envAttack);
    field(s.envHold);
    field(s.envAlternate);
    field(s.envHolding);
    field(s.lastShape);
    field(s.envRetrigger);
    field(s.lastSeenResetCount);
    field(s.lastSeenEnvWriteCount);
}
void putDouble(std::vector<uint8_t>& bytes, double value) {
    uint64_t bits; std::memcpy(&bits, &value, 8); pom2::byteio::putU64(bytes, bits);
}
double getDouble(pom2::byteio::Reader& r) {
    const auto bits = r.u64(); double value; std::memcpy(&value, &bits, 8); return value;
}
void pack(std::vector<uint8_t>& out, const pom2::Tms5220& speech, const pom2::Via6522& via,
          const pom2::Ay3_8910* ay, const Synth* synth, double viaPhase,
          double speechPhase, double samplePhase, int16_t lastSpeech, uint8_t lastWrite, uint32_t rate) {
    out.insert(out.end(), {'E','T','S',2});
    speech.appendSnapshot(out); via.appendSnapshot(out);
    pom2::byteio::putU8(out, via.t1Pb7); pom2::byteio::putU8(out, via.latchA);
    for (int i = 0; i < 2; ++i) ay[i].appendSnapshot(out);
    for (int i = 0; i < 2; ++i) {
        auto copy = synth[i];
        visit(copy, [&](auto& field) {
            using T = std::decay_t<decltype(field)>;
            uint32_t value;
            if constexpr (std::is_same_v<T,float>) std::memcpy(&value, &field, 4);
            else value = uint32_t(field);
            pom2::byteio::putU32(out, value);
        });
    }
    putDouble(out, viaPhase); putDouble(out, speechPhase); putDouble(out, samplePhase);
    pom2::byteio::putU16(out, uint16_t(lastSpeech));
    pom2::byteio::putU8(out, lastWrite); pom2::byteio::putU32(out, rate);
}
bool valid(const Synth& s) {
    if (!std::isfinite(s.tickPhase) || s.tickPhase < 0 || s.tickPhase >= 1 ||
        s.noiseLfsr > 0x1FFFF || s.noiseOut > 1 || s.noisePrescale > 1 ||
        s.envStep < -1 || s.envStep > 15 || s.lastShape < -1 || s.lastShape > 15 ||
        (s.envAttack != 0 && s.envAttack != 15) || s.envHold > 1 || s.envAlternate > 15 || s.envHolding > 1) return false;
    for (auto bit : s.toneOut) if (bit > 1) return false;
    return true;
}
constexpr size_t coreSize = 170;
constexpr size_t snapshotSize = 4 + coreSize + pom2::Via6522::kSnapshotBytes + 2 +
    2 * pom2::Ay3_8910::kSnapshotBytes + 2 * 84 + 24 + 2 + 1 + 4;
}
void EchoPlusTMS5220Card::appendSnapshotState(std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lock(mtx_);
    pack(out, speech_, via_, ay_, synth_, viaPhase_, speechPhase_, samplePhase_, lastSpeech_, lastWrite_, rate_);
}
void EchoPlusTMS5220Card::loadSnapshotState(const uint8_t* data, size_t size) {
    if (!data || size < snapshotSize || data[0] != 'E' || data[1] != 'T' || data[2] != 'S' || data[3] != 2) return;
    size = snapshotSize; // Future extension tails do not alter this versioned prefix.
    pom2::byteio::Reader r(data, size); r.pos = 4;
    pom2::Tms5220 speech;
    if (!speech.loadSnapshot(data + r.pos, coreSize)) return;
    r.pos += coreSize;
    pom2::Via6522 via; via.loadSnapshot(data + r.pos, pom2::Via6522::kSnapshotBytes); r.pos += pom2::Via6522::kSnapshotBytes;
    via.t1Pb7 = r.u8() != 0; via.latchA = r.u8();
    if (via.t1Counter < 0 || via.t1Counter > 65537 || via.t2Counter < 0 || via.t2Counter > 65537) return;
    pom2::Ay3_8910 ay[2];
    for (auto& chip : ay) { chip.loadSnapshot(data + r.pos); r.pos += pom2::Ay3_8910::kSnapshotBytes; }
    Synth synth[2];
    for (auto& chip : synth) {
        visit(chip, [&](auto& field) {
            using T = std::decay_t<decltype(field)>;
            const auto value = r.u32();
            if constexpr (std::is_same_v<T,float>) std::memcpy(&field, &value, 4);
            else field = T(value);
        });
        if (!valid(chip)) return;
    }
    const double viaPhase = getDouble(r), speechPhase = getDouble(r), samplePhase = getDouble(r);
    for (double phase : {viaPhase, speechPhase, samplePhase}) if (!std::isfinite(phase) || phase < 0 || phase >= 1) return;
    const int16_t lastSpeech = int16_t(r.u16()); const uint8_t lastWrite = r.u8(); const uint32_t rate = r.u32();
    if (rate < 8000 || rate > 192000) return;
    std::vector<uint8_t> canonical;
    pack(canonical, speech, via, ay, synth, viaPhase, speechPhase, samplePhase, lastSpeech, lastWrite, rate);
    if (canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), data)) return;
    std::lock_guard<std::mutex> lock(mtx_);
    speech_ = speech; via_ = via;
    for (int i = 0; i < 2; ++i) { ay_[i] = ay[i]; synth_[i] = synth[i]; }
    viaPhase_ = viaPhase; speechPhase_ = speechPhase;
    samplePhase_ = rate_ == rate ? samplePhase : 0;
    lastSpeech_ = lastSpeech; lastWrite_ = lastWrite;
    pcm_.clear(); lastCycle_ = cpu_ ? cpu_->getCycleCountNow() : 0;
    assertIrq(via_.irqOut());
}
