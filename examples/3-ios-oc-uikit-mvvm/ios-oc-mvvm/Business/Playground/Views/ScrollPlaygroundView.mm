#import "ScrollPlaygroundView.h"
#import "Masonry.h"

// ═════════════════════════════════════════════════════════════════════════════════════════
// ScrollPlaygroundView.mm
//
// Topics demonstrated:
//   (1) The three hard rules of UIScrollView, applied in code
//       (see setupScrollContainer + setupConstraints).
//   (2) Seven Masonry idioms in practice: edges /
//       mas_safeAreaLayoutGuideTop / mas_equalTo / centerX /
//       equalTo(view.mas_bottom) / inset / priority.
//   (3) Dynamic constraints: updateBioMaxHeight: uses
//       mas_updateConstraints to change the height ceiling.
//   (4) UIStackView wrapping the tag-row buttons (auto equal-width
//       distribution + spacing).
// ═══════════════════════════════════════════════════════════════════════════════════════════

@interface ScrollPlaygroundView ()

// readwrite redeclarations of the public read-only properties
@property (nonatomic, strong, readwrite) UILabel  *bioLabel;
@property (nonatomic, strong, readwrite) UIButton *toggleBtn;
@property (nonatomic, strong, readwrite) NSArray<UIButton *> *tagButtons;

// Internal containers / helpers
@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIView       *contentView;
@property (nonatomic, strong) UIImageView  *avatarView;
@property (nonatomic, strong) UILabel      *nameLabel;
@property (nonatomic, strong) UIStackView  *tagRow;

// Cached height constraint of the bio label so updateBioMaxHeight: can update it.
@property (nonatomic, strong) MASConstraint *bioHeightConstraint;

@end

@implementation ScrollPlaygroundView

#pragma mark - Init

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

#pragma mark - Setup entry

- (void)setup {
    self.backgroundColor = [UIColor systemBackgroundColor];
    [self setupScrollContainer];   // hard rules (1) and (2)
    [self setupAvatar];
    [self setupName];
    [self setupBio];
    [self setupToggleButton];
    [self setupTagRow];
    [self setupConstraints];        // chain everything together — hard rule (3)
    [self setupGesture];
}

#pragma mark - (1) Scroll container

- (void)setupScrollContainer {
    self.scrollView = [[UIScrollView alloc] init];
    // Always allow a vertical bounce, even when the content is shorter
    // than the viewport — useful for getting a feel for the scroll path.
    self.scrollView.alwaysBounceVertical = YES;
    self.scrollView.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    [self addSubview:self.scrollView];

    self.contentView = [[UIView alloc] init];
    [self.scrollView addSubview:self.contentView];
}

#pragma mark - (2) Subview construction (no constraints yet, just create + addSubview)

- (void)setupAvatar {
    self.avatarView = [[UIImageView alloc] init];
    self.avatarView.backgroundColor = [UIColor systemTealColor];
    self.avatarView.layer.cornerRadius = 48;             // 96/2
    self.avatarView.layer.masksToBounds = YES;
    self.avatarView.image = [UIImage systemImageNamed:@"person.crop.circle.fill"];
    self.avatarView.tintColor = [UIColor whiteColor];
    self.avatarView.contentMode = UIViewContentModeScaleAspectFit;
    [self.contentView addSubview:self.avatarView];
}

- (void)setupName {
    self.nameLabel = [[UILabel alloc] init];
    self.nameLabel.text = @"dqsjqian";
    self.nameLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
    self.nameLabel.textColor = [UIColor labelColor];
    self.nameLabel.textAlignment = NSTextAlignmentCenter;
    [self.contentView addSubview:self.nameLabel];
}

- (void)setupBio {
    self.bioLabel = [[UILabel alloc] init];
    // numberOfLines = 0 lets the label size itself to its intrinsic
    // content height. That's the key to a self-sizing subview inside
    // a UIScrollView.
    self.bioLabel.numberOfLines = 0;
    self.bioLabel.font = [UIFont systemFontOfSize:14];
    self.bioLabel.textColor = [UIColor secondaryLabelColor];
    self.bioLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.contentView addSubview:self.bioLabel];
}

- (void)setupToggleButton {
    self.toggleBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.toggleBtn setTitle:@"展开" forState:UIControlStateNormal];
    self.toggleBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
    self.toggleBtn.backgroundColor = [UIColor secondarySystemBackgroundColor];
    self.toggleBtn.layer.cornerRadius = 8;
    // contentEdgeInsets is deprecated since iOS 15 in favor of UIButtonConfiguration.
    // Keep the legacy call here for visual parity and silence the deprecation locally;
    // remove this pragma if this view is ever migrated to UIButtonConfiguration.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    self.toggleBtn.contentEdgeInsets = UIEdgeInsetsMake(6, 16, 6, 16);
#pragma clang diagnostic pop
    [self.contentView addSubview:self.toggleBtn];
}

- (void)setupTagRow {
    NSArray<NSString *> *titles = @[@"#C++", @"#OC", @"#MVVM", @"#Aria", @"#iOS"];
    NSMutableArray<UIButton *> *btns = [NSMutableArray arrayWithCapacity:titles.count];
    for (NSString *t in titles) {
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        [b setTitle:t forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
        b.backgroundColor = [UIColor systemGray6Color];
        [b setTitleColor:[UIColor systemBlueColor] forState:UIControlStateNormal];
        b.layer.cornerRadius = 6;
        [btns addObject:b];
    }
    self.tagButtons = [btns copy];

    self.tagRow = [[UIStackView alloc] initWithArrangedSubviews:self.tagButtons];
    self.tagRow.axis = UILayoutConstraintAxisHorizontal;
    self.tagRow.distribution = UIStackViewDistributionFillEqually;
    self.tagRow.spacing = 6;
    [self.contentView addSubview:self.tagRow];
}

#pragma mark - (3) Constraints (Masonry idioms in practice)

- (void)setupConstraints {
    // ─────────────────────────────────────────────────────────────────────────
    // Hard rule 1: scrollView pins to self (here against the safe
    //              area, so it stays clear of the notch / home
    //              indicator).
    // ─────────────────────────────────────────────────────────────────────────
    [self.scrollView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.mas_safeAreaLayoutGuideTop);
        make.left.equalTo(self.mas_safeAreaLayoutGuideLeft);
        make.right.equalTo(self.mas_safeAreaLayoutGuideRight);
        make.bottom.equalTo(self.mas_safeAreaLayoutGuideBottom);
    }];

    // ─────────────────────────────────────────────────────────────────────────
    // Hard rule 2: contentView pins all four edges to scrollView and
    //              locks its width = scrollView (we scroll vertically).
    //              Without an explicit width, AutoLayout cannot solve
    //              the contentView width and the layout is ambiguous.
    // ─────────────────────────────────────────────────────────────────────────
    [self.contentView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.edges.equalTo(self.scrollView);              // edges = top+left+right+bottom
        make.width.equalTo(self.scrollView);              // <- crucial
        // Deliberately no height — contentView's height is derived
        // from the chain of subviews below.
    }];

    // ─────────────────────────────────────────────────────────────────────────
    // Hard rule 3: the chain of subviews must run from contentView.top
    //              all the way down to contentView.bottom; that is
    //              what gives contentView a derivable height.
    // ─────────────────────────────────────────────────────────────────────────

    // Avatar: pinned to top, centered, fixed 96x96.
    [self.avatarView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.contentView).offset(20);    // start of the chain
        make.centerX.equalTo(self.contentView);
        make.size.mas_equalTo(CGSizeMake(96, 96));        // mas_equalTo for constants, not equalTo
    }];

    // Name: directly below the avatar.
    [self.nameLabel mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.avatarView.mas_bottom).offset(12);
        make.left.right.equalTo(self.contentView).inset(20);  // lock both sides with a 20pt inset
    }];

    // Bio: below the name; its height ceiling is updateable so we can
    // animate the expand/collapse transition.
    [self.bioLabel mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.nameLabel.mas_bottom).offset(16);
        make.left.right.equalTo(self.contentView).inset(20);
        // Cap height with `<= X` so updateBioMaxHeight: can change it
        // later. Initial cap = 64pt (~3-4 lines).
        // Use .priority(999) instead of required (1000) so intrinsic
        // content size can win in pathological cases without producing
        // a hard conflict.
        self.bioHeightConstraint = make.height.lessThanOrEqualTo(@(64)).priority(999);
    }];

    // Toggle button: below the bio, centered.
    [self.toggleBtn mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.bioLabel.mas_bottom).offset(12);
        make.centerX.equalTo(self.contentView);
        make.height.mas_equalTo(36);
    }];

    // Tag row: below the toggle button. Pinning bottom to contentView
    // is the critical step in hard rule 3 — without it,
    // contentSize.height cannot be solved.
    [self.tagRow mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.toggleBtn.mas_bottom).offset(28);
        make.left.right.equalTo(self.contentView).inset(20);
        make.height.mas_equalTo(36);
        make.bottom.equalTo(self.contentView).offset(-40); // required to close the chain
    }];
}

#pragma mark - (4) Dynamic constraints: expand / collapse

/// Called by the controller. Pass CGFLOAT_MAX for "fully expanded";
/// pass any concrete value to clamp the bio inside that height.
- (void)updateBioMaxHeight:(CGFloat)maxHeight {
    [self.bioLabel mas_updateConstraints:^(MASConstraintMaker *make) {
        make.height.lessThanOrEqualTo(@(maxHeight)).priority(999);
    }];

    // Animate the layout pass driven by the constraint change.
    [UIView animateWithDuration:0.25 animations:^{
        [self layoutIfNeeded];
    }];
}

#pragma mark - Tap-to-dismiss-keyboard gesture
- (void)setupGesture {
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc]
                                   initWithTarget:self action:@selector(onTap)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
}

- (void)onTap {
    [self endEditing:YES];
}

@end
