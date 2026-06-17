#import "RootView.h"

// ═════════════════════════════════════════════════════════════════════════════
// RootView — all the UIKit layout code lives here.
// See RootView.h for the contract with RootViewController.
//
// Layout strategy (mental-model differences vs the AppKit sibling demo):
//   - The AppKit version uses hand-rolled NSMakeRect with Y pointing up.
//   - This UIKit version uses Auto Layout (NSLayoutAnchor) with Y down.
//   - Top-to-bottom flow: title -> subtitle -> status -> result ->
//     5 pattern buttons -> separator -> hint text -> playground area
//     (title + 3 buttons) -> floating toast (pinned to the bottom).
//   - safeAreaLayoutGuide is used everywhere to dodge the notch /
//     home indicator.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootView ()
// Expose the outlets as readwrite internally.
@property (nonatomic, strong, readwrite) UILabel *statusLabel;
@property (nonatomic, strong, readwrite) UILabel *resultLabel;
@property (nonatomic, strong, readwrite) UILabel *toastLabel;
@property (nonatomic, strong, readwrite) NSArray<UIButton *> *patternButtons;
@property (nonatomic, strong, readwrite) NSArray<UIButton *> *playgroundButtons;

@property (nonatomic, strong) NSTimer *toastTimer;
@end

@implementation RootView

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor systemBackgroundColor];
        [self buildSubviews];
    }
    return self;
}

#pragma mark - Layout

- (void)buildSubviews {
    UILayoutGuide *safe = self.safeAreaLayoutGuide;

    // ── Title ────────────────────────────────────────────────────────────────
    UILabel *title = [self labelWithText:@"MVVM + aria Demo (iOS)"
                                    size:20
                                    bold:YES
                                   color:[UIColor labelColor]];
    title.textAlignment = NSTextAlignmentLeft;
    [self addSubview:title];

    // ── Subtitle ─────────────────────────────────────────────────────────────
    UILabel *subtitle = [self labelWithText:@"Powered by C++ aria framework"
                                       size:12
                                       bold:NO
                                      color:[UIColor tertiaryLabelColor]];
    [self addSubview:subtitle];

    // ── Status label ─────────────────────────────────────────────────────────
    self.statusLabel = [self labelWithText:@"Tap a button"
                                      size:13
                                      bold:NO
                                     color:[UIColor secondaryLabelColor]];
    [self addSubview:self.statusLabel];

    // ── Result label ─────────────────────────────────────────────────────────
    self.resultLabel = [self labelWithText:@""
                                      size:14
                                      bold:NO
                                     color:[UIColor systemGreenColor]];
    self.resultLabel.numberOfLines = 0;
    [self addSubview:self.resultLabel];

    // ── 5 pattern buttons (grid: 2x2 + 1 full-width) ─────────────────────────
    self.patternButtons = [self buildPatternButtons];

    // ── Separator 1 ─────────────────────────────────────────────────────────
    UIView *sep1 = [self separator];
    [self addSubview:sep1];

    // ── Legend ───────────────────────────────────────────────────────────────
    UILabel *legend = [self labelWithText:
        @"Flow: View → Command → ViewModel → Model(2s) → Property → View\n"
         "All 5 OC patterns unified into:\n"
         "  aria::Property<string>  (replaces KVO/Block/Notification/Delegate)\n"
         "  aria::Command<string>   (replaces Target-Action)"
                                     size:11
                                     bold:NO
                                    color:[UIColor tertiaryLabelColor]];
    legend.numberOfLines = 0;
    [self addSubview:legend];

    // ── Separator 2 ─────────────────────────────────────────────────────────
    UIView *sep2 = [self separator];
    [self addSubview:sep2];

    // ── Playground section ──────────────────────────────────────────────────
    UILabel *pgTitle = [self labelWithText:@"🧪 Playground — 空白面板随你发挥"
                                      size:14
                                      bold:YES
                                     color:[UIColor labelColor]];
    [self addSubview:pgTitle];

    UILabel *pgHint = [self labelWithText:@"点击按钮会 modal 弹出一个空白独立页面，可在里面自由添加 UI / C++ VM / 绑定等实验代码。"
                                     size:11
                                     bold:NO
                                    color:[UIColor tertiaryLabelColor]];
    pgHint.numberOfLines = 0;
    [self addSubview:pgHint];

    self.playgroundButtons = [self buildPlaygroundButtons];

    // ── Toast (bottom floating) ──────────────────────────────────────────────
    self.toastLabel = [self buildToastLabel];
    [self addSubview:self.toastLabel];

    // ── Auto Layout ──────────────────────────────────────────────────────────
    CGFloat L = 20;  // horizontal inset
    CGFloat gap = 8;

    [NSLayoutConstraint activateConstraints:@[
        // Title
        [title.topAnchor      constraintEqualToAnchor:safe.topAnchor constant:16],
        [title.leadingAnchor  constraintEqualToAnchor:safe.leadingAnchor constant:L],
        [title.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-L],

        // Subtitle
        [subtitle.topAnchor      constraintEqualToAnchor:title.bottomAnchor constant:4],
        [subtitle.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [subtitle.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],

        // Status
        [self.statusLabel.topAnchor      constraintEqualToAnchor:subtitle.bottomAnchor constant:12],
        [self.statusLabel.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [self.statusLabel.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],

        // Result
        [self.resultLabel.topAnchor      constraintEqualToAnchor:self.statusLabel.bottomAnchor constant:6],
        [self.resultLabel.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [self.resultLabel.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
    ]];

    // Pattern buttons grid (2x2 + 1 full-width)
    UIButton *b0 = self.patternButtons[0];  // Block
    UIButton *b1 = self.patternButtons[1];  // Notification
    UIButton *b2 = self.patternButtons[2];  // KVO
    UIButton *b3 = self.patternButtons[3];  // Delegate
    UIButton *b4 = self.patternButtons[4];  // TargetAction (full-width)
    CGFloat btnH = 40;

    [NSLayoutConstraint activateConstraints:@[
        // Row 1
        [b0.topAnchor      constraintEqualToAnchor:self.resultLabel.bottomAnchor constant:16],
        [b0.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [b0.heightAnchor   constraintEqualToConstant:btnH],

        [b1.topAnchor      constraintEqualToAnchor:b0.topAnchor],
        [b1.leadingAnchor  constraintEqualToAnchor:b0.trailingAnchor constant:gap],
        [b1.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [b1.heightAnchor   constraintEqualToConstant:btnH],
        [b1.widthAnchor    constraintEqualToAnchor:b0.widthAnchor],

        // Row 2
        [b2.topAnchor      constraintEqualToAnchor:b0.bottomAnchor constant:gap],
        [b2.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [b2.heightAnchor   constraintEqualToConstant:btnH],

        [b3.topAnchor      constraintEqualToAnchor:b2.topAnchor],
        [b3.leadingAnchor  constraintEqualToAnchor:b2.trailingAnchor constant:gap],
        [b3.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [b3.heightAnchor   constraintEqualToConstant:btnH],
        [b3.widthAnchor    constraintEqualToAnchor:b2.widthAnchor],

        // Row 3 (full-width)
        [b4.topAnchor      constraintEqualToAnchor:b2.bottomAnchor constant:gap],
        [b4.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [b4.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [b4.heightAnchor   constraintEqualToConstant:btnH],

        // Separator 1
        [sep1.topAnchor      constraintEqualToAnchor:b4.bottomAnchor constant:18],
        [sep1.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [sep1.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [sep1.heightAnchor   constraintEqualToConstant:1],

        // Legend
        [legend.topAnchor      constraintEqualToAnchor:sep1.bottomAnchor constant:10],
        [legend.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [legend.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],

        // Separator 2
        [sep2.topAnchor      constraintEqualToAnchor:legend.bottomAnchor constant:18],
        [sep2.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [sep2.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [sep2.heightAnchor   constraintEqualToConstant:1],

        // Playground title
        [pgTitle.topAnchor      constraintEqualToAnchor:sep2.bottomAnchor constant:12],
        [pgTitle.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [pgTitle.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],

        // Playground hint
        [pgHint.topAnchor      constraintEqualToAnchor:pgTitle.bottomAnchor constant:4],
        [pgHint.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [pgHint.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
    ]];

    // Playground buttons row (2x2 grid)
    UIButton *pa = self.playgroundButtons[0];
    UIButton *pb = self.playgroundButtons[1];
    UIButton *pc = self.playgroundButtons[2];
    UIButton *pd = self.playgroundButtons[3];

    [NSLayoutConstraint activateConstraints:@[
        // Row 1: A | B
        [pa.topAnchor      constraintEqualToAnchor:pgHint.bottomAnchor constant:12],
        [pa.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [pa.heightAnchor   constraintEqualToConstant:btnH],

        [pb.topAnchor      constraintEqualToAnchor:pa.topAnchor],
        [pb.leadingAnchor  constraintEqualToAnchor:pa.trailingAnchor constant:gap],
        [pb.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [pb.heightAnchor   constraintEqualToConstant:btnH],
        [pb.widthAnchor    constraintEqualToAnchor:pa.widthAnchor],

        // Row 2: C | D
        [pc.topAnchor      constraintEqualToAnchor:pa.bottomAnchor constant:gap],
        [pc.leadingAnchor  constraintEqualToAnchor:title.leadingAnchor],
        [pc.heightAnchor   constraintEqualToConstant:btnH],
        [pc.widthAnchor    constraintEqualToAnchor:pa.widthAnchor],

        [pd.topAnchor      constraintEqualToAnchor:pc.topAnchor],
        [pd.leadingAnchor  constraintEqualToAnchor:pc.trailingAnchor constant:gap],
        [pd.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [pd.heightAnchor   constraintEqualToConstant:btnH],
        [pd.widthAnchor    constraintEqualToAnchor:pa.widthAnchor],

        // Toast — floats near the bottom of safe area
        [self.toastLabel.leadingAnchor  constraintEqualToAnchor:safe.leadingAnchor constant:L],
        [self.toastLabel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-L],
        [self.toastLabel.bottomAnchor   constraintEqualToAnchor:safe.bottomAnchor constant:-16],
        [self.toastLabel.heightAnchor   constraintGreaterThanOrEqualToConstant:44],
    ]];
}

#pragma mark - Builders

- (NSArray<UIButton *> *)buildPatternButtons {
    NSArray<NSString *> *titles = @[
        @"Fetch (Block)",
        @"Fetch (Notification)",
        @"Fetch (KVO)",
        @"Fetch (Delegate)",
        @"Fetch (Target-Action)",
    ];
    NSMutableArray<UIButton *> *out = [NSMutableArray arrayWithCapacity:titles.count];
    for (NSString *t in titles) {
        UIButton *btn = [self roundedButtonWithTitle:t];
        [self addSubview:btn];
        [out addObject:btn];
    }
    return [out copy];
}

- (NSArray<UIButton *> *)buildPlaygroundButtons {
    NSArray<NSString *> *titles = @[@"Panel A", @"Panel B", @"Panel C", @"Panel D"];
    NSMutableArray<UIButton *> *out = [NSMutableArray arrayWithCapacity:titles.count];
    for (NSString *t in titles) {
        UIButton *btn = [self roundedButtonWithTitle:t];
        [self addSubview:btn];
        [out addObject:btn];
    }
    return [out copy];
}

- (UILabel *)buildToastLabel {
    UILabel *label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.textAlignment = NSTextAlignmentCenter;
    label.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    label.textColor = [UIColor whiteColor];
    label.backgroundColor = [UIColor colorWithRed:0.12 green:0.12 blue:0.12 alpha:0.92];
    label.layer.cornerRadius = 8;
    label.layer.masksToBounds = YES;
    label.numberOfLines = 0;
    label.hidden = YES;
    label.alpha = 0.0;
    return label;
}

#pragma mark - Toast

- (void)showToast:(NSString *)message {
    [self.toastTimer invalidate];
    self.toastLabel.text = message ?: @"";
    self.toastLabel.hidden = NO;

    // Fade in.
    [UIView animateWithDuration:0.2 animations:^{
        self.toastLabel.alpha = 1.0;
    }];

    __weak RootView *weakSelf = self;
    self.toastTimer = [NSTimer scheduledTimerWithTimeInterval:4.0
                                                       repeats:NO
                                                         block:^(NSTimer *t) {
        [weakSelf fadeOutToast];
    }];
}

- (void)fadeOutToast {
    [UIView animateWithDuration:0.3
                     animations:^{
        self.toastLabel.alpha = 0.0;
    }
                     completion:^(BOOL finished) {
        if (self.toastLabel.alpha == 0.0) {
            self.toastLabel.hidden = YES;
        }
    }];
}

- (void)hideToastNow {
    [self.toastTimer invalidate];
    self.toastTimer = nil;
    self.toastLabel.alpha = 0.0;
    self.toastLabel.hidden = YES;
}

#pragma mark - Helpers

- (UILabel *)labelWithText:(NSString *)text
                      size:(CGFloat)size
                      bold:(BOOL)bold
                     color:(UIColor *)color {
    UILabel *label = [[UILabel alloc] init];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.text = text;
    label.font = [UIFont monospacedSystemFontOfSize:size
                                             weight:bold ? UIFontWeightBold : UIFontWeightRegular];
    label.textColor = color;
    label.numberOfLines = 1;
    return label;
}

- (UIView *)separator {
    UIView *sep = [[UIView alloc] init];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    sep.backgroundColor = [UIColor separatorColor];
    return sep;
}

- (UIButton *)roundedButtonWithTitle:(NSString *)title {
    // Use the legacy UIButton API for iOS 13+ compatibility
    // (UIButtonConfiguration is iOS 15+).
    UIButton *btn = [UIButton buttonWithType:UIButtonTypeSystem];
    btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn setTitle:title forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightMedium];

    // Mimic the macOS bezel rounded-button look.
    btn.backgroundColor = [UIColor secondarySystemBackgroundColor];
    btn.layer.cornerRadius = 8;
    btn.layer.borderWidth = 0.5;
    btn.layer.borderColor = [UIColor separatorColor].CGColor;
    [btn setTitleColor:[UIColor labelColor] forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor tertiaryLabelColor] forState:UIControlStateDisabled];
    return btn;
}

- (void)dealloc {
    [self.toastTimer invalidate];
}

@end
