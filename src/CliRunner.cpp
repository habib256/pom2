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

// CliRunner.cpp — Phase-C deferred-action runner. Split out of
// CliDispatcher.cpp (2026-05-23) so the *parser* (`parseCli`) stays free of
// any EmulationController dependency and can be unit-tested without linking
// the whole emulation core. This TU is the only half that touches the live
// machine, so it carries the EmulationController include.

#include "CliDispatcher.h"

#include "AtomicFileReplace.h"
#include "EmulationController.h"
#include "MachineSnapshot.h"
#include "SnapshotIO.h"
#include "SystemProfile.h"
#include "Logger.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pom2 {
namespace {

/// Read a file and feed it through Memory::pasteText, which normalises
/// line-endings (\r\n / \r / \n → CR) and drains via the strobe-aware
/// queue (one byte per $C010 clear). Capped at Memory::kPasteMaxChars.
bool runPasteFile(const std::string& path, EmulationController& emu)
{
    std::ifstream f(path);
    if (!f) {
        pom2::log().error("CLI", "--paste cannot open " + path);
        return false;
    }
    // Read at most the paste-queue cap so a huge/unbounded source (e.g.
    // /dev/zero) can't exhaust memory before pasteText() applies its cap —
    // pasteText discards anything beyond Memory::kPasteMaxChars anyway.
    static constexpr size_t kMaxPaste = 4096;  // == Memory::kPasteMaxChars
    char buf[kMaxPaste];
    f.read(buf, sizeof(buf));
    const std::string content(buf, static_cast<size_t>(f.gcount()));
    // Unlocked on purpose: pasteText takes `Memory::kbMutex`, not the state
    // lock (see Memory.cpp:1147). Every other access here goes through
    // `emu.lockState()`.
    const size_t queued = emu.memory().pasteText(content);
    pom2::log().info("CLI", "--paste queued " + std::to_string(queued) +
                            " chars from " + path);
    return true;
}

bool runLoad(const CliAction& a, EmulationController& emu)
{
    std::ifstream f(a.pathS, std::ios::binary);
    if (!f) {
        pom2::log().error("CLI", "--load cannot open " + a.pathS);
        return false;
    }
    // Reject oversized sources before allocating, so an unbounded file (e.g.
    // /dev/zero) or a multi-GB file can't exhaust memory. A 6502 image can be
    // at most 64 KiB; the address+size>0x10000 check below still applies.
    std::vector<uint8_t> bytes(0x10001);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(f.gcount()));
    if (bytes.size() > 0x10000) {
        pom2::log().error("CLI", "--load file exceeds 64 KiB: " + a.pathS);
        return false;
    }
    if (bytes.empty()) {
        pom2::log().error("CLI", "--load file is empty: " + a.pathS);
        return false;
    }
    const size_t first = static_cast<size_t>(a.addressI);
    const size_t last  = first + bytes.size();          // exclusive
    if (last > 0x10000) {
        pom2::log().error("CLI", "--load overflows $FFFF");
        return false;
    }
    // ── $C000-$CFFF is the I/O page, not memory ─────────────────────────
    // This used to store every byte through memWrite(), which is the CPU BUS.
    // A span crossing the I/O page therefore EXECUTED soft switches on its
    // way past: `--load 0:image` of a 64 KiB dump flipped RAMRD/RAMWRT/
    // 80STORE/ALTZP, spun up the Disk II motor and stepped its head, and
    // then wrote the rest of the file through whatever memory map it had
    // just built. There is no honest way to "load" a byte into a soft
    // switch, so refuse the span instead of half-doing it.
    if (first < 0xD000 && last > 0xC000) {
        char why[160];
        std::snprintf(why, sizeof(why),
                      "--load refused: $%04X-$%04X crosses the $C000-$CFFF I/O "
                      "page (writing there executes soft switches, it does not "
                      "store bytes)",
                      static_cast<unsigned>(first),
                      static_cast<unsigned>(last - 1));
        pom2::log().error("CLI", why);
        return false;
    }
    {
        auto st = emu.lockState();
        for (size_t i = 0; i < bytes.size(); ++i) {
            const uint16_t addr = static_cast<uint16_t>(first + i);
            // Below $C000 the store must bypass the bus too — memWrite there
            // routes to aux under 80STORE/RAMWRT, so `--load` landed in the
            // wrong bank depending on what the guest happened to have set.
            // Above $CFFF the bus write is the right one: it is the language
            // card's own paging, which is what a --load at $D000 means.
            if (addr < 0xC000) st.memory().writeRamUnchecked(addr, bytes[i]);
            else               st.memory().memWrite(addr, bytes[i]);
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "--load wrote %zu bytes at $%04X (from %s)",
                  bytes.size(), a.addressI, a.pathS.c_str());
    pom2::log().info("CLI", buf);
    return true;
}

} // namespace

void runDeferredActions(const std::vector<CliAction>& actions,
                        EmulationController& emu)
{
    // ── The first failure ends the sequence ─────────────────────────────
    // CliDispatcher.h has always said "the first fatal error short-circuits
    // the rest"; nothing implemented it. `--load 2000:missing.bin --run 2000`
    // logged "cannot open" and then jumped the CPU into whatever was at
    // $2000 — on a cold machine, zeroed RAM, i.e. a BRK storm — while the
    // user is looking at a window that shows a running Apple II. Every arm
    // below reports its own outcome; `ok` is what stops the loop.
    bool ok = true;
    for (const CliAction& a : actions) {
        if (!ok) {
            pom2::log().warn("CLI",
                "skipping the remaining deferred action(s) after a failure");
            break;
        }
        switch (a.kind) {
            case CliAction::Kind::Load:
                ok = runLoad(a, emu);
                break;
            case CliAction::Kind::Run: {
                auto st = emu.lockState();
                st.cpu().setProgramCounter(static_cast<uint16_t>(a.addressI));
                emu.setMode(EmulationController::Mode::Running);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "--run jumped to $%04X", a.addressI);
                pom2::log().info("CLI", buf);
                break;
            }
            case CliAction::Kind::Paste:
                ok = runPasteFile(a.pathS, emu);
                break;
            case CliAction::Kind::Step: {
                emu.setMode(EmulationController::Mode::Stopped);
                emu.requestStep(a.countI);   // queues N steps (counter, not coalesced)
                pom2::log().info("CLI", "--step requested " + std::to_string(a.countI));
                break;
            }
            case CliAction::Kind::TraceBrk:
                pom2::log().info("CLI", "--trace-brk: not yet wired in M6502");
                break;
            case CliAction::Kind::PlayTape:
                emu.playTape();
                pom2::log().info("CLI", "--play: tape rolling");
                break;
            case CliAction::Kind::RecTape:
                emu.armRecording();   // locked wrapper — avoids racing the CPU worker
                pom2::log().info("CLI", "--rec: cassette capture armed");
                break;
            case CliAction::Kind::RewindTape:
                emu.rewindTape();
                pom2::log().info("CLI", "--rewind: tape rewound");
                break;
            case CliAction::Kind::SnapshotSave: {
                // Same machinery as the AI server's POST /snapshot/save
                // (AiControlServer::handleSnapshotSave): CPU + 64 KiB RAM
                // + MEX, captured under the state lock. These two actions
                // were parser-accepted and documented in --help but were
                // silent no-ops for a while ("not yet wired").
                //
                // Serialise under the lock, WRITE outside it. The capture is
                // RAM-only and takes microseconds; the file write and its two
                // fsyncs are what would otherwise hold `stateMutex` (30 ms for
                // 4 MB on the measured host), freezing the CPU worker and the
                // window mid-frame.
                std::vector<uint8_t> blob;
                bool captured = false;
                {
                    pom2::SnapshotWriter w(blob, emu.machineId());
                    auto st = emu.lockState();
                    pom2::captureMachineState(w, st.cpu(), st.memory());
                    captured = w.finish();
                }
                if (!captured) {
                    pom2::log().error("CLI",
                        "--snapshot-save: capture failed for " + a.pathS);
                    ok = false;
                    break;
                }
                std::error_code ec;
                if (!pom2::writeFileAtomic(a.pathS, blob.data(), blob.size(),
                                           ec)) {
                    pom2::log().error("CLI",
                        "--snapshot-save: write failed for " + a.pathS +
                        ": " + ec.message());
                    ok = false;
                    break;
                }
                pom2::log().info("CLI", "--snapshot-save: wrote " + a.pathS);
                break;
            }
            case CliAction::Kind::SnapshotLoad: {
                // Read the whole file BEFORE taking the lock, then parse from
                // memory. The file-backed reader pulls its bytes lazily from
                // inside restoreMachineState(), so constructing it here and
                // restoring under the lock would still put the disk read
                // (64 KiB MEM + a MEX section capped at 16 MiB) inside the
                // critical section.
                std::vector<uint8_t> blob;
                {
                    std::ifstream in(a.pathS, std::ios::binary);
                    if (!in) {
                        pom2::log().error("CLI",
                            "--snapshot-load: cannot open " + a.pathS);
                        ok = false;
                        break;
                    }
                    blob.assign(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
                    if (!in && !in.eof()) {
                        pom2::log().error("CLI",
                            "--snapshot-load: read error on " + a.pathS);
                        ok = false;
                        break;
                    }
                }
                pom2::SnapshotReader r(blob.data(), blob.size());
                if (!r.good()) {
                    pom2::log().error("CLI",
                        "--snapshot-load: cannot read " + a.pathS +
                        ": " + r.error());
                    ok = false;
                    break;
                }
                // Machine identity BEFORE any state is touched. CPU, MEM and
                // MEX all restore unconditionally, so a snapshot taken on
                // another Apple lands PC and 64 KB of RAM against a different
                // ROM and memory map — a freeze or silent wrong execution
                // with no diagnostic. A snapshot file is also the one thing
                // users hand to each other, so the mismatch is not
                // hypothetical. Legacy files (identity 0) still load: they
                // predate the field and refusing them would break every
                // snapshot taken before this build.
                const std::uint32_t want = emu.machineId();
                if (want != 0 && r.machineId() != 0 &&
                    r.machineId() != want) {
                    const std::string_view from =
                        pom2::profileNameForMachineId(r.machineId());
                    pom2::log().error("CLI",
                        "--snapshot-load: refused " + a.pathS + " — taken on " +
                        (from.empty() ? std::string("another machine")
                                      : std::string(from)) +
                        ", this session is " +
                        std::string(pom2::profileNameForMachineId(want)));
                    ok = false;
                    break;
                }
                auto st = emu.lockState();
                const auto res =
                    pom2::restoreMachineState(r, st.cpu(), st.memory());
                // Backwards cycleCounter jump: flush the speaker (its
                // cursor only snaps forward — audio would stay muted until
                // the counter re-passes it) and drop the stale rewind ring.
                if (!res.ok) {
                    pom2::log().error("CLI", "--snapshot-load: " + res.error);
                    ok = false;
                    break;
                }
                // Successful load abandons the former timeline. A failed one
                // is transactionally rolled back, so preserve audio + rewind.
                // One call for every free-running audio device — the same
                // one the rewind transport and the AI server use.
                emu.noteTimeJump();
                emu.rewind().clear();
                pom2::log().info("CLI",
                    "--snapshot-load: restored " + a.pathS);
                break;
            }
        }
    }
}

} // namespace pom2
