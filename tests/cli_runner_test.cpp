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

// CliRunner — the Phase-C deferred-action runner against a live machine.
//
// The sibling cli_kiosk_test.cpp pins the PARSER. This one pins what the
// parsed plan then does, which is where two hunt-#4 defects lived:
//
//   #9  `--load` stored every byte through memWrite() — the CPU BUS. A span
//       crossing $C000-$CFFF therefore EXECUTED soft switches on its way
//       past instead of storing bytes: RAMRD/RAMWRT/80STORE/ALTZP flipped,
//       the Disk II motor spun up, and the remainder of the file was then
//       written through whatever memory map the file's own bytes had just
//       built. Below $C000 it also routed to AUX whenever the guest happened
//       to have RAMWRT set.
//
//   #23 `runDeferredActions` never short-circuited, though CliDispatcher.h
//       promises it does. A failed `--load` followed by `--run` jumped the
//       CPU into zero RAM and left the window showing a "running" Apple II.

#include "CliDispatcher.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

fs::path writeBlob(const char* name, const std::vector<uint8_t>& bytes)
{
    const auto p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

pom2::CliAction loadAction(int addr, const fs::path& p)
{
    pom2::CliAction a{};
    a.kind = pom2::CliAction::Kind::Load;
    a.addressI = addr;
    a.pathS = p.string();
    return a;
}

// ── #9: a --load spanning the I/O page must be refused, not executed ────
void testLoadRefusesTheIoPage()
{
    EmulationController ctrl;
    // 64 KiB of $A9 — a byte that, written to $C001/$C003/$C005…, is simply
    // "any write", which is all a soft switch needs.
    const auto blob = writeBlob("pom2_cli_load_full.bin",
                                std::vector<uint8_t>(0x10000, 0xA9));

    // Sanity: the machine starts with the IIe paging switches clear.
    {
        auto st = ctrl.lockState();
        st.memory().setIIEMode(true);
        st.memory().resetSoftSwitches();
        check((st.memory().memRead(0xC013) & 0x80) == 0,
              "RAMRD starts clear");
        check((st.memory().memRead(0xC018) & 0x80) == 0,
              "80STORE starts clear");
    }

    std::vector<pom2::CliAction> acts{loadAction(0x0000, blob)};
    pom2::runDeferredActions(acts, ctrl);

    auto st = ctrl.lockState();
    check((st.memory().memRead(0xC013) & 0x80) == 0,
          "--load across $C000 does not set RAMRD");
    check((st.memory().memRead(0xC018) & 0x80) == 0,
          "--load across $C000 does not set 80STORE");
    // The whole span is refused, so nothing at all landed.
    check(st.memory().peekMainRam(0x0800) == 0x00,
          "and the refused --load stores nothing");

    std::error_code ec;
    fs::remove(blob, ec);
}

// ── #9 (second leg): a --load below $C000 lands in MAIN, not aux ────────
void testLoadBypassesAuxPaging()
{
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        st.memory().setIIEMode(true);
        st.memory().resetSoftSwitches();
        // The guest happens to be writing to aux: RAMWRT on.
        st.memory().memWrite(0xC005, 0x00);   // SETRAMWRT
    }
    const auto blob = writeBlob("pom2_cli_load_small.bin",
                                std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    std::vector<pom2::CliAction> acts{loadAction(0x0800, blob)};
    pom2::runDeferredActions(acts, ctrl);
    {
        auto st = ctrl.lockState();
        check(st.memory().peekMainRam(0x0800) == 0xDE &&
              st.memory().peekMainRam(0x0803) == 0xEF,
              "--load lands in MAIN RAM regardless of RAMWRT");
    }
    std::error_code ec;
    fs::remove(blob, ec);
}

// ── #23: a failed action stops the ones after it ────────────────────────
void testFirstFailureShortCircuits()
{
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        st.cpu().setProgramCounter(0x1234);
    }
    std::vector<pom2::CliAction> acts;
    acts.push_back(loadAction(0x2000,
                              fs::temp_directory_path() / "pom2_no_such_file.bin"));
    pom2::CliAction run{};
    run.kind = pom2::CliAction::Kind::Run;
    run.addressI = 0x2000;
    acts.push_back(run);

    pom2::runDeferredActions(acts, ctrl);

    auto st = ctrl.lockState();
    check(st.cpu().getProgramCounter() == 0x1234,
          "--run after a failed --load never jumps");
    check(ctrl.getMode() != EmulationController::Mode::Running,
          "and the machine is not started into zero RAM");
}

// A clean sequence still runs to the end — the short-circuit must not turn
// into "the first action is the only action".
void testHealthySequenceStillRuns()
{
    EmulationController ctrl;
    const auto blob = writeBlob("pom2_cli_load_ok.bin",
                                std::vector<uint8_t>{0xEA, 0xEA, 0xEA});
    std::vector<pom2::CliAction> acts{loadAction(0x0300, blob)};
    pom2::CliAction run{};
    run.kind = pom2::CliAction::Kind::Run;
    run.addressI = 0x0300;
    acts.push_back(run);

    pom2::runDeferredActions(acts, ctrl);
    {
        auto st = ctrl.lockState();
        check(st.memory().peekMainRam(0x0300) == 0xEA, "a good --load lands");
        check(st.cpu().getProgramCounter() == 0x0300,
              "and the --run after it still fires");
    }
    std::error_code ec;
    fs::remove(blob, ec);
}

// ── #6: --save-tape's path resolver (its only pure piece) ───────────────
void testResolveSaveTapePath()
{
    using pom2::CliSaveTapeFormat;
    check(pom2::resolveSaveTapePath("out", CliSaveTapeFormat::NoHint) == "out.aci",
          "--save-tape defaults to .aci");
    check(pom2::resolveSaveTapePath("out", CliSaveTapeFormat::Wav) == "out.wav",
          "--save-tape-format wav appends .wav");
    check(pom2::resolveSaveTapePath("out.WAV", CliSaveTapeFormat::Aci) == "out.WAV",
          "an explicit extension wins over the format hint");
    check(pom2::resolveSaveTapePath("", CliSaveTapeFormat::Wav).empty(),
          "an empty path stays empty");
}

}  // namespace

int main()
{
    testLoadRefusesTheIoPage();
    testLoadBypassesAuxPaging();
    testFirstFailureShortCircuits();
    testHealthySequenceStillRuns();
    testResolveSaveTapePath();

    if (failures) {
        std::printf("cli_runner: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("cli_runner OK\n");
    return 0;
}
