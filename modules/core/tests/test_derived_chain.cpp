// Stage 2 end-to-end acceptance: chain Filter → Sort → Map over a
// 10k-item source running 1000 random mutations, and assert the final
// derived state matches the ground truth computed from the source
// snapshot.
//
// The "no Reset under mutation" claim is ALSO checked — each link of
// the chain must emit incremental events only (Insert / Remove /
// Replace / ItemChanged / Move), never Reset (outside of an explicit
// source->clear()).

#include <doctest/doctest.h>

#include "aria/derived/filtered_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/derived/mapped_list.hpp"
#include "aria/observable_list.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace aria;

namespace {

struct Item {
    int         id;
    int         priority;
    bool        active;
};

struct ItemView {
    int         id;
    int         priority;
};

auto make_item(int id, int priority, bool active) {
    return std::make_shared<Item>(Item{id, priority, active});
}

}  // namespace

TEST_CASE("Derived chain: Filter → Sort → Map over 10k × 1000 mutations") {
    auto source = std::make_shared<ObservableList<Item>>();
    std::mt19937 seed_rng{7};
    for (int i = 0; i < 10'000; ++i) {
        std::uniform_int_distribution<int> prio{0, 999};
        std::bernoulli_distribution act{0.5};
        source->push_back(make_item(i, prio(seed_rng), act(seed_rng)));
    }

    auto filtered = aria::filtered(source, [](const Item& it) { return it.active; });
    auto ordered = aria::sorted(filtered, [](const Item& a, const Item& b) {
        return a.priority < b.priority;
    });
    auto mapped = aria::mapped<ItemView>(ordered, [](const Item& it) {
        return std::make_shared<ItemView>(ItemView{it.id, it.priority});
    });

    int filtered_events = 0, sorted_events = 0, mapped_events = 0;
    int filtered_resets = 0, sorted_resets = 0, mapped_resets = 0;
    auto sub_f = filtered->observe([&](const auto& change) {
        ++filtered_events;
        if (change.kind == ListChangeKind::Reset) ++filtered_resets;
    });
    auto sub_s = ordered->observe([&](const auto& change) {
        ++sorted_events;
        if (change.kind == ListChangeKind::Reset) ++sorted_resets;
    });
    // A downstream observer uses only owned events, never re-reads a source
    // that may already have committed the rest of a multi-event operation.
    auto mirror = mapped->snapshot();
    auto sub_m = mapped->observe([&](const ListChange<ItemView>& change) {
        ++mapped_events;
        const auto pos = static_cast<std::ptrdiff_t>(change.index);
        switch (change.kind) {
        case ListChangeKind::Insert:
            REQUIRE(change.index <= mirror.size());
            mirror.insert(mirror.begin() + pos, change.item);
            break;
        case ListChangeKind::Remove:
            REQUIRE(change.index < mirror.size());
            CHECK(mirror[change.index] == change.item);
            mirror.erase(mirror.begin() + pos);
            break;
        case ListChangeKind::Replace:
        case ListChangeKind::ItemChanged:
            REQUIRE(change.index < mirror.size());
            mirror[change.index] = change.item;
            break;
        case ListChangeKind::Move: {
            REQUIRE(change.from_index < mirror.size());
            REQUIRE(change.index < mirror.size());
            auto item = mirror[change.from_index];
            CHECK(item == change.item);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(change.from_index));
            mirror.insert(mirror.begin() + pos, std::move(item));
            break;
        }
        case ListChangeKind::Reset:
            REQUIRE(change.snapshot != nullptr);
            mirror = *change.snapshot;
            ++mapped_resets;
            break;
        }
    });

    // Independent oracle: source rows, a simple predicate pass, and the
    // standard stable sort. Never use a derived mapping to predict its output.
    const auto check_chain = [&] {
        std::vector<std::shared_ptr<Item>> expected;
        for (const auto& item : source->snapshot())
            if (item->active) expected.push_back(item);
        CHECK(filtered->snapshot() == expected);
        std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a->priority < b->priority;
        });
        CHECK(ordered->snapshot() == expected);
        std::vector<std::pair<int, int>> expected_values, actual_values;
        for (const auto& item : expected) expected_values.emplace_back(item->id, item->priority);
        const auto actual = mapped->snapshot();
        for (const auto& item : actual) actual_values.emplace_back(item->id, item->priority);
        CHECK(actual_values == expected_values);
        CHECK(mirror == actual);
    };
    check_chain();

    std::mt19937 rng{424242};
    std::uniform_int_distribution<int> pick_op{0, 99};
    std::uniform_int_distribution<int> pick_prio{0, 999};
    std::bernoulli_distribution pick_act{0.5};
    int next_id = 10'000;
    for (int step = 0; step < 1000; ++step) {
        CAPTURE(step);
        const int op = pick_op(rng);
        const std::size_t n = source->size();
        if (op < 40 || n == 0) {
            std::uniform_int_distribution<std::size_t> pick_ins{0, n};
            source->insert(pick_ins(rng), make_item(next_id++, pick_prio(rng), pick_act(rng)));
        } else {
            std::uniform_int_distribution<std::size_t> pick_pos{0, n - 1};
            if (op < 70) source->remove_at(pick_pos(rng));
            else if (op < 90)
                source->replace_at(pick_pos(rng), make_item(next_id++, pick_prio(rng), pick_act(rng)));
            else {
                const auto from = pick_pos(rng);
                const auto to = pick_pos(rng);
                if (from != to) source->move(from, to);
            }
        }
        check_chain();
    }

    CHECK(filtered_events > 0);
    CHECK(sorted_events > 0);
    CHECK(mapped_events > 0);
    CHECK(filtered_resets == 0);
    CHECK(sorted_resets == 0);
    CHECK(mapped_resets == 0);
    source->clear();
    CHECK(filtered_resets == 1);
    CHECK(sorted_resets == 1);
    CHECK(mapped_resets == 1);
    CHECK(filtered->empty());
    CHECK(ordered->empty());
    CHECK(mapped->empty());
    CHECK(mirror.empty());
}

TEST_CASE("Derived chain: Filter → Map live-updates under rapid bursts") {
    // A smaller, more surgical test: verify that chaining a
    // FilteredList into a MappedList (by hand, via a bridge list)
    // preserves incremental updates end-to-end.

    auto source = std::make_shared<ObservableList<Item>>();
    auto filtered = std::make_shared<FilteredList<Item>>(source,
        [](const Item& it) { return it.active; });
    bool clear_during_insert = false;
    bool cleared = false;
    auto earlier = filtered->observe([&](const auto& change) {
        if (clear_during_insert && change.kind == ListChangeKind::Insert) {
            clear_during_insert = false;
            cleared = true;
            filtered->set_predicate([](const Item&) { return false; });
        }
    });

    // Bridge: mirror `filtered` into its own ObservableList<Item>
    // so we can feed MappedList.
    auto bridge = std::make_shared<ObservableList<Item>>();
    auto bridge_sub = filtered->observe([&](const ListChange<Item>& ch) {
        switch (ch.kind) {
        case ListChangeKind::Insert: {
            bridge->insert(ch.index, ch.item);
            break;
        }
        case ListChangeKind::Remove:
            bridge->remove_at(ch.index);
            break;
        case ListChangeKind::Replace: {
            bridge->replace_at(ch.index, ch.item);
            break;
        }
        case ListChangeKind::Move:
            bridge->move(ch.from_index, ch.index);
            break;
        case ListChangeKind::ItemChanged:
            // Item is Plain and emits no ItemChanged in this fixture.
            break;
        case ListChangeKind::Reset:
            bridge->clear();
            REQUIRE(ch.snapshot != nullptr);
            bridge->insert_range(0, ch.snapshot->begin(), ch.snapshot->end());
            break;
        }
    });

    auto mapped = std::make_shared<MappedList<Item, ItemView>>(bridge,
        [](const Item& it) {
            return std::make_shared<ItemView>(ItemView{it.id, it.priority});
        });

    // Burst: push 200 items, half active.
    for (int i = 0; i < 200; ++i) {
        source->push_back(make_item(i, i, i % 2 == 0));
    }
    CHECK(filtered->size() == 100);
    CHECK(bridge->size()   == 100);
    CHECK(mapped->size()   == 100);

    // One source batch inserts before already-present rows. Every bridge
    // payload must be the item belonging to that intermediate event.
    std::vector<std::shared_ptr<Item>> extra{
        make_item(1000, 20, true), make_item(1001, 10, false), make_item(1002, 30, true)};
    source->insert_range(0, extra.begin(), extra.end());
    CHECK(bridge->snapshot() == filtered->snapshot());
    REQUIRE(mapped->size() == 102);
    CHECK(mapped->at(0)->id == 1000);
    CHECK(mapped->at(1)->id == 1002);
    CHECK(mapped->at(2)->id == 0);

    // The earlier observer removes every visible row before this Insert
    // reaches the bridge. Only its owned payload can preserve the original
    // event; reading filtered->at(index) now would access an absent slot.
    {
        std::vector<int> forwarded;
        auto record = mapped->observe([&](const auto& change) {
            if (change.kind == ListChangeKind::Insert) forwarded.push_back(change.item->id);
        });
        clear_during_insert = true;
        source->push_back(make_item(9999, 42, true));
        REQUIRE(cleared);
        CHECK(forwarded == std::vector<int>{9999});
        CHECK(bridge->empty());
        CHECK(mapped->empty());
    }
    filtered->set_predicate([](const Item& it) { return it.active; });

    // Now flip ALL items to inactive via Replace (ItemChanged would be
    // cheaper but Item is Plain). After the loop filtered → empty.
    const auto snapshot = source->snapshot();
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        source->replace_at(i, make_item(snapshot[i]->id, snapshot[i]->priority, false));
    }
    CHECK(filtered->empty());
    CHECK(bridge->empty());
    CHECK(mapped->empty());
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: filter callbacks can read and update their own view") {
    auto source = std::make_shared<ObservableList<int>>();
    std::shared_ptr<FilteredList<int>> view;
    bool nested = false;
    view = filtered(source, [&](const int& value) {
        if (view) {
            CHECK(view->snapshot().size() == view->size());
            if (value == 1 && !nested) {
                nested = true;
                view->set_predicate([](const int& n) { return n % 2 == 0; });
                source->emplace_back(2);
            }
        }
        return true;
    });
    std::vector<std::shared_ptr<int>> mirror;
    auto sub = view->observe([&](const auto& event) { detail::replay_list_change(mirror, event); });
    source->emplace_back(1);
    REQUIRE(view->size() == 1);
    CHECK(*view->at(0) == 2);
    CHECK(mirror == view->snapshot());
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: filter failure preserves old state and recovers") {
    auto source = std::make_shared<ObservableList<int>>();
    bool fail = false;
    auto view = filtered(source, [&](const int& value) {
        if (fail) { throw std::runtime_error("predicate failed"); }
        return value % 2 != 0;
    });
    source->emplace_back(1);
    std::vector<std::shared_ptr<int>> mirror = view->snapshot();
    auto sub = view->observe([&](const auto& event) { detail::replay_list_change(mirror, event); });
    fail = true;
    source->emplace_back(3);
    CHECK(view->snapshot() == mirror);
    fail = false;
    source->emplace_back(5);
    REQUIRE(view->size() == 3);
    CHECK(mirror == view->snapshot());
    CHECK_THROWS(view->set_predicate([](const int&) -> bool { throw std::runtime_error("new predicate failed"); }));
    source->emplace_back(7);
    REQUIRE(view->size() == 4);
    CHECK(mirror == view->snapshot());
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: sort callbacks can read and update their own view") {
    auto source = std::make_shared<ObservableList<int>>();
    std::shared_ptr<SortedList<int>> view;
    bool nested = false;
    view = sorted(source, [&](const int& a, const int& b) {
        if (view) {
            CHECK(view->snapshot().size() == view->size());
            if (!nested) {
                nested = true;
                view->set_comparator([](const int& lhs, const int& rhs) { return lhs > rhs; });
                source->emplace_back(3);
            }
        }
        return a < b;
    });
    std::vector<std::shared_ptr<int>> mirror;
    auto sub = view->observe([&](const auto& event) { detail::replay_list_change(mirror, event); });
    source->emplace_back(2);
    source->emplace_back(1);
    REQUIRE(view->size() == 3);
    CHECK(*view->at(0) == 3);
    CHECK(*view->at(1) == 2);
    CHECK(*view->at(2) == 1);
    CHECK(mirror == view->snapshot());
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: comparator failure preserves old state and recovers") {
    auto source = std::make_shared<ObservableList<int>>();
    bool fail = false;
    auto view = sorted(source, [&](const int& a, const int& b) {
        if (fail) { throw std::runtime_error("comparator failed"); }
        return a < b;
    });
    source->emplace_back(3);
    std::vector<std::shared_ptr<int>> mirror = view->snapshot();
    auto sub = view->observe([&](const auto& event) { detail::replay_list_change(mirror, event); });
    fail = true;
    source->emplace_back(1);
    CHECK(view->snapshot() == mirror);
    fail = false;
    source->emplace_back(2);
    REQUIRE(view->size() == 3);
    CHECK(*view->at(0) == 1);
    CHECK(*view->at(1) == 2);
    CHECK(*view->at(2) == 3);
    CHECK(mirror == view->snapshot());
    CHECK_THROWS(view->set_comparator([](const int&, const int&) -> bool {
        throw std::runtime_error("new comparator failed");
    }));
    source->emplace_back(0);
    REQUIRE(view->size() == 4);
    CHECK(*view->at(0) == 0);
    CHECK(mirror == view->snapshot());
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: failed batched projections recover from consumed events only") {
    auto source = std::make_shared<ObservableList<int>>();
    bool filter_fail = true;
    bool sort_fail = true;
    bool map_fail = true;
    auto filtered_view = filtered(source, [&](const int&) {
        if (std::exchange(filter_fail, false)) { throw std::runtime_error("filter once"); }
        return true;
    });
    auto sorted_view = sorted(source, [&](const int& a, const int& b) {
        if (std::exchange(sort_fail, false)) { throw std::runtime_error("sort once"); }
        return a < b;
    });
    auto mapped_view = mapped<int>(source, [&](const int& value) {
        if (std::exchange(map_fail, false)) { throw std::runtime_error("map once"); }
        return std::make_shared<int>(value * 2);
    });
    std::vector<std::shared_ptr<int>> filtered_mirror;
    std::vector<std::shared_ptr<int>> sorted_mirror;
    std::vector<std::shared_ptr<int>> mapped_mirror;
    auto fs = filtered_view->observe([&](const auto& e) { detail::replay_list_change(filtered_mirror, e); });
    auto ss = sorted_view->observe([&](const auto& e) { detail::replay_list_change(sorted_mirror, e); });
    auto ms = mapped_view->observe([&](const auto& e) { detail::replay_list_change(mapped_mirror, e); });
    const std::vector rows{std::make_shared<int>(3), std::make_shared<int>(1), std::make_shared<int>(2)};
    source->insert_range(0, rows.begin(), rows.end());
    CHECK(filtered_view->snapshot() == filtered_mirror);
    CHECK(sorted_view->snapshot() == sorted_mirror);
    CHECK(mapped_view->snapshot() == mapped_mirror);
    REQUIRE(filtered_view->size() == 3);
    REQUIRE(sorted_view->size() == 3);
    REQUIRE(mapped_view->size() == 3);
    CHECK(*sorted_view->at(0) == 1);
    CHECK(*sorted_view->at(2) == 3);
    CHECK(*mapped_view->at(0) == 6);
    CHECK(*mapped_view->at(2) == 4);
}

// Each CHECK expands exception-handling branches inside doctest.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Derived audit: replaced callback captures can read their view on destruction") {
    struct Capture {
        Capture() = default;
        Capture(const Capture&) = delete;
        Capture& operator=(const Capture&) = delete;
        Capture(Capture&&) = delete;
        Capture& operator=(Capture&&) = delete;
        std::function<void()> retire;
        ~Capture() noexcept {
            try { if (retire) { retire(); } }
            catch (...) { std::terminate(); }
        }
    };
    auto source = std::make_shared<ObservableList<int>>();
    source->emplace_back(1);
    int retired = 0;
    auto filter_capture = std::make_shared<Capture>();
    auto filtered_view = filtered(source, [filter_capture](const int&) { return true; });
    filter_capture->retire = [&] { CHECK(filtered_view->size() == 1); ++retired; };
    filter_capture.reset();
    filtered_view->set_predicate([](const int&) { return true; });
    CHECK(retired == 1);
    auto sort_capture = std::make_shared<Capture>();
    auto sorted_view = sorted(source, [sort_capture](const int& a, const int& b) { return a < b; });
    sort_capture->retire = [&] { CHECK(sorted_view->size() == 1); ++retired; };
    sort_capture.reset();
    sorted_view->set_comparator([](const int& a, const int& b) { return a > b; });
    CHECK(retired == 2);
}
