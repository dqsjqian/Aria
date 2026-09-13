#include <doctest/doctest.h>

#include "aria/property.hpp"
#include "aria/property_ops.hpp"

#include <chrono>
#include <queue>
#include <stdexcept>
#include <vector>

using namespace aria;
using namespace std::chrono_literals;

namespace {

// A trivial in-test timer: stores tasks with their virtual deadlines and
// fires them when advance() crosses each one.
struct FakeTimer : public IDelayedScheduler {
    struct Entry { std::chrono::milliseconds deadline; std::function<void()> fn; };
    std::chrono::milliseconds now{0};
    std::vector<Entry> q;

    void post_after(std::chrono::milliseconds delay,
                    std::function<void()> fn) override {
        q.push_back({now + delay, std::move(fn)});
    }
    void advance(std::chrono::milliseconds d) {
        now += d;
        std::vector<Entry> still_pending;
        for (auto& e : q) {
            if (e.deadline <= now) e.fn();
            else still_pending.push_back(std::move(e));
        }
        q.swap(still_pending);
    }
};

struct ThrowOnceValue {
    int value = 0;
    static inline bool reject_copy = false;
    explicit ThrowOnceValue(int v = 0) : value(v) {}
    ThrowOnceValue(const ThrowOnceValue& other) : value(other.value) {
        if (std::exchange(reject_copy, false)) throw std::runtime_error("value copy failed");
    }
    ThrowOnceValue(ThrowOnceValue&&) noexcept = default;
    ThrowOnceValue& operator=(const ThrowOnceValue&) = default;
    ThrowOnceValue& operator=(ThrowOnceValue&&) noexcept = default;
    bool operator==(const ThrowOnceValue&) const = default;
};

}  // namespace

TEST_CASE("distinct_until_changed dedupes consecutive equal values") {
    Property<int> src{0};
    auto dist = distinct_until_changed(src);
    int hits = 0;
    auto sub = dist->on_changed([&](int) { ++hits; });

    src = 1;  // 0 → 1, fire
    src = 1;  // dedupe (Property already drops, but downstream also does)
    src = 2;  // fire
    src = 2;  // dedupe
    src = 3;  // fire
    CHECK(hits == 3);
}

TEST_CASE("debounce only emits after quiet period") {
    Property<std::string> src{""};
    FakeTimer t;
    auto deb = debounce(src, 300ms, t);

    std::string last;
    int hits = 0;
    auto sub = deb->on_changed([&](const std::string& v) {
        last = v; ++hits;
    });

    src = "a";
    t.advance(100ms);
    src = "ab";
    t.advance(100ms);
    src = "abc";       // still typing — every change resets the timer
    t.advance(299ms);  // total 499ms but last keystroke only 299ms ago
    CHECK(hits == 0);

    t.advance(2ms);    // crosses the 300ms quiet boundary
    CHECK(hits == 1);
    CHECK(last == "abc");
}

TEST_CASE("throttle: leading edge, then drops within cooldown") {
    Property<int> src{0};
    FakeTimer t;
    auto thr = throttle(src, 200ms, t);

    std::vector<int> got;
    auto sub = thr->on_changed([&](int v) { got.push_back(v); });

    src = 1;          // immediate
    src = 2;          // dropped (cooldown active)
    src = 3;          // dropped
    t.advance(199ms);
    src = 4;          // still dropped
    t.advance(2ms);   // cooldown expired
    src = 5;          // fires
    src = 6;          // dropped again
    CHECK(got == std::vector{1, 5});
}

TEST_CASE("scan accumulates a running total") {
    Property<int> ticks{0};
    auto total = scan(ticks, 0, [](int acc, int v) { return acc + v; });

    int snapshot = 0;
    auto sub = total->on_changed([&](int v) { snapshot = v; });

    ticks = 1;  CHECK(snapshot == 1);
    ticks = 2;  CHECK(snapshot == 3);
    ticks = 5;  CHECK(snapshot == 8);
    CHECK(total->get() == 8);
}

// ════════════════════════════════════════════════════════════════════════════
// Cascade-cleanup regression tests
//   Pin down the contract of the ChainedNode-based rewrite:
//     dropping the last shared_ptr<Property<T>> reference MUST
//     synchronously detach the upstream subscription from `source`,
//     so further writes to `source` no longer drive the chain.
//   We assert this by behavior: subscribe a counter to the downstream,
//   release the chain, mutate `source`, and verify the counter does
//   not grow (the subscription kept by the test would have fired if
//   the upstream were still wired).
// ════════════════════════════════════════════════════════════════════════════
TEST_CASE("distinct_until_changed releases upstream when downstream dies") {
    Property<int> src{0};
    int hits = 0;
    Subscription sub;

    {
        auto dist = distinct_until_changed(src);
        sub = dist->on_changed([&](int) { ++hits; });
        src = 1;
        CHECK(hits == 1);
    }
    // Chain released. The user's `sub` is still alive but observes a dead
    // downstream Property; further source writes must NOT drive it.
    src = 2;
    src = 3;
    CHECK(hits == 1);
}

TEST_CASE("debounce drops upstream and stops firing after release") {
    Property<int> src{0};
    FakeTimer t;
    int hits = 0;
    Subscription sub;

    {
        auto deb = debounce(src, 100ms, t);
        sub = deb->on_changed([&](int) { ++hits; });
        src = 1;
        // Chain released here. Any pending timer task captures only weak
        // refs to the node and must lock() to nullptr.
    }
    src = 2;
    t.advance(500ms);
    CHECK(hits == 0);
}

TEST_CASE("throttle releases upstream when downstream dies") {
    Property<int> src{0};
    int hits = 0;
    Subscription sub;

    {
        FakeTimer t;
        auto thr = throttle(src, 100ms, t);
        sub = thr->on_changed([&](int) { ++hits; });
        src = 1;          // leading edge fires
        CHECK(hits == 1);
    }
    src = 2;              // chain dead; no more hits
    src = 3;
    CHECK(hits == 1);
}

TEST_CASE("scan releases upstream when downstream dies") {
    Property<int> src{0};
    int last = -1;
    Subscription sub;

    {
        auto total = scan(src, 0, [](int a, int v) { return a + v; });
        sub = total->on_changed([&](int v) { last = v; });
        src = 1;
        CHECK(last == 1);
    }
    src = 5;
    CHECK(last == 1);
}

TEST_CASE("aliasing shared_ptr keeps the whole chain alive") {
    // The public API returns shared_ptr<Property<T>> built via the
    // aliasing constructor pointing at ChainedNode::property. Even
    // through std::move and reset, the alias must own the node
    // lifetime correctly: chain stays live while any shared_ptr is
    // held, and the upstream detaches the moment the last alias dies.
    Property<int> src{0};
    int hits = 0;
    Subscription sub;

    auto chain = distinct_until_changed(src);
    sub = chain->on_changed([&](int) { ++hits; });

    auto chain2 = std::move(chain);   // chain == nullptr now
    src = 7;
    CHECK(hits == 1);

    chain2.reset();                   // last reference gone
    src = 8;
    CHECK(hits == 1);                 // upstream detached
}

// ── P2: combine_latest ──────────────────────────────────────────────────────

TEST_CASE("combine_latest: emits on either source change") {
    Property<int> a{1};
    Property<int> b{2};
    auto sum = combine_latest(a, b, [](int x, int y) { return x + y; });

    CHECK(sum->get() == 3);

    int hits = 0;
    auto sub = sum->on_changed([&](int) { ++hits; });

    a = 10;
    CHECK(sum->get() == 12);
    b = 5;
    CHECK(sum->get() == 15);
    CHECK(hits == 2);
}

TEST_CASE("combine_latest: heterogeneous types and projection") {
    Property<int>         qty{2};
    Property<double>      price{1.5};
    auto total = combine_latest(qty, price,
                                [](int q, double p) { return q * p; });
    CHECK(total->get() == doctest::Approx(3.0));
    qty = 4;
    CHECK(total->get() == doctest::Approx(6.0));
}

TEST_CASE("combine_latest: tears down both upstreams when dropped") {
    Property<int> a{0};
    Property<int> b{0};
    int hits = 0;
    {
        auto c = combine_latest(a, b, [](int x, int y) { return x + y; });
        auto sub = c->on_changed([&](int) { ++hits; });
        a = 1;
        b = 1;
        CHECK(hits == 2);
    }
    // Chain dropped: further source changes must not crash or fire.
    a = 99;
    b = 99;
    CHECK(hits == 2);
}

TEST_CASE("throttle resumes after its scheduler rejects a timer") {
    struct RejectOnce : FakeTimer {
        bool reject = true;
        void post_after(std::chrono::milliseconds delay, std::function<void()> fn) override {
            if (std::exchange(reject, false)) throw std::runtime_error("timer rejected");
            FakeTimer::post_after(delay, std::move(fn));
        }
    } timer;
    Property<int> source{0};
    auto limited = throttle(source, 20ms, timer);
    CHECK_THROWS_AS(source = 1, std::runtime_error);
    CHECK(limited->get() == 1);
    source = 2;
    CHECK(limited->get() == 2);
    source = 3;
    CHECK(limited->get() == 2);
    timer.advance(20ms);
    source = 4;
    CHECK(limited->get() == 4);
}

TEST_CASE("throttle resumes after a downstream observer throws") {
    FakeTimer timer;
    Property<int> source{0};
    auto limited = throttle(source, 20ms, timer);
    bool throw_once = true;
    auto sub = limited->on_changed([&](int) {
        if (std::exchange(throw_once, false)) throw std::runtime_error("observer failed");
    });
    CHECK_THROWS_AS(source = 1, std::runtime_error);
    CHECK(limited->get() == 1);
    source = 2;
    CHECK(limited->get() == 1); // The accepted cooldown survives downstream failure.
    timer.advance(20ms);
    source = 3;
    CHECK(limited->get() == 3);
}

TEST_CASE("throttle resumes when copying a value to its output fails") {
    FakeTimer timer;
    Property<ThrowOnceValue> source{ThrowOnceValue{0}};
    auto arm = source.on_changed([](const ThrowOnceValue& v) {
        if (v.value == 1) ThrowOnceValue::reject_copy = true;
    });
    auto limited = throttle(source, 20ms, timer);
    CHECK_THROWS_AS(source = ThrowOnceValue{1}, std::runtime_error);
    CHECK(limited->get().value == 0);
    source = ThrowOnceValue{2};
    CHECK(limited->get().value == 2);
    timer.advance(20ms);
    source = ThrowOnceValue{3};
    CHECK(limited->get().value == 3);
}

TEST_CASE("scan retains a mutable move-only reducer across source changes") {
    Property<int> source{0};
    auto total = scan(source, 0, [calls = std::make_unique<int>(0)](int acc, int v) mutable {
        return acc + v + ++*calls;
    });
    source = 1;
    CHECK(total->get() == 2);
    source = 2;
    CHECK(total->get() == 6);
}

TEST_CASE("combine_latest initializes and retains a mutable move-only combiner") {
    Property<int> a{1}, b{2};
    auto combined = combine_latest(a, b, [calls = std::make_unique<int>(0)](int x, int y) mutable {
        return x + y + ++*calls;
    });
    CHECK(combined->get() == 4);
    a = 3;
    CHECK(combined->get() == 7);
    b = 4;
    CHECK(combined->get() == 10);
}

TEST_CASE("combine_latest invokes its combiner as a retained lvalue") {
    struct Combine {
        int operator()(const int& a, const int& b) & { return a + b; }
    };
    Property<int> a{1}, b{2};
    auto sum = combine_latest(a, b, Combine{});
    CHECK(sum->get() == 3);
    a = 5;
    CHECK(sum->get() == 7);
}
