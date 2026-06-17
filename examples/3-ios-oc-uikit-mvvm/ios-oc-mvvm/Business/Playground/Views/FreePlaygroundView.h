#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// ═════════════════════════════════════════════════════════════════════════════
// FreePlaygroundView — Panel A's pure UI layer.
//
// Responsibilities:
//   * Create and lay out every subview (here we just have one
//     UITextView pinned to the safe area).
//   * Expose the controls the Controller needs to read/write (textView).
//   * NOT to hold business logic, listen to events, or touch the
//     navigationController.
//
// Companion controller: FreePlaygroundController.
//   The controller swaps in this custom view inside -loadView via
//   `self.view = [FreePlaygroundView new]`, replacing the default
//   UIView.
//
// This is the canonical "Controller + View split" pattern in mature
// iOS codebases.
// ═════════════════════════════════════════════════════════════════════════════
@interface FreePlaygroundView : UIView

/// Code-edit text area pinned to the safe area; the controller may
/// read and write its `text` property.
@property (nonatomic, strong, readonly) UITextView *textView;

@end

NS_ASSUME_NONNULL_END
