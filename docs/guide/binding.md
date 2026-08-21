# View Binding

`BindingEngine` bridges the gap between C++ ViewModels and native platform views. It manages the lifecycle of every binding — VM→View and View→VM — and handles thread marshalling automatically.

**Include:** `#include "aria/binding/binding_engine.hpp"`

---

## BindingEngine

### Construction

```cpp
#include "aria/binding/binding_engine.hpp"

// The engine always needs an adapter — it is the only way it can reach
// your UI toolkit. `adapter` here is a std::shared_ptr<IViewAdapter>,
// e.g. std::make_shared<aria::adapters::QtAdapter>().

// Simplest: direct dispatch (single-threaded, no marshal)
aria::binding::BindingEngine engine{adapter};

// Production: SmartMarshal — auto-posts from background threads
aria::binding::BindingEngine engine(adapter, dispatcher,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
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

### bind_text_projected

Render any `Property<T>` into a read-only text view through a `T -> std::string`
projection. This is the lightweight one-way alternative to a full bidirectional
`Converter` when the view never writes back (labels, status lines):

```cpp
aria::Property<int> unread{3};

engine.bind_text_projected(unread, badge_label,
    [](int n) { return std::to_string(n) + " new"; });
// label shows "3 new"; re-renders whenever `unread` changes
```

### bind_optional_text

Bind a `Property<std::optional<T>>` to a read-only text view. When the optional
holds a value it is rendered through the projection; when it is `std::nullopt`
the view shows `empty_text` (default: empty string):

```cpp
aria::Property<std::optional<std::string>> user{std::nullopt};

engine.bind_optional_text(user, welcome_label,
    [](const std::string& name) { return "Welcome, " + name; },
    "(signed out)");
// nullopt → "(signed out)"; user = "Alice" → "Welcome, Alice"
```

> **Async-agnostic by design.** Both helpers operate purely on `Property<T>`,
> so `BindingEngine` stays decoupled from `aria-async` at the API level (the
> engine never names an `AsyncCommand` type). They are the
> idiomatic way to render an `AsyncCommand`'s observable properties —
> `last_error_message` (a `Property<std::string>`) with `bind_text_projected`,
> and `last_result` (a `Property<std::optional<R>>`) with `bind_optional_text` —
> without teaching `BindingEngine` about coroutines. See
> [View-Destroy Cancellation](../cookbook/07-view-destroy-cancellation.md).

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

When the VM type doesn't match the view type, you have two options.

### One-way display: `bind_text_projected`

For a read-only label, pass a plain projection functor (`T -> std::string`).
No parsing back, no `Converter` needed:

```cpp
aria::Property<bool> is_admin{true};

// Bool → string for display
engine.bind_text_projected(is_admin, role_label,
    [](bool v) -> std::string { return v ? "Admin" : "Guest"; });
```

### Two-way: `bind_text_converted`

When the view must also write back, supply a `Converter<T, std::string>`:

```cpp
#include "aria/binding/converter.hpp"

aria::Property<int> quantity{1};

auto conv = aria::binding::converters::int_to_string();
engine.bind_text_converted(quantity, quantity_field, conv);
```

On the View → Model direction a built-in converter reports unparseable
input through `try_to_model` (returning `std::nullopt`) or by throwing
`ConversionError`; in both cases the engine **skips the model write**, so
the previous value stays authoritative rather than being clobbered with a
default-constructed one.

### Built-In Converters

All live in `aria::binding::converters` and return a `Converter<T, U>`:

| Factory | From | To |
|---------|------|----|
| `identity_string()` | `string` | `string` |
| `int_to_string()` | `int` | `string` |
| `double_to_string(int precision = 2)` | `double` | `string` |
| `bool_to_yes_no()` | `bool` | `string` (`"yes"` / `"no"`; parses `yes`/`true`/`1` and `no`/`false`/`0`) |

For anything else, construct a `Converter<T, U>` directly (or use
`bind_text_projected` when the binding is one-way).

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

Every binding the engine ships, with its direction and the source types it
accepts. Two rules explain the whole table:

- **One-way (VM→View) bindings accept any read-only reactive source** —
  `Property<T>` *or* `Computed<T>` (formally, anything satisfying
  `aria::ReadOnlyReactive`). A derived value needs no mirror property.
- **Two-way bindings accept `Property<T>` only.** A `Computed` has no
  write-back path, so passing one is a compile error rather than a silently
  dropped edit.

### Scalar bindings

| Method | Direction | Source | View channel |
|--------|-----------|--------|--------------|
| `bind_text` | Two-way | `Property<string>` | text input |
| `bind_text_oneway` | VM→View | `Property<string>` / `Computed<string>` | label text |
| `bind_bool` | Two-way | `Property<bool>` | checkbox / switch |
| `bind_bool_oneway` | VM→View | `Property<bool>` / `Computed<bool>` | checkbox / switch |
| `bind_int` | Two-way | `Property<int>` | spin box / slider |
| `bind_int_oneway` | VM→View | `Property<int>` / `Computed<int>` | spin box / progress bar |
| `bind_int64` | Two-way | `Property<int64_t>` | 64-bit numeric input |
| `bind_int64_oneway` | VM→View | `Property<int64_t>` / `Computed<int64_t>` | 64-bit numeric display |
| `bind_uint64` | Two-way | `Property<uint64_t>` | unsigned numeric input |
| `bind_uint64_oneway` | VM→View | `Property<uint64_t>` / `Computed<uint64_t>` | unsigned numeric display |
| `bind_float` | Two-way | `Property<float>` | slider / opacity |
| `bind_float_oneway` | VM→View | `Property<float>` / `Computed<float>` | slider / opacity |
| `bind_double` | Two-way | `Property<double>` | slider / double spin box |
| `bind_double_oneway` | VM→View | `Property<double>` / `Computed<double>` | numeric display |

### Text projection and conversion

| Method | Direction | Source | Notes |
|--------|-----------|--------|-------|
| `bind_text_projected` | VM→View | any `ReadOnlyReactive` of `T` | renders via `T → string`; the everyday "formatted label" binding |
| `bind_optional_text` | VM→View | any `ReadOnlyReactive` of `optional<T>` | renders `*opt` via `T → string`; `nullopt` shows `empty_text` |
| `bind_text_converted` | Two-way | `Property<T>` | full `Converter<T, string>`, parses text back into `T` |
| `bind_text_converted_oneway` | VM→View | any `ReadOnlyReactive` of `T` | uses only the converter's `to_view` |

### State, commands, lifetime

| Method | Direction | Source | Notes |
|--------|-----------|--------|-------|
| `bind_visible` | VM→View | `Property<bool>` / `Computed<bool>` | inherently one-way — the view never writes visibility back |
| `bind_enabled` | VM→View | `Property<bool>` / `Computed<bool>` | inherently one-way |
| `bind_command` | View→VM | `Command<Args...>` | click → `execute(args...)`; also drives `enabled` from `can_execute(args...)` |
| `bind_view_lifetime` | — | `std::function<void()>` | fires once on view-destroy (or engine teardown); use to cancel in-flight async work |
| `adopt` | — | `Subscription` | hands an arbitrary subscription to the view's per-view bucket, released on view-destroy |

> **When no binding fits**, write the `on_changed` by hand and hand the
> resulting `Subscription` to `adopt(view, std::move(sub))`. That keeps the
> lifetime story identical to a real binding — released on view-destroy — and
> avoids the usual workaround of a long-lived subscription vector that never
> releases anything.
>
> Composite labels are the common case: a binding has exactly one source, so
> a label reading *two* values either gets a `Computed` on the ViewModel that
> combines them (then it is a one-line `bind_text_oneway`), or stays a manual
> `on_changed` + `adopt`.

### Binding a derived value

```cpp
// ViewModel
Property<double> bill{20.0};
Property<int>    people{2};
Computed<double> per_person{[this] { return bill.get() / people.get(); }};

// View — Computed binds directly; no mirror Property, no manual on_changed.
engine.bind_text_projected(vm.per_person, label,
    [](double v) { return std::format("¥ {:.2f}", v); });

engine.bind_text(vm.per_person, input);   // ✗ compile error: not writable
```

---

## See Also

- [ViewModel →](viewmodel.md) — the VM side of the binding
- [Reactive Core →](reactive-core.md) — `Property` and `Command` that bindings consume
- [Lifecycle & Threading →](../reference/lifecycle.md) — thread-affinity and dispatch policies
