#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// ═════════════════════════════════════════════════════════════════════════════
// ListPlaygroundView — Panel C's pure UI layer.
//
// Responsibilities:
//   * Own and configure the UITableView (defensive flags for
//     scrolling / bouncing / inset adjustment).
//   * Register the custom cell class.
//   * Expose a single -showToast: helper for ad-hoc UI feedback
//     (the controller calls into it directly).
//   * NOT to provide dataSource / delegate (that is the controller's job).
//   * NOT to hold the data array (also the controller's job).
//
// Companion controller: ListPlaygroundController.
//   The controller sets itself as the tableView's dataSource/delegate
//   inside viewDidLoad.
//
// About LPGItemCell:
//   As a "Panel C-specific visual component" it is defined inside the
//   view's implementation file (.mm). The class declaration and a
//   minimal set of public properties are mirrored in this header so
//   the controller can configure cells in cellForRow.
// ═════════════════════════════════════════════════════════════════════════════

#pragma mark - Cell (exposed to Controller)

/// Custom cell: icon + title + subtitle + chevron.
@interface LPGItemCell : UITableViewCell
@property (nonatomic, strong) UIImageView *iconView;
@property (nonatomic, strong) UILabel     *titleLabel;
@property (nonatomic, strong) UILabel     *subtitleLabel;
@end

#pragma mark - View

@interface ListPlaygroundView : UIView

/// Panel C's central UITableView; the controller installs itself as
/// tableView.dataSource / tableView.delegate from viewDidLoad.
@property (nonatomic, strong, readonly) UITableView *tableView;

/// Reuse identifier exposed in one place so the controller and the
/// view never disagree on the literal string.
@property (nonatomic, copy, readonly) NSString *cellReuseIdentifier;

/// Float a short message at the bottom of the screen and fade it out.
/// Pure UI feedback fully managed by the view itself.
- (void)showToast:(NSString *)message;

@end

NS_ASSUME_NONNULL_END
