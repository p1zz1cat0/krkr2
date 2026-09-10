#import <AppKit/AppKit.h>

#import "YoghourtDockIcon.h"
#import "YoghourtWindowPresentation.h"

extern "C" void YoghourtRefreshWindowLayout(int width, int height, bool fullscreen);

extern "C" void YoghourtApplyPresentation(void) {
    @autoreleasepool {
        NSDictionary<NSString *, NSString *> *environment = NSProcessInfo.processInfo.environment;
        NSString *title = environment[@"YOGHOURT_GAME_TITLE"];
        if (title.length > 0) {
            setprogname(title.UTF8String);
            NSProcessInfo.processInfo.processName = title;
        }

        NSApplication *application = NSApplication.sharedApplication;
        NSString *iconPath = environment[@"YOGHOURT_GAME_ICON"];
        if (iconPath.length > 0) {
            NSImage *icon = YoghourtLoadDockIcon(iconPath);
            if (icon) application.applicationIconImage = icon;
        }
    }
}

extern "C" void YoghourtApplyWindowPresentation(void *nativeWindow) {
    NSWindow *window = (NSWindow *)nativeWindow;
    if(!window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            YoghourtConfigureGameWindow((__bridge void *)window, YoghourtRefreshWindowLayout);
        }
    });
}
