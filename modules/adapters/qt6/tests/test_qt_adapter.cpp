#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_view.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

using namespace aria;
using namespace aria::adapters::qt6;

TEST_CASE("QtAdapter: set_text on QLabel") {
    QLabel label;
    QtView v(&label);
    QtAdapter adapter;

    adapter.set_text(v, "hello");
    CHECK(label.text() == "hello");
    CHECK(adapter.get_text(v) == "hello");
}

TEST_CASE("QtAdapter: text round-trip on QLineEdit") {
    QLineEdit input;
    QtView v(&input);
    QtAdapter adapter;

    adapter.set_text(v, "alice");
    CHECK(input.text() == "alice");

    int hits = 0;
    std::string captured;
    auto sub = adapter.on_text_changed(v, [&](std::string_view sv) {
        ++hits;
        captured = std::string(sv);
    });

    input.setText("bob");
    CHECK(hits == 1);
    CHECK(captured == "bob");
}

TEST_CASE("QtAdapter: bool round-trip on QCheckBox") {
    QCheckBox box;
    QtView v(&box);
    QtAdapter adapter;

    adapter.set_bool(v, true);
    CHECK(box.isChecked());
    CHECK(adapter.get_bool(v));

    int hits = 0;
    bool last = false;
    auto sub = adapter.on_bool_changed(v, [&](bool b) { ++hits; last = b; });

    box.setChecked(false);
    CHECK(hits == 1);
    CHECK_FALSE(last);
}

TEST_CASE("QtAdapter: int round-trip on QSpinBox") {
    QSpinBox sp;
    sp.setRange(0, 100);
    QtView v(&sp);
    QtAdapter adapter;

    adapter.set_int(v, 42);
    CHECK(sp.value() == 42);
    CHECK(adapter.get_int(v) == 42);

    int last = -1;
    auto sub = adapter.on_int_changed(v, [&](int x) { last = x; });
    sp.setValue(7);
    CHECK(last == 7);
}

TEST_CASE("QtAdapter: visible/enabled on QWidget") {
    QPushButton btn;
    QtView v(&btn);
    QtAdapter adapter;

    adapter.set_enabled(v, false);
    CHECK_FALSE(btn.isEnabled());
    adapter.set_enabled(v, true);
    CHECK(btn.isEnabled());

    adapter.set_visible(v, false);
    CHECK_FALSE(btn.isVisible());
}

TEST_CASE("QtAdapter: click on QPushButton fires the callback") {
    QPushButton btn;
    QtView v(&btn);
    QtAdapter adapter;

    int hits = 0;
    auto sub = adapter.on_click(v, [&] { ++hits; });

    btn.click();
    btn.click();
    CHECK(hits == 2);

    sub.release();
    btn.click();
    CHECK(hits == 2);
}

TEST_CASE("QtAdapter: subscription survives QObject destruction") {
    QPushButton* btn = new QPushButton;
    QtView v(btn);
    QtAdapter adapter;

    int hits = 0;
    auto sub = adapter.on_click(v, [&] { ++hits; });

    delete btn;          // QPointer in QtView nulls out
    // Subscription still alive; releasing it must NOT crash.
    sub.release();
    CHECK(hits == 0);
}
