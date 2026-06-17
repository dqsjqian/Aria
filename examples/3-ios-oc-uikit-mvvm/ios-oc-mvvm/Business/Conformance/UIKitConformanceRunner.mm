//
// UIKitConformanceRunner.mm
// ios-oc-mvvm
//
// Runs the shared `adapter_conformance` test battery against
// `aria::adapters::uikit::UIKitAdapter` on first launch of the
// simulator app. Results are logged via NSLog; an on-screen banner
// (green for pass, red for fail) summarises status so a tester can
// eyeball the run at a glance.
//
// We do NOT pull doctest into the app — instead we hand-roll a
// minimal CHECK macro that increments pass/fail counters and
// accumulates failure locations. This keeps the app binary small
// and avoids dragging a full test runner into a production sample.
//
// Usage (from AppDelegate):
//
//     [UIKitConformanceRunner runAndLog];
//
// Usage (from anywhere wanting a UI summary):
//
//     UILabel* banner = [UIKitConformanceRunner makeResultBanner];
//     [someView addSubview:banner];

#import "UIKitConformanceRunner.h"

#import "UIKitAdapter.hpp"
#import "aria/reactive/reactive.hpp"
#import "aria/binding/binding_engine.hpp"

#import <UIKit/UIKit.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
//  Minimal doctest-compatible CHECK shim
// ═══════════════════════════════════════════════════════════════════════
//
// The conformance header calls `CHECK(expr)` / `CHECK_FALSE(expr)` /
// `REQUIRE(expr)`. We redirect those to our own counters so we do not
// have to link the full doctest runtime into the app.

namespace aria_conf_shim {

struct Stats {
    std::atomic<int>                     pass{0};
    std::atomic<int>                     fail{0};
    std::vector<std::string>             failure_messages;
    std::mutex                           m;
};
inline Stats& stats() { static Stats s; return s; }

inline void record(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
        stats().pass.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats().fail.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream os;
        os << "FAIL: " << expr << " @ " << file << ":" << line;
        std::lock_guard lk(stats().m);
        stats().failure_messages.push_back(os.str());
    }
}

}  // namespace aria_conf_shim

// Redefine CHECK/REQUIRE BEFORE we include adapter_conformance.hpp.
// Since adapter_conformance.hpp does `#include <doctest/doctest.h>`,
// we can't just replace its macros — instead we include a shim that
// provides a minimal `doctest/doctest.h`. Easier: drop `adapter_
// conformance.hpp` and inline a trimmed copy that uses our shim.
//
// To keep maintenance low, we instead hard-code the 9 conformance
// scenarios here (adapter contract is stable; copying ~60 lines of
// test logic is cheaper than building a doctest shim).

#undef CHECK
#undef CHECK_FALSE
#undef REQUIRE

#define AR_CHECK(expr) \
    aria_conf_shim::record(!!(expr), #expr, __FILE__, __LINE__)
#define AR_CHECK_FALSE(expr) \
    aria_conf_shim::record(!(expr), "!(" #expr ")", __FILE__, __LINE__)
#define AR_REQUIRE(expr) AR_CHECK(expr)

// ═══════════════════════════════════════════════════════════════════════
//  Harness
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct ViewHandle {
    std::unique_ptr<::aria::adapters::uikit::UIKitView> wrapper;
    UIView* __strong native = nil;
    ::aria::binding::IView& view() { return *wrapper; }
};

class UIKitHarness {
public:
    [[nodiscard]] std::shared_ptr<::aria::binding::IViewAdapter> make_adapter() {
        return std::make_shared<::aria::adapters::uikit::UIKitAdapter>();
    }

    ViewHandle make_text_view() {
        UITextField* tf = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 100, 22)];
        tf.text = @"";
        ViewHandle h; h.native = tf;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(tf);
        return h;
    }
    ViewHandle make_bool_view() {
        UISwitch* sw = [[UISwitch alloc] initWithFrame:CGRectZero];
        sw.on = NO;
        ViewHandle h; h.native = sw;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(sw);
        return h;
    }
    ViewHandle make_int_view() {
        UIStepper* s = [[UIStepper alloc] initWithFrame:CGRectZero];
        s.minimumValue = -1000000;
        s.maximumValue =  1000000;
        s.value = 0;
        ViewHandle h; h.native = s;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(s);
        return h;
    }
    ViewHandle make_double_view() {
        UISlider* s = [[UISlider alloc] initWithFrame:CGRectMake(0, 0, 100, 22)];
        s.minimumValue = -1000.0f;
        s.maximumValue =  1000.0f;
        s.value = 0;
        ViewHandle h; h.native = s;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(s);
        return h;
    }
    ViewHandle make_click_view() {
        UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(0, 0, 100, 22);
        [b setTitle:@"Click" forState:UIControlStateNormal];
        ViewHandle h; h.native = b;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(b);
        return h;
    }

    void user_type(::aria::binding::IView& v, std::string text) {
        auto& uv = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UITextField* tf = uv.as<UITextField>();
        tf.text = [NSString stringWithUTF8String:text.c_str()];
        [tf sendActionsForControlEvents:UIControlEventEditingChanged];
    }
    void user_toggle(::aria::binding::IView& v, bool b) {
        auto& uv = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UISwitch* sw = uv.as<UISwitch>();
        [sw setOn:b animated:NO];
        [sw sendActionsForControlEvents:UIControlEventValueChanged];
    }
    void user_set_int(::aria::binding::IView& v, int n) {
        auto& uv = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIControl* c = uv.as<UIControl>();
        if ([c isKindOfClass:[UIStepper class]]) ((UIStepper*)c).value = n;
        else if ([c isKindOfClass:[UISlider class]]) ((UISlider*)c).value = (float)n;
        [c sendActionsForControlEvents:UIControlEventValueChanged];
    }
    void user_set_double(::aria::binding::IView& v, double d) {
        auto& uv = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIControl* c = uv.as<UIControl>();
        if ([c isKindOfClass:[UISlider class]])  ((UISlider*)c).value = (float)d;
        else if ([c isKindOfClass:[UIStepper class]]) ((UIStepper*)c).value = d;
        [c sendActionsForControlEvents:UIControlEventValueChanged];
    }
    void user_click(::aria::binding::IView& v) {
        auto& uv = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIButton* b = uv.as<UIButton>();
        [b sendActionsForControlEvents:UIControlEventTouchUpInside];
    }
};

// ═══════════════════════════════════════════════════════════════════════
//  9 conformance scenarios (inlined; mirrors adapter_conformance.hpp)
// ═══════════════════════════════════════════════════════════════════════

void run_text_two_way(UIKitHarness& h) {
    auto vh = h.make_text_view();
    auto& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_text(v, "hello");
    AR_CHECK(adapter->get_text(v) == "hello");

    int hits = 0; std::string last;
    auto sub = adapter->on_text_changed(v, [&](std::string_view sv) {
        ++hits; last = std::string(sv);
    });
    h.user_type(v, "world");
    AR_CHECK(hits == 1);
    AR_CHECK(last == "world");
    sub.release();
    h.user_type(v, "after-release");
    AR_CHECK(hits == 1);
}

void run_text_engine_two_way(UIKitHarness& h) {
    auto adapter = h.make_adapter();
    ::aria::binding::BindingEngine engine(adapter);
    ::aria::Property<std::string> p("alpha");

    auto vh = h.make_text_view(); auto& v = vh.view();
    engine.bind_text(p, v);
    AR_CHECK(engine.adapter().get_text(v) == "alpha");
    p = "beta";
    AR_CHECK(engine.adapter().get_text(v) == "beta");
    h.user_type(v, "gamma");
    AR_CHECK(p.get() == "gamma");
}

void run_bool_two_way(UIKitHarness& h) {
    auto vh = h.make_bool_view(); auto& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_bool(v, true);
    AR_CHECK(adapter->get_bool(v));

    int hits = 0; bool last = false;
    auto sub = adapter->on_bool_changed(v, [&](bool b) { ++hits; last = b; });
    h.user_toggle(v, false);
    AR_CHECK(hits == 1);
    AR_CHECK_FALSE(last);
}

void run_bool_engine_two_way(UIKitHarness& h) {
    auto adapter = h.make_adapter();
    ::aria::binding::BindingEngine engine(adapter);
    ::aria::Property<bool> p(false);

    auto vh = h.make_bool_view(); auto& v = vh.view();
    engine.bind_bool(p, v);
    AR_CHECK_FALSE(engine.adapter().get_bool(v));
    p = true;
    AR_CHECK(engine.adapter().get_bool(v));
    h.user_toggle(v, false);
    AR_CHECK_FALSE(p.get());
}

void run_int_two_way(UIKitHarness& h) {
    auto vh = h.make_int_view(); auto& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_int(v, 42);
    AR_CHECK(adapter->get_int(v) == 42);

    int last = -1;
    auto sub = adapter->on_int_changed(v, [&](int n) { last = n; });
    h.user_set_int(v, 7);
    AR_CHECK(last == 7);
}

void run_double_two_way(UIKitHarness& h) {
    auto vh = h.make_double_view(); auto& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_double(v, 3.25);
    AR_CHECK(std::abs(adapter->get_double(v) - 3.25) < 0.01);

    double last = -1.0;
    auto sub = adapter->on_double_changed(v, [&](double d) { last = d; });
    h.user_set_double(v, 7.5);
    AR_CHECK(std::abs(last - 7.5) < 0.01);
}

void run_click(UIKitHarness& h) {
    auto vh = h.make_click_view(); auto& v = vh.view();
    auto adapter = h.make_adapter();

    int hits = 0;
    auto sub = adapter->on_click(v, [&]() { ++hits; });
    h.user_click(v);
    h.user_click(v);
    AR_CHECK(hits == 2);
    sub.release();
    h.user_click(v);
    AR_CHECK(hits == 2);
}

void run_command_enabled(UIKitHarness& h) {
    auto adapter = h.make_adapter();
    ::aria::binding::BindingEngine engine(adapter);

    int n = 0;
    ::aria::Property<bool> gate(false);
    ::aria::Command<> cmd(
        [&]() { ++n; },
        [&]() { return gate.get(); }
    );

    auto vh = h.make_click_view(); auto& v = vh.view();
    engine.bind_command(cmd, v);
    h.user_click(v);
    AR_CHECK(n == 0);
    gate = true;
    h.user_click(v);
    AR_CHECK(n == 1);
    gate = false;
    h.user_click(v);
    AR_CHECK(n == 1);
}

void run_view_destroy_safety(UIKitHarness& h) {
    auto adapter = h.make_adapter();
    ::aria::binding::BindingEngine engine(adapter);

    ::aria::Property<std::string> survivor("alive");
    auto survivor_vh = h.make_text_view();
    engine.bind_text(survivor, survivor_vh.view());

    ::aria::Property<std::string> dying("x");
    {
        auto dying_vh = h.make_text_view();
        engine.bind_text(dying, dying_vh.view());
        AR_CHECK(engine.adapter().get_text(dying_vh.view()) == "x");
        dying = "y";
        AR_CHECK(engine.adapter().get_text(dying_vh.view()) == "y");
    }
    dying = "after-death";  // must not crash
    survivor = "still here";
    AR_CHECK(engine.adapter().get_text(survivor_vh.view()) == "still here");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
//  Public ObjC entry point
// ═══════════════════════════════════════════════════════════════════════

@implementation UIKitConformanceRunner

+ (void)runAndLog {
    NSLog(@"═══ Aria UIKit conformance battery — start ═══");

    UIKitHarness h;

    struct Scenario { const char* name; void (*fn)(UIKitHarness&); };
    static const Scenario kCases[] = {
        {"text two-way",         &run_text_two_way},
        {"text engine two-way",  &run_text_engine_two_way},
        {"bool two-way",         &run_bool_two_way},
        {"bool engine two-way",  &run_bool_engine_two_way},
        {"int two-way",          &run_int_two_way},
        {"double two-way",       &run_double_two_way},
        {"click",                &run_click},
        {"command + enabled",    &run_command_enabled},
        {"view destroy safety",  &run_view_destroy_safety},
    };

    for (const auto& c : kCases) {
        int pass_before = aria_conf_shim::stats().pass.load();
        int fail_before = aria_conf_shim::stats().fail.load();
        @try {
            c.fn(h);
        } @catch (NSException* e) {
            NSLog(@"  ✗ %-24s  threw NSException: %@", c.name, e.reason);
            aria_conf_shim::stats().fail.fetch_add(1);
            continue;
        }
        int dp = aria_conf_shim::stats().pass.load() - pass_before;
        int df = aria_conf_shim::stats().fail.load() - fail_before;
        if (df == 0) {
            NSLog(@"  ✓ %-24s  (%d assertions)", c.name, dp);
        } else {
            NSLog(@"  ✗ %-24s  (%d pass, %d FAIL)", c.name, dp, df);
        }
    }

    int total_pass = aria_conf_shim::stats().pass.load();
    int total_fail = aria_conf_shim::stats().fail.load();
    NSLog(@"═══ Aria UIKit conformance: %d passed / %d failed ═══",
          total_pass, total_fail);

    if (total_fail > 0) {
        std::lock_guard lk(aria_conf_shim::stats().m);
        for (const auto& m : aria_conf_shim::stats().failure_messages) {
            NSLog(@"   %s", m.c_str());
        }
    }
}

+ (UILabel*)makeResultBanner {
    int pass = aria_conf_shim::stats().pass.load();
    int fail = aria_conf_shim::stats().fail.load();
    UILabel* l = [[UILabel alloc] init];
    l.textAlignment = NSTextAlignmentCenter;
    l.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
    l.text = [NSString stringWithFormat:@"Conformance: %d pass / %d fail",
              pass, fail];
    l.backgroundColor = (fail == 0)
        ? [UIColor colorWithRed:0.2 green:0.6 blue:0.2 alpha:1.0]
        : [UIColor colorWithRed:0.8 green:0.2 blue:0.2 alpha:1.0];
    l.textColor = [UIColor whiteColor];
    return l;
}

+ (BOOL)allPassed {
    return aria_conf_shim::stats().fail.load() == 0;
}

+ (NSInteger)passCount { return aria_conf_shim::stats().pass.load(); }
+ (NSInteger)failCount { return aria_conf_shim::stats().fail.load(); }

@end
