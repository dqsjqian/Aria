#pragma once

// retry / retry_if / retry_with_backoff — coroutine-friendly retry combinators.
//
// Inspired by RxCpp's retry / retryWhen but expressed as plain co_await.
//
//   // Try up to 3 times.  Re-throws the last exception on final failure.
//   auto profile = co_await retry(3, []() { return http::get(url); });
//
//   // Exponential backoff: 100ms, 200ms, 400ms, 800ms ...
//   auto profile = co_await retry_with_backoff(
//       /*max_attempts=*/5,
//       /*initial=*/100ms,
//       timer,
//       [&]() { return http::get(url); });
//
//   // Custom predicate — only retry on transient network errors.
//   auto profile = co_await retry_if(
//       /*max_attempts=*/3,
//       [](const std::exception& e) {
//           return std::string{e.what()}.starts_with("network:");
//       },
//       [&]() { return http::get(url); });

#include "aria/async/task.hpp"
#include "aria/async/cancellation.hpp"  // OperationCancelled — never retried
#include "aria/async/executor.hpp"
#include "aria/async/virtual_time_executor.hpp"  // also serves as IDelayedScheduler

#include <chrono>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace aria::async {

namespace detail {
    template<typename Factory>
    using factory_value_t =
        typename std::invoke_result_t<Factory>::promise_type::value_type;

    /// Awaiter that resumes after `delay` of (real or virtual) time.
    /// Bridges any IDelayedScheduler into a co_await-able expression.
    struct DelayAwaiter {
        IDelayedScheduler& scheduler;
        std::chrono::milliseconds delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const {
            scheduler.post_after(delay, [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };

    /// Single implementation backing all three public retry overloads.
    ///
    /// `should_retry` decides, given the just-thrown `std::exception&`,
    /// whether to attempt again.
    /// `next_delay` returns the delay to wait BEFORE the next attempt
    /// (called once per failure, in attempt order). Pass a no-op
    /// returning `0ms` if you don't want any waiting.
    /// `timer`, when non-null, is used to schedule the inter-attempt
    /// delays via virtual or real time.
    template<typename Factory, typename ShouldRetry, typename NextDelay>
    auto retry_impl_(int max_attempts,
                     ShouldRetry should_retry,
                     NextDelay next_delay,
                     IDelayedScheduler* timer,
                     Factory factory)
        -> Task<factory_value_t<Factory>>
    {
        using R = factory_value_t<Factory>;
        std::exception_ptr last;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            try {
                if constexpr (std::is_void_v<R>) {
                    co_await factory();
                    co_return;
                } else {
                    co_return co_await factory();
                }
            } catch (const OperationCancelled&) {
                // Cancellation is NOT a retryable failure. `OperationCancelled`
                // derives from std::exception, so without this arm it would be
                // swallowed by the generic handler below and — because the
                // default `should_retry` returns true unconditionally — the
                // operation the caller just cancelled would be retried until
                // `max_attempts` was exhausted. Rethrow immediately so
                // cancellation propagates to the awaiting frame intact.
                throw;
            } catch (const std::exception& e) {
                last = std::current_exception();
                const bool last_attempt = (attempt + 1 == max_attempts);
                if (last_attempt || !should_retry(e)) {
                    std::rethrow_exception(last);
                }
            } catch (...) {
                // Non-std::exception: out of contract for should_retry,
                // give up immediately.
                throw;
            }

            // Optional inter-attempt delay.
            if (timer) {
                const auto delay = next_delay(attempt);
                if (delay.count() > 0) {
                    co_await DelayAwaiter{*timer, delay};
                }
            }
        }
        std::rethrow_exception(last);
    }
}

// ── primary: blind retry N times, no delay ──────────────────────────────────
template<typename Factory>
auto retry(int max_attempts, Factory factory)
    -> Task<detail::factory_value_t<Factory>>
{
    return detail::retry_impl_(
        max_attempts,
        [](const std::exception&) { return true; },
        [](int) { return std::chrono::milliseconds{0}; },
        /*timer=*/nullptr,
        std::move(factory));
}

// ── retry_if: predicate decides whether to retry on a given exception ───────
template<typename Predicate, typename Factory>
auto retry_if(int max_attempts, Predicate should_retry, Factory factory)
    -> Task<detail::factory_value_t<Factory>>
{
    return detail::retry_impl_(
        max_attempts,
        std::move(should_retry),
        [](int) { return std::chrono::milliseconds{0}; },
        /*timer=*/nullptr,
        std::move(factory));
}

// ── retry_with_backoff: 100ms, 200ms, 400ms, 800ms ... ──────────────────────
template<typename Factory>
auto retry_with_backoff(int max_attempts,
                        std::chrono::milliseconds initial,
                        IDelayedScheduler& timer,
                        Factory factory)
    -> Task<detail::factory_value_t<Factory>>
{
    return detail::retry_impl_(
        max_attempts,
        [](const std::exception&) { return true; },
        [initial](int attempt) {
            // attempt is 0-based; doubling per failure.
            //
            // Two things are guarded here:
            //
            //   * `1 << attempt` on a plain `int` is signed-overflow UB from
            //     attempt 31 onward, and `max_attempts` is caller-supplied
            //     with no ceiling. The shift is therefore done in
            //     `unsigned long long`.
            //   * A doubling sequence reaches absurd delays long before it
            //     reaches the width of the type: with a 100ms base, attempt
            //     31 is already ~6.8 years. Clamping the exponent at 20 caps
            //     the multiplier at ~1e6 (100ms base → ~29 hours), which is
            //     far beyond any sensible retry window while keeping every
            //     later attempt well-defined and finite.
            constexpr int kMaxShift = 20;
            const int shift = attempt < kMaxShift ? attempt : kMaxShift;
            return initial * (1ull << shift);
        },
        &timer,
        std::move(factory));
}

}  // namespace aria::async
