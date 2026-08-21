# UIKit Adapter

Integrating Aria with iOS native UIKit applications. The UIKit adapter provides `UIKitAdapter` (widget binding) and `UIKitView` (view wrapper).

**Include:** `#include "aria/adapters/uikit/UIKitAdapter.hpp"`

> **Note:** This header is `.mm`-only — it imports `<UIKit/UIKit.h>`. Compile the consuming file as Objective-C++. The CMake target is INTERFACE because UIKit only links inside an iOS app target; the consuming application compiles the `.mm` directly.

---

## Setup

### Initialize

```objc++
#import "aria/adapters/uikit/UIKitAdapter.hpp"

aria::adapters::uikit::UIKitAdapter adapter;
aria::binding::BindingEngine engine(adapter,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
```

---

## UIKitView — Wrap Any UIView

```objc++
// Wrap a native UIView*
auto view = std::make_shared<aria::adapters::uikit::UIKitView>(textField);
```

`UIKitView` retains the `UIView*` via ARC (`__strong`). On destruction, `fire_destroy_()` is called while the native view is still valid, letting `BindingEngine` clean up subscriptions.

### Type-Safe Cast

```objc++
auto* tf = view->as<UITextField>();
```

---

## Widget Bindings

### Text (UITextField)

```objc++
aria::Property<std::string> query{""};

// Two-way: user typing → Property, Property → text field
auto tf_view = std::make_shared<aria::adapters::uikit::UIKitView>(searchField);
engine.bind_text(query, *tf_view);
```

Internally, `on_text_changed` uses an `AriaUITextTarget` that hooks into `UIControlEventEditingChanged`.

### Bool (UISwitch)

```objc++
aria::Property<bool> notifications_enabled{true};

auto sw_view = std::make_shared<aria::adapters::uikit::UIKitView>(toggleSwitch);
engine.bind_bool(notifications_enabled, *sw_view);
```

Uses `AriaUIToggleTarget` hooked to `UIControlEventValueChanged`.

### Int (UIStepper)

```objc++
aria::Property<int> quantity{1};

auto stepper_view = std::make_shared<aria::adapters::uikit::UIKitView>(stepper);
engine.bind_int(quantity, *stepper_view);
```

Uses `AriaUIStepperTarget` hooked to `UIControlEventValueChanged`.

### Double (UISlider)

```objc++
aria::Property<double> brightness{0.5};

auto slider_view = std::make_shared<aria::adapters::uikit::UIKitView>(brightnessSlider);
engine.bind_double(brightness, *slider_view);
```

Uses `AriaUISliderTarget` hooked to `UIControlEventValueChanged`.

### Click (UIButton)

```objc++
aria::Command<> submit_cmd{[&] { do_submit(); }};

auto btn_view = std::make_shared<aria::adapters::uikit::UIKitView>(submitButton);
engine.bind_command(submit_cmd, *btn_view);
```

Uses `AriaUIClickTarget` hooked to `UIControlEventTouchUpInside`.

### Visible / Enabled (Any UIView)

```objc++
aria::Property<bool> has_results{false};

auto panel_view = std::make_shared<aria::adapters::uikit::UIKitView>(resultsContainer);
engine.bind_visible(has_results, *panel_view);
engine.bind_enabled(can_submit, *btn_view);
```

---

## Supported Controls Summary

| Control | Binding | Direction |
|---------|---------|-----------|
| `UITextField` | `bind_text` | Two-way |
| `UILabel` | `bind_text_oneway` | VM→View |
| `UISwitch` | `bind_bool` | Two-way |
| `UIStepper` | `bind_int` | Two-way |
| `UISlider` | `bind_double` | Two-way |
| `UIButton` | `bind_command` | View→VM |
| Any `UIView` | `bind_visible` / `bind_enabled` | VM→View |

---

## Threading

UIKit widgets must be accessed from the main thread. The adapter assumes all methods are called there. If you write to a `Property` from a background thread, use `DispatchPolicy::SmartMarshal` with a main-thread dispatcher to ensure VM→View updates land on the main thread.

---

## Full Example

```objc++
// ViewController.mm
#import "aria/adapters/uikit/UIKitAdapter.hpp"
#import "aria/binding/binding_engine.hpp"

@interface SearchViewController ()
@end

@implementation SearchViewController {
    aria::adapters::uikit::UIKitAdapter _adapter;
    std::unique_ptr<aria::binding::BindingEngine> _engine;
    SearchVm _vm;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _engine = std::make_unique<aria::binding::BindingEngine>(
        _adapter, aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);

    _engine->bind_text(_vm.query,
        *std::make_shared<aria::adapters::uikit::UIKitView>(self.searchField));
    _engine->bind_text_oneway(_vm.result,
        *std::make_shared<aria::adapters::uikit::UIKitView>(self.resultLabel));
    _engine->bind_command(_vm.search_cmd,
        *std::make_shared<aria::adapters::uikit::UIKitView>(self.searchBtn));
}

@end
```

---

## See Also

- [View Binding →](../binding.md) — BindingEngine API reference
- [AppKit Adapter →](appkit.md) — macOS counterpart
- [AriaTools →](https://github.com/dqsjqian/AriaTools) — flagship cross-platform application (Qt, iOS, Android; Web in progress)
