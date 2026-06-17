#import <UIKit/UIKit.h>
#import "AppDelegate.h"

// ═════════════════════════════════════════════════════════════════════════════
// main.m — iOS app entry point
//
// Differences from the macOS sibling demo:
//   macOS: manual NSApplication + setDelegate + run
//   iOS:   one line UIApplicationMain does everything; it will
//            1) create the UIApplication singleton
//            2) instantiate AppDelegate
//            3) read UIApplicationSceneManifest from Info.plist
//            4) enter the event loop
//
// The 3rd argument (nil) means "use the default UIApplication class".
// The 4th argument is the AppDelegate class name string; UIKit will
// alloc/init it itself.
// ═════════════════════════════════════════════════════════════════════════════
int main(int argc, char * argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
