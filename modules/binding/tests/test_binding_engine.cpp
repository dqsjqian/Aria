#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/binding/converter.hpp"
#include "fake_adapter.hpp"

using namespace aria;
using namespace aria::binding;
using namespace aria::binding::testing;

TEST_CASE("BindingEngine: text two-way") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> name("Alice");
    FakeView view;
    engine.bind_text(name, view);

    CHECK(view.text == "Alice");

    // Model -> View
    name = "Bob";
    CHECK(view.text == "Bob");

    // View -> Model
    FakeAdapter::user_type(view, "Charlie");
    CHECK(name.get() == "Charlie");
}

TEST_CASE("BindingEngine: bool two-way") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<bool> on(false);
    FakeView view;
    engine.bind_bool(on, view);

    CHECK_FALSE(view.flag);
    on = true;
    CHECK(view.flag);
    FakeAdapter::user_toggle(view, false);
    CHECK_FALSE(on.get());
}

TEST_CASE("BindingEngine: command click + can_execute (auto-tracked)") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int n = 0;
    Property<bool> enabled(false);
    Command<> cmd(
        [&]() { ++n; },
        [&]() { return enabled.get(); }
    );

    FakeView btn;
    engine.bind_command(cmd, btn);

    // Initial snapshot: predicate returned false, so bind_command disabled us.
    CHECK_FALSE(btn.enabled);

    FakeAdapter::user_click(btn);
    CHECK(n == 0);  // blocked by predicate

    // No manual notify_can_execute_changed() here — Command<>'s internal
    // Effect re-evaluates the predicate when `enabled` changes and pushes
    // the new truth value through the binding.
    enabled = true;
    CHECK(btn.enabled);

    FakeAdapter::user_click(btn);
    CHECK(n == 1);

    enabled = false;
    CHECK_FALSE(btn.enabled);
}

// ── B11 regression: bind_command<Args...> must drive the view's
// `enabled` from `cmd.can_execute(args...)` — NOT from whatever bool
// payload the publisher chose to emit. The pre-fix implementation
// just blindly forwarded the signal payload, so a publisher that
// called `notify_can_execute_changed(other_args)` could enable a
// button bound with a different `args` set, even when the predicate
// said it should be disabled.
TEST_CASE("BindingEngine: bind_command(args) recomputes can_execute on its own args (B11)") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int last_arg = -1;
    Command<int> cmd(
        [&](const int& x) { last_arg = x; },
        // can_execute only true for x == 7 — no Property reads, so
        // auto-tracking won't help; we MUST go through the predicate
        // recomputation that the binding does on every signal pulse.
        [](const int& x) { return x == 7; }
    );

    FakeView btn;
    engine.bind_command(cmd, btn, 5);   // bound to args=5 → can_execute(5) is false
    CHECK_FALSE(btn.enabled);

    // Publisher fires a "can_execute_changed" with payload == TRUE
    // computed for a different argument (8). The buggy code would
    // surface that `true` straight to set_enabled, even though
    // `can_execute(5)` is still false.
    cmd.notify_can_execute_changed(8);
    // After the fix, the binding ignores the payload and re-evaluates
    // `cmd.can_execute(args=5)` — which is still false.
    CHECK_FALSE(btn.enabled);

    // Now actually flip via an args=7 notification. Predicate(5) is
    // still false (we're bound to 5, not 7), so the view stays
    // disabled. This pins down "bind_command tracks ITS args, not the
    // payload's args".
    cmd.notify_can_execute_changed(7);
    CHECK_FALSE(btn.enabled);

    (void)last_arg;
}

TEST_CASE("BindingEngine: two-way binding suppresses converter feedback loop") {
    // Reproduce the classic two-way round-trip: Property<double> bound
    // via a LOSSY converter ("%.2f" truncates precision). Without the
    // reentrancy guard, the VM→View write would make the view emit a
    // changed event with the reformatted text, which `to_model` would
    // parse back into a *different* double, re-triggering the property
    // and causing a visible round-off snap (1.004 → 1.00 → 1.0).
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<double> value(1.0);

    // Count VM→Model writes so we can assert the feedback loop was cut.
    int model_writes = 0;
    auto count_sub = value.on_changed([&](double) { ++model_writes; });

    Converter<double, std::string> conv = converters::double_to_string(2);

    FakeView view;
    engine.bind_text_converted(value, view, conv);

    // Initial sync: the view sees "1.00", and crucially the model is
    // NOT snapped by a view-echo round trip.
    CHECK(view.text == "1.00");
    CHECK(model_writes == 0);
    CHECK(value.get() == doctest::Approx(1.0));

    // Programmatic VM update with sub-precision digits. Without the
    // guard, the view echo "1.23" would parse back to 1.23 and snap
    // the property away from 1.234.
    value = 1.234;
    CHECK(view.text == "1.23");
    CHECK(model_writes == 1);                           // just our own write
    CHECK(value.get() == doctest::Approx(1.234));       // NOT snapped to 1.23

    // Genuine user edit still flows back to the model verbatim.
    FakeAdapter::user_type(view, "3.75");
    CHECK(value.get() == doctest::Approx(3.75));
    CHECK(model_writes == 2);
}

TEST_CASE("BindingEngine: visible/enabled bindings") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<bool> visible(true), enabled(true);
    FakeView v;
    engine.bind_visible(visible, v);
    engine.bind_enabled(enabled, v);

    CHECK(v.visible);
    CHECK(v.enabled);

    visible = false;
    enabled = false;
    CHECK_FALSE(v.visible);
    CHECK_FALSE(v.enabled);
}

TEST_CASE("BindingEngine: view destruction releases its bindings without UB") {
    // Verify that when an IView dies before the engine, the bindings
    // wired to that view are released automatically — subsequent
    // property writes must not call into the dead view.
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> alive_prop("alive");
    Property<std::string> dying_prop("dying");

    FakeView alive_view;
    engine.bind_text(alive_prop, alive_view);
    CHECK(alive_view.text == "alive");

    {
        // dying_view is scoped — it will be destroyed while `engine` and
        // `dying_prop` are still alive.
        FakeView dying_view;
        engine.bind_text(dying_prop, dying_view);
        CHECK(dying_view.text == "dying");
        dying_prop = "still here";
        CHECK(dying_view.text == "still here");
        // dying_view goes out of scope here → IView::~IView fires the
        // destroy signal → engine drops every binding tied to it.
    }

    // If the binding were still live, this assignment would try to call
    // adapter->set_text on freed memory (ASan/UBSan would flame out).
    // With the on_destroy hookup the write is simply a no-op for the
    // view side.
    dying_prop = "after death";

    // The other view is unaffected.
    alive_prop = "still bound";
    CHECK(alive_view.text == "still bound");
}

TEST_CASE("BindingEngine: engine destroyed before view, view's later destroy is safe") {
    // Mirror of the previous test — this time the engine dies first.
    // The bindings must be dropped when the engine is destroyed, and
    // the later destruction of `view` (which emits IView::on_destroy)
    // must NOT dereference the dead engine.
    auto adapter = std::make_shared<FakeAdapter>();

    FakeView view;

    {
        BindingEngine engine(adapter);
        Property<std::string> prop("hello");
        engine.bind_text(prop, view);
        CHECK(view.text == "hello");
        prop = "world";
        CHECK(view.text == "world");
        // `engine` goes out of scope here. It must disconnect its
        // on_destroy subscription on the view *before* the view's
        // destroy signal can ever reach the freed engine.
    }

    // The view must still be a healthy object we can write to through
    // the adapter directly — its destroy signal has not fired yet.
    FakeAdapter::user_type(view, "independent edit");
    CHECK(view.text == "independent edit");

    // When `view` goes out of scope at the end of the TEST_CASE, its
    // IView::~IView will fire `destroy_signal_`. No slot should still
    // be connected, so this is a plain no-op; if the engine's subscription
    // had outlived the engine, we'd get use-after-free here (ASan/UBSan
    // would flag it).
}

TEST_CASE("BindingEngine: clear() releases bindings but leaves views usable") {
    // Explicit lifecycle: engine.clear() must drop every binding the same
    // way the destructor would. After that, writing to the Property must
    // not touch the view, and the view must survive a subsequent
    // destruction without reaching into the engine.
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> prop("x");
    {
        FakeView view;
        engine.bind_text(prop, view);
        CHECK(view.text == "x");

        engine.clear();

        // Binding is gone: property writes no longer reach the view.
        prop = "after-clear";
        CHECK(view.text == "x");   // unchanged

        // User can still drive the view through the adapter directly —
        // only our binding was dropped.
        FakeAdapter::user_type(view, "user-edit");
        CHECK(view.text == "user-edit");
        CHECK(prop.get() == "after-clear");  // binding gone both ways

        // view goes out of scope here; engine.clear() already removed
        // its on_destroy subscription, so the signal fires into the void.
    }

    // The engine is still healthy — we can bind a fresh view to it.
    FakeView fresh;
    engine.bind_text(prop, fresh);
    CHECK(fresh.text == "after-clear");
    prop = "reused";
    CHECK(fresh.text == "reused");
}

// ═══════════════════════════════════════════════════════════════════════
//  Lifecycle edge-case battery
//
//  The tests below pin down boundary teardown orders that would otherwise
//  have to be rediscovered by users (often via a crash). They are the
//  first line of defence for every future change to BindingEngine,
//  IView::on_destroy and Command<>'s eager-tracking Effect.
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("BindingEngine: two views share one Command; one dies, the other keeps working") {
    // Regression: when a single Command<> is bound to multiple views, the
    // destruction of ONE view must not sever the command's subscription
    // used by the OTHER view. Each bind_command() path gets its own
    // `observe_can_execute` slot, and only the dying view's slot is
    // released by IView::on_destroy.
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int n = 0;
    Property<bool> enabled(true);
    Command<> cmd(
        [&]() { ++n; },
        [&]() { return enabled.get(); }
    );

    FakeView surviving;
    engine.bind_command(cmd, surviving);
    CHECK(surviving.enabled);

    {
        FakeView dying;
        engine.bind_command(cmd, dying);
        CHECK(dying.enabled);

        enabled = false;
        CHECK_FALSE(surviving.enabled);
        CHECK_FALSE(dying.enabled);

        enabled = true;
        CHECK(surviving.enabled);
        CHECK(dying.enabled);
        // `dying` goes out of scope here.
    }

    // The surviving view must still track the command: flipping
    // `enabled` has to continue driving it.
    enabled = false;
    CHECK_FALSE(surviving.enabled);
    enabled = true;
    CHECK(surviving.enabled);

    FakeAdapter::user_click(surviving);
    CHECK(n == 1);
}

TEST_CASE("BindingEngine: property outliving the engine is safe to keep mutating") {
    // The inverse of "view outlives engine": here the Property (i.e. the
    // model side) outlives the engine. The engine's on_changed
    // subscription has to be dropped on ~BindingEngine so that later
    // writes to the property do not call into freed bucket storage.
    auto adapter = std::make_shared<FakeAdapter>();
    Property<std::string> prop("hello");
    FakeView view;

    {
        BindingEngine engine(adapter);
        engine.bind_text(prop, view);
        CHECK(view.text == "hello");
        prop = "world";
        CHECK(view.text == "world");
        // engine goes out of scope — must disconnect from `prop`.
    }

    // View is decoupled: model-side writes no longer reach it.
    prop = "after-engine";
    CHECK(view.text == "world");

    // Property itself is still healthy — further writes and observers
    // continue to work independently of the now-gone binding.
    int obs_calls = 0;
    auto sub = prop.on_changed([&](const std::string&) { ++obs_calls; });
    prop = "standalone";
    CHECK(obs_calls == 1);
    CHECK(prop.get() == "standalone");
}

TEST_CASE("BindingEngine: Command<> outliving the engine is safe") {
    // Companion to the Property version: make sure a Command<> whose
    // internal Effect keeps running after the engine dies does not
    // reach into freed engine state when its can_execute flips.
    auto adapter = std::make_shared<FakeAdapter>();

    Property<bool> enabled(true);
    Command<> cmd(
        []() {},
        [&]() { return enabled.get(); }
    );

    FakeView btn;
    {
        BindingEngine engine(adapter);
        engine.bind_command(cmd, btn);
        CHECK(btn.enabled);

        enabled = false;
        CHECK_FALSE(btn.enabled);
        // engine goes out of scope; `btn.enabled` stays at its last set value.
    }

    // Command's Effect keeps firing; but its can_execute subscription to
    // the engine is gone, so `btn` no longer tracks. If we were still
    // somehow wired, the next flip would double-write btn.enabled.
    bool btn_snapshot = btn.enabled;
    enabled = true;
    CHECK(btn.enabled == btn_snapshot);     // untouched
    enabled = false;                         // force another flip
    CHECK(btn.enabled == btn_snapshot);
}

TEST_CASE("BindingEngine: re-binding after view destroy reuses a clean slate") {
    // After a view is destroyed the engine's per-view map entry must be
    // erased (not just its subscription bucket cleared), otherwise
    // binding a *new* FakeView that happens to reuse the same address
    // would hit a stale bucket and behave incorrectly.
    //
    // We cannot directly inspect `per_view_` from the outside, so we
    // exercise the observable consequence: repeatedly creating a view
    // at the same stack slot, binding it, letting it die, and binding
    // a new one. Each cycle must behave as if the engine had never
    // seen that address before.
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> prop("v0");

    for (int i = 0; i < 3; ++i) {
        FakeView v;
        engine.bind_text(prop, v);
        CHECK(v.text == prop.get());

        prop = std::string("v") + std::to_string(i + 1);
        CHECK(v.text == prop.get());
        // v destroyed here → engine drops its bucket.
    }

    // After the loop, `prop` has been mutated 3 times and every bound
    // view saw the write at its own scope. A fresh view now sees the
    // final value as its initial sync, and future writes reach it.
    FakeView final_view;
    engine.bind_text(prop, final_view);
    CHECK(final_view.text == "v3");
    prop = "final";
    CHECK(final_view.text == "final");
}

TEST_CASE("BindingEngine: binding the same view twice stays consistent when it dies") {
    // Two bindings on the same view → two subscriptions in the same
    // per-view bucket. View destruction has to release BOTH; a partial
    // release would leak a subscription that eventually dereferences
    // a dead view.
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> text("t");
    Property<bool>        enabled(true);

    {
        FakeView v;
        engine.bind_text(text, v);
        engine.bind_enabled(enabled, v);
        CHECK(v.text == "t");
        CHECK(v.enabled);

        text = "t2";
        enabled = false;
        CHECK(v.text == "t2");
        CHECK_FALSE(v.enabled);
        // v dies here → both subscriptions must be dropped.
    }

    // Writes after the view is gone must not crash. No observable
    // side effect to assert — ASan/UBSan would flag a regression.
    text = "after";
    enabled = true;
    CHECK(text.get() == "after");
    CHECK(enabled.get());
}

// ----------------------------------------------------------------------------
//  Converter failure semantics (Sprint4-#1)
//
//  Built-in converters MUST NOT silently coerce invalid input into 0/0.0.
//  The new contract:
//    * try_to_model returns nullopt on bad input.
//    * to_model throws ConversionError on bad input.
//    * binding_engine, when wired through bind_text_converted, prefers
//      try_to_model and on nullopt drops the View → Model write while
//      reporting via the unified callback-boundary sink.
// ----------------------------------------------------------------------------

TEST_CASE("Converter int_to_string: try_to_model returns nullopt on bad input") {
    auto c = converters::int_to_string();
    REQUIRE(c.try_to_model);
    CHECK(c.try_to_model("42").value() == 42);
    CHECK(c.try_to_model("-7").value() == -7);
    CHECK_FALSE(c.try_to_model("abc").has_value());
    CHECK_FALSE(c.try_to_model("12abc").has_value());  // trailing garbage
    CHECK_FALSE(c.try_to_model("").has_value());
}

TEST_CASE("Converter int_to_string: to_model throws ConversionError on bad input") {
    auto c = converters::int_to_string();
    CHECK(c.to_model("42") == 42);
    CHECK_THROWS_AS(c.to_model("abc"),   ConversionError);
    CHECK_THROWS_AS(c.to_model("12abc"), ConversionError);
    CHECK_THROWS_AS(c.to_model(""),      ConversionError);
}

TEST_CASE("Converter double_to_string: try_to_model returns nullopt on bad input") {
    auto c = converters::double_to_string();
    REQUIRE(c.try_to_model);
    CHECK(c.try_to_model("3.14").value() == doctest::Approx(3.14));
    CHECK_FALSE(c.try_to_model("xyz").has_value());
    CHECK_FALSE(c.try_to_model("3.14xyz").has_value());
}

TEST_CASE("Converter double_to_string: to_model throws ConversionError on bad input") {
    auto c = converters::double_to_string();
    CHECK(c.to_model("3.14") == doctest::Approx(3.14));
    CHECK_THROWS_AS(c.to_model("xyz"),     ConversionError);
    CHECK_THROWS_AS(c.to_model("3.14xyz"), ConversionError);
    CHECK_THROWS_AS(c.to_model(""),        ConversionError);
}

TEST_CASE("Converter bool_to_yes_no: try_to_model recognises canonical truthy/falsy") {
    auto c = converters::bool_to_yes_no();
    REQUIRE(c.try_to_model);
    CHECK(c.try_to_model("yes").value()   == true);
    CHECK(c.try_to_model("true").value()  == true);
    CHECK(c.try_to_model("1").value()     == true);
    CHECK(c.try_to_model("no").value()    == false);
    CHECK(c.try_to_model("false").value() == false);
    CHECK(c.try_to_model("0").value()     == false);
    CHECK_FALSE(c.try_to_model("maybe").has_value());
    CHECK_FALSE(c.try_to_model("").has_value());
}

TEST_CASE("BindingEngine: bad converter input drops the Model write (no silent zero)") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    // Capture binding.converter failures via the unified sink.
    struct Capture { int rejected = 0; std::string last_msg; };
    static Capture cap{};
    cap = Capture{};

    auto previous_sink = aria::set_callback_failure_sink(
        [](const aria::CallbackFailure& f) {
            if (f.category == std::string_view{"binding.converter"}) {
                ++cap.rejected;
                cap.last_msg = std::string{f.message};
            }
        });

    Property<int> n(7);
    FakeView      v;
    engine.bind_text_converted(n, v, converters::int_to_string());

    // Initial sync.
    CHECK(v.text == "7");
    CHECK(n.get() == 7);

    // Legitimate user edit still flows.
    FakeAdapter::user_type(v, "42");
    CHECK(n.get() == 42);
    CHECK(cap.rejected == 0);

    // Bad input: Model retains its previous value, sink observes one
    // rejection. The crucial property: n.get() is NOT silently 0.
    FakeAdapter::user_type(v, "abc");
    CHECK(n.get() == 42);                         // unchanged
    CHECK(cap.rejected == 1);
    CHECK_FALSE(cap.last_msg.empty());

    // More bad input — every rejection accounted for.
    FakeAdapter::user_type(v, "12abc");
    CHECK(n.get() == 42);
    CHECK(cap.rejected == 2);

    aria::set_callback_failure_sink(previous_sink);
}

// ── P1-H: view-destroy lifetime hook (async-cancellation primitive) ────────

TEST_CASE("BindingEngine::bind_view_lifetime fires on view destroy") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int cancelled = 0;
    {
        FakeView view;
        engine.bind_view_lifetime(view, [&cancelled] { ++cancelled; });
        CHECK(cancelled == 0);
        // Leaving this scope destroys `view` → IView::on_destroy fans out
        // → the engine clears the view's bucket → our lifetime callback
        // (a Subscription deleter) fires exactly once.
    }
    CHECK(cancelled == 1);
}

TEST_CASE("BindingEngine::bind_view_lifetime fires on engine clear") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int cancelled = 0;
    FakeView view;  // outlives the engine clear()
    engine.bind_view_lifetime(view, [&cancelled] { ++cancelled; });
    CHECK(cancelled == 0);

    engine.clear();          // engine teardown must also fire the hook
    CHECK(cancelled == 1);

    // View still alive; destroying it now must NOT double-fire.
}

TEST_CASE("BindingEngine::bind_view_lifetime fires once, not per binding") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    int cancelled = 0;
    {
        FakeView view;
        Property<std::string> name("x");
        engine.bind_text(name, view);                       // unrelated binding
        engine.bind_view_lifetime(view, [&cancelled] { ++cancelled; });
    }
    CHECK(cancelled == 1);
}

TEST_CASE("BindingEngine::bind_view_lifetime ignores empty callback") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);
    FakeView view;
    // Must be a no-op, never throw.
    engine.bind_view_lifetime(view, {});
    CHECK(true);
}

// ── Projected one-way text bindings ─────────────────────────────────
//  bind_text_projected / bind_optional_text are async-agnostic helpers
//  for read-only labels (the common shape when rendering an
//  AsyncCommand's last_error_message / last_result, or any Computed).

TEST_CASE("BindingEngine::bind_text_projected syncs initial + on change") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> count(3);
    FakeView label;
    engine.bind_text_projected(count, label,
        [](int n) { return "count=" + std::to_string(n); });

    // Initial sync.
    CHECK(label.text == "count=3");

    // VM → View on change.
    count = 7;
    CHECK(label.text == "count=7");
}

TEST_CASE("BindingEngine::bind_text_projected is one-way (view edits ignored)") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> count(1);
    FakeView label;
    engine.bind_text_projected(count, label, [](int n) { return std::to_string(n); });
    CHECK(label.text == "1");

    // A stray view-side text change must NOT feed back into the model.
    FakeAdapter::user_type(label, "999");
    CHECK(count.get() == 1);
}

TEST_CASE("BindingEngine::bind_optional_text renders value vs empty_text") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::optional<std::string>> result(std::nullopt);
    FakeView label;
    engine.bind_optional_text(result, label,
        [](const std::string& s) { return "welcome " + s; },
        "(none)");

    // nullopt → empty_text.
    CHECK(label.text == "(none)");

    // some → projected.
    result = std::optional<std::string>{"Alice"};
    CHECK(label.text == "welcome Alice");

    // back to nullopt → empty_text again.
    result = std::nullopt;
    CHECK(label.text == "(none)");
}

TEST_CASE("BindingEngine::bind_optional_text default empty_text is empty string") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::optional<int>> maybe(std::nullopt);
    FakeView label;
    engine.bind_optional_text(maybe, label,
        [](int n) { return std::to_string(n); });
    CHECK(label.text.empty());

    maybe = std::optional<int>{42};
    CHECK(label.text == "42");
}

TEST_CASE("BindingEngine: projected bindings survive view destroy without UB") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int>                    n(0);
    Property<std::optional<int>>     opt(std::nullopt);
    {
        FakeView label_a;
        FakeView label_b;
        engine.bind_text_projected(n, label_a, [](int v) { return std::to_string(v); });
        engine.bind_optional_text(opt, label_b, [](int v) { return std::to_string(v); });
        n   = 5;
        opt = std::optional<int>{9};
        CHECK(label_a.text == "5");
        CHECK(label_b.text == "9");
        // labels go out of scope → IView::on_destroy releases bindings.
    }
    // Writes after the views died must be safe no-ops (ASan/UBSan guard).
    n   = 123;
    opt = std::optional<int>{456};
    CHECK(true);
}
