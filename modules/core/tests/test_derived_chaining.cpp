// ============================================================================
//  test_derived_chaining.cpp
// ----------------------------------------------------------------------------
//  Every derived list used to hardcode `std::shared_ptr<ObservableList<T>>` as
//  its source, so a pipeline like FilteredList -> SortedList -> PagedList
//  simply did not compile. `list_source.hpp` had defined the `ListSource`
//  concept for exactly this purpose, but nothing consumed it — D-40 described
//  a capability the code did not have.
//
//  Each derived list now takes its source as a template parameter constrained
//  to `ListSourceOf<Source, T>`, defaulting to `ObservableList<T>` so all
//  existing single-level code is unaffected.
//
//  These cases are the acceptance test for that: they build real multi-stage
//  pipelines, mutate the ROOT list, and assert the change propagates all the
//  way to the far end. A pipeline that compiles but does not propagate would
//  be worse than no pipeline at all.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/derived/distinct_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/grouped_list.hpp"
#include "aria/derived/mapped_list.hpp"
#include "aria/derived/paged_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/observable_list.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace aria;

namespace {

struct Row {
    int value;
    std::string label;
    explicit Row(int v, std::string l = {}) : value(v), label(std::move(l)) {}
};

std::shared_ptr<ObservableList<Row>> make_source(std::initializer_list<int> vs) {
    auto src = std::make_shared<ObservableList<Row>>();
    for (int v : vs) src->push_back(std::make_shared<Row>(v));
    return src;
}

template <typename L>
std::vector<int> values_of(const L& list) {
    std::vector<int> out;
    for (std::size_t i = 0; i < list.size(); ++i) out.push_back(list.at(i)->value);
    return out;
}

}  // namespace

TEST_CASE("Chaining: filtered -> sorted") {
    auto src = make_source({5, 2, 8, 1, 9, 4});

    auto evens = filtered(src, [](const Row& r) { return r.value % 2 == 0; });
    auto desc  = sorted(evens, [](const Row& a, const Row& b) {
        return a.value > b.value;
    });

    CHECK(values_of(*evens) == std::vector<int>{2, 8, 4});
    CHECK(values_of(*desc) == std::vector<int>{8, 4, 2});

    // Mutating the ROOT must flow through both stages.
    src->push_back(std::make_shared<Row>(6));
    CHECK(values_of(*desc) == std::vector<int>{8, 6, 4, 2});

    // An odd number is rejected by stage one and never reaches stage two.
    src->push_back(std::make_shared<Row>(7));
    CHECK(values_of(*desc) == std::vector<int>{8, 6, 4, 2});
}

TEST_CASE("Chaining: filtered -> sorted -> paged") {
    auto src = make_source({5, 2, 8, 1, 9, 4, 6, 10, 3});

    auto evens = filtered(src, [](const Row& r) { return r.value % 2 == 0; });
    auto asc   = sorted(evens, [](const Row& a, const Row& b) {
        return a.value < b.value;
    });
    auto page  = paged(asc, /*page_size=*/2, /*page_index=*/0);

    // evens sorted ascending: 2, 4, 6, 8, 10 -> first page is {2, 4}
    CHECK(values_of(*asc) == std::vector<int>{2, 4, 6, 8, 10});
    CHECK(values_of(*page) == std::vector<int>{2, 4});

    page->page_index().set(1);
    CHECK(values_of(*page) == std::vector<int>{6, 8});

    page->page_index().set(2);
    CHECK(values_of(*page) == std::vector<int>{10});

    // Insert at the root: it sorts to the front, shifting the window content.
    src->push_back(std::make_shared<Row>(0));
    CHECK(values_of(*asc) == std::vector<int>{0, 2, 4, 6, 8, 10});
    page->page_index().set(0);
    CHECK(values_of(*page) == std::vector<int>{0, 2});
}

TEST_CASE("Chaining: filtered -> distinct") {
    auto src = make_source({2, 4, 2, 6, 4, 8});

    auto evens  = filtered(src, [](const Row& r) { return r.value % 2 == 0; });
    auto unique = distinct<int>(evens, [](const Row& r) { return r.value; });

    CHECK(values_of(*unique) == std::vector<int>{2, 4, 6, 8});

    // A duplicate arriving at the root must not reach the distinct view.
    src->push_back(std::make_shared<Row>(6));
    CHECK(values_of(*unique) == std::vector<int>{2, 4, 6, 8});

    // A genuinely new key does.
    src->push_back(std::make_shared<Row>(10));
    CHECK(values_of(*unique) == std::vector<int>{2, 4, 6, 8, 10});
}

TEST_CASE("Chaining: sorted -> mapped changes the element type mid-pipeline") {
    struct Doubled {
        int value;
        explicit Doubled(int v) : value(v) {}
    };

    auto src = make_source({3, 1, 2});
    auto asc = sorted(src, [](const Row& a, const Row& b) {
        return a.value < b.value;
    });
    auto twice = mapped<Doubled>(asc, [](const Row& r) {
        return std::make_shared<Doubled>(r.value * 2);
    });

    CHECK(values_of(*twice) == std::vector<int>{2, 4, 6});

    src->push_back(std::make_shared<Row>(0));
    CHECK(values_of(*twice) == std::vector<int>{0, 2, 4, 6});
}

TEST_CASE("Chaining: filtered -> grouped") {
    auto src = make_source({1, 2, 3, 4, 5, 6, 7, 8});

    // Keep 2..7, then bucket by parity.
    auto mid = filtered(src, [](const Row& r) {
        return r.value >= 2 && r.value <= 7;
    });
    auto by_parity = grouped<int>(mid, [](const Row& r) { return r.value % 2; });

    REQUIRE(by_parity->size() == 2);

    // Groups are keyed 0 (even) and 1 (odd); find each rather than assume order.
    std::size_t evens_total = 0, odds_total = 0;
    for (std::size_t i = 0; i < by_parity->size(); ++i) {
        auto g = by_parity->at(i);
        if (g->key == 0) evens_total = g->items->size();
        else             odds_total = g->items->size();
    }
    CHECK(evens_total == 3);  // 2, 4, 6
    CHECK(odds_total == 3);   // 3, 5, 7
}

TEST_CASE("Chaining: a four-stage pipeline still propagates from the root") {
    auto src = make_source({9, 4, 7, 4, 2, 8, 5, 6, 2});

    auto evens  = filtered(src, [](const Row& r) { return r.value % 2 == 0; });
    auto unique = distinct<int>(evens, [](const Row& r) { return r.value; });
    auto asc    = sorted(unique, [](const Row& a, const Row& b) {
        return a.value < b.value;
    });
    auto page   = paged(asc, /*page_size=*/3, /*page_index=*/0);

    // evens: 4,4,2,8,6,2 -> distinct: 4,2,8,6 -> sorted: 2,4,6,8
    CHECK(values_of(*asc) == std::vector<int>{2, 4, 6, 8});
    CHECK(values_of(*page) == std::vector<int>{2, 4, 6});

    // One insert at the root, visible four stages later.
    src->push_back(std::make_shared<Row>(0));
    CHECK(values_of(*asc) == std::vector<int>{0, 2, 4, 6, 8});
    CHECK(values_of(*page) == std::vector<int>{0, 2, 4});

    // A duplicate is absorbed by `distinct` and changes nothing downstream.
    src->push_back(std::make_shared<Row>(4));
    CHECK(values_of(*asc) == std::vector<int>{0, 2, 4, 6, 8});
    CHECK(values_of(*page) == std::vector<int>{0, 2, 4});
}

TEST_CASE("Chaining: intermediate stages are kept alive by their consumers") {
    // Each derived list holds a strong shared_ptr to its source, so the chain
    // stays alive transitively as long as the far end is held. Drop every
    // local handle except the last and verify the pipeline still works.
    std::shared_ptr<PagedList<Row, SortedList<Row, FilteredList<Row>>>> page;
    std::shared_ptr<ObservableList<Row>> src;

    {
        src = make_source({5, 2, 8, 1, 4});
        auto evens = filtered(src, [](const Row& r) { return r.value % 2 == 0; });
        auto asc   = sorted(evens, [](const Row& a, const Row& b) {
            return a.value < b.value;
        });
        page = paged(asc, 10, 0);
        // `evens` and `asc` go out of scope here; only `page` (and `src`)
        // remain. The intermediate stages must survive.
    }

    CHECK(values_of(*page) == std::vector<int>{2, 4, 8});

    // And the surviving chain must still be wired to the root.
    src->push_back(std::make_shared<Row>(6));
    CHECK(values_of(*page) == std::vector<int>{2, 4, 6, 8});
}

TEST_CASE("Chaining: existing single-level usage is unchanged") {
    // The source parameter defaults to ObservableList<T>, so pre-existing
    // spellings must keep compiling exactly as before.
    auto src = make_source({3, 1, 2});

    FilteredList<Row> f{src, [](const Row& r) { return r.value > 1; }};
    SortedList<Row>   s{src, [](const Row& a, const Row& b) {
        return a.value < b.value;
    }};
    PagedList<Row>    p{src, 2, 0};

    CHECK(values_of(f) == std::vector<int>{3, 2});
    CHECK(values_of(s) == std::vector<int>{1, 2, 3});
    CHECK(values_of(p) == std::vector<int>{3, 1});
}
