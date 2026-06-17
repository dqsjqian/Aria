#pragma once

// ============================================================================
//  reactive/computed.hpp
// ----------------------------------------------------------------------------
//  `Computed<T>` is a **Derivation node**: a read-only value produced from
//  other reactive sources via a user-supplied compute function.
//
//  Dependency discovery
//  --------------------
//  Automatic. During every recompute the Graph installs a fresh
//  TrackingContext; every Property::get() (or nested Computed::get())
//  called from the compute function registers itself as an upstream.
//  Conditional branches therefore "just work": the next recompute gathers
//  a brand-new edge set, and the old edges (that were not re-visited)
//  are quietly dropped.
//
//      Property<bool> flag{true};
//      Property<int>  a{1}, b{2};
//      Computed<int>  value{[&]{ return flag.get() ? a.get() : b.get(); }};
//      // value currently depends on {flag, a}. If flag flips to false,
//      // the next recompute will depend on {flag, b}.
//
//  Laziness & memoization
//  ----------------------
//  * `get()` is always cheap: it returns the cached value plus (if a
//    tracker is active) registers self as an upstream read.
//  * Recompute is triggered by the Graph's flush pass, which only runs
//    Derivations whose upstream versions actually moved.
//  * Equal-to-previous results short-circuit downstream propagation,
//    keeping the graph glitch-free.
//
//  Replaces the two legacy concepts (`AutoComputed` and the explicit-deps
//  `Computed`) with a single unified name. The "explicit deps" ergonomic
//  is still supported: callers who want to track something they did not
//  actually read can call `dep(x)` inside the body.
// ============================================================================

#include "aria/reactive/graph.hpp"
#include "aria/reactive/node.hpp"

#include "aria/concepts.hpp"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

// Forward declaration of the unified subscription handle -- its definition
// lives in <aria/subscription.hpp>, which is included by effect.hpp
// where the Computed observer methods are actually defined.
namespace aria { class Subscription; }

namespace aria::reactive {

// ---------------------------------------------------------------------------
//  dep(x)
//  ------
//  Explicit hint: "treat `x` as an upstream dependency of the currently
//  recomputing Derivation/Reaction, even if we do not read its value".
//
//  For the 99% case where you *do* read the value, you can simply call
//  `x.get()` -- auto-tracking picks it up. `dep(x)` exists for the edge
//  cases where a dependency is inferred from the mere *presence* of a
//  value (for instance, "recompute when the user clicks", where the
//  Property<int> holds a click counter you do not care about otherwise).
// ---------------------------------------------------------------------------
template<class Reactive>
    requires ::aria::ReactiveNode<Reactive>
void dep(Reactive& r) noexcept {
    if (auto* t = Node::graph().current_tracker()) {
        t->record_read(static_cast<Node&>(r));
    }
}

// ---------------------------------------------------------------------------
//  Computed<T>
// ---------------------------------------------------------------------------
template<PropertyValue T>
class Computed final : public Node {
public:
    using value_type = T;

    // S-30 second tier (see Property<T>).
    static_assert(std::copyable<T>,
        "Computed<T> requires T to be copyable: every observer is handed "
        "a copy of the latest computed value.");
    static_assert(EqualityComparable<T>,
        "Computed<T> requires T to be equality-comparable: equal-to-cached "
        "recomputes are skipped, which is what keeps the graph glitch-free.");

    /// Construct and perform the initial compute eagerly, so `get()`
    /// immediately returns the correct value without any flush.
    /// A deliberate choice: eager first-run keeps observer semantics
    /// simple (observers never see "empty" / default-constructed state).
    template<std::invocable<> Fn>
        requires std::convertible_to<std::invoke_result_t<Fn>, T>
    explicit Computed(Fn fn)
        : Node(NodeKind::Derivation),
          compute_(std::move(fn)) {
        // Eager initial evaluation. Bump version to 1 (Node starts at 1
        // already; we overwrite via the standard recompute path so edges
        // are registered correctly).
        (void)recompute();
    }

    Computed(const Computed&)            = delete;
    Computed& operator=(const Computed&) = delete;
    Computed(Computed&&)                 = delete;
    Computed& operator=(Computed&&)      = delete;

    /// Explicit destructor: detach every upstream edge **before** the
    /// edge pool releases the storage backing them. If we rely on the
    /// default destruction sequence, `edge_pool_` (a derived-class member)
    /// runs first and frees every Edge object; then `~Node()` would call
    /// `clear_sources()` and dereference the already-freed list head ->
    /// classic use-after-free SIGSEGV.
    ~Computed() noexcept override {
        clear_sources();
        // `edge_pool_` will now destroy safely: each Edge slot is no
        // longer referenced by any source node's intrusive list.
    }

    // ── Read ────────────────────────────────────────────────────────────

    /// Return the cached value, ensuring it is up to date. If the graph
    /// has pending changes that could affect us, we pull ourselves first
    /// so the caller always sees a glitch-free value.
    [[nodiscard]] T get() const {
        auto& g = graph();
        // If we are MaybeDirty/Dirty, synchronously evaluate. This makes
        // Computed::get() usable in non-flush contexts (e.g. a unit test
        // that reads a Computed right after writing to its source,
        // without wrapping in batch()).
        if (state() != NodeState::Clean) {
            const_cast<Computed*>(this)->pull_self_();
        }
        // Register as an upstream of any outer Derivation/Reaction.
        if (auto* t = g.current_tracker()) {
            t->record_read(const_cast<Computed&>(*this));
        }
        return cached_;
    }

    /// Read by const reference. Same semantics as `get()` (auto-tracking
    /// + lazy pull) but avoids the copy on hot paths.
    [[nodiscard]] const T& get_ref() const {
        auto& g = graph();
        if (state() != NodeState::Clean) {
            const_cast<Computed*>(this)->pull_self_();
        }
        if (auto* t = g.current_tracker()) {
            t->record_read(const_cast<Computed&>(*this));
        }
        return cached_;
    }

    /// Non-tracking snapshot read. `noexcept` is conditional on T's
    /// copy ctor; if T may throw we still don't want to terminate.
    [[nodiscard]] T peek() const noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return cached_;
    }

    /// Non-tracking snapshot read by const reference.
    [[nodiscard]] const T& peek_ref() const noexcept { return cached_; }

    operator T() const { return get(); }

    // ── Observe ─────────────────────────────────────────────────────────
    // Defined out-of-line in computed_observe.inl (after Observer/Reaction
    // are visible) to avoid forward-declaration gymnastics.
    [[nodiscard]] ::aria::Subscription on_changed(std::function<void(const T&)> fn);
    [[nodiscard]] ::aria::Subscription bind(std::function<void(const T&)> fn);
    [[nodiscard]] ::aria::Subscription observe(std::function<void(const T&, const T&)> fn);

    // ── Graph integration ────────────────────────────────────────────────

    /// Called by the Graph when an upstream has changed. Re-establishes
    /// edges from scratch using a fresh TrackingContext and commits the
    /// new value, returning `true` iff the cached value actually moved.
    ///
    /// Exception safety: if the user-supplied `compute_()` throws, we
    /// MUST keep the previous edge set intact — otherwise this Computed
    /// becomes orphaned in the graph and never recomputes again, even
    /// after the upstream stabilises. We achieve this by computing the
    /// new value FIRST under a temporary tracker (without touching our
    /// existing edges), and only swapping edges in once the user code
    /// has succeeded.
    bool recompute() override {
        // 1. Run the user lambda under a fresh tracker, leaving our
        //    existing edges untouched. If compute_() throws, we
        //    propagate the exception with our previous dependency set
        //    fully intact — the graph will continue to wake us up on
        //    the next upstream change.
        TrackingContext ctx;
        T              new_val;
        {
            TrackerScope guard(ctx);
            new_val = compute_();
        }

        // 2. Compute succeeded. Now swap dependency sets: detach old
        //    edges, then re-attach from `ctx.reads()`.
        //
        //    Edge storage is REUSED across recomputes. `clear_sources()`
        //    only unlinks the Edge objects from their upstreams' intrusive
        //    lists; the Edge slots themselves stay in `edge_pool_` (a
        //    std::deque, so element addresses are stable — required because
        //    `attach_as_observer_of` threads `&edge` into the graph's
        //    intrusive lists). The pool's capacity grows monotonically to
        //    the deepest dependency-set size ever observed and is reused
        //    thereafter, so a Computed whose dependency set is stable does
        //    ZERO heap allocation per recompute. This is what makes the
        //    "no hidden allocations on the hot path" contract hold for
        //    Derivation re-evaluation, not just Property set/get.
        clear_sources();

        const std::vector<Node*>& reads = ctx.reads();

        // Reset depth so it can shrink as well as grow when the
        // dependency set shifts (otherwise depth is monotonically
        // non-decreasing across recomputes, which inflates the cost
        // of `flush()`'s topological sort over time).
        set_depth(0);

        // Grow the pool only when this recompute needs more edges than
        // we have ever needed before. Existing slots are reused in place.
        while (edge_pool_.size() < reads.size()) {
            edge_pool_.emplace_back();
        }
        for (std::size_t i = 0; i < reads.size(); ++i) {
            // `attach_as_observer_of` fully (re)initialises the Edge's
            // source/observer/version and link pointers, so a recycled
            // slot is safe to reuse without an explicit reset.
            attach_as_observer_of(*reads[i], edge_pool_[i]);
        }
        active_edges_ = reads.size();

        // 3. Commit and report change-or-not to the Graph.
        // eq-gate: if value did not move, downstream propagation stops.
        if (cached_ == new_val) return false;
        cached_ = std::move(new_val);
        bump_version_();
        return true;
    }

    /// Number of upstreams currently in use. Exposed for diagnostics /
    /// tests; not part of the user-facing API surface.
    [[nodiscard]] std::size_t dependency_count() const noexcept {
        return active_edges_;
    }

private:
    /// Force an evaluation if our state is non-Clean. Used by `get()` so
    /// callers do not have to manually flush before reading.
    void pull_self_() {
        // Graph::pull handles the MaybeDirty/Dirty state machine.
        graph().pull(*this);
    }

    std::function<T()>                  compute_;
    T                                   cached_{};
    /// Edge storage pool. Reused across recomputes — see `recompute()`.
    /// `std::deque` is chosen over `std::vector` because the graph stores
    /// `&edge` (Edge addresses) in intrusive linked lists, so the storage
    /// must NOT relocate elements on growth. The pool grows to the
    /// high-water-mark dependency count and is reused thereafter; only the
    /// first `active_edges_` slots are live at any moment.
    std::deque<Edge>                    edge_pool_;
    std::size_t                         active_edges_ = 0;
};

}  // namespace aria::reactive
