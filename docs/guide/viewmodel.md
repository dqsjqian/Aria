# ViewModel

The `aria::binding::ViewModel` base class is the structural backbone of every Aria MVVM application. It provides lifecycle management, automatic cleanup, child composition, and structured concurrency — everything a ViewModel needs beyond raw reactive primitives.

**Header:** `aria/binding/view_model.hpp`

---

## Why a Base Class?

Without a common base, every ViewModel reinvents the same bookkeeping:

- Who cleans up subscriptions when the VM dies?
- How do you guarantee in-flight async work finishes before teardown?
- How do parent–child VMs coordinate activation?

`ViewModel` solves all three. It is **not** a pure interface — it carries real state (`is_active`, subscription bag, child list, destroy hooks) and provides default implementations you can override.

---

## Quick Start

```cpp
#include "aria/aria.hpp"
#include "aria/binding/view_model.hpp"

class GreetingVm : public aria::binding::ViewModel {
public:
    aria::Property<std::string> name{"World"};
    aria::Property<std::string> greeting{""};
    aria::Command<std::string> greet;

    GreetingVm() : greet([this](const std::string& who) {
        name.set(who);
    }) {
        // Auto-clean: subscription dies with the VM
        track(name.on_changed([this](const std::string&) {
            greeting.set("Hello, " + name.get() + "!");
        }));
    }
};
```

Three things happening:

1. **`Property<T>`** — observable state. Changes propagate to subscribers.
2. **`Command<T>`** — user-action entry point. Takes a parameter, executes a lambda.
3. **`track()`** — registers a `Subscription` in the VM's bag. Destroyed automatically.

---

## Lifecycle: Activate / Deactivate

Every ViewModel starts inactive. `activate()` and `deactivate()` control the lifecycle:

```cpp
auto vm = std::make_shared<GreetingVm>();

assert(!vm->is_active().get());   // starts inactive

vm->activate();
assert(vm->is_active().get());   // now active

vm->deactivate();
assert(!vm->is_active().get());  // back to inactive
```

Override `on_activate()` / `on_deactivate()` to react:

```cpp
class HomeVm : public aria::binding::ViewModel {
public:
    void on_activate() override {
        // Start polling, warm caches, etc.
    }

    void on_deactivate() override {
        // Pause animations, release transient resources, etc.
    }
};
```

**Guarantees:**

| Property | Guarantee |
|----------|-----------|
| Idempotent | Calling `activate()` twice does nothing on the second call |
| Atomic | `is_active` flips inside `reactive::batch` — observers see one flush |
| Ordered | `on_activate()` runs *before* `is_active` becomes `true` |
| Symmetric | `deactivate()` reverses: children first, then hook, then flag |

---

## Child Composition

ViewModels form a tree. Activating a parent activates all children; deactivating propagates downward:

```cpp
auto parent = std::make_shared<DashboardVm>();
auto sidebar = std::make_shared<SidebarVm>();
auto content = std::make_shared<ContentVm>();

parent->add_child(sidebar);
parent->add_child(content);

parent->activate();   // sidebar and content also activate
parent->deactivate(); // sidebar and content also deactivate
```

Typical pattern: a top-level `ShellVm` owns child VMs for each screen region.

---

## Automatic Cleanup

### Subscription Bag

`track(subscription)` adds a subscription to the VM's internal `SubscriptionBag`. When the VM is destroyed, every tracked subscription is released — no dangling callbacks.

```cpp
class ProfileVm : public aria::binding::ViewModel {
public:
    ProfileVm(DataService& svc) {
        track(user_name.on_changed([&](const std::string&) {
            svc.mark_dirty();
        }));
        // More subscriptions...
    }
    // No destructor boilerplate — bag cleans up automatically
};
```

### Destroy Hooks

For cleanup that isn't a `Subscription` (closing file handles, unregistering observers):

```cpp
class CameraVm : public aria::binding::ViewModel {
public:
    CameraVm() {
        camera_ = open_camera();
        add_destroy_hook([this] { camera_->close(); });
    }
private:
    Camera* camera_;
};
```

Destroy hooks fire in reverse registration order. Exceptions are routed through `aria::report_callback_failure` — they never escape the destructor.

---

## Structured Concurrency with ViewModelScope

When a ViewModel launches async work, you need to guarantee it finishes (or cancels) before the VM dies. `ViewModelScope` ties a `CoroutineScope` to the VM's lifetime:

**Header:** `aria/binding/view_model_scope.hpp`

```cpp
#include "aria/binding/view_model_scope.hpp"

class PollingVm : public aria::binding::ViewModel {
public:
    PollingVm() {
        scope_.attach(*this);  // Wire scope to VM's destroy hook
    }

    void start_polling() {
        scope_.launch([this](aria::async::CancellationToken tok) -> aria::async::Task<void> {
            while (!tok.is_cancelled()) {
                co_await schedule_after(timer_, 1s);
                tok.throw_if_cancelled();
                refresh_data();
            }
        });
    }

private:
    aria::binding::ViewModelScope scope_;
};
```

When `PollingVm` is destroyed:

1. The destroy hook calls `scope_.cancel_and_join()`
2. All in-flight coroutines receive a cancellation signal
3. The destructor waits (up to 5 s default) for them to exit
4. Stuck coroutines are reported as leaks via the async error sink

---

## Common Patterns

### Property + Command (Synchronous)

The simplest ViewModel: observable state + user actions.

```cpp
class CounterVm : public aria::binding::ViewModel {
public:
    aria::Property<int> count{0};
    aria::Command<> increment{[this]() { count.set(count.get() + 1); }};
    aria::Command<> decrement{[this]() { count.set(count.get() - 1); }};
    aria::Command<> reset{[this]() { count.set(0); }};
};
```

### Property + Computed (Derived State)

Computed values auto-update when their dependencies change:

```cpp
class TipCalcVm : public aria::binding::ViewModel {
public:
    aria::Property<double> bill{0.0};
    aria::Property<int>    tipPercent{15};
    aria::Property<int>    people{1};

    aria::Computed<double> tipAmount{[this] {
        return bill.get() * tipPercent.get() / 100.0;
    }};
    aria::Computed<double> total{[this] {
        return bill.get() + tipAmount.get();
    }};
    aria::Computed<double> perPerson{[this] {
        return total.get() / people.get();
    }};
};
```

### AsyncCommand (Three-State Async)

`AsyncCommand` manages `is_running`, `error`, and `result` properties automatically:

```cpp
class LoginVm : public aria::binding::ViewModel {
public:
    LoginVm(aria::async::IExecutor& ui, aria::async::IExecutor& worker)
        : login(ui, worker, [this](std::string user, std::string pass)
            -> aria::async::Task<LoginResult> {
            co_await aria::async::schedule_on(worker);
            co_await aria::async::sleep_for(500ms);
            if (pass.empty()) throw std::runtime_error("Empty password");
            co_return LoginResult{"Welcome, " + user};
        })
    {
        scope_.attach(*this);
    }

    aria::async::AsyncCommand<LoginResult, std::string, std::string> login;

private:
    aria::binding::ViewModelScope scope_;
};
```

### Shared Draft Across Child VMs

Multiple child VMs sharing a common data object:

```cpp
struct WizardDraft {
    aria::Property<std::string> username{""};
    aria::Property<std::string> email{""};
    aria::Property<std::string> theme{"Light"};
};

class Step1Vm : public aria::binding::ViewModel {
public:
    std::shared_ptr<WizardDraft> draft;
    explicit Step1Vm(std::shared_ptr<WizardDraft> d) : draft(std::move(d)) {}
};

class Step2Vm : public aria::binding::ViewModel {
public:
    std::shared_ptr<WizardDraft> draft;
    explicit Step2Vm(std::shared_ptr<WizardDraft> d) : draft(std::move(d)) {}
};
```

### Cross-Platform Bridge (JNI / AppKit / UIKit)

The ViewModel stays in C++. Platform adapters observe `Property` changes and push them to native UI:

```
C++ ViewModel  →  Property.on_changed  →  JNI / ObjC bridge  →  Native UI
                 ←  Command.execute    ←  Button tap          ←  Native UI
```

See adapter guides for platform-specific wiring:
- [JNI / Android](adapters/jni.md)
- [AppKit / macOS](adapters/appkit.md)
- [UIKit / iOS](adapters/uikit.md)
- [Qt6](adapters/qt6.md)

---

## API Reference

### Constructor / Destructor

| Method | Notes |
|--------|-------|
| `ViewModel()` | Default constructor. `is_active` starts `false`. |
| `virtual ~ViewModel()` | Runs destroy hooks in reverse order, cleans up bag and children. |

### Lifecycle

| Method | Notes |
|--------|-------|
| `Property<bool>& is_active()` | Read/write activation flag. |
| `virtual void on_activate()` | Override to react to activation. Runs before `is_active` flips. |
| `virtual void on_deactivate()` | Override to react to deactivation. Runs before `is_active` flips. |
| `void activate()` | Idempotent activation. Propagates to children. |
| `void deactivate()` | Idempotent deactivation. Propagates to children. |

### Composition

| Method | Notes |
|--------|-------|
| `void add_child(shared_ptr<ViewModel>)` | Register a child VM. Activated/deactivated with parent. |
| `void track(Subscription)` | Auto-release subscription on destruction. |
| `void add_destroy_hook(function<void()>)` | Run cleanup on destruction. Reverse order. Exceptions routed to error sink. |

### Internals

| Member | Notes |
|--------|-------|
| `SubscriptionBag& bag()` | Protected access to the subscription bag. |

### Type Alias

| Alias | Notes |
|-------|-------|
| `using IViewModel = ViewModel` | Compatibility alias for WPF/Avalonia/MAUI naming conventions. Despite the `I` prefix, this is a concrete base class with state. |

---

## See Also

- [Reactive Core →](reactive-core.md) — `Property`, `Computed`, `Effect`, `Subscription`
- [Async & Coroutines →](async.md) — `Task`, `AsyncCommand`, `CancellationToken`
- [View Binding →](binding.md) — `BindingEngine`, `IViewAdapter`, converters
- [Lifecycle & Threading →](../reference/lifecycle.md) — Thread-affinity and race-model contracts
