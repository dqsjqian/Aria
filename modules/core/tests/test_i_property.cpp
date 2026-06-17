#include <doctest/doctest.h>

#include "aria/i_property.hpp"
#include "aria/reactive/reactive.hpp"   // brings graph.inl → Node defs

#include <any>
#include <atomic>
#include <memory>
#include <string>
#include <typeinfo>

using aria::IProperty;
using aria::Property;

// ── Type identity ──────────────────────────────────────────────────────

TEST_CASE("IProperty: type() returns the underlying typeid") {
    Property<int>         pi{42};
    Property<std::string> ps{"hello"};

    IProperty& ai = pi;
    IProperty& as = ps;

    CHECK(ai.type() == typeid(int));
    CHECK(as.type() == typeid(std::string));
    CHECK(ai.type() != as.type());
}

// ── get_any ────────────────────────────────────────────────────────────

TEST_CASE("IProperty: get_any returns the current value with the right runtime type") {
    Property<int> p{7};
    IProperty& ip = p;

    auto a = ip.get_any();
    REQUIRE(a.has_value());
    REQUIRE(a.type() == typeid(int));
    CHECK(std::any_cast<int>(a) == 7);
}

TEST_CASE("IProperty: get_any reflects later set_any updates") {
    Property<int> p{1};
    IProperty& ip = p;

    CHECK(std::any_cast<int>(ip.get_any()) == 1);
    REQUIRE(ip.set_any(std::any{100}));
    CHECK(std::any_cast<int>(ip.get_any()) == 100);
}

// ── set_any ────────────────────────────────────────────────────────────

TEST_CASE("IProperty: set_any with matching type updates the value and returns true") {
    Property<int> p{0};
    IProperty& ip = p;

    REQUIRE(ip.set_any(std::any{42}));
    CHECK(p.get() == 42);
}

TEST_CASE("IProperty: set_any with mismatched type returns false and leaves value untouched") {
    Property<int> p{99};
    IProperty& ip = p;

    // Pass a string; expect failure with no mutation.
    bool ok = ip.set_any(std::any{std::string{"oops"}});
    CHECK_FALSE(ok);
    CHECK(p.get() == 99);

    // Float is also a mismatch (no implicit conversion through std::any_cast).
    ok = ip.set_any(std::any{3.14});
    CHECK_FALSE(ok);
    CHECK(p.get() == 99);
}

// ── subscribe_any ──────────────────────────────────────────────────────

TEST_CASE("IProperty: subscribe_any fires on change with the new value as std::any") {
    Property<int> p{0};
    IProperty& ip = p;

    int    callbacks = 0;
    int    last_value = -1;
    auto sub = ip.subscribe_any([&](const std::any& v) {
        ++callbacks;
        last_value = std::any_cast<int>(v);
    });

    p.set(5);
    CHECK(callbacks == 1);
    CHECK(last_value == 5);

    p.set(99);
    CHECK(callbacks == 2);
    CHECK(last_value == 99);
}

TEST_CASE("IProperty: subscribe_any cancellation stops further callbacks") {
    Property<int> p{0};
    IProperty& ip = p;

    int callbacks = 0;
    {
        auto sub = ip.subscribe_any([&](const std::any&) { ++callbacks; });
        p.set(1);
        CHECK(callbacks == 1);
        // sub goes out of scope here.
    }
    p.set(2);
    CHECK(callbacks == 1);  // no further fires
}

TEST_CASE("IProperty: subscribe_any fires synchronously on graph thread") {
    // Regression guard: callbacks should run on the graph thread, same
    // as typed `on_changed`. We sanity-check the call count rather than
    // probing thread identity (which would couple the test to the
    // dispatcher impl).
    Property<int> p{0};
    IProperty& ip = p;

    std::atomic<int> count{0};
    auto sub = ip.subscribe_any([&](const std::any&) {
        count.fetch_add(1, std::memory_order_relaxed);
    });

    for (int i = 0; i < 50; ++i) p.set(i + 1);
    CHECK(count.load() == 50);
}

// ── Polymorphic use through IProperty* ─────────────────────────────────

namespace {

// Pretend this lives in a "plug-in" that only sees IProperty*.
int sum_through_interface(IProperty& ip) {
    auto a = ip.get_any();
    return std::any_cast<int>(a);
}

}  // namespace

TEST_CASE("IProperty: polymorphic access via base interface pointer") {
    Property<int> p{123};
    int v = sum_through_interface(p);
    CHECK(v == 123);
}
