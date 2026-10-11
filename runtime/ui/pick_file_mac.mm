// The file picker on macOS: an NSOpenPanel. See pick_file.h.
#include "ui/pick_file.h"
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

std::string ui_pick_file(const std::string& title, const std::vector<std::string>& exts) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        for (const std::string& ext : exts)
            if (UTType* t = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext.c_str()]]) [types addObject:t];
        panel.allowedContentTypes = types;
        NSWindow* key = [NSApp keyWindow];
        const NSModalResponse r = [panel runModal];
        [key makeKeyAndOrderFront:nil];   // back to the game's window, as it was
        if (r != NSModalResponseOK || panel.URLs.count == 0) return {};
        return panel.URLs[0].path.UTF8String;
    }
}
