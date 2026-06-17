//
// UIKitConformanceRunner.h
// ios-oc-mvvm
//
// In-app conformance runner for aria::adapters::uikit::UIKitAdapter.
// Replaces a dedicated XCUITest target — results show up in NSLog
// and (optionally) a small banner view attached to any UI surface.

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface UIKitConformanceRunner : NSObject

/// Run the 9-scenario conformance battery synchronously on the main
/// thread. Results are logged to NSLog; per-scenario pass/fail and
/// total counts are both reported.
+ (void)runAndLog;

/// Convenience: returns a UILabel summarising the most recent run
/// (must be called after `runAndLog`). Green for all-pass, red
/// otherwise. Callers are responsible for layout.
+ (UILabel*)makeResultBanner;

+ (BOOL)allPassed;
+ (NSInteger)passCount;
+ (NSInteger)failCount;

@end

NS_ASSUME_NONNULL_END
