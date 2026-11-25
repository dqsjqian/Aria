#include <doctest/doctest.h>

// Regressions for the 2026-11 review of the reactive graph. Each case failed
// before its fix; see the CHANGELOG entry for 3.3.1.

#include "aria/reactive/reactive.hpp"

#include <deque>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace aria::reactive;

TEST_CASE("Computed callbacks depend only on the Computed") {
    Property<int> source{1};
    Property<int> unrelated{0};
    Computed<int> doubled{[&] { return source.get() * 2; }};

    int changed = 0;
    auto on_changed = doubled.on_changed([&](const int&) {
        ++changed;
        (void)unrelated.get();  // must not become a dependency
    });
    int bound = 0;
    auto bind = doubled.bind([&](const int&) {
        ++bound;
        (void)unrelated.get();
    });
    int observed = 0;
    auto observe = doubled.observe([&](const int&, const int&) {
        ++observed;
        (void)unrelated.get();
    });

    source = 2;
    CHECK(changed == 1);
    CHECK(bound == 2);  // initial sync + one change
    CHECK(observed == 1);

    unrelated = 5;
    unrelated = 6;
    CHECK(changed == 1);
    CHECK(bound == 2);
    CHECK(observed == 1);
}

TEST_CASE("A throwing effect does not strand the rest of its flush round") {
    Property<int> q{0};
    Computed<bool> positive{[&] { return q.get() > 0; }};
    bool armed = false;
    Effect failing{[&] {
        (void)positive.get();
        if (armed) {
            throw std::runtime_error("effect failure");
        }
    }};
    int shown = -1;
    Effect display{[&] { shown = positive.get() ? 1 : 0; }};

    armed = true;
    CHECK_THROWS_WITH_AS(q = 5, "effect failure", std::runtime_error);
    armed = false;
    // The writer still sees the failure, but the sibling effect settled in
    // the same flush instead of staying MaybeDirty and unqueued.
    CHECK(shown == 1);
    q = 6;  // positive does not change again
    CHECK(shown == 1);
    CHECK(positive.get());
}

TEST_CASE("Large tracking contexts keep first-read order and distinct live nodes") {
    TrackingContext ctx;
    std::deque<Node> sources;
    for (int i = 0; i < 192; ++i) {
        auto& n = sources.emplace_back(NodeKind::Source);
        ctx.record_read(n);
        ctx.record_read(n);
    }
    const bool ordered = std::ranges::equal(ctx.reads(), sources,
        [](const auto& handle, const Node& node) { return handle.get() == &node; });
    REQUIRE(ordered);
    // A new node can occupy the same storage mid-evaluation.
    std::optional<Node> ephemeral{std::in_place, NodeKind::Source};
    ctx.record_read(*ephemeral);
    ephemeral.emplace(NodeKind::Source);
    ctx.record_read(*ephemeral);
    CHECK(ctx.reads().size() == sources.size() + 2);
    CHECK(ctx.reads().back().get() == &*ephemeral);
    ctx.clear();
    ctx.record_read(sources.front());
    CHECK(ctx.reads().size() == 1);
}

TEST_CASE("A long acyclic chain of property notifications settles") {
    constexpr std::size_t length = 256;
    std::deque<Property<int>> values(length + 1);
    std::vector<aria::Subscription> edges;
    edges.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        edges.push_back(values.at(i).on_changed([&, i](int v) { values.at(i + 1) = v; }));
    }
    values.front() = 42;
    CHECK(values.back().get() == 42);
}

TEST_CASE("Only the first effect failure of a flush is rethrown") {
    Property<int> p{0};
    bool armed = false;
    Effect first{[&] {
        (void)p.get();
        if (armed) {
            throw std::runtime_error("first");
        }
    }};
    Effect second{[&] {
        (void)p.get();
        if (armed) {
            throw std::runtime_error("second");
        }
    }};
    int seen = -1;
    Effect third{[&] { seen = p.get(); }};
    armed = true;
    CHECK_THROWS_AS(p = 1, std::runtime_error);
    armed = false;
    CHECK(seen == 1);
}
