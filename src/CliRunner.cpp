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
void runPasteFile(const std::string& path, EmulationController& emu)
{
    std::ifstream f(path);
    if (!f) {
        pom2::log().error("CLI", "--paste cannot open " + path);
        return;
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
}

void runLoad(const CliAction& a, EmulationController& emu)
{
    std::ifstream f(a.pathS, std::ios::binary);
    if (!f) {
        pom2::log().error("CLI", "--load cannot open " + a.pathS);
        return;
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
        return;
    }
    if (bytes.empty()) {
        pom2::log().error("CLI", "--load file is empty: " + a.pathS);
        return;
    }
    if (static_cast<size_t>(a.addressI) + bytes.size() > 0x10000) {
        pom2::log().error("CLI", "--load overflows $FFFF");
        return;
    }
    {
        auto st = emu.lockState();
        for (size_t i = 0; i < bytes.size(); ++i) {
            st.memory().memWrite(static_cast<uint16_t>(a.addressI + i), bytes[i]);
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "--load wrote %zu bytes at $%04X (from %s)",
                  bytes.size(), a.addressI, a.pathS.c_str());
    pom2::log().info("CLI", buf);
}

} // namespace

void runDeferredActions(const std::vector<CliAction>& actions,
                        EmulationController& emu)
{
    for (const CliAction& a : actions) {
        switch (a.kind) {
            case CliAction::Kind::Load:
                runLoad(a, emu);
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
                runPasteFile(a.pathS, emu);
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
                    break;
                }
                std::error_code ec;
                if (!pom2::writeFileAtomic(a.pathS, blob.data(), blob.size(),
                                           ec)) {
                    pom2::log().error("CLI",
                        "--snapshot-save: write failed for " + a.pathS +
                        ": " + ec.message());
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
                        break;
                    }
                    blob.assign(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
                    if (!in && !in.eof()) {
                        pom2::log().error("CLI",
                            "--snapshot-load: read error on " + a.pathS);
                        break;
                    }
                }
                pom2::SnapshotReader r(blob.data(), blob.size());
                if (!r.good()) {
                    pom2::log().error("CLI",
                        "--snapshot-load: cannot read " + a.pathS +
                        ": " + r.error());
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
