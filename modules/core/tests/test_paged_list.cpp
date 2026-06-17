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

#include "aria/derived/paged_list.hpp"
#include "aria/observable_list.hpp"

#include <memory>
#include <vector>

using namespace aria;

namespace {

[[nodiscard]] std::shared_ptr<int> sp(int x) {
    return std::make_shared<int>(x);
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
