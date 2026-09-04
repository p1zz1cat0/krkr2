#import <AppKit/AppKit.h>

#import "YoghourtDockIcon.h"

extern "C" void YoghourtRefreshWindowLayout(int width, int height);

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

static void YoghourtApplyStyle(NSWindow *window) {
    window.styleMask |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    window.collectionBehavior &= ~(1 << 9);
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    window.titleVisibility = NSWindowTitleVisible;
    window.titlebarAppearsTransparent = NO;
}

static void YoghourtRequestFullscreenIfNeeded(NSWindow *window) {
    NSDictionary<NSString *, NSString *> *environment = NSProcessInfo.processInfo.environment;
    if([environment[@"YOGHOURT_DISPLAY_MODE"] isEqualToString:@"fullscreen"] &&
       !(window.styleMask & NSWindowStyleMaskFullScreen)) {
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        __block id observer = nil;
        observer = [center addObserverForName:NSWindowDidEnterFullScreenNotification
                                       object:window
                                        queue:NSOperationQueue.mainQueue
                                   usingBlock:^(NSNotification *note) {
            (void)note;
            [center removeObserver:observer];
            NSSize size = window.contentView.bounds.size;
            YoghourtRefreshWindowLayout((int)size.width, (int)size.height);
        }];
        [window toggleFullScreen:nil];
    }
}

extern "C" void YoghourtApplyWindowPresentation(void *nativeWindow) {
    NSWindow *window = (NSWindow *)nativeWindow;
    if(!window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            YoghourtApplyStyle(window);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(),
                           ^{ YoghourtRequestFullscreenIfNeeded(window); });
        }
    });
}
