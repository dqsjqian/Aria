// ============================================================================
//  aria/async/async_validator.hpp
// ----------------------------------------------------------------------------
//  Async validation rules as a first-class citizen of `Validator<T>`.
//  Callers should not have to hand-roll cancellation,
//  de-duplication, or "latest-wins" arbitration when a validation rule
//  is asynchronous (e.g. "is this username taken?" hitting the network).
//
//  Design contract (V-N IDs, cross-referenced from docs/error-model.md
//  E-21 and lifecycle.md L-37):
//
//    V-1 (latest-wins). Each fresh source-property change cancels the
//        previous in-flight rule; the cancelled rule's result MUST be
//        dropped (it never reaches `Validator::end_pending`). Mirrors
//        AsyncResource R-1.
//
//    V-2 (pending semantics). Between fire and settle the validator
//        sits in `ValidationState.pending == true`. UI consumes
//        `state().pending` for spinner / disable-submit.
//
//    V-3 (cancellation never surfaces as Error). Per error-model.md
//        E-22, a rule that observed cancellation MUST NOT add any
//        Error to `state.errors`. The validator simply settles back
//        to its previous error set.
//
//    V-4 (key + rule_id). Async errors live under
//        `ValidationKey{validator.field_path(), rule_id}`. This makes
//        them indistinguishable from sync rules at the form level.
//
//    V-5 (de-duplication). Two consecutive identical source values
//        do NOT spawn two rules; the second is a no-op. Avoids
//        spamming a slow remote validator on Property echo / re-emit.
//
//    V-6 (lifetime). Destroying the AsyncValidator cancels any
//        in-flight rule; detaching via the returned Subscription
//        does the same.
// ============================================================================
#pragma once

#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/validation_key.hpp"
#include "aria/validator.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace aria::async {

/// Outcome of an async rule invocation. The same shape covers
/// "passed" (no error, optional warnings) and "failed" (one or more
/// errors). Cancellation is not a result -- a cancelled rule never
/// produces an `AsyncRuleResult`.
struct AsyncRuleResult {
    /// Hard failures. Empty iff the rule passed.
    std::vector<::aria::Error> errors;

    /// Soft advisories.
    std::vector<::aria::Error> warnings;

    [[nodiscard]] static AsyncRuleResult passed() { return {}; }

    /// Convenience: build a single-error failure under the given key.
    [[nodiscard]] static AsyncRuleResult failed(::aria::ValidationKey key,
                                                std::string message) {
        AsyncRuleResult r;
        r.errors.push_back(
            ::aria::Error::validation(std::move(key), std::move(message)));
        return r;
    }
};

namespace detail {

template<class T>
struct AsyncValidatorState
    : std::enable_shared_from_this<AsyncValidatorState<T>>
{
    using Factory = std::function<
        Task<AsyncRuleResult>(T, ::aria::async::CancellationToken)>;

    IExecutor*                        ui;
    IExecutor*                        worker;
    Factory                           factory;
    ::aria::async::CancellationSource cancel;

    // Latest-wins generation counter. Each fire bumps it; only the
    // run whose `my_gen` still equals `gen` on completion gets to
    // settle the validator.
    std::atomic<std::uint64_t>        gen{0};

    // V-5 de-dupe: stash the most recent source value and skip
    // identical successors. Stored as optional so the "first ever
    // fire" is never accidentally suppressed.
    std::optional<T>                  last_value;

    // Validator pointer is cleared when the Subscription detaches; a
    // subsequent stale-rule completion sees nullptr and drops.
    ::aria::Validator<T>*             target{nullptr};

    AsyncValidatorState(IExecutor& u, IExecutor& w, Factory f)
        : ui(&u), worker(&w), factory(std::move(f)) {}
};

template<class T>
Task<void> async_validator_run_one_(
    std::shared_ptr<AsyncValidatorState<T>> self,
    T                                       value,
    std::uint64_t                           my_gen)
{
    auto tok = self->cancel.token();

    std::optional<AsyncRuleResult> outcome;
    std::optional<::aria::Error>   failure;
    try {
        co_await schedule_on(*self->worker);
        tok.throw_if_cancelled();
        outcome = co_await self->factory(std::move(value), tok);
        tok.throw_if_cancelled();
    } catch (const OperationCancelled&) {
        // V-3 -- silent.
        co_return;
    } catch (...) {
        // V-3 -- arbitrary throws map to AsyncFailure under the
        // validator's source tag. The error message follows
        // error-model.md E-13 (What/Where/How).
        failure = ::aria::Error::from_exception(
            std::current_exception(), "AsyncValidator");
    }

    co_await schedule_on(*self->ui);

    // Stale-result guard (V-1). Mirrors AsyncResource R-1: a stale
    // run does NOT touch the validator's pending state; the winner
    // clears it.
    if (self->gen.load(std::memory_order_acquire) != my_gen) {
        co_return;
    }
    if (!self->target) {
        // Detached mid-flight; nothing to settle.
        co_return;
    }

    std::vector<::aria::Error> extras;
    if (failure.has_value()) {
        extras.push_back(std::move(*failure));
    } else if (outcome.has_value()) {
        for (auto& e : outcome->errors)   extras.push_back(std::move(e));
        for (auto& w : outcome->warnings) extras.push_back(std::move(w));
    }
    self->target->end_pending(std::move(extras));
    co_return;
}

}  // namespace detail

/// Driver that turns a coroutine factory into a latest-wins async
/// validation rule attached to a `Validator<T>`.
///
/// Typical usage:
///
///     AsyncValidator<std::string> av{
///         ui_executor, worker_pool,
///         [&](std::string username, CancellationToken tok)
///             -> Task<AsyncRuleResult> {
///             tok.throw_if_cancelled();
///             bool taken = co_await api.check_username(username);
///             tok.throw_if_cancelled();
///             if (taken) {
///                 co_return AsyncRuleResult::failed(
///                     ValidationKey{"signup.username", "remote_taken"},
///                     "Username already taken");
///             }
///             co_return AsyncRuleResult::passed();
///         }};
///     Subscription sub = av.attach_to(validator, source_property);
///
/// On every change of `source_property` the AsyncValidator cancels
/// the previous rule, fires a new one, and surfaces the result via
/// `validator.begin_pending()` / `end_pending(...)`.
template<class T>
class AsyncValidator {
public:
    using Factory = typename detail::AsyncValidatorState<T>::Factory;

    AsyncValidator(IExecutor& ui, IExecutor& worker, Factory factory)
        : state_(std::make_shared<detail::AsyncValidatorState<T>>(
              ui, worker, std::move(factory))) {}

    ~AsyncValidator() {
        if (state_) state_->cancel.cancel();
    }

    AsyncValidator(const AsyncValidator&)            = delete;
    AsyncValidator& operator=(const AsyncValidator&) = delete;
    AsyncValidator(AsyncValidator&&) noexcept        = default;
    AsyncValidator& operator=(AsyncValidator&&) noexcept = default;

    /// Attach to a Validator + its source Property. The returned
    /// Subscription owns the lifetime: destroying it detaches the
    /// validator and cancels any in-flight rule (V-6).
    [[nodiscard]] ::aria::Subscription attach_to(::aria::Validator<T>& v,
                                                 ::aria::Property<T>&  source) {
        auto state    = state_;
        state->target = &v;

        // Initial fire on the current value, so binding immediately
        // produces a pending state for the user (matches the sync
        // `Validator::rule` behaviour, which runs on attach).
        state->last_value.reset();
        fire_(state, source.get());

        auto inner = source.on_changed(
            [state](const T& value) { fire_(state, value); });

        return ::aria::Subscription{std::function<void()>{
            [state, holder = std::make_shared<::aria::Subscription>(
                std::move(inner))]() mutable {
                state->cancel.cancel();
                state->target = nullptr;
                if (holder) {
                    holder->detach();
                    holder.reset();
                }
            }}};
    }

private:
    static void fire_(const std::shared_ptr<detail::AsyncValidatorState<T>>& state,
                      const T& value)
    {
        if (state->last_value.has_value() && *state->last_value == value) {
            // V-5 -- identical to last fire, no-op.
            return;
        }
        state->last_value = value;

        // V-1 -- cancel any prior in-flight rule, then mint a fresh
        // CancellationSource for the new run.
        state->cancel.cancel();
        state->cancel = ::aria::async::CancellationSource{};

        if (state->target) state->target->begin_pending();   // V-2

        const auto my_gen =
            state->gen.fetch_add(1, std::memory_order_acq_rel) + 1;
        detail::async_validator_run_one_<T>(state, value, my_gen)
            .start_detached();
    }

    std::shared_ptr<detail::AsyncValidatorState<T>> state_;
};

}  // namespace aria::async
