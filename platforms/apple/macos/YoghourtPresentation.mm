#import <AppKit/AppKit.h>

#import "YoghourtDockIcon.h"
#import "YoghourtWindowPresentation.h"

extern "C" void YoghourtRefreshWindowLayout(int width, int height, bool fullscreen);

static __weak NSWindow *gRuntimeWindow;

extern "C" void YoghourtSetGameWindowContentSize(int width, int height) {
    if(width <= 0 || height <= 0) return;
    static int lastWidth = 0, lastHeight = 0;
    if(width == lastWidth && height == lastHeight) return;
    lastWidth = width;
    lastHeight = height;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSWindow *window = gRuntimeWindow;
        if(!window) return;
        const CGFloat ratio = static_cast<CGFloat>(width) / height;
        static bool configured = false;
        window.contentAspectRatio = NSMakeSize(ratio, 1);
        // Fit the windowed frame to the game canvas. Keep fullscreen intact.
        if(!(window.styleMask & NSWindowStyleMaskFullScreen)) {
            NSRect frame = YoghourtFitFrameToContentAspectRatio(
                window.frame, window.styleMask, ratio);
            [window setFrame:frame display:YES];
        }
        if(!configured) {
            // Restore the saved frame after the game ratio is known.
            YoghourtConfigureGameWindow((__bridge void *)window, YoghourtRefreshWindowLayout);
            configured = true;
        }
    });
}

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
    NSWindow *window = (__bridge NSWindow *)nativeWindow;
    if(!window) return;
    gRuntimeWindow = window;
}
