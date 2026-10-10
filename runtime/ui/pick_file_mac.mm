// The disc image picker on macOS: an NSOpenPanel. See pick_file.h.
#include "ui/pick_file.h"
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

std::string ui_pick_disc(const std::string& title) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.message = [NSString stringWithUTF8String:title.c_str()];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        for (NSString* ext in @[ @"iso", @"ciso" ])
            if (UTType* t = [UTType typeWithFilenameExtension:ext]) [types addObject:t];
        panel.allowedContentTypes = types;
        NSWindow* key = [NSApp keyWindow];
        const NSModalResponse r = [panel runModal];
        [key makeKeyAndOrderFront:nil];   // back to the game's window, as it was
        if (r != NSModalResponseOK || panel.URLs.count == 0) return {};
        return panel.URLs[0].path.UTF8String;
    }
}
