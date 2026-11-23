// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "program_file.hpp"

#include "AppleSingle.h"
#include "ApplesoftTokenizer.h"
#include "HostFileTag.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace pom2::libretro {
namespace {

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

constexpr std::uint16_t kDefaultBinary = 0x0803;   // cc65's apple2 start address
constexpr std::uint32_t kRamTop = 0xC000;

} // namespace

bool isProgramFile(const std::filesystem::path& path)
{
    const auto suffix = lower(path.extension().string());
    return suffix == ".bin" || suffix == ".bas";
}

ProgramImage loadProgram(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.u8string());
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    const auto name = path.filename().u8string();
    ProgramImage image;

    if (lower(path.extension().string()) == ".bas") {
        std::vector<pom2::applesoft::TokenizeError> errors;
        if (!pom2::applesoft::tokenize(std::string(bytes.begin(), bytes.end()), image.bytes, errors, 0x0801)) {
            const auto& first = errors.front();
            throw std::runtime_error(name + " line " + std::to_string(first.sourceLine) + ": " + first.message);
        }
        image.address = 0x0801;
        image.applesoft = true;
        image.command = "RUN\r";
    } else {
        std::uint16_t address = kDefaultBinary;
        std::string stem = path.stem().u8string();
        std::uint8_t type = 0;
        std::uint16_t aux = 0;
        pom2::applesingle::File single;
        std::string error;
        if (pom2::stripHostFileTag(stem, type, aux)) {
            address = aux;                                   // NAME#06AAAA
            image.bytes = std::move(bytes);
        } else if (pom2::applesingle::looksLikeAppleSingle(bytes.data(), bytes.size()) &&
                   pom2::applesingle::decode(bytes, single, error)) {
            if (single.hasProdosInfo) address = static_cast<std::uint16_t>(single.auxType);
            image.bytes = std::move(single.data);
        } else if (bytes.size() > 4 && std::size_t(bytes[2] | (bytes[3] << 8)) == bytes.size() - 4) {
            address = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));   // DOS 3.3 B header
            image.bytes.assign(bytes.begin() + 4, bytes.end());
        } else {
            image.bytes = std::move(bytes);
        }
        image.address = address;
        image.command = "CALL " + std::to_string(address) + "\r";
    }
    if (image.bytes.empty() || image.address < 0x0200 ||
        image.address + image.bytes.size() > kRamTop)
        throw std::runtime_error(name + " does not fit in RAM between $0200 and $BFFF");
    return image;
}

} // namespace pom2::libretro
