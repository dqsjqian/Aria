# AppKit Adapter

Integrating Aria with macOS native AppKit applications. The AppKit adapter provides `AppKitAdapter` (widget binding), `AppKitView` (view wrapper), and `ObservableTableSource` (NSTableView binding).

**Include:** `#include "aria/adapters/appkit/AppKitAdapter.hpp"`, `#include "aria/adapters/appkit/AppKitTableSource.hpp"`

> **Note:** These headers are `.mm`-only — they import `<Cocoa/Cocoa.h>`. Compile the consuming file as Objective-C++.

---

## Setup

### Initialize

```objc++
#import "aria/adapters/appkit/AppKitAdapter.hpp"

aria::adapters::appkit::AppKitAdapter adapter;
aria::binding::BindingEngine engine(adapter,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
```

---

## AppKitView — Wrap Any NSView

```objc++
// Wrap a native NSView*
auto view = std::make_shared<aria::adapters::appkit::AppKitView>(textField);
```

`AppKitView` retains the `NSView*` via ARC (`__strong`). When the wrapper is destroyed, `fire_destroy_()` is called while the native view is still valid, allowing `BindingEngine` to clean up subscriptions.

### Type-Safe Cast

```objc++
auto* tf = view->as<NSTextField>();
```

---

## Widget Bindings

### Text (NSTextField — editable + uneditable)

```objc++
aria::Property<std::string> name{"Alice"};

// Two-way: user typing → Property, Property → text field
auto tf_view = std::make_shared<aria::adapters::appkit::AppKitView>(nameField);
engine.bind_text(name, *tf_view);

// One-way: Property → label display only
auto lbl_view = std::make_shared<aria::adapters::appkit::AppKitView>(labelField);
engine.bind_text_oneway(name, *lbl_view);
```

Internally, `on_text_changed` uses an `AriaTextDelegate` conforming to `NSTextFieldDelegate`, capturing `textDidChange:` notifications.

### Bool (NSButton with NSSwitchButton style)

```objc++
aria::Property<bool> enabled{true};

auto sw_view = std::make_shared<aria::adapters::appkit::AppKitView>(switchButton);
engine.bind_bool(enabled, *sw_view);
```

Uses `AriaToggleTarget` as the action target for the switch button.

### Int (NSStepper)

```objc++
aria::Property<int> count{0};

auto stepper_view = std::make_shared<aria::adapters::appkit::AppKitView>(stepper);
engine.bind_int(count, *stepper_view);
```

### Double (NSSlider)

```objc++
aria::Property<double> brightness{0.5};

auto slider_view = std::make_shared<aria::adapters::appkit::AppKitView>(slider);
engine.bind_double(brightness, *slider_view);
```

### Click (NSButton — push button)

```objc++
aria::Command<> save_cmd{[&] { do_save(); }};

auto btn_view = std::make_shared<aria::adapters::appkit::AppKitView>(saveButton);
engine.bind_command(save_cmd, *btn_view);
```

Uses `AriaClickTarget` as the action target.

### Visible / Enabled (Any NSView)

```objc++
aria::Property<bool> has_results{false};

auto panel_view = std::make_shared<aria::adapters::appkit::AppKitView>(resultsPanel);
engine.bind_visible(has_results, *panel_view);
engine.bind_enabled(can_save, *btn_view);
```

---

## Supported Controls Summary

| Control | Binding | Direction |
|---------|---------|-----------|
| `NSTextField` (editable) | `bind_text` | Two-way |
| `NSTextField` (uneditable) | `bind_text_oneway` | VM→View |
| `NSButton` (switch/check) | `bind_bool` | Two-way |
| `NSStepper` | `bind_int` | Two-way |
| `NSSlider` | `bind_double` | Two-way |
| `NSButton` (push) | `bind_command` | View→VM |
| Any `NSView` | `bind_visible` / `bind_enabled` | VM→View |

---

## ObservableTableSource — Drive NSTableView

Bridge any `ObservableList<T>` (or derived list) onto `NSTableView`:

```objc++
#import "aria/adapters/appkit/AppKitTableSource.hpp"

// Define render callback
auto render = [](NSTableView* tv, NSTableColumn* col,
                 std::shared_ptr<Task> item, NSInteger row) -> NSView* {
    NSTableCellView* cell = [tv makeViewWithIdentifier:@"TaskCell" owner:nil];
    cell.textField.stringValue =
        [NSString stringWithUTF8String:item->title.c_str()];
    return cell;
};

// Create bridge
aria::adapters::appkit::ObservableTableSource<Task> source{
    tableView, vm.tasks, render};
```

### Event Mapping

| Aria Event | NSTableView Call |
|------------|-----------------|
| `Insert` | `insertRowsAtIndexes:withAnimation:` |
| `Remove` | `removeRowsAtIndexes:withAnimation:` |
| `Replace` / `ItemChanged` | `reloadDataForRowIndexes:columnIndexes:` |
| `Move` | `moveRowAtIndex:toIndex:` |
| `Reset` | `reloadData` |

### Thread Safety

If the source list mutates from a background thread, the bridge dispatches the change to the main queue via `dispatch_async(dispatch_get_main_queue(), ...)`. Items are resolved at emit time to prevent stale reads.

### Lifetime

The bridge holds a non-owning `__weak` reference to the `NSTableView`. On destruction:

1. Drops the subscription (stops receiving events)
2. Sets `detached` flag (in-flight queued blocks bail out)
3. Zeros `dataSource` / `delegate` if still pointing at the bridge's ObjC object

---

## Threading

AppKit widgets must be accessed from the main thread. The adapter assumes all methods are called there. If you write to a `Property` from a background thread, use `DispatchPolicy::SmartMarshal` with a main-thread dispatcher to ensure VM→View updates land on the main thread.

---

## Full Example

```objc++
// ViewController.mm
#import "aria/adapters/appkit/AppKitAdapter.hpp"
#import "aria/binding/binding_engine.hpp"

@interface MainWindowController ()
@end

@implementation MainWindowController {
    aria::adapters::appkit::AppKitAdapter _adapter;
    std::unique_ptr<aria::binding::BindingEngine> _engine;
    SearchVm _vm;
}

- (void)windowDidLoad {
    [super windowDidLoad];

    _engine = std::make_unique<aria::binding::BindingEngine>(
        _adapter, aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);

    _engine->bind_text(_vm.query,
        *std::make_shared<aria::adapters::appkit::AppKitView>(self.searchField));
    _engine->bind_text_oneway(_vm.result,
        *std::make_shared<aria::adapters::appkit::AppKitView>(self.resultLabel));
    _engine->bind_command(_vm.search_cmd,
        *std::make_shared<aria::adapters::appkit::AppKitView>(self.searchBtn));
}

@end
```

---

## See Also

- [View Binding →](../binding.md) — BindingEngine API reference
- [Collections →](../collections.md) — ObservableList and derived views
- [AriaTools →](https://github.com/dqsjqian/AriaTools) — flagship cross-platform application (Qt, iOS, Android; Web in progress)
