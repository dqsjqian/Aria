#import "LayoutPlaygroundController.h"
#import "LayoutPlaygroundView.h"

// ═════════════════════════════════════════════════════════════════════════════════════════
// LayoutPlaygroundController — Panel B (full-screen push).
//
// Controller + View split:
//   * All control creation, Masonry layout and gradient tracking
//     live in LayoutPlaygroundView.
//   * The controller only:
//       - replaces the root view inside -loadView
//       - sets the navigation-bar title
//       - attaches target/action to controls exposed by the view
//       - reads textField values and presents alerts (business logic)
//
// This trims the controller from ~240 lines down to ~80.
// ═════════════════════════════════════════════════════════════════════════════

@implementation LayoutPlaygroundController

#pragma mark - View lifecycle

- (void)loadView {
    self.view = [[LayoutPlaygroundView alloc] init];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Panel B";

    LayoutPlaygroundView *v = self.rootView;
    // Wire the same handler to all three inline buttons;
    // sender.currentTitle distinguishes which one was tapped.
    [v.primaryBtn   addTarget:self action:@selector(onRowButtonTapped:)
             forControlEvents:UIControlEventTouchUpInside];
    [v.secondaryBtn addTarget:self action:@selector(onRowButtonTapped:)
             forControlEvents:UIControlEventTouchUpInside];
    [v.dangerBtn    addTarget:self action:@selector(onRowButtonTapped:)
             forControlEvents:UIControlEventTouchUpInside];

    [v.signInBtn addTarget:self action:@selector(onSignInTapped:)
          forControlEvents:UIControlEventTouchUpInside];
    [v.emailField addTarget:self action:@selector(onEmailChanged:)
           forControlEvents:UIControlEventEditingChanged];
}

#pragma mark - Typed accessor

- (LayoutPlaygroundView *)rootView {
    return (LayoutPlaygroundView *)self.view;
}

#pragma mark - Actions

- (void)onRowButtonTapped:(UIButton *)sender {
    NSString *title = sender.currentTitle ?: @"";
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:@"Tapped"
                         message:[NSString stringWithFormat:@"You tapped \"%@\".", title]
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                              style:UIAlertActionStyleDefault
                                            handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)onSignInTapped:(UIButton *)sender {
    NSString *email = self.rootView.emailField.text ?: @"";
    NSString *pwd   = self.rootView.passwordField.text ?: @"";
    NSString *msg = [NSString stringWithFormat:@"email: %@\npwd length: %lu",
                     email.length > 0 ? email : @"(empty)",
                     (unsigned long)pwd.length];
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:@"Sign In (demo)"
                         message:msg
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                              style:UIAlertActionStyleDefault
                                            handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)onEmailChanged:(UITextField *)tf {
    NSLog(@"email len = %lu", (unsigned long)tf.text.length);
}
@end
