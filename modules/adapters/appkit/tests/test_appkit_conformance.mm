// test_appkit_conformance.mm — Run the shared `adapter_conformance`
// battery against AppKitAdapter.
//
// This binary spins up a minimal NSApplication so AppKit widgets can
// be created off-screen. We never enter the run loop — controls are
// driven programmatically through their target/action pipeline (which
// fires synchronously in our test harness).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#import "aria/adapters/appkit/AppKitAdapter.hpp"
#import "aria/binding/testing/adapter_conformance.hpp"
#import "aria/reactive/reactive.hpp"   // pulls graph.inl Node defs

#import <Cocoa/Cocoa.h>

#include <memory>

namespace conformance = ::aria::binding::testing::conformance;

namespace {

// RAII wrapper around a (NSView, AppKitView) pair.
struct ViewHandle {
    std::unique_ptr<::aria::adapters::appkit::AppKitView> wrapper;
    NSView* __strong native = nil;

    ::aria::binding::IView& view() { return *wrapper; }
};

// Make sure NSApplication is initialised exactly once for the whole
// test process. Off-screen widgets still need [NSApplication
// sharedApplication] for things like target/action plumbing to work.
void ensure_nsapp() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    });
}

class AppKitHarness {
public:
    AppKitHarness() { ensure_nsapp(); }

    [[nodiscard]] std::shared_ptr<::aria::binding::IViewAdapter> make_adapter() {
        return std::make_shared<::aria::adapters::appkit::AppKitAdapter>();
    }

    // ── Widget factories ────────────────────────────────────────────────
    ViewHandle make_text_view() {
        NSTextField* tf = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, 22)];
        tf.editable = YES;
        tf.bezeled  = YES;
        tf.stringValue = @"";
        ViewHandle h;
        h.native  = tf;
        h.wrapper = std::make_unique<::aria::adapters::appkit::AppKitView>(tf);
        return h;
    }

    ViewHandle make_bool_view() {
        NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 100, 22)];
        b.buttonType = NSButtonTypeSwitch;       // checkbox
        b.title      = @"";
        b.state      = NSControlStateValueOff;
        ViewHandle h;
        h.native  = b;
        h.wrapper = std::make_unique<::aria::adapters::appkit::AppKitView>(b);
        return h;
    }

    ViewHandle make_int_view() {
        NSStepper* s = [[NSStepper alloc] initWithFrame:NSMakeRect(0, 0, 30, 22)];
        s.minValue = -1'000'000'000;
        s.maxValue =  1'000'000'000;
        s.intValue = 0;
        ViewHandle h;
        h.native  = s;
        h.wrapper = std::make_unique<::aria::adapters::appkit::AppKitView>(s);
        return h;
    }

    ViewHandle make_double_view() {
        NSSlider* s = [[NSSlider alloc] initWithFrame:NSMakeRect(0, 0, 100, 22)];
        s.minValue = -1000.0;
        s.maxValue =  1000.0;
        s.doubleValue = 0.0;
        ViewHandle h;
        h.native  = s;
        h.wrapper = std::make_unique<::aria::adapters::appkit::AppKitView>(s);
        return h;
    }

    ViewHandle make_click_view() {
        NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 100, 22)];
        b.buttonType = NSButtonTypeMomentaryPushIn;   // push button
        b.title      = @"Click";
        ViewHandle h;
        h.native  = b;
        h.wrapper = std::make_unique<::aria::adapters::appkit::AppKitView>(b);
        return h;
    }

    // ── User-action simulators ──────────────────────────────────────────
    void user_type(::aria::binding::IView& v, std::string text) {
        auto& av = static_cast<::aria::adapters::appkit::AppKitView&>(v);
        NSTextField* tf = av.as<NSTextField>();
        NSString* ns = [NSString stringWithUTF8String:text.c_str()];
        tf.stringValue = ns;
        // Programmatically fire textDidChange so any wired delegate
        // observes the same path a real user would.
        NSNotification* n = [NSNotification
            notificationWithName:NSControlTextDidChangeNotification
                          object:tf];
        if (id<NSTextFieldDelegate> d = tf.delegate) {
            if ([d respondsToSelector:@selector(controlTextDidChange:)]) {
                [d controlTextDidChange:n];
            }
        }
    }

    void user_toggle(::aria::binding::IView& v, bool b) {
        auto& av = static_cast<::aria::adapters::appkit::AppKitView&>(v);
        NSButton* btn = av.as<NSButton>();
        btn.state = b ? NSControlStateValueOn : NSControlStateValueOff;
        // Fire the target/action pipeline. The selector is a runtime
        // value (it was wired by the framework via -setAction:), so
        // ARC's "may leak" check trips here. The IBAction selectors we
        // test are all `void`-returning; suppressing the warning around
        // these synthetic test-only invocations is intentional.
        if (btn.target && btn.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [btn.target performSelector:btn.action withObject:btn];
#pragma clang diagnostic pop
        }
    }

    void user_set_int(::aria::binding::IView& v, int n) {
        auto& av = static_cast<::aria::adapters::appkit::AppKitView&>(v);
        NSControl* c = av.as<NSControl>();
        c.intValue = n;
        if (c.target && c.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [c.target performSelector:c.action withObject:c];
#pragma clang diagnostic pop
        }
    }

    void user_set_double(::aria::binding::IView& v, double d) {
        auto& av = static_cast<::aria::adapters::appkit::AppKitView&>(v);
        NSControl* c = av.as<NSControl>();
        c.doubleValue = d;
        if (c.target && c.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [c.target performSelector:c.action withObject:c];
#pragma clang diagnostic pop
        }
    }

    void user_click(::aria::binding::IView& v) {
        auto& av = static_cast<::aria::adapters::appkit::AppKitView&>(v);
        NSButton* btn = av.as<NSButton>();
        if (btn.target && btn.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
            [btn.target performSelector:btn.action withObject:btn];
#pragma clang diagnostic pop
        }
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
//  Conformance battery (one TEST_CASE per public entry point)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("AppKitAdapter conforms: text two-way") {
    AppKitHarness h; conformance::run_text_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: text engine two-way") {
    AppKitHarness h; conformance::run_text_engine_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: bool two-way") {
    AppKitHarness h; conformance::run_bool_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: bool engine two-way") {
    AppKitHarness h; conformance::run_bool_engine_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: int two-way") {
    AppKitHarness h; conformance::run_int_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: double two-way") {
    AppKitHarness h; conformance::run_double_two_way(h);
}
TEST_CASE("AppKitAdapter conforms: click") {
    AppKitHarness h; conformance::run_click(h);
}
TEST_CASE("AppKitAdapter conforms: command + enabled") {
    AppKitHarness h; conformance::run_command_enabled(h);
}
TEST_CASE("AppKitAdapter conforms: view destroy safety") {
    AppKitHarness h; conformance::run_view_destroy_safety(h);
}

// Optional numeric extras (forward through int/double).
TEST_CASE("AppKitAdapter conforms: int64 via int view") {
    AppKitHarness h; conformance::run_int64_two_way_via_int_view(h);
}
TEST_CASE("AppKitAdapter conforms: uint64 via int view") {
    AppKitHarness h; conformance::run_uint64_two_way_via_int_view(h);
}
TEST_CASE("AppKitAdapter conforms: float via double view") {
    AppKitHarness h; conformance::run_float_two_way_via_double_view(h);
}
