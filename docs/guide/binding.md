# View Binding

`BindingEngine` bridges the gap between C++ ViewModels and native platform views. It manages the lifecycle of every binding — VM→View and View→VM — and handles thread marshalling automatically.

**Include:** `#include "aria/binding/binding_engine.hpp"`

---

## BindingEngine

### Construction

```cpp
#include "aria/binding/binding_engine.hpp"

// Simplest: direct dispatch (single-threaded, no marshal)
aria::binding::BindingEngine engine;

// Production: SmartMarshal — auto-posts from background threads
aria::binding::BindingEngine engine(dispatcher,
    aria::binding::DispatchPolicy::SmartMarshal);
```

### Dispatch Policies

| Policy | VM→View Path | Use Case |
|--------|---------------|----------|
| `Direct` | Synchronous, same thread | Single-threaded apps, tests |
| `SmartMarshal` | Direct on UI thread, post from background | Production apps |
| `AlwaysPost` | Always post via dispatcher | Tests needing deterministic ordering |

---

## One-Way Bindings (VM → View)

### bind_text_oneway

```cpp
aria::Property<std::string> status{"Ready"};

engine.bind_text_oneway(status, label_view);
// label displays "Ready"; updates when status changes
```

### bind_visible / bind_enabled

```cpp
aria::Property<bool> has_items{true};

engine.bind_visible(has_items, list_container);
engine.bind_enabled(can_submit, submit_button);
```

### bind_int_oneway / bind_double_oneway

```cpp
aria::Property<int> count{0};
aria::Property<double> progress{0.75};

engine.bind_int_oneway(count, badge_view);
engine.bind_double_oneway(progress, progress_bar);
```

---

## Two-Way Bindings (VM ↔ View)

Two-way bindings synchronize in both directions: VM changes update the view, and user input in the view writes back to the VM.

### bind_text

```cpp
aria::Property<std::string> query{""};

engine.bind_text(query, search_input);
// Typing in search_input → query.set(text)
// query.set("hello") → search_input displays "hello"
```

### bind_bool

```cpp
aria::Property<bool> dark_mode{false};

engine.bind_bool(dark_mode, toggle_switch);
```

### bind_int / bind_double

```cpp
aria::Property<int> quantity{1};
aria::Property<double> price{9.99};

engine.bind_int(quantity, spin_box);
engine.bind_double(price, price_input);
```

---

## Command Bindings

### bind_command (Parameterless)

```cpp
aria::Command<> save{[&] { do_save(); }};

engine.bind_command(save, save_button);
// Button click → save.execute()
// save.can_execute() drives button.enabled automatically
```

### bind_command (Parameterized)

```cpp
aria::Command<std::string> open{[&](const std::string& path) { /* ... */ }};

engine.bind_command(open, button, "documents/report.pdf");
// Button click → open.execute("documents/report.pdf")
```

---

## Converters

When the VM type doesn't match the view type, insert a converter:

```cpp
#include "aria/binding/converter.hpp"

aria::Property<bool> is_admin{true};

// Bool → string for display
engine.bind_text_oneway(
    aria::binding::convert(is_admin,
        [](bool v) -> std::string { return v ? "Admin" : "Guest"; }),
    role_label
);
```

### Built-In Converters

| Converter | From | To |
|-----------|------|----|
| `convert_bool_to_text` | `bool` | `string` ("true"/"false") |
| `convert_int_to_text` | `int` | `string` |
| `convert_double_to_text` | `double` | `string` |
| `invert_bool` | `bool` | `bool` (negated) |

---

## Feedback-Loop Suppression

Two-way bindings create a potential loop: VM change → view update → view callback → VM set (same value). The engine suppresses this automatically:

1. When pushing a value VM→View, the engine marks the binding as "writing"
2. The incoming View→VM callback checks this flag and skips if set
3. Result: no infinite ping-pong

---

## View Lifetime

Views can be destroyed before the engine. When a view is destroyed:

1. `IView::on_destroy` fires
2. The engine releases all bindings to that view
3. Subsequent property changes silently skip the dead view
4. Other bindings in the same engine remain active

```cpp
// View destroyed (e.g. user closes panel)
panel->close();  // triggers on_destroy

// Property changes no longer attempt to update panel
status.set("Done");  // no crash, no dangling reference
```

---

## Clear All Bindings

```cpp
engine.clear();  // Releases every binding in one shot
```

Called automatically in the engine's destructor.

---

## IViewAdapter / IView

Platform adapters implement `IViewAdapter` to teach the engine how to talk to native widgets:

- `IView` — represents a single native widget (text getter/setter, visibility, enabled, destroy callback)
- `IViewAdapter` — factory that creates `IView` wrappers for platform controls

See adapter guides for platform-specific implementations:
- [Qt6 →](adapters/qt6.md)
- [AppKit →](adapters/appkit.md)
- [UIKit →](adapters/uikit.md)
- [JNI / Android →](adapters/jni.md)

---

## Quick Reference

| Method | Direction | Types |
|--------|-----------|-------|
| `bind_text` | Two-way | `Property<string>` ↔ text input |
| `bind_text_oneway` | VM→View | `Property<string>` → label |
| `bind_bool` | Two-way | `Property<bool>` ↔ checkbox |
| `bind_int` | Two-way | `Property<int>` ↔ spin box |
| `bind_double` | Two-way | `Property<double>` ↔ slider |
| `bind_visible` | One-way | `Property<bool>` → visibility |
| `bind_enabled` | One-way | `Property<bool>` → enabled |
| `bind_command` | View→VM | `Command<>` ↔ button |

---

## See Also

- [ViewModel →](viewmodel.md) — the VM side of the binding
- [Reactive Core →](reactive-core.md) — `Property` and `Command` that bindings consume
- [Lifecycle & Threading →](../reference/lifecycle.md) — thread-affinity and dispatch policies
