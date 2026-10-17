#include "FujiNetRuntime.h"

#include "ResourcePaths.h"

#include <filesystem>
#include <system_error>

namespace pom2 {
namespace fs = std::filesystem;

namespace {

const char* platformTag()
{
#if defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x64";
#elif defined(_WIN32) && defined(_M_X64)
    return "windows-x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return "";
#endif
}

const char* exeName()
{
#ifdef _WIN32
    return "fujinet.exe";
#else
    return "fujinet";
#endif
}

bool complete(const fs::path& root)
{
    std::error_code ec;
    return fs::is_regular_file(root / exeName(), ec) &&
           fs::is_regular_file(root / "fnconfig.ini", ec) &&
           fs::is_regular_file(root / "data" / "autorun.po", ec);
}

} // namespace

std::string bundledFujiNetExecutable()
{
    const char* tag = platformTag();
    if (!*tag) return {};
    const fs::path rel = fs::path("fujinet-pc-APPLE") / tag / exeName();
    const fs::path dir = executableDir();
    if (dir.empty()) return {};
    // Probe only executable-relative package roots. A writable ROM override
    // directory must not be able to replace a program we execute.
    for (const fs::path& base : {dir, dir / ".." / "share" / "POM2", dir / ".."}) {
        const fs::path exe = base / rel;
        if (complete(exe.parent_path())) return fs::absolute(exe).string();
    }
    return {};
}

bool prepareBundledFujiNet(std::string& executable, std::string& error)
{
    executable.clear();
    error.clear();
    const std::string sourceExe = bundledFujiNetExecutable();
    if (sourceExe.empty()) return false;

    const fs::path source = fs::path(sourceExe).parent_path();
    const fs::path root = userDataDir() / "fujinet-pc-APPLE" / "v1.5.2";
    const fs::path target = root / platformTag();
    if (complete(target)) {
        executable = (target / exeName()).string();
        return true;
    }

    // Never copy directly into the live directory. If POM2 exits halfway
    // through, a later launch must not mistake a partial CONFIG disk for a
    // usable runtime. Also leave a user's pre-existing partial directory
    // untouched instead of deleting its possibly edited configuration.
    std::error_code ec;
    if (fs::exists(target, ec)) {
        error = "Incomplete FujiNet runtime at " + target.string();
        return false;
    }
    const fs::path staging = target.string() + ".staging";
    if (fs::exists(staging, ec)) {
        error = "Incomplete FujiNet staging directory at " + staging.string();
        return false;
    }
    fs::create_directories(root, ec);
    if (ec) {
        error = "Cannot create FujiNet data directory: " + ec.message();
        return false;
    }
    fs::copy(source, staging, fs::copy_options::recursive, ec);
    if (ec || !complete(staging)) {
        error = "Cannot copy bundled FujiNet runtime: " +
                (ec ? ec.message() : std::string("missing CONFIG files"));
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
        return false;
    }
    fs::rename(staging, target, ec);
    if (ec) {
        error = "Cannot activate bundled FujiNet runtime: " + ec.message();
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
        return false;
    }
    executable = (target / exeName()).string();
    return true;
}

} // namespace pom2
