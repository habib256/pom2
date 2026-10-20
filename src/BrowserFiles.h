// POM2 — GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

namespace pom2 {
enum class BrowserFileKind { Text = 0, Session = 1, Disk = 2 };
struct BrowserFileResult {
    BrowserFileKind kind;
    std::string path, error;
};
// Nonblocking, one picker at a time. Returns false if busy or unavailable.
// Results are consumed on the UI thread; disk paths remain live for write-back.
bool chooseBrowserFile(BrowserFileKind kind);
std::optional<BrowserFileResult> takeBrowserFile();
bool downloadBrowserFile(const std::string& path, const std::string& name, std::string& error);
}
