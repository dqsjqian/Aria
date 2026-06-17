#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// ═════════════════════════════════════════════════════════════════════════════
// ScrollPlaygroundController — Panel D (thin controller).
//
// Responsibilities:
//   * Pair with ScrollPlaygroundView and wire up all actions.
//   * Hold the local "is-expanded" state (a future iteration could
//     replace this with an `aria::Property<bool>` so the reactive
//     pipeline drives the view refresh instead).
//   * Translate state changes into view operations (refresh).
// ═════════════════════════════════════════════════════════════════════════════
@interface ScrollPlaygroundController : UIViewController
@end

NS_ASSUME_NONNULL_END
