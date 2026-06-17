#import "FreePlaygroundView.h"

// ═════════════════════════════════════════════════════════════════════════════
// FreePlaygroundView.mm
// Pure UI: only "places the controls". Every event / data path is
// handled by the controller.
// ═════════════════════════════════════════════════════════════════════════════

@interface FreePlaygroundView ()
@property (nonatomic, strong, readwrite) UITextView *textView;
@end

@implementation FreePlaygroundView

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        [self setup];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder {
    if ((self = [super initWithCoder:coder])) {
        [self setup];
    }
    return self;
}

#pragma mark - Setup

- (void)setup {
    self.backgroundColor = [UIColor systemBackgroundColor];

    // ── Text editing area ────────────────────────────────────────────
    self.textView = [[UITextView alloc] init];
    self.textView.font = [UIFont monospacedSystemFontOfSize:13
                                                     weight:UIFontWeightRegular];
    self.textView.text =
        @"// Panel A — Free Playground (ObjC++)\n"
         "//\n"
         "// 这是【A 面板】专属的简单样本，半屏 formSheet 呈现。\n"
         "// 一个 UITextView 铺满 safe area，没有其它逻辑。\n"
         "//\n"
         "// 你可以把 Aria 的 C++ ViewModel 接进来：\n"
         "//   - UIKitAdapter 做 Property ↔ UIKit 双向绑定\n"
         "//   - Command 挂给按钮触发\n";
    self.textView.alwaysBounceVertical = YES;
    self.textView.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:self.textView];

    // ── Auto Layout: pin textView top/bottom to the safe area, and
    //                left/right to self for full-bleed width.
    UILayoutGuide *safe = self.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.textView.topAnchor      constraintEqualToAnchor:safe.topAnchor],
        [self.textView.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [self.textView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.textView.bottomAnchor   constraintEqualToAnchor:safe.bottomAnchor],
    ]];
}

@end
