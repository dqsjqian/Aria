// ============================================================================
//  test_grouped_list.cpp
// ----------------------------------------------------------------------------
//  Pin down the PGR-N invariants laid out in
//  modules/core/include/aria/derived/grouped_list.hpp.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/derived/grouped_list.hpp"
#include "aria/observable_list.hpp"
#include "aria/property.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace aria;

namespace grouped_test {

struct CountingKey {
    int value;
    std::size_t* comparisons;

    bool operator==(const CountingKey& other) const noexcept {
        ++*comparisons;
        return value == other.value;
    }
};

}  // namespace grouped_test

template<>
struct std::hash<grouped_test::CountingKey> {
    std::size_t operator()(const grouped_test::CountingKey& key) const noexcept {
        return std::hash<int>{}(key.value);
    }
};

namespace {

struct Tag {
    std::string  group;
    int          serial{0};
};

[[nodiscard]] std::shared_ptr<Tag> tag(std::string g, int s) {
    return std::make_shared<Tag>(Tag{std::move(g), s});
}

}  // namespace

// ----------------------------------------------------------------------------
//  PGR-2 / PGR-4: initial groups in first-appearance order
// ----------------------------------------------------------------------------
TEST_CASE("PGR-2/PGR-4: initial groups in first-appearance order") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("b", 2));
    src->push_back(tag("a", 3));
    src->push_back(tag("c", 4));
    src->push_back(tag("b", 5));

    GroupedList<Tag, std::string> g{src,
        [](const Tag& t) { return t.group; }};

    REQUIRE(g.size() == 3);
    auto snap = g.snapshot();
    CHECK(snap[0]->key == "a");  CHECK(snap[0]->items->size() == 2);
    CHECK(snap[1]->key == "b");  CHECK(snap[1]->items->size() == 2);
    CHECK(snap[2]->key == "c");  CHECK(snap[2]->items->size() == 1);
}

// ----------------------------------------------------------------------------
//  PGR-3: source insert into existing group emits no outer event
// ----------------------------------------------------------------------------
TEST_CASE("PGR-3: insert into existing group does not emit outer event") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("b", 2));

    GroupedList<Tag, std::string> g{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(g.size() == 2);

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = g.observe([&](auto& c) { log.push_back(c); });

    src->push_back(tag("a", 99));
    CHECK(log.empty());
    CHECK(g.find("a")->items->size() == 2);
}

TEST_CASE("PGR-3: insert under new key emits outer Insert") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));

    GroupedList<Tag, std::string> g{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(g.size() == 1);

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = g.observe([&](auto& c) { log.push_back(c); });

    src->push_back(tag("z", 1));
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 1);
    REQUIRE(g.find("z") != nullptr);
    CHECK(g.find("z")->items->size() == 1);
}

// ----------------------------------------------------------------------------
//  PGR-3: removing the last item of a group emits outer Remove
// ----------------------------------------------------------------------------
TEST_CASE("PGR-3: removing last item of a group emits outer Remove") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("b", 2));

    GroupedList<Tag, std::string> g{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = g.observe([&](auto& c) { log.push_back(c); });

    src->remove_at(0);   // drop the only "a"
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind == ListChangeKind::Remove);
    CHECK(g.find("a") == nullptr);
    CHECK(g.size() == 1);
}

// ----------------------------------------------------------------------------
//  PGR-4: source Reset becomes outer Reset
// ----------------------------------------------------------------------------
TEST_CASE("PGR-4: source Reset becomes outer Reset") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("b", 2));

    GroupedList<Tag, std::string> g{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = g.observe([&](auto& c) { log.push_back(c); });

    src->clear();
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind == ListChangeKind::Reset);
    CHECK(g.empty());
}

// ----------------------------------------------------------------------------
//  PGR-5: outliving the source is safe
// ----------------------------------------------------------------------------
TEST_CASE("PGR-5: outliving the source is safe (weak source observer)") {
    std::shared_ptr<GroupedList<Tag, std::string>> g;
    {
        auto src = std::make_shared<ObservableList<Tag>>();
        src->push_back(tag("a", 1));
        src->push_back(tag("a", 2));
        src->push_back(tag("b", 3));
        g = std::make_shared<GroupedList<Tag, std::string>>(
            src, [](const Tag& t) { return t.group; });
        REQUIRE(g->size() == 2);
    }
    // Source dropped; cached groups + inner lists still answer.
    CHECK(g->size() == 2);
    CHECK(g->find("a")->items->size() == 2);
}

// ----------------------------------------------------------------------------
//  PGR-1: default key extractor for hashable T (Key == T)
// ----------------------------------------------------------------------------
TEST_CASE("PGR-1: default key extractor uses identity for hashable T") {
    auto src = std::make_shared<ObservableList<int>>();
    src->push_back(std::make_shared<int>(1));
    src->push_back(std::make_shared<int>(2));
    src->push_back(std::make_shared<int>(1));
    src->push_back(std::make_shared<int>(3));

    GroupedList<int> g{src};
    REQUIRE(g.size() == 3);
    CHECK(g.find(1) != nullptr);
    CHECK(g.find(1)->items->size() == 2);
    CHECK(g.find(2)->items->size() == 1);
    CHECK(g.find(3)->items->size() == 1);
}

// ----------------------------------------------------------------------------
//  PGR-4 (source-position order): inserting a new-group seed in the
//  middle of the source must place the new group at the corresponding
//  outer position, not append.
// ----------------------------------------------------------------------------
TEST_CASE("PGR-4: new-group seed in middle of source"
          " creates outer slot at matching position") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 0));
    src->push_back(tag("c", 0));

    GroupedList<Tag, std::string> gl{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(gl.size() == 2);

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = gl.observe(
        [&](const ListChange<Group<Tag, std::string>>& c) { log.push_back(c); });

    src->insert(1, tag("b", 0));   // new group "b" between "a" and "c"
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 1);

    auto snap = gl.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0]->key == "a");
    CHECK(snap[1]->key == "b");
    CHECK(snap[2]->key == "c");
}

TEST_CASE("PGR-4: prepending a new-group seed places it at outer[0]") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("y", 0));
    src->push_back(tag("z", 0));

    GroupedList<Tag, std::string> gl{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = gl.observe(
        [&](const ListChange<Group<Tag, std::string>>& c) { log.push_back(c); });

    src->insert(0, tag("a", 0));
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 0);

    auto snap = gl.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0]->key == "a");
    CHECK(snap[1]->key == "y");
    CHECK(snap[2]->key == "z");
}

TEST_CASE("PGR-4: once created, a group's outer position is frozen"
          " against later seed-removal") {
    auto src = std::make_shared<ObservableList<Tag>>();
    auto a1 = tag("a", 0); src->push_back(a1);
    auto a2 = tag("a", 0); src->push_back(a2);   // second "a"
    src->push_back(tag("b", 0));

    GroupedList<Tag, std::string> gl{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(gl.size() == 2);
    REQUIRE(gl.at(0)->key == "a");
    REQUIRE(gl.at(1)->key == "b");

    std::vector<ListChange<Group<Tag, std::string>>> log;
    auto sub = gl.observe(
        [&](const ListChange<Group<Tag, std::string>>& c) { log.push_back(c); });

    src->remove_at(0);   // drop a1 (the seed for "a"), a2 still alive
    // Outer event stream must be silent: group "a" still has a2,
    // its outer slot must NOT migrate. This pins down the
    // "frozen position" wording in PGR-4.
    CHECK(log.empty());
    auto snap = gl.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0]->key == "a");
    CHECK(snap[1]->key == "b");
}

TEST_CASE("GroupedList: inner observers can read and reenter the source") {
    auto src = std::make_shared<ObservableList<int>>();
    src->emplace_back(1);
    auto groups = grouped<int>(src, [](int v) { return v % 2; });
    auto odd = groups->find(1)->items;
    auto mirror = odd->snapshot();
    bool nested = false;
    auto sub = odd->observe([&](const auto& ch) {
        CHECK(groups->size() == 1);
        if (ch.kind == ListChangeKind::Insert) {
            REQUIRE(ch.index <= mirror.size());
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(ch.index),
                          ch.item);
        } else if (ch.kind == ListChangeKind::Remove) {
            REQUIRE(ch.index < mirror.size());
            CHECK(mirror[ch.index] == ch.item);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(ch.index));
        }
        if (!nested) {
            nested = true;
            src->insert(0, std::make_shared<int>(5));
            src->remove_at(2);
        }
    });
    src->emplace_back(3);
    REQUIRE(mirror == odd->snapshot());
    REQUIRE(odd->size() == 2);
    CHECK(*odd->at(0) == 5);
    CHECK(*odd->at(1) == 1);
}

TEST_CASE("GroupedList: inner order and identity survive insertion Move and Replace") {
    auto src = std::make_shared<ObservableList<int>>();
    for (int n : {1, 2, 3}) src->emplace_back(n);
    auto groups = grouped<int>(src, [](int v) { return v % 2; });
    auto odd = groups->find(1);
    src->insert(0, std::make_shared<int>(5));
    CHECK(odd->items->snapshot() == std::vector<std::shared_ptr<int>>{
        src->at(0), src->at(1), src->at(3)});
    src->move(3, 0);
    CHECK(odd->items->snapshot() == std::vector<std::shared_ptr<int>>{
        src->at(0), src->at(1), src->at(2)});
    src->replace_at(0, std::make_shared<int>(7));
    CHECK(groups->find(1) == odd);
    REQUIRE(odd->items->size() == 3);
    CHECK(*odd->items->at(0) == 7);
    src->replace_at(0, std::make_shared<int>(8));
    REQUIRE(odd->items->size() == 2);
    CHECK(*odd->items->at(0) == 5);
    REQUIRE(groups->find(0)->items->size() == 2);
    CHECK(*groups->find(0)->items->at(0) == 8);
}

TEST_CASE("GroupedList: repeated handles retain each source occurrence") {
    auto src = std::make_shared<ObservableList<int>>();
    auto shared = std::make_shared<int>(1);
    src->push_back(shared);
    src->emplace_back(3);
    src->push_back(shared);
    auto groups = grouped<int>(src, [](int n) { return n % 2; });
    src->remove_at(2);
    src->remove_at(0);
    REQUIRE(groups->find(1)->items->size() == 1);
    CHECK(*groups->find(1)->items->at(0) == 3);
}

TEST_CASE("GroupedList: tail insertion does not scan the source keys") {
    auto source = std::make_shared<ObservableList<int>>();
    for (int i = 0; i < 4096; ++i) source->emplace_back(i % 16);
    std::size_t comparisons = 0;
    auto groups = grouped<grouped_test::CountingKey>(source, [&](int value) {
        return grouped_test::CountingKey{value, &comparisons};
    });

    comparisons = 0;
    for (int i = 0; i < 64; ++i) source->emplace_back(i % 16);
    // Allow hash-table implementation differences, while rejecting a scan
    // proportional to the 4096 existing source slots on every append.
    CHECK(comparisons < 256);
    CHECK(groups->size() == 16);
    CHECK(groups->at(0)->items->size() == 260);

    comparisons = 0;
    source->emplace_back(16);
    CHECK(comparisons < 256);
    REQUIRE(groups->size() == 17);
    CHECK(groups->at(16)->key.value == 16);
    CHECK(groups->at(16)->items->snapshot() ==
          std::vector<std::shared_ptr<int>>{source->at(source->size() - 1)});
}

TEST_CASE("GroupedList: nested tail inserts and repeated key changes replay in order") {
    struct LiveKey {
        Property<int> key;
        explicit LiveKey(int value) : key(value) {}
        Subscription on_changed(std::function<void(const LiveKey&)> fn) {
            return key.on_changed([this, fn = std::move(fn)](int) { fn(*this); });
        }
    };
    auto source = std::make_shared<ObservableList<LiveKey>>();
    auto shared = source->emplace_back(1);
    auto even = source->emplace_back(0);
    auto groups = grouped<int>(source, [](const LiveKey& item) { return item.key.get(); });
    auto odd_group = groups->find(1);
    auto even_group = groups->find(0);
    auto odd_mirror = odd_group->items->snapshot();
    auto even_mirror = even_group->items->snapshot();
    auto replay = [](auto& mirror, const ListChange<LiveKey>& change) {
        if (change.kind == ListChangeKind::Insert) {
            REQUIRE(change.index <= mirror.size());
            mirror.insert(mirror.begin() + static_cast<std::ptrdiff_t>(change.index), change.item);
        } else if (change.kind == ListChangeKind::Remove) {
            REQUIRE(change.index < mirror.size());
            CHECK(mirror[change.index] == change.item);
            mirror.erase(mirror.begin() + static_cast<std::ptrdiff_t>(change.index));
        } else {
            CHECK(change.kind == ListChangeKind::ItemChanged);
        }
    };
    bool nested = false;
    std::shared_ptr<LiveKey> other;
    auto odd_sub = odd_group->items->observe([&](const auto& change) {
        replay(odd_mirror, change);
        if (!nested && change.kind == ListChangeKind::Insert) {
            nested = true;
            other = source->emplace_back(1);
            source->push_back(shared);
        }
    });
    auto even_sub = even_group->items->observe([&](const auto& change) {
        replay(even_mirror, change);
    });
    source->push_back(shared);
    CHECK(odd_mirror == std::vector<std::shared_ptr<LiveKey>>{shared, shared, other, shared});
    CHECK(odd_mirror == odd_group->items->snapshot());

    shared->key.set(0);
    CHECK(groups->find(1) == odd_group);
    CHECK(groups->find(0) == even_group);
    CHECK(odd_mirror == std::vector<std::shared_ptr<LiveKey>>{other});
    CHECK(even_mirror == std::vector<std::shared_ptr<LiveKey>>{shared, even, shared, shared});
    CHECK(odd_mirror == odd_group->items->snapshot());
    CHECK(even_mirror == even_group->items->snapshot());
}
