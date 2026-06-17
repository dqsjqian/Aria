#pragma once

#include "aria/subscription.hpp"
#include "aria/diagnostics.hpp"
#include "aria/detail/typed_signal.hpp"
#include "aria/reactive/reactive.hpp"  // Effect -- auto-tracks reads in predicate()

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace aria {

/// Encapsulated user action with an optional `CanExecute` predicate.
///
/// Two flavours:
///
///   * `Command<>` (parameterless) — the predicate is a `() -> bool` closure.
///     On construction the command installs a reactive `Effect` that re-runs
///     the predicate whenever any reactive value it reads changes, and emits
///     a `can_execute` signal with the new truth value.
///     **Bound buttons auto-update `enabled` with no manual plumbing.**
///
///   * `Command<Args...>` (parameterised) — the predicate takes the same
///     arguments the View supplies at click time. Because those arguments
///     are only known at invocation, we cannot run the predicate eagerly
///     to register reactive dependencies; callers that want `enabled` to
///     reflect model state changes must call `notify_can_execute_changed(args...)`
///     explicitly. (The `BindingEngine::bind_command` helper will also
///     re-evaluate on demand for fixed args.)
///
/// ## Lifetime contract (READ BEFORE CAPTURING REFERENCES)
///
/// 1. **Declare `Command<>` AFTER every Property / Computed its predicate
///    reads.** The no-arg specialisation runs the predicate once at
///    construction to register its reactive dependency set; reading a
///    not-yet-constructed member is undefined behaviour.
///
/// 2. **Keep the predicate pure and side-effect free.** The internal
///    Effect may re-run it any time an upstream changes, and re-runs are
///    coalesced by the reactive graph; observable side effects will
///    appear non-deterministic.
///
/// 3. **A Property freed before the Command it feeds is undefined
///    behaviour — avoid it.** Normal ViewModel usage co-owns both, so
///    they die together at the owning scope's end. The reactive graph
///    detaches the Effect's source edge when Property's `~Node` runs, so
///    the Command does not crash on destruction, but re-reading the
///    predicate (via `notify_can_execute_changed()`) after an upstream
///    was freed is UB (the predicate captures a reference to freed
///    storage). See the test `Command<>: Property freed before Command`
///    for the one case that IS safe: don't touch the command again, just
///    let it destruct.
template<typename... Args>
class Command {
public:
    using Action = std::function<void(Args...)>;
    using Predicate = std::function<bool(const Args&...)>;
    using CanExecuteSignal = detail::TypedSignal<bool>;

    template<std::invocable<Args...> A>
    explicit Command(A&& action)
        : action_(std::forward<A>(action)),
          predicate_([](const Args&...) noexcept { return true; }),
          can_signal_(std::make_shared<CanExecuteSignal>()) {}

    template<std::invocable<Args...> A, std::predicate<const Args&...> P>
    Command(A&& action, P&& predicate)
        : action_(std::forward<A>(action)),
          predicate_(std::forward<P>(predicate)),
          can_signal_(std::make_shared<CanExecuteSignal>()) {}

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    // Non-movable: a Command's identity is tied to its `can_signal_` and
    // (for Command<>) the auto-tracking Effect, which captures a *copy* of
    // the predicate at construction. Allowing moves would let the moved-to
    // Command's `predicate_` diverge from the copy the Effect still calls,
    // and silently re-home reactive plumbing. ViewModels hold Commands as
    // direct members (never moved), exactly like Property / Computed, so
    // deleting move costs nothing and removes a footgun.
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    /// Invoke the action if can_execute(args...) is true.
    void execute(const Args&... args) {
        if (predicate_(args...)) {
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Command,
                    ::aria::trace::Command{"execute"});
            }
            action_(args...);
        } else if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Command,
                ::aria::trace::Command{"rejected_can_execute"});
        }
    }

    void operator()(const Args&... args) { execute(args...); }

    [[nodiscard]] bool can_execute(const Args&... args) const {
        return predicate_(args...);
    }

    /// Manually notify observers that can_execute may have changed.
    ///
    /// For `Command<Args...>` this is the primary mechanism (we cannot
    /// auto-track a predicate whose inputs we do not know).
    /// For `Command<>` callers normally never need to call this -- the
    /// built-in Effect already re-evaluates automatically -- but it is
    /// retained for force-refresh scenarios (e.g. external state that
    /// isn't expressed as a Property).
    void notify_can_execute_changed(const Args&... args) const {
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Command, 
                ::aria::trace::Command{"can_execute_changed"});
        }
        can_signal_->emit(predicate_(args...));
    }

    [[nodiscard]] Subscription observe_can_execute(std::function<void(bool)> fn) {
        return can_signal_->connect(std::move(fn));
    }

private:
    Action action_;
    Predicate predicate_;
    std::shared_ptr<CanExecuteSignal> can_signal_;
};

// ----------------------------------------------------------------------------
//  Command<> specialisation: parameterless predicate can be auto-tracked.
//
//  The predicate is wrapped in an `Effect`; every reactive read inside it
//  becomes an upstream edge, and the `Effect` re-fires whenever any of
//  those upstreams changes, emitting the fresh `can_execute()` on the
//  signal. As a result `BindingEngine::bind_command(cmd, button)` is
//  enough for the button's `enabled` to stay in sync with model state —
//  callers no longer need to sprinkle `notify_can_execute_changed()` in
//  their setters. This is the behaviour the docs describe.
// ----------------------------------------------------------------------------
template<>
class Command<> {
public:
    using Action = std::function<void()>;
    using Predicate = std::function<bool()>;
    using CanExecuteSignal = detail::TypedSignal<bool>;

    template<std::invocable<> A>
    explicit Command(A&& action)
        : action_(std::forward<A>(action)),
          predicate_([]() noexcept { return true; }),
          can_signal_(std::make_shared<CanExecuteSignal>()) {
        // `can_execute` is a constant `true`; no Effect needed.
    }

    template<std::invocable<> A, std::predicate<> P>
    Command(A&& action, P&& predicate)
        : action_(std::forward<A>(action)),
          predicate_(std::forward<P>(predicate)),
          can_signal_(std::make_shared<CanExecuteSignal>()) {
        // ── Eager auto-tracking contract ─────────────────────────────
        // We install a reactive `Effect` right here in the constructor.
        // The Effect runs the predicate exactly once synchronously, under
        // a TrackingContext, so every reactive value it reads becomes an
        // upstream edge. Subsequent changes to any of those upstreams
        // re-fire the Effect, which then emits on `can_signal_` **only
        // when the truth value actually flips** (equality gate).
        //
        // What this means for callers:
        //   * The predicate executes **once** during construction.
        //     It must therefore be safe to run at that moment — every
        //     Property / Computed it touches via `this->member` must
        //     already be fully constructed. Because C++ initialises
        //     non-static data members in declaration order, this is
        //     satisfied iff the Command<> is declared **after** every
        //     Property / Computed whose values its predicate reads.
        //     (Standard MVVM style already does this.)
        //   * Predicate should be side-effect free. Tracking is eager
        //     rather than lazy so that `can_execute()` is authoritative
        //     from the moment the Command exists and observers attached
        //     later do not need a priming emit.
        //   * The Effect stays alive for the lifetime of the Command,
        //     independent of any UI binding. This is intentional: a
        //     Command owns its own reactive plumbing so a ViewModel
        //     doesn't have to wire / unwire it in activate / deactivate.
        //     The cost is O(deps) shared_ptr / edge nodes; there is NO
        //     per-change work while upstreams stay stable.
        auto signal = can_signal_;
        auto last   = std::make_shared<bool>();
        effect_.emplace(
            [pred = predicate_, signal, last,
             primed = std::make_shared<bool>(false)]() mutable {
                const bool now = pred();
                if (!*primed) {
                    // First (eager) run: seed the cache and register
                    // every tracked source, but do NOT emit — no one
                    // can have observed us yet, and callers that
                    // connect later should read `can_execute()`
                    // synchronously to get the current state.
                    *primed = true;
                    *last   = now;
                    return;
                }
                if (*last != now) {
                    *last = now;
                    signal->emit(now);
                }
            });
    }

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    // Non-movable — see the primary template. The auto-tracking Effect
    // captures a copy of the predicate, so a move would desync it.
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    void execute() {
        if (predicate_()) {
            if (::aria::has_trace_sink()) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Command,
                    ::aria::trace::Command{"execute"});
            }
            action_();
        } else if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Command,
                ::aria::trace::Command{"rejected_can_execute"});
        }
    }

    void operator()() { execute(); }

    [[nodiscard]] bool can_execute() const { return predicate_(); }

    /// Force-emit a `can_execute` notification. Rarely needed for
    /// `Command<>` (the built-in Effect handles reactive state
    /// automatically); provided as an escape hatch when the predicate
    /// depends on non-reactive state.
    void notify_can_execute_changed() const {
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Command,
                ::aria::trace::Command{"can_execute_changed"});
        }
        can_signal_->emit(predicate_());
    }

    [[nodiscard]] Subscription observe_can_execute(std::function<void(bool)> fn) {
        return can_signal_->connect(std::move(fn));
    }

private:
    Action action_;
    Predicate predicate_;
    std::shared_ptr<CanExecuteSignal> can_signal_;
    // Effect owns the reactive node. `std::optional<Effect>` rather than a
    // bare `Effect` so the default-true-predicate constructor can leave it
    // empty (no auto-tracking node installed) without paying for a dummy
    // Effect. The `std::nullopt` state represents "no auto-tracking effect
    // was installed".
    std::optional<reactive::Effect> effect_;
};

}  // namespace aria
