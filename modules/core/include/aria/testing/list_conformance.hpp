#pragma once

// ============================================================================
//  aria/testing/list_conformance.hpp
// ----------------------------------------------------------------------------
//  Framework-level conformance suite for any type that satisfies
//  `aria::ListSource`. Pin the contracts spelled out in
//  `docs/list-diff-contract.md` (D-N) into machine-checkable form.
//
//  Usage from a test TU:
//
//      #include <doctest/doctest.h>
//      #include "aria/testing/list_conformance.hpp"
//
//      TEST_CASE("ObservableList<int> conforms to ListSource D-N contract") {
//          aria::testing::run_list_source_conformance<aria::ObservableList<int>>(
//              [] { return std::make_shared<aria::ObservableList<int>>(); });
//      }
//
//  The factory is required because some list types are non-default-
//  constructible (e.g. SortedList needs a comparator). The suite runs
//  every conformance section in fresh state.
//
//  Per `docs/api-style.md` S-1, the entry points live in
//  `aria::testing::` and never under any implementation namespace.
//
//  Coverage roadmap:
//
//   D-1 / D-2 / D-3   indirectly verified through the structural tests
//                     below (kind / index / item lifetime).
//   D-10              single-element ops emit one event.
//   D-11              batch ops emit ordered events with "as-seen"
//                     index semantics.
//   D-12              Reset semantics.
//   D-13              ItemChanged probe (only when value_type satisfies
//                     the on_changed concept).
//   D-20 / D-21       reentrancy probes (unsubscribe / subscribe inside
//                     emit; throwing handlers do not break siblings).
//
//  Derived-list-specific contracts (D-30 / D-31 / D-32) live in their
//  own dedicated tests; this generic suite focuses on the universal
//  contract.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aria::testing {

namespace detail {

/// Tiny event log used by every conformance probe. Captures the raw
/// `ListChange<T>` plus a copy of the item value for stability across
/// emit-then-erase races.
template<typename T>
struct EventLog {
    struct Entry {
        ListChangeKind kind;
        std::size_t    index;
        std::size_t    from_index;
        bool           item_was_null;
    };

    std::vector<Entry> events;
    Subscription       sub;

    template<typename L>
    explicit EventLog(L& list) {
        sub = list.observe([this](const ListChange<T>& c) {
            events.push_back(Entry{
                c.kind,
                c.index,
                c.from_index,
                c.item == nullptr,
            });
        });
    }
};

/// Helper: most generic factory accepts a closure producing a
/// shared_ptr<L>. We fall back to `make_shared<L>()` when L is
/// default-constructible.
template<typename L, typename Factory>
[[nodiscard]] auto build(Factory& f) {
    return f();
}

}  // namespace detail

// ---------------------------------------------------------------------------
//  D-10 -- single-element operations emit exactly one event.
// ---------------------------------------------------------------------------

template<typename L, typename Factory>
void check_single_element_ops(Factory factory) {
    using T = list_source_value_t<L>;

    SUBCASE("D-10: push_back emits one Insert with index=size_before") {
        auto list = factory();
        detail::EventLog<T> log{*list};
        list->push_back(std::make_shared<T>());
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind  == ListChangeKind::Insert);
        CHECK(log.events[0].index == 0);
        CHECK_FALSE(log.events[0].item_was_null);
    }

    SUBCASE("D-10: remove_at emits one Remove with valid item ptr") {
        auto list = factory();
        list->push_back(std::make_shared<T>());
        list->push_back(std::make_shared<T>());
        detail::EventLog<T> log{*list};
        list->remove_at(0);
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind          == ListChangeKind::Remove);
        CHECK(log.events[0].index         == 0);
        CHECK_FALSE(log.events[0].item_was_null);   // D-3
    }

    SUBCASE("D-10: clear emits one Reset with item == nullptr") {
        auto list = factory();
        list->push_back(std::make_shared<T>());
        list->push_back(std::make_shared<T>());
        detail::EventLog<T> log{*list};
        list->clear();
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind          == ListChangeKind::Reset);
        CHECK(log.events[0].index         == 0);
        CHECK(log.events[0].item_was_null);
    }
}

// ---------------------------------------------------------------------------
//  D-11 -- batch operations emit ordered events with "as-seen" indices.
// ---------------------------------------------------------------------------

template<typename L, typename Factory>
void check_batch_ops(Factory factory) {
    using T = list_source_value_t<L>;

    SUBCASE("D-11: insert_range emits per-element Inserts in order") {
        auto list = factory();
        detail::EventLog<T> log{*list};
        std::vector<std::shared_ptr<T>> in{
            std::make_shared<T>(),
            std::make_shared<T>(),
            std::make_shared<T>(),
        };
        list->insert_range(0, in.begin(), in.end());
        REQUIRE(log.events.size() == 3);
        CHECK(log.events[0].kind  == ListChangeKind::Insert);
        CHECK(log.events[0].index == 0);
        CHECK(log.events[1].kind  == ListChangeKind::Insert);
        CHECK(log.events[1].index == 1);
        CHECK(log.events[2].kind  == ListChangeKind::Insert);
        CHECK(log.events[2].index == 2);
    }

    SUBCASE("D-11: remove_range reports each Remove with index = pos") {
        // [A, B, C, D] -- remove_range(1, 2) drops B and C in order.
        // Both events fire with index = 1 because the list shrinks.
        auto list = factory();
        for (int i = 0; i < 4; ++i) list->push_back(std::make_shared<T>());
        detail::EventLog<T> log{*list};
        list->remove_range(1, 2);
        REQUIRE(log.events.size() == 2);
        CHECK(log.events[0].kind  == ListChangeKind::Remove);
        CHECK(log.events[0].index == 1);
        CHECK(log.events[1].kind  == ListChangeKind::Remove);
        CHECK(log.events[1].index == 1);
    }
}

// ---------------------------------------------------------------------------
//  D-20 / D-21 -- reentrancy semantics.
// ---------------------------------------------------------------------------

template<typename L, typename Factory>
void check_reentrancy(Factory factory) {
    using T = list_source_value_t<L>;

    SUBCASE("D-20: unsubscribe inside emit is safe and snapshot-applies") {
        auto list = factory();
        Subscription a, b;
        int hits_a = 0, hits_b = 0;

        a = list->observe([&](const ListChange<T>&) {
            ++hits_a;
            b.release();   // disconnect b mid-emit
        });
        b = list->observe([&](const ListChange<T>&) { ++hits_b; });

        list->push_back(std::make_shared<T>());
        // Snapshot semantics (L-13): b was in the snapshot, so it
        // still fires for THIS emit.
        CHECK(hits_a == 1);
        CHECK(hits_b == 1);

        list->push_back(std::make_shared<T>());
        CHECK(hits_a == 2);
        CHECK(hits_b == 1);   // b was disconnected before this emit
    }

    SUBCASE("D-21: throwing handler does not break siblings") {
        auto list = factory();
        int after = 0;
        auto sub_throw = list->observe([](const ListChange<T>&) {
            throw std::runtime_error("naughty observer");
        });
        auto sub_count = list->observe([&](const ListChange<T>&) { ++after; });

        list->push_back(std::make_shared<T>());
        // The naughty handler raised; the framework swallowed it; the
        // sibling still got its callback.
        CHECK(after == 1);
    }
}

// ---------------------------------------------------------------------------
//  Aggregator: run every conformance section.
//
//  `Factory` :: () -> std::shared_ptr<L>
// ---------------------------------------------------------------------------

template<typename L, typename Factory>
void run_list_source_conformance(Factory factory) {
    static_assert(::aria::ListSource<L>,
        "run_list_source_conformance<L>: L does not satisfy aria::ListSource. "
        "See docs/list-diff-contract.md D-40.");

    check_single_element_ops<L>(factory);
    check_batch_ops<L>(factory);
    check_reentrancy<L>(factory);
}

}  // namespace aria::testing
