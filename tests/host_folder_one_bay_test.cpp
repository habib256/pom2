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

// host_folder_one_bay — one folder, one bay (bug hunt 2026-11-01 round 2,
// MED-1). Two bays serving one host tree each kept their own snapshot: every
// flush saw the other's output as host-newer and copied it to
// `.pom2-conflicts/` for ever, and two decodes interleaved their writes. The
// refusal compares the DIRECTORY, not the bay's label: "/x/f" and "/x/f/" are
// one folder, and a second card in another slot is a second bay. The
// file-image sibling of this rule is pinned in `storage_coordinator`.

#include "EmulationController.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "StorageCoordinator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok) {
        std::cout << "FAIL: " << what << "\n";
        ++failures;
    }
}

}  // namespace

int main()
{
    const auto folder = std::filesystem::temp_directory_path() /
                        "pom2_host_folder_one_bay";
    const auto other = std::filesystem::temp_directory_path() /
                       "pom2_host_folder_one_bay_other";
    std::error_code ec;
    for (const auto& d : {folder, other}) {
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        std::ofstream(d / "a.txt", std::ios::binary) << "a";
    }
    EmulationController ctl;
    // Read-only: the commands below persist, and Settings::save() would
    // write the developer's real state.cfg (storage_coordinator, bug hunt #11).
    pom2::Settings st;
    st.setReadOnly(true);
    pom2::StorageCoordinator sc;
    {
        auto state = ctl.lockState();
        state.memory().slotBus().plug(5, std::make_unique<ProDOSHardDiskCard>(5));
        state.memory().slotBus().plug(4, std::make_unique<pom2::SmartPortCard>(4));
    }
    auto synth = [](const std::filesystem::path& d) {
        std::vector<std::uint8_t> bytes;
        const auto built = pom2::buildVolumeFromFolder(d.string(), "HOST", bytes);
        expect(built.ok, "the folder synthesises: " + built.error);
        return bytes;
    };
    auto mountAt = [&](int slot, int bay, const std::string& dir,
                       const std::filesystem::path& from) {
        return sc.mountBlockBytes(ctl, st, slot, synth(from),
                                  "[host folder] " + dir, dir, bay);
    };
    const std::string plain = folder.string();
    const std::string slashed = plain + "/";

    expect(mountAt(5, 0, plain, folder).ok, "the first host-folder mount");
    const auto twin = mountAt(5, 1, slashed, folder);
    expect(!twin.ok, "one folder served into a second drive (\"/f\" then \"/f/\")");
    expect(twin.error.find(plain) != std::string::npos,
           "the refusal names the folder (" + twin.error + ")");
    expect(!mountAt(4, 1, plain, folder).ok, "one folder served into a second card");
    {
        auto state = ctl.lockState();
        auto* hdv = dynamic_cast<ProDOSHardDiskCard*>(
            state.memory().slotBus().peripheral(5));
        expect(hdv && hdv->bayInfo(0).loaded && !hdv->bayInfo(1).loaded,
               "a refused twin leaves both drives as they were");
        expect(hdv && hdv->bayInfo(0).hostFolder == plain,
               "bayInfo reports the folder, not the label");
    }
    // The same bay may take its own folder back (the re-mount), and a
    // different folder may go anywhere.
    expect(mountAt(5, 0, slashed, folder).ok, "a bay re-mounts its own folder");
    expect(mountAt(5, 1, other.string(), other).ok, "another folder in drive 2");
    // Ejecting frees the folder for another bay.
    expect(sc.ejectMediaBay(ctl, st, 5, 0).ok, "eject drive 1");
    expect(mountAt(4, 1, plain, folder).ok, "the ejected folder mounts elsewhere");

    std::filesystem::remove_all(folder, ec);
    std::filesystem::remove_all(other, ec);
    if (failures) return 1;
    std::cout << "host_folder_one_bay: OK\n";
    return 0;
}
