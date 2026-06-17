#pragma once

// AsyncCommand action wrappers — `action_with_timeout` and
// `action_with_retry`.
//
// These compose the existing `aria::async::with_timeout` and
// `aria::async::retry_with_backoff` / `retry_if` combinators into an
// AsyncCommand-friendly form, so a user can write:
//
//     AsyncCommand<Profile, std::string> load_profile(
//         ui, worker,
//         async::action_with_retry<Profile, std::string>(
//             3, 200ms, timer,
//             async::action_with_timeout<Profile, std::string>(
//                 timer, 3s,
//                 [this](CancellationToken tok, std::string id) -> Task<Profile> {
//                     return api_->fetch_profile(tok, std::move(id));
//                 })));
//
// Composition semantics:
//   * Each retry attempt gets its OWN timeout (the inner factory is
//     re-invoked per attempt — that is the contract of with_timeout +
//     retry already documented in timeout.hpp).
//   * External cancellation (the AsyncCommand's per-invocation
//     CancellationToken) propagates into the timeout's parent token,
//     so cancelling the AsyncCommand stops both the in-flight attempt
//     AND any pending retries instantly.
//   * `action_with_retry`'s `should_retry` predicate, when supplied,
//     gates retries on `std::exception&` (including `TimeoutError`).
//     Pass nullptr (default) to retry on every exception.

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/retry.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"
#include "aria/async/virtual_time_executor.hpp"  // IDelayedScheduler

#include <chrono>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace aria::async {

/// Wrap an AsyncCommand action factory with a per-invocation timeout.
///
/// Returns a cancellable factory `Task<R>(CancellationToken, Args...)`
/// suitable for passing to `AsyncCommand`'s constructor.
///
/// `timer` is the IDelayedScheduler that drives the timeout (in
/// production: a `ThreadPoolExecutor` or `MainThreadExecutor`; in
/// tests: a `VirtualTimeExecutor`).
template<typename R, typename... Args, typename Fn>
auto action_with_timeout(IDelayedScheduler& timer,
                         std::chrono::milliseconds duration,
                         Fn factory)
    -> std::function<Task<R>(CancellationToken, Args...)>
{
    return [&timer, duration, factory = std::move(factory)](
               CancellationToken parent_tok, Args... args) mutable -> Task<R> {
        co_return co_await aria::async::with_timeout(
            parent_tok, timer, duration,
            [factory, args...](CancellationToken inner_tok) mutable {
                return factory(inner_tok, args...);
            });
    };
}

/// Wrap an AsyncCommand action with retry + exponential backoff.
///
/// Each attempt sees the SAME parent CancellationToken (the one the
/// AsyncCommand injects on every invocation), so cancelling the
/// command stops both the in-flight attempt AND the inter-attempt
/// sleep.
///
/// `should_retry` is optional. When supplied, it gates retries: the
/// predicate receives the exception that escaped the attempt and
/// returns true to retry, false to propagate immediately.
/// When null (default), every `std::exception` is retried up to
/// `max_attempts`.
template<typename R, typename... Args, typename Fn>
auto action_with_retry(int max_attempts,
                       std::chrono::milliseconds initial_backoff,
                       IDelayedScheduler& timer,
                       Fn factory,
                       std::function<bool(const std::exception&)>
                           should_retry = nullptr)
    -> std::function<Task<R>(CancellationToken, Args...)>
{
    return [max_attempts, initial_backoff, &timer,
            factory       = std::move(factory),
            should_retry  = std::move(should_retry)](
               CancellationToken parent_tok, Args... args) mutable -> Task<R> {
        // Bind the user args into a nullary factory the retry combinator
        // can re-invoke per attempt. Each attempt shares `parent_tok`
        // so external cancel flips every in-flight attempt.
        auto bound = [factory, args..., parent_tok]() mutable {
            return factory(parent_tok, args...);
        };

        // Delay schedule: 1x, 2x, 4x, 8x ... of initial_backoff.
        auto next_delay = [initial_backoff](int attempt)
                           -> std::chrono::milliseconds {
            return initial_backoff
                   * (static_cast<std::chrono::milliseconds::rep>(1)
                      << attempt);
        };

        // `should_retry` routes directly into retry_impl_ so the
        // predicate is actually honoured (earlier drafts double-threw
        // and silently lost the predicate — that was the bug this
        // shape fixes).
        auto predicate = should_retry
            ? should_retry
            : std::function<bool(const std::exception&)>(
                  [](const std::exception&) { return true; });

        if constexpr (std::is_void_v<R>) {
            co_await detail::retry_impl_(
                max_attempts, predicate, next_delay, &timer, std::move(bound));
            co_return;
        } else {
            co_return co_await detail::retry_impl_(
                max_attempts, predicate, next_delay, &timer, std::move(bound));
        }
    };
}

}  // namespace aria::async
