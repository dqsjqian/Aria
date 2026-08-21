#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_view.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/property.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include <memory>
#include <stdexcept>

using namespace aria;
using namespace aria::adapters::qt6;

// ── QtAdapter::view_for — handle → IView, owned by the adapter ─────────────
// Replaces the per-app `view_for(handle)` helper that every host used to
// hand-roll (with a process-global keepalive vector that never released).

TEST_CASE("view_for: returns the same QtView for the same QObject") {
    QLabel a, b;
    QtAdapter adapter;

    auto& va1 = adapter.view_for(&a);
    auto& va2 = adapter.view_for(&a);
    auto& vb  = adapter.view_for(&b);

    CHECK(&va1 == &va2);          // cached — one QtView per handle
    CHECK(&va1 != &vb);
    CHECK(va1.object() == &a);
    CHECK(va1.kind() == "qt6");
}

TEST_CASE("view_for: rejects a null handle") {
    QtAdapter adapter;
    CHECK_THROWS_AS((void)adapter.view_for(nullptr), std::invalid_argument);
}

TEST_CASE("view_for: the returned view is usable by the typed adapter API") {
    QLineEdit edit;
    QtAdapter adapter;

    auto& v = adapter.view_for(&edit);
    adapter.set_text(v, "alice");
    CHECK(edit.text() == "alice");
    CHECK(adapter.get_text(v) == "alice");
}

TEST_CASE("view_for: widget destruction releases the binding, no dangling write") {
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<std::string> name("Alice");
    auto* label = new QLabel;

    engine.bind_text_oneway(name, adapter->view_for(label));
    CHECK(label->text() == "Alice");

    // Native teardown: QObject::destroyed fires → QtView::fire_destroy_ →
    // BindingEngine drops the per-view bucket. A subsequent property write
    // must be a no-op rather than a use-after-free.
    delete label;
    name = "Bob";   // must not crash
    CHECK(name.get() == "Bob");
}

TEST_CASE("view_for: adopted subscription is released on widget destroy") {
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<int> counter(0);
    int hits = 0;
    auto* w = new QWidget;

    engine.adopt(adapter->view_for(w),
                 counter.on_changed([&hits](int) { ++hits; }));

    counter = 1;
    CHECK(hits == 1);

    delete w;       // view-destroy → bucket cleared → subscription released
    counter = 2;
    CHECK(hits == 1);   // no further callbacks
}

TEST_CASE("view_for: engine clear() also drops adopted subscriptions") {
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<int> counter(0);
    int hits = 0;
    QWidget w;   // outlives the engine

    engine.adopt(adapter->view_for(&w),
                 counter.on_changed([&hits](int) { ++hits; }));

    counter = 1;
    CHECK(hits == 1);

    engine.clear();
    counter = 2;
    CHECK(hits == 1);
}

TEST_CASE("view_for: adapter outliving its widgets destroys cleanly") {
    auto adapter = std::make_shared<QtAdapter>();
    {
        QLabel tmp;
        auto& v = adapter->view_for(&tmp);
        adapter->set_text(v, "x");
        CHECK(tmp.text() == "x");
    }
    // `tmp` is gone; the cache entry was dropped by the destroyed handler.
    // A fresh view_for on a new widget must still work.
    QLabel other;
    auto& v2 = adapter->view_for(&other);
    adapter->set_text(v2, "y");
    CHECK(other.text() == "y");
}
