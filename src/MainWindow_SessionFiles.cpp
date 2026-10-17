// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "SessionFiles.h"

void MainWindow::renderSessionFileDialog()
{
    if (const auto picked = sessionFilePicker_.render()) {
        std::string error;
        const bool ok = picked->save
            ? pom2::saveSessionFile(*controller, picked->path, error)
            : pom2::loadSessionFile(*controller, picked->path, error);
        sessionFilePicker_.complete(ok, error);
        tapeStatusMessage = ok ? (picked->save ? "Session saved: " : "Session loaded: ") + picked->path
                               : "Session failed: " + error;
        tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
    }
}
