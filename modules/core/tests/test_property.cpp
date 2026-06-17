#include <doctest/doctest.h>

#include "aria/property.hpp"

#include <string>
#include <vector>

using namespace aria;

// ─── basics ────────────────────────────────────────────────────────────────

TEST_CASE("Property: initial value") {
    Property<int> p(42);
    CHECK(p.get() == 42);
}

TEST_CASE("Property: set updates and notifies") {
    Property<int> p(0);
    int seen = -1;
    auto sub = p.on_changed([&](const int& v) { seen = v; });
    p.set(10);
    CHECK(p.get() == 10);
    CHECK(seen == 10);
}

TEST_CASE("Property: set with same value does not notify (equality gate)") {
    Property<int> p(5);
    int n = 0;
    auto sub = p.on_changed([&](const int&) { ++n; });
    p.set(5);
    CHECK(n == 0);
    p.set(6);
    CHECK(n == 1);
}

TEST_CASE("Property: observe receives old and new") {
    Property<int> p(10);
    int old_v = -1, new_v = -1;
    auto sub = p.observe([&](const int& o, const int& n) { old_v = o; new_v = n; });
    p.set(20);
    CHECK(old_v == 10);
    CHECK(new_v == 20);
}

TEST_CASE("Property: subscription RAII") {
    Property<int> p(0);
    int n = 0;
    {
        auto s = p.on_changed([&](const int&) { ++n; });
        p.set(1);
        CHECK(n == 1);
    }
    p.set(2);
    CHECK(n == 1);  // sub destroyed -> no more notifications
}

TEST_CASE("Property: bind() fires immediately, then on change") {
    Property<int> p(42);
    int seen = -1;
    auto s = p.bind([&](const int& v) { seen = v; });
    CHECK(seen == 42);
    p.set(100);
    CHECK(seen == 100);
}

TEST_CASE("Property: mutate() in-place fires a change") {
    Property<std::vector<int>> p(std::vector<int>{1, 2, 3});
    int n = 0;
    auto s = p.on_changed([&](const auto&) { ++n; });
    p.mutate([](auto& v) { v.push_back(4); });
    CHECK(p.get().size() == 4);
    CHECK(n == 1);
}

TEST_CASE("Property: peek() does not register a dependency") {
    Property<int> count(0);
    int compute_runs = 0;
    // Build a reaction that peeks but does not read tracked.
    reactive::Effect reaction{[&]{
        (void)count.peek();
        ++compute_runs;
    }};
    int before = compute_runs;
    count.set(1);
    count.set(2);
    CHECK(compute_runs == before);  // peek did not latch a dep
}

// ─── batch / untracked ────────────────────────────────────────────────────

TEST_CASE("reactive::batch coalesces multiple writes") {
    Property<int> a(0), b(0);
    int n = 0;
    auto s = a.on_changed([&](const int&) { ++n; });
    auto t = b.on_changed([&](const int&) { ++n; });

    reactive::batch([&]{
        a = 1;
        a = 2;
        b = 1;
        b = 2;
        CHECK(n == 0);          // nothing fires inside the batch
    });
    // Exactly one invalidation per observed Property, not four.
    CHECK(n == 2);
    CHECK(a.get() == 2);
    CHECK(b.get() == 2);
}

TEST_CASE("reactive::untracked suppresses dependency recording") {
    Property<int> a(1), b(10);
    int runs = 0;
    reactive::Effect reaction{[&]{
        int x = a.get();
        int y = reactive::untracked([&]{ return b.get(); });
        (void)x; (void)y;
        ++runs;
    }};
    int before = runs;
    b.set(20);                  // should NOT retrigger the effect
    CHECK(runs == before);
    a.set(2);                   // should retrigger
    CHECK(runs == before + 1);
}

// ─── operator= and string type ───────────────────────────────────────────

TEST_CASE("Property: string type, operator=") {
    Property<std::string> p("hello");
    std::string seen;
    auto s = p.on_changed([&](const auto& v) { seen = v; });
    p = "world";
    CHECK(seen == "world");
    CHECK(p.get() == "world");
}

// ─── Subscription outliving Property ─────────────────────────────────────

TEST_CASE("Subscription outliving Property is safe") {
    Subscription sub;
    {
        Property<int> p(0);
        sub = p.on_changed([](const int&) {});
        CHECK(sub.active());
    }
    // Property destroyed first. Releasing the subscription must not crash.
    sub.release();
    CHECK_FALSE(sub.active());
}
