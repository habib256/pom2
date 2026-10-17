#include "FujiNetRuntime.h"
#include "ResourcePaths.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {
fs::path root;

void put(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

std::string read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
}

namespace pom2 {
std::filesystem::path userDataDir() { return root / "user"; }
std::filesystem::path executableDir()
{
    return root / "package";
}
}

int main()
{
#if defined(__APPLE__) && defined(__aarch64__)
    const char* tag = "macos-arm64";
    const char* exe = "fujinet";
#elif defined(__APPLE__) && defined(__x86_64__)
    const char* tag = "macos-x64";
    const char* exe = "fujinet";
#elif defined(_WIN32) && defined(_M_X64)
    const char* tag = "windows-x64";
    const char* exe = "fujinet.exe";
#elif defined(__linux__) && defined(__x86_64__)
    const char* tag = "linux-x64";
    const char* exe = "fujinet";
#else
    return 77; // upstream has no FujiNet-PC Apple binary for this target
#endif
    root = fs::temp_directory_path() / "pom2-fujinet-runtime-test";
    fs::remove_all(root);
    const fs::path source = root / "package" / "fujinet-pc-APPLE" / tag;
    put(source / exe, "executable");
#ifndef _WIN32
    fs::permissions(source / exe, fs::perms::owner_exec, fs::perm_options::add);
#endif
    put(source / "fnconfig.ini", "original");
    put(source / "data" / "autorun.po", "CONFIG");

    // A crash during an earlier first-run copy can leave the old fixed
    // staging path behind. It must not prevent the next launch.
    const fs::path stale = root / "user" / "fujinet-pc-APPLE" /
        "v1.5.2" / (std::string(tag) + ".staging");
    put(stale / "partial", "keep");

    std::string path, error;
    if (!pom2::prepareBundledFujiNet(path, error) || !error.empty()) {
        std::cerr << "first prepare failed: " << error << '\n';
        return 1;
    }
    const fs::path installed(path);
#ifndef _WIN32
    if ((fs::status(installed).permissions() & fs::perms::owner_exec) ==
        fs::perms::none)
        return 4;
#endif
    if (read(installed.parent_path() / "data" / "autorun.po") != "CONFIG")
        return 2;
    if (read(stale / "partial") != "keep") return 5;
    put(installed.parent_path() / "fnconfig.ini", "user changes");
    if (!pom2::prepareBundledFujiNet(path, error) ||
        read(installed.parent_path() / "fnconfig.ini") != "user changes")
        return 3;
    fs::remove_all(root);
    return 0;
}
