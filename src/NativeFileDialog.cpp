// POM2 — GPL-3.0-or-later
#include "NativeFileDialog.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#elif !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#include "ChildProcess.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pom2 {
#if defined(__APPLE__)
// AppKit implementation is compiled as Objective-C++.
#else
bool nativeFileDialogAvailable()
{
#if defined(_WIN32)
    return true;
#elif defined(__EMSCRIPTEN__)
    return false;
#else
    return !ChildProcess::findOnPath("zenity").empty() ||
           !ChildProcess::findOnPath("kdialog").empty();
#endif
}

FileDialogResult nativeFileDialog(bool save, const std::string& title,
    const std::string& extCsv, const std::string& directory,
    const std::string& name, std::string& path)
{
    path.clear();
#if defined(__EMSCRIPTEN__)
    (void)save; (void)title; (void)extCsv; (void)directory; (void)name;
    return FileDialogResult::Unavailable;
#else
    std::string patterns;
    for (size_t start = 0; start < extCsv.size();) {
        const size_t end = extCsv.find(',', start);
        if (!patterns.empty()) patterns += ';';
        patterns += "*." + extCsv.substr(start, end == std::string::npos ? end : end - start);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (patterns.empty()) patterns = "*";
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768, 0);
    const auto initialName = std::filesystem::u8path(name).wstring();
    if (initialName.size() >= buffer.size()) return FileDialogResult::Unavailable;
    std::copy(initialName.begin(), initialName.end(), buffer.begin());
    const auto wideTitle = std::filesystem::u8path(title).wstring();
    const auto wideDir = std::filesystem::u8path(directory).wstring();
    std::wstring filter = L"Supported files";
    filter.push_back(0); filter += std::filesystem::u8path(patterns).wstring();
    filter.push_back(0); filter += L"All files"; filter.push_back(0);
    filter += L"*.*"; filter.push_back(0); filter.push_back(0);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrTitle = wideTitle.c_str();
    dialog.lpstrInitialDir = wideDir.empty() ? nullptr : wideDir.c_str();
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const bool selected = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!selected) return CommDlgExtendedError() ? FileDialogResult::Unavailable : FileDialogResult::Cancelled;
    path = std::filesystem::path(buffer.data()).u8string();
#else
    const auto initial = directory.empty() ? std::filesystem::u8path(name) :
        std::filesystem::u8path(directory) / std::filesystem::u8path(name);
    std::string executable = ChildProcess::findOnPath("zenity");
    std::vector<std::string> owned;
    if (!executable.empty()) {
        for (char& c : patterns) if (c == ';') c = ' ';
        owned = {executable, "--file-selection", "--title=" + title,
            "--filename=" + initial.u8string(), "--file-filter=" + patterns};
        if (save) { owned.push_back("--save"); owned.push_back("--confirm-overwrite"); }
    } else {
        executable = ChildProcess::findOnPath("kdialog");
        if (executable.empty()) return FileDialogResult::Unavailable;
        for (char& c : patterns) if (c == ';') c = ' ';
        owned = {executable, "--title", title, save ? "--getsavefilename" : "--getopenfilename",
            initial.u8string(), patterns};
    }
    std::vector<char*> argv;
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 3 || maxFd > 65536) maxFd = 65536;
    int channel[2];
    if (::pipe(channel) != 0) return FileDialogResult::Unavailable;
    const pid_t child = ::fork();
    if (child == 0) {
        // No allocation or shell after fork in this multithreaded process.
        if (::dup2(channel[1], STDOUT_FILENO) < 0) ::_exit(127);
        const int nullFd = ::open("/dev/null", O_RDWR);
        if (nullFd >= 0) { ::dup2(nullFd, STDIN_FILENO); ::dup2(nullFd, STDERR_FILENO); }
        for (int fd = 3; fd < maxFd; ++fd) ::close(fd);
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(channel[1]);
    if (child < 0) { ::close(channel[0]); return FileDialogResult::Unavailable; }
    char buffer[4096];
    bool readFailed = false;
    for (;;) {
        const auto count = ::read(channel[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count < 0) readFailed = true; break; }
        if (path.size() + static_cast<size_t>(count) <= 32768)
            path.append(buffer, static_cast<size_t>(count));
        else readFailed = true;
    }
    ::close(channel[0]);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || readFailed || !WIFEXITED(status) || WEXITSTATUS(status) > 1)
        return FileDialogResult::Unavailable;
    if (WEXITSTATUS(status) == 1) { path.clear(); return FileDialogResult::Cancelled; }
    if (!path.empty() && path.back() == '\n') path.pop_back();
#endif
    return path.empty() ? FileDialogResult::Cancelled : FileDialogResult::Selected;
#endif
}
#endif
} // namespace pom2
