#import "RootViewController.h"
#import "RootView.h"
#import "AppKitAdapter.hpp"
#import "MainViewModel.hpp"
#import "FreePlaygroundController.h"

#import <objc/runtime.h>

#include "aria/binding/binding_engine.hpp"

#include <memory>
#include <vector>

// ═════════════════════════════════════════════════════════════════════════════
// RootViewController — assembly + binding + business logic.
//
// View is in RootView (pure AppKit, no VM, no bindings).
// Controller responsibilities:
//   1. Create & own the C++ ViewModel (MainViewModel)
//   2. Create & own the RootView
//   3. Wire the BindingEngine: VM properties ↔ AppKitAdapter (real framework
//      use — not hand-rolled KVO/target-action).
//   4. Bind View buttons → VM commands / open-panel handlers
//   5. Drive lifecycle (activate / deactivate VM, clean up windows)
//
// RootViewController is a faithful demonstration of the
// framework: every "VM → label" is a `BindingEngine::bind_text_oneway`,
// every "button → VM" is a `BindingEngine::bind_command`. The only
// hand-rolled glue left is for purely view-local navigation (opening
// playground panels), where there is no VM piece to bind to.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootViewController ()
@property (nonatomic, strong) RootView *rootView;
@property (nonatomic, strong) NSMutableArray<NSWindow *> *playgroundWindows;
@end

@implementation RootViewController {
    std::shared_ptr<MainViewModel>                        _vm;

    // Binding plumbing — real framework wiring (replaces hand-rolled
    // KVO + target-action + objc_setAssociatedObject wrapper retain).
    //
    // Declaration order matters for destruction:
    //   1. _engine dies first → drops every Subscription, detaches
    //      adapter slots from native widgets.
    //   2. _adapter dies next → its Bridge map (per-view ObjC targets)
    //      goes away; native widgets that survive the controller are
    //      already cleaned up.
    //   3. _views die last → ~AppKitView fires on_destroy; by now
    //      _engine is gone so the engine-side handler is a no-op (the
    //      subscription was already detached). Safe.
    std::shared_ptr<aria::adapters::appkit::AppKitAdapter> _adapter;
    std::unique_ptr<aria::binding::BindingEngine>          _engine;
    std::vector<std::unique_ptr<aria::adapters::appkit::AppKitView>> _views;

    // Side-effect subscriptions that are *not* part of the binding graph
    // (e.g. "show toast on result_text change"). Kept in a SubscriptionBag
    // because we want them released when the controller goes away even if
    // the underlying NSView lives on (toast handler captures `weakView`).
    aria::SubscriptionBag                                  _bag;
}

#pragma mark - View lifecycle

- (void)loadView {
    self.rootView = [[RootView alloc]
        initWithFrame:NSMakeRect(0, 0, 520, 720)];
    self.view = self.rootView;
    self.playgroundWindows = [NSMutableArray new];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self setupViewModel];
    [self setupBindingEngine];
    [self bindStateToLabels];
    [self bindPatternButtons];
    [self bindPlaygroundButtons];
}

- (void)dealloc {
    if (_vm) {
        _vm->deactivate();
        _vm.reset();
    }
    // Tear down in the documented order: engine → adapter → views.
    _bag.clear();
    _engine.reset();
    _adapter.reset();
    _views.clear();
    [self.rootView hideToastNow];
    for (NSWindow *w in self.playgroundWindows) {
        [w close];
    }
    [self.playgroundWindows removeAllObjects];
}

#pragma mark - ViewModel setup

- (void)setupViewModel {
    auto model = std::make_shared<DataModel>();
    _vm = MainViewModel::create(std::move(model));

    // Route VM callbacks back to main thread (the Property setters they
    // wake up will then synchronously drive the BindingEngine, which is
    // safe on AppKit's main thread).
    __weak RootViewController *weakSelf = self;
    _vm->main_dispatcher = [weakSelf](std::function<void()> work) {
        dispatch_async(dispatch_get_main_queue(), ^{ work(); });
    };

    _vm->activate();
}

#pragma mark - Binding engine setup

- (void)setupBindingEngine {
    _adapter = std::make_shared<aria::adapters::appkit::AppKitAdapter>();
    _engine  = std::make_unique<aria::binding::BindingEngine>(_adapter);
}

// Helper: wrap a raw NSView into an AppKitView and stash ownership in
// _views so the wrapper outlives no longer than the controller.
- (aria::binding::IView&)wrapView:(NSView *)nsView {
    auto wrapper = std::make_unique<aria::adapters::appkit::AppKitView>(nsView);
    auto& ref = *wrapper;
    _views.push_back(std::move(wrapper));
    return ref;
}

#pragma mark - Bindings: VM properties → labels (one-way)

- (void)bindStateToLabels {
    RootView *view = self.rootView;

    // status_text → statusLabel  (one-way: NSTextField labelWithString
    // is not editable, no view-side write-back ever happens).
    _engine->bind_text_oneway(_vm->status_text,
                              [self wrapView:view.statusLabel]);

    // result_text → resultLabel + toast side-effect.
    // The label part is a clean BindingEngine binding; the toast is an
    // extra side-effect we keep on the side bag because it isn't a
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
    NSArray<NSButton *> *buttons = self.rootView.patternButtons;
    NSArray<NSString *> *tags = @[@"Block", @"Notification", @"KVO",
                                  @"Delegate", @"TargetAction"];
    for (NSUInteger i = 0; i < buttons.count && i < tags.count; i++) {
        NSButton *btn = buttons[i];
        std::string tag = [tags[i] UTF8String];
        // Real BindingEngine::bind_command — no hand-rolled wrapper, no
        // objc_setAssociatedObject, no @selector(fire). The engine
        // wires button.click → fetch_cmd.execute(tag) and also drives
        // button.enabled from cmd.can_execute().
        _engine->bind_command(_vm->fetch_cmd,
                              [self wrapView:btn],
                              tag);
    }
}

#pragma mark - Bindings: playground buttons → open panel
//
// These intentionally do NOT go through BindingEngine::bind_command —
// opening a window is purely a view-side navigation concern, there is
// no VM piece to bind to. We still go through the AppKitAdapter's
// `on_click` so the framework's click pipeline is what's running, not
// hand-rolled NSButton target-action.

- (void)bindPlaygroundButtons {
    NSArray<NSButton *> *buttons = self.rootView.playgroundButtons;
    NSArray<NSString *> *titles = @[@"Playground A", @"Playground B", @"Playground C"];

    auto& adapter = _engine->adapter();
    for (NSUInteger i = 0; i < buttons.count && i < titles.count; i++) {
        NSButton *btn = buttons[i];
        NSString *title = titles[i];
        char tag = 'A' + (char)i;
        __weak RootViewController *weakSelf = self;

        _bag += adapter.on_click([self wrapView:btn],
            [weakSelf, title, tag]() {
                [weakSelf openPlaygroundWithTitle:title tag:tag];
            });
    }
}

#pragma mark - Playground (3 blank panels)

- (void)openPlaygroundWithTitle:(NSString *)title tag:(char)tag {
    FreePlaygroundController *vc = [[FreePlaygroundController alloc] init];

    // Stagger the panel origins so they don't stack perfectly on top.
    CGFloat offsetX = 60 + (tag - 'A') * 30;
    CGFloat offsetY = 60 + (tag - 'A') * 30;
    NSRect frame = NSMakeRect(offsetX, 600 - offsetY, 520, 420);

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled
                           | NSWindowStyleMaskClosable
                           | NSWindowStyleMaskMiniaturizable
                           | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = title;
    win.releasedWhenClosed = NO;
    win.contentViewController = vc;

    __weak RootViewController *weakSelf = self;
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowWillCloseNotification
                    object:win
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
        [weakSelf.playgroundWindows removeObject:note.object];
    }];

    [self.playgroundWindows addObject:win];
    [win makeKeyAndOrderFront:nil];
}

@end
