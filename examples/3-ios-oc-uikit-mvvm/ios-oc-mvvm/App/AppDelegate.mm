#import "AppDelegate.h"
#import "SceneDelegate.h"
#import "UIKitConformanceRunner.h"

// ═════════════════════════════════════════════════════════════════════════════════════════
// AppDelegate — app-level lifecycle (ObjC++).
//
// Comparison with the macOS sibling demo:
//   macOS: AppDelegate creates an NSWindow + attaches the
//          contentViewController + calls orderFront from
//          applicationDidFinishLaunching: directly.
//   iOS:   AppDelegate is restricted to "app-level" events (launch,
//          background, memory warnings). Window construction lives
//          in SceneDelegate. The two are wired together via the
//          UIApplicationSceneManifest entry in Info.plist.
//
// ───────────────────────────────────────────────────────────────────────────────────────────
// Debugging tip (re. "hitting the red frame in main.m on app close"):
//   * Pressing Stop (⌘.) in Xcode: Xcode kills the process via
//     SIGKILL/SIGTERM, and lldb traps inside UIApplicationMain.
//     That is **lldb behaviour, not an app crash** — in this case
//     applicationWillTerminate: is typically NOT invoked.
//   * To observe the "graceful exit" flow, swipe up the home gesture
//     in the simulator, then swipe the app away from the app
//     switcher. The following hooks fire in order:
//         sceneWillResignActive  → sceneDidEnterBackground
//         → sceneDidDisconnect   → applicationWillTerminate (not guaranteed)
//   * On modern iOS, applicationWillTerminate: is frequently skipped
//     — the system prefers to reclaim background processes directly.
//     Anything that MUST be persisted should therefore be written
//     from sceneDidEnterBackground:.
// ═════════════════════════════════════════════════════════════════════════════════════════

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {    // Application launched. This is the place for global init
    // (analytics SDK, logging, singleton DataModel, ...).
    // UI construction happens in SceneDelegate.
    NSLog(@"[Aria][App] didFinishLaunching");

    // UIKit adapter conformance self-test — runs synchronously on
    // the main thread, results go to NSLog. Skip by setting environment
    // variable ARIA_SKIP_CONFORMANCE=1 before launch.
    if (!getenv("ARIA_SKIP_CONFORMANCE")) {
        [UIKitConformanceRunner runAndLog];
    }
    return YES;
}

#pragma mark - UISceneSession lifecycle

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
    // Tell UIKit to use the "Default Configuration" entry whenever a
    // new scene connects. The name must match the configuration entry
    // under UISceneConfigurations in Info.plist.
    return [[UISceneConfiguration alloc]
            initWithName:@"Default Configuration"
            sessionRole:connectingSceneSession.role];
}

- (void)application:(UIApplication *)application
    didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
    // Called when the user closes a scene from the multitasking switcher.
    // We don't need to persist any state here — keep the body empty.
    NSLog(@"[Aria][App] didDiscardSceneSessions count=%lu",
          (unsigned long)sceneSessions.count);
}

#pragma mark - Background / Termination / Memory

- (void)applicationDidEnterBackground:(UIApplication *)application {
    // The whole app moved to the background. (For a single-scene app
    // the app-level and scene-level hooks fire almost simultaneously.)
    // Real apps would: persist critical state, pause timers, stop
    // upload tasks. The demo has nothing persistable, so we just log.
    NSLog(@"[Aria][App] didEnterBackground");
}

- (void)applicationWillTerminate:(UIApplication *)application {
    // The system is about to terminate the app (the user swiped it
    // away, or memory pressure forced the system to reclaim it).
    // NOTE: iOS does NOT guarantee this method runs — the system
    //       may simply SIGKILL the process. Anything that MUST be
    //       persisted should be saved earlier in didEnterBackground.
    // The log below is a marker: when we see it, we know we walked
    // through the "graceful exit" path, as opposed to the Xcode-Stop
    // path that traps lldb in main.m.
    NSLog(@"[Aria][App] willTerminate — graceful shutdown");
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application {
    // The system is asking for memory back. Real apps would clear
    // caches and drop reconstructable resources. The demo just logs.
    NSLog(@"[Aria][App] didReceiveMemoryWarning");
}

@end
