// POM2 — GPL-3.0-or-later
#include "BrowserFiles.h"
#include <utility>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace {
std::optional<pom2::BrowserFileResult> pending;
}
extern "C" EMSCRIPTEN_KEEPALIVE void pom2_browser_file_ready(int kind, const char* path, const char* error)
{
    pending = pom2::BrowserFileResult{kind ? pom2::BrowserFileKind::Session : pom2::BrowserFileKind::Text,
                                    path ? path : "", error ? error : ""};
}
#endif

namespace pom2 {
void chooseBrowserFile(BrowserFileKind kind)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({ Module.pom2ChooseFile($0); }, kind == BrowserFileKind::Session ? 1 : 0);
#else
    (void)kind;
#endif
}
std::optional<BrowserFileResult> takeBrowserFile()
{
#ifdef __EMSCRIPTEN__
    auto result = std::move(pending);
    pending.reset();
    return result;
#else
    return {};
#endif
}
bool downloadBrowserFile(const std::string& path, const std::string& name, std::string& error)
{
#ifdef __EMSCRIPTEN__
    const bool ok = EM_ASM_INT({ return Module.pom2DownloadFile(UTF8ToString($0), UTF8ToString($1)) ? 1 : 0; }, path.c_str(), name.c_str());
    if (!ok) error = "The browser could not start the download.";
    return ok;
#else
    (void)path; (void)name;
    error = "Browser downloads require the WebAssembly build.";
    return false;
#endif
}
}
