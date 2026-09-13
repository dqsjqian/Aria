#include <doctest/doctest.h>

#include "aria/derived/sorted_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/property.hpp"

#include <memory>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>

using namespace aria;

namespace {

// Plain struct — no on_changed. Insert/Remove/Replace/Move/Reset reach
// the derived side, but ItemChanged does not.
struct Plain {
    int value;
};

// Reactive struct — exposes on_changed so the source propagates
// ItemChanged. Same pattern as test_filtered_list.cpp.
struct Reactive {
    Property<int> v{0};
    [[nodiscard]] Subscription on_changed(std::function<void(const Reactive&)> fn) {
        return v.bind([this, f = std::move(fn)](const int&){ f(*this); });
    }
};

template<typename T>
struct EventLog {
    std::vector<ListChange<T>> events;
    Subscription sub;

    explicit EventLog(SortedList<T>& l) {
        sub = l.observe([this](const ListChange<T>& ch) {
            events.push_back(ch);
        });
    }
};

auto make_plain(int v) { return std::make_shared<Plain>(Plain{v}); }

// Ascending comparator by `value`.
auto asc = [](const Plain& a, const Plain& b) { return a.value < b.value; };
// Descending comparator by `value`.
auto desc = [](const Plain& a, const Plain& b) { return a.value > b.value; };

// Helper: extract the observed `value` sequence from the derived list.
std::vector<int> values_of(SortedList<Plain>& s) {
    std::vector<int> out;
    auto snap = s.snapshot();
    out.reserve(snap.size());
    for (const auto& p : snap) out.push_back(p->value);
    return out;
}

}  // namespace

TEST_CASE("SortedList: construction from initial snapshot orders items") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));
    src->push_back(make_plain(2));
    src->push_back(make_plain(8));
    src->push_back(make_plain(1));

    SortedList<Plain> sorted{src, asc};
    CHECK(values_of(sorted) == std::vector<int>{1, 2, 5, 8});
}

TEST_CASE("SortedList: empty source yields empty derived") {
    auto src = std::make_shared<ObservableList<Plain>>();
    SortedList<Plain> sorted{src, asc};
    CHECK(sorted.empty());
    CHECK(sorted.size() == 0);
}

TEST_CASE("SortedList: Insert binary-searches derived position (ascending)") {
    auto src = std::make_shared<ObservableList<Plain>>();
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    src->push_back(make_plain(10));      // derived: [10]
    src->push_back(make_plain(5));       // derived: [5, 10]
    src->push_back(make_plain(7));       // derived: [5, 7, 10]
    src->push_back(make_plain(20));      // derived: [5, 7, 10, 20]
    src->push_back(make_plain(1));       // derived: [1, 5, 7, 10, 20]

    CHECK(values_of(sorted) == std::vector<int>{1, 5, 7, 10, 20});
    REQUIRE(log.events.size() == 5);
    CHECK(log.events[0].kind == ListChangeKind::Insert);
    CHECK(log.events[0].index == 0);
    CHECK(log.events[1].index == 0);  // 5 before 10
    CHECK(log.events[2].index == 1);  // 7 between
    CHECK(log.events[3].index == 3);  // 20 at tail
    CHECK(log.events[4].index == 0);  // 1 at head
}

TEST_CASE("SortedList: Remove from middle emits Remove with derived index") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));
    src->push_back(make_plain(2));
    src->push_back(make_plain(8));
    SortedList<Plain> sorted{src, asc};  // derived: [2, 5, 8]
    EventLog<Plain> log{sorted};

    src->remove_at(0);  // remove '5' from source → derived loses 5

    CHECK(values_of(sorted) == std::vector<int>{2, 8});
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 1);  // 5 was at derived index 1
}

TEST_CASE("SortedList: Replace without position change emits Replace") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(2));
    src->push_back(make_plain(5));
    src->push_back(make_plain(8));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    // Replace the middle '5' with '4' — still sits between 2 and 8.
    src->replace_at(1, make_plain(4));

    CHECK(values_of(sorted) == std::vector<int>{2, 4, 8});
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Replace);
    CHECK(log.events[0].index == 1);
}

TEST_CASE("SortedList: Replace that crosses sort position → Remove + Insert") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(2));
    src->push_back(make_plain(5));
    src->push_back(make_plain(8));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    // Replace '5' with '10' — now should go to the tail.
    src->replace_at(1, make_plain(10));

    CHECK(values_of(sorted) == std::vector<int>{2, 8, 10});
    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 1);
    CHECK(log.events[1].kind == ListChangeKind::Insert);
    CHECK(log.events[1].index == 2);
}

TEST_CASE("SortedList: ItemChanged without position change emits ItemChanged") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto a = std::make_shared<Reactive>(); a->v = 2;
    auto b = std::make_shared<Reactive>(); b->v = 5;
    auto c = std::make_shared<Reactive>(); c->v = 8;
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Reactive> sorted{src,
        [](const Reactive& x, const Reactive& y) {
            return x.v.get() < y.v.get();
        }};
    EventLog<Reactive> log{sorted};

    b->v = 4;  // still between 2 and 8

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[0].index == 1);
    // Underlying items unchanged.
    auto snap = sorted.snapshot();
    CHECK(snap[0].get() == a.get());
    CHECK(snap[1].get() == b.get());
    CHECK(snap[2].get() == c.get());
}

TEST_CASE("SortedList: ItemChanged that crosses sort position emits Move") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto a = std::make_shared<Reactive>(); a->v = 2;
    auto b = std::make_shared<Reactive>(); b->v = 5;
    auto c = std::make_shared<Reactive>(); c->v = 8;
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Reactive> sorted{src,
        [](const Reactive& x, const Reactive& y) {
            return x.v.get() < y.v.get();
        }};
    EventLog<Reactive> log{sorted};

    b->v = 10;  // 5 → 10, now goes to the tail

    REQUIRE(log.events.size() == 2);
    CHECK(log.events[1].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[1].index == log.events[0].index);
    CHECK(log.events[1].item == log.events[0].item);
    CHECK(log.events[0].kind == ListChangeKind::Move);
    CHECK(log.events[0].from_index == 1);
    CHECK(log.events[0].index == 2);
    // Derived: [a(2), c(8), b(10)]
    auto snap = sorted.snapshot();
    CHECK(snap[0].get() == a.get());
    CHECK(snap[1].get() == c.get());
    CHECK(snap[2].get() == b.get());
}

TEST_CASE("SortedList: ItemChanged move leftward emits Move") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto a = std::make_shared<Reactive>(); a->v = 2;
    auto b = std::make_shared<Reactive>(); b->v = 5;
    auto c = std::make_shared<Reactive>(); c->v = 8;
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Reactive> sorted{src,
        [](const Reactive& x, const Reactive& y) {
            return x.v.get() < y.v.get();
        }};
    EventLog<Reactive> log{sorted};

    c->v = 1;  // 8 → 1, now goes to the head

    REQUIRE(log.events.size() == 2);
    CHECK(log.events[1].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[1].index == log.events[0].index);
    CHECK(log.events[1].item == log.events[0].item);
    CHECK(log.events[0].kind == ListChangeKind::Move);
    CHECK(log.events[0].from_index == 2);
    CHECK(log.events[0].index == 0);
    auto snap = sorted.snapshot();
    CHECK(snap[0].get() == c.get());
}

TEST_CASE("SortedList: source Move emits NO derived event (pure re-index)") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));
    src->push_back(make_plain(2));
    src->push_back(make_plain(8));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    src->move(0, 2);  // source: [5,2,8] → [2,8,5]. Derived order unchanged.

    CHECK(log.events.empty());
    CHECK(values_of(sorted) == std::vector<int>{2, 5, 8});
}

TEST_CASE("SortedList: source Reset propagates Reset") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));
    src->push_back(make_plain(2));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    src->clear();

    CHECK(sorted.empty());
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Reset);
}

TEST_CASE("SortedList: set_comparator emits Reset and re-sorts") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));
    src->push_back(make_plain(2));
    src->push_back(make_plain(8));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    sorted.set_comparator(desc);

    CHECK(values_of(sorted) == std::vector<int>{8, 5, 2});
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Reset);
}

TEST_CASE("SortedList: source_index_of is consistent with the mapping") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(5));    // source idx 0
    src->push_back(make_plain(2));    // source idx 1
    src->push_back(make_plain(8));    // source idx 2
    src->push_back(make_plain(1));    // source idx 3
    SortedList<Plain> sorted{src, asc};
    // Expected derived: [1(s=3), 2(s=1), 5(s=0), 8(s=2)]
    CHECK(*sorted.source_index_of(0) == 3);
    CHECK(*sorted.source_index_of(1) == 1);
    CHECK(*sorted.source_index_of(2) == 0);
    CHECK(*sorted.source_index_of(3) == 2);
    CHECK(!sorted.source_index_of(4).has_value());
}

TEST_CASE("SortedList: equivalent keys retain source order (stability)") {
    auto src = std::make_shared<ObservableList<Plain>>();
    // All same key '5', source order A, B, C.
    auto a = make_plain(5);
    auto b = make_plain(5);
    auto c = make_plain(5);
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Plain> sorted{src, asc};

    auto snap = sorted.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].get() == a.get());
    CHECK(snap[1].get() == b.get());
    CHECK(snap[2].get() == c.get());

    // Insert another '5' at source tail — must go after c (source_index tie-break).
    auto d = make_plain(5);
    src->push_back(d);
    snap = sorted.snapshot();
    REQUIRE(snap.size() == 4);
    CHECK(snap[3].get() == d.get());

    // Insert another '5' at source HEAD — must land before a.
    auto e = make_plain(5);
    src->insert(0, e);
    snap = sorted.snapshot();
    REQUIRE(snap.size() == 5);
    CHECK(snap[0].get() == e.get());
    CHECK(snap[1].get() == a.get());
}

TEST_CASE("SortedList: observer_count + on_any_change hook") {
    auto src = std::make_shared<ObservableList<Plain>>();
    SortedList<Plain> sorted{src, asc};

    int fires = 0;
    auto s1 = sorted.observe([](const ListChange<Plain>&){});
    auto s2 = sorted.on_any_change([&]{ ++fires; });
    CHECK(sorted.observer_count() == 2);

    src->push_back(make_plain(3));
    CHECK(fires == 1);
}

TEST_CASE("SortedList: destruction during pending source listener is safe") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(7));

    {
        SortedList<Plain> sorted{src, asc};
        CHECK(sorted.size() == 1);
    }
    // SortedList went out of scope; the source still works, and
    // further mutations must not dereference the destroyed derived
    // state (the weak_ptrs in the listener return nullptr).
    src->push_back(make_plain(3));
    CHECK(src->size() == 2);
}

TEST_CASE("SortedList: large stress — 1000 random mutations preserve order") {
    auto src = std::make_shared<ObservableList<Plain>>();
    SortedList<Plain> sorted{src, asc};

    std::mt19937 rng{12345};
    std::uniform_int_distribution<int> value_dist{0, 999};
    std::uniform_int_distribution<int> op_dist{0, 99};

    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        const int op = op_dist(rng);
        if (op < 60 || src->size() == 0) {
            // Insert at a random source position.
            std::uniform_int_distribution<int> pos_dist{0, static_cast<int>(src->size())};
            src->insert(static_cast<std::size_t>(pos_dist(rng)),
                        make_plain(value_dist(rng)));
        } else if (op < 90) {
            std::uniform_int_distribution<int> pos_dist{0, static_cast<int>(src->size()) - 1};
            src->remove_at(static_cast<std::size_t>(pos_dist(rng)));
        } else {
            std::uniform_int_distribution<int> pos_dist{0, static_cast<int>(src->size()) - 1};
            src->replace_at(static_cast<std::size_t>(pos_dist(rng)),
                            make_plain(value_dist(rng)));
        }
    }

    auto vs = values_of(sorted);
    CHECK(vs.size() == src->size());
    for (std::size_t i = 1; i < vs.size(); ++i) {
        CHECK(vs[i - 1] <= vs[i]);
    }

    // And source_index_of must round-trip: the shared_ptr at each
    // derived index must equal source->at(source_index_of(d)).
    for (std::size_t d = 0; d < sorted.size(); ++d) {
        const auto si = sorted.source_index_of(d);
        REQUIRE(si.has_value());
        CHECK(sorted.at(d).get() == src->at(*si).get());
    }
}

TEST_CASE("SortedList: Insert after initial snapshot maintains stability with ties") {
    auto src = std::make_shared<ObservableList<Plain>>();
    // Initial: key sequence [3, 1, 2, 1, 3] — two 3s and two 1s.
    auto p0 = make_plain(3);
    auto p1 = make_plain(1);
    auto p2 = make_plain(2);
    auto p3 = make_plain(1);
    auto p4 = make_plain(3);
    src->push_back(p0);
    src->push_back(p1);
    src->push_back(p2);
    src->push_back(p3);
    src->push_back(p4);

    SortedList<Plain> sorted{src, asc};
    auto snap = sorted.snapshot();
    REQUIRE(snap.size() == 5);
    // Expected derived: [p1(1, s=1), p3(1, s=3), p2(2, s=2), p0(3, s=0), p4(3, s=4)]
    CHECK(snap[0].get() == p1.get());
    CHECK(snap[1].get() == p3.get());
    CHECK(snap[2].get() == p2.get());
    CHECK(snap[3].get() == p0.get());
    CHECK(snap[4].get() == p4.get());
}

TEST_CASE("SortedList: Replace in tail region nudges left by one derived slot") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(1));
    src->push_back(make_plain(3));
    src->push_back(make_plain(5));
    src->push_back(make_plain(7));
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    // Replace the 5 with 2 — target sort position is derived index 1.
    src->replace_at(2, make_plain(2));

    CHECK(values_of(sorted) == std::vector<int>{1, 2, 3, 7});
    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 2);
    CHECK(log.events[1].kind == ListChangeKind::Insert);
    CHECK(log.events[1].index == 1);
}

TEST_CASE("SortedList: ItemChanged into equivalent key class stays put") {
    // We want to exercise "item mutated but new key is still
    // equivalent to the old one". Property::operator= is a no-op for
    // equal values, so we mutate through a key derived from TWO
    // fields, changing the ignored one.
    struct Twin {
        Property<int> key{0};
        Property<int> spare{0};
        [[nodiscard]] Subscription on_changed(std::function<void(const Twin&)> fn) {
            // Aggregate both field subscriptions behind a single
            // shared_ptr owned by the returned Subscription.
            auto bag = std::make_shared<SubscriptionBag>();
            bag->add(key.bind  ([this, f = fn](const int&){ f(*this); }));
            bag->add(spare.bind([this, f = fn](const int&){ f(*this); }));
            return Subscription{std::static_pointer_cast<void>(bag)};
        }
    };

    auto src = std::make_shared<ObservableList<Twin>>();
    auto a = std::make_shared<Twin>(); a->key = 5;
    auto b = std::make_shared<Twin>(); b->key = 5;
    auto c = std::make_shared<Twin>(); c->key = 5;
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Twin> sorted{src,
        [](const Twin& x, const Twin& y) {
            return x.key.get() < y.key.get();
        }};
    EventLog<Twin> log{sorted};

    // Mutate `spare` on b — fires on_changed but the sort key is
    // unchanged, so derived position stays.
    b->spare = 42;

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[0].index == 1);
}

TEST_CASE("SortedList: multiple observers all see the same event stream") {
    auto src = std::make_shared<ObservableList<Plain>>();
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> a{sorted};
    EventLog<Plain> b{sorted};

    src->push_back(make_plain(5));
    src->push_back(make_plain(2));

    REQUIRE(a.events.size() == 2);
    REQUIRE(b.events.size() == 2);
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        CHECK(a.events[i].kind == b.events[i].kind);
        CHECK(a.events[i].index == b.events[i].index);
    }
}

TEST_CASE("SortedList: Move-then-Remove keeps mapping consistent") {
    auto src = std::make_shared<ObservableList<Plain>>();
    src->push_back(make_plain(1));   // s=0
    src->push_back(make_plain(4));   // s=1
    src->push_back(make_plain(2));   // s=2
    src->push_back(make_plain(3));   // s=3
    SortedList<Plain> sorted{src, asc};
    // Derived: [1(0), 2(2), 3(3), 4(1)]

    src->move(1, 3);  // source → [1, 2, 3, 4]. Derived order unchanged.
    CHECK(values_of(sorted) == std::vector<int>{1, 2, 3, 4});

    // Now remove the '3' at source index 2.
    src->remove_at(2);
    CHECK(values_of(sorted) == std::vector<int>{1, 2, 4});

    // Derived -> source round-trip must still hold.
    for (std::size_t d = 0; d < sorted.size(); ++d) {
        const auto si = sorted.source_index_of(d);
        REQUIRE(si.has_value());
        CHECK(sorted.at(d).get() == src->at(*si).get());
    }
}

// ────────────────────────────────────────────────────────────────────────
// Equivalence-class & source.move edge cases (added after a
// peer review pointed out that the existing battery covered "stability
// at insertion" but not the corner cases where Remove / Replace /
// ItemChanged interact with an existing equivalence class, and where
// source.move CAN reshuffle within an equivalence class.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("SortedList: Remove of middle equivalent element preserves remaining stable order") {
    auto src = std::make_shared<ObservableList<Plain>>();
    auto a = make_plain(5);   // s=0
    auto b = make_plain(5);   // s=1
    auto c = make_plain(5);   // s=2
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    // Pre-condition: derived [a, b, c].
    auto pre = sorted.snapshot();
    REQUIRE(pre.size() == 3);
    CHECK(pre[0].get() == a.get());
    CHECK(pre[1].get() == b.get());
    CHECK(pre[2].get() == c.get());

    // Remove `b` (source idx 1). `a` stays at d=0; `c` slides from
    // d=2 to d=1, and its source idx renumbers from 2 to 1.
    src->remove_at(1);

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 1);

    auto post = sorted.snapshot();
    REQUIRE(post.size() == 2);
    CHECK(post[0].get() == a.get());
    CHECK(post[1].get() == c.get());

    // Mapping round-trip after the renumber.
    CHECK(*sorted.source_index_of(0) == 0);   // a
    CHECK(*sorted.source_index_of(1) == 1);   // c (was source idx 2)
}

TEST_CASE("SortedList: Replace inside an equivalence class with the same key emits a single Replace") {
    auto src = std::make_shared<ObservableList<Plain>>();
    auto a = make_plain(5);
    auto b = make_plain(5);
    auto c = make_plain(5);
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};

    // Replace `b` (source idx 1) with another '5'. Source idx is
    // unchanged, so the stability tie-breaker keeps derived position
    // 1 — the new ptr just slots into the same derived index.
    auto b2 = make_plain(5);
    src->replace_at(1, b2);

    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == ListChangeKind::Replace);
    CHECK(log.events[0].index == 1);

    auto snap = sorted.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].get() == a.get());
    CHECK(snap[1].get() == b2.get());
    CHECK(snap[2].get() == c.get());
}

TEST_CASE("SortedList: Replace lands inside an existing equivalence class — Remove + Insert at correct stable slot") {
    auto src = std::make_shared<ObservableList<Plain>>();
    // Source: [5(s=0), 3(s=1), 3(s=2), 8(s=3)]
    auto p5 = make_plain(5);
    auto p3a = make_plain(3);
    auto p3b = make_plain(3);
    auto p8 = make_plain(8);
    src->push_back(p5);
    src->push_back(p3a);
    src->push_back(p3b);
    src->push_back(p8);
    SortedList<Plain> sorted{src, asc};
    // Derived: [3a(s=1), 3b(s=2), 5(s=0), 8(s=3)]
    REQUIRE(values_of(sorted) == std::vector<int>{3, 3, 5, 8});
    {
        auto snap = sorted.snapshot();
        CHECK(snap[0].get() == p3a.get());
        CHECK(snap[1].get() == p3b.get());
    }

    EventLog<Plain> log{sorted};

    // Replace `p5` (s=0) with another `3`. Stability tie-break: new
    // p3c has source idx 0, which is LESS than p3a(s=1) and p3b(s=2),
    // so it must become the FIRST element of the equivalence class.
    auto p3c = make_plain(3);
    src->replace_at(0, p3c);

    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == ListChangeKind::Remove);
    CHECK(log.events[0].index == 2);   // p5 was at derived idx 2
    CHECK(log.events[1].kind == ListChangeKind::Insert);
    CHECK(log.events[1].index == 0);   // p3c lands at the head of the eq-class

    CHECK(values_of(sorted) == std::vector<int>{3, 3, 3, 8});
    auto snap = sorted.snapshot();
    CHECK(snap[0].get() == p3c.get());
    CHECK(snap[1].get() == p3a.get());
    CHECK(snap[2].get() == p3b.get());
    CHECK(snap[3].get() == p8.get());

    // d2s consistency: derived[0]→s=0, derived[1]→s=1, derived[2]→s=2.
    CHECK(*sorted.source_index_of(0) == 0);
    CHECK(*sorted.source_index_of(1) == 1);
    CHECK(*sorted.source_index_of(2) == 2);
    CHECK(*sorted.source_index_of(3) == 3);
}

TEST_CASE("SortedList: ItemChanged that crosses INTO an existing equivalence class emits Move to the stable slot") {
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto a = std::make_shared<Reactive>(); a->v = 5;   // s=0
    auto b = std::make_shared<Reactive>(); b->v = 3;   // s=1
    auto c = std::make_shared<Reactive>(); c->v = 3;   // s=2
    auto d = std::make_shared<Reactive>(); d->v = 8;   // s=3
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    src->push_back(d);
    SortedList<Reactive> sorted{src,
        [](const Reactive& x, const Reactive& y) {
            return x.v.get() < y.v.get();
        }};
    // Derived: [b(3,s=1), c(3,s=2), a(5,s=0), d(8,s=3)]
    EventLog<Reactive> log{sorted};

    // Mutate `a` (s=0) to key 3 — joins the eq-class. By stability,
    // s=0 < s=1 < s=2, so `a` MUST become the first element of the
    // equivalence class. ItemChanged crossing positions emits Move.
    a->v = 3;

    REQUIRE(log.events.size() == 2);
    CHECK(log.events[1].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[1].index == log.events[0].index);
    CHECK(log.events[1].item == log.events[0].item);
    CHECK(log.events[0].kind == ListChangeKind::Move);
    CHECK(log.events[0].from_index == 2);   // a was at d=2
    CHECK(log.events[0].index      == 0);   // a lands at d=0 (head of eq-class)

    auto snap = sorted.snapshot();
    REQUIRE(snap.size() == 4);
    CHECK(snap[0].get() == a.get());
    CHECK(snap[1].get() == b.get());
    CHECK(snap[2].get() == c.get());
    CHECK(snap[3].get() == d.get());
}

TEST_CASE("SortedList: source Move preserves stable order within equivalent keys") {
    auto src = std::make_shared<ObservableList<Plain>>();
    auto a = make_plain(5);
    auto b = make_plain(5);
    auto c = make_plain(5);
    auto low = make_plain(1);
    auto high = make_plain(9);
    const std::vector initial{low, a, b, c, high};
    src->insert_range(0, initial.begin(), initial.end());
    SortedList<Plain> sorted{src, asc};
    EventLog<Plain> log{sorted};
    auto mirror = sorted.snapshot();

    SUBCASE("forward") { src->move(1, 3); }
    SUBCASE("backward") { src->move(3, 1); }
    REQUIRE_FALSE(log.events.empty());
    for (const auto& change : log.events) {
        REQUIRE(change.kind == ListChangeKind::Move);
        REQUIRE(change.from_index < mirror.size());
        CHECK(mirror[change.from_index] == change.item);
        mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(change.from_index));
        REQUIRE(change.index <= mirror.size());
        mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(change.index), change.item);
    }
    CHECK(mirror == sorted.snapshot());
    CHECK(sorted.snapshot() == src->snapshot());
    for (std::size_t d = 0; d < sorted.size(); ++d) {
        const auto si = sorted.source_index_of(d);
        REQUIRE(si.has_value());
        CHECK(*si == d);
        CHECK(sorted.at(d) == src->at(*si));
    }
}

TEST_CASE("SortedList: ItemChanged routes correctly after a source Move renumbered the maps") {
    // Regression guard: a source Move renumbers s2d / d2s; a
    // subsequent ItemChanged on the same logical item must still
    // resolve to the right derived slot.
    auto src = std::make_shared<ObservableList<Reactive>>();
    auto a = std::make_shared<Reactive>(); a->v = 1;   // s=0
    auto b = std::make_shared<Reactive>(); b->v = 4;   // s=1
    auto c = std::make_shared<Reactive>(); c->v = 2;   // s=2
    auto d = std::make_shared<Reactive>(); d->v = 3;   // s=3
    src->push_back(a);
    src->push_back(b);
    src->push_back(c);
    src->push_back(d);
    SortedList<Reactive> sorted{src,
        [](const Reactive& x, const Reactive& y) {
            return x.v.get() < y.v.get();
        }};
    // Derived: [a(1,s=0), c(2,s=2), d(3,s=3), b(4,s=1)]

    // Source: move b from s=1 to s=3. Source becomes [a, c, d, b];
    // b's new source index is 3, others renumber accordingly.
    src->move(1, 3);

    // Derived items vector unchanged; only s2d / d2s renumbered.
    {
        auto snap = sorted.snapshot();
        CHECK(snap[0].get() == a.get());
        CHECK(snap[1].get() == c.get());
        CHECK(snap[2].get() == d.get());
        CHECK(snap[3].get() == b.get());
    }

    // Now mutate `b` so it sorts to the head. The handler must look
    // up `b` via the renumbered s2d (b is now at source idx 3),
    // produce a Move from d=3 to d=0, and not corrupt the maps.
    EventLog<Reactive> log{sorted};
    b->v = 0;

    REQUIRE(log.events.size() == 2);
    CHECK(log.events[1].kind == ListChangeKind::ItemChanged);
    CHECK(log.events[1].index == log.events[0].index);
    CHECK(log.events[1].item == log.events[0].item);
    CHECK(log.events[0].kind == ListChangeKind::Move);
    CHECK(log.events[0].from_index == 3);
    CHECK(log.events[0].index      == 0);

    auto snap = sorted.snapshot();
    REQUIRE(snap.size() == 4);
    CHECK(snap[0].get() == b.get());
    CHECK(snap[1].get() == a.get());
    CHECK(snap[2].get() == c.get());
    CHECK(snap[3].get() == d.get());

    // Round-trip after the Move: every derived idx d round-trips to
    // its source idx via source_index_of, and src->at() yields the
    // same shared_ptr.
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto si = sorted.source_index_of(i);
        REQUIRE(si.has_value());
        CHECK(sorted.at(i).get() == src->at(*si).get());
    }
}

TEST_CASE("SortedList: changing a repeated handle keeps the complete sequence ordered") {
    auto source = std::make_shared<ObservableList<Reactive>>();
    auto repeated = std::make_shared<Reactive>(); repeated->v = 1;
    auto other = std::make_shared<Reactive>(); other->v = 2;
    for (int i = 0; i < 4; ++i) source->push_back(repeated);
    source->push_back(other);
    SortedList<Reactive> view{source, [](const Reactive& a, const Reactive& b) {
        return a.v.get() < b.v.get();
    }};
    auto mirror = view.snapshot();
    auto subscription = view.observe([&](const ListChange<Reactive>& ch) {
        if (ch.kind == ListChangeKind::Move) {
            REQUIRE(ch.from_index < mirror.size());
            REQUIRE(ch.index < mirror.size());
            const auto item = mirror[ch.from_index];
            CHECK(item == ch.item);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(ch.from_index));
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(ch.index), item);
        } else {
            REQUIRE(ch.kind == ListChangeKind::ItemChanged);
            REQUIRE(ch.index < mirror.size());
            CHECK(mirror[ch.index] == ch.item);
        }
    });
    repeated->v = 3;
    REQUIRE(view.at(0) == other);
    CHECK(mirror == view.snapshot());
    for (std::size_t i = 1; i < view.size(); ++i) CHECK(view.at(i) == repeated);
    repeated->v = 0;
    REQUIRE(view.at(4) == other);
    CHECK(mirror == view.snapshot());
}

TEST_CASE("SortedList: randomized repeated objects preserve ordering and replay through Reset and Replace") {
    auto source = std::make_shared<ObservableList<Reactive>>();
    auto cmp = [](const Reactive& a, const Reactive& b) { return a.v.get() < b.v.get(); };
    auto upstream = std::make_shared<SortedList<Reactive>>(source, cmp);
    SortedList<Reactive, SortedList<Reactive>> view{upstream, cmp};
    std::vector<std::shared_ptr<Reactive>> pool;
    for (int i = 0; i < 5; ++i) {
        auto item = std::make_shared<Reactive>(); item->v = i;
        pool.push_back(std::move(item));
    }
    auto mirror = view.snapshot();
    auto subscription = view.observe([&](const ListChange<Reactive>& ch) {
        const auto pos = static_cast<std::ptrdiff_t>(ch.index);
        switch (ch.kind) {
        case ListChangeKind::Insert:
            REQUIRE(ch.index <= mirror.size());
            mirror.insert(mirror.begin() + pos, ch.item); break;
        case ListChangeKind::Remove:
            REQUIRE(ch.index < mirror.size());
            CHECK(mirror[ch.index] == ch.item);
            mirror.erase(mirror.begin() + pos); break;
        case ListChangeKind::Replace:
            REQUIRE(ch.index < mirror.size());
            mirror[ch.index] = ch.item; break;
        case ListChangeKind::Move: {
            REQUIRE(ch.from_index < mirror.size());
            REQUIRE(ch.index < mirror.size());
            const auto item = mirror[ch.from_index];
            CHECK(item == ch.item);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(ch.from_index));
            mirror.insert(mirror.begin() + pos, item); break;
        }
        case ListChangeKind::ItemChanged:
            REQUIRE(ch.index < mirror.size()); CHECK(mirror[ch.index] == ch.item); break;
        case ListChangeKind::Reset:
            REQUIRE(ch.snapshot); mirror = *ch.snapshot; break;
        }
    });
    std::mt19937 random{20260913};
    auto pick = [&] { return pool[random() % pool.size()]; };
    for (int step = 0; step < 600; ++step) {
        const auto size = source->size();
        const auto index = size ? random() % size : 0;
        switch (random() % 8) {
        case 0: source->push_back(pick()); break;
        case 1: if (size) source->remove_at(index); break;
        case 2: if (size) source->replace_at(index, pick()); break;
        case 3: {
            const std::vector<std::shared_ptr<Reactive>> items{pick(), pick(), pick()};
            source->insert_range(index, items.begin(), items.end()); break;
        }
        case 4: pick()->v = static_cast<int>(random() % 9); break;
        case 5: if (size) source->move(index, random() % size); break;
        case 6: source->reconcile({pick(), pick(), pick(), pick()}); break;
        case 7: upstream->set_comparator(cmp); break; // owned, nonempty Reset
        }
        if (source->size() > 32) source->clear();
        const auto actual = view.snapshot();
        CHECK(mirror == actual);
        auto expected = source->snapshot();
        std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a->v.get() < b->v.get();
        });
        REQUIRE(actual.size() == expected.size());
        std::unordered_map<const Reactive*, int> counts;
        for (const auto& item : expected) ++counts[item.get()];
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i]->v.get() == expected[i]->v.get());
            --counts[actual[i].get()];
            const auto source_index = view.source_index_of(i);
            REQUIRE(source_index.has_value());
            CHECK(actual[i] == upstream->at(*source_index));
        }
        for (const auto& [item, count] : counts) { (void)item; CHECK(count == 0); }
    }
}

TEST_CASE("SortedList: independent sort-key writes in one graph batch preserve order") {
    auto source = std::make_shared<ObservableList<Reactive>>();
    std::vector<std::shared_ptr<Reactive>> changed;
    for (int i = 0; i < 4; ++i) {
        auto item = std::make_shared<Reactive>(); item->v = 1;
        source->push_back(item); changed.push_back(std::move(item));
    }
    auto other = std::make_shared<Reactive>(); other->v = 2;
    source->push_back(other);
    SortedList<Reactive> view{source, [](const Reactive& a, const Reactive& b) {
        return a.v.get() < b.v.get();
    }};
    aria::batch([&] { for (const auto& item : changed) item->v = 3; });
    CHECK(view.at(0) == other);
}

TEST_CASE("SortedList: structural edits validate rows with pending sort-key notifications") {
    auto source = std::make_shared<ObservableList<Reactive>>();
    std::vector<std::shared_ptr<Reactive>> changed;
    for (int i = 0; i < 4; ++i) {
        auto item = std::make_shared<Reactive>(); item->v = 1;
        source->push_back(item); changed.push_back(std::move(item));
    }
    auto other = std::make_shared<Reactive>(); other->v = 2;
    source->push_back(other);
    SortedList<Reactive> view{source, [](const Reactive& a, const Reactive& b) {
        return a.v.get() < b.v.get();
    }};
    aria::batch([&] {
        for (const auto& item : changed) item->v = 3;
        SUBCASE("Insert") { source->insert(1, other); }
        SUBCASE("Replace") { source->replace_at(0, other); }
        SUBCASE("Remove") { source->remove_at(0); }
        SUBCASE("Move") { source->move(0, 1); }
        // The structural event arrives before the graph's queued ItemChanged.
        const auto snapshot = view.snapshot();
        CHECK(std::is_sorted(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) {
            return a->v.get() < b->v.get();
        }));
    });
}
