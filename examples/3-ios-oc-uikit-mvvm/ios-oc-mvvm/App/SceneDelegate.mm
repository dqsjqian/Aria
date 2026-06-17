#import "SceneDelegate.h"
#import "RootViewController.h"

// ═════════════════════════════════════════════════════════════════════════════
// SceneDelegate — lifecycle of a single "scene" (UI window) (ObjC++).
//
// On macOS the equivalent code lives in AppDelegate:
//     self.window = [[NSWindow alloc] init...];
//     self.window.contentViewController = rootVC;
//     [self.window orderFront:nil];
//
// On iOS this moves into SceneDelegate's scene:willConnectToSession:options:.
// We construct UIWindow purely in code — no Storyboard required.
// ═════════════════════════════════════════════════════════════════════════════

@implementation SceneDelegate

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions {
    // `scene` is actually a UIWindowScene at runtime.
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    if (![windowScene isKindOfClass:[UIWindowScene class]]) return;

    // 1) Create the UIWindow bound to the current windowScene
    //    (more modern than the `initWithFrame:` form).
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];

    // 2) Attach the rootViewController — entry point of the whole UI tree.
    //    Wrap it in a UINavigationController so Panel B can push another
    //    full-screen VC. The Root screen wants the original chromeless look,
    //    so it hides the navigation bar inside its own viewWillAppear, and
    //    re-shows it once a child is pushed.
    RootViewController *rootVC = [[RootViewController alloc] init];
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:rootVC];
    nav.navigationBar.prefersLargeTitles = NO;
    self.window.rootViewController = nav;

    // 3) Show it. iOS has no orderFront — use makeKeyAndVisible.
    [self.window makeKeyAndVisible];

    NSLog(@"[Aria][Scene] willConnect — window attached");
}

#pragma mark - Other scene lifecycle hooks
//
// Full call order when the user kills the app from the app switcher:
//   willResignActive → didEnterBackground → didDisconnect
// Full call order when foregrounding from background:
//   willEnterForeground → didBecomeActive
// Each hook just NSLogs so they can be observed against the console.

- (void)sceneDidDisconnect:(UIScene *)scene {
    // The scene was reclaimed by the system (e.g. the user swiped the
    // app away from the multitasking switcher). Drop our window
    // reference so the chain RootViewController → ViewModel →
    // SubscriptionBag deallocates promptly, releasing C++ side
    // resources actively rather than waiting for process teardown.
    NSLog(@"[Aria][Scene] didDisconnect — releasing window");
    self.window = nil;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    // Scene is interactive in the foreground; resume animations/timers.
    NSLog(@"[Aria][Scene] didBecomeActive");
}

- (void)sceneWillResignActive:(UIScene *)scene {
    // Scene is about to lose focus (incoming call, control center, etc.);
    // a good place to pause timers.
    NSLog(@"[Aria][Scene] willResignActive");
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
    // Scene is about to come back to the foreground.
    NSLog(@"[Aria][Scene] willEnterForeground");
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
    // Scene has moved to the background. Persist data, release resources.
    // For real apps, anything that MUST be persisted should be saved
    // here — applicationWillTerminate: is not guaranteed to be called.
    NSLog(@"[Aria][Scene] didEnterBackground");
}

@end
