// POM2 — GPL-3.0-or-later
#include "NativeFileDialog.h"
#import <Cocoa/Cocoa.h>
namespace pom2 {
bool nativeFileDialogAvailable() { return true; }
FileDialogResult nativeFileDialog(bool save, const std::string& title,
    const std::string& extCsv, const std::string& directory,
    const std::string& name, std::string& path)
{
    path.clear();
    @autoreleasepool {
        NSSavePanel* panel = save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        panel.title = [NSString stringWithUTF8String:title.c_str()];
        if (!save) {
            NSOpenPanel* open = (NSOpenPanel*)panel;
            open.canChooseFiles = YES; open.canChooseDirectories = NO;
            open.allowsMultipleSelection = NO;
        }
        if (!directory.empty())
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:directory.c_str()] isDirectory:YES];
        if (!name.empty()) panel.nameFieldStringValue = [NSString stringWithUTF8String:name.c_str()];
        if (!extCsv.empty()) {
            // allowedFileTypes remains available on the supported macOS 10.15.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            panel.allowedFileTypes = [[NSString stringWithUTF8String:extCsv.c_str()] componentsSeparatedByString:@","];
#pragma clang diagnostic pop
            panel.allowsOtherFileTypes = YES;
        }
        if ([panel runModal] != NSModalResponseOK) return FileDialogResult::Cancelled;
        if (!panel.URL.isFileURL) return FileDialogResult::Unavailable;
        path = panel.URL.fileSystemRepresentation;
        return FileDialogResult::Selected;
    }
}
}
