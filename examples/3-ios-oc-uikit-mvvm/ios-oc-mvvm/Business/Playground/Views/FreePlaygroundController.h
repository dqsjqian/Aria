#import <UIKit/UIKit.h>

@interface FreePlaygroundController : UIViewController

// Exposed so callers (e.g. RootViewController) can wire this as the
// action of a UIBarButtonItem without tripping -Wundeclared-selector.
- (void)onClearTapped:(UIBarButtonItem *)sender;

@end
