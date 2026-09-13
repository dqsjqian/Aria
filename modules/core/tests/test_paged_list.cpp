// ============================================================================
//  test_paged_list.cpp
// ----------------------------------------------------------------------------
//  Pin down the PG-N invariants laid out in
//  modules/core/include/aria/derived/paged_list.hpp:
//
//    PG-1 window over source slice
//    PG-2 page_index / page_size live properties
//    PG-3 source-driven update propagates to window
//    PG-4 page_count / is_last_page
//    PG-5 source-destroyed lifetime safety
// ============================================================================

#include <doctest/doctest.h>

#include "aria/derived/mapped_list.hpp"
#include "aria/derived/paged_list.hpp"
#include "aria/observable_list.hpp"

#include <memory>
#include <limits>
#include <utility>
#include <vector>

using namespace aria;

namespace {

[[nodiscard]] std::shared_ptr<int> sp(int x) {
    return std::make_shared<int>(x);
}

// Item notifications can be synchronous even inside a reactive graph batch.
struct NotifyingInt {
    int value;
    detail::TypedSignal<NotifyingInt> changed;

    explicit NotifyingInt(int initial) : value(initial) {}
    operator int() const { return value; }
    Subscription on_changed(std::function<void(const NotifyingInt&)> fn) {
        return changed.connect(std::move(fn));
    }
    void set(int next) { value = next; changed.emit(*this); }
};

template<typename List>
[[nodiscard]] std::vector<int> values_of(const List& list) {
    std::vector<int> values;
    for (const auto& item : list.snapshot()) values.push_back(*item);
    return values;
}

// Model an adapter that consumes the diff without reloading its snapshot.
template<typename T, typename Source>
[[nodiscard]] Subscription replay_diff(PagedList<T, Source>& page,
                                       std::vector<int>& mirror) {
    return page.observe([&mirror](const ListChange<T>& change) {
        const auto pos = static_cast<std::ptrdiff_t>(change.index);
        switch (change.kind) {
        case ListChangeKind::Insert:
            CHECK(change.index <= mirror.size());
            if (change.index > mirror.size()) return;
            mirror.insert(mirror.begin() + pos, *change.item);
            break;
        case ListChangeKind::Remove:
            CHECK(change.index < mirror.size());
            if (change.index >= mirror.size()) return;
            CHECK(mirror[change.index] == *change.item);
            mirror.erase(mirror.begin() + pos);
            break;
        case ListChangeKind::Move: {
            CHECK(change.from_index < mirror.size());
            CHECK(change.index < mirror.size());
            if (change.from_index >= mirror.size() ||
                change.index >= mirror.size()) return;
            const int moved = mirror[change.from_index];
            CHECK(moved == *change.item);
            mirror.erase(mirror.begin()
                         + static_cast<std::ptrdiff_t>(change.from_index));
            mirror.insert(mirror.begin() + pos, moved);
            break;
        }
        case ListChangeKind::Replace:
        case ListChangeKind::ItemChanged:
            CHECK(change.index < mirror.size());
            if (change.index < mirror.size()) mirror[change.index] = *change.item;
            break;
        default:
            CHECK_MESSAGE(false, "Re-windowing should not emit Reset");
            break;
        }
    });
}

}  // namespace

// ----------------------------------------------------------------------------
//  PG-1: initial window mirrors the source slice
// ----------------------------------------------------------------------------
TEST_CASE("PG-1: initial window covers [page*size, (page+1)*size)") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 25; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, /*size=*/10, /*page=*/1};   // page 1 -> [10..20)
    REQUIRE(pl.size() == 10);
    auto snap = pl.snapshot();
    CHECK(*snap.front() == 10);
    CHECK(*snap.back()  == 19);
}

TEST_CASE("PG-1: last page may be short") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 25; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 10, 2};   // page 2 -> [20..25)
    REQUIRE(pl.size() == 5);
    CHECK(*pl.at(0) == 20);
    CHECK(*pl.at(4) == 24);
}

// ----------------------------------------------------------------------------
//  PG-2: page_index / page_size are live
// ----------------------------------------------------------------------------
TEST_CASE("PG-2: changing page_index re-windows synchronously") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 30; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 10, 0};
    REQUIRE(*pl.at(0) == 0);

    std::vector<ListChange<int>> log;
    auto sub = pl.observe([&](const ListChange<int>& c) { log.push_back(c); });

    pl.page_index().set(2);
    CHECK(*pl.at(0) == 20);
    CHECK_FALSE(log.empty());
    // No full Reset for a window slide -- the diff is composed of
    // Insert / Remove only.
    for (const auto& ev : log) {
        CHECK(ev.kind != ListChangeKind::Reset);
    }
}

TEST_CASE("PG-2: changing page_size re-windows synchronously") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 30; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 10, 0};
    pl.page_size().set(5);
    CHECK(pl.size() == 5);
    CHECK(*pl.at(0) == 0);
    CHECK(*pl.at(4) == 4);
}

// ----------------------------------------------------------------------------
//  PG-3: source-driven update inside the window
// ----------------------------------------------------------------------------
TEST_CASE("PG-3: appending past the window does not change current page") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 10; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 5, 0};
    REQUIRE(pl.size() == 5);

    std::vector<ListChange<int>> log;
    auto sub = pl.observe([&](const ListChange<int>& c) { log.push_back(c); });

    src->push_back(sp(99));   // index 10 -- way past window
    CHECK(log.empty());
    CHECK(pl.size() == 5);
    CHECK(*pl.at(0) == 0);
}

TEST_CASE("PG-3/PG-4: outside tail batches remain available to later pages and refills") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 4; ++i) src->emplace_back(i);
    auto page = paged(src, 2, 1);
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);
    int changes = 0;
    auto count = page->on_any_change([&] { ++changes; });
    REQUIRE(page->is_last_page());

    std::vector<std::shared_ptr<int>> incoming{sp(4), sp(5), sp(6)};
    src->insert_range(src->size(), incoming.begin(), incoming.end());
    CHECK(changes == 0);
    CHECK(page->page_count() == 4);
    CHECK_FALSE(page->is_last_page());
    CHECK(mirror == std::vector<int>{2, 3});

    src->remove_range(0, 2);
    CHECK(values_of(*page) == std::vector<int>{4, 5});
    CHECK(mirror == values_of(*page));
    page->page_index().set(2);
    CHECK(values_of(*page) == std::vector<int>{6});
    CHECK(mirror == values_of(*page));
    CHECK(page->is_last_page());
}

TEST_CASE("PG-2/PG-3: a tail insert applies page parameters pending in a reactive batch") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 6; ++i) src->emplace_back(i);
    std::size_t initial_size = 2;
    std::size_t initial_index = 0;
    SUBCASE("pending page index") {}
    SUBCASE("pending page size with an equally long cached window") {
        initial_size = 4;
        initial_index = 1;  // short page [4, 5] is also the new full page length
    }
    auto page = paged(src, initial_size, initial_index);
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);
    const auto before = mirror;
    const std::vector<int> expected{2, 3};
    reactive::batch([&] {
        page->page_index().set(1);
        page->page_size().set(2);
        REQUIRE(values_of(*page) == before);
        src->emplace_back(6);
        CHECK(values_of(*page) == expected);
        CHECK(mirror == expected);
    });
    CHECK(values_of(*page) == expected);
    CHECK(mirror == expected);
}

TEST_CASE("PG-3: reentrant outside append preserves complete diff order") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 4; ++i) src->emplace_back(i);
    auto page = paged(src, 2);
    using Event = std::pair<ListChangeKind, int>;
    std::vector<Event> first, second;
    bool reentered = false;
    auto reenter = page->observe([&](const ListChange<int>& change) {
        first.emplace_back(change.kind, *change.item);
        if (!std::exchange(reentered, true)) {
            src->emplace_back(4);
            page->page_index().set(1);
        }
    });
    auto record = page->observe([&](const ListChange<int>& change) {
        second.emplace_back(change.kind, *change.item);
    });
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);

    src->insert(0, sp(-1));
    REQUIRE(reentered);
    CHECK(first == second);
    CHECK(first.size() == 6);
    CHECK(values_of(*page) == std::vector<int>{1, 2});
    CHECK(mirror == values_of(*page));
    CHECK(page->page_count() == 3);
    page->page_index().set(2);
    CHECK(values_of(*page) == std::vector<int>{3, 4});
    CHECK(mirror == values_of(*page));
}

TEST_CASE("PG-3: tail insertions fill an absent and then short page") {
    auto src = std::make_shared<ObservableList<int>>();
    src->emplace_back(0);
    auto page = paged(src, 2, 1);
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);
    int changes = 0;
    auto count = page->on_any_change([&] { ++changes; });
    src->emplace_back(1);
    CHECK(page->empty());
    src->emplace_back(2);
    CHECK(values_of(*page) == std::vector<int>{2});
    src->emplace_back(3);
    CHECK(values_of(*page) == std::vector<int>{2, 3});
    src->emplace_back(4);
    CHECK(changes == 2);
    CHECK(mirror == std::vector<int>{2, 3});
    CHECK(page->page_count() == 3);
}

TEST_CASE("PG-2/PG-3: content events apply a pending page change before refreshing") {
    auto src = std::make_shared<ObservableList<NotifyingInt>>();
    for (int i = 0; i < 4; ++i) src->emplace_back(i);
    auto page = paged(src, 2);
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);
    std::vector<ListChangeKind> events;
    auto record = page->observe([&](const auto& change) { events.push_back(change.kind); });
    ListChangeKind refresh = ListChangeKind::Replace;
    SUBCASE("Replace") {}
    SUBCASE("ItemChanged") { refresh = ListChangeKind::ItemChanged; }

    reactive::batch([&] {
        page->page_index().set(1);
        if (refresh == ListChangeKind::Replace)
            src->replace_at(3, std::make_shared<NotifyingInt>(9));
        else
            src->at(3)->set(9);
        CHECK(values_of(*page) == std::vector<int>{2, 9});
        CHECK(mirror == values_of(*page));
        REQUIRE(events.size() == 5);
        CHECK(events == std::vector<ListChangeKind>{ListChangeKind::Remove,
            ListChangeKind::Remove, ListChangeKind::Insert, ListChangeKind::Insert, refresh});
    });
    CHECK(mirror == values_of(*page));
}

TEST_CASE("PG-2/PG-3: a changed page size retains content refresh for identical handles") {
    auto src = std::make_shared<ObservableList<NotifyingInt>>();
    src->emplace_back(0); src->emplace_back(1);
    auto page = paged(src, 3);
    auto mirror = values_of(*page);
    auto replay = replay_diff(*page, mirror);
    std::vector<ListChangeKind> events;
    auto record = page->observe([&](const auto& change) { events.push_back(change.kind); });
    ListChangeKind refresh = ListChangeKind::Replace;
    SUBCASE("Replace with the same handle") {}
    SUBCASE("ItemChanged") { refresh = ListChangeKind::ItemChanged; }

    reactive::batch([&] {
        page->page_size().set(2); // same two handles, different applied parameter
        auto item = src->at(1);
        if (refresh == ListChangeKind::Replace) {
            item->value = 9;
            src->replace_at(1, item);
        } else {
            item->set(9);
        }
        CHECK(mirror == std::vector<int>{0, 9});
        CHECK(events == std::vector<ListChangeKind>{refresh});
    });
    CHECK(mirror == values_of(*page));
}

TEST_CASE("PG-3: removing in front of the window pulls the next item in") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 10; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 5, 1};   // window = [5..10)
    REQUIRE(*pl.at(0) == 5);

    std::vector<ListChange<int>> log;
    auto sub = pl.observe([&](const ListChange<int>& c) { log.push_back(c); });

    src->remove_at(0);   // drop 0; window now starts at source[5] which is 6
    CHECK(*pl.at(0) == 6);
    CHECK_FALSE(log.empty());
}

TEST_CASE("PG-3: moves within a full page preserve downstream rows") {
    for (const auto& [from, to] : {
             std::pair<std::size_t, std::size_t>{3, 0}, {0, 3}, {1, 2}}) {
        CAPTURE(from);
        CAPTURE(to);
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < 4; ++i) src->push_back(sp(i));
        auto page = paged(src, 4);
        auto projection = mapped<int>(page, [](int value) { return sp(value); });
        const auto original_targets = projection->snapshot();
        auto mirror = values_of(*page);
        auto sub = replay_diff(*page, mirror);

        src->move(from, to);

        CHECK(mirror == values_of(*page));
        CHECK(values_of(*projection) == values_of(*page));
        CHECK(projection->size() == 4);
        for (const auto& target : projection->snapshot()) {
            CHECK(target == original_targets[static_cast<std::size_t>(*target)]);
        }
    }
}

TEST_CASE("PG-3: moves across either page boundary keep downstream in sync") {
    for (const auto& [from, to] : {
             std::pair<std::size_t, std::size_t>{0, 5}, {5, 0}, {7, 3},
             {3, 7}, {0, 7}, {7, 0}}) {
        CAPTURE(from);
        CAPTURE(to);
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < 8; ++i) src->push_back(sp(i));
        auto page = paged(src, 3, 1);
        auto projection = mapped<int>(page, [](int value) { return sp(value); });
        auto mirror = values_of(*page);
        auto sub = replay_diff(*page, mirror);

        src->move(from, to);

        const auto source_values = values_of(*src);
        const std::vector<int> expected(source_values.begin() + 3,
                                        source_values.begin() + 6);
        CHECK(values_of(*page) == expected);
        CHECK(mirror == expected);
        CHECK(values_of(*projection) == expected);
    }
}

TEST_CASE("PG-2: re-windowing preserves duplicate handle counts downstream") {
    auto src = std::make_shared<ObservableList<int>>();
    auto repeated = sp(1);
    src->push_back(repeated);
    src->push_back(sp(2));
    src->push_back(repeated);
    auto page = paged(src, 3);
    auto projection = mapped<int>(page, [](int value) { return sp(value); });
    auto mirror = values_of(*page);
    auto sub = replay_diff(*page, mirror);

    page->page_size().set(2);
    CHECK(mirror == std::vector<int>{1, 2});
    CHECK(values_of(*projection) == mirror);

    page->page_index().set(1);
    CHECK(mirror == std::vector<int>{1});
    CHECK(values_of(*projection) == mirror);

    page->page_index().set(0);
    page->page_size().set(3);
    CHECK(mirror == std::vector<int>{1, 2, 1});
    CHECK(values_of(*projection) == mirror);
}

// ----------------------------------------------------------------------------
//  PG-4: page_count / is_last_page
// ----------------------------------------------------------------------------
TEST_CASE("PG-4: page_count uses ceil division") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 23; ++i) src->push_back(sp(i));

    PagedList<int> pl{src, 10, 0};
    CHECK(pl.page_count() == 3);   // 23 / 10 ceil = 3
    CHECK_FALSE(pl.is_last_page());

    pl.page_index().set(2);
    CHECK(pl.is_last_page());
}

TEST_CASE("PG-4: empty source -> 0 pages") {
    auto src = std::make_shared<ObservableList<int>>();
    PagedList<int> pl{src, 10, 0};
    CHECK(pl.page_count() == 0);
    CHECK(pl.is_last_page());
    CHECK(pl.empty());
}

// ----------------------------------------------------------------------------
//  PG-5: outliving the source is safe
// ----------------------------------------------------------------------------
TEST_CASE("PG-5: outliving the source is safe (weak source observer)") {
    std::shared_ptr<PagedList<int>> pl;
    {
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < 10; ++i) src->push_back(sp(i));
        pl = std::make_shared<PagedList<int>>(src, 5, 0);
        REQUIRE(pl->size() == 5);
    }
    // Source dropped; PagedList still answers from its cached
    // window vector.
    CHECK(pl->size() == 5);
    CHECK(*pl->at(0) == 0);
}

TEST_CASE("PG-5: destruction by an earlier source observer skips a stale callback") {
    auto src = std::make_shared<ObservableList<int>>();
    src->push_back(sp(1));
    std::shared_ptr<PagedList<int>> page;
    auto earlier = src->observe([&](const auto&) { page.reset(); });
    page = paged(src, 2);
    src->push_back(sp(2));
    CHECK_FALSE(page);
}

TEST_CASE("PG-5: destruction during a multi-event update keeps pending events alive") {
    auto src = std::make_shared<ObservableList<int>>();
    src->push_back(sp(1));
    src->push_back(sp(2));
    auto page = paged(src, 2);
    int events = 0;
    auto sub = page->observe([&](const auto&) { ++events; page.reset(); });
    src->insert(0, sp(0));
    CHECK_FALSE(page);
    CHECK(events == 2);
}

TEST_CASE("PG-3: range changes replay each intermediate source state") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 5; ++i) src->emplace_back(i);
    auto page = paged(src, 2);
    auto next = mapped<int>(page, [](int n) { return sp(n); });
    auto mirror = values_of(*page);
    auto sub = replay_diff(*page, mirror);
    src->remove_range(0, 2);
    CHECK(values_of(*page) == std::vector<int>{2, 3});
    CHECK(values_of(*next) == std::vector<int>{2, 3});
    CHECK(mirror == values_of(*page));
    std::vector<std::shared_ptr<int>> incoming{sp(8), sp(9), sp(10)};
    src->insert_range(0, incoming.begin(), incoming.end());
    CHECK(values_of(*page) == std::vector<int>{8, 9});
    CHECK(values_of(*next) == values_of(*page));
    CHECK(mirror == values_of(*page));
}

TEST_CASE("PG-1/PG-4: large page parameters never wrap or access an absent page") {
    auto src = std::make_shared<ObservableList<int>>();
    src->emplace_back(1); src->emplace_back(2);
    const auto largest = std::numeric_limits<std::size_t>::max();
    PagedList<int> page{src, largest};
    CHECK(page.page_count() == 1);
    CHECK(page.size() == 2);
    page.page_index().set(largest);
    CHECK(page.empty());
    CHECK(page.is_last_page());
    page.page_size().set(2);
    page.page_index().set(largest / 2 + 1);
    CHECK(page.empty());
    CHECK_NOTHROW(src->insert(0, sp(3)));
    CHECK(page.empty());
}
