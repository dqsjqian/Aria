#pragma once

// Reactive operators on Property<T> — inspired by ReactiveX (RxCpp).
//
// Each operator returns `std::shared_ptr<Property<T>>` ("downstream") that
// reflects a transformed view of the source.  The chain auto-cleans when
// the last reference is dropped: the upstream Subscription, the per-op
// state and the downstream Property are bundled into a single heap node
// owned by the returned shared_ptr (via aliasing constructor), so there
// are no raw `new`/`delete`, no two-phase weak_ptr patching, and the
// teardown order is defined by member declaration order.
//
//   Property<std::string> raw_query{""};
//   auto debounced = debounce(raw_query, 300ms, ui_dispatcher);
//   auto distinct  = distinct_until_changed(*debounced);
//   auto sub = distinct->bind([](const std::string& q) { do_search(q); });
//
// ── Why shared_ptr<Property<T>> as the public return?
//    The operator needs to attach a subscription that writes back into the
//    downstream Property.  Returning by value would rely on RVO; when RVO
//    kicks out (debug builds, vector placement, etc.) the Property is
//    moved and the subscription's captured pointer dangles.  Wrapping in
//    shared_ptr eliminates that whole class of lifetime hazards.
//
// The "delay" family (debounce / throttle) needs a place to schedule
// timers. To keep core dependency-free we accept a tiny
// `IDelayedScheduler` interface that ANY of the following can implement:
//
//   - SimpleDispatcher      (real wall-clock, runtime module)
//   - VirtualTimeExecutor   (deterministic, async module)
//   - any custom timer

#include "aria/abi/export.hpp"
#include "aria/property.hpp"
#include "aria/scheduler.hpp"
#include "aria/subscription.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace aria {

/// Tiny interface — anything that can post a function to run after a delay.
///
/// Inherits virtually from `IScheduler` so that multi-role implementations
/// (e.g. `VirtualTimeExecutor : IExecutor, IDelayedScheduler`) collapse to
/// a single `IScheduler` subobject.
///
/// The base contract `IScheduler::schedule(fn)` is satisfied by routing
/// through `post_after(0ms, fn)`, and `caps()` advertises `Delay | Post`
/// by default. Concrete implementations that also offer main-thread
/// affinity, pumping, etc. should override `caps()` to widen the bitmask.
class ARIA_CORE_API IDelayedScheduler : public virtual IScheduler {
public:
    ~IDelayedScheduler() override;

    /// Legacy / canonical timer entry point. Implementations override
    /// this; the unified `IScheduler::schedule_after` is wired to it
    /// below.
    virtual void post_after(std::chrono::milliseconds delay,
                            std::function<void()> fn) = 0;

    // ── IScheduler bridge ────────────────────────────────────────────
    [[nodiscard]] SchedulerCaps caps() const noexcept override {
        return SchedulerCaps::Post | SchedulerCaps::Delay;
    }
    void schedule(std::function<void()> fn) override {
        post_after(std::chrono::milliseconds{0}, std::move(fn));
    }
    void schedule_after(std::chrono::milliseconds delay,
                        std::function<void()> fn) override {
        post_after(delay, std::move(fn));
    }
};

namespace detail {

// ─────────────────────────────────────────────────────────────────────────
// ChainedNode<T, State>
//   The heap-allocated bundle that owns one operator's downstream Property,
//   its per-op State, and the upstream Subscription. Constructed in two
//   phases (Property + State first, upstream wired afterwards) because the
//   upstream callback must be able to weak-reference the very node that
//   holds the Subscription.
//
// Member-order matters: destruction runs bottom-up, so the upstream
// Subscription is detached BEFORE the State and Property are torn down.
// That guarantees no late callback can race with destruction.
// ─────────────────────────────────────────────────────────────────────────
template<PropertyValue T, class State>
struct ChainedNode {
    // 1. Per-operator state (gen counters, last value, accumulator, ...).
    State state;

    // 2. Downstream Property — the value users observe.
    Property<T> property;

    // 3. Upstream subscription — wired LAST (after the node is shared_ptr-
    //    managed) and torn down FIRST. The default-constructed subscription
    //    is detached, so a partially-constructed node is safe even on
    //    exceptions in the wiring step.
    Subscription upstream;

    // In-place construct State from `state_args...`, then construct
    // Property<T> from `initial`. Using a tag dispatch (instead of a
    // forwarding ctor) lets us support States that are neither copyable
    // nor movable (e.g. those holding std::atomic members).
    template<class... StateArgs>
    ChainedNode(std::in_place_t, T initial, StateArgs&&... state_args)
        : state{std::forward<StateArgs>(state_args)...}
        , property(std::move(initial)) {}
};

/// Build the public `shared_ptr<Property<T>>` from a node, using the
/// aliasing constructor so the returned pointer keeps the whole node
/// alive while exposing only its `property` member to the caller.
template<PropertyValue T, class State>
[[nodiscard]] inline std::shared_ptr<Property<T>>
expose_property(std::shared_ptr<ChainedNode<T, State>> node) noexcept {
    Property<T>* p = &node->property;
    return std::shared_ptr<Property<T>>(std::move(node), p);
}

}  // namespace detail

// ════════════════════════════════════════════════════════════════════════════
// distinct_until_changed
//   Forwards values from `source`, but suppresses notifications when the
//   new value equals the previous one.
// ════════════════════════════════════════════════════════════════════════════
template<PropertyValue T>
[[nodiscard]] std::shared_ptr<Property<T>>
distinct_until_changed(Property<T>& source) {
    struct State { T last; };

    auto node = std::make_shared<detail::ChainedNode<T, State>>(
        std::in_place, source.get(), source.get());

    std::weak_ptr<detail::ChainedNode<T, State>> weak = node;
    node->upstream = source.on_changed([weak](const T& v) {
        if (auto n = weak.lock()) {
            if (v == n->state.last) return;
            n->state.last = v;
            n->property.set(v);
        }
    });

    return detail::expose_property(std::move(node));
}

// ════════════════════════════════════════════════════════════════════════════
// debounce
//   Emit only when the source has been quiet for `quiet` duration.
// ════════════════════════════════════════════════════════════════════════════
template<PropertyValue T>
[[nodiscard]] std::shared_ptr<Property<T>>
debounce(Property<T>& source,
         std::chrono::milliseconds quiet,
         IDelayedScheduler& timer) {
    struct State {
        std::atomic<std::uint64_t> gen{0};
        T pending;

        explicit State(T initial) : pending(std::move(initial)) {}
    };

    auto node = std::make_shared<detail::ChainedNode<T, State>>(
        std::in_place, source.get(), source.get());

    std::weak_ptr<detail::ChainedNode<T, State>> weak = node;
    node->upstream = source.on_changed(
        [weak, &timer, quiet](const T& v) {
            auto n = weak.lock();
            if (!n) return;
            n->state.pending = v;
            auto my_gen = n->state.gen.fetch_add(1, std::memory_order_acq_rel) + 1;
            timer.post_after(quiet, [weak, my_gen]() {
                if (auto nn = weak.lock()) {
                    if (nn->state.gen.load(std::memory_order_acquire) != my_gen) return;
                    nn->property.set(nn->state.pending);
                }
            });
        });

    return detail::expose_property(std::move(node));
}

// ════════════════════════════════════════════════════════════════════════════
// throttle (leading edge)
// ════════════════════════════════════════════════════════════════════════════
template<PropertyValue T>
[[nodiscard]] std::shared_ptr<Property<T>>
throttle(Property<T>& source,
         std::chrono::milliseconds cooldown,
         IDelayedScheduler& timer) {
    struct State {
        std::atomic<bool> blocked{false};
    };

    auto node = std::make_shared<detail::ChainedNode<T, State>>(
        std::in_place, source.get());

    std::weak_ptr<detail::ChainedNode<T, State>> weak = node;
    node->upstream = source.on_changed(
        [weak, &timer, cooldown](const T& v) {
            auto n = weak.lock();
            if (!n) return;
            bool expected = false;
            if (!n->state.blocked.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return;  // currently in cooldown — drop
            }
            n->property.set(v);
            timer.post_after(cooldown, [weak]() {
                if (auto nn = weak.lock()) {
                    nn->state.blocked.store(false, std::memory_order_release);
                }
            });
        });

    return detail::expose_property(std::move(node));
}

// ════════════════════════════════════════════════════════════════════════════
// scan (a.k.a. fold / accumulate)
// ════════════════════════════════════════════════════════════════════════════
template<PropertyValue T, PropertyValue Acc, typename Reducer>
[[nodiscard]] std::shared_ptr<Property<Acc>>
scan(Property<T>& source, Acc seed, Reducer reduce) {
    struct State {
        Acc                                  acc;
        std::function<Acc(const Acc&, const T&)> reducer;
    };

    auto node = std::make_shared<detail::ChainedNode<Acc, State>>(
        std::in_place,
        seed,                                       // Property<Acc> initial
        seed,                                       // State::acc
        [r = std::move(reduce)](const Acc& a, const T& v) {  // State::reducer
            return r(a, v);
        });

    std::weak_ptr<detail::ChainedNode<Acc, State>> weak = node;
    node->upstream = source.on_changed([weak](const T& v) {
        if (auto n = weak.lock()) {
            n->state.acc = n->state.reducer(n->state.acc, v);
            n->property.set(n->state.acc);
        }
    });

    return detail::expose_property(std::move(node));
}

// ════════════════════════════════════════════════════════════════════════════
// combine_latest
//   Combine the latest values of two source Properties through a binary
//   function into a derived Property. Emits whenever EITHER source changes,
//   carrying the most recent value of the other.
//
//   Property<int>    a{1};
//   Property<int>    b{2};
//   auto sum = combine_latest(a, b, [](int x, int y){ return x + y; });
//   // *sum == 3; a = 10 → *sum == 12; b = 5 → *sum == 15
//
//   Note: for the common case where the combiner only READS reactive
//   sources, `Computed<R>([&]{ return f(a.get(), b.get()); })` is the more
//   idiomatic, glitch-free path (it participates in the dependency graph).
//   `combine_latest` is provided for parity with Rx and for combining
//   sources that are NOT both reactive-graph nodes, or when a detached
//   shared_ptr<Property<R>> handle (rather than a graph Computed) is wanted.
// ════════════════════════════════════════════════════════════════════════════
template<PropertyValue A, PropertyValue B, typename Combiner>
[[nodiscard]] auto
combine_latest(Property<A>& a, Property<B>& b, Combiner combine)
    -> std::shared_ptr<Property<std::invoke_result_t<Combiner, const A&, const B&>>> {
    using R = std::invoke_result_t<Combiner, const A&, const B&>;
    struct State {
        A    last_a;
        B    last_b;
        std::function<R(const A&, const B&)> fn;
        // Second upstream subscription (source `b`). The node's own
        // `upstream` member holds source `a`; this one holds `b`. Both are
        // torn down when the node dies (State is destroyed after `upstream`,
        // which is fine — both callbacks are weak-guarded against the node).
        Subscription b_sub;
    };

    auto node = std::make_shared<detail::ChainedNode<R, State>>(
        std::in_place,
        combine(a.get(), b.get()),                 // Property<R> initial
        a.get(),                                   // State::last_a
        b.get(),                                   // State::last_b
        [c = std::move(combine)](const A& x, const B& y) { return c(x, y); },
        Subscription{});                           // State::b_sub (wired below)

    std::weak_ptr<detail::ChainedNode<R, State>> weak = node;
    node->upstream = a.on_changed([weak](const A& v) {
        if (auto n = weak.lock()) {
            n->state.last_a = v;
            n->property.set(n->state.fn(n->state.last_a, n->state.last_b));
        }
    });
    node->state.b_sub = b.on_changed([weak](const B& v) {
        if (auto n = weak.lock()) {
            n->state.last_b = v;
            n->property.set(n->state.fn(n->state.last_a, n->state.last_b));
        }
    });

    return detail::expose_property(std::move(node));
}

}  // namespace aria
