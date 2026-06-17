#include <doctest/doctest.h>

#include "aria/selection.hpp"
#include "aria/observable_list.hpp"

#include <memory>
#include <string>

using namespace aria;

namespace {
struct Row {
    int id;
    explicit Row(int i) : id(i) {}
};
}  // namespace

// ── SE-1: single selection ──────────────────────────────────────────────────

TEST_CASE("Selection: select / clear / is_selected") {
    Selection<Row> sel;
    CHECK_FALSE(sel.has_value());

    auto a = std::make_shared<Row>(1);
    auto b = std::make_shared<Row>(2);

    int hits = 0;
    auto sub = sel.selected().on_changed([&](const std::shared_ptr<Row>&) { ++hits; });

    sel.select(a);
    CHECK(sel.has_value());
    CHECK(sel.is_selected(a));
    CHECK_FALSE(sel.is_selected(b));
    CHECK(sel.value()->id == 1);

    sel.select(b);
    CHECK(sel.is_selected(b));

    sel.clear();
    CHECK_FALSE(sel.has_value());
    CHECK(hits == 3);  // a, b, clear
}

// ── SE-3: single selection follows source list ──────────────────────────────

TEST_CASE("Selection: bind_to drops selection when item removed") {
    ObservableList<Row> list;
    auto a = std::make_shared<Row>(1);
    auto b = std::make_shared<Row>(2);
    list.push_back(a);
    list.push_back(b);

    Selection<Row> sel;
    sel.bind_to(list);
    sel.select(b);
    CHECK(sel.is_selected(b));

    // Remove a different element -> selection intact.
    list.remove_at(0);          // removes a
    CHECK(sel.is_selected(b));

    // Remove the selected element -> selection cleared.
    list.remove_first([](const Row& r) { return r.id == 2; });
    CHECK_FALSE(sel.has_value());
}

TEST_CASE("Selection: bind_to clears on Reset") {
    ObservableList<Row> list;
    auto a = std::make_shared<Row>(1);
    list.push_back(a);

    Selection<Row> sel;
    sel.bind_to(list);
    sel.select(a);
    CHECK(sel.has_value());

    list.clear();
    CHECK_FALSE(sel.has_value());
}

TEST_CASE("Selection: repositioning (move) keeps selection") {
    ObservableList<Row> list;
    auto a = std::make_shared<Row>(1);
    auto b = std::make_shared<Row>(2);
    auto c = std::make_shared<Row>(3);
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    Selection<Row> sel;
    sel.bind_to(list);
    sel.select(c);

    list.move(2, 0);            // c moves to front
    CHECK(sel.is_selected(c));  // still selected
}

// ── SE-2 / SE-5: multi selection ─────────────────────────────────────────────

TEST_CASE("MultiSelection: add / remove / toggle preserve pick order") {
    MultiSelection<Row> sel;
    auto a = std::make_shared<Row>(1);
    auto b = std::make_shared<Row>(2);
    auto c = std::make_shared<Row>(3);

    sel.add(b);
    sel.add(a);
    sel.add(c);
    CHECK(sel.size() == 3);

    auto vals = sel.values();
    CHECK(vals[0]->id == 2);
    CHECK(vals[1]->id == 1);
    CHECK(vals[2]->id == 3);

    sel.add(b);                 // duplicate ignored
    CHECK(sel.size() == 3);

    sel.toggle(a);              // a was present -> removed
    CHECK_FALSE(sel.is_selected(a));
    CHECK(sel.size() == 2);

    sel.toggle(a);              // re-add
    CHECK(sel.is_selected(a));

    sel.clear();
    CHECK(sel.empty());
}

TEST_CASE("MultiSelection: bind_to drops removed items only") {
    ObservableList<Row> list;
    auto a = std::make_shared<Row>(1);
    auto b = std::make_shared<Row>(2);
    auto c = std::make_shared<Row>(3);
    list.push_back(a);
    list.push_back(b);
    list.push_back(c);

    MultiSelection<Row> sel;
    sel.bind_to(list);
    sel.add(a);
    sel.add(c);
    CHECK(sel.size() == 2);

    list.remove_first([](const Row& r) { return r.id == 1; });  // remove a
    CHECK(sel.size() == 1);
    CHECK(sel.is_selected(c));
    CHECK_FALSE(sel.is_selected(a));

    list.clear();
    CHECK(sel.empty());
}

TEST_CASE("Selection: unbind stops following the list") {
    ObservableList<Row> list;
    auto a = std::make_shared<Row>(1);
    list.push_back(a);

    Selection<Row> sel;
    sel.bind_to(list);
    sel.select(a);
    sel.unbind();

    list.clear();               // would clear if still bound
    CHECK(sel.has_value());     // unbound -> selection retained
}
