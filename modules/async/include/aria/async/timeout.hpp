#pragma once

// timeout / with_timeout — bound a coroutine's execution by a deadline.
//
// Usage:
//
//   auto v = co_await with_timeout(timer, 3s, [](CancellationToken tok) {
//       return http::get(tok, url);
//   });
//
//   // No-token form: timeout still works, but inner work cannot
//   // cooperatively cancel — it just runs to completion in the background.
//   auto v = co_await with_timeout(timer, 3s, []{ return work(); });
//
//   // With a parent token; parent-cancel takes priority over timeout.
//   auto v = co_await with_timeout(parent_tok, timer, 3s, factory);
//
//   // Fail-fast mode: do not wait for inner to unwind; resume parent
//   // immediately with TimeoutError when the deadline expires.
//   auto v = co_await with_timeout(timer, 3s, factory, OnTimeout::Fail);
//
// Behaviour matrix:
//
//   OnTimeout::Cancel  (default; cooperative)
//     When `duration` expires, the inner CancellationToken is flipped.
//     The awaiting coroutine resumes only when the inner factory unwinds
//     via OperationCancelled (or completes naturally just before the
//     timeout). If the timer was the cause, the caller observes
//     `TimeoutError`; if the inner work threw something else, that
//     exception is propagated.
//
//     ⚠️ Footgun — Cancel + no-token factory:
//       The no-token overload (`with_timeout(timer, dur, []{ return work(); })`)
//       has NO way to signal cancellation into the inner work. Combined
//       with OnTimeout::Cancel, this means the timer can only OBSERVE
//       the inner outcome and re-label it as `TimeoutError` AFTER the
//       inner work has finished naturally — the caller does NOT get a
//       prompt timeout. If you need fail-fast behaviour, either pass a
//       factory that takes `CancellationToken` (so cooperative unwind
//       is possible), or use `OnTimeout::Fail`. Mixing no-token + Cancel
//       is a deliberate "best-effort observe" mode and is documented
//       here so reviewers can flag it; for "real" deadlines, prefer
//       one of the other two combinations.
//
//   OnTimeout::Fail    (non-cooperative; fail-fast)
//     When `duration` expires the parent is resumed IMMEDIATELY with
//     TimeoutError. The inner Task is NOT awaited any further — it
//     continues running detached in the background until it naturally
//     completes (its result and any exception are discarded). The
//     inner CancellationToken is still flipped, so cooperative inner
//     work CAN unwind early; non-cooperative inner work simply runs
//     to completion off-stage.
//
//     ⚠️ Caveat — only safe when the inner work has no observable
//     side effects after the timeout, OR side effects are idempotent.
//     If inner mutates shared state the caller can no longer track,
//     prefer OnTimeout::Cancel.
//
// Composition with retry:
//   When wrapped inside retry/retry_with_backoff, each attempt gets its
//   OWN deadline (factory() is re-invoked per attempt). A "global"
//   timeout across all attempts is NOT provided — wrap the outer call
//   in another with_timeout if you need it.

#include "aria/async/task.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/virtual_time_executor.hpp"  // for IDelayedScheduler
#include "aria/async/detail/race_slot.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace aria::async {

class TimeoutError : public std::runtime_error {
public:
    TimeoutError() : std::runtime_error("operation timed out") {}
};

/// Behaviour when the deadline expires.
///
///  * `Cancel` (default): cooperative. Flip the inner CancellationToken
///    and wait for the inner work to unwind before resuming the parent.
///    Safe under all conditions; requires inner work to probe the token.
/// Behaviour when the deadline expires.
///
///  * `Cancel` (default): cooperative. Flip the inner CancellationToken
///    and wait for the inner work to unwind before resuming the parent.
///    Safe under all conditions; requires inner work to probe the token.
///    See the file-header "Footgun" note about combining `Cancel` with
///    a no-token factory — that combination is observe-only, not
///    fail-fast.
///
///  * `Fail`: non-cooperative. Resume the parent immediately with
///    TimeoutError; the inner work continues detached. Only safe when
///    inner side effects are absent or idempotent.
enum class OnTimeout {
    Cancel,
    Fail,
};

namespace detail {

template<typename Factory>
concept TimeoutTokenAcceptingFactory =
    std::invocable<Factory, CancellationToken>;

template<typename Factory>
concept TimeoutPlainFactory =
    std::invocable<Factory>;

template<typename Factory>
struct timeout_factory_value;

template<typename Factory>
    requires TimeoutTokenAcceptingFactory<Factory>
struct timeout_factory_value<Factory> {
    using type = typename std::invoke_result_t<Factory, CancellationToken>::promise_type::value_type;
};

template<typename Factory>
    requires (!TimeoutTokenAcceptingFactory<Factory>) && TimeoutPlainFactory<Factory>
struct timeout_factory_value<Factory> {
    using type = typename std::invoke_result_t<Factory>::promise_type::value_type;
};

template<typename Factory>
using timeout_factory_value_t = typename timeout_factory_value<Factory>::type;

/// Invoke factory with or without a CancellationToken, depending on its concept.
template<typename Factory>
auto invoke_factory(Factory& f, CancellationToken tok) {
    if constexpr (TimeoutTokenAcceptingFactory<Factory>) {
        return f(tok);
    } else {
        return f();
    }
}

// ── Cancel mode (cooperative) ────────────────────────────────────────────

/// Single race implementation that backs the cooperative `with_timeout`
/// path. `parent` is engaged only when the caller passed one; when
/// present, parent-cancel takes priority over the deadline.
///
/// The `if constexpr (std::is_void_v<R>)` arms keep the void / non-void
/// paths in a single body so the race protocol is written exactly once.
template<typename Factory>
auto with_timeout_impl_(std::optional<CancellationToken> parent,
                        IDelayedScheduler& timer,
                        std::chrono::milliseconds duration,
                        Factory factory)
    -> Task<timeout_factory_value_t<Factory>>
{
    using R = timeout_factory_value_t<Factory>;

    // Per-invocation cancellation source for the inner work; flipped by
    // either the deadline or (if engaged) the parent token.
    auto inner_src = std::make_shared<CancellationSource>();
    auto inner_tok = inner_src->token();

    // Race flag: 0 = pending, 1 = inner-won, 2 = timer-won.
    auto winner = std::make_shared<std::atomic<int>>(0);

    // Arm the deadline. The lambda below takes care of cancelling the
    // inner work iff it wins the race.
    timer.post_after(duration, [winner, inner_src]() {
        int expected = 0;
        if (winner->compare_exchange_strong(expected, 2)) {
            inner_src->cancel();
        }
    });

    // If a parent token is engaged, propagate its cancellation to the
    // inner work synchronously. Parent-cancel always wins over the
    // deadline (we re-throw the original OperationCancelled below
    // instead of converting to TimeoutError).
    if (parent) {
        parent->on_cancel([inner_src]{ inner_src->cancel(); });
    }

    try {
        if constexpr (std::is_void_v<R>) {
            co_await invoke_factory(factory, inner_tok);
            int expected = 0;
            winner->compare_exchange_strong(expected, 1);
            if (parent && parent->is_cancelled()) throw OperationCancelled{};
            co_return;
        } else {
            R value = co_await invoke_factory(factory, inner_tok);
            int expected = 0;
            winner->compare_exchange_strong(expected, 1);
            if (parent && parent->is_cancelled()) throw OperationCancelled{};
            co_return value;
        }
    } catch (const OperationCancelled&) {
        // Parent-cancel wins outright.
        if (parent && parent->is_cancelled()) throw;
        // Otherwise: if the timer fired we surface TimeoutError;
        // any other cancellation propagates verbatim.
        if (winner->load() == 2) throw TimeoutError{};
        throw;
    }
}

// ── Fail mode (non-cooperative, fail-fast) ───────────────────────────────

// Race state and awaiter live in detail/race_slot.hpp (shared with
// when_any's race-hardening path).
//
// Winner codes for with_timeout::Fail:
//   1 = Inner completed first
//   2 = Timer fired first
//   3 = Parent token cancelled (engaged parent only)

/// Driver coroutine that runs the inner factory under its own
/// CancellationToken and posts the result back to the shared slot.
/// Detached on construction so it outlives the parent if Fail mode
/// times out.
template<typename Factory, typename R>
Task<void> drive_inner_for_fail_(
    Factory factory,
    CancellationToken inner_tok,
    std::shared_ptr<RaceSlot<R>> slot)
{
    try {
        if constexpr (std::is_void_v<R>) {
            co_await invoke_factory(factory, inner_tok);
            if (slot->try_claim(/*Inner=*/1)) {
                slot->result.template emplace<1>();   // void success
                slot->publish(/*Inner=*/1);
                slot->notify_winner_resume();
            }
            // else: timer or parent already won; result discarded.
        } else {
            R value = co_await invoke_factory(factory, inner_tok);
            if (slot->try_claim(/*Inner=*/1)) {
                slot->result.template emplace<1>(std::move(value));
                slot->publish(/*Inner=*/1);
                slot->notify_winner_resume();
            }
        }
    } catch (...) {
        if (slot->try_claim(/*Inner=*/1)) {
            slot->result.template emplace<2>(std::current_exception());
            slot->publish(/*Inner=*/1);
            slot->notify_winner_resume();
        }
        // else: timer/parent won; exception discarded silently.
    }
}

/// Fail-mode wrapper. Returns `Task<R>` so call sites do not need to
/// distinguish between Cancel and Fail at the syntax level — both look
/// like `co_await with_timeout(...)`.
template<typename Factory>
auto with_timeout_fail_impl_(std::optional<CancellationToken> parent,
                             IDelayedScheduler& timer,
                             std::chrono::milliseconds duration,
                             Factory factory)
    -> Task<timeout_factory_value_t<Factory>>
{
    using R = timeout_factory_value_t<Factory>;

    auto slot      = std::make_shared<RaceSlot<R>>();
    auto inner_src = std::make_shared<CancellationSource>();
    auto inner_tok = inner_src->token();

    // Arm the deadline.
    timer.post_after(duration, [slot, inner_src]() {
        if (slot->try_claim(/*Timer=*/2)) {
            // Mark inner as cancelled so cooperative work CAN unwind
            // (best-effort; non-cooperative inner just runs in the bg).
            inner_src->cancel();
            slot->result.template emplace<2>(
                std::make_exception_ptr(TimeoutError{}));
            slot->publish(/*Timer=*/2);
            slot->notify_winner_resume();
        }
    });

    // Wire parent token (if engaged): parent-cancel beats timeout, same
    // as Cancel mode. Manifest as OperationCancelled in await_resume.
    if (parent) {
        parent->on_cancel([slot, inner_src]{
            if (slot->try_claim(/*ParentCancel=*/3)) {
                inner_src->cancel();
                slot->result.template emplace<2>(
                    std::make_exception_ptr(OperationCancelled{}));
                slot->publish(/*ParentCancel=*/3);
                slot->notify_winner_resume();
            }
        });
    }

    // Start the inner driver detached. It owns its own coroutine frame
    // via Task::start_detached and may outlive this wrapper if the
    // timer wins the race.
    drive_inner_for_fail_<Factory, R>(std::move(factory), inner_tok, slot)
        .start_detached();

    // Park until a winner publishes a result.
    if constexpr (std::is_void_v<R>) {
        co_await RaceSlotAwaiter<R>{slot};
        co_return;
    } else {
        co_return co_await RaceSlotAwaiter<R>{slot};
    }
}

}  // namespace detail

// ── Public API: 4 thin wrappers, dispatched on OnTimeout ─────────────────

template<typename Factory>
auto with_timeout(IDelayedScheduler& timer,
                  std::chrono::milliseconds duration,
                  Factory factory,
                  OnTimeout on_timeout = OnTimeout::Cancel)
    -> Task<detail::timeout_factory_value_t<Factory>>
{
    if (on_timeout == OnTimeout::Fail) {
        return detail::with_timeout_fail_impl_(
            std::optional<CancellationToken>{}, timer, duration, std::move(factory));
    }
    return detail::with_timeout_impl_(
        std::optional<CancellationToken>{}, timer, duration, std::move(factory));
}

template<typename Factory>
auto with_timeout(CancellationToken parent,
                  IDelayedScheduler& timer,
                  std::chrono::milliseconds duration,
                  Factory factory,
                  OnTimeout on_timeout = OnTimeout::Cancel)
    -> Task<detail::timeout_factory_value_t<Factory>>
{
    if (on_timeout == OnTimeout::Fail) {
        return detail::with_timeout_fail_impl_(
            std::optional<CancellationToken>{std::move(parent)},
            timer, duration, std::move(factory));
    }
    return detail::with_timeout_impl_(
        std::optional<CancellationToken>{std::move(parent)},
        timer, duration, std::move(factory));
}

}  // namespace aria::async
