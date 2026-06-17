// ============================================================================
//  test_list_signal_mixin.cpp
// ----------------------------------------------------------------------------
//  Pin down the observe() / on_any_change() / observer_count() triple
//  contract supplied by detail::ListSignalMixin to every observable list
//  type. This test exists for one purpose: if a future refactor accidentally
//  drops the mixin from any of the seven lists (or breaks one of the three
//  contract points), this file fails to compile or fails to pass.
//
//  Each list is exercised through three orthogonal invariants:
//
//   I1.  observe(...) returns a Subscription that, while alive, delivers
//        exactly one ListChange<E> per upstream change.
//   I2.  on_any_change(...) returns a Subscription that fires once per
//        change with no payload — and counts toward observer_count() the
//        same way observe(...) does.
//   I3.  observer_count() reflects the number of currently-connected
//        slots (sum of observe + on_any_change), drops back to zero when
//        every Subscription is released, and never under- or over-counts.
//
//  We deliberately do NOT cover list-specific semantics here (filtering,
//  sorting, paging, …) — those have their own per-list test files. This
//  file only verifies the *uniform observation surface* that the mixin
//  was extracted to provide.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/observable_list.hpp"
#include "aria/derived/distinct_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/grouped_list.hpp"
#include "aria/derived/mapped_list.hpp"
#include "aria/derived/paged_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/subscription.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {

// ─── Helpers ────────────────────────────────────────────────────────────────

template<typename List, typename Element>
void check_observation_contract(List& list,
                                 auto&& trigger_one_change,
                                 const char* tag) {
    INFO("list under test: ", tag);

    // Baseline: a freshly-constructed list has no observers.
    REQUIRE(list.observer_count() == 0);

    // I1. observe() — every change is delivered exactly once.
    int observed_calls = 0;
    {
        auto sub = list.observe(
            [&observed_calls](const aria::ListChange<Element>&) {
                ++observed_calls;
            });
        REQUIRE(list.observer_count() == 1);

        trigger_one_change();
        CHECK(observed_calls == 1);

        // Subscription has not been released yet, so the count stays at 1.
        REQUIRE(list.observer_count() == 1);
    }
    // After scope exit the Subscription must drop the slot.
    CHECK(list.observer_count() == 0);

    // I2. on_any_change() — fires once per change with no payload, and
    // counts toward observer_count() the same way observe() does.
    int any_calls = 0;
    {
        auto sub = list.on_any_change([&any_calls] { ++any_calls; });
        REQUIRE(list.observer_count() == 1);

        trigger_one_change();
        CHECK(any_calls == 1);
    }
    CHECK(list.observer_count() == 0);

    // I3. observer_count() additivity — observe + on_any_change.
    {
        auto a = list.observe([](const aria::ListChange<Element>&) {});
        auto b = list.on_any_change([] {});
        CHECK(list.observer_count() == 2);
        auto c = list.observe([](const aria::ListChange<Element>&) {});
        CHECK(list.observer_count() == 3);
    }
    CHECK(list.observer_count() == 0);
}

}  // namespace

// ─── ObservableList ─────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: ObservableList<int>") {
    auto list = std::make_shared<aria::ObservableList<int>>();
    check_observation_contract<decltype(*list), int>(
        *list,
        [&] { list->push_back(std::make_shared<int>(42)); },
        "ObservableList<int>");
}

// ─── FilteredList ───────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: FilteredList<int>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    aria::FilteredList<int> filtered(src, [](const int& x) { return x > 0; });
    check_observation_contract<decltype(filtered), int>(
        filtered,
        [&] { src->push_back(std::make_shared<int>(7)); },
        "FilteredList<int>");
}

// ─── SortedList ─────────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: SortedList<int>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    aria::SortedList<int> sorted(src,
        [](const int& a, const int& b) { return a < b; });
    check_observation_contract<decltype(sorted), int>(
        sorted,
        [&] { src->push_back(std::make_shared<int>(3)); },
        "SortedList<int>");
}

// ─── MappedList ─────────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: MappedList<int, std::string>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    aria::MappedList<int, std::string> mapped(src,
        [](const int& x) { return std::make_shared<std::string>(std::to_string(x)); });
    check_observation_contract<decltype(mapped), std::string>(
        mapped,
        [&] { src->push_back(std::make_shared<int>(1)); },
        "MappedList<int, std::string>");
}

// ─── DistinctList ───────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: DistinctList<int>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    aria::DistinctList<int> distinct(src);
    int next = 0;
    check_observation_contract<decltype(distinct), int>(
        distinct,
        // Each push uses a fresh, never-seen-before key so the distinct
        // semantics admit every change as an Insert on the derived list.
        [&] { src->push_back(std::make_shared<int>(++next)); },
        "DistinctList<int>");
}
// ─── PagedList ──────────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: PagedList<int>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    aria::PagedList<int> paged(src, /*page_size=*/4, /*page_index=*/0);
    check_observation_contract<decltype(paged), int>(
        paged,
        [&] { src->push_back(std::make_shared<int>(99)); },
        "PagedList<int>");
}

// ─── GroupedList ─────────────────────────────────────────────────────────

TEST_CASE("ListSignalMixin contract: GroupedList<int>") {
    auto src = std::make_shared<aria::ObservableList<int>>();
    // Use the value itself as the group key, so every distinct value the
    // trigger pushes manifests as an outer-list Insert event — which is
    // what the mixin's observers see. (If we used `x % 2` here, the
    // second push of an even or odd value would only mutate an inner
    // bucket and would not surface to the outer list, which is correct
    // GroupedList semantics but is not what this contract test wants to
    // exercise.)
    aria::GroupedList<int> grouped(src,
        [](const int& x) { return x; });
    using Element = aria::Group<int, int>;
    int next = 0;
    check_observation_contract<decltype(grouped), Element>(
        grouped,
        [&] { src->push_back(std::make_shared<int>(++next)); },
        "GroupedList<int>");
}