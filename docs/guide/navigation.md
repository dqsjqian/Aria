# Navigation

`Navigator` is a UI-toolkit-agnostic navigation stack for ViewModels. It manages a stack of `ViewModel` entries with push/pop semantics, modal presentation, result passing, per-entry cancellation, and deep-link routing.

**Include:** `#include "aria/binding/navigation.hpp"`

---

## Basic Stack Navigation

### Create a Navigator

```cpp
auto nav = std::make_shared<aria::binding::Navigator>();
```

### Push and Pop

```cpp
auto home_vm = std::make_shared<HomeVm>();
nav->push(home_vm);

auto settings_vm = std::make_shared<SettingsVm>();
nav->push(settings_vm);

nav->pop();           // back to home_vm
nav->pop_to_root();   // back to bottom of stack
nav->clear();         // remove everything
```

### Current Entry

```cpp
nav->current().bind([](std::shared_ptr<aria::binding::ViewModel> vm) {
    if (vm) render(vm);
});
```

### Stack Depth

```cpp
nav->depth();  // std::size_t
```

---

## Modal Presentation

Entries can be pushed as modals — they overlay without disturbing the back-stack:

```cpp
auto dialog_vm = std::make_shared<ConfirmDialogVm>();
nav->push(dialog_vm, aria::binding::Presentation::Modal);
```

### Dismiss a Modal

```cpp
nav->dismiss_modal();  // Removes the topmost modal entry
```

### Pop Behaviour with Modals

- `pop()` removes the topmost entry regardless of presentation kind
- `pop_to_root()` bottoms out at the deepest non-modal entry; modals above are torn down

---

## Result Passing (push_for_result)

Like Android's `registerForActivityResult` or iOS delegate-back patterns:

```cpp
// Caller: push and await result
auto future = nav->push_for_result<EditResult>(edit_vm);

// Later, the callee settles the result:
nav->dismiss_with(EditResult{"saved", 42});

// Caller reads the result (any thread):
if (auto result = future.get()) {
    std::cout << "Got: " << result->message << "\n";
}
```

If the callee is popped without calling `dismiss_with`, the future resolves to `std::nullopt`.

---

## Per-Entry Cancellation

Every entry owns a `CancellationSource`. When an entry is popped (or the navigator drops it), the source fires:

```cpp
nav->push(detail_vm);

// Inside DetailVm, observe the entry's token:
auto entry = nav->current_entry();
entry->token();  // CancellationToken — fires when popped
```

This is independent from `ViewModelScope` — works even if the VM is cached for back-stack restoration.

---

## Deep-Link Routing

Register path patterns and factories:

```cpp
nav->register_route("users/{id}",
    [](const std::unordered_map<std::string, std::string>& params) {
        auto vm = std::make_shared<UserDetailVm>(params.at("id"));
        return vm;
    });

nav->register_route("settings",
    [](const auto&) { return std::make_shared<SettingsVm>(); });
```

### Navigate to a Route

```cpp
nav->route("users/42");
// Parses "users/{id}" → params = {{"id", "42"}}
// Creates UserDetailVm("42") and pushes it
```

### Route Options

```cpp
aria::binding::RouteOptions opts;
opts.clear_stack = true;                       // replace entire stack
opts.presentation = aria::binding::Presentation::Modal;  // present as modal

nav->route("users/42", opts);
```

---

## Replace Root

Swap the bottom of the stack (e.g. switching from login to main):

```cpp
nav->replace_root(main_vm);
```

---

## Entry Introspection

```cpp
// Current entry (top of stack)
auto entry = nav->current_entry();

// Entry properties
entry->vm;              // shared_ptr<ViewModel>
entry->kind;            // Presentation::Push or Modal
entry->cancel;          // CancellationSource
entry->route_path;      // string (set by deep-link)
```

---

## Integration with Platform Adapters

The navigator is platform-agnostic. Platform adapters observe `nav->current()` and render the appropriate native view:

- **Qt6**: `QtAdapter` listens to `current()` and swaps `QWidget` stacks
- **AppKit**: `AppKitAdapter` drives `NSWindowController` / `NSViewController` transitions
- **UIKit**: `UIKitAdapter` drives `UIViewController` push/pop
- **JNI/Android**: The JNI bridge maps navigation events to Jetpack Navigation

See [Adapters →](adapters/) for platform-specific details.

---

## Quick Reference

| Method | Description |
|--------|-------------|
| `push(vm, presentation)` | Push entry onto stack |
| `pop()` | Remove top entry |
| `pop_to_root()` | Pop to bottom non-modal |
| `clear()` | Remove all entries |
| `dismiss_modal()` | Remove topmost modal |
| `push_for_result<R>(vm)` | Push and return `shared_future<optional<R>>` |
| `dismiss_with(result)` | Settle the result promise |
| `replace_root(vm)` | Swap bottom entry |
| `register_route(pattern, factory)` | Register deep-link route |
| `route(uri, opts)` | Navigate to deep-link |
| `current()` | `Property<shared_ptr<ViewModel>>` |
| `depth()` | Current stack depth |

---

## See Also

- [ViewModel →](viewmodel.md) — what goes on the navigation stack
- [Async & Coroutines →](async.md) — `CancellationToken` used by per-entry cancellation
- [View Binding →](binding.md) — connecting navigated VMs to views
