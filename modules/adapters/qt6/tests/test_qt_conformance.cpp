#include <doctest/doctest.h>

// Qt6 adapter — conformance suite driver.
//
// Runs the shared `aria::binding::testing::conformance` battery against
// the Qt6 `QtAdapter`, making sure the native Qt implementation honours
// the same cross-platform contract as `FakeAdapter`.

#include "aria/binding/testing/adapter_conformance.hpp"
#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_view.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <memory>
#include <string>
#include <utility>

namespace {

using namespace aria;
using namespace aria::binding;
using namespace aria::binding::testing;
using namespace aria::adapters::qt6;

// ViewHolder templated on the Qt widget type. Owns the widget on the
// heap (so we can destroy it to exercise IView::on_destroy semantics)
// plus a QtView wrapping it.
//
// Member ORDER matters: we want `qt_view` (the IView) to die FIRST,
// so that `~IView` fires on_destroy while the underlying QWidget is
// still valid — giving BindingEngine a chance to tear down its
// bindings before the QWidget is reclaimed. Members are destroyed in
// reverse declaration order, so `widget` goes first in the struct and
// `qt_view` goes last.
template<class Widget>
struct QtViewHolder {
    std::unique_ptr<Widget>  widget = std::make_unique<Widget>();
    std::unique_ptr<QtView>  qt_view{new QtView(widget.get())};

    IView& view() { return *qt_view; }
    Widget* w() { return widget.get(); }
};

struct QtHarness {
    std::shared_ptr<IViewAdapter> make_adapter() {
        return std::make_shared<QtAdapter>();
    }

    QtViewHolder<QLineEdit>      make_text_view()   { return {}; }
    QtViewHolder<QCheckBox>      make_bool_view()   { return {}; }

    QtViewHolder<QSpinBox> make_int_view() {
        QtViewHolder<QSpinBox> h;
        h.w()->setRange(-1'000'000, 1'000'000);
        return h;
    }

    QtViewHolder<QDoubleSpinBox> make_double_view() {
        QtViewHolder<QDoubleSpinBox> h;
        h.w()->setRange(-1e9, 1e9);
        h.w()->setDecimals(3);
        return h;
    }

    QtViewHolder<QPushButton>    make_click_view()  { return {}; }

    // Simulated user input — touch the native widget with the same API
    // a human user's key / click events would hit.
    void user_type(IView& v, std::string text) {
        auto* le = static_cast<QtView&>(v).as<QLineEdit>();
        REQUIRE(le != nullptr);
        le->setText(QString::fromStdString(text));
    }
    void user_toggle(IView& v, bool b) {
        auto* box = static_cast<QtView&>(v).as<QCheckBox>();
        REQUIRE(box != nullptr);
        box->setChecked(b);
    }
    void user_set_int(IView& v, int n) {
        auto* sp = static_cast<QtView&>(v).as<QSpinBox>();
        REQUIRE(sp != nullptr);
        sp->setValue(n);
    }
    void user_set_double(IView& v, double d) {
        auto* sp = static_cast<QtView&>(v).as<QDoubleSpinBox>();
        REQUIRE(sp != nullptr);
        sp->setValue(d);
    }
    void user_click(IView& v) {
        auto* btn = static_cast<QtView&>(v).as<QPushButton>();
        REQUIRE(btn != nullptr);
        btn->click();
    }
};

}  // namespace

TEST_CASE("Conformance (Qt6): text two-way at adapter level") {
    QtHarness h;
    conformance::run_text_two_way(h);
}

TEST_CASE("Conformance (Qt6): text two-way via BindingEngine") {
    QtHarness h;
    conformance::run_text_engine_two_way(h);
}

TEST_CASE("Conformance (Qt6): bool two-way at adapter level") {
    QtHarness h;
    conformance::run_bool_two_way(h);
}

TEST_CASE("Conformance (Qt6): bool two-way via BindingEngine") {
    QtHarness h;
    conformance::run_bool_engine_two_way(h);
}

TEST_CASE("Conformance (Qt6): int two-way at adapter level") {
    QtHarness h;
    conformance::run_int_two_way(h);
}

TEST_CASE("Conformance (Qt6): double two-way at adapter level") {
    QtHarness h;
    conformance::run_double_two_way(h);
}

TEST_CASE("Conformance (Qt6): click callback subscribe / release") {
    QtHarness h;
    conformance::run_click(h);
}

TEST_CASE("Conformance (Qt6): Command<> drives button enabled") {
    QtHarness h;
    conformance::run_command_enabled(h);
}

TEST_CASE("Conformance (Qt6): view destroy releases bindings safely") {
    QtHarness h;
    conformance::run_view_destroy_safety(h);
}

// Optional numeric extras — Qt's QSpinBox / QDoubleSpinBox forward
// int64 / uint64 / float through the int / double fast paths. These
// pin the cast path as lossless for in-range values, matching the
// AppKit / UIKit conformance contract.
TEST_CASE("Conformance (Qt6): int64 via int view") {
    QtHarness h;
    conformance::run_int64_two_way_via_int_view(h);
}

TEST_CASE("Conformance (Qt6): uint64 via int view") {
    QtHarness h;
    conformance::run_uint64_two_way_via_int_view(h);
}

TEST_CASE("Conformance (Qt6): float via double view") {
    QtHarness h;
    conformance::run_float_two_way_via_double_view(h);
}
