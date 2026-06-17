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

Implement the typed get/set/observe triples. The canonical fast paths are
`text` / `bool` / `int` / `double`; the wider numeric variants
(`int64` / `uint64` / `float`) may delegate to a narrower one with a
range check (see `FakeAdapter` for the default pattern).

```cpp
class GtkAdapter : public IViewAdapter {
public:
    // L-39.1: a stable lowercase id that EXACTLY matches IView::kind().
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

    // ... set_bool/get_bool/on_bool_changed, set_int/..., set_double/...,
    //     set_visible, set_enabled, on_click, and the wider numerics.

private:
    // L-39.2: a mis-bound widget MUST warn (not silently no-op), so a
    // developer who binds the wrong control sees it in the logs.
    GtkView* as_gtk_(IView& v, const char* op) {
        if (v.kind() != "gtk") { warn_unsupported_(op, v); return nullptr; }
        return static_cast<GtkView*>(&v);
    }
    void warn_unsupported_(const char* op, IView& v) {
        aria::runtime::Logger::warn("gtk_adapter",
            std::string{op} + ": no binding path for widget class '"
            + std::string{v.kind()} + "'");
    }
};
```

## Step 3 — verify against the conformance suite

Aria ships an adapter conformance suite so a new adapter is held to the
same contract as the shipped ones. Drive your adapter through it (see
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
      calls `warn_unsupported_` then returns safely (no-op / default /
      empty Subscription) — never a silent early return.
- [ ] The adapter fires `fire_destroy_()` from the native teardown, not
      only from `~IView` (L-32).
- [ ] No marshalling on the View→VM path (L-4); marshalling policy is the
      engine's job on the VM→View path (`SmartMarshal` / `AlwaysPost`).
