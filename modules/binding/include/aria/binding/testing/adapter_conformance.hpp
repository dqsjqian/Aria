#pragma once

// ============================================================================
//  binding/testing/adapter_conformance.hpp
// ----------------------------------------------------------------------------
//  A shared contract test battery for every `IViewAdapter` implementation.
//
//  Why this exists
//  ---------------
//  Aria's selling point is "one ViewModel, many platforms". That only
//  holds if every adapter behaves **identically** for the small number
//  of operations `BindingEngine` actually exercises. When a new adapter
//  is written (AppKit, UIKit, JNI, WASM, headless-test, ...), the
//  platform author wants a single drop-in file that pins down the
//  contract -- not a fresh translation of the Qt tests into the local
//  idiom.
//
//  How to use it
//  -------------
//  Implement a `Harness` class that knows how to build adapter + view
//  pairs on your platform, and knows how to simulate "a user typed /
//  toggled / clicked this widget". Then in your platform test target:
//
//      TEST_CASE("MyAdapter conforms: text two-way") {
//          MyHarness h;
//          aria::binding::testing::conformance::run_text_two_way(h);
//      }
//      ... (one TEST_CASE per public entry point below)
//
//  Harness concept (duck-typed -- no inheritance required)
//  -------------------------------------------------------
//      struct Harness {
//          // Return an owning adapter. Called once per TEST_CASE.
//          std::shared_ptr<IViewAdapter> make_adapter();
//
//          // Return an owning view (a container the harness manages).
//          // The returned pair MUST be valid together for the lifetime
//          // of the returned handle -- destroying the handle must
//          // destroy the underlying native widget, so view-destroy
//          // semantics can be tested.
//          //
//          // `ViewHandle` is any RAII-holding-a-unique_ptr-or-similar
//          // that exposes `.view()` → `IView&`. Default-construct it
//          // via:
//          //     auto h = harness.make_text_view();
//          //     IView& v = h.view();
//          //
//          // (the concrete type stays a template parameter, so each
//          // platform can return its own RAII wrapper).
//          auto make_text_view();
//          auto make_bool_view();
//          auto make_int_view();
//          auto make_double_view();
//          auto make_click_view();
//
//          // Simulate "a user typed `text` into this widget".
//          void user_type(IView& v, std::string text);
//          // Simulate "a user toggled this checkbox to `b`".
//          void user_toggle(IView& v, bool b);
//          // Simulate "a user set this spinbox/slider to `n`".
//          void user_set_int(IView& v, int n);
//          // Simulate "a user set this double spinbox to `d`".
//          void user_set_double(IView& v, double d);
//          // Simulate "a user clicked this button".
//          void user_click(IView& v);
//      };
//
//  Every `run_*` function below executes doctest `CHECK` macros, so it
//  must be called from *inside* a TEST_CASE; the bundled CHECKs will be
//  reported against that case.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_adapter.hpp"
#include "aria/property.hpp"

#include <memory>
#include <string>
#include <utility>

namespace aria::binding::testing::conformance {

// ---------------------------------------------------------------------------
//  Small helper: construct a BindingEngine whose adapter came from the
//  harness. Every run_* function opens with this and tears it down via
//  normal RAII.
// ---------------------------------------------------------------------------
template<class Harness>
[[nodiscard]] inline BindingEngine make_engine(Harness& h) {
    return BindingEngine(h.make_adapter());
}

// ═══════════════════════════════════════════════════════════════════════
//  Text
// ═══════════════════════════════════════════════════════════════════════

/// VM → View → VM round trip on a text widget.
/// Contract:
///   * set_text() is visible via get_text().
///   * on_text_changed subscribers fire when `user_type` arrives.
///   * Two-way binding: Property::set pushes to the widget, and
///     `user_type` flows back into the Property.
template<class Harness>
inline void run_text_two_way(Harness& h) {
    auto vh = h.make_text_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_text(v, "hello");
    CHECK(adapter->get_text(v) == "hello");

    int hits = 0;
    std::string last;
    auto sub = adapter->on_text_changed(v, [&](std::string_view sv) {
        ++hits;
        last = std::string(sv);
    });

    h.user_type(v, "world");
    CHECK(hits == 1);
    CHECK(last == "world");

    // Releasing the subscription must detach the slot.
    sub.release();
    h.user_type(v, "after-release");
    CHECK(hits == 1);
}

/// Two-way binding via BindingEngine: VM → View and View → VM both work.
template<class Harness>
inline void run_text_engine_two_way(Harness& h) {
    auto engine = make_engine(h);
    Property<std::string> p("alpha");

    auto vh = h.make_text_view();
    IView& v = vh.view();
    engine.bind_text(p, v);

    CHECK(engine.adapter().get_text(v) == "alpha");

    p = "beta";
    CHECK(engine.adapter().get_text(v) == "beta");

    h.user_type(v, "gamma");
    CHECK(p.get() == "gamma");
}

// ═══════════════════════════════════════════════════════════════════════
//  Bool
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_bool_two_way(Harness& h) {
    auto vh = h.make_bool_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_bool(v, true);
    CHECK(adapter->get_bool(v));

    int hits = 0;
    bool last = false;
    auto sub = adapter->on_bool_changed(v, [&](bool b) { ++hits; last = b; });

    h.user_toggle(v, false);
    CHECK(hits == 1);
    CHECK_FALSE(last);
}

template<class Harness>
inline void run_bool_engine_two_way(Harness& h) {
    auto engine = make_engine(h);
    Property<bool> p(false);

    auto vh = h.make_bool_view();
    IView& v = vh.view();
    engine.bind_bool(p, v);

    CHECK_FALSE(engine.adapter().get_bool(v));

    p = true;
    CHECK(engine.adapter().get_bool(v));

    h.user_toggle(v, false);
    CHECK_FALSE(p.get());
}

// ═══════════════════════════════════════════════════════════════════════
//  Int
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_int_two_way(Harness& h) {
    auto vh = h.make_int_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_int(v, 42);
    CHECK(adapter->get_int(v) == 42);

    int last = -1;
    auto sub = adapter->on_int_changed(v, [&](int n) { last = n; });
    h.user_set_int(v, 7);
    CHECK(last == 7);
}

// ═══════════════════════════════════════════════════════════════════════
//  Double
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_double_two_way(Harness& h) {
    auto vh = h.make_double_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_double(v, 3.25);
    CHECK(adapter->get_double(v) == doctest::Approx(3.25));

    double last = -1.0;
    auto sub = adapter->on_double_changed(v, [&](double d) { last = d; });
    h.user_set_double(v, 7.5);
    CHECK(last == doctest::Approx(7.5));
}

// ═══════════════════════════════════════════════════════════════════════
//  Int64 / UInt64 / Float — optional conformance for adapters whose
//  host widgets natively speak wider/narrower numeric types. Adapters
//  that forward int64/uint64/float through int/double (the default
//  strategy for Qt / AppKit / UIKit) can reuse their existing
//  make_int_view / make_double_view and the round-trip just checks the
//  cast path is lossless for in-range values.
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_int64_two_way_via_int_view(Harness& h) {
    auto vh = h.make_int_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_int64(v, 1'000'000);
    CHECK(adapter->get_int64(v) == 1'000'000);
}

template<class Harness>
inline void run_uint64_two_way_via_int_view(Harness& h) {
    auto vh = h.make_int_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_uint64(v, 123'456u);
    CHECK(adapter->get_uint64(v) == 123'456u);
}

template<class Harness>
inline void run_float_two_way_via_double_view(Harness& h) {
    auto vh = h.make_double_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    adapter->set_float(v, 2.5f);
    // Widen explicitly: `Approx` holds a double, so comparing a float
    // against it promotes implicitly, and -Wdouble-promotion flags that
    // from inside doctest's comparison template. See the same note in
    // test_binding_readonly_source.cpp.
    CHECK(static_cast<double>(adapter->get_float(v)) == doctest::Approx(2.5));
}

// ═══════════════════════════════════════════════════════════════════════
//  Click
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_click(Harness& h) {
    auto vh = h.make_click_view();
    IView& v = vh.view();
    auto adapter = h.make_adapter();

    int hits = 0;
    auto sub = adapter->on_click(v, [&]() { ++hits; });

    h.user_click(v);
    h.user_click(v);
    CHECK(hits == 2);

    sub.release();
    h.user_click(v);
    CHECK(hits == 2);  // released subscription must not fire
}

// ═══════════════════════════════════════════════════════════════════════
//  Command + enabled
// ═══════════════════════════════════════════════════════════════════════

template<class Harness>
inline void run_command_enabled(Harness& h) {
    auto engine = make_engine(h);

    int n = 0;
    Property<bool> gate(false);
    Command<> cmd(
        [&]() { ++n; },
        [&]() { return gate.get(); }
    );

    auto vh = h.make_click_view();
    IView& v = vh.view();
    engine.bind_command(cmd, v);

    // Predicate was false at bind time → click is blocked either by
    // enabled=false (BindingEngine will have set it) or by the
    // predicate itself; either way the action must not run.
    h.user_click(v);
    CHECK(n == 0);

    // Flip the gate. Command<>'s internal Effect must push the change
    // to the button's enabled state via the adapter.
    gate = true;
    h.user_click(v);
    CHECK(n == 1);

    gate = false;
    h.user_click(v);
    CHECK(n == 1);
}

// ═══════════════════════════════════════════════════════════════════════
//  View destruction safety
// ═══════════════════════════════════════════════════════════════════════

/// When the native view dies before the BindingEngine, property writes
/// that would otherwise reach the adapter must become no-ops. Every
/// conforming adapter needs to emit the `IView::on_destroy` signal in
/// such a way that `BindingEngine` drops the per-view subscription
/// bucket before the view's storage is reclaimed.
template<class Harness>
inline void run_view_destroy_safety(Harness& h) {
    auto engine = make_engine(h);

    Property<std::string> survivor("alive");
    auto survivor_vh = h.make_text_view();
    engine.bind_text(survivor, survivor_vh.view());

    Property<std::string> dying("x");
    {
        auto dying_vh = h.make_text_view();
        engine.bind_text(dying, dying_vh.view());
        CHECK(engine.adapter().get_text(dying_vh.view()) == "x");

        dying = "y";
        CHECK(engine.adapter().get_text(dying_vh.view()) == "y");
        // dying_vh goes out of scope; the IView must fire on_destroy.
    }

    // Writing to `dying` after the view is gone MUST NOT crash.
    // (ASan/UBSan will flag a regression.)
    dying = "after-death";

    // Unaffected view keeps working.
    survivor = "still here";
    CHECK(engine.adapter().get_text(survivor_vh.view()) == "still here");
}

// ═══════════════════════════════════════════════════════════════════════
//  Convenience: run the entire battery with a single call.
//
//  Most platform authors want this; they can hand-pick the above if a
//  particular widget flavour is not supported natively (e.g. a console
//  adapter has no double input).
// ═══════════════════════════════════════════════════════════════════════
template<class Harness>
inline void run_all(Harness& h) {
    run_text_two_way(h);
    run_text_engine_two_way(h);
    run_bool_two_way(h);
    run_bool_engine_two_way(h);
    run_int_two_way(h);
    run_double_two_way(h);
    run_click(h);
    run_command_enabled(h);
    run_view_destroy_safety(h);
}

}  // namespace aria::binding::testing::conformance
