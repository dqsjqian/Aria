#import <UIKit/UIKit.h>

// iOS AppDelegate — app-level lifecycle (launch / background / memory warnings).
// The actual UI is owned by SceneDelegate (the iOS 13+ UIScene architecture).
//
// Note: AppDelegate does not need objc_runtime_name. main.m uses
//       NSStringFromClass([AppDelegate class]) to look up the class
//       at runtime — the compiler resolves it to the class object
//       directly, so we don't go through Info.plist's string-keyed
//       lookup. Only SceneDelegate needs a pinned runtime name because
//       Info.plist references it by string.
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
