#include <doctest/doctest.h>

#include "aria/aria.hpp"

#include <memory>
#include <stdexcept>

using namespace aria;

TEST_CASE("Graph lifetime: initial bind callback may destroy its property") {
    auto source = std::make_unique<Property<int>>(7);
    int seen = 0;
    auto subscription = source->bind([&](const int& value) {
        seen = value;
        source.reset();
    });
    CHECK(seen == 7);
    CHECK_FALSE(source);
    CHECK_FALSE(subscription);
}

namespace {
struct ThrowingComputedValue {
    int value = 0;
    static inline bool fail_move = false;
    ThrowingComputedValue() = default;
    explicit ThrowingComputedValue(int v) : value(v) {}
    ThrowingComputedValue(const ThrowingComputedValue&) = default;
    ThrowingComputedValue& operator=(const ThrowingComputedValue&) = default;
    ThrowingComputedValue(ThrowingComputedValue&& other) {
        if (fail_move) throw std::runtime_error("cache move failed");
        value = other.value;
    }
    ThrowingComputedValue& operator=(ThrowingComputedValue&& other) {
        if (fail_move) throw std::runtime_error("cache move failed");
        value = other.value;
        return *this;
    }
    bool operator==(const ThrowingComputedValue&) const = default;
};
}  // namespace

TEST_CASE("Graph lifetime: failed computed cache construction leaves no edges") {
    Property<int> source{7};
    ThrowingComputedValue::fail_move = true;
    CHECK_THROWS_WITH_AS(([&] {
        Computed<ThrowingComputedValue> result{[&] {
            return ThrowingComputedValue{source.get()};
        }};
    }()), "cache move failed", std::runtime_error);
    ThrowingComputedValue::fail_move = false;
    CHECK_FALSE(source.has_observers());
    CHECK_NOTHROW(source.set(8));
}

TEST_CASE("Computed: failed cache assignment preserves the previous dependencies") {
    Property<int> first{7};
    Property<int> second{9};
    bool use_second = false;
    Computed<ThrowingComputedValue> result{[&] {
        return ThrowingComputedValue{use_second ? second.get() : first.get()};
    }};
    use_second = true;
    ThrowingComputedValue::fail_move = true;
    CHECK_THROWS_AS(first.set(8), std::runtime_error);
    ThrowingComputedValue::fail_move = false;
    CHECK(first.has_observers());
    CHECK_FALSE(second.has_observers());
    CHECK(result.peek().value == 7);
    first.set(10);
    CHECK(result.get().value == 9);
    CHECK_FALSE(first.has_observers());
    CHECK(second.has_observers());
}

TEST_CASE("Graph lifetime: a subscription can be released before batch flush") {
    Property<int> value{0};
    int calls = 0;
    auto subscription = value.on_changed([&](int) { ++calls; });
    reactive::batch([&] {
        value.set(1);
        subscription.release();
    });
    value.set(2);
    CHECK(calls == 0);
}

TEST_CASE("Graph lifetime: pending computed and effect may be destroyed") {
    Property<int> value{0};
    int calls = 0;
    auto computed = std::make_unique<Computed<int>>([&] { return value.get() * 2; });
    auto effect = std::make_unique<Effect>([&] { (void)computed->get(); ++calls; });
    CHECK(calls == 1);
    reactive::batch([&] {
        value.set(1);
        effect.reset();
        computed.reset();
    });
    CHECK(calls == 1);
}

TEST_CASE("Graph lifetime: callback may cancel a later reaction") {
    Property<int> trigger{0};
    Property<int> source{0};
    Computed<int> intermediate{[&] { return source.get(); }};
    int calls = 0;
    auto victim = intermediate.on_changed([&](int) { ++calls; });
    auto killer = trigger.on_changed([&](int) { victim.release(); });
    reactive::batch([&] { source.set(1); trigger.set(1); });
    CHECK(calls == 0);
}

TEST_CASE("Graph lifetime: self release retains the executing callback captures") {
    Property<int> value{0};
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak = capture;
    Subscription subscription;
    int calls = 0;
    subscription = value.on_changed([&, capture = std::move(capture)](int) {
        subscription.release();
        CHECK_FALSE(weak.expired());
        CHECK(*capture == 42);
        ++calls;
    });
    value.set(1);
    CHECK(weak.expired());
    value.set(2);
    CHECK(calls == 1);
}

TEST_CASE("Graph lifetime: effect can stop itself during recompute") {
    Property<int> value{0};
    std::unique_ptr<Effect> effect;
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak = capture;
    int calls = 0;
    effect = std::make_unique<Effect>([&, capture = std::move(capture)] {
        if (value.get() == 0) return;
        effect->stop();
        CHECK_FALSE(weak.expired());
        CHECK(*capture == 42);
        ++calls;
    });
    value.set(1);
    CHECK(weak.expired());
    CHECK_FALSE(effect->active());
    value.set(2);
    CHECK(calls == 1);
}

TEST_CASE("Graph lifetime: a tracked source can die before edges are committed") {
    Property<int> trigger{0};
    auto source = std::make_unique<Property<int>>(7);
    Computed<int> result{[&] {
        const int generation = trigger.get();
        const int read = source ? source->get() : 0;
        if (generation == 1) source.reset();
        return generation + read;
    }};
    trigger.set(1);
    CHECK(result.get() == 8);
    CHECK(result.dependency_count() == 1);
    trigger.set(2);
    CHECK(result.get() == 2);
}

TEST_CASE("Graph lifetime: upstream recompute may destroy the requested node") {
    Property<int> source{0};
    std::unique_ptr<Computed<int>> victim;
    Computed<int> upstream{[&] {
        const int value = source.get();
        if (value != 0) victim.reset();
        return value;
    }};
    victim = std::make_unique<Computed<int>>([&] { return upstream.get(); });
    reactive::batch([&] {
        source.set(1);
        CHECK_FALSE(reactive::Node::graph().pull(*victim));
        CHECK_FALSE(victim);
    });
}

TEST_CASE("Graph lifetime: trace exceptions leave the graph usable") {
    Property<int> value{0};
    int seen = 0;
    auto subscription = value.on_changed([&](int n) { seen = n; });
    reactive::flush_trace_hook_() = std::make_shared<reactive::FlushTraceFn>([](int, const reactive::Node*, int, bool) {
        throw std::runtime_error("trace failed");
    });
    CHECK_NOTHROW(value.set(1));
    CHECK(seen == 1);
    reactive::flush_trace_hook_() = {};
    value.set(2);
    CHECK(seen == 2);
}

TEST_CASE("Graph lifetime: trace can release a node just before it is pulled") {
    Property<int> value{0};
    int calls = 0;
    auto subscription = value.on_changed([&](int) { ++calls; });
    reactive::flush_trace_hook_() = std::make_shared<reactive::FlushTraceFn>([&](int phase, const reactive::Node*, int, bool) {
        if (phase == 2) {
            subscription.release();
            reactive::flush_trace_hook_() = {};
        }
    });
    value.set(1);
    reactive::flush_trace_hook_() = {};
    CHECK(calls == 0);
}

TEST_CASE("Graph: an unresolved dependency cycle raises instead of growing the worklist") {
    Property<int> trigger{0};
    Computed<int>* second = nullptr;
    Computed<int> first{[&] {
        dep(trigger);
        if (second) dep(*second);
        return trigger.peek();
    }};
    first.set_debug_name("first");
    Computed<int> tail{[&] { dep(first); return trigger.peek(); }};
    tail.set_debug_name("tail");
    second = &tail;
    CHECK_THROWS_WITH_AS(([&] { trigger.set(1); trigger.set(2); }()),
        doctest::Contains("first"), reactive::CircularDependencyError);
}

TEST_CASE("Graph: changed conditional dependencies may reverse direction without a cycle") {
    Property<bool> reversed{false};
    Property<int> source{7};
    Computed<int>* second = nullptr;
    Computed<int> first{[&] {
        return reversed.get() && second ? second->get() : source.get();
    }};
    Computed<int> tail{[&] { return reversed.get() ? source.get() : first.get(); }};
    second = &tail;
    reversed.set(true);
    CHECK(first.get() == 7);
    source.set(9);
    CHECK(first.get() == 9);
    CHECK(tail.get() == 9);
}
