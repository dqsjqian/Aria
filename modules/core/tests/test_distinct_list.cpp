// ============================================================================
//  test_distinct_list.cpp
// ----------------------------------------------------------------------------
//  Pin down the PD-N invariants laid out in
//  modules/core/include/aria/derived/distinct_list.hpp:
//
//    PD-1 canonical key
//    PD-2 first-wins
//    PD-3 differential events (no Reset on Insert/Remove of source)
//    PD-4 ItemChanged fast path
//    PD-5 source-destroyed lifetime safety
// ============================================================================

#include <doctest/doctest.h>

#include "aria/derived/distinct_list.hpp"
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

[[nodiscard]] std::shared_ptr<Tag> tag(std::string g, int s = 0) {
    return std::make_shared<Tag>(Tag{std::move(g), s});
}

}  // namespace

// ----------------------------------------------------------------------------
//  PD-2: first-wins on initial snapshot
// ----------------------------------------------------------------------------
TEST_CASE("PD-2: initial snapshot keeps first occurrence per key") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("b", 2));
    src->push_back(tag("a", 3));   // duplicate of "a"
    src->push_back(tag("c", 4));
    src->push_back(tag("b", 5));   // duplicate of "b"

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};

    REQUIRE(d.size() == 3);
    auto snap = d.snapshot();
    CHECK(snap[0]->group == "a");  CHECK(snap[0]->serial == 1);
    CHECK(snap[1]->group == "b");  CHECK(snap[1]->serial == 2);
    CHECK(snap[2]->group == "c");  CHECK(snap[2]->serial == 4);
}

// ----------------------------------------------------------------------------
//  PD-3: insert of a new key emits Insert; insert of a duplicate is silent
// ----------------------------------------------------------------------------
TEST_CASE("PD-3: insert of new key emits Insert") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a"));
    src->push_back(tag("b"));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    src->push_back(tag("c"));
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 2);
}

TEST_CASE("PD-3: insert of duplicate key is silent") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a"));
    src->push_back(tag("b"));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    src->push_back(tag("a"));   // duplicate
    src->push_back(tag("b"));   // duplicate
    CHECK(log.empty());
    CHECK(d.size() == 2);
}

// ----------------------------------------------------------------------------
//  PD-3: removing the visible representative promotes the next duplicate
// ----------------------------------------------------------------------------
TEST_CASE("PD-3: removing visible rep promotes next duplicate") {
    auto src = std::make_shared<ObservableList<Tag>>();
    auto a1 = tag("a", 1); src->push_back(a1);
    src->push_back(tag("b", 2));
    auto a2 = tag("a", 3); src->push_back(a2);

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(d.size() == 2);
    REQUIRE(d.snapshot()[0]->serial == 1);

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    src->remove_at(0);   // drop a1
    // Incremental contract (PD-3): the removed item a1 was the
    // visible representative for key "a"; the next duplicate a2 is
    // promoted into the SAME derived slot (Replace), preserving
    // observers' positional bindings. The visible set becomes
    // [a2 (promoted), b]; b stays at derived[1] untouched.
    bool saw_replace = false;
    bool saw_remove  = false;
    for (const auto& ev : log) {
        if (ev.kind == ListChangeKind::Replace) saw_replace = true;
        if (ev.kind == ListChangeKind::Remove)  saw_remove  = true;
    }
    CHECK(saw_replace);
    CHECK_FALSE(saw_remove);
    auto snap = d.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0]->group  == "a");
    CHECK(snap[0]->serial == 3);     // a2 promoted into a1's slot
    CHECK(snap[1]->group  == "b");
}

// ----------------------------------------------------------------------------
//  PD-3: source Reset becomes derived Reset
// ----------------------------------------------------------------------------
TEST_CASE("PD-3: source Reset becomes derived Reset") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a"));
    src->push_back(tag("b"));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    src->clear();
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind == ListChangeKind::Reset);
    CHECK(d.empty());
}

// ----------------------------------------------------------------------------
//  PD-1: default key is identity (T == Key)
// ----------------------------------------------------------------------------
TEST_CASE("PD-1: default key extractor is identity for hashable T") {
    auto src = std::make_shared<ObservableList<int>>();
    src->push_back(std::make_shared<int>(1));
    src->push_back(std::make_shared<int>(2));
    src->push_back(std::make_shared<int>(1));   // duplicate of 1
    src->push_back(std::make_shared<int>(3));

    DistinctList<int> d{src};
    REQUIRE(d.size() == 3);
    auto snap = d.snapshot();
    CHECK(*snap[0] == 1);
    CHECK(*snap[1] == 2);
    CHECK(*snap[2] == 3);
}

// ----------------------------------------------------------------------------
//  PD-5: source destroyed before DistinctList is destroyed -- no UB
// ----------------------------------------------------------------------------
TEST_CASE("PD-5: outliving the source is safe (weak source observer)") {
    std::shared_ptr<DistinctList<int>> d;
    {
        auto src = std::make_shared<ObservableList<int>>();
        src->push_back(std::make_shared<int>(7));
        src->push_back(std::make_shared<int>(7));
        d = std::make_shared<DistinctList<int>>(src);
        REQUIRE(d->size() == 1);
    }
    // Source dropped; the DistinctList still answers queries from
    // its own cached visible vector and does NOT crash.
    CHECK(d->size() == 1);
    CHECK(*d->at(0) == 7);
}

// ----------------------------------------------------------------------------
//  PD-2 (source-order): inserting a new-key item between two visible
//  representatives must place the new derived slot BETWEEN them, not
//  at the end. This pins down the contract correction over the
//  earlier "always append" implementation.
// ----------------------------------------------------------------------------
TEST_CASE("PD-2: insert of new key in the middle of the source"
          " lands at the matching derived position") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("c", 3));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(d.size() == 2);

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    // Insert "b" between "a" and "c" in the source.
    src->insert(1, tag("b", 2));

    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 1);   // <-- between a and c, not appended

    auto snap = d.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0]->group == "a");
    CHECK(snap[1]->group == "b");
    CHECK(snap[2]->group == "c");
}

TEST_CASE("PD-2: many sequential mid-inserts maintain source order") {
    auto src = std::make_shared<ObservableList<Tag>>();
    // Seed with a wide skeleton.
    src->push_back(tag("a"));
    src->push_back(tag("z"));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(d.size() == 2);

    // Repeatedly insert new keys at source position 1. After each
    // insert, the derived list MUST mirror the source order of the
    // visible representatives. We sample the resulting derived
    // ordering after a handful of inserts.
    src->insert(1, tag("b"));
    src->insert(2, tag("c"));
    src->insert(3, tag("d"));
    src->insert(4, tag("e"));

    auto snap = d.snapshot();
    REQUIRE(snap.size() == 6);
    CHECK(snap[0]->group == "a");
    CHECK(snap[1]->group == "b");
    CHECK(snap[2]->group == "c");
    CHECK(snap[3]->group == "d");
    CHECK(snap[4]->group == "e");
    CHECK(snap[5]->group == "z");
}

TEST_CASE("PD-2: prepending new keys lands at derived[0]") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("y"));
    src->push_back(tag("z"));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    src->insert(0, tag("a"));
    REQUIRE(log.size() == 1);
    CHECK(log[0].kind  == ListChangeKind::Insert);
    CHECK(log[0].index == 0);   // <-- prepended, not appended

    auto snap = d.snapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0]->group == "a");
    CHECK(snap[1]->group == "y");
    CHECK(snap[2]->group == "z");
}

TEST_CASE("PD-2: inserting a duplicate-of-existing-rep key is silent"
          " regardless of the source position") {
    auto src = std::make_shared<ObservableList<Tag>>();
    src->push_back(tag("a", 1));
    src->push_back(tag("z", 9));

    DistinctList<Tag, std::string> d{src,
        [](const Tag& t) { return t.group; }};
    REQUIRE(d.size() == 2);

    std::vector<ListChange<Tag>> log;
    auto sub = d.observe([&](const ListChange<Tag>& c) { log.push_back(c); });

    // Insert a duplicate of "a" at the end and at the front.
    src->insert(1, tag("a", 2));
    src->insert(0, tag("a", 3));

    CHECK(log.empty());
    auto snap = d.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0]->group == "a");
    CHECK(snap[0]->serial == 1);   // first-appearance preserved
}
