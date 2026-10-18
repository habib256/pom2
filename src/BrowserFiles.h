// POM2 — GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

namespace pom2 {
enum class BrowserFileKind { Text, Session };
struct BrowserFileResult {
    BrowserFileKind kind;
    std::string path, error;
};
// Nonblocking, one picker at a time. Results are consumed on the UI thread.
void chooseBrowserFile(BrowserFileKind kind);
std::optional<BrowserFileResult> takeBrowserFile();
bool downloadBrowserFile(const std::string& path, const std::string& name, std::string& error);
}
