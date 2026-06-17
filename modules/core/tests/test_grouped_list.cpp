// ============================================================================
//  test_grouped_list.cpp
// ----------------------------------------------------------------------------
//  Pin down the PGR-N invariants laid out in
//  modules/core/include/aria/derived/grouped_list.hpp.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/derived/grouped_list.hpp"
#include "aria/observable_list.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace aria;

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
