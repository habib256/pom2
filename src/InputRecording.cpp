// POM2 — GPL-3.0-or-later
#include "InputRecording.h"

#include "AtomicFileReplace.h"
#include "EmulationController.h"
#include "SessionArchive.h"
#include "SessionLoadQueue.h"
#include "SessionMachine.h"
#include "SessionSnapshot.h"
#include "SnapshotIO.h"

#include <cstring>
#include <filesystem>
#include <system_error>

namespace pom2 {
namespace {

constexpr char kMagic[8] = {'P', 'O', 'M', '2', 'I', 'N', 'P', 'T'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxEntries = 50u * 1000u * 1000u;
constexpr uint32_t kMaxText = 1u << 20;

void putU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void putU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putU64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putF64(std::vector<uint8_t>& o, double v) { uint64_t u; std::memcpy(&u, &v, 8); putU64(o, u); }

struct Reader {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    bool ok = true;
    bool need(size_t n) { if (b.size() - at < n || at > b.size()) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return b[at++]; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[at++]) << (8 * i); return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[at++]) << (8 * i); return v; }
    double f64() { const uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return v; }
};

} // namespace

bool encodeInputRecording(const InputRecordingFile& file, std::vector<uint8_t>& out)
{
    out.clear();
    out.insert(out.end(), kMagic, kMagic + 8);
    putU32(out, kVersion);
    putU32(out, static_cast<uint32_t>(file.session.size()));
    out.insert(out.end(), file.session.begin(), file.session.end());
    putU32(out, static_cast<uint32_t>(file.entries.size()));
    for (const TimedInput& t : file.entries) {
        const InputEvent& e = t.event;
        putU64(out, t.cycle);
        putU8(out, static_cast<uint8_t>(e.kind));
        putU32(out, static_cast<uint32_t>(e.a));
        putU32(out, static_cast<uint32_t>(e.b));
        putU32(out, static_cast<uint32_t>(e.c));
        putF64(out, e.hz); putF64(out, e.stockHz); putF64(out, e.machineHz); putF64(out, e.ratio);
        putU64(out, e.hash);
        putU64(out, static_cast<uint64_t>(e.when));
        putU32(out, static_cast<uint32_t>(e.text.size()));
        out.insert(out.end(), e.text.begin(), e.text.end());
    }
    return true;
}

bool decodeInputRecording(const std::vector<uint8_t>& bytes, InputRecordingFile& out,
                          std::string& error)
{
    out = {};
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
        error = "not a POM2 input recording"; return false;
    }
    Reader r{bytes, 8};
    if (r.u32() != kVersion) { error = "an input recording of a version this POM2 does not read"; return false; }
    const uint32_t sessionLen = r.u32();
    if (!r.need(sessionLen) || sessionLen > kMaxSessionBytes) { error = "truncated recording (session)"; return false; }
    out.session.assign(bytes.begin() + static_cast<std::ptrdiff_t>(r.at),
                       bytes.begin() + static_cast<std::ptrdiff_t>(r.at + sessionLen));
    r.at += sessionLen;
    const uint32_t count = r.u32();
    if (!r.ok || count > kMaxEntries) { error = "truncated recording (entries)"; return false; }
    out.entries.reserve(count);
    uint64_t lastCycle = 0;
    for (uint32_t i = 0; i < count; ++i) {
        TimedInput t;
        t.cycle = r.u64();
        const uint8_t kind = r.u8();
        InputEvent& e = t.event;
        e.a = static_cast<int32_t>(r.u32());
        e.b = static_cast<int32_t>(r.u32());
        e.c = static_cast<int32_t>(r.u32());
        e.hz = r.f64(); e.stockHz = r.f64(); e.machineHz = r.f64(); e.ratio = r.f64();
        e.hash = r.u64();
        e.when = static_cast<int64_t>(r.u64());
        const uint32_t len = r.u32();
        if (!r.ok || len > kMaxText || !r.need(len)) { error = "truncated recording (entry)"; return false; }
        e.text.assign(reinterpret_cast<const char*>(bytes.data() + r.at), len);
        r.at += len;
        if (kind < static_cast<uint8_t>(InputEvent::Kind::Key) ||
            kind > static_cast<uint8_t>(InputEvent::Kind::Start)) {
            error = "an input recording with an entry this POM2 does not know"; return false;
        }
        e.kind = static_cast<InputEvent::Kind>(kind);
        if (t.cycle < lastCycle) { error = "an input recording whose entries go back in time"; return false; }
        lastCycle = t.cycle;
        out.entries.push_back(std::move(t));
    }
    if (!r.ok) { error = "truncated recording"; return false; }
    return true;
}

bool startInputRecording(EmulationController& controller, std::string& error)
{
    SessionArchive captured;
    {
        auto state = controller.lockState();
        if (!controller.beginInputRecordingLocked(error)) return false;
        // The same lock hold: the session and the recording's first entry
        // describe one cycle.
        if (!captureSession(captured, state.cpu(), state.memory(), controller.machineId(), error) ||
            !controller.cassette().appendSessionState(captured.cassette, error)) {
            controller.endInputJournalLocked("the machine's session could not be captured");
        }
    }
    if (!error.empty()) {
        std::string why;
        (void)controller.takeInputRecording(why);   // discard it
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!encodeSessionArchive(captured, bytes, error)) {
        {
            auto state = controller.lockState();
            controller.endInputJournalLocked("the machine's session could not be encoded");
        }
        std::string why;
        (void)controller.takeInputRecording(why);
        return false;
    }
    controller.inputRecordingSession() = std::move(bytes);
    return true;
}

bool saveInputRecording(EmulationController& controller, const std::string& path,
                        std::string& why, std::string& error)
{
    InputRecordingFile file;
    file.entries = controller.takeInputRecording(why);
    if (file.entries.empty()) { error = "there is no recording to save"; return false; }
    file.session = std::move(controller.inputRecordingSession());
    controller.inputRecordingSession().clear();
    std::vector<uint8_t> bytes;
    encodeInputRecording(file, bytes);
    std::error_code ec;
    if (!writeFileAtomic(std::filesystem::path(path), bytes.data(), bytes.size(), ec)) {
        error = "the recording could not be written: " + ec.message();
        return false;
    }
    return true;
}

bool playInputRecording(EmulationController& controller, const std::string& path,
                        std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!readSnapshotFileBytes(path, bytes, error)) return false;
    InputRecordingFile file;
    if (!decodeInputRecording(bytes, file, error)) return false;
    SessionArchive session;
    if (!decodeSessionArchive(file.session, session, error)) return false;
    // The session load the user knows (File ▸ Open session): on the
    // desktop it goes through the UI's own queue, which rebuilds the panels.
    const bool loaded = controller.sessionLoadQueue()
        ? controller.sessionLoadQueue()->load(session, error)
        : reconstructSession(controller, session, error);
    if (!loaded) return false;
    {
        auto state = controller.lockState();
        if (!controller.beginInputPlaybackLocked(std::move(file.entries), error)) return false;
    }
    controller.setMode(EmulationController::Mode::Running);
    return true;
}

} // namespace pom2
