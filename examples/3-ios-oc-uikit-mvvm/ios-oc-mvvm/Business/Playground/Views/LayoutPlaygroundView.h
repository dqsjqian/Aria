#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// ═════════════════════════════════════════════════════════════════════════════
// LayoutPlaygroundView — Panel B's pure UI layer.
//
// Responsibilities:
//   * Build and lay out the whole content tree with Masonry
//     (avatar / buttonRow / form / signInBtn).
//   * Expose every control the Controller needs to observe or read.
//   * Handle layout-local concerns (e.g. keeping signInGradient's
//     frame in sync with layout passes).
//   * NOT to hold business logic: no alerts, no navigationController access.
//
// Public API the Controller relies on:
//   - primaryBtn / secondaryBtn / dangerBtn — three inline buttons
//     (the controller attaches actions).
//   - signInBtn — bottom-anchored primary button (action attached by
//     the controller).
//   - emailField / passwordField — the controller reads their text.
//
// Companion controller: LayoutPlaygroundController.
// ═════════════════════════════════════════════════════════════════════════════
@interface LayoutPlaygroundView : UIView

// ── Header ───────────────────────────────────────────────────────────
@property (nonatomic, strong, readonly) UIImageView *avatarView;
@property (nonatomic, strong, readonly) UILabel     *nameLabel;

// ── Button row (controller attaches target/action) ───────────────────
@property (nonatomic, strong, readonly) UIButton *primaryBtn;
@property (nonatomic, strong, readonly) UIButton *secondaryBtn;
@property (nonatomic, strong, readonly) UIButton *dangerBtn;

// ── Form (controller reads the text) ─────────────────────────────────
@property (nonatomic, strong, readonly) UITextField *emailField;
@property (nonatomic, strong, readonly) UITextField *passwordField;

// ── Action (controller attaches target/action) ───────────────────────
@property (nonatomic, strong, readonly) UIButton *signInBtn;

@end

NS_ASSUME_NONNULL_END
