#include <doctest/doctest.h>

#include "aria/computed.hpp"
#include "aria/property.hpp"
#include <stdexcept>
#include <string>

using namespace aria;

TEST_CASE("Computed: basic dependency") {
    Property<int> a(3), b(4);
    Computed<int> sum([&]{ return a.get() + b.get(); });
    CHECK(sum.get() == 7);
    a = 10;
    CHECK(sum.get() == 14);
    b = 20;
    CHECK(sum.get() == 30);
}

TEST_CASE("Computed: chain of dependencies") {
    Property<int> base(10);
    Computed<int> doubled([&]{ return base.get() * 2; });
    Computed<int> quadrupled([&]{ return doubled.get() * 2; });
    CHECK(doubled.get() == 20);
    CHECK(quadrupled.get() == 40);
    base = 5;
    CHECK(quadrupled.get() == 20);
}

TEST_CASE("Computed: notifies on actual change") {
    Property<int> a(1);
    Computed<int> doubled([&]{ return a.get() * 2; });
    int seen = -1;
    auto s = doubled.on_changed([&](const int& v) { seen = v; });
    a = 5;
    CHECK(seen == 10);
}

TEST_CASE("Computed: bind() fires immediately") {
    Property<int> a(7);
    Computed<int> tripled([&]{ return a.get() * 3; });
    int seen = -1;
    auto s = tripled.bind([&](const int& v) { seen = v; });
    CHECK(seen == 21);
    a = 10;
    CHECK(seen == 30);
}

TEST_CASE("Computed: type transformation") {
    Property<int> n(42);
    Computed<std::string> label([&]{ return "value=" + std::to_string(n.get()); });
    CHECK(label.get() == "value=42");
    n = 7;
    CHECK(label.get() == "value=7");
}

TEST_CASE("Computed: conditional deps re-track each run") {
    // The flag selects between `a` and `b`. The dep set must migrate
    // between them as the flag flips; otherwise the chosen value cannot
    // be observed on changes of the newly-relevant source.
    Property<bool> flag(true);
    Property<int>  a(10), b(20);
    Computed<int>  chosen([&]{ return flag.get() ? a.get() : b.get(); });
    CHECK(chosen.get() == 10);

    a = 11;
    CHECK(chosen.get() == 11);

    flag = false;
    CHECK(chosen.get() == 20);

    b = 21;
    CHECK(chosen.get() == 21);

    a = 999;              // should NOT propagate (a is no longer a dep)
    CHECK(chosen.get() == 21);
}

TEST_CASE("Computed: nested Computed works") {
    Property<int> a(3), b(4);
    Computed<int> sum([&]{ return a.get() + b.get(); });
    Computed<int> squared([&]{ int s = sum.get(); return s * s; });

    CHECK(squared.get() == 49);
    a = 6;
    CHECK(squared.get() == 100);
}

// ── B2 regression: Computed::recompute exception must NOT orphan us
//
// Earlier the recompute path was:
//   clear_sources(); edges_.clear();   // ← drop all upstreams FIRST
//   new_val = compute_();              // ← may throw
//   ...                                 // never reached on throw
//
// So a single thrown exception (e.g. parsing failure inside the user
// lambda) permanently severed the Computed from its sources — even
// after the upstream stabilised, the cached value never refreshed.
// The fixed implementation evaluates compute_() FIRST under a fresh
// tracker, and only swaps the dependency set in once the user code
// has succeeded. This test pins down that invariant.
TEST_CASE("Computed: recompute exception keeps prior dependencies wired (B2)") {
    Property<int> trigger(0);
    Property<int> safe(7);
    int eval_count = 0;
    bool should_throw = false;

    Computed<int> c([&]{
        ++eval_count;
        // Always read both upstreams so they are always real dependencies.
        // (We can't do `if (should_throw) throw;` BEFORE the reads,
        // otherwise the failing recompute would not have collected them
        // and the test below couldn't tell "kept prior deps" from
        // "newly registered deps via this same call".)
        const int t = trigger.get();
        const int s = safe.get();
        if (should_throw) {
            throw std::runtime_error("simulated failure");
        }
        return t + s;
    });

    // Initial eager run: both Properties are upstreams; cached == 0+7.
    CHECK(c.get() == 7);
    CHECK(eval_count == 1);

    // Arm the throw. Trigger a recompute via `trigger.set` and assert
    // it propagates. The buggy implementation here would have called
    // clear_sources()+edges_.clear() before throwing, severing the
    // edges to `trigger` and `safe`.
    should_throw = true;
    bool caught = false;
    try {
        trigger.set(1);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);

    // Cached value should still be the LAST successful one.
    CHECK(c.peek() == 7);

    // Disarm and bump the OTHER upstream (`safe`). If the dependency
    // set was orphaned by the failed recompute, `c` would never see
    // this change. The fix guarantees both edges stayed alive, so
    // setting `safe` now wakes up `c` and triggers a fresh recompute.
    should_throw = false;
    safe.set(100);
    CHECK(c.get() == 1 + 100);   // sees both `trigger=1` AND `safe=100`
    CHECK(eval_count >= 3);
}
