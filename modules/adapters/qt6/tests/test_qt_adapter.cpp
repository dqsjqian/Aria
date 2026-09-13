#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QDoubleSpinBox>
#include "aria/binding/binding_engine.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
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

TEST_CASE("QtAdapter: QComboBox already supports text two-way binding") {
    auto adapter = std::make_shared<QtAdapter>();
    QComboBox combo;
    combo.addItems({"A", "B", "C"});
    QtView view(&combo);
    Property<std::string> selection("B");
    binding::BindingEngine engine(adapter);
    engine.bind_text(selection, view);
    CHECK(combo.currentText() == "B");
    selection.set("C");
    CHECK(combo.currentText() == "C");
    combo.setCurrentIndex(0);
    CHECK(selection.get() == "A");
}

TEST_CASE("QtAdapter: QComboBox integer binding uses indices and supports empty selection") {
    auto adapter = std::make_shared<QtAdapter>();
    QComboBox combo;
    combo.addItems({"Same", "Same", "Other"});
    auto& view = adapter->view_for(&combo);
    Property<int> selection(1);
    binding::BindingEngine engine(adapter);
    engine.bind_int(selection, view);
    CHECK(combo.currentIndex() == 1);
    CHECK(adapter->get_int(view) == 1);
    selection.set(2);
    CHECK(combo.currentIndex() == 2);
    combo.setCurrentIndex(0);
    CHECK(selection.get() == 0);
    selection.set(-1);
    CHECK(combo.currentIndex() == -1);
    CHECK(adapter->get_int(view) == -1);
    combo.setCurrentIndex(1);
    CHECK(selection.get() == 1);
    combo.setCurrentIndex(-1);
    CHECK(selection.get() == -1);
}

TEST_CASE("QtAdapter: QComboBox enum conversion preserves explicit index mapping") {
    enum class Category { Temperature = 10, Length = 30, Weight = 80 };
    auto adapter = std::make_shared<QtAdapter>();
    QComboBox combo;
    combo.addItems({"Temperature", "Length", "Weight"});
    Property<Category> category(Category::Length);
    binding::BindingEngine engine(adapter);
    engine.bind_int_converted(category, adapter->view_for(&combo),
        binding::Converter<Category, int>{
            [](Category value) {
                if (value == Category::Temperature) return 0;
                return value == Category::Length ? 1 : 2;
            }, {},
            [](int index) -> std::optional<Category> {
                switch (index) {
                    case 0: return Category::Temperature;
                    case 1: return Category::Length;
                    case 2: return Category::Weight;
                    default: return std::nullopt;
                }
            }});
    CHECK(combo.currentIndex() == 1);
    category.set(Category::Weight);
    CHECK(combo.currentIndex() == 2);
    combo.setCurrentIndex(0);
    CHECK(category.get() == Category::Temperature);
}

TEST_CASE("QtAdapter: QComboBox destruction releases converted binding and index subscription") {
    auto adapter = std::make_shared<QtAdapter>();
    auto combo = std::make_unique<QComboBox>();
    combo->addItems({"First", "Second"});
    auto& view = adapter->view_for(combo.get());
    Property<int> selection(0);
    binding::BindingEngine engine(adapter);
    int renders = 0;
    engine.bind_int_converted(selection, view, binding::Converter<int, int>{
        [&](int value) { ++renders; return value; },
        [](int value) { return value; }, {}});
    auto sub = adapter->on_int_changed(view, [](int) {});
    CHECK(renders == 1);
    combo.reset();
    sub.release();
    selection.set(1);
    CHECK(renders == 1);
    CHECK_FALSE(sub.active());
}

namespace {
struct AdapterCaptureRelease {
    std::function<void()> callback;
    ~AdapterCaptureRelease() { callback(); }
};
} // namespace

TEST_CASE("QtAdapter: widget destruction permits subscription capture reentry") {
    QtAdapter adapter;
    auto widget = std::make_unique<QLineEdit>();
    QtView view(widget.get());
    QLineEdit other_widget;
    QtView other(&other_widget);
    int releases = 0;
    auto capture = std::shared_ptr<AdapterCaptureRelease>(new AdapterCaptureRelease{[&] {
        ++releases;
        auto nested = adapter.on_text_changed(other, [](std::string_view) {});
        CHECK(nested.active());
    }});
    auto sub = adapter.on_text_changed(view, [capture](std::string_view) {});
    capture.reset();
    widget.reset();
    CHECK(releases == 1);
    sub.release();
}

TEST_CASE("QtAdapter: teardown rejects reentrant subscriptions and cached views") {
    QLineEdit text;
    QCheckBox boolean;
    QSpinBox integer;
    QDoubleSpinBox number;
    QPushButton button;
    auto adapter = std::make_unique<QtAdapter>();
    auto* raw = adapter.get();
    QtView other(&text);
    int releases = 0;
    auto capture = std::shared_ptr<AdapterCaptureRelease>(new AdapterCaptureRelease{[&] {
        ++releases;
        auto nested = raw->on_text_changed(other, [](std::string_view) {});
        CHECK_FALSE(nested.active());
        CHECK_THROWS_AS((void)raw->view_for(&text), std::logic_error);
    }});
    auto t = adapter->on_text_changed(adapter->view_for(&text), [capture](std::string_view) {});
    auto b = adapter->on_bool_changed(adapter->view_for(&boolean), [capture](bool) {});
    auto i = adapter->on_int_changed(adapter->view_for(&integer), [capture](int) {});
    auto d = adapter->on_double_changed(adapter->view_for(&number), [capture](double) {});
    auto c = adapter->on_click(adapter->view_for(&button), [capture] {});
    capture.reset();
    adapter.reset();
    CHECK(releases == 1);
    t.release(); b.release(); i.release(); d.release(); c.release();
    text.setText("after adapter destruction");
    button.click();
}

TEST_CASE("QtAdapter: callback can destroy adapter and cancel later subscribers") {
    QLineEdit widget;
    QtView view(&widget);
    auto adapter = std::make_unique<QtAdapter>();
    int calls = 0;
    auto first = adapter->on_text_changed(view, [&](std::string_view) {
        ++calls;
        adapter.reset();
    });
    auto second = adapter->on_text_changed(view, [&](std::string_view) { calls += 10; });
    widget.setText("invoke");
    CHECK(calls == 1);
    first.release();
    second.release();
    widget.setText("disconnected");
    CHECK(calls == 1);
}

TEST_CASE("QtAdapter: QDial and QScrollBar support both integer directions") {
    QtAdapter adapter;
    QDial dial;
    QScrollBar scroll;
    for (QAbstractSlider* widget : {static_cast<QAbstractSlider*>(&dial),
                                    static_cast<QAbstractSlider*>(&scroll)}) {
        widget->setRange(0, 100);
        QtView view(widget);
        int observed = -1;
        auto sub = adapter.on_int_changed(view, [&](int value) { observed = value; });
        adapter.set_int(view, 42);
        CHECK(widget->value() == 42);
        CHECK(adapter.get_int(view) == 42);
        CHECK(observed == 42);
        widget->setValue(77);
        CHECK(observed == 77);
    }
}

TEST_CASE("QtAdapter: QProgressBar forwards valueChanged") {
    QtAdapter adapter;
    QProgressBar progress;
    QtView view(&progress);
    int observed = -1;
    auto sub = adapter.on_int_changed(view, [&](int value) { observed = value; });
    adapter.set_int(view, 42);
    CHECK(adapter.get_int(view) == 42);
    CHECK(observed == 42);
    progress.setValue(77);
    CHECK(observed == 77);
}
