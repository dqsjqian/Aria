#import <UIKit/UIKit.h>

// SceneDelegate — UI scene lifecycle (iOS 13+).
// Owns the UIWindow, attaches the root view controller, and reacts to
// the scene's active / background transitions.
//
// Why __attribute__((objc_runtime_name("SceneDelegate"))):
//   This pins the class name in the ObjC runtime to literally
//   "SceneDelegate" (no module / namespace prefix). Info.plist's
//   UISceneDelegateClassName = "SceneDelegate" must be resolvable via
//   NSClassFromString(); otherwise UIApplicationMain throws
//   "Invalid parameter not satisfying: cls" at launch and the app
//   crashes immediately. Explicitly locking the runtime name is the
//   most robust spelling — independent of whether the project name
//   contains hyphens or whether ARC / Modules are enabled.
__attribute__((objc_runtime_name("SceneDelegate")))
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong, nullable) UIWindow *window;
@end
