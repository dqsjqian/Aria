#import "ScrollPlaygroundController.h"
#import "ScrollPlaygroundView.h"

// ═════════════════════════════════════════════════════════════════════════════════════════
// ScrollPlaygroundController.mm — thin controller.
//
// The MVVM split is explicit here:
//   * View       : every UIKit control + Masonry constraint
//                  (ScrollPlaygroundView).
//   * Controller : the state machine (_bioExpanded) and event
//                  routing (toggleBtn → onToggleTapped).
//
// Future iteration: replace _bioExpanded with `aria::Property<bool>`
// so Aria's reactive pipeline drives the view refresh, lifting the
// last bit of "state" out of the controller.
// ═════════════════════════════════════════════════════════════════════════════

@interface ScrollPlaygroundController ()
@property (nonatomic, assign) BOOL bioExpanded;
@end

@implementation ScrollPlaygroundController

#pragma mark - View lifecycle

- (void)loadView {
    // Recommended pattern: replace the default `view` inside loadView,
    // never inside viewDidLoad. Replacing self.view from viewDidLoad
    // re-triggers viewDidLoad and recurses indefinitely.
    self.view = [[ScrollPlaygroundView alloc] init];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Panel D · ScrollView";
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    [self bindActions];
    self.bioExpanded = NO;
    [self refreshBio];
}

#pragma mark - Typed accessor

// Cast self.view to the concrete subclass once, so each method below
// can read `self.rootView` instead of `(ScrollPlaygroundView *)self.view`.
- (ScrollPlaygroundView *)rootView {
    return (ScrollPlaygroundView *)self.view;
}

#pragma mark - Bind actions

- (void)bindActions {
    [self.rootView.toggleBtn addTarget:self
                                action:@selector(onToggleTapped:)
                      forControlEvents:UIControlEventTouchUpInside];
}

#pragma mark - Actions

- (void)onToggleTapped:(UIButton *)sender {
    self.bioExpanded = !self.bioExpanded;
    [self refreshBio];
}

#pragma mark - State → View

- (void)refreshBio {
    static NSString *const kBio =
        @"我是 dqsjqian，一个深耕 C++ 多年的桌面/客户端工程师，正在把 Aria —— "
         "一个用 Modern C++20 写成的 MVVM 框架 —— 同时跑在 Qt / AppKit / UIKit "
         "三个平台上。Day 2 的目标是把 iOS 的 AutoLayout 和 Masonry 吃透，从"
         "「会抄模板」进化到「能独立写出任意布局」。这段 bio 故意写长，方便演示 "
         "ScrollView 嵌套时的自适应高度，以及 mas_updateConstraints 切换"
         "「展开 / 收起」的动态约束效果。如果你看到这一行，说明已经成功展开了 🎉";

    self.rootView.bioLabel.text = kBio;

    // Toggle the height cap: collapsed = 64pt (~3-4 lines) vs.
    // expanded = CGFLOAT_MAX (uncapped).
    [self.rootView updateBioMaxHeight:self.bioExpanded ? CGFLOAT_MAX : 64];

    [self.rootView.toggleBtn setTitle:self.bioExpanded ? @"收起" : @"展开"
                             forState:UIControlStateNormal];
}

@end
