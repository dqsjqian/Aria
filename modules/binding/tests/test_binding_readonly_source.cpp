// ============================================================================
//  test_binding_readonly_source.cpp
// ----------------------------------------------------------------------------
//  One-way bindings against a read-only reactive source (`Computed<T>`).
//
//  Before this, `BindingEngine` took `Property<T>&` everywhere, so a derived
//  value could not be bound at all — every computed display value fell back
//  to a hand-written `on_changed` plus a caller-owned subscription store.
//  The binders now constrain on `aria::ReadOnlyReactiveOf<T>`, which both
//  `Property<T>` and `Computed<T>` satisfy.
//
//  Two-way binders deliberately still take `Property<T>&`: a computed value
//  has no write-back path, so `bind_text(computed, view)` must stay a
//  compile error. That half is asserted statically at the bottom.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/computed.hpp"
#include "aria/concepts.hpp"
#include "fake_adapter.hpp"

#include <optional>
#include <string>

using namespace aria;
using namespace aria::binding;
using namespace aria::binding::testing;

// ── Concept sanity ─────────────────────────────────────────────────────────

static_assert(ReadOnlyReactiveOf<Property<std::string>, std::string>);
static_assert(ReadOnlyReactiveOf<Computed<std::string>, std::string>);
static_assert(ReadOnlyReactiveOf<Property<double>, double>);
static_assert(ReadOnlyReactiveOf<Computed<double>, double>);

// Wrong value_type must NOT satisfy the pinned form — that is what keeps
// `bind_visible(computed_of_int, view)` from silently compiling.
static_assert(!ReadOnlyReactiveOf<Computed<int>, bool>);

// A plain value is not a reactive source at all.
static_assert(!ReadOnlyReactive<int>);
static_assert(!ReadOnlyReactive<std::string>);

// Optional-shaped sources, for bind_optional_text.
static_assert(ReadOnlyReactiveOptional<Property<std::optional<int>>>);
static_assert(ReadOnlyReactiveOptional<Computed<std::optional<int>>>);
static_assert(!ReadOnlyReactiveOptional<Computed<int>>);

// ── Scalar one-way binders ─────────────────────────────────────────────────

TEST_CASE("bind_text_oneway accepts a Computed<std::string>") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> first("Ada");
    Property<std::string> last("Lovelace");
    Computed<std::string> full([&] { return first.get() + " " + last.get(); });

    FakeView label;
    engine.bind_text_oneway(full, label);

    CHECK(label.text == "Ada Lovelace");

    // Upstream write → Computed recomputes → view follows. No hand-written
    // on_changed, no caller-owned subscription.
    last = "Byron";
    CHECK(label.text == "Ada Byron");

    first = "Augusta";
    CHECK(label.text == "Augusta Byron");
}

TEST_CASE("bind_double_oneway / bind_int_oneway accept a Computed") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<double> bill(100.0);
    Property<int>    pct(10);
    Computed<double> tip([&] { return bill.get() * pct.get() / 100.0; });
    Computed<int>    doubled([&] { return pct.get() * 2; });

    FakeView tip_view;
    FakeView pct_view;
    engine.bind_double_oneway(tip, tip_view);
    engine.bind_int_oneway(doubled, pct_view);

    CHECK(tip_view.number == doctest::Approx(10.0));
    CHECK(pct_view.integer == 20);

    pct = 25;
    CHECK(tip_view.number == doctest::Approx(25.0));
    CHECK(pct_view.integer == 50);

    bill = 200.0;
    CHECK(tip_view.number == doctest::Approx(50.0));
}

TEST_CASE("bind_visible / bind_enabled accept a Computed<bool>") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> name("");
    Property<bool>        busy(false);
    // The classic "submit is enabled when the form is filled and idle"
    // composition — previously this had to be hand-wired in the view.
    Computed<bool> can_submit([&] { return !name.get().empty() && !busy.get(); });
    Computed<bool> show_spinner([&] { return busy.get(); });

    FakeView button;
    FakeView spinner;
    engine.bind_enabled(can_submit, button);
    engine.bind_visible(show_spinner, spinner);

    CHECK_FALSE(button.enabled);
    CHECK_FALSE(spinner.visible);

    name = "Ada";
    CHECK(button.enabled);

    busy = true;
    CHECK_FALSE(button.enabled);
    CHECK(spinner.visible);

    busy = false;
    CHECK(button.enabled);
    CHECK_FALSE(spinner.visible);
}

TEST_CASE("bind_bool_oneway / bind_int64_oneway / bind_uint64_oneway / "
          "bind_float_oneway accept a Computed") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(1);
    Computed<bool>          odd  ([&] { return n.get() % 2 != 0; });
    Computed<std::int64_t>  wide ([&] { return static_cast<std::int64_t>(n.get()) * 1000; });
    Computed<std::uint64_t> uwide([&] { return static_cast<std::uint64_t>(n.get()) * 7u; });
    Computed<float>         half ([&] { return static_cast<float>(n.get()) / 2.0f; });

    FakeView v_bool, v_i64, v_u64, v_f32;
    engine.bind_bool_oneway(odd, v_bool);
    engine.bind_int64_oneway(wide, v_i64);
    engine.bind_uint64_oneway(uwide, v_u64);
    engine.bind_float_oneway(half, v_f32);

    CHECK(v_bool.flag);
    CHECK(v_i64.i64 == 1000);
    CHECK(v_u64.u64 == 7u);
    CHECK(v_f32.f32 == doctest::Approx(0.5f));

    n = 4;
    CHECK_FALSE(v_bool.flag);
    CHECK(v_i64.i64 == 4000);
    CHECK(v_u64.u64 == 28u);
    CHECK(v_f32.f32 == doctest::Approx(2.0f));
}

// ── Projected / converted / optional ───────────────────────────────────────

TEST_CASE("bind_text_projected accepts a Computed") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<double> bill(20.0);
    Property<int>    people(2);
    Computed<double> per_person([&] { return bill.get() / people.get(); });

    FakeView label;
    // This is the exact shape the Qt tip-calculator demo used to hand-roll:
    // a Computed<double> rendered into a formatted label.
    engine.bind_text_projected(per_person, label,
        [](double v) { return "per person: " + std::to_string(static_cast<int>(v)); });

    CHECK(label.text == "per person: 10");

    people = 4;
    CHECK(label.text == "per person: 5");
}

TEST_CASE("bind_text_converted_oneway accepts a Computed") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(3);
    Computed<int> squared([&] { return n.get() * n.get(); });

    FakeView label;
    engine.bind_text_converted_oneway(squared, label,
        Converter<int, std::string>{
            .to_view      = [](const int& v) { return std::to_string(v); },
            .to_model     = nullptr,
            .try_to_model = nullptr,
        });

    CHECK(label.text == "9");
    n = 5;
    CHECK(label.text == "25");
}

TEST_CASE("bind_optional_text accepts a Computed<std::optional<T>>") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(0);
    Computed<std::optional<std::string>> maybe_name([&] {
        return n.get() > 0 ? std::optional<std::string>{"item " + std::to_string(n.get())}
                           : std::nullopt;
    });

    FakeView label;
    engine.bind_optional_text(maybe_name, label,
        [](const std::string& s) { return s; },
        "(nothing)");

    CHECK(label.text == "(nothing)");

    n = 2;
    CHECK(label.text == "item 2");

    n = 0;
    CHECK(label.text == "(nothing)");
}

// ── Lifetime: the Computed path uses the same per-view bucket ──────────────

TEST_CASE("Computed binding is released when the view dies first") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(1);
    Computed<int> doubled([&] { return n.get() * 2; });

    {
        FakeView label;
        engine.bind_int_oneway(doubled, label);
        CHECK(label.integer == 2);
        n = 3;
        CHECK(label.integer == 6);
        // label leaves scope → IView::on_destroy → bucket cleared.
    }

    // Writing through the Computed after the view died must be a safe
    // no-op, not a dangling adapter call (ASan/UBSan guard).
    n = 7;
    CHECK(doubled.get() == 14);
}

TEST_CASE("Computed binding is released by engine clear()") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(1);
    Computed<int> doubled([&] { return n.get() * 2; });
    FakeView label;

    engine.bind_int_oneway(doubled, label);
    CHECK(label.integer == 2);

    engine.clear();
    n = 10;
    CHECK(label.integer == 2);   // no further view writes
}

// ── Property<T> still selects the non-template overload ───────────────────
//
// The one-way binders now have a `Property<T>&` overload AND a constrained
// template. Passing a Property must keep resolving to the former (no
// behavioural change, ABI-stable symbol still used), which is what these
// two cases exercise end-to-end.

TEST_CASE("Property still binds one-way exactly as before") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> s("x");
    Property<bool>        vis(false);
    FakeView label, panel;

    engine.bind_text_oneway(s, label);
    engine.bind_visible(vis, panel);

    CHECK(label.text == "x");
    CHECK_FALSE(panel.visible);

    s   = "y";
    vis = true;
    CHECK(label.text == "y");
    CHECK(panel.visible);
}

// ── Two-way against a Computed must NOT compile ────────────────────────────
//
// A derived value has no write-back path. We assert the *absence* of a
// viable two-way overload rather than relying on a comment, so a future
// refactor that accidentally widens `bind_text` / `bind_double` to
// ReadOnlyReactive fails here instead of silently dropping user edits.

namespace {

template<typename Engine, typename Src, typename View>
concept TwoWayTextBindable = requires(Engine& e, Src& s, View& v) {
    e.bind_text(s, v);
};

template<typename Engine, typename Src, typename View>
concept TwoWayDoubleBindable = requires(Engine& e, Src& s, View& v) {
    e.bind_double(s, v);
};

}  // namespace

static_assert(TwoWayTextBindable<BindingEngine, Property<std::string>, FakeView>,
    "bind_text must still accept Property<std::string> — regression guard.");
static_assert(!TwoWayTextBindable<BindingEngine, Computed<std::string>, FakeView>,
    "bind_text must reject Computed: a derived value has no write-back path.");

static_assert(TwoWayDoubleBindable<BindingEngine, Property<double>, FakeView>);
static_assert(!TwoWayDoubleBindable<BindingEngine, Computed<double>, FakeView>);
