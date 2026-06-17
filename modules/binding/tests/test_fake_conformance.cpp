#include <doctest/doctest.h>

// Adapter conformance suite — Fake driver.
//
// This file's sole purpose is to demonstrate that `FakeAdapter` fully
// satisfies the shared contract defined in
// `aria/binding/testing/adapter_conformance.hpp`. The same file is what
// every future platform adapter (AppKit / UIKit / JNI / WASM / ...)
// should author as its first test.

#include "aria/binding/testing/adapter_conformance.hpp"
#include "fake_adapter.hpp"

#include <memory>

namespace {

using namespace aria;
using namespace aria::binding;
using namespace aria::binding::testing;

// RAII holder: owns a FakeView and exposes a reference. FakeView has no
// native resources to release, but this wrapper keeps the `ViewHandle`
// semantics uniform across platforms (and lets us test view-destroy
// safety via simple scope-based destruction).
struct FakeViewHolder {
    std::unique_ptr<FakeView> v = std::make_unique<FakeView>();
    IView& view() { return *v; }
};

struct FakeHarness {
    std::shared_ptr<IViewAdapter> make_adapter() {
        return std::make_shared<FakeAdapter>();
    }

    FakeViewHolder make_text_view()   { return {}; }
    FakeViewHolder make_bool_view()   { return {}; }
    FakeViewHolder make_int_view()    { return {}; }
    FakeViewHolder make_double_view() { return {}; }
    FakeViewHolder make_click_view()  { return {}; }

    void user_type(IView& v, std::string text) {
        FakeAdapter::user_type(static_cast<FakeView&>(v), std::move(text));
    }
    void user_toggle(IView& v, bool b) {
        FakeAdapter::user_toggle(static_cast<FakeView&>(v), b);
    }
    void user_set_int(IView& v, int n) {
        auto& fv = static_cast<FakeView&>(v);
        fv.integer = n;
        fv.sig_int.emit(n);
    }
    void user_set_double(IView& v, double d) {
        auto& fv = static_cast<FakeView&>(v);
        fv.number = d;
        fv.sig_double.emit(d);
    }
    void user_click(IView& v) {
        FakeAdapter::user_click(static_cast<FakeView&>(v));
    }
};

}  // namespace

// ───────────────────────────────────────────────────────────────────────
//  Each conformance entry becomes one doctest case, so a failure points
//  directly at the offending contract.
// ───────────────────────────────────────────────────────────────────────

TEST_CASE("Conformance (Fake): text two-way at adapter level") {
    FakeHarness h;
    conformance::run_text_two_way(h);
}

TEST_CASE("Conformance (Fake): text two-way via BindingEngine") {
    FakeHarness h;
    conformance::run_text_engine_two_way(h);
}

TEST_CASE("Conformance (Fake): bool two-way at adapter level") {
    FakeHarness h;
    conformance::run_bool_two_way(h);
}

TEST_CASE("Conformance (Fake): bool two-way via BindingEngine") {
    FakeHarness h;
    conformance::run_bool_engine_two_way(h);
}

TEST_CASE("Conformance (Fake): int two-way at adapter level") {
    FakeHarness h;
    conformance::run_int_two_way(h);
}

TEST_CASE("Conformance (Fake): double two-way at adapter level") {
    FakeHarness h;
    conformance::run_double_two_way(h);
}

TEST_CASE("Conformance (Fake): click callback subscribe / release") {
    FakeHarness h;
    conformance::run_click(h);
}

TEST_CASE("Conformance (Fake): Command<> drives button enabled") {
    FakeHarness h;
    conformance::run_command_enabled(h);
}

TEST_CASE("Conformance (Fake): view destroy releases bindings safely") {
    FakeHarness h;
    conformance::run_view_destroy_safety(h);
}
