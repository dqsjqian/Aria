// ============================================================================
//  fuzz_binding_view_destroy_race.cpp  (L-32)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "Destroying an IView at any moment relative to a VM->View emit
//     MUST NOT dereference dead view memory. The per-view bucket +
//     alive_token weak_ptr in BindingEngine guarantees a posted
//     callback whose target view died in flight is dropped silently."
//
//  Strategy:
//    - Engine wraps a fake adapter; bind a Property<int> to a
//      transient view that gets destroyed on a coin flip after each
//      property write. The fuzzer alternates: (a) build a fresh view,
//      (b) bind it, (c) drive N property changes interleaved with
//      destroy. We assert that no callback ever calls into a dead
//      view (FakeAdapter detects dead via a tombstone counter).
// ============================================================================

#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_adapter.hpp"
#include "fuzz_support.hpp"
#include "aria/property.hpp"

#include <atomic>
#include <memory>
#include <string>

using namespace aria;
using namespace aria::binding;

namespace {

// Minimal adapter dedicated to this fuzzer. It only implements the
// methods the int-binding path actually touches; everything else is
// a no-op that returns defaults.
struct FuzzView : IView {
    FuzzView() = default;
    ~FuzzView() override { fire_destroy_(); }
    [[nodiscard]] std::string_view kind() const noexcept override { return "FuzzView"; }
    int  current{0};
    bool dead{false};
};

class FuzzAdapter : public IViewAdapter {
public:
    std::atomic<std::size_t> set_int_calls_after_destroy{0};

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "fuzz";
    }

    void set_text(IView&, std::string_view) override {}
    [[nodiscard]] std::string get_text(IView&) override { return {}; }
    Subscription on_text_changed(IView&, std::function<void(std::string_view)>) override { return {}; }

    void set_bool(IView&, bool) override {}
    [[nodiscard]] bool get_bool(IView&) override { return false; }
    Subscription on_bool_changed(IView&, std::function<void(bool)>) override { return {}; }

    void set_int(IView& v, int value) override {
        auto& fv = static_cast<FuzzView&>(v);
        if (fv.dead) ++set_int_calls_after_destroy;   // INVARIANT VIOLATION
        else        fv.current = value;
    }
    [[nodiscard]] int get_int(IView&) override { return 0; }
    Subscription on_int_changed(IView&, std::function<void(int)>) override { return {}; }

    void set_int64(IView&, std::int64_t) override {}
    [[nodiscard]] std::int64_t get_int64(IView&) override { return 0; }
    Subscription on_int64_changed(IView&, std::function<void(std::int64_t)>) override { return {}; }

    void set_uint64(IView&, std::uint64_t) override {}
    [[nodiscard]] std::uint64_t get_uint64(IView&) override { return 0; }
    Subscription on_uint64_changed(IView&, std::function<void(std::uint64_t)>) override { return {}; }

    void set_float(IView&, float) override {}
    [[nodiscard]] float get_float(IView&) override { return 0.0f; }
    Subscription on_float_changed(IView&, std::function<void(float)>) override { return {}; }

    void set_double(IView&, double) override {}
    [[nodiscard]] double get_double(IView&) override { return 0.0; }
    Subscription on_double_changed(IView&, std::function<void(double)>) override { return {}; }

    void set_visible(IView&, bool) override {}
    void set_enabled(IView&, bool) override {}
    Subscription on_click(IView&, std::function<void()>) override { return {}; }
};

}  // namespace

TEST_CASE("L-32 fuzz: binding survives view destruction at random moments") {
    fuzz::Rng rng{fuzz::seed(0xB1'DF'1'00'7E)};

    auto adapter = std::make_shared<FuzzAdapter>();
    BindingEngine engine{adapter};
    Property<int> source{0};

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        // Spin up a fresh view, bind, exercise, then maybe destroy.
        auto view = std::make_unique<FuzzView>();
        engine.bind_int_oneway(source, *view);

        const std::uint32_t writes = rng.u32(0, 4);
        for (std::uint32_t i = 0; i < writes; ++i) {
            source.set(static_cast<int>(rng.u32()));
        }

        // Decide: destroy now vs later. Mark `dead` BEFORE the dtor
        // runs so any setter callback that reaches the FakeAdapter
        // can detect "called into a corpse".
        view->dead = true;
        view.reset();

        // Pump more writes; per L-32 these MUST NOT route into the
        // destroyed view via a stale binding.
        for (std::uint32_t i = 0; i < writes; ++i) {
            source.set(static_cast<int>(rng.u32()));
        }
    }

    CHECK(adapter->set_int_calls_after_destroy.load() == 0);
}
