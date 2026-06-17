#import "RootViewController.h"
#import "RootView.h"
#import "UIKitAdapter.hpp"
#import "MainViewModel.hpp"
#import "FreePlaygroundController.h"
#import "LayoutPlaygroundController.h"
#import "ListPlaygroundController.h"
#import "ScrollPlaygroundController.h"

#import <objc/runtime.h>

#include "aria/binding/binding_engine.hpp"

#include <memory>
#include <vector>

// ═════════════════════════════════════════════════════════════════════════════
// RootViewController — assembly + binding + business logic.
//
// View is in RootView (pure UIKit, no VM, no bindings).
// Controller responsibilities:
//   1. Create & own the C++ ViewModel (MainViewModel)
//   2. Create & own the RootView
//   3. Wire the BindingEngine: VM properties ↔ UIKitAdapter (real framework
//      use — not hand-rolled target-action).
//   4. Bind View buttons → VM commands / open-panel handlers
//   5. Drive lifecycle (activate / deactivate VM, clean up)
//
// RootViewController is a faithful demonstration of the
// framework on iOS: every "VM → label" is a `BindingEngine::bind_text_oneway`,
// every "pattern button → VM" is a `BindingEngine::bind_command`. The
// only hand-rolled glue left is for purely view-local navigation
// (panels A/B/C/D), where there is no VM piece to bind to — and even
// there we route the click through `adapter.on_click` rather than
// hand-rolled `addTarget:action:` so the framework's click pipeline is
// always the one running.
//
// Differences from the macOS sibling demo:
//   * NSViewController → UIViewController
//   * NSWindow popup     → presentViewController: modal presentation
//   * loadView is still available; on iOS one may also override
//     -[UIViewController view] directly.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootViewController ()
@property (nonatomic, strong) RootView *rootView;
@property (nonatomic, strong) NSMutableArray<UIViewController *> *playgroundControllers;
@end

@implementation RootViewController {
    std::shared_ptr<MainViewModel>                         _vm;

    // Binding plumbing — real framework wiring (replaces hand-rolled
    // target-action + objc_setAssociatedObject wrapper retain).
    //
    // Declaration order matters for destruction:
    //   1. _engine dies first → drops every Subscription, detaches
    //      adapter slots from native widgets.
    //   2. _adapter dies next → its Bridge map (per-view ObjC targets)
    //      goes away; native widgets that survive the controller are
    //      already cleaned up.
    //   3. _views die last → ~UIKitView fires on_destroy; by now
    //      _engine is gone so the engine-side handler is a no-op (the
    //      subscription was already detached). Safe.
    std::shared_ptr<aria::adapters::uikit::UIKitAdapter>   _adapter;
    std::unique_ptr<aria::binding::BindingEngine>          _engine;
    std::vector<std::unique_ptr<aria::adapters::uikit::UIKitView>> _views;

    // Side-effect subscriptions that are *not* part of the binding graph
    // (e.g. "show toast on result_text change"). Kept in a SubscriptionBag
    // so they release with the controller even if the underlying UIView
    // happens to outlive it (toast handler captures `weakView`).
    aria::SubscriptionBag                                  _bag;
}

#pragma mark - View lifecycle

- (void)loadView {
    // iOS: replace the default view with our custom RootView.
    // The frame auto-resizes with UIWindow; we seed it with the screen
    // bounds and let auto-resizing pull it to the actual window bounds.
    self.rootView = [[RootView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.rootView.autoresizingMask = UIViewAutoresizingFlexibleWidth
                                   | UIViewAutoresizingFlexibleHeight;
    self.view = self.rootView;
    self.playgroundControllers = [NSMutableArray new];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];
    [self setupViewModel];
    [self setupBindingEngine];
    [self bindStateToLabels];
    [self bindPatternButtons];
    [self bindPlaygroundButtons];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // The Root screen runs without a visible navigation bar (matching
    // the macOS sibling demo's chromeless look); pushed children
    // (e.g. Panel B) will surface the bar again automatically.
    [self.navigationController setNavigationBarHidden:YES animated:animated];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    // Bring the navigation bar back when pushing to Panel B — the
    // child screen needs the back button and the title.
    [self.navigationController setNavigationBarHidden:NO animated:animated];
}

- (void)dealloc {
    if (_vm) {
        _vm->deactivate();
        _vm.reset();
    }
    // Tear down in the documented order: bag → engine → adapter → views.
    _bag.clear();
    _engine.reset();
    _adapter.reset();
    _views.clear();
    [self.rootView hideToastNow];
    for (UIViewController *vc in self.playgroundControllers) {
        [vc dismissViewControllerAnimated:NO completion:nil];
    }
    [self.playgroundControllers removeAllObjects];
}

#pragma mark - ViewModel setup

- (void)setupViewModel {
    auto model = std::make_shared<DataModel>();
    _vm = MainViewModel::create(std::move(model));

    // Route VM callbacks back to main thread (the Property setters they
    // wake up will then synchronously drive the BindingEngine, which is
    // safe on UIKit's main thread).
    __weak RootViewController *weakSelf = self;
    _vm->main_dispatcher = [weakSelf](std::function<void()> work) {
        dispatch_async(dispatch_get_main_queue(), ^{ work(); });
    };

    _vm->activate();
}

#pragma mark - Binding engine setup

- (void)setupBindingEngine {
    _adapter = std::make_shared<aria::adapters::uikit::UIKitAdapter>();
    _engine  = std::make_unique<aria::binding::BindingEngine>(_adapter);
}

// Helper: wrap a raw UIView into a UIKitView and stash ownership in
// _views so the wrapper outlives no longer than the controller.
- (aria::binding::IView&)wrapView:(UIView *)uiView {
    auto wrapper = std::make_unique<aria::adapters::uikit::UIKitView>(uiView);
    auto& ref = *wrapper;
    _views.push_back(std::move(wrapper));
    return ref;
}

#pragma mark - Bindings: VM properties → labels (one-way)

- (void)bindStateToLabels {
    RootView *view = self.rootView;

    // status_text → statusLabel  (UILabel — strictly output-only).
    _engine->bind_text_oneway(_vm->status_text,
                              [self wrapView:view.statusLabel]);

    // result_text → resultLabel + toast side-effect.
    // The label is a clean BindingEngine binding; the toast is an extra
    // side-effect we keep on the side bag because it isn't a
    // "VM ↔ widget property" relationship — it's a transient UI affordance.
    _engine->bind_text_oneway(_vm->result_text,
                              [self wrapView:view.resultLabel]);

    __weak RootView *weakView = view;
    _bag += _vm->result_text.on_changed([weakView](const std::string& val) {
        NSString *ns = [NSString stringWithUTF8String:val.c_str()];
        // Re-dispatch to main thread defensively — Property may fire on
        // any thread that calls .set(); the dispatcher in setupViewModel
        // already lands us here on most paths, but the assert is cheap.
        dispatch_async(dispatch_get_main_queue(), ^{
            RootView *strongView = weakView;
            if (!strongView) return;
            [strongView showToast:ns];
        });
    });
}

#pragma mark - Bindings: pattern buttons → fetch_cmd (real framework binding)

- (void)bindPatternButtons {
    NSArray<UIButton *> *buttons = self.rootView.patternButtons;
    NSArray<NSString *> *tags = @[@"Block", @"Notification", @"KVO",
                                  @"Delegate", @"TargetAction"];
    for (NSUInteger i = 0; i < buttons.count && i < tags.count; i++) {
        UIButton *btn = buttons[i];
        std::string tag = [tags[i] UTF8String];
        // Real BindingEngine::bind_command — no hand-rolled UIKitClickWrapper,
        // no objc_setAssociatedObject, no @selector(fire). The engine
        // wires button.touchUpInside → fetch_cmd.execute(tag) and also
        // drives button.enabled from cmd.can_execute().
        _engine->bind_command(_vm->fetch_cmd,
                              [self wrapView:btn],
                              tag);
    }
}

#pragma mark - Bindings: playground buttons → open panel
//
// These intentionally do NOT go through BindingEngine::bind_command —
// presenting a panel is purely a view-side navigation concern, there
// is no VM piece to bind to. We still go through the UIKitAdapter's
// `on_click` so the framework's click pipeline is what's running, not
// hand-rolled `addTarget:action:`.

- (void)bindPlaygroundButtons {
    NSArray<UIButton *> *buttons = self.rootView.playgroundButtons;
    if (buttons.count < 4) return;

    // Each button has its own dedicated handler — A/B/C/D are four
    // independent demos with different presentation styles.
    SEL selectors[4] = {
        @selector(openPanelA),
        @selector(openPanelB),
        @selector(openPanelC),
        @selector(openPanelD),
    };

    auto& adapter = _engine->adapter();
    __weak RootViewController *weakSelf = self;
    for (NSUInteger i = 0; i < 4; i++) {
        UIButton *btn = buttons[i];
        SEL action = selectors[i];

        _bag += adapter.on_click([self wrapView:btn],
            [weakSelf, action]() {
                RootViewController *strongSelf = weakSelf;
                if (!strongSelf) return;
                // Dispatch to per-panel method via dynamic IMP.
                IMP imp = [strongSelf methodForSelector:action];
                void (*func)(id, SEL) = (void (*)(id, SEL))imp;
                func(strongSelf, action);
            });
    }
}

#pragma mark - Playground (4 independent panels)

// Panel A — half-screen formSheet (matches historical behaviour);
// the content is a simple text editor.
- (void)openPanelA {
    FreePlaygroundController *vc = [[FreePlaygroundController alloc] init];
    vc.title = @"Panel A";

    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:vc];
    nav.modalPresentationStyle = UIModalPresentationFormSheet;
    vc.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                      target:self
                                                      action:@selector(dismissPresented:)];
    vc.navigationItem.rightBarButtonItem =
    [[UIBarButtonItem alloc] initWithTitle:@"Clear"
                                     style:UIBarButtonItemStylePlain
                                    target:vc
                                    action:@selector(onClearTapped:)];

    [self.playgroundControllers addObject:nav];
    [self presentViewController:nav animated:YES completion:nil];
}

// Panel B — full-screen push. Requires the current VC to be embedded
// in a UINavigationController (SceneDelegate wraps RootVC for us).
// The navigation bar provides the back button automatically.
- (void)openPanelB {
    LayoutPlaygroundController *vc = [[LayoutPlaygroundController alloc] init];
    UINavigationController *nav = self.navigationController;
    if (!nav) {
        // Fallback: when not embedded in a nav, degrade to a full-screen
        // modal so tapping B still does something visible.
        UINavigationController *tmp = [[UINavigationController alloc]            initWithRootViewController:vc];
        tmp.modalPresentationStyle = UIModalPresentationFullScreen;
        vc.navigationItem.leftBarButtonItem =
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                          target:self
                                                          action:@selector(dismissPresented:)];
        [self.playgroundControllers addObject:tmp];
        [self presentViewController:tmp animated:YES completion:nil];
        return;
    }
    [nav pushViewController:vc animated:YES];
}

// Panel C — full-screen pageSheet (iOS 15+ default style, swipeable to
// dismiss). The top bar provides a Close button.
- (void)openPanelC {
    ListPlaygroundController *vc = [[ListPlaygroundController alloc] init];
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:vc];
    nav.modalPresentationStyle = UIModalPresentationPageSheet;
    vc.navigationItem.leftBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                      target:self
                                                      action:@selector(dismissPresented:)];

    [self.playgroundControllers addObject:nav];
    [self presentViewController:nav animated:YES completion:nil];
}

// Panel D — full-screen push (same shape as Panel B). Topic: ScrollView
//           + AutoLayout. We pick push rather than modal so the user
//           can see the nav bar and the back gesture, and so the
//           transition style differs from Panel C (which is a sheet).
- (void)openPanelD {
    ScrollPlaygroundController *vc = [[ScrollPlaygroundController alloc] init];
    UINavigationController *nav = self.navigationController;
    if (!nav) {
        // Fallback: when not embedded in a nav, degrade to a full-screen
        // modal and tack on an explicit Close button.
        UINavigationController *tmp = [[UINavigationController alloc]            initWithRootViewController:vc];
        tmp.modalPresentationStyle = UIModalPresentationFullScreen;
        vc.navigationItem.leftBarButtonItem =
            [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                          target:self
                                                          action:@selector(dismissPresented:)];
        [self.playgroundControllers addObject:tmp];
        [self presentViewController:tmp animated:YES completion:nil];
        return;
    }
    [nav pushViewController:vc animated:YES];
}

- (void)dismissPresented:(UIBarButtonItem *)sender {
    UIViewController *presented = self.presentedViewController;
    if (!presented) return;
    __weak RootViewController *weakSelf = self;
    [presented dismissViewControllerAnimated:YES completion:^{
        [weakSelf.playgroundControllers removeObject:presented];
    }];
}

@end
