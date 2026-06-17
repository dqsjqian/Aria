#import <UIKit/UIKit.h>

// ═════════════════════════════════════════════════════════════════════════════
// RootView — pure UIKit view (no ViewModel, no binding, no business logic)
//
// Responsibilities (V in MVC):
//   - Lay out all widgets with Auto Layout
//   - Expose outlets the Controller binds to
//   - Own UI-only behavior (toast show/hide animation)
//
// What it does NOT do:
//   - Know about MainViewModel / aria::Property / DataModel
//   - Decide when to push/present a VC or fire a command
//   - Talk to any C++ code
//
// The Controller reads these outlets after the view is loaded and wires
// them to the ViewModel.
//
// NOTE (vs AppKit demo2):
//   - UIKit's coordinate system has Y pointing downwards (opposite of
//     AppKit). We rely entirely on Auto Layout here, with a "stack from
//     the top" mental model that matches what a phone screen looks like.
//   - No NSBox needed; the separator is just a 1pt-tall UIView.
//   - On iOS 15+ Apple recommends UIButtonConfiguration; we keep the
//     legacy API here for backwards compatibility.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootView : UIView

/// Label showing status text (bound to VM's status_text).
@property (nonatomic, readonly) UILabel *statusLabel;

/// Label showing fetch result (bound to VM's result_text).
@property (nonatomic, readonly) UILabel *resultLabel;

/// Floating toast near the bottom (UI-only; Controller calls -showToast:).
@property (nonatomic, readonly) UILabel *toastLabel;

/// The 5 pattern buttons (Block / Notification / KVO / Delegate / TargetAction).
/// Controller wires each to a Command.
@property (nonatomic, readonly) NSArray<UIButton *> *patternButtons;

/// The 3 playground buttons (Panel A / B / C).
/// Controller wires each to its present-modal handler.
@property (nonatomic, readonly) NSArray<UIButton *> *playgroundButtons;

/// Show a floating toast for `message`. Auto-hides after 4s (fades in/out).
- (void)showToast:(NSString *)message;

/// Hide the toast immediately (e.g. called from dealloc).
- (void)hideToastNow;

@end
