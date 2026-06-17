#import "LayoutPlaygroundView.h"
#import "Masonry.h"

// ═════════════════════════════════════════════════════════════════════════════
// LayoutPlaygroundView.mm
// Pure UI: control creation + Masonry layout.
// Every business action is wired up by the controller.
// ═════════════════════════════════════════════════════════════════════════════

@interface LayoutPlaygroundView ()

// readwrite redeclarations of the public read-only properties
@property (nonatomic, strong, readwrite) UIImageView *avatarView;
@property (nonatomic, strong, readwrite) UILabel     *nameLabel;
@property (nonatomic, strong, readwrite) UIButton    *primaryBtn;
@property (nonatomic, strong, readwrite) UIButton    *secondaryBtn;
@property (nonatomic, strong, readwrite) UIButton    *dangerBtn;
@property (nonatomic, strong, readwrite) UITextField *emailField;
@property (nonatomic, strong, readwrite) UITextField *passwordField;
@property (nonatomic, strong, readwrite) UIButton    *signInBtn;

// Internal containers / helpers
@property (nonatomic, strong) UIView        *contentView;
@property (nonatomic, strong) UIStackView   *buttonRow;
@property (nonatomic, strong) UIView        *formContainer;
@property (nonatomic, strong) CAGradientLayer *signInGradient;

@end

@implementation LayoutPlaygroundView

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

#pragma mark - Layout (keep the gradient layer in sync with the button bounds)

- (void)layoutSubviews {
    [super layoutSubviews];
    self.signInGradient.frame = self.signInBtn.bounds;
}

#pragma mark - Setup

- (void)setup {
    self.backgroundColor = [UIColor systemBackgroundColor];
    [self setupContentView];
    [self setupHeader];
    [self setupButtonRow];
    [self setupForm];
    [self setupActionButton];
    [self setupConstraints];
    [self setupGesture];
}

- (void)setupContentView {
    self.contentView = [[UIView alloc] init];
    [self addSubview:self.contentView];
    // Masonry does not accept a UILayoutGuide directly in .equalTo(...);
    // we have to go through the mas_safeAreaLayoutGuideXxx properties.
    [self.contentView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.mas_safeAreaLayoutGuideTop).offset(16);
        make.left.equalTo(self.mas_safeAreaLayoutGuideLeft).offset(16);
        make.right.equalTo(self.mas_safeAreaLayoutGuideRight).offset(-16);
        make.bottom.equalTo(self.mas_safeAreaLayoutGuideBottom).offset(-16);
    }];
}

#pragma mark - Header (avatar + name)

- (void)setupHeader {
    self.avatarView = [[UIImageView alloc] init];
    self.avatarView.backgroundColor = [UIColor systemBlueColor];
    self.avatarView.layer.cornerRadius = 48;  // 96/2
    self.avatarView.layer.masksToBounds = YES;
    self.avatarView.image = [UIImage systemImageNamed:@"person.crop.circle.fill"];
    self.avatarView.tintColor = [UIColor whiteColor];
    self.avatarView.contentMode = UIViewContentModeScaleAspectFit;

    self.nameLabel = [[UILabel alloc] init];
    self.nameLabel.text = @"dqsjqian";
    self.nameLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
    self.nameLabel.textColor = [UIColor labelColor];
    self.nameLabel.textAlignment = NSTextAlignmentCenter;

    [self.contentView addSubview:self.avatarView];
    [self.contentView addSubview:self.nameLabel];
}

#pragma mark - Button row

- (void)setupButtonRow {
    self.primaryBtn   = [self makeStyledButtonTitle:@"Primary"
                                                 fg:[UIColor whiteColor]
                                                 bg:[UIColor systemBlueColor]];
    self.secondaryBtn = [self makeStyledButtonTitle:@"Secondary"
                                                 fg:[UIColor systemBlueColor]
                                                 bg:[UIColor systemGray6Color]];
    self.dangerBtn    = [self makeStyledButtonTitle:@"Danger"
                                                 fg:[UIColor whiteColor]
                                                 bg:[UIColor systemRedColor]];

    self.buttonRow = [[UIStackView alloc] initWithArrangedSubviews:@[
        self.primaryBtn, self.secondaryBtn, self.dangerBtn
    ]];
    self.buttonRow.axis = UILayoutConstraintAxisHorizontal;
    self.buttonRow.distribution = UIStackViewDistributionFillEqually;
    self.buttonRow.spacing = 8;

    [self.contentView addSubview:self.buttonRow];
}

- (UIButton *)makeStyledButtonTitle:(NSString *)title
                                 fg:(UIColor *)fg
                                 bg:(UIColor *)bg {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    [b setTitleColor:fg forState:UIControlStateNormal];
    b.backgroundColor = bg;
    b.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    b.layer.cornerRadius = 10;
    // Note: NO addTarget here — actions are wired by the controller.
    return b;
}

#pragma mark - Form

- (void)setupForm {
    self.formContainer = [[UIView alloc] init];
    self.emailField    = [self makeLabeledFieldPlaceholder:@"Email"
                                                  keyboard:UIKeyboardTypeEmailAddress
                                                    secure:NO];
    self.passwordField = [self makeLabeledFieldPlaceholder:@"Password"
                                                  keyboard:UIKeyboardTypeDefault
                                                    secure:YES];
    [self.formContainer addSubview:self.emailField];
    [self.formContainer addSubview:self.passwordField];
    [self.contentView addSubview:self.formContainer];

    [self.emailField mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.left.right.equalTo(self.formContainer);
        make.height.mas_equalTo(44);
    }];
    [self.passwordField mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.emailField.mas_bottom).offset(12);
        make.left.right.bottom.equalTo(self.formContainer);
        make.height.mas_equalTo(44);
    }];
}

- (UITextField *)makeLabeledFieldPlaceholder:(NSString *)placeholder
                                    keyboard:(UIKeyboardType)kbType
                                      secure:(BOOL)secure {
    UITextField *tf = [[UITextField alloc] init];
    tf.placeholder = placeholder;
    tf.keyboardType = kbType;
    tf.secureTextEntry = secure;
    tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
    tf.autocorrectionType = UITextAutocorrectionTypeNo;
    tf.borderStyle = UITextBorderStyleRoundedRect;
    tf.backgroundColor = [UIColor secondarySystemBackgroundColor];
    tf.font = [UIFont systemFontOfSize:15];
    return tf;
}

#pragma mark - Action button (gradient "Sign In")

- (void)setupActionButton {
    self.signInBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.signInBtn setTitle:@"Sign In" forState:UIControlStateNormal];
    [self.signInBtn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    self.signInBtn.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    self.signInBtn.layer.cornerRadius = 12;
    self.signInBtn.layer.masksToBounds = YES;
    // Note: NO addTarget here — actions are wired by the controller.

    // Gradient backdrop (managed by the view itself — purely visual).
    self.signInGradient = [CAGradientLayer layer];
    self.signInGradient.colors = @[
        (id)[UIColor systemBlueColor].CGColor,
        (id)[UIColor systemPurpleColor].CGColor,
    ];
    self.signInGradient.startPoint = CGPointMake(0, 0.5);
    self.signInGradient.endPoint   = CGPointMake(1, 0.5);
    [self.signInBtn.layer insertSublayer:self.signInGradient atIndex:0];

    [self.contentView addSubview:self.signInBtn];
}

#pragma mark - Constraints (top-level layout)

- (void)setupConstraints {
    [self.avatarView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.contentView).offset(16);
        make.centerX.equalTo(self.contentView);
        make.width.height.mas_equalTo(96);
    }];

    [self.nameLabel mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.avatarView.mas_bottom).offset(12);
        make.left.right.equalTo(self.contentView);
    }];

    [self.buttonRow mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.nameLabel.mas_bottom).offset(24);
        make.left.right.equalTo(self.contentView);
        make.height.mas_equalTo(44);
    }];

    [self.formContainer mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.buttonRow.mas_bottom).offset(28);
        make.left.right.equalTo(self.contentView);
    }];

    [self.signInBtn mas_makeConstraints:^(MASConstraintMaker *make) {
        make.top.equalTo(self.formContainer.mas_bottom).offset(24);
        make.left.right.equalTo(self.contentView);
        make.height.mas_equalTo(52);
    }];
}

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
