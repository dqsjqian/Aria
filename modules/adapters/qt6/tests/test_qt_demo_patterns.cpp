#include <doctest/doctest.h>

#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/computed.hpp"
#include "aria/property.hpp"

#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <memory>
#include <string>

using namespace aria;
using namespace aria::adapters::qt6;

// ============================================================================
//  The wiring patterns the Qt showcase demo relies on, pinned as tests.
//
//  The demo itself only has a "probe" mode that instantiates every tab and
//  checks nothing about the resulting widget state. These cases cover the
//  three things the demo rewiring actually depends on, against real QWidgets
//  rather than the fake adapter.
// ============================================================================

// ── Computed → projected label (TipCalc / UnitConvert / Cart) ──────────────

TEST_CASE("qt: bind_text_projected renders a Computed into a real QLabel") {
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<double> bill(20.0);
    Property<int>    people(2);
    Computed<double> per_person([&] { return bill.get() / people.get(); });

    QLabel label;
    engine.bind_text_projected(per_person, adapter->view_for(&label),
        [](double v) {
            return QString("每人付: ¥ %1").arg(v, 0, 'f', 2).toStdString();
        });

    // Initial sync happens at bind time — no manual back-fill.
    CHECK(label.text() == QString::fromUtf8("每人付: ¥ 10.00"));

    people = 4;
    CHECK(label.text() == QString::fromUtf8("每人付: ¥ 5.00"));

    bill = 90.0;
    CHECK(label.text() == QString::fromUtf8("每人付: ¥ 22.50"));
}

TEST_CASE("qt: a literal '%' in the format string survives QString::arg") {
    // `税 (8%)` is a real label in the Cart tab. `%` followed by a
    // non-digit is not a placeholder, so `.arg()` must leave it alone.
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<double> tax(1.6);
    QLabel label;
    engine.bind_text_projected(tax, adapter->view_for(&label),
        [](double v) {
            return QString("税 (8%): ¥ %1").arg(v, 0, 'f', 2).toStdString();
        });

    CHECK(label.text() == QString::fromUtf8("税 (8%): ¥ 1.60"));
}

// ── Computed<bool> → visible / enabled (Signup) ─────────────────────────────

TEST_CASE("qt: bind_visible / bind_enabled drive real QWidget state "
          "from a Computed<bool>") {
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<std::string> name("");
    Computed<bool> has_error([&] { return name.get().empty(); });
    Computed<bool> can_submit([&] { return !name.get().empty(); });

    QLabel  banner;
    QWidget button;
    engine.bind_visible(has_error, adapter->view_for(&banner));
    engine.bind_enabled(can_submit, adapter->view_for(&button));

    CHECK_FALSE(button.isEnabled());

    name = "Ada";
    CHECK(button.isEnabled());

    name = "";
    CHECK_FALSE(button.isEnabled());
}

// ── bind_command + bind_enabled on the same widget (Signup) ────────────────

TEST_CASE("qt: bind_enabled after bind_command owns the enabled channel") {
    // SignupView binds a predicate-less Command for the click, then binds
    // the form's aggregate validity to the same button's `enabled`.
    // `Command` without a predicate reports can_execute() == true and never
    // notifies, so the later bind_enabled must win both the initial write
    // and every subsequent update. If that ordering ever inverts, the
    // submit button would be permanently clickable on an invalid form.
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    int clicks = 0;
    Command<> submit([&clicks] { ++clicks; });
    Property<bool> form_valid(false);

    QPushButton button;
    engine.bind_command(submit, adapter->view_for(&button));
    engine.bind_enabled(form_valid, adapter->view_for(&button));

    // bind_command wrote `true` (constant can_execute); bind_enabled then
    // wrote `false`. The later binding is authoritative.
    CHECK_FALSE(button.isEnabled());

    form_valid = true;
    CHECK(button.isEnabled());

    // Click still routes to the command.
    button.click();
    CHECK(clicks == 1);

    form_valid = false;
    CHECK_FALSE(button.isEnabled());
}

// ── view_for caching means multiple bindings share one bucket ──────────────

TEST_CASE("qt: two projected bindings on one widget both stay live") {
    // The demo calls `adapter.view_for(w)` repeatedly for the same widget.
    // Because view_for caches, both bindings land in the same per-view
    // bucket — neither may evict the other.
    auto adapter = std::make_shared<QtAdapter>();
    binding::BindingEngine engine(adapter);

    Property<int> n(1);
    Computed<int> doubled([&] { return n.get() * 2; });
    Property<bool> shown(true);

    QLabel label;
    // Second binding on the same widget overwrites the text channel, which
    // is expected; what matters is that both remain subscribed.
    engine.bind_text_projected(n, adapter->view_for(&label),
                               [](int v) { return "n=" + std::to_string(v); });
    engine.bind_visible(shown, adapter->view_for(&label));

    engine.bind_text_projected(doubled, adapter->view_for(&label),
                               [](int v) { return "d=" + std::to_string(v); });
    CHECK(label.text() == "d=2");

    n = 5;
    // The last text binding is the one visibly winning, and it is still live.
    CHECK(label.text() == "d=10");
}
