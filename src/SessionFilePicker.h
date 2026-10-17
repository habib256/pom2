// POM2 — GPL-3.0-or-later
#pragma once
#include <array>
#include <optional>
#include <string>

namespace pom2 {
struct SessionFileSelection { bool save; std::string path; };
// UI thread only, without the machine lock. File operations belong to the
// caller, which reports success/failure through complete().
class SessionFilePicker {
public:
    void open(bool save);
    std::optional<SessionFileSelection> render();
    void complete(bool ok, const std::string& error);
private:
    bool setPath(const std::string& path);
    std::optional<SessionFileSelection> submit(bool replace = false);
    std::array<char, 4096> path_{};
    std::string error_;
    bool save_ = false, opening_ = false, closing_ = false, replacing_ = false;
};
}
