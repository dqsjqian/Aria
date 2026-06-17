#import <Cocoa/Cocoa.h>
#import "AppDelegate.h"

// Manual bootstrap: no reliance on Info.plist's NSMainStoryboardFile /
// NSMainNibFile / NSApplicationDelegateClassName. The app bundle does
// not need a Storyboard/Nib; the launch sequence is fully visible in
// code:
//   1) grab the NSApplication singleton
//   2) install our AppDelegate
//   3) set the regular activation policy (dock icon, foreground window)
//   4) enter the event loop
int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
