#import "AppDelegate.h"
#import "RootViewController.h"

// ═════════════════════════════════════════════════════════════════════════════
// AppDelegate — app lifecycle (ObjC++ version)
// ═════════════════════════════════════════════════════════════════════════════

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(200, 100, 520, 720)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"MVVM + aria Demo";
    self.window.minSize = NSMakeSize(520, 720);

    RootViewController *rootVC = [[RootViewController alloc] init];
    self.window.contentViewController = rootVC;
    [self.window orderFront:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end
