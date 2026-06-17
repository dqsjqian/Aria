#pragma once

// ============================================================================
//  reactive/effect.hpp
// ----------------------------------------------------------------------------
//  `Effect` is the public Reaction primitive: it runs a user function
//  every time any reactive value it read has changed.
//
//  Conceptual model
//  ----------------
//  * Effects are *pure side effects* -- they produce no value.
//  * Effects are auto-tracked (same mechanism as Computed).
//  * Effects live as long as the `Effect` object is alive. Destroying the
//    object disconnects it from the graph in O(deps).
//  * For a library-user, `Effect` is the direct equivalent of MobX's
//    `autorun`, SolidJS's `createEffect`, or Svelte 5's `$effect`.
//
//  Typical usage
//  -------------
//      Property<int> count{0};
//      Effect logger{[&]{
//          std::cout << "count is now " << count.get() << '\n';
//      }};
//      count = 1;   // prints "count is now 1"
//      count = 2;   // prints "count is now 2"
//
//  This file also provides the out-of-line implementations of
//  `Computed<T>::on_changed / bind / observe`, which share the same
//  ReactionNode machinery as Effect.
// ============================================================================

#include "aria/reactive/graph.hpp"
#include "aria/reactive/node.hpp"
#include "aria/reactive/property.hpp"     // for detail::ReactionNode
#include "aria/reactive/computed.hpp"

#include "aria/subscription.hpp"  // unified aria::Subscription handle

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

namespace aria::reactive {

// ---------------------------------------------------------------------------
//  Internal: an auto-tracking ReactionNode whose body is re-run on every
//  upstream change. Unlike `detail::ReactionNode` (a plain callback with
//  hand-wired edges for Property::on_changed), this one discovers its
//  dependencies via a TrackingContext on each run -- making it suitable
//  for arbitrary user lambdas (`Effect`).
// ---------------------------------------------------------------------------
namespace detail {

class AutoReactionNode final : public Node {
public:
    explicit AutoReactionNode(std::function<void()> fn)
        : Node(NodeKind::Reaction), fn_(std::move(fn)) {
        // Eager first run so that side effects fire immediately, mirroring
        // MobX's autorun / SolidJS's createEffect contract.
        (void)recompute();
    }

    ~AutoReactionNode() override {
        // See detail::ReactionNode / Computed for the rationale: derived
        // members (`edge_pool_`) are destroyed before the `Node` base, so
        // we must detach upstream edges here, while the backing storage is
        // still alive, to avoid a use-after-free in `~Node`.
        clear_sources();
    }

    /// Drop current edges, re-run body under a fresh tracker, reconcile
    /// the new edge set. Mirrors `Computed::recompute` (minus the
    /// cached-value bookkeeping) including the exception-safety
    /// rebuild order: gather the new dependency set FIRST, only then
    /// detach the old edges. If `fn_()` throws we propagate the
    /// exception with our prior edge set still intact, so the graph
    /// keeps waking us up on subsequent upstream changes.
    ///
    /// Edge storage is REUSED across recomputes (see Computed::recompute
    /// for the full rationale): a stable-dependency Effect that re-fires
    /// on every upstream change does ZERO heap allocation per run. The
    /// pool is a std::deque so Edge addresses stay stable while it grows.
    bool recompute() override {
        TrackingContext ctx;
        {
            TrackerScope guard(ctx);
            if (fn_) fn_();
        }

        // User body succeeded — swap dependency sets, reusing edge slots.
        clear_sources();

        const std::vector<Node*>& reads = ctx.reads();

        // Reset depth so it can shrink, not just grow, across
        // recomputes whose dependency set narrowed.
        set_depth(0);
        while (edge_pool_.size() < reads.size()) {
            edge_pool_.emplace_back();
        }
        for (std::size_t i = 0; i < reads.size(); ++i) {
            attach_as_observer_of(*reads[i], edge_pool_[i]);
        }
        active_edges_ = reads.size();

        // Reactions do not have a value, so they never need to propagate
        // any further. Returning `false` stops the graph cleanly.
        return false;
    }

private:
    std::function<void()> fn_;
    /// Reused edge storage — see Computed::edge_pool_ for why std::deque.
    std::deque<Edge>      edge_pool_;
    std::size_t           active_edges_ = 0;
};

}  // namespace detail

// ---------------------------------------------------------------------------
//  Effect -- user-facing Reaction wrapper. The Effect object owns the
//  underlying AutoReactionNode; destroying the Effect detaches it.
// ---------------------------------------------------------------------------
class Effect {
public:
    /// Runs `fn` once eagerly (to collect its initial dependency set),
    /// then automatically re-runs it whenever any tracked read changes.
    template<std::invocable<> Fn>
    explicit Effect(Fn fn)
        : node_(std::make_shared<detail::AutoReactionNode>(std::move(fn))) {}

    Effect(const Effect&)            = delete;
    Effect& operator=(const Effect&) = delete;
    Effect(Effect&&) noexcept                 = default;
    Effect& operator=(Effect&&) noexcept      = default;

    /// Explicitly cancel (without waiting for destruction).
    void stop() noexcept { node_.reset(); }

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(node_); }

    /// Transfer ownership into a unified `aria::Subscription`, so that
    /// an Effect can be dropped into any SubscriptionBag alongside other
    /// observers (Property::on_changed, EventBus::subscribe, ...).
    [[nodiscard]] ::aria::Subscription into_subscription() && noexcept {
        return ::aria::Subscription{std::move(node_)};
    }

private:
    std::shared_ptr<detail::AutoReactionNode> node_;
};

// ---------------------------------------------------------------------------
//  Computed<T>::on_changed / bind / observe -- deferred definitions.
// ---------------------------------------------------------------------------
template<PropertyValue T>
::aria::Subscription Computed<T>::on_changed(std::function<void(const T&)> fn) {
    // `AutoReactionNode`'s constructor runs the body eagerly once to
    // collect its dependency set. For `on_changed` we must NOT surface
    // that first run to the user callback (otherwise it would fire with
    // the current value as a "change" event, diverging from
    // `Property::on_changed`). A shared "primed" flag swallows the first
    // invocation; subsequent ones are real changes.
    auto primed = std::make_shared<bool>(false);
    auto reaction = std::make_shared<detail::AutoReactionNode>(
        [this, fn = std::move(fn), primed] {
            const auto& v = this->get();
            if (!*primed) { *primed = true; return; }
            fn(v);
        });
    reaction->set_debug_name("Computed::on_changed");
    return ::aria::Subscription{std::move(reaction)};
}

template<PropertyValue T>
::aria::Subscription Computed<T>::bind(std::function<void(const T&)> fn) {
    // `bind` = initial sync + follow-up updates. `AutoReactionNode` runs
    // its body eagerly in the constructor, which is exactly that
    // semantics -- no extra first-call guard needed.
    auto reaction = std::make_shared<detail::AutoReactionNode>(
        [this, fn = std::move(fn)] { fn(this->get()); });
    reaction->set_debug_name("Computed::bind");
    return ::aria::Subscription{std::move(reaction)};
}

template<PropertyValue T>
::aria::Subscription Computed<T>::observe(std::function<void(const T&, const T&)> fn) {
    auto last = std::make_shared<T>(peek());
    auto reaction = std::make_shared<detail::AutoReactionNode>(
        [this, fn = std::move(fn), last] {
            T new_val = this->get();
            T old     = std::move(*last);
            *last     = new_val;
            // Suppress the initial edge (old == new on first run) so
            // `observe` only fires on actual changes.
            if (!(old == new_val)) fn(old, new_val);
        });
    reaction->set_debug_name("Computed::observe");
    return ::aria::Subscription{std::move(reaction)};
}

}  // namespace aria::reactive
