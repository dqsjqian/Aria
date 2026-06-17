#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// ═════════════════════════════════════════════════════════════════════════════
// ScrollPlaygroundView — Panel D's pure UI layer.
//
// Topic: UIScrollView nesting + self-sizing height + dynamic
//        constraint updates (mas_updateConstraints).
//
// Three hard rules of UIScrollView (must follow):
//   1. scrollView must contain a single contentView wrapper; every
//      subview is added onto that contentView.
//   2. contentView pins its four edges to scrollView AND explicitly
//      locks its width (for vertical scrolling) or height (for
//      horizontal scrolling).
//   3. The chain of subviews must run from contentView.top all the way
//      down to contentView.bottom so the contentView's height is
//      derived — that's how AutoLayout knows the contentSize.height.
//
// Public API the Controller relies on:
//   - bioLabel  : the controller writes its text and calls
//                 updateBioMaxHeight: to toggle expansion.
//   - toggleBtn : the controller attaches target/action (expand/collapse).
//   - tagButtons: 5 inline tag buttons (purely decorative, no action).
//
// Deliberately NOT exposed: scrollView / contentView / avatar /
// nameLabel — those are internal layout machinery and the controller
// should never touch them directly.
// ═════════════════════════════════════════════════════════════════════════════
@interface ScrollPlaygroundView : UIView

// ── Text / actions (controller reads & writes) ───────────────────────
@property (nonatomic, strong, readonly) UILabel  *bioLabel;
@property (nonatomic, strong, readonly) UIButton *toggleBtn;

// ── Tag row (UIStackView demo) ───────────────────────────────────────
@property (nonatomic, strong, readonly) NSArray<UIButton *> *tagButtons;

// ── Controller-callable: change the bio's height ceiling ─────────────
//    Pass CGFLOAT_MAX for "fully expanded", or a concrete value to
//    collapse within that bound. Internally implemented with
//    mas_updateConstraints + UIView animation for smooth transitions.
- (void)updateBioMaxHeight:(CGFloat)maxHeight;

@end

NS_ASSUME_NONNULL_END
