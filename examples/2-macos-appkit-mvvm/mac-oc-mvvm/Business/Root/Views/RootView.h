#import <Cocoa/Cocoa.h>

// ═════════════════════════════════════════════════════════════════════════════
// RootView — pure AppKit view (no ViewModel, no binding, no business logic)
//
// Responsibilities (V in MVC):
//   - Lay out all widgets with hard-coded frames
//   - Expose outlets the Controller binds to
//   - Own UI-only behavior (toast show/hide animation)
//
// What it does NOT do:
//   - Know about MainViewModel / aria::Property / DataModel
//   - Decide when to open a window or fire a command
//   - Talk to any C++ code
//
// The Controller reads these outlets after the view is loaded and wires
// them to the ViewModel.
// ═════════════════════════════════════════════════════════════════════════════

@interface RootView : NSView

/// Label showing status text (bound to VM's status_text).
@property (nonatomic, readonly) NSTextField *statusLabel;

/// Label showing fetch result (bound to VM's result_text).
@property (nonatomic, readonly) NSTextField *resultLabel;

/// Floating toast below the legend (UI-only; Controller calls -showToast:).
@property (nonatomic, readonly) NSTextField *toastLabel;

/// The 5 pattern buttons (Block / Notification / KVO / Delegate / TargetAction).
/// Controller wires each to a Command.
@property (nonatomic, readonly) NSArray<NSButton *> *patternButtons;

/// The 3 playground buttons (Panel A / B / C).
/// Controller wires each to its open-panel handler.
@property (nonatomic, readonly) NSArray<NSButton *> *playgroundButtons;

/// Show a floating toast for `message`. Auto-hides after 4s.
- (void)showToast:(NSString *)message;

/// Hide the toast immediately (e.g. called from dealloc).
- (void)hideToastNow;

@end
