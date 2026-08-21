# Recipe 8 — Writing a new `IViewAdapter`

**Goal:** bind Aria ViewModels to a new UI toolkit by implementing two
interfaces: `IView` (wraps one native handle) and `IViewAdapter` (knows
how to read/write/observe widgets). Once done, `BindingEngine` works
against your toolkit unchanged.

See `aria/binding/view_adapter.hpp` for the interface, the shipped
adapters (`modules/adapters/{qt6,appkit,uikit}`) for references, and the
`L-39` adapter contract in `docs/reference/lifecycle.md`.

## Step 1 — wrap the native handle in an `IView`

```cpp
#include "aria/binding/view_adapter.hpp"
using namespace aria::binding;

class GtkView : public IView {
public:
    explicit GtkView(GtkWidget* w) : widget_(w) {}

    std::string_view kind() const noexcept override { return "gtk"; }

    // STRONGLY RECOMMENDED: fire the destroy signal the moment the native
    // handle goes away, while this subclass's state is still valid (L-32).
    // The base ~IView fallback is too late — derived state is already gone.
    void on_native_destroyed() { fire_destroy_(); }

    GtkWidget* widget() const noexcept { return widget_; }
private:
    GtkWidget* widget_;
};
```

## Step 2 — implement `IViewAdapter`

`IViewAdapter` declares 25 pure virtuals: `set_` / `get_` / `on_*_changed`
for text, bool, int, int64, uint64, float and double, plus `set_visible`,
`set_enabled`, `on_click` and `platform_name`. Most toolkits do not need
all of them on day one.

**Start from `ViewAdapterBase`.** It implements every operation as the
compliant L-39 unsupported path — report through the diagnostics boundary,
then return a safe default (no-op setter, zeroed getter, empty
`Subscription`) — so you override only what your toolkit really supports,
and `warn_unsupported_` no longer has to be hand-rolled per adapter:

```cpp
#include "aria/binding/view_adapter_base.hpp"
using namespace aria::binding;

class GtkAdapter : public ViewAdapterBase {
public:
    // Still required — L-39.1 wants a stable lowercase id that EXACTLY
    // matches IView::kind(), and there is no sane default for it.
    std::string_view platform_name() const noexcept override { return "gtk"; }

    void set_text(IView& v, std::string_view t) override {
        if (auto* g = as_gtk_(v, "set_text"))
            gtk_label_set_text(GTK_LABEL(g->widget()), std::string{t}.c_str());
    }
    std::string get_text(IView& v) override {
        auto* g = as_gtk_(v, "get_text");
        return g ? gtk_label_get_text(GTK_LABEL(g->widget())) : std::string{};
    }
    Subscription on_text_changed(IView& v,
                                 std::function<void(std::string_view)> cb) override {
        // connect to the native "changed" signal; return a Subscription
        // whose deleter disconnects it.
        ...
    }

    Subscription on_click(IView& v, std::function<void()> cb) override { ... }

    // bool / int / int64 / uint64 / float / double / visible / enabled:
    // not implemented yet. Inherited from ViewAdapterBase, so a binding
    // that reaches one is reported and degrades safely rather than
    // silently doing nothing.

private:
    // Wrong-toolkit widget → delegate to the base's reporting path.
    GtkView* as_gtk_(IView& v, const char* op) {
        if (v.kind() != "gtk") { report_unsupported(op, v); return nullptr; }
        return static_cast<GtkView*>(&v);
    }
};
```

Then add channels as real screens need them. The canonical fast paths are
`text` / `bool` / `int` / `double`; the wider numeric variants
(`int64` / `uint64` / `float`) may delegate to a narrower one with a range
check (see `FakeAdapter` for the default pattern).

`report_unsupported(op, view)` is `virtual` — override it to throw during
development, or to route into a platform log instead of the framework's
callback-failure sink.

> **Deriving from `IViewAdapter` directly is still supported**, and is what
> the four first-party adapters do: they implement nearly the whole surface
> and *want* a compile error if the interface grows. Use the base when
> bringing up a new toolkit; use the raw interface when you intend to cover
> everything.

## Step 3 — verify against the conformance suite

Aria ships an adapter conformance suite so a new adapter is held to the
same contract as the shipped ones. **It is installed with the public
headers**, so a third-party adapter living outside this repository can run
the very same battery:

```cpp
#include "aria/binding/testing/adapter_conformance.hpp"
```

Drive your adapter through it (see
`modules/binding/tests/test_fake_conformance.cpp` and
`aria/testing/list_conformance.hpp`) — it pins:

- round-trip set→get for every typed channel;
- `on_*_changed` fires on native change and stops after the Subscription
  drops;
- View→VM is never marshalled (callbacks already arrive on the UI
  thread; L-4);
- the `warn_unsupported_` path on a mis-bound widget (L-39.2).

## Contract checklist (L-39)

- [ ] `platform_name()` is a **stable lowercase id** equal to
      `IView::kind()` (`"gtk"`), because trace events and log filters
      route on it.
- [ ] Every `set_*` / `get_*` / `on_*_changed` on an unsupported widget
      reports, then returns safely (no-op / default / empty
      Subscription) — never a silent early return. Inheriting
      `ViewAdapterBase` satisfies this for every channel you do not
      override; adapters deriving from `IViewAdapter` directly supply
      their own `warn_unsupported_`.
- [ ] The adapter fires `fire_destroy_()` from the native teardown, not
      only from `~IView` (L-32).
- [ ] No marshalling on the View→VM path (L-4); marshalling policy is the
      engine's job on the VM→View path (`SmartMarshal` / `AlwaysPost`).
- [ ] If the adapter caches wrappers (a `view_for`-style entry point),
      cached views are destroyed **outside** any adapter mutex — view
      teardown fires `on_destroy`, which typically re-enters the
      adapter's own cleanup and would self-deadlock.
