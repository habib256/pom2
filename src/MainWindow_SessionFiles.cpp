// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "SessionFiles.h"
#include "BrowserFiles.h"
#include "EmulationController.h"
#include "imgui.h"
#include <cstdio>

void MainWindow::restoreBrowserSession()
{
#ifdef __EMSCRIPTEN__
    browserSession_.initialize(*controller);
    if (!browserSession_.error().empty()) {
        tapeStatusMessage = "Automatic resume failed: " + browserSession_.error();
        tapeStatusUntil = lastFrameTime + 15.0;
    }
#endif
}
void MainWindow::checkpointBrowserSession(bool running)
{
#ifdef __EMSCRIPTEN__
    browserSession_.checkpoint(*controller, running);
#else
    (void)running;
#endif
}
void MainWindow::renderBrowserSessionMenu()
{
#ifdef __EMSCRIPTEN__
    if (!ImGui::BeginMenu("Automatic resume")) return;
    bool enabled = browserSession_.enabled();
    if (ImGui::MenuItem("Resume this machine on reload", nullptr, &enabled))
        browserSession_.setEnabled(*controller, enabled);
    ImGui::TextWrapped("Checkpoints every 10 seconds and when leaving the tab. Requires a supported II/II+/IIe or //c configuration and idle serial ports. Restored writable floppy changes stay in the session.");
    if (browserSession_.suspended()) {
        ImGui::TextWrapped("Checkpointing suspended: %s", browserSession_.error().c_str());
        if (ImGui::MenuItem("Retry with current configuration")) browserSession_.setEnabled(*controller, true);
    }
    ImGui::EndMenu();
#endif
}

void MainWindow::renderSessionFileDialog()
{
#ifdef __EMSCRIPTEN__
    if (const auto file = pom2::takeBrowserFile()) {
        if (!file->error.empty()) {
            tapeStatusMessage = file->error;
            tapeStatusUntil = lastFrameTime + 10.0;
        } else if (!file->path.empty()) {
            if (file->kind == pom2::BrowserFileKind::Text) pasteDialogPath = file->path;
            else {
                std::string error;
                const bool ok = pom2::loadSessionFile(*controller, file->path, error);
                tapeStatusMessage = ok ? "Session imported." : "Session failed: " + error;
                tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
                std::remove(file->path.c_str());
            }
        }
    }
#endif
    if (const auto picked = sessionFilePicker_.render()) {
        std::string error;
        bool ok = picked->save
            ? pom2::saveSessionFile(*controller, picked->path, error)
            : pom2::loadSessionFile(*controller, picked->path, error);
#ifdef __EMSCRIPTEN__
        if (ok && picked->save)
            ok = pom2::downloadBrowserFile(picked->path, "session.pom2session", error);
        if (ok) std::remove(picked->path.c_str());
#endif
        sessionFilePicker_.complete(ok, error);
#ifdef __EMSCRIPTEN__
        tapeStatusMessage = ok ? "Session download started." : "Session failed: " + error;
#else
        tapeStatusMessage = ok ? (picked->save ? "Session saved: " : "Session loaded: ") + picked->path
                               : "Session failed: " + error;
#endif
        tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
    }
}
