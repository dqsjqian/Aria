#include <doctest/doctest.h>

#include "aria/derived/filtered_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/property.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace aria;

namespace {

// Plain struct — no on_changed, so only Insert/Remove/Replace/Move/Reset
// reach the derived side.
struct Plain {
    int value;
};

// Reactive struct — exposes on_changed so the source propagates
// ItemChanged.
//
// Implementation note: we use Property::bind(), which fires the
// callback once synchronously at subscribe time. This used to
// deadlock against ObservableList::push_back's write lock; the
// fix (subscribe outside the write lock) makes this
// safe and is regression-tested in test_observable_list.cpp.
struct Reactive {
    Property<int> v{0};
    [[nodiscard]] Subscription on_changed(std::function<void(const Reactive&)> fn) {
        return v.bind([this, f = std::move(fn)](const int&){ f(*this); });
    }
};

// Collector of derived events — a straightforward test helper.
template<typename T>
struct EventLog {
    std::vector<ListChange<T>> events;
    Subscription sub;
    explicit EventLog(FilteredList<T>& fl) {
        sub = fl.observe([this](const ListChange<T>& ch){
            events.push_back(ch);
        });
    }
    std::size_t count(ListChangeKind k) const {
        std::size_t n = 0;
        for (auto& e : events) if (e.kind == k) ++n;
        return n;
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// Construction from existing source
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: initial snapshot honours predicate") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{-2}));
    src->push_back(std::make_shared<Plain>(Plain{3}));

    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    CHECK(fl.size() == 2);
    CHECK(fl.at(0)->value == 1);
    CHECK(fl.at(1)->value == 3);
}

TEST_CASE("FilteredList: empty source -> empty derived, no events") {
    auto src = std::make_shared<ObservableList<Plain>>();
    FilteredList<Plain> fl{src, [](const Plain&){ return true; }};
    EventLog<Plain> log{fl};
    CHECK(fl.size() == 0);
    CHECK(log.events.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// Insert contract
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: Insert that passes predicate emits Insert") {
    auto src = std::make_shared<ObservableList<Plain>>();
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->push_back(std::make_shared<Plain>(Plain{7}));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 0);
    CHECK(fl.size() == 1);
    CHECK(fl.at(0)->value == 7);
}

TEST_CASE("FilteredList: Insert that fails predicate emits nothing") {
    auto src = std::make_shared<ObservableList<Plain>>();
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->push_back(std::make_shared<Plain>(Plain{-5}));
    CHECK(log.events.empty());
    CHECK(fl.size() == 0);
}

TEST_CASE("FilteredList: Insert in the middle keeps derived indices correct") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{10}));
    src->push_back(std::make_shared<Plain>(Plain{-1}));
    src->push_back(std::make_shared<Plain>(Plain{30}));

    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    // Insert at source position 2 (between -1 and 30), passes filter.
    src->insert(2, std::make_shared<Plain>(Plain{20}));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 1);       // derived index: between 10 and 30

    CHECK(fl.size() == 3);
    CHECK(fl.at(0)->value == 10);
    CHECK(fl.at(1)->value == 20);
    CHECK(fl.at(2)->value == 30);
}

// ═══════════════════════════════════════════════════════════════════════
// Remove contract
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: Remove of in-filter item emits Remove") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{2}));
    FilteredList<Plain> fl{src, [](const Plain&){ return true; }};
    EventLog<Plain> log{fl};

    src->remove_at(0);
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 0);
    CHECK(fl.size() == 1);
    CHECK(fl.at(0)->value == 2);
}

TEST_CASE("FilteredList: Remove of out-of-filter item emits nothing") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{-2}));  // filtered out
    src->push_back(std::make_shared<Plain>(Plain{3}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->remove_at(1);
    CHECK(log.events.empty());
    CHECK(fl.size() == 2);
    CHECK(fl.at(0)->value == 1);
    CHECK(fl.at(1)->value == 3);
}

// ═══════════════════════════════════════════════════════════════════════
// Replace contract (all four transition quadrants)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: Replace in→in emits Replace") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{5}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->replace_at(0, std::make_shared<Plain>(Plain{9}));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Replace);
    CHECK(log.events[0].index == 0);
    CHECK(fl.at(0)->value == 9);
}

TEST_CASE("FilteredList: Replace in→out emits Remove") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{5}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->replace_at(0, std::make_shared<Plain>(Plain{-9}));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(fl.size() == 0);
}

TEST_CASE("FilteredList: Replace out→in emits Insert") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{-5}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->replace_at(0, std::make_shared<Plain>(Plain{9}));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 0);
    CHECK(fl.at(0)->value == 9);
}

TEST_CASE("FilteredList: Replace out→out emits nothing") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{-5}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    src->replace_at(0, std::make_shared<Plain>(Plain{-9}));
    CHECK(log.events.empty());
    CHECK(fl.size() == 0);
}

// ═══════════════════════════════════════════════════════════════════════
// ItemChanged contract — requires a T with on_changed
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: ItemChanged in→in emits ItemChanged") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto r = std::make_shared<Reactive>();
    r->v.set(10);
    src->push_back(r);
    FilteredList<Reactive> fl{src, [](const Reactive& x){ return x.v.get() > 0; }};
    EventLog<Reactive> log{fl};

    r->v.set(20);
    // At least one ItemChanged
    CHECK(log.count(ListChangeKind::ItemChanged) >= 1);
    CHECK(log.count(ListChangeKind::Insert) == 0);
    CHECK(log.count(ListChangeKind::Remove) == 0);
    CHECK(fl.at(0)->v.get() == 20);
}

TEST_CASE("FilteredList: ItemChanged in→out emits Remove") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto r = std::make_shared<Reactive>();
    r->v.set(10);
    src->push_back(r);
    FilteredList<Reactive> fl{src, [](const Reactive& x){ return x.v.get() > 0; }};
    EventLog<Reactive> log{fl};

    r->v.set(-1);
    CHECK(log.count(ListChangeKind::Remove) >= 1);
    CHECK(fl.size() == 0);
}

TEST_CASE("FilteredList: ItemChanged out→in emits Insert") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto r = std::make_shared<Reactive>();
    r->v.set(-10);
    src->push_back(r);
    FilteredList<Reactive> fl{src, [](const Reactive& x){ return x.v.get() > 0; }};
    EventLog<Reactive> log{fl};

    r->v.set(5);
    CHECK(log.count(ListChangeKind::Insert) >= 1);
    CHECK(fl.size() == 1);
    CHECK(fl.at(0)->v.get() == 5);
}

// ═══════════════════════════════════════════════════════════════════════
// Move contract
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: Move of in-filter items emits Move with derived coords") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));   // d=0
    src->push_back(std::make_shared<Plain>(Plain{-2}));  // not in
    src->push_back(std::make_shared<Plain>(Plain{3}));   // d=1
    src->push_back(std::make_shared<Plain>(Plain{4}));   // d=2
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    // Move source index 0 → 3. In source: values become [-2, 3, 4, 1].
    // Derived should now be [3, 4, 1] — the 1 moved to the end.
    src->move(0, 3);
    CHECK(log.count(ListChangeKind::Move) == 1);
    CHECK(fl.at(0)->value == 3);
    CHECK(fl.at(1)->value == 4);
    CHECK(fl.at(2)->value == 1);
}

TEST_CASE("FilteredList: Move of out-of-filter item emits nothing") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{-2}));
    src->push_back(std::make_shared<Plain>(Plain{3}));
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    EventLog<Plain> log{fl};

    // Move the filtered-out item around.
    src->move(1, 2);
    CHECK(log.events.empty());
    // Derived view is unchanged.
    CHECK(fl.size() == 2);
    CHECK(fl.at(0)->value == 1);
    CHECK(fl.at(1)->value == 3);
}

// ═══════════════════════════════════════════════════════════════════════
// Reset
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: source Reset propagates as Reset, no splatter") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{2}));
    src->push_back(std::make_shared<Plain>(Plain{3}));
    FilteredList<Plain> fl{src, [](const Plain&){ return true; }};
    EventLog<Plain> log{fl};

    src->clear();
    CHECK(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Reset);
    CHECK(fl.size() == 0);
}

// ═══════════════════════════════════════════════════════════════════════
// set_predicate: incremental diff
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: set_predicate emits only membership-change events") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{2}));
    src->push_back(std::make_shared<Plain>(Plain{3}));
    src->push_back(std::make_shared<Plain>(Plain{4}));

    FilteredList<Plain> fl{src, [](const Plain&){ return true; }};
    EventLog<Plain> log{fl};

    // New predicate keeps only evens.
    fl.set_predicate([](const Plain& p){ return p.value % 2 == 0; });

    // Two items transitioned out: 1, 3. No Reset.
    CHECK(log.count(ListChangeKind::Reset)  == 0);
    CHECK(log.count(ListChangeKind::Remove) == 2);
    CHECK(log.count(ListChangeKind::Insert) == 0);
    CHECK(fl.size() == 2);
    CHECK(fl.at(0)->value == 2);
    CHECK(fl.at(1)->value == 4);
}

TEST_CASE("FilteredList: set_predicate emits Insert for newly-in items") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));
    src->push_back(std::make_shared<Plain>(Plain{-2}));
    src->push_back(std::make_shared<Plain>(Plain{-3}));
    src->push_back(std::make_shared<Plain>(Plain{4}));

    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value > 0; }};
    REQUIRE(fl.size() == 2);
    EventLog<Plain> log{fl};

    fl.set_predicate([](const Plain&){ return true; });

    CHECK(log.count(ListChangeKind::Reset)  == 0);
    CHECK(log.count(ListChangeKind::Remove) == 0);
    CHECK(log.count(ListChangeKind::Insert) == 2);
    CHECK(fl.size() == 4);
}

// ═══════════════════════════════════════════════════════════════════════
// set_predicate: D-11 "as observed" index compliance
//
// The cases above assert only how MANY events fire. That is not enough:
// an observer rebuilds its mirror from the (kind, index) pairs, so a
// correct event count with wrong indices still corrupts every UI bound to
// the list — and silently, because no later event repairs it.
//
// `Mirror` below is exactly the incremental observer D-11 promises to
// support: apply each event to a local vector at the index carried by that
// event. After the dust settles the mirror must equal the derived list. If
// it does not, the emitted stream was lying.
//
// Regression: set_predicate used to emit Remove with the OLD derived index
// and Insert with the NEW one. For [A,B,C] all-passing, narrowing to "only
// C" emitted Remove(0), Remove(1) — mirror [A,B,C] -> [B,C] -> [B] while
// the real list is [C].
// ═══════════════════════════════════════════════════════════════════════

namespace {

// Incremental observer per D-11: index is "as observed at that emit".
template<typename T>
struct Mirror {
    std::vector<const T*> items;
    Subscription          sub;

    explicit Mirror(FilteredList<T>& fl) {
        // Seed from the current state, then track events.
        for (std::size_t i = 0; i < fl.size(); ++i) items.push_back(fl.at(i).get());
        sub = fl.observe([this](const ListChange<T>& ch) {
            switch (ch.kind) {
                case ListChangeKind::Insert:
                    REQUIRE(ch.index <= items.size());
                    items.insert(items.begin() + static_cast<std::ptrdiff_t>(ch.index),
                                 ch.item);
                    break;
                case ListChangeKind::Remove:
                    REQUIRE(ch.index < items.size());
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(ch.index));
                    break;
                case ListChangeKind::Replace:
                    REQUIRE(ch.index < items.size());
                    items[ch.index] = ch.item;
                    break;
                case ListChangeKind::Move: {
                    REQUIRE(ch.from_index < items.size());
                    REQUIRE(ch.index < items.size());
                    const T* moved = items[ch.from_index];
                    items.erase(items.begin() +
                                static_cast<std::ptrdiff_t>(ch.from_index));
                    items.insert(items.begin() +
                                 static_cast<std::ptrdiff_t>(ch.index), moved);
                    break;
                }
                case ListChangeKind::Reset:
                    items.clear();
                    break;
                default:
                    break;  // ItemChanged etc. carry no structural meaning
            }
        });
    }

    // Compare the mirror against the authoritative derived list.
    void check_matches(FilteredList<T>& fl) const {
        CHECK(items.size() == fl.size());
        const std::size_t n = std::min(items.size(), fl.size());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(items[i] == fl.at(i).get());
        }
    }
};

}  // namespace

TEST_CASE("FilteredList: set_predicate narrowing keeps observer mirror exact") {
    auto src = std::make_shared<ObservableList<Plain>>();
    for (int v : {1, 2, 3}) src->push_back(std::make_shared<Plain>(Plain{v}));

    FilteredList<Plain> fl{src, [](const Plain&) { return true; }};
    REQUIRE(fl.size() == 3);

    Mirror<Plain>   mirror{fl};
    EventLog<Plain> log{fl};

    // Keep only 3 — the first two elements drop out.
    fl.set_predicate([](const Plain& p) { return p.value == 3; });

    REQUIRE(fl.size() == 1);
    CHECK(fl.at(0)->value == 3);

    // Correct D-11 stream is Remove(0), Remove(0): after the first removal
    // the observer's mirror is [B,C], so C's predecessor is again at 0.
    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 0);
    CHECK(log.events[1].kind == ListChangeKind::Remove);
    CHECK(log.events[1].index == 0);  // pre-fix: 1

    mirror.check_matches(fl);
}

TEST_CASE("FilteredList: set_predicate widening keeps observer mirror exact") {
    auto src = std::make_shared<ObservableList<Plain>>();
    for (int v : {1, -2, -3, 4}) src->push_back(std::make_shared<Plain>(Plain{v}));

    FilteredList<Plain> fl{src, [](const Plain& p) { return p.value > 0; }};
    REQUIRE(fl.size() == 2);  // [1, 4]

    Mirror<Plain>   mirror{fl};
    EventLog<Plain> log{fl};

    fl.set_predicate([](const Plain&) { return true; });

    REQUIRE(fl.size() == 4);
    // -2 is admitted at mirror position 1, then -3 at position 2.
    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 1);
    CHECK(log.events[1].kind == ListChangeKind::Insert);
    CHECK(log.events[1].index == 2);

    mirror.check_matches(fl);
}

TEST_CASE("FilteredList: set_predicate mixed in/out keeps observer mirror exact") {
    // Interleave so removals and insertions alternate: the two coordinate
    // systems diverge immediately if indices are computed from the wrong
    // mapping.
    auto src = std::make_shared<ObservableList<Plain>>();
    for (int v : {1, 2, 3, 4, 5, 6}) src->push_back(std::make_shared<Plain>(Plain{v}));

    // Start: keep odds -> [1, 3, 5]
    FilteredList<Plain> fl{src, [](const Plain& p) { return p.value % 2 == 1; }};
    REQUIRE(fl.size() == 3);

    Mirror<Plain> mirror{fl};

    // Flip to evens -> [2, 4, 6]: every element changes membership.
    fl.set_predicate([](const Plain& p) { return p.value % 2 == 0; });

    REQUIRE(fl.size() == 3);
    CHECK(fl.at(0)->value == 2);
    CHECK(fl.at(1)->value == 4);
    CHECK(fl.at(2)->value == 6);

    mirror.check_matches(fl);
}

TEST_CASE("FilteredList: repeated set_predicate keeps observer mirror exact") {
    auto src = std::make_shared<ObservableList<Plain>>();
    for (int v : {5, 1, 4, 2, 3}) src->push_back(std::make_shared<Plain>(Plain{v}));

    FilteredList<Plain> fl{src, [](const Plain&) { return true; }};
    Mirror<Plain>       mirror{fl};

    // Walk through a series of unrelated predicates; the mirror must track
    // the derived list after every single one.
    fl.set_predicate([](const Plain& p) { return p.value >= 3; });
    mirror.check_matches(fl);

    fl.set_predicate([](const Plain& p) { return p.value <= 2; });
    mirror.check_matches(fl);

    fl.set_predicate([](const Plain& p) { return p.value != 4; });
    mirror.check_matches(fl);

    fl.set_predicate([](const Plain&) { return false; });
    mirror.check_matches(fl);
    CHECK(fl.size() == 0);

    fl.set_predicate([](const Plain&) { return true; });
    mirror.check_matches(fl);
    CHECK(fl.size() == 5);
}

// ═══════════════════════════════════════════════════════════════════════
// Lifetime: FilteredList outlives without dangling; source outlives derived
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: destroying the derived view stops further emissions") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(std::make_shared<Plain>(Plain{1}));

    int observed = 0;
    Subscription sub;
    {
        FilteredList<Plain> fl{src, [](const Plain&){ return true; }};
        sub = fl.observe([&](const ListChange<Plain>&){ ++observed; });
        src->push_back(std::make_shared<Plain>(Plain{2}));
        CHECK(observed == 1);
    }
    // Derived view destroyed. Subsequent source mutations must not
    // invoke our subscription (the subscription object is still alive
    // but the signal it was attached to has already released).
    src->push_back(std::make_shared<Plain>(Plain{3}));
    CHECK(observed == 1);
}

TEST_CASE("FilteredList: source holding only a strong ref from derived stays alive") {
    // We deliberately drop the local shared_ptr to the source AFTER
    // constructing the FilteredList; it must still work because
    // FilteredList holds a strong ref internally.
    FilteredList<Plain>* leak = nullptr;
    {
        auto src = std::make_shared<ObservableList<Plain>>();
        src->push_back(std::make_shared<Plain>(Plain{7}));
        leak = new FilteredList<Plain>(src,
            [](const Plain& p){ return p.value > 0; });
        // `src` goes out of scope here — but the derived still owns it.
    }
    CHECK(leak->size() == 1);
    CHECK(leak->at(0)->value == 7);
    delete leak;
}

// ═══════════════════════════════════════════════════════════════════════
// Stress / bookkeeping sanity
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("FilteredList: 1000 mixed insert/remove keeps invariants") {
    auto src = std::make_shared<ObservableList<Plain>>();
    FilteredList<Plain> fl{src, [](const Plain& p){ return p.value % 2 == 0; }};

    // Seed with 200 items.
    for (int i = 0; i < 200; ++i) {
        src->push_back(std::make_shared<Plain>(Plain{i}));
    }

    // Insert 500 items at pseudo-random positions (deterministic).
    std::size_t rng = 17;
    for (int k = 0; k < 500; ++k) {
        rng = rng * 1103515245u + 12345u;
        const std::size_t pos = rng % (src->size() + 1);
        src->insert(pos, std::make_shared<Plain>(Plain{k * 3}));
    }
    // Remove 300 items.
    for (int k = 0; k < 300; ++k) {
        rng = rng * 1103515245u + 12345u;
        const std::size_t pos = rng % src->size();
        src->remove_at(pos);
    }

    // Invariant: fl.size() == number of even-valued items in src.
    std::size_t expected_even = 0;
    for (std::size_t i = 0; i < src->size(); ++i) {
        if (src->at(i)->value % 2 == 0) ++expected_even;
    }
    CHECK(fl.size() == expected_even);

    // Invariant: source_index_of(j) gives increasing, in-filter source indices.
    std::optional<std::size_t> prev;
    for (std::size_t j = 0; j < fl.size(); ++j) {
        auto si = fl.source_index_of(j);
        REQUIRE(si.has_value());
        CHECK(src->at(*si)->value % 2 == 0);
        if (prev) CHECK(*si > *prev);
        prev = si;
    }

    // Invariant: the snapshot() and at()-sequence match.
    auto snap = fl.snapshot();
    CHECK(snap.size() == fl.size());
    for (std::size_t j = 0; j < snap.size(); ++j) {
        CHECK(snap[j].get() == fl.at(j).get());
    }
}
