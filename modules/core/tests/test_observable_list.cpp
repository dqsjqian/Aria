#include <doctest/doctest.h>

#include "aria/observable_list.hpp"
#include "aria/property.hpp"

using namespace aria;

namespace {
struct Item {
    int id;
    Property<bool> done{false};
    explicit Item(int i) : id(i) {}

    [[nodiscard]] Subscription on_changed(std::function<void(const Item&)> fn) {
        return done.on_changed([this, fn](const bool&) { fn(*this); });
    }
};
}  // namespace

TEST_CASE("ObservableList: empty initially") {
    ObservableList<Item> list;
    CHECK(list.empty());
    CHECK(list.size() == 0);
}

TEST_CASE("ObservableList: emplace_back grows the list") {
    ObservableList<Item> list;
    list.emplace_back(1);
    list.emplace_back(2);
    list.emplace_back(3);
    CHECK(list.size() == 3);
    CHECK(list.at(1)->id == 2);
}

TEST_CASE("ObservableList: insert/remove notifications") {
    ObservableList<Item> list;
    int inserts = 0, removes = 0, item_changes = 0, resets = 0;
    auto sub = list.observe([&](const ListChange<Item>& c) {
        switch (c.kind) {
            case ListChangeKind::Insert: ++inserts; break;
            case ListChangeKind::Remove: ++removes; break;
            case ListChangeKind::ItemChanged: ++item_changes; break;
            case ListChangeKind::Reset: ++resets; break;
            case ListChangeKind::Replace: break;
            case ListChangeKind::Move:    break;  // not exercised here; covered in its own test
        }
    });

    auto a = list.emplace_back(1);
    list.emplace_back(2);
    CHECK(inserts == 2);

    a->done.set(true);
    CHECK(item_changes == 1);

    list.remove_at(0);
    CHECK(removes == 1);

    list.clear();
    CHECK(resets == 1);
}

TEST_CASE("ObservableList: remove_all by predicate") {
    ObservableList<Item> list;
    list.emplace_back(1);
    auto b = list.emplace_back(2);
    list.emplace_back(3);
    b->done.set(true);

    auto removed = list.remove_all([](const Item& i) { return i.done.get(); });
    CHECK(removed == 1);
    CHECK(list.size() == 2);
    CHECK(list.at(0)->id == 1);
    CHECK(list.at(1)->id == 3);
}

TEST_CASE("ObservableList: snapshot is independent") {
    ObservableList<Item> list;
    list.emplace_back(1);
    list.emplace_back(2);
    auto snap = list.snapshot();
    list.clear();
    CHECK(snap.size() == 2);
    CHECK(list.empty());
}

TEST_CASE("ObservableList: on_any_change fires for everything") {
    ObservableList<Item> list;
    int n = 0;
    auto s = list.on_any_change([&]() { ++n; });
    auto a = list.emplace_back(1);   // 1
    a->done.set(true);                // 2
    list.remove_at(0);                // 3
    CHECK(n == 3);
}

// ═══════════════════════════════════════════════════════════════════════
//  Lifecycle / re-entrancy edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ObservableList: subscription dropped inside observer stays safe") {
    // Dropping an observer's Subscription from within the callback must
    // not leave the signal's slot vector in a corrupt state. TypedSignal
    // uses weak disconnect, so this is expected to work; pin it down.
    ObservableList<Item> list;
    int calls = 0;
    std::shared_ptr<Subscription> sub_slot = std::make_shared<Subscription>();
    *sub_slot = list.observe([&, sub_slot](const ListChange<Item>&) {
        ++calls;
        sub_slot->release();  // unsubscribe from inside the callback
    });

    list.emplace_back(1);   // fires once, then self-unsubscribes
    CHECK(calls == 1);

    list.emplace_back(2);   // no more observer
    CHECK(calls == 1);
}

TEST_CASE("ObservableList: list destroyed before observer Subscription") {
    // Outliving the signal: the Subscription is shared_ptr-backed and
    // holds a custom deleter that uses weak_ptr to the signal. Dropping
    // the sub after the list is gone must be a clean no-op.
    Subscription sub;
    {
        ObservableList<Item> list;
        sub = list.observe([](const ListChange<Item>&) {});
        list.emplace_back(1);
        // list goes out of scope here.
    }
    // Dropping sub now — deleter sees expired weak_ptr and returns.
    sub = Subscription{};
    CHECK(true);    // reaching this line under ASan means we're clean
}

TEST_CASE("ObservableList: re-entrant mutation from within observer is consistent") {
    // When an observer mutates the same list it's observing, the
    // secondary change must emit a notification and the final list
    // state must match what the observer code wrote — no deadlock, no
    // skipped change, no corrupted index.
    //
    // The current implementation emits signals OUTSIDE the list mutex,
    // so a re-entrant write reacquires the mutex cleanly. We pin this
    // behaviour with a test so future locking changes (e.g. moving
    // signal emit inside the critical section) would trip immediately.
    ObservableList<Item> list;

    int insert_events = 0;
    bool already_reentered = false;

    auto sub = list.observe([&](const ListChange<Item>& c) {
        if (c.kind == ListChangeKind::Insert) {
            ++insert_events;
            if (!already_reentered && insert_events == 1) {
                already_reentered = true;
                // Re-entrant mutation — the outer call is `list.emplace_back(1)`.
                list.emplace_back(99);
            }
        }
    });

    list.emplace_back(1);

    // We expect two Insert events: the original and the re-entrant one.
    CHECK(insert_events == 2);
    CHECK(list.size() == 2);
    // Re-entrant push_back lands at index 1 in the final list.
    CHECK(list.at(0)->id == 1);
    CHECK(list.at(1)->id == 99);
}

TEST_CASE("ObservableList: item destroyed via remove_at no longer fires ItemChanged") {
    // After remove_at the per-item subscription must be dropped so that
    // subsequent writes to the removed item do not reach the list's
    // signal. Otherwise a user who kept a shared_ptr to the removed
    // item (e.g. "undo" caches) would leak notifications for an item
    // that no longer belongs to the list.
    ObservableList<Item> list;
    int item_changes = 0;
    auto sub = list.observe([&](const ListChange<Item>& c) {
        if (c.kind == ListChangeKind::ItemChanged) ++item_changes;
    });

    auto a = list.emplace_back(1);
    auto b = list.emplace_back(2);

    a->done.set(true);      // fires once
    CHECK(item_changes == 1);

    // Keep `a` alive (shared_ptr) but remove it from the list.
    list.remove_at(0);

    // Writing to the detached item must NOT fire ItemChanged on the list.
    a->done.set(false);
    CHECK(item_changes == 1);

    // `b` is now at index 0 and still fires.
    b->done.set(true);
    CHECK(item_changes == 2);
}

// ═══════════════════════════════════════════════════════════════════════
//  Range operations & Move
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ObservableList: insert_range emits per-item Inserts in order") {
    ObservableList<Item> list;
    list.emplace_back(1);
    list.emplace_back(4);

    std::vector<std::size_t> indices;
    auto sub = list.observe([&](const ListChange<Item>& c) {
        if (c.kind == ListChangeKind::Insert) indices.push_back(c.index);
    });

    std::vector<std::shared_ptr<Item>> extra{
        std::make_shared<Item>(2),
        std::make_shared<Item>(3),
    };
    list.insert_range(1, extra.begin(), extra.end());

    CHECK(list.size() == 4);
    CHECK(list.at(0)->id == 1);
    CHECK(list.at(1)->id == 2);
    CHECK(list.at(2)->id == 3);
    CHECK(list.at(3)->id == 4);

    REQUIRE(indices.size() == 2);
    CHECK(indices[0] == 1);
    CHECK(indices[1] == 2);
}

TEST_CASE("ObservableList: remove_range emits per-item Removes at the same pivot") {
    ObservableList<Item> list;
    for (int i = 0; i < 5; ++i) list.emplace_back(i);
    // 0 1 2 3 4  ->  0 4
    std::vector<std::size_t> indices;
    auto sub = list.observe([&](const ListChange<Item>& c) {
        if (c.kind == ListChangeKind::Remove) indices.push_back(c.index);
    });

    list.remove_range(1, 3);

    CHECK(list.size() == 2);
    CHECK(list.at(0)->id == 0);
    CHECK(list.at(1)->id == 4);

    // Each Remove carries the index as observers see it at emit time:
    // since every prior Remove shifted the tail left, every emit
    // reports `1` (the pivot).
    REQUIRE(indices.size() == 3);
    CHECK(indices[0] == 1);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == 1);
}

TEST_CASE("ObservableList: remove_range out-of-bounds clamps") {
    ObservableList<Item> list;
    for (int i = 0; i < 3; ++i) list.emplace_back(i);

    list.remove_range(1, 100);      // only 2 elements from pos 1
    CHECK(list.size() == 1);
    CHECK(list.at(0)->id == 0);

    list.remove_range(99, 5);       // start past end → no-op
    CHECK(list.size() == 1);
}

TEST_CASE("ObservableList: move emits a single Move with from_index/index") {
    ObservableList<Item> list;
    auto a = list.emplace_back(1);
    auto b = list.emplace_back(2);
    auto c = list.emplace_back(3);

    ListChange<Item> captured{};
    int events = 0;
    auto sub = list.observe([&](const ListChange<Item>& ch) {
        captured = ch;
        ++events;
    });

    list.move(0, 2);    // a,b,c  →  b,c,a

    CHECK(events == 1);
    CHECK(captured.kind == ListChangeKind::Move);
    CHECK(captured.from_index == 0);
    CHECK(captured.index == 2);
    CHECK(captured.item == a.get());

    CHECK(list.at(0)->id == 2);
    CHECK(list.at(1)->id == 3);
    CHECK(list.at(2)->id == 1);
}

TEST_CASE("ObservableList: move rejects no-ops and out-of-range") {
    ObservableList<Item> list;
    list.emplace_back(1);
    list.emplace_back(2);

    int events = 0;
    auto sub = list.observe([&](const ListChange<Item>&) { ++events; });

    list.move(0, 0);            // same index — silent
    list.move(5, 1);            // out-of-range — silent
    list.move(0, 5);            // out-of-range — silent
    CHECK(events == 0);
}

TEST_CASE("ObservableList: ItemChanged index is consistent after move") {
    // After a move, the O(1) index map must still resolve to the
    // correct post-move position. This is exactly the kind of bug the
    // map would expose if we forgot to rebuild it.
    ObservableList<Item> list;
    auto a = list.emplace_back(1);
    auto b = list.emplace_back(2);
    auto c = list.emplace_back(3);

    std::vector<std::pair<std::size_t, int>> changes;
    auto sub = list.observe([&](const ListChange<Item>& ch) {
        if (ch.kind == ListChangeKind::ItemChanged) {
            changes.emplace_back(ch.index, ch.item->id);
        }
    });

    list.move(0, 2);    // a is now at index 2

    a->done.set(true);
    b->done.set(true);
    c->done.set(true);

    REQUIRE(changes.size() == 3);
    // Post-move layout: b=0, c=1, a=2.
    auto find_for = [&](int id) -> std::size_t {
        for (auto& [idx, i] : changes) if (i == id) return idx;
        return 999;
    };
    CHECK(find_for(2) == 0);
    CHECK(find_for(3) == 1);
    CHECK(find_for(1) == 2);
}

// ═══════════════════════════════════════════════════════════════════════
//  Contract: inserting the same shared_ptr twice
// ═══════════════════════════════════════════════════════════════════════
//
// The O(1) index map keys on the raw T*, so if the *same* shared_ptr
// (same underlying object) is inserted twice, only the LATER position
// survives in `index_of_`. ItemChanged events for that object will
// therefore report the later index.
//
// This is a deliberate trade-off: keeping a multi-map would make every
// insertion / removal more expensive for a rare edge case. The same
// limitation exists in most list-model-style APIs across other MVVM
// frameworks. Users who legitimately need two distinct "slots" for the
// same logical row should wrap them in two distinct shared_ptrs (e.g.
// `std::make_shared<Item>(*original)`).
//
// This test pins down the current behaviour so future refactors do not
// silently change the observable semantics.
TEST_CASE("ObservableList: same shared_ptr inserted twice -- ItemChanged reports latest index") {
    ObservableList<Item> list;
    auto shared = std::make_shared<Item>(42);
    list.push_back(shared);           // index 0
    list.push_back(shared);           // index 1 (SAME underlying T)
    CHECK(list.size() == 2);
    CHECK(list.at(0).get() == shared.get());
    CHECK(list.at(1).get() == shared.get());

    std::vector<std::size_t> observed_indices;
    auto sub = list.observe([&](const ListChange<Item>& ch) {
        if (ch.kind == ListChangeKind::ItemChanged) {
            observed_indices.push_back(ch.index);
        }
    });

    // Each push_back wires up its own per-slot subscription to
    // `done.on_changed`, so a single Property flip produces TWO
    // ItemChanged emits. Both emits resolve `raw` through the O(1)
    // index map, which currently holds the last-inserted position.
    shared->done.set(true);
    REQUIRE(observed_indices.size() == 2);
    CHECK(observed_indices[0] == 1);     // last-inserted position wins
    CHECK(observed_indices[1] == 1);

    // Removing slot 1 tears down slot 1's per-item subscription AND
    // erases the raw->index mapping. However, slot 0 still has its
    // independent subscription installed on the SAME `done` Property,
    // so a flip still fires an ItemChanged. Since `index_of_` no
    // longer knows the raw pointer, `index_of_raw_` returns
    // `slots_.size()` as a past-the-end "stale" sentinel — observers
    // are expected to treat that as "ignore, the item may no longer
    // be in the list".
    list.remove_at(1);
    observed_indices.clear();
    shared->done.set(false);
    REQUIRE(observed_indices.size() == 1);
    CHECK(observed_indices[0] == list.size());    // past-the-end sentinel
}

// ═══════════════════════════════════════════════════════════════════════
//  Regression: bind-style on_changed must NOT deadlock with push_back
// ═══════════════════════════════════════════════════════════════════════
//
// `Property::bind(fn)` is documented to fire `fn(value_)` synchronously
// at subscribe time. If a user types T puts that bind() inside their
// `on_changed`, the very act of `push_back(item)` will trigger the
// per-item subscription which fires the callback while the list's
// write lock is held — and the callback would shared_lock the same
// mutex.
//
// An earlier revision had this deadlock; the follow-up fix moves the
// per-item subscription installation OUTSIDE the write lock. This
// test pins the contract.
namespace {
struct BindReactive {
    Property<int> v{0};
    [[nodiscard]] Subscription on_changed(std::function<void(const BindReactive&)> fn) {
        // Use bind, which fires once at subscribe time.
        return v.bind([this, f = std::move(fn)](const int&){ f(*this); });
    }
};
}  // namespace

TEST_CASE("ObservableList: bind-style on_changed does not deadlock push_back") {
    ObservableList<BindReactive> list;
    auto r = std::make_shared<BindReactive>();
    r->v.set(42);

    // Before the fix this call hung forever (write-lock + recursive
    // shared_lock).
    list.push_back(r);
    REQUIRE(list.size() == 1);
    CHECK(list.at(0).get() == r.get());

    // And ItemChanged still fires correctly afterwards.
    int hits = 0;
    auto sub = list.observe([&](const ListChange<BindReactive>& c) {
        if (c.kind == ListChangeKind::ItemChanged) ++hits;
    });
    r->v.set(100);
    CHECK(hits >= 1);
}

TEST_CASE("ObservableList: bind-style on_changed survives insert/replace too") {
    ObservableList<BindReactive> list;
    auto a = std::make_shared<BindReactive>();   a->v.set(1);
    auto b = std::make_shared<BindReactive>();   b->v.set(2);
    auto c = std::make_shared<BindReactive>();   c->v.set(3);

    list.push_back(a);
    list.insert(0, b);          // would deadlock before the fix
    list.replace_at(1, c);      // would deadlock before the fix

    REQUIRE(list.size() == 2);
    CHECK(list.at(0).get() == b.get());
    CHECK(list.at(1).get() == c.get());

    int hits = 0;
    auto sub = list.observe([&](const ListChange<BindReactive>& ch) {
        if (ch.kind == ListChangeKind::ItemChanged) ++hits;
    });
    b->v.set(20);
    c->v.set(30);
    CHECK(hits >= 2);
}

// ── P2: std::ranges-compatible snapshot range ──────────────────────────────

#include <algorithm>
#include <ranges>
#include <string>

TEST_CASE("ObservableList::items() is a std::ranges::range") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(1));
    list.push_back(std::make_shared<Item>(2));
    list.push_back(std::make_shared<Item>(3));

    auto range = list.items();
    static_assert(std::ranges::range<decltype(range)>);
    static_assert(std::ranges::sized_range<decltype(range)>);

    CHECK(range.size() == 3);
    CHECK_FALSE(range.empty());

    // Range-for
    int sum = 0;
    for (auto& item : range) sum += item->id;
    CHECK(sum == 6);

    // Standard range algorithm
    auto n = std::ranges::count_if(list.items(),
                                   [](const std::shared_ptr<Item>& p) {
                                       return p->id >= 2;
                                   });
    CHECK(n == 2);
}

TEST_CASE("ObservableList::items() composes with views::transform") {
    ObservableList<Item> list;
    for (int i = 1; i <= 4; ++i) list.push_back(std::make_shared<Item>(i * 10));

    auto ids = list.items()
             | std::views::transform([](const std::shared_ptr<Item>& p) {
                   return p->id;
               });
    int total = 0;
    for (int v : ids) total += v;
    CHECK(total == 10 + 20 + 30 + 40);
}

TEST_CASE("ObservableList::items() snapshot is stable across later mutation") {
    ObservableList<Item> list;
    list.push_back(std::make_shared<Item>(1));
    auto range = list.items();          // snapshot of size 1
    list.push_back(std::make_shared<Item>(2));  // later mutation
    CHECK(range.size() == 1);           // snapshot unaffected
    CHECK(list.size() == 2);            // live list reflects it
}
