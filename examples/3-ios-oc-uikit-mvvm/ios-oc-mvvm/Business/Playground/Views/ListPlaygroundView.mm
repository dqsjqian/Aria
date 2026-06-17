#import "ListPlaygroundView.h"
#import "Masonry.h"

// ═════════════════════════════════════════════════════════════════════════════
// ListPlaygroundView.mm
// Owns: tableView creation + configuration + layout + cell registration
//       + toast helper.
// Does NOT touch business data, dataSource or delegate.
// ═════════════════════════════════════════════════════════════════════════════

#pragma mark - Cell

@implementation LPGItemCell

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString *)reuseIdentifier {
    if (self = [super initWithStyle:style reuseIdentifier:reuseIdentifier]) {
        self.accessoryType = UITableViewCellAccessoryDisclosureIndicator;

        self.iconView = [[UIImageView alloc] init];
        self.iconView.contentMode = UIViewContentModeScaleAspectFit;
        self.iconView.tintColor = [UIColor systemBlueColor];
        [self.contentView addSubview:self.iconView];

        self.titleLabel = [[UILabel alloc] init];
        self.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
        self.titleLabel.textColor = [UIColor labelColor];
        [self.contentView addSubview:self.titleLabel];

        self.subtitleLabel = [[UILabel alloc] init];
        self.subtitleLabel.font = [UIFont systemFontOfSize:13];
        self.subtitleLabel.textColor = [UIColor secondaryLabelColor];
        [self.contentView addSubview:self.subtitleLabel];

        [self.iconView mas_makeConstraints:^(MASConstraintMaker *make) {
            make.left.equalTo(self.contentView).offset(16);
            make.centerY.equalTo(self.contentView);
            make.width.height.mas_equalTo(28);
        }];
        [self.titleLabel mas_makeConstraints:^(MASConstraintMaker *make) {
            make.left.equalTo(self.iconView.mas_right).offset(12);
            make.right.lessThanOrEqualTo(self.contentView).offset(-8);
            make.top.equalTo(self.contentView).offset(10);
        }];
        [self.subtitleLabel mas_makeConstraints:^(MASConstraintMaker *make) {
            make.left.equalTo(self.titleLabel);
            make.right.equalTo(self.titleLabel);
            make.top.equalTo(self.titleLabel.mas_bottom).offset(2);
            make.bottom.lessThanOrEqualTo(self.contentView).offset(-10);
        }];
    }
    return self;
}

@end

#pragma mark - View

@interface ListPlaygroundView ()
@property (nonatomic, strong, readwrite) UITableView *tableView;
@property (nonatomic, copy,   readwrite) NSString    *cellReuseIdentifier;
@end

@implementation ListPlaygroundView

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
    self.cellReuseIdentifier = @"LPGItemCell";

    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero
                                                  style:UITableViewStylePlain];
    self.tableView.rowHeight = 60;
    // Explicitly enable scrolling (defaults to YES, but stating it here
    // makes regressions easier to spot).
    self.tableView.scrollEnabled = YES;
    self.tableView.alwaysBounceVertical = YES;
    self.tableView.showsVerticalScrollIndicator = YES;
    // Let UIKit adjust contentInset for the navigation bar / safe area.
    self.tableView.contentInsetAdjustmentBehavior =
        UIScrollViewContentInsetAdjustmentAutomatic;
    [self.tableView registerClass:[LPGItemCell class]
           forCellReuseIdentifier:self.cellReuseIdentifier];
    [self addSubview:self.tableView];

    [self.tableView mas_makeConstraints:^(MASConstraintMaker *make) {
        make.edges.equalTo(self);
    }];
}

#pragma mark - Toast

- (void)showToast:(NSString *)message {
    UILabel *toast = [[UILabel alloc] init];
    toast.text = message;
    toast.textColor = [UIColor whiteColor];
    toast.font = [UIFont systemFontOfSize:13];
    toast.textAlignment = NSTextAlignmentCenter;
    toast.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.75];
    toast.layer.cornerRadius = 8;
    toast.layer.masksToBounds = YES;
    toast.alpha = 0;
    [self addSubview:toast];
    [toast mas_makeConstraints:^(MASConstraintMaker *make) {
        make.centerX.equalTo(self);
        make.bottom.equalTo(self.mas_safeAreaLayoutGuideBottom).offset(-24);
        make.height.mas_equalTo(36);
        make.width.mas_greaterThanOrEqualTo(120);
        make.left.greaterThanOrEqualTo(self).offset(40);
        make.right.lessThanOrEqualTo(self).offset(-40);
    }];

    toast.layer.shadowColor = [UIColor blackColor].CGColor;
    toast.layer.shadowRadius = 6;
    toast.layer.shadowOpacity = 0.2;
    toast.layer.shadowOffset = CGSizeMake(0, 2);

    [UIView animateWithDuration:0.2 animations:^{ toast.alpha = 1; }
                     completion:^(BOOL _) {
        [UIView animateWithDuration:0.3 delay:1.2 options:0
                         animations:^{ toast.alpha = 0; }
                         completion:^(BOOL __) { [toast removeFromSuperview]; }];
    }];
}

@end
