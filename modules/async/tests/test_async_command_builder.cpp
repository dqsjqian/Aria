// Tests for action_with_timeout / action_with_retry — the
// AsyncCommand action wrappers.

#include <doctest/doctest.h>

#include "aria/async/async_command.hpp"
#include "aria/async/async_command_builder.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"
#include "aria/async/virtual_time_executor.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace aria::async;
using namespace std::chrono_literals;

TEST_CASE("action_with_timeout: under-deadline action completes normally") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    auto action = action_with_timeout<int, int>(
        timer, 5s,
        [](aria::async::CancellationToken, int x) -> Task<int> {
            co_return x * 2;
        });

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, action);
    cmd.execute(7);
    CHECK(*cmd.last_result.get() == 14);
    CHECK(cmd.last_error_message.get() == "");
}

TEST_CASE("action_with_retry: succeeds after a transient failure") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    std::atomic<int> attempts{0};
    auto action = action_with_retry<int, int>(
        /*max_attempts=*/3,
        /*initial_backoff=*/100ms,
        timer,
        [&attempts](aria::async::CancellationToken, int x) -> Task<int> {
            const int n = attempts.fetch_add(1) + 1;
            if (n < 2) throw std::runtime_error("flaky");
            co_return x * 3;
        });

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, action);

    // Drive virtual time so retry_with_backoff's sleep can advance.
    cmd.execute(7);
    timer.advance_by(500ms);

    CHECK(attempts.load() >= 2);
    CHECK(*cmd.last_result.get() == 21);
}

TEST_CASE("action_with_retry: exhausts attempts then fails with last error") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    std::atomic<int> attempts{0};
    auto action = action_with_retry<int, int>(
        /*max_attempts=*/2,
        /*initial_backoff=*/50ms,
        timer,
        [&attempts](aria::async::CancellationToken, int) -> Task<int> {
            ++attempts;
            throw std::runtime_error("always fails");
            co_return 0;  // unreachable
        });

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, action);
    cmd.execute(0);
    timer.advance_by(1s);

    CHECK(attempts.load() == 2);
    CHECK(cmd.last_error_message.get() == "always fails");
    CHECK_FALSE(cmd.last_result.get().has_value());
}

TEST_CASE("action_with_retry: respects should_retry predicate — non-retryable short-circuits") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    std::atomic<int> attempts{0};
    auto action = action_with_retry<int, int>(
        /*max_attempts=*/5,
        /*initial_backoff=*/10ms,
        timer,
        [&attempts](aria::async::CancellationToken, int) -> Task<int> {
            ++attempts;
            throw std::runtime_error("non-retryable");
            co_return 0;
        },
        [](const std::exception& e) {
            return std::string{e.what()}.find("transient") != std::string::npos;
        });

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, action);
    cmd.execute(0);
    timer.advance_by(1s);

    // Predicate rejects this exception → exactly ONE attempt, then
    // propagate. The fix for the pre-release should_retry-is-a-noop
    // bug is what makes this hold.
    CHECK(attempts.load() == 1);
    CHECK_FALSE(cmd.last_result.get().has_value());
    CHECK(cmd.last_error_message.get() == "non-retryable");
}

TEST_CASE("action_with_retry: respects should_retry predicate — retryable exhausts attempts") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    std::atomic<int> attempts{0};
    auto action = action_with_retry<int, int>(
        /*max_attempts=*/3,
        /*initial_backoff=*/10ms,
        timer,
        [&attempts](aria::async::CancellationToken, int) -> Task<int> {
            ++attempts;
            throw std::runtime_error("transient glitch");
            co_return 0;
        },
        [](const std::exception& e) {
            return std::string{e.what()}.find("transient") != std::string::npos;
        });

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, action);
    cmd.execute(0);
    timer.advance_by(1s);

    // Predicate says "retry" → every attempt is taken, last exception
    // propagates.
    CHECK(attempts.load() == 3);
    CHECK(cmd.last_error_message.get() == "transient glitch");
}

TEST_CASE("action_with_timeout + action_with_retry compose: each attempt has own deadline") {
    InlineExecutor inline_exec;
    VirtualTimeExecutor timer;

    std::atomic<int> attempts{0};

    // Retry over a "with_timeout" action. Each attempt has a 1s
    // deadline; the action sleeps 500ms via the timer (succeeds).
    auto inner = action_with_timeout<int, int>(
        timer, 1s,
        [&attempts](aria::async::CancellationToken tok, int x) -> Task<int> {
            ++attempts;
            // Simulate a quick async wait, then return.
            (void)tok;
            co_return x + 100;
        });

    auto outer = action_with_retry<int, int>(
        2, 50ms, timer, std::move(inner));

    AsyncCommand<int, int> cmd(inline_exec, inline_exec, outer);
    cmd.execute(5);
    timer.advance_by(2s);

    CHECK(attempts.load() == 1);
    CHECK(*cmd.last_result.get() == 105);
}
