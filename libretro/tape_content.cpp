// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tape_content.hpp"
#include "host_path.hpp"

#include "pom2/core.hpp"


namespace pom2::libretro {
namespace {

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

} // namespace

bool isTapeFile(const std::filesystem::path& path)
{
    const auto suffix = lower(path.extension().u8string());
    return suffix == ".wav" || suffix == ".aci";
}

TapePlan planTape(const std::filesystem::path& path, std::string& failure)
{
    failure.clear();
    std::vector<pom2::CassetteRecord> records;
    std::string error;
    if (!pom2::Core::readCassetteRecords(sdkPath(path), records, error)) {
        failure = path.filename().u8string() + ": " + error;
        return {};
    }
    if (records.empty()) {
        failure = path.filename().u8string() + " holds no Apple II tape record";
        return {};
    }
    TapePlan plan;
    // [C] Integer BASIC's SAVE writes a 2-byte length record, Applesoft's a
    //     3-byte one (CassetteRecords.h); a Monitor W dump is one record.
    const auto& head = records.front();
    const bool header = records.size() >= 2 && head.checksumOk;
    if (header && head.bytes.size() == 2) {
        plan.kind = TapePlan::Kind::IntegerBasic;
        plan.length = records[1].bytes.size();
        // Integer BASIC is in ROM on the original Apple ][ only.
        plan.prefer = {MachineKind::II};
    } else if (header && head.bytes.size() == 3) {
        plan.kind = TapePlan::Kind::Applesoft;
        plan.length = records[1].bytes.size();
        plan.prefer = {MachineKind::IIPlus, MachineKind::IIeEnhanced, MachineKind::IIe};
    } else {
        plan.kind = TapePlan::Kind::Binary;
        plan.length = head.bytes.size();
        plan.prefer = {MachineKind::IIPlus, MachineKind::II, MachineKind::IIeEnhanced, MachineKind::IIe};
    }
    return plan;
}

std::string tapeCommand(const TapePlan& plan, MachineKind machine, std::string& note)
{
    const std::string size = std::to_string(plan.length) + " bytes";
    switch (plan.kind) {
    case TapePlan::Kind::Applesoft:
        if (machine == MachineKind::II) {
            note = "Applesoft tape (" + size + "): the Apple ][ (1977) has Integer BASIC in ROM, choose a ][+ or a //e";
            return {};
        }
        note = "Applesoft tape (" + size + "): LOAD, then RUN";
        return "LOAD\rRUN\r";
    case TapePlan::Kind::IntegerBasic:
        if (machine != MachineKind::II) {
            note = "Integer BASIC tape (" + size + "): it needs the Apple ][ (1977) ROM, apple2o.rom";
            return {};
        }
        // [C] The ][ powers on in the Monitor; Ctrl-B Return enters BASIC.
        note = "Integer BASIC tape (" + size + "): LOAD, then RUN";
        return "\x02\rLOAD\rRUN\r";
    case TapePlan::Kind::Binary:
        break;
    }
    note = "Machine-code tape (" + size + "): the tape does not say where it loads. In the Monitor "
           "(CALL -151), type START.ENDR with the addresses its instructions give";
    return {};
}

} // namespace pom2::libretro
