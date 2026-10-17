// POM2 — GPL-3.0-or-later
// Linux chooser protocol, using fake helpers: no GUI/display is required.
#include "NativeFileDialog.h"
#include "ChildProcess.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
int main()
{
    const fs::path root = fs::temp_directory_path() /
        ("pom2-native-dialog-" + std::to_string(::getpid()));
    fs::create_directories(root);
    const fs::path helper = root / "zenity";
    {
        std::ofstream script(helper);
        script << "#!/bin/sh\n"
            "printf '%s\\n' \"$@\" > \"$POM2_DIALOG_ARGUMENTS\"\n"
            "case \"$POM2_DIALOG_MODE\" in cancel) exit 1;; failed) exit 2;; esac\n"
            "printf '%s\\n' \"$POM2_DIALOG_OUTPUT\"\n";
    }
    fs::permissions(helper, fs::perms::owner_all);
    const std::string arguments = (root / "arguments.txt").string();
    const std::string picked = (root / "écran ' $(literal).hgr").string();
    ::setenv("PATH", root.c_str(), 1);
    ::setenv("POM2_DIALOG_ARGUMENTS", arguments.c_str(), 1);
    ::setenv("POM2_DIALOG_OUTPUT", picked.c_str(), 1);
    ::setenv("POM2_DIALOG_MODE", "selected", 1);
    assert(pom2::nativeFileDialogAvailable());
    std::string result;
    assert(pom2::nativeFileDialog(false, "Open ' $name; file", "dsk,woz", root.string(), "a b.dsk", result)
        == pom2::FileDialogResult::Selected);
    assert(result == picked);
    {
        std::ifstream file(arguments);
        const std::string args((std::istreambuf_iterator<char>(file)), {});
        assert(args.find("--title=Open ' $name; file\n") != std::string::npos);
        assert(args.find("--file-filter=*.dsk *.woz\n") != std::string::npos);
        assert(args.find("--filename=" + (root / "a b.dsk").string() + "\n") != std::string::npos);
    }
    assert(pom2::nativeFileDialog(true, "Save", "hgr", root.string(), "page.hgr", result)
        == pom2::FileDialogResult::Selected);
    {
        std::ifstream file(arguments);
        const std::string args((std::istreambuf_iterator<char>(file)), {});
        assert(args.find("--save\n") != std::string::npos);
        assert(args.find("--confirm-overwrite\n") != std::string::npos);
    }
    ::setenv("POM2_DIALOG_MODE", "cancel", 1);
    assert(pom2::nativeFileDialog(false, "Open", "", "", "", result)
        == pom2::FileDialogResult::Cancelled);
    assert(result.empty());
    ::setenv("POM2_DIALOG_MODE", "failed", 1);
    assert(pom2::nativeFileDialog(false, "Open", "", "", "", result)
        == pom2::FileDialogResult::Unavailable);
    fs::rename(helper, root / "kdialog");
    // A system zenity may remain discoverable through the helper's fallback
    // paths. Never launch it from this headless test just to reach kdialog.
    if (pom2::ChildProcess::findOnPath("zenity").empty()) {
        ::setenv("POM2_DIALOG_MODE", "selected", 1);
        assert(pom2::nativeFileDialog(false, "Open", "hgr", root.string(), "", result)
            == pom2::FileDialogResult::Selected);
        assert(result == picked);
    }
    fs::remove_all(root);
}
