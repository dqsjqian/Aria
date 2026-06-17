#import "FreePlaygroundController.h"

// ═════════════════════════════════════════════════════════════════════════════
// FreePlaygroundController — child view controller with a blank text editor
//
// Changed to .mm (ObjC++) so it can include C++ headers if needed in future.
// Current implementation is pure UI, no C++ logic needed yet.
// ═════════════════════════════════════════════════════════════════════════════

@implementation FreePlaygroundController

- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.wantsLayer = YES;
    self.view.layer.backgroundColor = [[NSColor windowBackgroundColor] CGColor];

    NSScrollView *scrollView = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scrollView.hasVerticalScroller = YES;
    scrollView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    NSTextView *textView = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0, 400, 300)];
    textView.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
    textView.string =
        @"// Free Playground (ObjC++)\n"
         "//\n"
         "// This child view controller is now compiled as Objective-C++.\n"
         "// It can include C++ headers directly.\n"
         "//\n"
         "// Try adding:\n"
         "//   - A C++ ViewModel from aria\n"
         "//   - Property bindings via AppKitAdapter\n"
         "//   - Async commands\n";
    textView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    scrollView.documentView = textView;
    [self.view addSubview:scrollView];
}

@end
