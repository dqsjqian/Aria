#import "FreePlaygroundController.h"
#import "FreePlaygroundView.h"

// ═════════════════════════════════════════════════════════════════════════════
// FreePlaygroundController — Panel A (half-screen formSheet).
//
// Controller + View split:
//   * All subview creation / layout lives in FreePlaygroundView.
//   * The controller only:
//       - replaces self.view with FreePlaygroundView inside -loadView
//       - exposes a typed accessor -rootView for child-control access
//       - is the future home for ViewModel / binding wiring
//
// This is the canonical "thin controller, fat view" iOS engineering
// pattern.
// ═════════════════════════════════════════════════════════════════════════════

@interface FreePlaygroundController ()
@end

@implementation FreePlaygroundController

#pragma mark - View lifecycle

/// -loadView is UIKit's dedicated hook for replacing the default root
/// view. Only assign self.view here; do NOT call [super loadView] and
/// do NOT touch any subview of self.view here — they don't exist yet.
- (void)loadView {
    self.view = [[FreePlaygroundView alloc] init];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // Future hooks live here:
    //   * bind a C++ ViewModel Property to self.rootView.textView
    //   * subscribe to textView edits and forward them to the VM
    //   * wire navigation-bar right/left bar-button actions
}

#pragma mark - Typed accessor

/// Cast self.view to the concrete subclass once so callers can read
/// `self.rootView` instead of repeating
/// `(FreePlaygroundView *)self.view` everywhere.
- (FreePlaygroundView *)rootView {
    return (FreePlaygroundView *)self.view;
}

#pragma mark - Actions

- (void)onClearTapped:(UIBarButtonItem *)sender {
    self.rootView.textView.text = @"";
}

@end
