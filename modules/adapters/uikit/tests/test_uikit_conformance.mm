#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#import "aria/adapters/uikit/UIKitAdapter.hpp"
#import "aria/binding/testing/adapter_conformance.hpp"
#import "aria/reactive/reactive.hpp"

#import <UIKit/UIKit.h>

#include <memory>
#include <string>

namespace conformance = ::aria::binding::testing::conformance;

namespace {

void fire_actions(UIControl* control, UIControlEvents events) {
    for (id target in control.allTargets) {
        for (NSString* actionName in [control actionsForTarget:target
                                              forControlEvent:events]) {
            SEL action = NSSelectorFromString(actionName);
            if ([target respondsToSelector:action]) {
                IMP imp = [target methodForSelector:action];
                auto fn = reinterpret_cast<void (*)(id, SEL, id)>(imp);
                fn(target, action, control);
            }
        }
    }
}

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
        UITextField* field = [[UITextField alloc]
            initWithFrame:CGRectMake(0, 0, 100, 22)];
        ViewHandle h;
        h.native = field;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(field);
        return h;
    }

    ViewHandle make_bool_view() {
        UISwitch* control = [[UISwitch alloc] initWithFrame:CGRectZero];
        ViewHandle h;
        h.native = control;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(control);
        return h;
    }

    ViewHandle make_int_view() {
        UIStepper* control = [[UIStepper alloc] initWithFrame:CGRectZero];
        control.minimumValue = -1'000'000;
        control.maximumValue = 1'000'000;
        ViewHandle h;
        h.native = control;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(control);
        return h;
    }

    ViewHandle make_double_view() {
        // UIStepper stores a real double; using it here avoids introducing
        // UISlider's float quantization into the adapter contract battery.
        UIStepper* control = [[UIStepper alloc] initWithFrame:CGRectZero];
        control.minimumValue = -1000.0;
        control.maximumValue = 1000.0;
        ViewHandle h;
        h.native = control;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(control);
        return h;
    }

    ViewHandle make_click_view() {
        UIButton* control = [UIButton buttonWithType:UIButtonTypeSystem];
        ViewHandle h;
        h.native = control;
        h.wrapper = std::make_unique<::aria::adapters::uikit::UIKitView>(control);
        return h;
    }

    void user_type(::aria::binding::IView& v, std::string text) {
        auto& view = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UITextField* field = view.as<UITextField>();
        field.text = [NSString stringWithUTF8String:text.c_str()];
        fire_actions(field, UIControlEventEditingChanged);
    }

    void user_toggle(::aria::binding::IView& v, bool value) {
        auto& view = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UISwitch* control = view.as<UISwitch>();
        [control setOn:value animated:NO];
        fire_actions(control, UIControlEventValueChanged);
    }

    void user_set_int(::aria::binding::IView& v, int value) {
        auto& view = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIControl* control = view.as<UIControl>();
        if ([control isKindOfClass:[UIStepper class]]) {
            ((UIStepper*)control).value = value;
        } else if ([control isKindOfClass:[UISlider class]]) {
            ((UISlider*)control).value = static_cast<float>(value);
        }
        fire_actions(control, UIControlEventValueChanged);
    }

    void user_set_double(::aria::binding::IView& v, double value) {
        auto& view = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIControl* control = view.as<UIControl>();
        if ([control isKindOfClass:[UISlider class]]) {
            ((UISlider*)control).value = static_cast<float>(value);
        } else if ([control isKindOfClass:[UIStepper class]]) {
            ((UIStepper*)control).value = value;
        }
        fire_actions(control, UIControlEventValueChanged);
    }

    void user_click(::aria::binding::IView& v) {
        auto& view = static_cast<::aria::adapters::uikit::UIKitView&>(v);
        UIButton* control = view.as<UIButton>();
        fire_actions(control, UIControlEventTouchUpInside);
    }
};

}  // namespace

TEST_CASE("UIKitAdapter conforms: text two-way") {
    UIKitHarness h; conformance::run_text_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: text engine two-way") {
    UIKitHarness h; conformance::run_text_engine_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: bool two-way") {
    UIKitHarness h; conformance::run_bool_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: bool engine two-way") {
    UIKitHarness h; conformance::run_bool_engine_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: int two-way") {
    UIKitHarness h; conformance::run_int_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: double two-way") {
    UIKitHarness h; conformance::run_double_two_way(h);
}
TEST_CASE("UIKitAdapter conforms: click") {
    UIKitHarness h; conformance::run_click(h);
}
TEST_CASE("UIKitAdapter conforms: command + enabled") {
    UIKitHarness h; conformance::run_command_enabled(h);
}
TEST_CASE("UIKitAdapter conforms: view destroy safety") {
    UIKitHarness h; conformance::run_view_destroy_safety(h);
}
TEST_CASE("UIKitAdapter conforms: int64 via int view") {
    UIKitHarness h; conformance::run_int64_two_way_via_int_view(h);
}
TEST_CASE("UIKitAdapter conforms: uint64 via int view") {
    UIKitHarness h; conformance::run_uint64_two_way_via_int_view(h);
}
TEST_CASE("UIKitAdapter conforms: float via double view") {
    UIKitHarness h; conformance::run_float_two_way_via_double_view(h);
}
