// POM2 — GPL-3.0-or-later
#ifndef POM2_NATIVE_FILE_DIALOG_H
#define POM2_NATIVE_FILE_DIALOG_H
#include <string>
namespace pom2 {
enum class FileDialogResult { Selected, Cancelled, Unavailable };
bool nativeFileDialogAvailable();
// UI thread only, without stateMutex. Cancellation never opens a second UI.
// extCsv has extensions without dots; empty permits any file.
FileDialogResult nativeFileDialog(bool save, const std::string& title,
    const std::string& extCsv, const std::string& directory,
    const std::string& name, std::string& path);
}
#endif
