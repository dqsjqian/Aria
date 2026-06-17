#import "RootView.h"

// ═════════════════════════════════════════════════════════════════════════════
// RootView — all the AppKit layout code lives here.
// See RootView.h for the contract with RootViewController.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootView ()
// Expose the outlets as readwrite internally.
@property (nonatomic, strong, readwrite) NSTextField *statusLabel;
@property (nonatomic, strong, readwrite) NSTextField *resultLabel;
@property (nonatomic, strong, readwrite) NSTextField *toastLabel;
@property (nonatomic, strong, readwrite) NSArray<NSButton *> *patternButtons;
@property (nonatomic, strong, readwrite) NSArray<NSButton *> *playgroundButtons;

@property (nonatomic, strong) NSTimer *toastTimer;
@end

@implementation RootView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        [self buildSubviews];
    }
    return self;
}

#pragma mark - Layout

- (void)buildSubviews {
    CGFloat W = 480;
    CGFloat L = 20;

    // ── Title ────────────────────────────────────────────────────────────────
    [self addSubview:[self labelWith:@"MVVM + aria Demo"
                                frame:NSMakeRect(L, 650, W, 28)
                                  size:18 bold:YES]];

    // ── Subtitle ─────────────────────────────────────────────────────────────
    NSTextField *subtitle = [self labelWith:@"Powered by C++ aria framework"
                                      frame:NSMakeRect(L, 625, W, 18)
                                        size:11 bold:NO];
    subtitle.textColor = [NSColor tertiaryLabelColor];
    [self addSubview:subtitle];

    // ── Status label ─────────────────────────────────────────────────────────
    self.statusLabel = [self labelWith:@"Tap a button"
                                 frame:NSMakeRect(L, 595, W, 22)
                                   size:12 bold:NO];
    self.statusLabel.textColor = [NSColor secondaryLabelColor];
    [self addSubview:self.statusLabel];

    // ── Result label ─────────────────────────────────────────────────────────
    self.resultLabel = [self labelWith:@""
                                 frame:NSMakeRect(L, 560, W, 28)
                                   size:14 bold:NO];
    self.resultLabel.textColor = [NSColor systemGreenColor];
    [self addSubview:self.resultLabel];

    // ── 5 pattern buttons ────────────────────────────────────────────────────
    self.patternButtons = [self buildPatternButtonsAt:520 width:W leftX:L];

    // ── Separator + legend ───────────────────────────────────────────────────
    CGFloat sepY = 520 - 2 * (32 + 10) - 15;
    [self addSubview:[self separatorAt:NSMakeRect(L, sepY, W, 1)]];

    NSString *legend =
        @"Flow: View → Command → ViewModel → Model(2s) → Property → View\n"
         "All 5 OC patterns unified into:\n"
         "  aria::Property<string>  (replaces KVO/Block/Notification/Delegate)\n"
         "  aria::Command<string>   (replaces Target-Action)";
    NSTextField *legendLabel = [self labelWith:legend
                                         frame:NSMakeRect(L, sepY - 75, W, 70)
                                           size:10 bold:NO];
    legendLabel.textColor = [NSColor tertiaryLabelColor];
    [self addSubview:legendLabel];

    // ── Toast (below legend) ─────────────────────────────────────────────────
    self.toastLabel = [self buildToastLabelAt:NSMakeRect(L, 210, W, 36)];
    [self addSubview:self.toastLabel];

    // ── Playground (very bottom) ─────────────────────────────────────────────
    self.playgroundButtons = [self buildPlaygroundSectionAt:150 width:W leftX:L];
}

- (NSArray<NSButton *> *)buildPatternButtonsAt:(CGFloat)topY
                                          width:(CGFloat)W
                                          leftX:(CGFloat)L {
    CGFloat btnH = 32;
    CGFloat gap = 10;

    struct BtnInfo { NSString *title; CGFloat x; CGFloat w; };
    BtnInfo info[5] = {
        {@"Fetch (Block)",         L,   230},
        {@"Fetch (Notification)",  260, 240},
        {@"Fetch (KVO)",           L,   230},
        {@"Fetch (Delegate)",      260, 240},
        {@"Fetch (Target-Action)", L,   W  },
    };

    NSMutableArray<NSButton *> *out = [NSMutableArray arrayWithCapacity:5];
    for (int i = 0; i < 5; i++) {
        CGFloat y = topY - (i / 2) * (btnH + gap);
        if (i == 4) y = topY - 2 * (btnH + gap);

        NSButton *btn = [NSButton buttonWithTitle:info[i].title
                                            target:nil
                                            action:nil];
        btn.frame = NSMakeRect(info[i].x, y, info[i].w, btnH);
        btn.bezelStyle = NSBezelStyleRounded;
        [self addSubview:btn];
        [out addObject:btn];
    }
    return [out copy];
}

- (NSArray<NSButton *> *)buildPlaygroundSectionAt:(CGFloat)sepY
                                             width:(CGFloat)W
                                             leftX:(CGFloat)L {
    [self addSubview:[self separatorAt:NSMakeRect(L, sepY, W, 1)]];

    NSTextField *title = [self labelWith:@"🧪 Playground — 空白面板随你发挥"
                                   frame:NSMakeRect(L, sepY - 26, W, 20)
                                     size:13 bold:YES];
    title.textColor = [NSColor labelColor];
    [self addSubview:title];

    NSTextField *hint = [self labelWith:@"点击按钮会弹出一个空白独立窗口，可在里面自由添加 UI / C++ VM / 绑定等实验代码。"
                                  frame:NSMakeRect(L, sepY - 46, W, 18)
                                    size:10 bold:NO];
    hint.textColor = [NSColor tertiaryLabelColor];
    [self addSubview:hint];

    CGFloat btnY = sepY - 92;
    CGFloat btnH = 32;
    CGFloat btnW = (W - 2 * 10) / 3.0;
    NSString *titles[3] = {@"Panel A", @"Panel B", @"Panel C"};

    NSMutableArray<NSButton *> *out = [NSMutableArray arrayWithCapacity:3];
    for (int i = 0; i < 3; i++) {
        NSButton *btn = [NSButton buttonWithTitle:titles[i] target:nil action:nil];
        btn.frame = NSMakeRect(L + i * (btnW + 10), btnY, btnW, btnH);
        btn.bezelStyle = NSBezelStyleRounded;
        [self addSubview:btn];
        [out addObject:btn];
    }
    return [out copy];
}

#pragma mark - Toast

- (NSTextField *)buildToastLabelAt:(NSRect)frame {
    NSTextField *label = [self labelWith:@""
                                   frame:frame
                                    size:13 bold:NO];
    label.alignment = NSTextAlignmentCenter;
    label.wantsLayer = YES;
    label.layer.backgroundColor = [[NSColor colorWithCalibratedRed:0.12
                                                             green:0.12
                                                              blue:0.12
                                                             alpha:0.92] CGColor];
    label.layer.cornerRadius = 8;
    label.textColor = [NSColor whiteColor];
    label.hidden = YES;
    return label;
}

- (void)showToast:(NSString *)message {
    [self.toastTimer invalidate];
    self.toastLabel.stringValue = message ?: @"";
    self.toastLabel.hidden = NO;

    __weak RootView *weakSelf = self;
    self.toastTimer = [NSTimer scheduledTimerWithTimeInterval:4.0
                                                       repeats:NO
                                                         block:^(NSTimer *t) {
        [weakSelf hideToastNow];
    }];
}

- (void)hideToastNow {
    [self.toastTimer invalidate];
    self.toastTimer = nil;
    self.toastLabel.hidden = YES;
}

#pragma mark - Helpers

- (NSTextField *)labelWith:(NSString *)text
                      frame:(NSRect)frame
                        size:(CGFloat)size
                        bold:(BOOL)bold {
    NSTextField *label = [NSTextField labelWithString:text];
    label.frame = frame;
    label.font = [NSFont monospacedSystemFontOfSize:size
                                              weight:bold ? NSFontWeightBold : NSFontWeightRegular];
    label.textColor = [NSColor labelColor];
    label.lineBreakMode = NSLineBreakByWordWrapping;
    label.usesSingleLineMode = NO;
    return label;
}

- (NSBox *)separatorAt:(NSRect)frame {
    NSBox *sep = [[NSBox alloc] initWithFrame:frame];
    sep.boxType = NSBoxSeparator;
    return sep;
}

- (void)dealloc {
    [self.toastTimer invalidate];
}

@end
