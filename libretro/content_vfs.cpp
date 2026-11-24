// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
#include "content_vfs.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace pom2::libretro {
namespace {

retro_vfs_interface* vfs = nullptr;

std::uint64_t pathHash(const std::string& text)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : text) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentOf(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool hasExtension(const std::string& path, const char* ext)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ext;
}

// One file through the VFS into `target`.
bool copyThroughVfs(const std::string& source, const std::filesystem::path& target, std::string& error)
{
    if (!vfs || !vfs->open || !vfs->read || !vfs->close) {
        error = "The frontend cannot open " + source + " (no VFS)";
        return false;
    }
    retro_vfs_file_handle* handle = vfs->open(source.c_str(), RETRO_VFS_FILE_ACCESS_READ,
                                              RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!handle) {
        error = "The frontend could not open " + source;
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const auto partial = target.u8string() + ".part";
    std::ofstream out(std::filesystem::u8path(partial), std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1 << 16);
    bool ok = static_cast<bool>(out);
    std::uint64_t total = 0;
    while (ok) {
        const auto got = vfs->read(handle, buffer.data(), buffer.size());
        if (got < 0) { ok = false; break; }
        if (got == 0) break;
        out.write(buffer.data(), static_cast<std::streamsize>(got));
        total += static_cast<std::uint64_t>(got);
        ok = static_cast<bool>(out) && total <= (64ull << 20);   // no Apple II medium is bigger
    }
    vfs->close(handle);
    out.close();
    if (!ok || !out) {
        std::filesystem::remove(std::filesystem::u8path(partial), ec);
        error = "Could not copy " + source + " through the frontend";
        return false;
    }
    std::filesystem::rename(std::filesystem::u8path(partial), target, ec);
    if (ec) {
        error = "Could not keep the copy of " + source + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

void initContentVfs(retro_environment_t environment)
{
    vfs = nullptr;
    if (!environment) return;
    retro_vfs_interface_info info{1, nullptr};
    if (environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &info) && info.iface)
        vfs = info.iface;
}

bool isLocalPath(const std::string& path)
{
    return path.find("://") == std::string::npos;
}

std::string localizeContent(const std::string& path, const std::filesystem::path& cacheRoot,
                            std::string& error)
{
    error.clear();
    if (isLocalPath(path)) return path;
    // One folder per original location: its disks keep their private copies
    // from one session to the next.
    char key[17];
    std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(pathHash(parentOf(path))));
    const auto folder = cacheRoot / key;
    const auto local = folder / std::filesystem::u8path(baseName(path));
    if (!copyThroughVfs(path, local, error)) return {};
    if (!hasExtension(path, ".m3u")) return local.u8string();

    // A playlist names its disks relative to itself: bring each one, and
    // write the copy with the local paths.
    std::ifstream in(local);
    std::stringstream rewritten;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') { rewritten << line << '\n'; continue; }
        const auto bar = line.find('|');
        const std::string entry = line.substr(0, bar);
        const std::string label = bar == std::string::npos ? std::string() : line.substr(bar);
        const std::string source = isLocalPath(entry) && !entry.empty() && entry[0] != '/'
            ? parentOf(path) + "/" + entry : entry;
        const auto copy = isLocalPath(source) ? source : localizeContent(source, cacheRoot, error);
        if (copy.empty()) return {};
        rewritten << copy << label << '\n';
    }
    in.close();
    std::ofstream(local, std::ios::trunc) << rewritten.str();
    return local.u8string();
}

} // namespace pom2::libretro
