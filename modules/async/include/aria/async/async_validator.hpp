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
#include <stdexcept>
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
    std::weak_ptr<void>               target_lifetime;
    ::aria::Subscription              source_subscription;
    std::uint64_t                     attachment{0};

    // These fields are accessed only on the graph/UI thread. The worker
    // observes only its captured token and the atomic generation counter.
    [[nodiscard]] bool has_target() const noexcept {
        return target && !target_lifetime.expired();
    }

    AsyncValidatorState(IExecutor& u, IExecutor& w, Factory f)
        : ui(&u), worker(&w), factory(std::move(f)) {}
};

template<class T>
Task<void> async_validator_run_one_(
    std::shared_ptr<AsyncValidatorState<T>> self,
    T                                       value,
    std::uint64_t                           my_gen,
    CancellationToken                       tok)
{
    std::optional<AsyncRuleResult> outcome;
    std::optional<::aria::Error>   failure;
    bool cancelled = false;
    try {
        co_await schedule_on(*self->worker);
        tok.throw_if_cancelled();
        outcome = co_await self->factory(std::move(value), tok);
        tok.throw_if_cancelled();
    } catch (const OperationCancelled&) {
        // V-3: a current run must leave pending, preserving prior errors.
        // Superseded runs are discarded by the same generation guard below.
        cancelled = true;
    } catch (...) {
        // V-3 -- arbitrary throws map to AsyncFailure under the
        // validator's source tag. The error message follows
        // error-model.md E-13 (What/Where/How).
        failure = ::aria::Error::from_exception(
            std::current_exception(), "AsyncValidator");
    }

    if (self->gen.load(std::memory_order_acquire) != my_gen) co_return;
    co_await schedule_on(*self->ui);

    // Stale-result guard (V-1). Mirrors AsyncResource R-1: a stale
    // run does NOT touch the validator's pending state; the winner
    // clears it.
    if (self->gen.load(std::memory_order_acquire) != my_gen) {
        co_return;
    }
    if (!self->has_target()) {
        // Detached mid-flight; nothing to settle.
        co_return;
    }
    if (cancelled || tok.is_cancelled()) {
        self->target->cancel_pending();
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
/// Attach, detach, move assignment and destruction run on the graph thread.
/// Both executors must outlive their queued coroutine work.
template<class T>
class AsyncValidator {
public:
    using Factory = typename detail::AsyncValidatorState<T>::Factory;

    AsyncValidator(IExecutor& ui, IExecutor& worker, Factory factory)
        : state_(std::make_shared<detail::AsyncValidatorState<T>>(
              ui, worker, std::move(factory))) {}

    ~AsyncValidator() {
        auto retired = std::move(state_);
        detach_(retired);
    }

    AsyncValidator(const AsyncValidator&)            = delete;
    AsyncValidator& operator=(const AsyncValidator&) = delete;
    AsyncValidator(AsyncValidator&&) noexcept        = default;
    AsyncValidator& operator=(AsyncValidator&& other) noexcept {
        if (this != &other) {
            auto retired = std::exchange(state_, std::move(other.state_));
            detach_(retired);
        }
        return *this;
    }

    /// Attach to a Validator + its source Property. The returned
    /// Subscription owns the lifetime: destroying it detaches the
    /// validator and cancels any in-flight rule (V-6).
    [[nodiscard]] ::aria::Subscription attach_to(::aria::Validator<T>& v,
                                                 ::aria::Property<T>&  source) {
        auto state = state_;
        if (!state) throw std::logic_error("AsyncValidator: cannot attach a moved-from driver");
        auto lifetime = v.lifetime_token_();
        const auto attachment = detach_(state);
        if (state->attachment != attachment) return {}; // Reattached during teardown.
        state->target = &v;
        state->target_lifetime = std::move(lifetime);
        state->last_value.reset();

        // The state owns the connection; its callback is weak to avoid a
        // cycle. Install it before begin_pending can notify user observers.
        std::weak_ptr<detail::AsyncValidatorState<T>> weak = state;
        state->source_subscription = source.on_changed(
            [weak, attachment](const T& value) {
                if (auto current = weak.lock(); current && current->attachment == attachment) {
                    fire_(current, value);
                }
            });
        ::aria::Subscription connection{std::function<void()>{[weak, attachment] {
            if (auto current = weak.lock(); current && current->attachment == attachment) {
                detach_(current);
            }
        }}};
        // Initial fire on the current value, as for synchronous rules.
        fire_(state, source.get());
        return connection;
    }

private:
    static std::uint64_t detach_(
        const std::shared_ptr<detail::AsyncValidatorState<T>>& state) noexcept {
        if (!state) return 0;
        const auto attachment = ++state->attachment;
        state->gen.fetch_add(1, std::memory_order_acq_rel);
        auto* target = std::exchange(state->target, nullptr);
        auto lifetime = std::move(state->target_lifetime);
        auto cancellation = std::move(state->cancel);
        state->source_subscription.release();
        // Invalidate before invoking observers/cancellation callbacks. A
        // reentrant attach then owns a fresh generation and cancellation source.
        if (target && !lifetime.expired()) {
            try { target->cancel_pending(); }
            catch (...) {
                ::aria::report_callback_failure("AsyncValidator.detach", std::current_exception());
            }
        }
        try { cancellation.cancel(); }
        catch (...) {
            ::aria::report_callback_failure("AsyncValidator.cancel", std::current_exception());
        }
        return attachment;
    }

    static void fire_(const std::shared_ptr<detail::AsyncValidatorState<T>>& state,
                      const T& value)
    {
        if (!state->has_target()) return;
        if (state->last_value.has_value() && *state->last_value == value) {
            // V-5 -- identical to last fire, no-op.
            return;
        }
        T snapshot = value;
        state->last_value = snapshot;

        CancellationSource next;
        auto previous = std::move(state->cancel);
        state->cancel = std::move(next);
        auto token = state->cancel.token();
        const auto my_gen =
            state->gen.fetch_add(1, std::memory_order_acq_rel) + 1;
        previous.cancel();
        if (state->gen.load(std::memory_order_acquire) != my_gen || !state->has_target()) return;
        state->target->begin_pending(); // May synchronously change the source or detach.
        if (state->gen.load(std::memory_order_acquire) != my_gen || !state->has_target()) return;
        detail::async_validator_run_one_<T>(state, std::move(snapshot), my_gen, std::move(token))
            .start_detached();
    }

    std::shared_ptr<detail::AsyncValidatorState<T>> state_;
};

}  // namespace aria::async
